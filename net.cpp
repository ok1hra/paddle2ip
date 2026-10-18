// net.cpp — KEY packets to the server (OI3), STATUS packets back. Task on core 0.
//
// Send rules (see rkey_proto.h / doc/remote-cw-key.md):
//   - immediately on every key edge (the keyer task notifies this task)
//   - heartbeat every 20 ms while any edge happened in the last 2 s, else 100 ms
//   - each packet repeats the edges of the last 500 ms (max 8); nev = 0 = idle
// The task wakes every 1 ms (or at once on an edge) and drains the socket, so
// the RTT measured from STATUS echoes is accurate to about 1 ms.
#include "shared.h"
#include "rkey_proto.h"
#include <WiFi.h>
#include "lwip/sockets.h"
#include "esp_timer.h"

static int      sock = -1;
static volatile uint32_t peerIp = 0;            // network order, 0 = unknown
static uint16_t session = 0;
static uint32_t seq = 0;
static uint32_t nonce = 0;                       // last nonce from an authentic STATUS

static portMUX_TYPE lsMux = portMUX_INITIALIZER_UNLOCKED;
static LinkState ls;
static uint32_t lastStatusMs = 0;

// loss / RTT windows (net task only)
static bool     haveStSeq = false;
static uint32_t lastStSeq = 0, dnExp = 0, dnGot = 0;
static bool     upWinOk = false;
static uint32_t upWinSeq0 = 0, upWinMs = 0;
static uint16_t upWinRx0 = 0;
static int32_t  rttMinCur = -1, rttMinPrev = -1;
static uint32_t rttWinMs = 0;

static inline uint32_t now_us() { return (uint32_t)esp_timer_get_time(); }

void     net_set_peer(uint32_t ip) { peerIp = ip; }
uint16_t net_session() { return session; }

void net_get(LinkState* out) {
  portENTER_CRITICAL(&lsMux);
  *out = ls;
  uint32_t age = millis() - lastStatusMs;
  portEXIT_CRITICAL(&lsMux);
  out->fresh = out->valid && age < 1000;
  out->peer  = peerIp != 0;
}

static bool sock_open() {
  sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) return false;
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(RKEY_DEFAULT_PORT);         // fixed local port: easy firewall/NAT rules
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(sock, (sockaddr*)&a, sizeof(a)) < 0) {
    a.sin_port = 0;                               // port busy: any port, OI3 replies to the source
    bind(sock, (sockaddr*)&a, sizeof(a));
  }
  int fl = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, fl | O_NONBLOCK);
  return true;
}

static void send_key() {
  uint32_t ip = peerIp;
  if (sock < 0 || ip == 0 || !g_set.keySet) return;
  uint8_t buf[sizeof(RkeyKeyHdr) + 4 * RKEY_MAX_EV + RKEY_TAG_LEN];
  uint32_t ev[RKEY_MAX_EV];
  uint16_t idx0;
  uint8_t  n = keyer_collect(now_us(), ev, &idx0);

  RkeyKeyHdr h;
  h.magic   = RKEY_MAGIC;
  h.ver     = RKEY_VER;
  h.type    = RKEY_T_KEY;
  h.flags   = (keyer_idle_us(now_us()) < 2000000u) ? RKEY_KF_ACTIVE : 0;
  h.session = session;
  h.wpm     = g_set.wpm;
  h.nev     = n;
  h.seq     = ++seq;
  h.nonce   = nonce;
  h.evIdx0  = idx0;
  h.delayMs = g_set.delayMs;
  h.tSend   = now_us();                           // after collect: tSend >= every edge time
  memcpy(buf, &h, sizeof(h));
  memcpy(buf + sizeof(h), ev, 4u * n);
  uint16_t len = rkey_sign(g_set.key, buf, sizeof(h) + 4u * n);

  sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_port = htons(g_set.port);
  to.sin_addr.s_addr = ip;
  if (sendto(sock, buf, len, 0, (sockaddr*)&to, sizeof(to)) == len) {
    portENTER_CRITICAL(&lsMux); ls.txPkts++; portEXIT_CRITICAL(&lsMux);
  }
}

static void on_status(const uint8_t* buf, int n, uint32_t arrive) {
  if (n != (int)(sizeof(RkeyStatus) + RKEY_TAG_LEN)) return;
  RkeyStatus st;
  memcpy(&st, buf, sizeof(st));
  if (st.magic != RKEY_MAGIC || st.ver != RKEY_VER || st.type != RKEY_T_STATUS) return;
  if (!g_set.keySet || !rkey_verify(g_set.key, buf, (uint16_t)n)) {
    portENTER_CRITICAL(&lsMux); ls.badTag++; portEXIT_CRITICAL(&lsMux);
    return;
  }
  nonce = st.nonce;
  bool mine = (st.session == session);
  uint32_t ms = millis();

  // RTT = now - echoTSend - echoAgeUs (only valid once OI3 runs our session)
  int32_t rtt = -1;
  if (mine && st.echoTSend != 0) {
    int32_t r = (int32_t)(arrive - st.echoTSend - st.echoAgeUs);
    if (r >= 0 && r < 5000000) rtt = r;
  }
  if (rtt >= 0) {
    if (ms - rttWinMs > 5000) { rttMinPrev = rttMinCur; rttMinCur = -1; rttWinMs = ms; }
    if (rttMinCur < 0 || rtt < rttMinCur) rttMinCur = rtt;
  }
  int32_t rttMin = (rttMinPrev >= 0 && (rttMinCur < 0 || rttMinPrev < rttMinCur)) ? rttMinPrev : rttMinCur;

  // downlink loss: gaps in the STATUS seq
  if (haveStSeq) {
    uint32_t d = st.seq - lastStSeq;
    if (d >= 1 && d < 1000) { dnExp += d; dnGot++; }
    else if (d != 0) { dnExp = dnGot = 0; }      // OI3 rebooted
  }
  haveStSeq = true; lastStSeq = st.seq;

  // uplink loss: accepted KEY packets (rxPkts) vs. sent, over >= 5 s windows
  uint8_t upPct = 255;
  if (mine) {
    if (!upWinOk) { upWinOk = true; upWinSeq0 = seq; upWinRx0 = st.rxPkts; upWinMs = ms; }
    else if (ms - upWinMs >= 5000) {
      uint32_t sent = seq - upWinSeq0;
      uint16_t got  = (uint16_t)(st.rxPkts - upWinRx0);
      if (sent > 0) upPct = (got >= sent) ? 0 : (uint8_t)(100u - (100u * got) / sent);
      upWinSeq0 = seq; upWinRx0 = st.rxPkts; upWinMs = ms;
    }
  } else upWinOk = false;

  portENTER_CRITICAL(&lsMux);
  ls.valid    = true;
  ls.flags    = st.flags;
  ls.civMode  = st.civMode;
  ls.seqLevel = st.seqLevel;
  ls.freqHz   = st.freqHz;
  ls.delayMs  = st.delayMs;
  ls.lateEv   = st.lateEv;
  ls.lostEv   = st.lostEv;
  ls.sessionOk = mine;
  if (rtt >= 0) { ls.rttUs = rtt; ls.rttMinUs = rttMin; }
  if (upPct != 255) ls.upLossPct = upPct;
  if (dnExp >= 50) {
    ls.dnLossPct = (uint8_t)(100u - (100u * dnGot) / dnExp);
    dnExp = dnGot = 0;
  }
  lastStatusMs = ms;
  portEXIT_CRITICAL(&lsMux);
}

static void net_task(void*) {
  uint32_t lastSendUs = 0;
  uint8_t  rx[128];
  for (;;) {
    bool edge = ulTaskNotifyTake(pdTRUE, 1) > 0;  // wake on edge or after 1 ms
    bool wifi = WiFi.isConnected();
    if (ls.wifi != wifi) { portENTER_CRITICAL(&lsMux); ls.wifi = wifi; portEXIT_CRITICAL(&lsMux); }
    if (!wifi) continue;
    // Session id only now: with the radio on, esp_random() is a true RNG. A
    // session id repeated after a reboot would make the OI3 drop our packets
    // (seq restarts at 1) until it reboots too.
    if (session == 0) { do session = (uint16_t)esp_random(); while (session == 0); }
    if (sock < 0 && !sock_open()) { vTaskDelay(100); continue; }

    for (int k = 0; k < 8; k++) {
      sockaddr_in from; socklen_t fl = sizeof(from);
      int n = recvfrom(sock, rx, sizeof(rx), MSG_DONTWAIT, (sockaddr*)&from, &fl);
      if (n <= 0) break;
      on_status(rx, n, now_us());
    }

    uint32_t now = now_us();
    uint32_t hb  = (keyer_idle_us(now) < 2000000u) ? 20000u : 100000u;
    if (edge || (now - lastSendUs) >= hb) {
      send_key();
      lastSendUs = now;
    }
  }
}

TaskHandle_t net_begin() {
  ls.rttUs = ls.rttMinUs = -1;
  ls.upLossPct = ls.dnLossPct = 255;
  TaskHandle_t h = nullptr;
  xTaskCreatePinnedToCore(net_task, "p2ip-net", 4096, nullptr, 10, &h, 0);
  return h;
}
