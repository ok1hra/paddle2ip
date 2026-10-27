// wlan.cpp — WiFi: up to 4 saved networks, setup AP with captive portal.
//
// Joining: ONE asynchronous scan of all channels (~1.5 s), then the strongest
// saved network in the air is joined directly by channel + BSSID. No slot is
// "tried" blindly, so a missing network costs nothing. A dropped link is
// first re-joined on the same AP for 6 s, then a new scan picks the best one.
// After power-on, when that scan finds two or more saved networks, the join
// waits in W_CHOOSE until the display menu picks one (wlan_connect). The USB
// network, when a computer is on the cable, is one more item of that menu:
// then even a single saved network (or none) waits for the choice.
//
// AP: in setup mode (button at boot / nothing saved) Paddle2IP runs AP+STA:
// the AP serves the web page, a DNS server answers every name with the AP
// address (phones then open the page by themselves), and the STA side scans
// for the network list and joins a saved network on request (display menu).
// Outside setup mode the AP is opened as a fallback when the STA has been
// down for 20 s, and closed again 30 s after the STA is back and nobody uses it.
// Scanning blocks the radio's channel for ~1.5 s, so it never runs while the
// STA is connected unless the web page asks for it.
// W_OFF: radio off while the USB network carries the traffic (usbnet.cpp);
// on resume the usual scan + join starts from W_IDLE.
// Console log: every join attempt (slot, channel, BSSID), the disconnect
// reason code from the WiFi stack and status changes; `scan` prints one raw
// scan including hidden networks and the auth mode of each AP.
#include "shared.h"
#include <WiFi.h>
#include <DNSServer.h>

enum WState : uint8_t { W_IDLE, W_SCAN, W_CONNECTING, W_RETRY, W_CONNECTED, W_WAIT, W_CHOOSE, W_OFF };
static WState   st = W_IDLE;
static uint32_t stMs = 0;              // entered current state
static uint32_t waitMs = 0;            // W_WAIT duration
static bool     setupMode = false;
static bool     apOn = false;
static uint32_t apIdleMs = 0, staDownMs = 0;
static char     apName[24];
static DNSServer dns;

static bool     scanForJoin = false;   // current scan should end in a join
static int8_t   joinSlot = -1;         // -1 = best saved
static bool     joinRequested = false; // explicit request (setup menu)
static bool     bootChoice = false;    // until the first join: >= 2 saved in range -> W_CHOOSE
static bool     bootDecided = false;   // the power-on scan is through (menu or join)
static char     statusText[48] = "";
static char     lastLog[48] = "";      // statusText last printed on the console
static volatile uint8_t discReason = 0; // from the WiFi event task, printed by wlan_service
static uint8_t  joinReason = 0;        // disconnect reason of the current join attempt
static bool     scanDump = false;      // console 'scan': print the next scan raw

#define MAX_NETS 20
static WlanNet  nets[MAX_NETS];
static uint8_t  nNets = 0;
static int8_t   curSlot = -1;          // slot we are joining / joined

static void set_state(WState s) { st = s; stMs = millis(); }
static bool any_saved() { for (int i = 0; i < WIFI_SLOTS; i++) if (g_set.ssid[i][0]) return true; return false; }
static int8_t saved_slot(const char* s) {
  for (int k = 0; k < WIFI_SLOTS; k++) if (g_set.ssid[k][0] && !strcmp(g_set.ssid[k], s)) return k;
  return -1;
}

// Status changes go to the console too, each once (a failing join repeats every few seconds).
static void log_status() {
  if (!strcmp(lastLog, statusText)) return;
  strlcpy(lastLog, statusText, sizeof(lastLog));
  con().printf("wifi: %s\n", statusText);
}

static void on_disconnect(arduino_event_id_t, arduino_event_info_t info) {
  uint8_t r = info.wifi_sta_disconnected.reason;
  discReason = r ? r : WIFI_REASON_UNSPECIFIED;
}

static const char* auth_name(int a) {
  static const char* const n[] = { "open", "WEP", "WPA", "WPA2", "WPA/WPA2", "WPA2-ENT", "WPA3", "WPA2/WPA3", "WAPI" };
  return a >= 0 && a < (int)(sizeof(n) / sizeof(n[0])) ? n[a] : "?";
}

bool        wlan_setup_mode() { return setupMode; }
bool        wlan_ap_on() { return apOn; }
const char* wlan_ap_name() { return apName; }
const char* wlan_status() { return statusText; }
bool        wlan_scanning() { return st == W_SCAN; }
bool        wlan_choosing() { return st == W_CHOOSE; }
bool        wlan_boot_decided() { return bootDecided || millis() > 10000; }   // scans failing: don't wait forever
bool        wlan_is_ap_client(IPAddress local) { return apOn && local == WiFi.softAPIP(); }

uint8_t wlan_nets(WlanNet* out, uint8_t max) {
  uint8_t n = nNets < max ? nNets : max;
  memcpy(out, nets, n * sizeof(WlanNet));
  return n;
}

static void ap_start() {
  if (apOn) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apName, g_set.webPass);
  WiFi.setSleep(false);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", WiFi.softAPIP());
  apOn = true; apIdleMs = millis();
}

static void ap_stop() {
  if (!apOn) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  apOn = false;
}

void wlan_ap_restart() {
  if (!apOn) return;
  WiFi.softAP(apName, g_set.webPass);            // same SSID, new password
}

static void scan_start(bool join) {
  scanForJoin = join;
  // hidden networks too: only the 'scan' dump shows them, scan_collect skips them
  if (WiFi.scanNetworks(true, true, false, 120) == WIFI_SCAN_FAILED) {
    if (scanDump) { scanDump = false; con().println("scan: failed to start"); }
    set_state(W_WAIT); waitMs = 2000;
    return;
  }
  set_state(W_SCAN);
}

bool wlan_scan_start() {
  if (st == W_SCAN) return true;
  if (st == W_CONNECTING || st == W_RETRY || st == W_CHOOSE || st == W_OFF) return false;
  scan_start(false);
  return st == W_SCAN;
}

// Console 'scan': every AP of the scan as the WiFi stack returned it.
static void scan_dump(int n) {
  if (n < 0) { con().println("scan: failed"); return; }
  con().printf("scan: %d APs\n", n);
  bool seen[WIFI_SLOTS] = { false };
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    int8_t k = saved_slot(s.c_str());
    if (k >= 0) seen[k] = true;
    const uint8_t* b = WiFi.BSSID(i);
    con().printf("  %4d dBm ch%2d %02X:%02X:%02X:%02X:%02X:%02X %-9s %s%s%s",
                 WiFi.RSSI(i), WiFi.channel(i), b[0], b[1], b[2], b[3], b[4], b[5],
                 auth_name(WiFi.encryptionType(i)), s.length() ? "'" : "<hidden>", s.c_str(), s.length() ? "'" : "");
    if (k >= 0) con().printf("  <- wifi %d", k + 1);
    con().println();
  }
  for (int k = 0; k < WIFI_SLOTS; k++)
    if (g_set.ssid[k][0] && !seen[k]) con().printf("  wifi %d '%s' not in this scan\n", k + 1, g_set.ssid[k]);
}

// Copy the scan into nets[], strongest first, one entry per SSID. When more
// SSIDs are in the air than nets[] holds, saved networks always get in and
// the weakest unsaved ones are left out (the scan order is not by RSSI).
static void scan_collect() {
  int n = WiFi.scanComplete();
  if (scanDump) { scanDump = false; scan_dump(n); }
  nNets = 0;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    int dup = -1;
    for (int j = 0; j < nNets; j++) if (s == nets[j].ssid) { dup = j; break; }
    int8_t rssi = (int8_t)WiFi.RSSI(i);
    if (dup >= 0 && nets[dup].rssi >= rssi) continue;
    WlanNet* e;
    if (dup >= 0) e = &nets[dup];
    else if (nNets < MAX_NETS) e = &nets[nNets++];
    else {                                         // full: replace the weakest unsaved one
      int w = -1;
      for (int j = 0; j < nNets; j++)
        if (saved_slot(nets[j].ssid) < 0 && (w < 0 || nets[j].rssi < nets[w].rssi)) w = j;
      if (w < 0 || (saved_slot(s.c_str()) < 0 && nets[w].rssi >= rssi)) continue;
      e = &nets[w];
    }
    strlcpy(e->ssid, s.c_str(), sizeof(e->ssid));
    e->rssi = rssi;
    e->ch   = (uint8_t)WiFi.channel(i);
    memcpy(e->bssid, WiFi.BSSID(i), 6);
    e->open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  for (int i = 1; i < nNets; i++)                  // sort by RSSI
    for (int j = i; j > 0 && nets[j].rssi > nets[j - 1].rssi; j--) { WlanNet t = nets[j]; nets[j] = nets[j - 1]; nets[j - 1] = t; }
}

// Saved networks of the last scan, strongest first (a duplicate SSID: the lower slot).
static uint8_t saved_in_range(int8_t* slot, int8_t* rssi, uint8_t max) {
  uint8_t n = 0;
  for (int i = 0; i < nNets && n < max; i++)
    for (int k = 0; k < WIFI_SLOTS; k++)
      if (g_set.ssid[k][0] && !strcmp(g_set.ssid[k], nets[i].ssid)) { slot[n] = k; rssi[n++] = nets[i].rssi; break; }
  return n;
}

uint8_t wlan_choices(int8_t* slot, int8_t* rssi, uint8_t max) {
  return st == W_CHOOSE ? saved_in_range(slot, rssi, max) : 0;
}

// Join the strongest saved network of the last scan (or only joinSlot).
static bool join_best() {
  for (int i = 0; i < nNets; i++) {
    for (int k = 0; k < WIFI_SLOTS; k++) {
      if (!g_set.ssid[k][0] || strcmp(g_set.ssid[k], nets[i].ssid)) continue;
      if (joinSlot >= 0 && k != joinSlot) continue;
      curSlot = k;
      joinReason = 0;
      const uint8_t* b = nets[i].bssid;
      con().printf("wifi: join %d '%s' ch %u %02X:%02X:%02X:%02X:%02X:%02X %d dBm\n",
                   k + 1, g_set.ssid[k], nets[i].ch, b[0], b[1], b[2], b[3], b[4], b[5], nets[i].rssi);
      WiFi.begin(g_set.ssid[k], g_set.pass[k], nets[i].ch, nets[i].bssid, true);
      snprintf(statusText, sizeof(statusText), "joining %.24s", g_set.ssid[k]);
      strlcpy(lastLog, statusText, sizeof(lastLog));
      set_state(W_CONNECTING);
      return true;
    }
  }
  if (joinSlot >= 0) snprintf(statusText, sizeof(statusText), "%.24s not found", g_set.ssid[joinSlot]);
  else               strlcpy(statusText, "no saved WiFi in range", sizeof(statusText));
  log_status();
  return false;
}

void wlan_connect(int8_t slot) {
  if (st == W_OFF) return;
  if (slot >= 0 && (slot >= WIFI_SLOTS || !g_set.ssid[slot][0])) return;
  joinSlot = slot;
  joinRequested = true;
  bootChoice = false;
  bootDecided = true;
  if (st == W_CHOOSE) {                               // menu answer: the scan is seconds old
    if (!join_best()) { joinRequested = false; set_state(W_WAIT); waitMs = 1000; }
    return;
  }
  if (st == W_SCAN) { scanForJoin = true; return; }   // join when the running scan ends
  WiFi.disconnect(false);
  scan_start(true);
  strlcpy(statusText, "scanning...", sizeof(statusText));
}

void wlan_scan_dump() {
  scanDump = true;
  if (wlan_scan_start()) return;
  scanDump = false;
  con().printf("scan: not now (%s)\n", st == W_OFF ? "WiFi off" : st == W_CHOOSE ? "network menu open" : "joining");
}

void wlan_suspend(bool off) {
  if (off == (st == W_OFF)) return;
  if (off) {
    ap_stop();
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
    bootChoice = false;
    bootDecided = true;
    set_state(W_OFF);
    strlcpy(statusText, "off (USB)", sizeof(statusText));
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    staDownMs = millis();                         // fallback AP only 20 s from now
    strlcpy(statusText, "", sizeof(statusText));
    set_state(W_IDLE);
  }
}

void wlan_begin(bool setup) {
  WiFi.persistent(false);                         // our NVS only; no flash writes by the WiFi lib
  WiFi.setAutoReconnect(false);                   // re-joining is done here (fast, by BSSID)
  WiFi.onEvent(on_disconnect, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.setHostname(HOSTNAME);
  uint8_t mac[6]; WiFi.macAddress(mac);
  snprintf(apName, sizeof(apName), "Paddle2IP-%02X%02X", mac[4], mac[5]);
  setupMode = setup || !any_saved();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                           // no modem sleep: it adds 100+ ms latency
  if (setupMode) {
    bootDecided = true;                           // no network menu in setup mode
    ap_start();
    scan_start(false);                            // network list for the web page and the menu
    strlcpy(statusText, "setup", sizeof(statusText));
  } else {
    bootChoice = true;
    scan_start(true);
  }
  staDownMs = millis();
}

void wlan_service() {
  uint32_t ms = millis();
  if (apOn) dns.processNextRequest();
  bool up = WiFi.isConnected();
  if (uint8_t r = discReason) {
    discReason = 0;
    if (st == W_CONNECTING) joinReason = r;
    con().printf("wifi: disconnected, reason %u %s\n", r, WiFi.disconnectReasonName((wifi_err_reason_t)r));
  }

  switch (st) {
    case W_SCAN:
      if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
        if (ms - stMs > 8000) { WiFi.scanDelete(); set_state(W_WAIT); waitMs = 1000; bootDecided = true; }
        break;
      }
      scan_collect();
      if (up) { set_state(W_CONNECTED); break; }  // list refresh while connected
      if (scanForJoin && bootChoice && !joinRequested) {
        int8_t sl[WIFI_SLOTS], rs[WIFI_SLOTS];
        bootDecided = true;
        if (saved_in_range(sl, rs, WIFI_SLOTS) >= 2 || usbnet_offered()) {
          set_state(W_CHOOSE);
          strlcpy(statusText, "choose WiFi", sizeof(statusText));
          break;
        }
      }
      if (scanForJoin && join_best()) break;
      if (joinRequested) joinRequested = false;
      set_state(W_WAIT);
      // with phones on the AP, scan rarely: every scan disturbs the AP channel
      waitMs = (apOn && WiFi.softAPgetStationNum() > 0) ? 30000 : 5000;
      break;

    case W_CONNECTING:
      if (up) {
        set_state(W_CONNECTED);
        bootChoice = false;
        snprintf(statusText, sizeof(statusText), "%.24s", WiFi.SSID().c_str());
        strlcpy(lastLog, statusText, sizeof(lastLog));
        con().printf("wifi: connected '%s' %s %d dBm\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
        if (setupMode && joinRequested) { setupMode = false; apIdleMs = ms; }   // leave setup
        joinRequested = false;
      } else if (ms - stMs > 12000) {
        WiFi.disconnect(false);
        const char* ss = curSlot >= 0 ? g_set.ssid[curSlot] : "";
        if (joinReason) snprintf(statusText, sizeof(statusText), "%.24s failed %u", ss, joinReason);
        else            snprintf(statusText, sizeof(statusText), "%.24s failed", ss);
        log_status();
        joinRequested = false;
        set_state(W_WAIT); waitMs = 3000;
      }
      break;

    case W_CONNECTED:
      if (!up) {                                  // link lost: re-join the same AP first
        WiFi.reconnect();
        set_state(W_RETRY);
        strlcpy(statusText, "reconnecting", sizeof(statusText));
      }
      break;

    case W_RETRY:
      if (up) { set_state(W_CONNECTED); snprintf(statusText, sizeof(statusText), "%.24s", WiFi.SSID().c_str()); }
      else if (ms - stMs > 6000) { WiFi.disconnect(false); set_state(W_IDLE); }
      break;

    case W_WAIT:
      if (ms - stMs >= waitMs) set_state(W_IDLE);
      break;

    case W_CHOOSE:                                // waits for wlan_connect()
    case W_OFF:                                   // waits for wlan_suspend(false)
      staDownMs = ms;                             // no fallback AP meanwhile
      break;

    case W_IDLE:
      if (up) { set_state(W_CONNECTED); break; }
      if (!setupMode && any_saved()) { joinSlot = -1; scan_start(true); }
      break;
  }

  // fallback AP / closing it again
  if (up) staDownMs = ms;
  if (!setupMode) {
    if (!up && !apOn && ms - staDownMs > 20000) ap_start();
    if (apOn && up) {
      if (WiFi.softAPgetStationNum() > 0) apIdleMs = ms;
      else if (ms - apIdleMs > 30000) ap_stop();
    }
  }
}
