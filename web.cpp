// web.cpp — configuration page.
//
// Login: HTTP basic auth, user WEB_USER, password = webPass. Requests that come
// in through the Paddle2IP AP need no login: joining that AP already took the
// same password (WPA2), and phone captive-portal windows cannot do basic auth.
// In AP mode every unknown URL redirects to the page (captive portal), so a
// phone opens it by itself after joining the AP.
//
// WiFi and web passwords and the shared key are filled into the form (hidden,
// an eye button shows them), so the key can be copied into oi3.cfg later.
// Plain HTTP: the key crosses the local network on every page load (accepted).
#include "shared.h"
#include <WiFi.h>
#include <WebServer.h>

static WebServer server(80);
static volatile bool peerChanged = false;

bool web_take_peer_changed() { bool c = peerChanged; peerChanged = false; return c; }

static String esc(const char* s) {
  String o;
  for (; *s; s++) {
    switch (*s) {
      case '&': o += F("&amp;"); break;
      case '<': o += F("&lt;"); break;
      case '>': o += F("&gt;"); break;
      case '"': o += F("&quot;"); break;
      case '\'': o += F("&#39;"); break;
      default: o += *s;
    }
  }
  return o;
}

static String json_str(const char* s) {
  String o = "\"";
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') { o += '\\'; o += *s; }
    else if ((uint8_t)*s < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", (uint8_t)*s); o += b; }
    else o += *s;
  }
  return o + "\"";
}

static bool from_ap() { return wlan_is_ap_client(server.client().localIP()); }

// Captive portal: a request for a foreign host through the AP -> our page.
static bool captive() {
  if (!from_ap()) return false;
  String ip = WiFi.softAPIP().toString();
  if (server.hostHeader() == ip) return false;
  server.sendHeader("Location", "http://" + ip + "/", true);
  server.send(302, "text/plain", "");
  return true;
}

static bool auth() {
  if (from_ap()) return true;
  if (server.authenticate(WEB_USER, g_set.webPass)) return true;
  server.requestAuthentication(BASIC_AUTH, "Paddle2IP");
  return false;
}

static const char HEAD[] PROGMEM =
  "<!doctype html><html><head><meta charset=utf-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'><title>Paddle2IP</title>"
  "<style>body{font-family:sans-serif;max-width:34em;margin:1em auto;padding:0 1em}"
  "label{display:block;margin-top:.6em}input,select{width:100%;padding:.3em;box-sizing:border-box}"
  "input[type=checkbox]{width:auto}fieldset{margin-top:1em}code{word-break:break-all}"
  "button{margin-top:1em;padding:.5em 1.2em}.pw{display:flex;gap:.3em}.pw button{margin:0;padding:0 .7em}"
  "#nets div{padding:.35em;border-bottom:1px solid #ccc;cursor:pointer}#nets div:hover{background:#eef}"
  "#nets span{float:right;color:#666}</style>"
  "<script>function eye(b){var i=b.previousElementSibling;i.type=i.type=='password'?'text':'password'}</script>"
  "</head><body><h2 id=title>Paddle2IP</h2>"
  // newer release? The browser asks the web flasher (manifest.json written by
  // release.sh), so Paddle2IP itself needs no internet; offline = no warning.
  // FW_REV is YYYYMMDD[.N] -> compare as YYYYMMDD * 1000 + N.
  "<script>(function(){function v(s){var p=String(s).split('.');return(+p[0]||0)*1000+(+p[1]||0)}"
  "fetch('" FLASH_URL "manifest.json?'+Date.now(),{cache:'no-store'}).then(function(r){return r.json()})"
  ".then(function(m){if(v(m.version)<=v('" FW_REV "'))return;var a=document.createElement('a');"
  "a.href='" FLASH_URL "';a.target='_blank';a.style.cssText='display:block;font-size:.6em;color:#c00';"
  "a.textContent='\u26a0 newer firmware '+m.version+' available (running " FW_REV "), update with the web flasher';"
  "document.getElementById('title').appendChild(a);document.title='\u26a0 '+document.title})"
  ".catch(function(){})})()</script>";

static void field(String& h, const char* label, const char* name, const String& value,
                  const char* type = "text", const char* extra = "") {
  h += F("<label>"); h += label;
  h += F("<input type="); h += type; h += F(" name="); h += name; h += F(" id="); h += name;
  h += F(" value=\""); h += value; h += F("\" "); h += extra; h += F("></label>");
}

// password input with a show/hide button
static void pwfield(String& h, const char* label, const char* name, const String& value, const char* extra = "") {
  h += F("<label>"); h += label;
  h += F("<span class=pw><input type=password name="); h += name; h += F(" id="); h += name;
  h += F(" value=\""); h += value; h += F("\" autocomplete=off "); h += extra;
  h += F("><button type=button onclick=eye(this) title='show / hide'>&#128065;</button></span></label>");
}

static void check(String& h, const char* label, const char* name, bool on) {
  h += F("<label><input type=checkbox name="); h += name; h += on ? F(" checked") : F("");
  h += F("> "); h += label; h += F("</label>");
}

// network list (scan): a click fills the focused slot if it is empty, else
// the first empty slot, else the focused one
static const char NETS_JS[] PROGMEM =
  "<script>var slot=0;function $(i){return document.getElementById(i)}"
  "for(let i=0;i<SLOTS;i++){$('ssid'+i).onfocus=function(){slot=i}}"
  "function pick(s){var k=slot;if($('ssid'+slot).value){for(let i=0;i<SLOTS;i++){if(!$('ssid'+i).value){k=i;break}}}"
  "$('ssid'+k).value=s;$('pass'+k).value='';$('pass'+k).focus()}"
  "function show(j){var d=$('nets');d.innerHTML='';"
  "j.nets.forEach(function(n){var e=document.createElement('div');e.textContent=n.s+(n.o?' (open)':'');"
  "var r=document.createElement('span');r.textContent=n.r+' dBm ch'+n.c;e.appendChild(r);"
  "e.onclick=function(){pick(n.s)};d.appendChild(e)});"
  "if(!j.nets.length)d.textContent=j.scanning?'scanning...':'no networks (press Scan)';"
  "if(j.scanning)setTimeout(function(){poll(0)},1500)}"
  "function poll(st){fetch('/scan'+(st?'?start=1':'')).then(function(r){return r.json()}).then(show)}"
  "poll(AUTOSCAN)</script>";

static void handle_root() {
  if (captive() || !auth()) return;
  bool ap = from_ap();
  String h = FPSTR(HEAD);
  h += F("<p>"); h += FW_VERSION; h += F(" &middot; ");
  h += WiFi.isConnected() ? ("WiFi " + esc(WiFi.SSID().c_str()) + " " + WiFi.localIP().toString()) : String(F("WiFi not connected"));
  if (wlan_ap_on()) { h += F(" &middot; AP "); h += esc(wlan_ap_name()); }
  h += F(" &middot; <a href=/status>status</a></p><form method=post action=/save>");

  h += F("<fieldset><legend>WiFi networks (the strongest one in range is used)</legend>"
         "<div id=nets>&hellip;</div><button type=button onclick=poll(1)>Scan</button>");
  for (int i = 0; i < WIFI_SLOTS; i++) {
    char n[8], l[24];
    snprintf(l, sizeof(l), "%d. SSID", i + 1);
    snprintf(n, sizeof(n), "ssid%d", i);
    field(h, l, n, esc(g_set.ssid[i]), "text", "maxlength=32");
    snprintf(l, sizeof(l), "%d. password", i + 1);
    snprintf(n, sizeof(n), "pass%d", i);
    pwfield(h, l, n, esc(g_set.pass[i]), "maxlength=64");
  }
  h += F("</fieldset><fieldset><legend>Server (OI3)</legend>");
  field(h, "Server IP or hostname", "host", esc(g_set.host));
  field(h, "UDP port (RkeyPort)", "port", String(g_set.port), "number", "min=1 max=65535");
  h += F("<label>Shared key (RkeyKey): <b>");
  h += g_set.keySet ? F("set") : F("NOT SET");
  h += F("</b></label>");
  char hex[33] = "";
  if (g_set.keySet) settings_key_hex(hex);
  pwfield(h, "", "key", hex, "maxlength=32 placeholder='32 hex chars, empty = keep'");
  field(h, "Playout delay D ms (0 = server default, OI3 RkeyDelayMs; LAN 30, internet 80-150, LTE 200+)", "delay",
        String(g_set.delayMs), "number", "min=0 max=1000");
  h += F("</fieldset><fieldset><legend>Keyer</legend>");
  field(h, "WPM", "wpm", String(g_set.wpm), "number", "min=5 max=60");
  field(h, "Sidetone Hz", "tone", String(g_set.toneHz), "number", "min=300 max=1200");
  field(h, "Volume % (buzzer duty 1-50)", "vol", String(g_set.vol), "number", "min=1 max=50");
  h += F("<label>Iambic mode<select name=mode><option");
  h += (g_set.mode == 'A') ? F(" selected") : F("");
  h += F(">A</option><option");
  h += (g_set.mode == 'B') ? F(" selected") : F("");
  h += F(">B</option></select></label>");
  check(h, "Swap paddles (dit = G1, dah = G2)", "swap", g_set.swap);
  check(h, "Straight key on Port A G13", "sk", g_set.skEnable);
  check(h, "Headphone tone on Port A G15 (reboot)", "hp", g_set.hpEnable);
  check(h, "Display rotated 180&deg;", "rot", g_set.rot180);
  h += F("</fieldset><fieldset><legend>Login</legend><p>Web user <b>" WEB_USER "</b>. "
         "The password is also the password of the setup AP.</p>");
  pwfield(h, "Password (8-32 chars)", "webpass", esc(g_set.webPass), "minlength=8 maxlength=32");
  h += F("</fieldset><button>Save</button></form>"
         "<form method=post action=/genkey onsubmit=\"return confirm('Replace the shared key? The server (OI3 RkeyKey) must get the new key too.')\">"
         "<button>Generate new key</button></form>");
  h += F("<p>Firmware update: <a href=" FLASH_URL " target=_blank>web flasher</a> "
         "(Chrome / Edge, Dial on USB; the settings are kept) &middot; <a href=" FW_URL " target=_blank>GitHub</a></p>");
  h += F("<script>var AUTOSCAN="); h += ap ? '1' : '0'; h += F(",SLOTS=" STR(WIFI_SLOTS) ";</script>");
  h += FPSTR(NETS_JS);
  h += F("</body></html>");
  server.send(200, "text/html", h);
}

static void handle_scan() {
  if (!auth()) return;
  if (server.hasArg("start")) wlan_scan_start();
  WlanNet n[20];
  uint8_t k = wlan_nets(n, 20);
  String j = F("{\"scanning\":");
  j += wlan_scanning() ? F("true") : F("false");
  j += F(",\"nets\":[");
  for (uint8_t i = 0; i < k; i++) {
    if (i) j += ',';
    j += F("{\"s\":"); j += json_str(n[i].ssid);
    j += F(",\"r\":"); j += n[i].rssi;
    j += F(",\"c\":"); j += n[i].ch;
    j += F(",\"o\":"); j += n[i].open ? '1' : '0';
    j += '}';
  }
  j += F("]}");
  server.send(200, "application/json", j);
}

static bool copy_arg(const char* name, char* dst, size_t n, bool trim) {
  if (!server.hasArg(name)) return false;
  String v = server.arg(name);
  if (trim) v.trim();
  if (strcmp(dst, v.c_str()) == 0) return false;
  strlcpy(dst, v.c_str(), n);
  return true;
}

static void handle_save() {
  if (!auth()) return;
  bool reboot = false;
  String v, msg;

  for (int i = 0; i < WIFI_SLOTS; i++) {
    char n[8];
    snprintf(n, sizeof(n), "ssid%d", i);
    if (copy_arg(n, g_set.ssid[i], sizeof(g_set.ssid[i]), true)) reboot = true;
    snprintf(n, sizeof(n), "pass%d", i);
    if (copy_arg(n, g_set.pass[i], sizeof(g_set.pass[i]), false)) reboot = true;
    if (!g_set.ssid[i][0]) g_set.pass[i][0] = 0;
  }

  if (copy_arg("host", g_set.host, sizeof(g_set.host), true)) peerChanged = true;
  long p = server.arg("port").toInt();
  if (p >= 1 && p <= 65535) g_set.port = (uint16_t)p;

  v = server.arg("key"); v.trim();
  if (v.length()) {
    if (settings_parse_key(v.c_str(), g_set.key)) g_set.keySet = true;
    else msg += F("<p style=color:red>Key ignored: needs 32 hex chars, not all zero.</p>");
  }
  long d = server.arg("delay").toInt();
  g_set.delayMs = (d <= 0) ? 0 : (uint16_t)constrain(d, 10, DELAY_MAX);

  long w = server.arg("wpm").toInt();   if (w) g_set.wpm = constrain(w, WPM_MIN, WPM_MAX);
  long t = server.arg("tone").toInt();  if (t) { g_set.toneHz = constrain(t, TONE_MIN, TONE_MAX); keyer_set_tone(g_set.toneHz); }
  long vo = server.arg("vol").toInt();  if (vo) g_set.vol = constrain(vo, VOL_MIN, VOL_MAX);
  g_set.mode = (server.arg("mode") == "A") ? 'A' : 'B';
  g_set.swap = server.hasArg("swap");
  g_set.skEnable = server.hasArg("sk");
  g_set.rot180 = server.hasArg("rot");
  bool hp = server.hasArg("hp");
  if (hp != g_set.hpEnable) { g_set.hpEnable = hp; reboot = true; }

  v = server.arg("webpass");
  if (server.hasArg("webpass") && v != g_set.webPass) {
    if (v.length() >= 8 && v.length() < sizeof(g_set.webPass)) {
      strlcpy(g_set.webPass, v.c_str(), sizeof(g_set.webPass));
      if (!reboot) wlan_ap_restart();
      msg += F("<p>Login password changed.</p>");
    } else msg += F("<p style=color:red>Password ignored: 8-32 chars.</p>");
  }

  settings_save();
  String h = FPSTR(HEAD);
  h += F("<p>Saved.</p>"); h += msg;
  h += reboot ? F("<p>Rebooting&hellip; Paddle2IP joins the strongest saved network.</p>") : F("<p><a href=/>Back</a></p>");
  h += F("</body></html>");
  server.send(200, "text/html", h);
  if (reboot) { delay(500); ESP.restart(); }
}

static void handle_genkey() {
  if (!auth()) return;
  uint8_t k[16];
  do { esp_fill_random(k, sizeof(k)); } while (!(k[0] | k[1] | k[2] | k[3]));
  memcpy(g_set.key, k, 16);
  g_set.keySet = true;
  settings_save();
  char hex[33];
  settings_key_hex(hex);
  String h = FPSTR(HEAD);
  h += F("<p>New key stored in Paddle2IP. Put this line into <b>oi3.cfg</b> on the OI3 SD card "
         "(the key stays visible on the setup page and via the serial command <code>key</code>):</p>"
         "<p><code>[RkeyKey="); h += hex; h += F("]");
  h += F("</code></p><p><a href=/>Back</a></p></body></html>");
  server.send(200, "text/html", h);
}

static void handle_status() {
  if (!auth()) return;
  LinkState L; net_get(&L);
  char s[400];
  snprintf(s, sizeof(s),
    "{\"wifi\":%d,\"peer\":%d,\"fresh\":%d,\"sessionOk\":%d,\"flags\":%u,\"freqHz\":%lu,\"civMode\":%u,"
    "\"seqLevel\":%u,\"delayMs\":%u,\"lateEv\":%u,\"lostEv\":%u,\"rttUs\":%ld,\"rttMinUs\":%ld,"
    "\"upLossPct\":%d,\"dnLossPct\":%d,\"badTag\":%lu,\"txPkts\":%lu,\"session\":%u,\"wpm\":%u}",
    L.wifi, L.peer, L.fresh, L.sessionOk, L.flags, (unsigned long)L.freqHz, L.civMode,
    L.seqLevel, L.delayMs, L.lateEv, L.lostEv, (long)L.rttUs, (long)L.rttMinUs,
    L.upLossPct == 255 ? -1 : L.upLossPct, L.dnLossPct == 255 ? -1 : L.dnLossPct,
    (unsigned long)L.badTag, (unsigned long)L.txPkts, net_session(), g_set.wpm);
  server.send(200, "application/json", s);
}

void web_begin() {
  server.on("/", HTTP_GET, handle_root);
  server.on("/save", HTTP_POST, handle_save);
  server.on("/genkey", HTTP_POST, handle_genkey);
  server.on("/scan", HTTP_GET, handle_scan);
  server.on("/status", HTTP_GET, handle_status);
  server.onNotFound([] {
    if (!captive()) server.send(404, "text/plain", "not found");   // captive-portal probes land here
  });
  server.begin();
}

void web_loop() { server.handleClient(); }
