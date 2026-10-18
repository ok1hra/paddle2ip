// ui.cpp — display, encoder, button, serial console, NVS save. Task on core 0.
//
// Key screen: encoder (PCNT, x4 quadrature) = 1 step of the selected function
// per detent. Short press: next function (WPM / tone / volume / delay D /
// iambic A-B / display rotation); the display falls back to WPM 6 s after the
// last change.
// Setup screen (AP mode): encoder selects a saved network or "best in range",
// short press joins it; on success Paddle2IP leaves setup mode.
// Long press (both): info page (IP, RSSI, server, counters).
#include "shared.h"
#include "rkey_proto.h"
#include <M5Dial.h>
#include <WiFi.h>
#include "driver/pcnt.h"
#include "esp_timer.h"

void web_begin();
void web_loop();
bool web_take_peer_changed();

enum Func : uint8_t { F_WPM, F_TONE, F_VOL, F_DELAY, F_MODE, F_ROT, F_COUNT };
static Func     func = F_WPM;
static uint32_t funcMs = 0;
static bool     infoPage = false;
static bool     dirty = false;
static uint32_t dirtyMs = 0;
static int8_t   menuSel = 0;                     // setup menu: 0 = best in range, 1.. = saved slots

static M5Canvas canvas(&M5Dial.Display);
static bool     haveCanvas = false;

// ---------------- encoder ----------------
static int16_t encLast = 0;
static int     encAcc  = 0;

static void enc_begin() {
  // ESP-IDF rotary encoder example: both channels, x4 decoding.
  pcnt_config_t c = {};
  c.unit = PCNT_UNIT_0;
  c.channel = PCNT_CHANNEL_0;
  c.pulse_gpio_num = PIN_ENC_A; c.ctrl_gpio_num = PIN_ENC_B;
  c.pos_mode = PCNT_COUNT_DEC;  c.neg_mode = PCNT_COUNT_INC;
  c.lctrl_mode = PCNT_MODE_REVERSE; c.hctrl_mode = PCNT_MODE_KEEP;
  c.counter_h_lim = 32767; c.counter_l_lim = -32768;
  pcnt_unit_config(&c);
  c.channel = PCNT_CHANNEL_1;
  c.pulse_gpio_num = PIN_ENC_B; c.ctrl_gpio_num = PIN_ENC_A;
  c.pos_mode = PCNT_COUNT_INC;  c.neg_mode = PCNT_COUNT_DEC;
  pcnt_unit_config(&c);
  pcnt_set_filter_value(PCNT_UNIT_0, 1023);      // ~12.8 µs glitch filter
  pcnt_filter_enable(PCNT_UNIT_0);
  pcnt_counter_pause(PCNT_UNIT_0);
  pcnt_counter_clear(PCNT_UNIT_0);
  pcnt_counter_resume(PCNT_UNIT_0);
}

static int enc_steps() {
  int16_t v = 0;
  pcnt_get_counter_value(PCNT_UNIT_0, &v);
  encAcc += ENC_DIRECTION * (int16_t)(v - encLast);
  encLast = v;
  if (v > 16000 || v < -16000) { pcnt_counter_clear(PCNT_UNIT_0); encLast = 0; }
  int s = encAcc / ENC_COUNTS_PER_DETENT;
  encAcc -= s * ENC_COUNTS_PER_DETENT;
  return s;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void apply_steps(int s) {
  switch (func) {
    case F_WPM:  g_set.wpm = clampi(g_set.wpm + s, WPM_MIN, WPM_MAX); break;
    case F_TONE: g_set.toneHz = clampi(g_set.toneHz + 10 * s, TONE_MIN, TONE_MAX); keyer_set_tone(g_set.toneHz); break;
    case F_VOL:  g_set.vol = clampi(g_set.vol + s, VOL_MIN, VOL_MAX); break;
    case F_DELAY: {                               // 0 = OI3 default, else 10..1000 ms
      int d = g_set.delayMs + 10 * s;
      g_set.delayMs = (d < 10) ? 0 : clampi(d, 10, DELAY_MAX);
      break;
    }
    case F_MODE: if (s & 1) g_set.mode = (g_set.mode == 'A') ? 'B' : 'A'; break;
    case F_ROT:
      if (s & 1) g_set.rot180 = !g_set.rot180;   // applied in draw()
      break;
    default: break;
  }
  dirty = true; dirtyMs = millis(); funcMs = millis();
}

// setup menu: item 0 = best saved network in range, then the saved slots
static uint8_t menu_items(int8_t* slotOf) {
  uint8_t n = 0;
  slotOf[n++] = -1;
  for (int i = 0; i < WIFI_SLOTS; i++) if (g_set.ssid[i][0]) slotOf[n++] = i;
  return n;
}

// ---------------- server address ----------------
static uint32_t resolveMs = (uint32_t)-60000;   // resolve at once after boot
static bool     resolveOk = false;

static void peer_service(bool force) {
  uint32_t ms = millis();
  if (!force && ms - resolveMs < (resolveOk ? 60000u : 3000u)) return;
  resolveMs = ms;
  IPAddress ip;
  if (!g_set.host[0]) { net_set_peer(0); resolveOk = false; return; }
  if (ip.fromString(g_set.host)) { net_set_peer((uint32_t)ip); resolveOk = true; return; }
  if (!WiFi.isConnected()) return;
  if (WiFi.hostByName(g_set.host, ip) == 1 && (uint32_t)ip != 0) { net_set_peer((uint32_t)ip); resolveOk = true; }
  else resolveOk = false;                         // keep the last known address
}

// ---------------- display ----------------
static const char* mode_name(uint8_t m) {
  switch (m) {
    case 0x00: return "LSB";  case 0x01: return "USB"; case 0x02: return "AM";
    case 0x03: return "CW";   case 0x04: return "RTTY"; case 0x05: return "FM";
    case 0x07: return "CW-R"; case 0x08: return "RTTY-R"; case 0x17: return "DV";
    default:   return "?";
  }
}

static const char* warning(const LinkState& L, uint16_t* col) {
  *col = TFT_RED;
  if (!g_set.keySet)            return "NO KEY";
  if (!L.wifi)                  return "NO WIFI";
  if (!L.peer)                  return "NO SERVER";
  if (!L.fresh)                 return "NO LINK";
  if (L.flags & RKEY_SF_NONCE_BAD) { *col = TFT_ORANGE; return "SYNC"; }
  if (L.flags & RKEY_SF_INTERLOCK) return "INTERLOCK";
  if (L.flags & RKEY_SF_TUNE)      { *col = TFT_ORANGE; return "TUNE"; }
  if (!(L.flags & RKEY_SF_CW_MODE)) return "TRX NOT CW";
  if (L.flags & RKEY_SF_MUTED)     { *col = TFT_ORANGE; return "MUTED"; }
  return nullptr;
}

// WiFi bars + dBm (green / orange = at the edge / red = drop-outs likely),
// then packet loss up/down, centred on line y.
template <class G> static void draw_signal(G& g, const LinkState& L, int y) {
  bool up = WiFi.isConnected();
  int rssi = up ? WiFi.RSSI() : -127;
  uint16_t col = !up ? (uint16_t)0x8410 : rssi >= RSSI_GOOD ? (uint16_t)TFT_GREEN
               : rssi >= RSSI_EDGE ? (uint16_t)TFT_ORANGE : (uint16_t)TFT_RED;
  int bars = !up ? 0 : rssi >= -55 ? 4 : rssi >= RSSI_GOOD ? 3 : rssi >= RSSI_EDGE ? 2 : 1;
  char sig[12], loss[16], up_[6], dn[6];
  if (up) snprintf(sig, sizeof(sig), "%ddBm", rssi); else strcpy(sig, "--");
  if (L.upLossPct == 255) strcpy(up_, "-"); else snprintf(up_, sizeof(up_), "%u", L.upLossPct);
  if (L.dnLossPct == 255) strcpy(dn, "-"); else snprintf(dn, sizeof(dn), "%u", L.dnLossPct);
  snprintf(loss, sizeof(loss), "%s/%s%%", up_, dn);
  g.setFont(&fonts::Font2);
  const int bw = 15, gap = 4, gap2 = 8;
  int wSig = g.textWidth(sig), wLoss = g.textWidth(loss);
  int x = 120 - (bw + gap + wSig + gap2 + wLoss) / 2;
  for (int i = 0; i < 4; i++) {                  // 4 bars, 3 px wide, 3..12 px high
    int h = 3 * (i + 1);
    g.fillRect(x + 4 * i, y + 6 - h, 3, h, i < bars ? col : (uint16_t)0x4208);
  }
  g.setTextDatum(middle_left);
  g.setTextColor(col);
  g.drawString(sig, x + bw + gap, y);
  g.setTextColor(TFT_LIGHTGREY);
  g.drawString(loss, x + bw + gap + wSig + gap2, y);
  g.setTextDatum(middle_center);
}

template <class G> static void draw_main(G& g, const LinkState& L) {
  g.fillScreen(TFT_BLACK);
  bool txDown = L.fresh && (L.flags & RKEY_SF_KEY_DOWN);
  uint16_t ring = txDown ? TFT_RED : (L.fresh && L.sessionOk ? 0x03E0 /*dark green*/ : 0x4208 /*grey*/);
  g.fillArc(120, 120, 119, txDown ? 108 : 114, 0, 360, ring);

  g.setTextDatum(middle_center);
  char s[40];
  // own IP address (on top of the ring)
  g.setFont(&fonts::Font2);
  g.setTextColor(TFT_CYAN);
  if (WiFi.isConnected())  g.drawString(WiFi.localIP().toString().c_str(), 120, 25);
  else if (wlan_ap_on())   { snprintf(s, sizeof(s), "AP %s", WiFi.softAPIP().toString().c_str()); g.drawString(s, 120, 25); }
  else                     { g.setTextColor(0x8410); g.drawString("no IP", 120, 25); }

  // rig frequency + mode
  g.setTextColor(TFT_WHITE);
  g.setFont(&fonts::Font4);
  if (L.fresh && (L.flags & RKEY_SF_CIV_VALID) && L.freqHz) {
    snprintf(s, sizeof(s), "%lu.%03lu.%02lu", (unsigned long)(L.freqHz / 1000000UL),
             (unsigned long)((L.freqHz / 1000UL) % 1000UL), (unsigned long)((L.freqHz % 1000UL) / 10UL));
    g.drawString(s, 120, 47);
    g.setFont(&fonts::Font2);
    g.setTextColor((L.flags & RKEY_SF_CW_MODE) ? TFT_GREEN : TFT_ORANGE);
    snprintf(s, sizeof(s), "%s  SEQ %u", mode_name(L.civMode), L.seqLevel);
    g.drawString(s, 120, 69);
  } else {
    g.setTextColor(0x8410);
    g.drawString("---.---.--", 120, 47);
  }

  // centre: big value of the selected function
  const char* label = "WPM";
  bool num = true;
  int  val = g_set.wpm;
  switch (func) {
    case F_TONE:  label = "TONE Hz";  val = g_set.toneHz; break;
    case F_VOL:   label = "VOLUME %"; val = g_set.vol; break;
    case F_DELAY: label = g_set.delayMs ? "DELAY ms" : "DELAY (server)"; val = g_set.delayMs ? g_set.delayMs : L.delayMs; break;
    case F_MODE:  label = "IAMBIC"; num = false; break;
    case F_ROT:   label = "ROTATE (deg)"; val = g_set.rot180 ? 180 : 0; break;
    default: break;
  }
  g.setTextColor(func == F_WPM ? TFT_WHITE : TFT_YELLOW);
  if (num) {
    g.setFont(&fonts::Font7);
    g.setTextSize(func == F_TONE || func == F_DELAY || func == F_ROT ? 1.2f : 1.45f);
    snprintf(s, sizeof(s), "%d", val);
  } else {
    g.setFont(&fonts::FreeSansBold24pt7b);
    g.setTextSize(1.4f);
    snprintf(s, sizeof(s), "%c", g_set.mode);
  }
  g.drawString(s, 120, 119);
  g.setTextSize(1);
  g.setFont(&fonts::Font2);
  g.drawString(label, 120, 162);

  // warning line
  uint16_t wc;
  const char* w = warning(L, &wc);
  if (w) { g.setFont(&fonts::FreeSansBold9pt7b); g.setTextColor(wc); g.drawString(w, 120, 183); }

  // bottom: RTT, delay, OI3 lost/late edges
  g.setFont(&fonts::Font2);
  g.setTextColor(TFT_LIGHTGREY);
  if (L.fresh && L.rttUs >= 0)
    snprintf(s, sizeof(s), "RTT %ld/%ld D%u L%u/%u", (long)(L.rttUs / 1000), (long)(L.rttMinUs / 1000),
             L.delayMs, L.lostEv, L.lateEv);
  else
    snprintf(s, sizeof(s), "RTT -- D%u", L.fresh ? L.delayMs : g_set.delayMs);
  g.drawString(s, 120, 203);
  draw_signal(g, L, 220);
}

template <class G> static void draw_setup(G& g) {
  g.fillScreen(TFT_BLACK);
  g.fillArc(120, 120, 119, 114, 0, 360, TFT_YELLOW);
  g.setTextDatum(middle_center);
  g.setFont(&fonts::FreeSansBold9pt7b);
  g.setTextColor(TFT_YELLOW);
  g.drawString("SETUP", 120, 26);
  g.setFont(&fonts::Font2);
  g.setTextColor(TFT_WHITE);
  char s[64];
  g.drawString(wlan_ap_name(), 120, 48);
  snprintf(s, sizeof(s), "pass %s", g_set.webPass);
  g.drawString(s, 120, 66);
  g.setTextColor(TFT_CYAN);
  snprintf(s, sizeof(s), "http://%s", WiFi.softAPIP().toString().c_str());
  g.drawString(s, 120, 84);

  int8_t slotOf[WIFI_SLOTS + 1];
  uint8_t n = menu_items(slotOf);
  if (menuSel >= n) menuSel = n - 1;
  int y = 110;
  for (uint8_t i = 0; i < n; i++, y += 20) {
    bool sel = (i == menuSel);
    if (slotOf[i] < 0) snprintf(s, sizeof(s), "%s", n > 1 ? "best in range" : "no saved WiFi");
    else               snprintf(s, sizeof(s), "%d: %.18s", slotOf[i] + 1, g_set.ssid[slotOf[i]]);
    if (sel) g.fillRoundRect(40, y - 9, 160, 19, 4, 0x2945);
    g.setTextColor(sel ? TFT_YELLOW : TFT_LIGHTGREY);
    g.drawString(s, 120, y);
  }
  g.setTextColor(TFT_LIGHTGREY);
  g.drawString(wlan_scanning() ? "scanning..." : wlan_status(), 120, 196);
  g.setTextColor(0x8410);
  g.drawString("turn = select, press = join", 120, 214);
}

template <class G> static void draw_info(G& g, const LinkState& L) {
  g.fillScreen(TFT_BLACK);
  g.setTextDatum(middle_center);
  g.setFont(&fonts::Font2);
  g.setTextColor(TFT_CYAN);
  g.drawString(FW_VERSION, 120, 36);
  g.setTextColor(TFT_WHITE);
  char s[64];
  int y = 58;
  auto line = [&](const char* t) { g.drawString(t, 120, y); y += 18; };
  if (wlan_ap_on()) {
    snprintf(s, sizeof(s), "AP %s", wlan_ap_name()); line(s);
    snprintf(s, sizeof(s), "http://%s", WiFi.softAPIP().toString().c_str()); line(s);
  }
  if (WiFi.isConnected()) {
    int r = WiFi.RSSI();
    g.setTextColor(r >= RSSI_GOOD ? TFT_GREEN : r >= RSSI_EDGE ? TFT_ORANGE : TFT_RED);
    snprintf(s, sizeof(s), "%.20s %ddBm", WiFi.SSID().c_str(), r); line(s);
    g.setTextColor(TFT_WHITE);
    snprintf(s, sizeof(s), "http://%s", WiFi.localIP().toString().c_str()); line(s);
  } else { snprintf(s, sizeof(s), "STA %.22s", wlan_status()); line(s); }
  snprintf(s, sizeof(s), "SRV %.24s:%u", g_set.host[0] ? g_set.host : "-", g_set.port); line(s);
  snprintf(s, sizeof(s), "sess %04X tx %lu", net_session(), (unsigned long)L.txPkts); line(s);
  snprintf(s, sizeof(s), "bad tag %lu  %c %s", (unsigned long)L.badTag, g_set.mode, g_set.swap ? "swap" : ""); line(s);
  snprintf(s, sizeof(s), "login %s / %s", WEB_USER, g_set.webPass); line(s);
}

template <class G> static void paint(G& g, const LinkState& L) {
  if (infoPage)                 draw_info(g, L);
  else if (wlan_setup_mode())   draw_setup(g);
  else                          draw_main(g, L);
}

static void draw() {
  static int8_t rotNow = -1;                     // follows g_set.rot180 (encoder or web page)
  if (rotNow != (int8_t)g_set.rot180) { rotNow = g_set.rot180; M5Dial.Display.setRotation(rotNow ? 2 : 0); }
  LinkState L;
  net_get(&L);
  if (haveCanvas) { paint(canvas, L); canvas.pushSprite(0, 0); }
  else { M5Dial.Display.startWrite(); paint(M5Dial.Display, L); M5Dial.Display.endWrite(); }
}

// ---------------- task ----------------
static void ui_task(void*) {
  uint32_t drawMs = 0;
  for (;;) {
    M5Dial.update();
    uint32_t ms = millis();
    bool setup = wlan_setup_mode();
    int s = enc_steps();
    if (M5Dial.BtnA.wasHold()) { infoPage = !infoPage; drawMs = 0; }
    else if (M5Dial.BtnA.wasClicked()) {
      if (infoPage) infoPage = false;
      else if (setup) {
        int8_t slotOf[WIFI_SLOTS + 1];
        uint8_t n = menu_items(slotOf);
        if (n > 1) wlan_connect(slotOf[menuSel < n ? menuSel : 0]);
      }
      else { func = (Func)((func + 1) % F_COUNT); funcMs = ms; }
      drawMs = 0;
    }
    if (s) {
      if (setup) {
        int8_t slotOf[WIFI_SLOTS + 1];
        menuSel = clampi(menuSel + s, 0, menu_items(slotOf) - 1);
      } else apply_steps(s);
      drawMs = 0;
    }
    if (func != F_WPM && ms - funcMs > 6000) { func = F_WPM; drawMs = 0; }

    // NVS write stalls both cores -> only while the keyer has been idle for 2 s
    if (dirty && ms - dirtyMs > 3000 && keyer_idle_us((uint32_t)esp_timer_get_time()) > 2000000u) {
      settings_save();
      dirty = false;
    }

    console_service();
    wlan_service();
    peer_service(web_take_peer_changed());
    web_loop();

    if (ms - drawMs >= 100) { draw(); drawMs = ms; }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void ui_begin(bool forceSetup) {
  M5Dial.Display.setBrightness(128);
  canvas.setColorDepth(16);
  haveCanvas = canvas.createSprite(240, 240) != nullptr;
  if (!haveCanvas) { canvas.setColorDepth(8); haveCanvas = canvas.createSprite(240, 240) != nullptr; }
  enc_begin();
  wlan_begin(forceSetup);
  web_begin();
  xTaskCreatePinnedToCore(ui_task, "p2ip-ui", 8192, nullptr, 2, nullptr, 0);
}
