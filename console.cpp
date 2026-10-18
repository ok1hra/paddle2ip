// console.cpp — serial console (USB CDC, 115200), for a forgotten login.
//
//   help              commands
//   show              web login, AP name, IP, saved networks
//   key               shared key as the oi3.cfg line [RkeyKey=...]
//   resetpass         web/AP password back to the default
//   pass <new>        set the web/AP password (8-32 chars)
//   wifi              rescan and join the best saved network
//   reboot
#include "shared.h"
#include <WiFi.h>

static char    line[80];
static uint8_t len = 0;

static void show() {
  Serial.printf("web login : user '%s'  password '%s'\n", WEB_USER, g_set.webPass);
  if (wlan_ap_on()) Serial.printf("AP        : %s (same password)  http://%s\n", wlan_ap_name(), WiFi.softAPIP().toString().c_str());
  if (WiFi.isConnected()) Serial.printf("STA       : %s  http://%s  %d dBm\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
  else                    Serial.printf("STA       : not connected (%s)\n", wlan_status());
  for (int i = 0; i < WIFI_SLOTS; i++) Serial.printf("wifi %d    : %s\n", i + 1, g_set.ssid[i][0] ? g_set.ssid[i] : "-");
  Serial.printf("server    : %s:%u  key %s\n", g_set.host[0] ? g_set.host : "-", g_set.port, g_set.keySet ? "set" : "NOT SET");
}

static void show_key() {
  if (!g_set.keySet) { Serial.println("shared key NOT SET"); return; }
  char hex[33];
  settings_key_hex(hex);
  Serial.printf("[RkeyKey=%s]\n", hex);
}

static void set_pass(const char* p) {
  size_t n = strlen(p);
  if (n < 8 || n >= sizeof(g_set.webPass)) { Serial.println("password must have 8-32 chars"); return; }
  strlcpy(g_set.webPass, p, sizeof(g_set.webPass));
  settings_save();
  wlan_ap_restart();
  Serial.printf("web/AP password is now '%s'\n", g_set.webPass);
}

static void exec(char* s) {
  while (*s == ' ') s++;
  char* arg = strchr(s, ' ');
  if (arg) { *arg++ = 0; while (*arg == ' ') arg++; }
  if (!*s) return;
  if      (!strcmp(s, "help"))      Serial.println("commands: show | key | resetpass | pass <new> | wifi | reboot");
  else if (!strcmp(s, "show"))      show();
  else if (!strcmp(s, "key"))       show_key();
  else if (!strcmp(s, "resetpass")) set_pass(WEB_PASS_DEFAULT);
  else if (!strcmp(s, "pass"))      set_pass(arg ? arg : "");
  else if (!strcmp(s, "wifi"))      { wlan_connect(-1); Serial.println("scanning for saved networks"); }
  else if (!strcmp(s, "reboot"))    { Serial.println("reboot"); delay(100); ESP.restart(); }
  else Serial.printf("unknown '%s', type help\n", s);
}

void console_service() {
  while (Serial.available()) {
    int c = Serial.read();
    if (c == '\r' || c == '\n') {
      if (len) { line[len] = 0; Serial.printf("> %s\n", line); exec(line); len = 0; }
    } else if (c == 8 || c == 127) {
      if (len) len--;
    } else if (c >= 32 && len < sizeof(line) - 1) {
      line[len++] = (char)c;
    }
  }
}
