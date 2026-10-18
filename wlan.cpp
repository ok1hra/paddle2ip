// wlan.cpp — WiFi: up to 3 saved networks, setup AP with captive portal.
//
// Joining: ONE asynchronous scan of all channels (~1.5 s), then the strongest
// saved network in the air is joined directly by channel + BSSID. No slot is
// "tried" blindly, so a missing network costs nothing. A dropped link is
// first re-joined on the same AP for 6 s, then a new scan picks the best one.
//
// AP: in setup mode (button at boot / nothing saved) Paddle2IP runs AP+STA:
// the AP serves the web page, a DNS server answers every name with the AP
// address (phones then open the page by themselves), and the STA side scans
// for the network list and joins a saved network on request (display menu).
// Outside setup mode the AP is opened as a fallback when the STA has been
// down for 20 s, and closed again 30 s after the STA is back and nobody uses it.
// Scanning blocks the radio's channel for ~1.5 s, so it never runs while the
// STA is connected unless the web page asks for it.
#include "shared.h"
#include <WiFi.h>
#include <DNSServer.h>

enum WState : uint8_t { W_IDLE, W_SCAN, W_CONNECTING, W_RETRY, W_CONNECTED, W_WAIT };
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
static char     statusText[48] = "";

#define MAX_NETS 20
static WlanNet  nets[MAX_NETS];
static uint8_t  nNets = 0;
static int8_t   curSlot = -1;          // slot we are joining / joined

static void set_state(WState s) { st = s; stMs = millis(); }
static bool any_saved() { for (int i = 0; i < WIFI_SLOTS; i++) if (g_set.ssid[i][0]) return true; return false; }

bool        wlan_setup_mode() { return setupMode; }
bool        wlan_ap_on() { return apOn; }
const char* wlan_ap_name() { return apName; }
const char* wlan_status() { return statusText; }
bool        wlan_scanning() { return st == W_SCAN; }
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
  if (WiFi.scanNetworks(true, false, false, 120) == WIFI_SCAN_FAILED) {
    set_state(W_WAIT); waitMs = 2000;
    return;
  }
  set_state(W_SCAN);
}

bool wlan_scan_start() {
  if (st == W_SCAN) return true;
  if (st == W_CONNECTING || st == W_RETRY) return false;
  scan_start(false);
  return st == W_SCAN;
}

// Copy the scan into nets[], strongest first, one entry per SSID.
static void scan_collect() {
  int n = WiFi.scanComplete();
  nNets = 0;
  for (int i = 0; i < n && nNets < MAX_NETS; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    int dup = -1;
    for (int j = 0; j < nNets; j++) if (s == nets[j].ssid) { dup = j; break; }
    int8_t rssi = (int8_t)WiFi.RSSI(i);
    if (dup >= 0 && nets[dup].rssi >= rssi) continue;
    WlanNet& e = (dup >= 0) ? nets[dup] : nets[nNets++];
    strlcpy(e.ssid, s.c_str(), sizeof(e.ssid));
    e.rssi = rssi;
    e.ch   = (uint8_t)WiFi.channel(i);
    memcpy(e.bssid, WiFi.BSSID(i), 6);
    e.open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  for (int i = 1; i < nNets; i++)                  // sort by RSSI
    for (int j = i; j > 0 && nets[j].rssi > nets[j - 1].rssi; j--) { WlanNet t = nets[j]; nets[j] = nets[j - 1]; nets[j - 1] = t; }
}

// Join the strongest saved network of the last scan (or only joinSlot).
static bool join_best() {
  for (int i = 0; i < nNets; i++) {
    for (int k = 0; k < WIFI_SLOTS; k++) {
      if (!g_set.ssid[k][0] || strcmp(g_set.ssid[k], nets[i].ssid)) continue;
      if (joinSlot >= 0 && k != joinSlot) continue;
      curSlot = k;
      WiFi.begin(g_set.ssid[k], g_set.pass[k], nets[i].ch, nets[i].bssid, true);
      snprintf(statusText, sizeof(statusText), "joining %.24s", g_set.ssid[k]);
      set_state(W_CONNECTING);
      return true;
    }
  }
  if (joinSlot >= 0) snprintf(statusText, sizeof(statusText), "%.24s not found", g_set.ssid[joinSlot]);
  else               strlcpy(statusText, "no saved WiFi in range", sizeof(statusText));
  return false;
}

void wlan_connect(int8_t slot) {
  if (slot >= 0 && (slot >= WIFI_SLOTS || !g_set.ssid[slot][0])) return;
  joinSlot = slot;
  joinRequested = true;
  if (st == W_SCAN) { scanForJoin = true; return; }   // join when the running scan ends
  WiFi.disconnect(false);
  scan_start(true);
  strlcpy(statusText, "scanning...", sizeof(statusText));
}

void wlan_begin(bool setup) {
  WiFi.persistent(false);                         // our NVS only; no flash writes by the WiFi lib
  WiFi.setAutoReconnect(false);                   // re-joining is done here (fast, by BSSID)
  WiFi.setHostname("paddle2ip");
  uint8_t mac[6]; WiFi.macAddress(mac);
  snprintf(apName, sizeof(apName), "Paddle2IP-%02X%02X", mac[4], mac[5]);
  setupMode = setup || !any_saved();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                           // no modem sleep: it adds 100+ ms latency
  if (setupMode) {
    ap_start();
    scan_start(false);                            // network list for the web page and the menu
    strlcpy(statusText, "setup", sizeof(statusText));
  } else {
    scan_start(true);
  }
  staDownMs = millis();
}

void wlan_service() {
  uint32_t ms = millis();
  if (apOn) dns.processNextRequest();
  bool up = WiFi.isConnected();

  switch (st) {
    case W_SCAN:
      if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
        if (ms - stMs > 8000) { WiFi.scanDelete(); set_state(W_WAIT); waitMs = 1000; }
        break;
      }
      scan_collect();
      if (up) { set_state(W_CONNECTED); break; }  // list refresh while connected
      if (scanForJoin && join_best()) break;
      if (joinRequested) joinRequested = false;
      set_state(W_WAIT);
      // with phones on the AP, scan rarely: every scan disturbs the AP channel
      waitMs = (apOn && WiFi.softAPgetStationNum() > 0) ? 30000 : 5000;
      break;

    case W_CONNECTING:
      if (up) {
        set_state(W_CONNECTED);
        snprintf(statusText, sizeof(statusText), "%.24s", WiFi.SSID().c_str());
        if (setupMode && joinRequested) { setupMode = false; apIdleMs = ms; }   // leave setup
        joinRequested = false;
      } else if (ms - stMs > 12000) {
        WiFi.disconnect(false);
        snprintf(statusText, sizeof(statusText), "%.24s failed", curSlot >= 0 ? g_set.ssid[curSlot] : "");
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
