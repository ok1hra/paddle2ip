// ui.cpp — display, encoder, button, serial console, NVS save. Task on core 0.
//
// Key screen: encoder (PCNT, x4 quadrature) = 1 step of the selected function
// per detent. Short press: next function (WPM / tone / volume / delay D /
// iambic A-B / display rotation / USB mode: turn = serial or network, the
// press applies it, QR code of the manual's computer setup / test: turn =
// send "TEST" / info: FW revision, GitHub QR code + own IP); the display
// falls back to WPM FUNC_HOLD_MS after the last change (info, USB: INFO_HOLD_MS).
// While keying, the bottom line shows the trace of the sent elements unless
// a warning takes it.
// With the fallback AP up and no WiFi the main screen shows the AP name,
// password, page URL and WiFi status instead of frequency / warning / link.
// Setup screen (AP mode): encoder selects a saved network or "best in range",
// short press joins it; on success Paddle2IP leaves setup mode.
// Choose screen (power-on, >= 2 saved networks in range): encoder selects,
// short press joins; untouched for CHOOSE_AUTO_MS it joins the strongest.
// USB screen (a computer appeared on the cable, before the WiFi choice):
// serial / network, untouched for CHOOSE_AUTO_MS it takes the saved mode.
// Long press (both): service page (IP, RSSI, server, counters, legend of the
// main screen status line).
#include "shared.h"
#include "rkey_proto.h"
#include <M5Dial.h>
#include <WiFi.h>
#include "driver/pcnt.h"
#include "esp_timer.h"

void web_begin();
void web_loop();
bool web_take_peer_changed();

enum Func : uint8_t { F_WPM, F_TONE, F_VOL, F_DELAY, F_MODE, F_ROT, F_USB, F_SEND, F_INFO, F_COUNT };
static Func     func = F_WPM;
static uint32_t funcMs = 0;
static bool     infoPage = false;
static bool     dirty = false;
static uint32_t dirtyMs = 0;
static int8_t   menuSel = 0;                     // setup menu: 0 = best in range, 1.. = saved slots
static const uint32_t FUNC_HOLD_MS = 20000;      // back to WPM after the last change
static const uint32_t INFO_HOLD_MS = 60000;
static const uint32_t TRACE_HOLD_US = 10000000;  // trace shown until 10 s after the last edge
static int8_t   chooseSel = 0;                   // choose menu, 0 = strongest
static bool     chooseHold = false;              // encoder turned: no automatic join
static uint32_t chooseMs = 0;
static const uint32_t CHOOSE_AUTO_MS = 10000;
static int8_t   usbSel = 0;                      // USB menu: 0 = serial, 1 = network
static bool     usbHold = false;                 // encoder turned: no automatic choice
static uint32_t usbMs = 0;
static bool     usbPend = false;                 // F_USB: mode shown, applied by the press
static bool     rebootAfterSave = false;         // network -> serial needs a reboot

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
  funcMs = millis();
  if (func == F_INFO) return;                    // nothing to change
  if (func == F_SEND) { keyer_send_test(); return; }   // either direction
  switch (func) {
    case F_WPM:  g_set.wpm = clampi(g_set.wpm + s, WPM_MIN, WPM_MAX); break;
    case F_TONE: g_set.toneHz = clampi(g_set.toneHz + 10 * s, TONE_MIN, TONE_MAX); keyer_set_tone(g_set.toneHz); break;
    case F_VOL: {                                 // steps of 5 %, an odd value snaps to the grid
      int v = (s > 0 ? g_set.vol / 5 : (g_set.vol + 4) / 5) * 5 + 5 * s;
      g_set.vol = clampi(v, VOL_MIN, VOL_MAX);
      break;
    }
    case F_DELAY: {                               // 0 = OI3 default, else 10..1000 ms
      int d = g_set.delayMs + 10 * s;
      g_set.delayMs = (d < 10) ? 0 : clampi(d, 10, DELAY_MAX);
      break;
    }
    case F_MODE: if (s & 1) g_set.mode = (g_set.mode == 'A') ? 'B' : 'A'; break;
    case F_ROT:
      if (s & 1) g_set.rot180 = !g_set.rot180;   // applied in draw()
      break;
    case F_USB: if (s & 1) usbPend = !usbPend; return;   // applied by the press
    default: break;
  }
  dirty = true; dirtyMs = millis();
}

// setup menu: item 0 = best saved network in range, then the saved slots
static uint8_t menu_items(int8_t* slotOf) {
  uint8_t n = 0;
  slotOf[n++] = -1;
  for (int i = 0; i < WIFI_SLOTS; i++) if (g_set.ssid[i][0]) slotOf[n++] = i;
  return n;
}

// USB mode from the menu or F_USB: network starts at once, back to serial reboots
static void usb_set(bool net) {
  if (net != g_set.usbNet) { g_set.usbNet = net; dirty = true; dirtyMs = millis(); }
  if (net) {
    usbnet_choose(true);
    if (wlan_choosing()) wlan_connect(-1);       // WiFi only as standby until USB has an address
  } else if (usbnet_active()) {
    dirty = true; dirtyMs = millis();
    rebootAfterSave = true;
  } else usbnet_choose(false);
}

static void choose_join() {
  int8_t slot[WIFI_SLOTS], rssi[WIFI_SLOTS];
  uint8_t n = wlan_choices(slot, rssi, WIFI_SLOTS);
  if (n) wlan_connect(slot[chooseSel < n ? chooseSel : 0]);
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
  if (!WiFi.isConnected() && !usbnet_up()) return;
  if (WiFi.hostByName(g_set.host, ip) == 1 && (uint32_t)ip != 0) { net_set_peer((uint32_t)ip); resolveOk = true; }
  else resolveOk = false;                         // keep the last known address
}

// ---------------- display ----------------
static const char* warning(const LinkState& L, uint16_t* col) {
  *col = TFT_RED;
  if (usbnet_failed() && millis() < 60000) { *col = TFT_ORANGE; return "USB NET FAILED"; }
  if (rebootAfterSave)          { *col = TFT_ORANGE; return "REBOOT..."; }
  if (!g_set.keySet)            return "NO KEY";
  if (!L.wifi && !L.usb)        return usbnet_active() ? "NO NET" : "NO WIFI";
  if (!L.peer)                  return "NO SERVER";
  if (!L.fresh)                 return "NO LINK";
  if (L.flags & RKEY_SF_NONCE_BAD) { *col = TFT_ORANGE; return "SYNC"; }
  if (L.flags & RKEY_SF_INTERLOCK) return "INTERLOCK";
  if (L.flags & RKEY_SF_TUNE)      { *col = TFT_ORANGE; return "TUNE"; }
  if (!(L.flags & RKEY_SF_CW_MODE)) return "TRX NOT CW";
  if (L.flags & RKEY_SF_MUTED)     { *col = TFT_ORANGE; return "MUTED"; }
  return nullptr;
}

// centre value: scale up to maxSize, but leave room for the signal on both sides
template <class G> static void fit_size(G& g, const char* t, float maxSize) {
  g.setTextSize(1);
  int w = g.textWidth(t);
  g.setTextSize(w > 0 && 104.0f / w < maxSize ? 104.0f / w : maxSize);
}

// text with a 1 px black outline, readable on the red TX glow
template <class G> static void draw_halo(G& g, const char* t, int x, int y, uint16_t col) {
  static const int8_t d[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
  g.setTextColor(TFT_BLACK);
  for (int i = 0; i < 4; i++) g.drawString(t, x + d[i][0], y + d[i][1]);
  g.setTextColor(col);
  g.drawString(t, x, y);
}

// WiFi signal beside the centre value: vertical bargraph on the left (bottom =
// weakest), dBm above the packet loss up/down on the right. Green / orange =
// at the edge / red = drop-outs likely.
template <class G> static void draw_signal(G& g, const LinkState& L) {
  if (L.usb) {                                   // cable: no signal to show, only the loss
    char loss[16], up_[6], dn[6];
    if (L.upLossPct == 255) strcpy(up_, "-"); else snprintf(up_, sizeof(up_), "%u", L.upLossPct);
    if (L.dnLossPct == 255) strcpy(dn, "-"); else snprintf(dn, sizeof(dn), "%u", L.dnLossPct);
    snprintf(loss, sizeof(loss), "%s/%s%%", up_, dn);
    g.setFont(&fonts::Font2);
    draw_halo(g, "USB", 40, 120, TFT_GREEN);    // in place of the bars
    draw_halo(g, loss, 198, 120, TFT_LIGHTGREY);
    return;
  }
  bool up = WiFi.isConnected();
  int rssi = up ? WiFi.RSSI() : -127;
  uint16_t col = !up ? (uint16_t)0x8410 : rssi >= RSSI_GOOD ? (uint16_t)TFT_GREEN
               : rssi >= RSSI_EDGE ? (uint16_t)TFT_ORANGE : (uint16_t)TFT_RED;
  int bars = !up ? 0 : rssi >= -55 ? 4 : rssi >= RSSI_GOOD ? 3 : rssi >= RSSI_EDGE ? 2 : 1;
  for (int i = 0; i < 4; i++) {                  // 4 segments 6 px high, 8..14 px wide
    int w = 8 + 2 * i;
    g.fillRect(40 - w / 2, 132 - 9 * i, w, 6, i < bars ? col : (uint16_t)0x4208);
  }
  char sig[12], loss[16], up_[6], dn[6];
  if (up) snprintf(sig, sizeof(sig), "%ddBm", rssi); else strcpy(sig, "--");
  if (L.upLossPct == 255) strcpy(up_, "-"); else snprintf(up_, sizeof(up_), "%u", L.upLossPct);
  if (L.dnLossPct == 255) strcpy(dn, "-"); else snprintf(dn, sizeof(dn), "%u", L.dnLossPct);
  snprintf(loss, sizeof(loss), "%s/%s%%", up_, dn);
  g.setFont(&fonts::Font2);
  draw_halo(g, sig, 198, 111, col);              // may sit on the red TX glow
  draw_halo(g, loss, 198, 129, TFT_LIGHTGREY);
}

// TX: solid red edge RING_TX_IN..119, then a linear fade to black down to
// RING_FADE_IN (linear RGB565 already looks smooth through the panel gamma)
static const int RING_TX_IN = 116, RING_FADE_IN = 91;

template <class G> static void draw_ring(G& g, const LinkState& L) {
  // red for the whole over (OI3 sequencer up), not blinking with every element
  bool tx = L.fresh && (L.flags & (RKEY_SF_PTT | RKEY_SF_KEY_DOWN));
  if (!tx) {
    g.fillArc(120, 120, 119, 114, 0, 360, L.fresh && L.sessionOk ? 0x03E0 /*dark green*/ : 0x4208 /*grey*/);
    return;
  }
  const int span = RING_TX_IN - RING_FADE_IN;
  for (int r = RING_FADE_IN; r < RING_TX_IN; r += 2) {   // inner first, 3 px wide = no gaps
    int v = 31 * (r - RING_FADE_IN) / span;
    g.fillArc(120, 120, r + 2, r, 0, 360, (uint16_t)(v << 11));
  }
  g.fillArc(120, 120, 119, RING_TX_IN, 0, 360, TFT_RED);
}

// "info" function: FW revision, QR code + text of the GitHub URL, own IP
template <class G> static void draw_about(G& g, const LinkState& L) {
  g.fillScreen(TFT_BLACK);
  draw_ring(g, L);
  g.setTextDatum(middle_center);
  g.setFont(&fonts::Font2);
  g.setTextColor(TFT_LIGHTGREY);
  g.drawString("Paddle2IP", 120, 30);
  g.setTextColor(TFT_YELLOW);
  g.drawString("FW " FW_REV, 120, 48);
  g.qrcode(FW_URL, 64, 62, 111, 1, true);        // 29 modules x 3 px + 4-module quiet zone
  const char* url = FW_URL + 8;                  // without "https://"
  g.setTextColor(TFT_CYAN);
  if (g.textWidth(url) > 184) g.setFont(&fonts::Font0);
  g.drawString(url, 120, 188);
  // own IP address (the only line that fits below the URL)
  char s[24];
  g.setFont(&fonts::Font2);
  g.setTextColor(TFT_CYAN);
  if (L.usb)               g.drawString(("USB " + usbnet_ip().toString()).c_str(), 120, 208);
  else if (WiFi.isConnected()) g.drawString(WiFi.localIP().toString().c_str(), 120, 208);
  else if (wlan_ap_on())   { snprintf(s, sizeof(s), "AP %s", WiFi.softAPIP().toString().c_str()); g.drawString(s, 120, 208); }
  else                     { g.setTextColor(0x8410); g.drawString("no IP", 120, 208); }
}

// "USB" function: mode (turn = change, press = apply), QR code of the manual
// section with the computer's setup, MAC of the computer's interface
template <class G> static void draw_usbfunc(G& g, const LinkState& L) {
  g.fillScreen(TFT_BLACK);
  draw_ring(g, L);
  g.setTextDatum(middle_center);
  g.setFont(&fonts::FreeSansBold9pt7b);
  g.setTextColor(TFT_YELLOW);
  g.drawString(usbPend ? "USB: NETWORK" : "USB: SERIAL", 120, 28);
  g.setFont(&fonts::Font2);
  char s[32];
  if (usbPend != usbnet_active())  { g.setTextColor(TFT_ORANGE); strcpy(s, usbPend ? "press = start" : "press = reboot"); }
  else if (usbnet_active())        { g.setTextColor(L.usb ? TFT_GREEN : TFT_ORANGE);
                                     if (L.usb) snprintf(s, sizeof(s), "USB %s", usbnet_ip().toString().c_str());
                                     else strcpy(s, "no address (DHCP)"); }
  else                             { g.setTextColor(0x8410); strcpy(s, "turn = change"); }
  g.drawString(s, 120, 48);
  g.qrcode(USB_DOC_URL, 64, 62, 111, 1, true);   // 29 modules x 3 px + 4-module quiet zone
  g.setTextColor(TFT_CYAN);
  g.drawString("computer setup: manual", 120, 188);
  snprintf(s, sizeof(s), "MAC %s", usbnet_host_mac());
  if (g.textWidth(s) > 160) g.setFont(&fonts::Font0);   // round panel at y 206
  g.setTextColor(TFT_LIGHTGREY);
  g.drawString(s, 120, 206);
}

// Font2 line, Font0 when wider than w (long passwords / status texts)
template <class G> static void draw_fit(G& g, const char* t, int y, int w, uint16_t col) {
  g.setFont(&fonts::Font2);
  if (g.textWidth(t) > w) g.setFont(&fonts::Font0);
  g.setTextColor(col);
  g.drawString(t, 120, y);
}

template <class G> static void draw_main(G& g, const LinkState& L) {
  g.fillScreen(TFT_BLACK);
  draw_ring(g, L);

  g.setTextDatum(middle_center);
  char s[40];
  // fallback AP and no STA: no frequency, warning or link numbers to show,
  // the screen tells how to reach the web page instead
  bool ap = wlan_ap_on() && !L.wifi && !L.usb;
  g.setFont(&fonts::Font4);
  if (ap) {
    snprintf(s, sizeof(s), "AP %s", wlan_ap_name());
    draw_fit(g, s, 32, 140, TFT_YELLOW);
    char p[48];
    snprintf(p, sizeof(p), "pass %s", g_set.webPass);
    draw_fit(g, p, 50, 176, TFT_WHITE);
  } else if (L.fresh && (L.flags & RKEY_SF_CIV_VALID) && L.freqHz) {
    // rig frequency "14 023.75"; sequencer level n turns the last n digits grey
    snprintf(s, sizeof(s), "%lu %03lu.%02lu", (unsigned long)(L.freqHz / 1000000UL),
             (unsigned long)((L.freqHz / 1000UL) % 1000UL), (unsigned long)((L.freqHz % 1000UL) / 10UL));
    int x = 120 - g.textWidth(s) / 2;
    int left = 0;                                // digits still to come, incl. this one
    for (const char* p = s; *p; p++) if (*p != ' ' && *p != '.') left++;
    g.setTextDatum(middle_left);
    for (const char* p = s; *p; p++) {
      char c[2] = { *p, 0 };
      bool digit = (*p != ' ' && *p != '.');
      draw_halo(g, c, x, 50, digit && left <= L.seqLevel ? TFT_LIGHTGREY : TFT_WHITE);   // on the TX glow
      if (digit) left--;
      x += g.textWidth(c);
    }
    g.setTextDatum(middle_center);
  } else {
    g.setTextColor(0x8410);
    g.drawString("-- ---.--", 120, 50);
  }

  // centre: big value of the selected function
  const char* label = "WPM";
  bool num = true;
  int  val = g_set.wpm;
  char txt[8] = "";
  switch (func) {
    case F_TONE:  label = "TONE Hz";  val = g_set.toneHz; break;
    case F_VOL:   label = g_set.vol ? "VOLUME %" : "VOLUME (off)"; val = g_set.vol; break;
    case F_DELAY: label = g_set.delayMs ? "DELAY ms" : "DELAY (server)"; val = g_set.delayMs ? g_set.delayMs : L.delayMs; break;
    case F_MODE:  label = "IAMBIC"; num = false; txt[0] = g_set.mode; txt[1] = 0; break;
    case F_SEND:  label = keyer_msg_active() ? "sending..." : "turn = send"; num = false; strcpy(txt, "TEST"); break;
    case F_ROT:   label = "ROTATE (deg)"; val = g_set.rot180 ? 180 : 0; break;
    default: break;
  }
  g.setTextColor(func == F_WPM ? TFT_WHITE : TFT_YELLOW);
  if (num) {
    g.setFont(&fonts::Font7);
    snprintf(s, sizeof(s), "%d", val);
    fit_size(g, s, 1.45f);
  } else {
    g.setFont(&fonts::FreeSansBold24pt7b);
    snprintf(s, sizeof(s), "%s", txt);
    fit_size(g, s, 1.4f);
  }
  g.drawString(s, 120, 119);
  g.setTextSize(1);
  g.setFont(&fonts::Font2);
  g.drawString(label, 120, 162);

  if (ap) {
    snprintf(s, sizeof(s), "http://%s", WiFi.softAPIP().toString().c_str());
    draw_fit(g, s, 183, 190, TFT_CYAN);
    draw_fit(g, wlan_scanning() ? "scanning..." : wlan_status(), 203, 150, TFT_ORANGE);
    return;
  }

  // status line: R = RTT last/min, D = delay, L = OI3 lost/late edges (legend on the service page)
  g.setFont(&fonts::Font2);
  if (L.fresh && L.rttUs >= 0)
    snprintf(s, sizeof(s), "R%ld/%ld D%u L%u/%u", (long)(L.rttUs / 1000), (long)(L.rttMinUs / 1000),
             L.delayMs, L.lostEv, L.lateEv);
  else
    snprintf(s, sizeof(s), "R-- D%u", L.fresh ? L.delayMs : g_set.delayMs);
  draw_halo(g, s, 120, 183, TFT_LIGHTGREY);       // may sit on the TX glow
  draw_signal(g, L);

  // bottom: the first warning, else while keying the trace of the sent
  // elements (newest right; the same edges as the KEY packets, not an echo)
  uint16_t wc;
  const char* w = warning(L, &wc);
  if (w) { g.setFont(&fonts::FreeSansBold9pt7b); draw_halo(g, w, 120, 203, wc); }
  else if (keyer_idle_us((uint32_t)esp_timer_get_time()) < TRACE_HOLD_US) {
    char tr[16];
    keyer_trace(tr, 13);                         // 13 x 11 px fit the round panel at y 203
    g.setFont(&fonts::FreeMonoBold9pt7b);
    draw_halo(g, tr, 120, 203, TFT_WHITE);
  } else keyer_trace_clear();                    // the next over starts on an empty line
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
  int y = 106;
  for (uint8_t i = 0; i < n; i++, y += 17) {     // up to 1 + WIFI_SLOTS = 5 items
    bool sel = (i == menuSel);
    if (slotOf[i] < 0) snprintf(s, sizeof(s), "%s", n > 1 ? "best in range" : "no saved WiFi");
    else               snprintf(s, sizeof(s), "%d: %.18s", slotOf[i] + 1, g_set.ssid[slotOf[i]]);
    if (sel) g.fillRoundRect(40, y - 8, 160, 17, 4, 0x2945);
    g.setTextColor(sel ? TFT_YELLOW : TFT_LIGHTGREY);
    g.drawString(s, 120, y);
  }
  g.setTextColor(TFT_LIGHTGREY);
  g.drawString(wlan_scanning() ? "scanning..." : wlan_status(), 120, 196);
  g.setTextColor(0x8410);
  g.drawString("turn = select, press = join", 120, 214);
}

template <class G> static void draw_choose(G& g, const LinkState& L) {
  g.fillScreen(TFT_BLACK);
  draw_ring(g, L);
  g.setTextDatum(middle_center);
  g.setFont(&fonts::FreeSansBold9pt7b);
  g.setTextColor(TFT_YELLOW);
  g.drawString("CHOOSE WiFi", 120, 44);
  int8_t slot[WIFI_SLOTS], rssi[WIFI_SLOTS];
  uint8_t n = wlan_choices(slot, rssi, WIFI_SLOTS);
  if (chooseSel >= n) chooseSel = n ? n - 1 : 0;
  char s[40];
  int y = 120 - (n - 1) * 13;                    // list centred, 26 px rows, max WIFI_SLOTS = 4
  for (uint8_t i = 0; i < n; i++, y += 26) {
    bool sel = (i == chooseSel);
    if (sel) g.fillRoundRect(28, y - 11, 184, 22, 5, 0x2945);
    g.setFont(&fonts::Font2);
    snprintf(s, sizeof(s), "%s", g_set.ssid[slot[i]]);
    if (g.textWidth(s) > 136) g.setFont(&fonts::Font0);
    g.setTextDatum(middle_left);
    g.setTextColor(sel ? TFT_YELLOW : TFT_LIGHTGREY);
    g.drawString(s, 36, y);
    g.setFont(&fonts::Font2);
    snprintf(s, sizeof(s), "%d", rssi[i]);
    g.setTextDatum(middle_right);
    g.setTextColor(rssi[i] >= RSSI_GOOD ? TFT_GREEN : rssi[i] >= RSSI_EDGE ? TFT_ORANGE : TFT_RED);
    g.drawString(s, 204, y);
  }
  g.setTextDatum(middle_center);
  g.setFont(&fonts::Font2);
  g.setTextColor(0x8410);
  g.drawString("turn = select, press = join", 120, 186);
  if (!chooseHold) {
    uint32_t left = (CHOOSE_AUTO_MS - (millis() - chooseMs) + 999) / 1000;
    snprintf(s, sizeof(s), "join in %lu s", (unsigned long)left);
    g.setTextColor(TFT_ORANGE);
    g.drawString(s, 120, 204);
  }
}

template <class G> static void draw_usb(G& g, const LinkState& L) {
  g.fillScreen(TFT_BLACK);
  draw_ring(g, L);
  g.setTextDatum(middle_center);
  g.setFont(&fonts::FreeSansBold9pt7b);
  g.setTextColor(TFT_YELLOW);
  g.drawString("USB MODE", 120, 52);
  static const char* const item[2] = { "Serial (console)", "Network" };
  for (int i = 0; i < 2; i++) {
    int y = 107 + 26 * i;
    bool sel = (i == usbSel);
    if (sel) g.fillRoundRect(28, y - 11, 184, 22, 5, 0x2945);
    g.setFont(&fonts::Font2);
    g.setTextColor(sel ? TFT_YELLOW : TFT_LIGHTGREY);
    g.drawString(item[i], 120, y);
  }
  g.setFont(&fonts::Font2);
  g.setTextColor(0x8410);
  g.drawString("turn = select, press = OK", 120, 166);
  if (!usbHold) {
    char s[32];
    uint32_t left = (CHOOSE_AUTO_MS - (millis() - usbMs) + 999) / 1000;
    snprintf(s, sizeof(s), "%s in %lu s", g_set.usbNet ? "network" : "serial", (unsigned long)left);
    g.setTextColor(TFT_ORANGE);
    g.drawString(s, 120, 186);
  }
}

template <class G> static void draw_info(G& g, const LinkState& L) {
  g.fillScreen(TFT_BLACK);
  g.setTextDatum(middle_center);
  g.setFont(&fonts::Font2);
  g.setTextColor(TFT_CYAN);
  g.drawString(FW_VERSION, 120, 34);
  g.setTextColor(TFT_WHITE);
  char s[64];
  int y = 52;                                    // up to 8 lines, the last at y 157
  auto line = [&](const char* t) { g.drawString(t, 120, y); y += 15; };
  if (wlan_ap_on()) {
    snprintf(s, sizeof(s), "AP %s", wlan_ap_name()); line(s);
    snprintf(s, sizeof(s), "http://%s", WiFi.softAPIP().toString().c_str()); line(s);
  }
  if (usbnet_active()) {
    if (L.usb) snprintf(s, sizeof(s), "USB http://%s", usbnet_ip().toString().c_str());
    else       strcpy(s, "USB net: no address");
    line(s);
  }
  bool sta = WiFi.isConnected();
  if (sta) {
    int r = WiFi.RSSI();
    g.setTextColor(r >= RSSI_GOOD ? TFT_GREEN : r >= RSSI_EDGE ? TFT_ORANGE : TFT_RED);
    snprintf(s, sizeof(s), "%.20s %ddBm", WiFi.SSID().c_str(), r); line(s);
    g.setTextColor(TFT_WHITE);
    snprintf(s, sizeof(s), "http://%s", WiFi.localIP().toString().c_str()); line(s);
  } else { snprintf(s, sizeof(s), "STA %.22s", wlan_status()); line(s); }
  // mDNS name, while a network has us; only if the 4 lines below still fit
  if ((sta || L.usb) && y <= 157 - 4 * 15) { g.setTextColor(TFT_CYAN); line(MDNS_URL); g.setTextColor(TFT_WHITE); }
  snprintf(s, sizeof(s), "SRV %.24s:%u", g_set.host[0] ? g_set.host : "-", g_set.port); line(s);
  snprintf(s, sizeof(s), "sessID %04X tx %lu", net_session(), (unsigned long)L.txPkts); line(s);
  snprintf(s, sizeof(s), "bad tag %lu | %c key%s", (unsigned long)L.badTag, g_set.mode, g_set.swap ? " swap" : ""); line(s);
  snprintf(s, sizeof(s), "login %s / %s", WEB_USER, g_set.webPass); line(s);

  // legend of the main screen status line "R4/3 D120 L0/391", with the values
  g.drawFastHLine(70, 165, 100, 0x4208);
  g.setTextColor(TFT_LIGHTGREY);
  if (L.fresh && L.rttUs >= 0)
    snprintf(s, sizeof(s), "R %ld/%ld RTT last/min ms", (long)(L.rttUs / 1000), (long)(L.rttMinUs / 1000));
  else strcpy(s, "R -- RTT last/min ms");
  g.drawString(s, 120, 174);
  snprintf(s, sizeof(s), "D %u playout delay ms", L.fresh ? L.delayMs : g_set.delayMs);
  g.drawString(s, 120, 189);
  if (L.fresh) snprintf(s, sizeof(s), "L %u/%u lost/late edges", L.lostEv, L.lateEv);
  else         strcpy(s, "L -/- lost/late edges");
  if (g.textWidth(s) > 170) *strrchr(s, ' ') = 0;   // huge counters: drop "edges"
  g.drawString(s, 120, 204);
}

template <class G> static void paint(G& g, const LinkState& L) {
  if (infoPage)                 draw_info(g, L);
  else if (wlan_setup_mode())   draw_setup(g);
  else if (usbnet_choosing())   draw_usb(g, L);
  else if (wlan_choosing())     draw_choose(g, L);
  else if (func == F_INFO)      draw_about(g, L);
  else if (func == F_USB)       draw_usbfunc(g, L);
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
  bool wasChoose = false, wasUsb = false;
  for (;;) {
    M5Dial.update();
    uint32_t ms = millis();
    bool setup = wlan_setup_mode();
    bool usb = usbnet_choosing();
    bool choose = wlan_choosing() && !usb;       // the WiFi choice waits for the USB one
    if (usb && !wasUsb) { usbSel = g_set.usbNet; usbHold = false; usbMs = ms; drawMs = 0; }
    wasUsb = usb;
    if (choose && !wasChoose) { chooseSel = 0; chooseHold = false; chooseMs = ms; drawMs = 0; }
    wasChoose = choose;
    int s = enc_steps();
    if (M5Dial.BtnA.wasHold()) { infoPage = !infoPage; drawMs = 0; }
    else if (M5Dial.BtnA.wasClicked()) {
      if (infoPage) infoPage = false;
      else if (setup) {
        int8_t slotOf[WIFI_SLOTS + 1];
        uint8_t n = menu_items(slotOf);
        if (n > 1) wlan_connect(slotOf[menuSel < n ? menuSel : 0]);
      }
      else if (usb) usb_set(usbSel == 1);
      else if (choose) choose_join();
      else {
        if (func == F_USB && usbPend != usbnet_active()) usb_set(usbPend);
        func = (Func)((func + 1) % F_COUNT); funcMs = ms;
        if (func == F_USB) usbPend = usbnet_active();
      }
      drawMs = 0;
    }
    if (s && !infoPage) {                         // service page: turns would change hidden values
      if (setup) {
        int8_t slotOf[WIFI_SLOTS + 1];
        menuSel = clampi(menuSel + s, 0, menu_items(slotOf) - 1);
      } else if (usb) {
        usbSel = clampi(usbSel + s, 0, 1);
        usbHold = true;
      } else if (choose) {
        int8_t slot[WIFI_SLOTS], rssi[WIFI_SLOTS];
        chooseSel = clampi(chooseSel + s, 0, wlan_choices(slot, rssi, WIFI_SLOTS) - 1);
        chooseHold = true;
      } else apply_steps(s);
      drawMs = 0;
    }
    if (usb && !usbHold && ms - usbMs >= CHOOSE_AUTO_MS) usb_set(g_set.usbNet);
    if (choose && !chooseHold && ms - chooseMs >= CHOOSE_AUTO_MS) choose_join();
    if (func == F_SEND && keyer_msg_active()) funcMs = ms;   // stay while sending
    if (func != F_WPM && ms - funcMs > (func == F_INFO || func == F_USB ? INFO_HOLD_MS : FUNC_HOLD_MS)) { func = F_WPM; drawMs = 0; }

    // NVS write stalls both cores -> only while the keyer has been idle for 2 s
    if (dirty && ms - dirtyMs > 3000 && keyer_idle_us((uint32_t)esp_timer_get_time()) > 2000000u) {
      settings_save();
      dirty = false;
      if (rebootAfterSave) ESP.restart();        // usbnet_early() gives the USB back to the console
    }

    console_service();
    usbnet_service();
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
