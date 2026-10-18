// settings.cpp — persistent configuration in NVS ("paddle2ip" namespace).
//
// NVS writes stall the flash cache on BOTH cores, i.e. also the keyer task.
// Callers therefore save only while the keyer is idle (see ui.cpp).
#include "shared.h"
#include <Preferences.h>

Settings g_set;

void settings_defaults() {
  memset(&g_set, 0, sizeof(g_set));
  g_set.port     = 5690;
  g_set.delayMs  = 0;
  strcpy(g_set.webPass, WEB_PASS_DEFAULT);
  g_set.wpm      = 22;
  g_set.toneHz   = 600;
  g_set.vol      = 25;
  g_set.mode     = 'B';
  g_set.swap     = false;
  g_set.skEnable = false;
  g_set.hpEnable = false;
  g_set.rot180   = false;
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// 32 hex chars -> 16 bytes. All-zero is rejected (= "not configured" in OI3).
bool settings_parse_key(const char* hex, uint8_t out[16]) {
  if (!hex || strlen(hex) != 32) return false;
  uint8_t k[16], any = 0;
  for (int i = 0; i < 16; i++) {
    int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    k[i] = (uint8_t)((hi << 4) | lo);
    any |= k[i];
  }
  if (!any) return false;
  memcpy(out, k, 16);
  return true;
}

static uint32_t clampu(uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

void settings_load() {
  settings_defaults();
  Preferences p;
  if (!p.begin("paddle2ip", true)) return;            // first boot: namespace does not exist yet
  for (int i = 0; i < WIFI_SLOTS; i++) {
    char k[8];
    snprintf(k, sizeof(k), "ssid%d", i);
    if (p.isKey(k)) p.getString(k, g_set.ssid[i], sizeof(g_set.ssid[i]));
    snprintf(k, sizeof(k), "pass%d", i);
    if (p.isKey(k)) p.getString(k, g_set.pass[i], sizeof(g_set.pass[i]));
  }
  p.getString("host", g_set.host, sizeof(g_set.host));
  if (p.isKey("webpass")) p.getString("webpass", g_set.webPass, sizeof(g_set.webPass));
  g_set.port    = p.getUShort("port", g_set.port);
  g_set.keySet  = (p.getBytes("key", g_set.key, 16) == 16) && p.getBool("keyset", false);
  g_set.delayMs = p.getUShort("delay", g_set.delayMs);
  g_set.wpm     = clampu(p.getUChar("wpm", g_set.wpm), WPM_MIN, WPM_MAX);
  g_set.toneHz  = clampu(p.getUShort("tone", g_set.toneHz), TONE_MIN, TONE_MAX);
  g_set.vol     = clampu(p.getUChar("vol", g_set.vol), VOL_MIN, VOL_MAX);
  char m        = (char)p.getUChar("mode", g_set.mode);
  g_set.mode    = (m == 'A') ? 'A' : 'B';
  g_set.swap    = p.getBool("swap", g_set.swap);
  g_set.skEnable = p.getBool("sk", g_set.skEnable);
  g_set.hpEnable = p.getBool("hp", g_set.hpEnable);
  g_set.rot180   = p.getBool("rot", g_set.rot180);
  p.end();
  if (g_set.delayMs != 0) g_set.delayMs = clampu(g_set.delayMs, 10, DELAY_MAX);
  if (strlen(g_set.webPass) < 8) strcpy(g_set.webPass, WEB_PASS_DEFAULT);
}

void settings_save() {
  Preferences p;
  if (!p.begin("paddle2ip", false)) return;
  for (int i = 0; i < WIFI_SLOTS; i++) {
    char k[8];
    snprintf(k, sizeof(k), "ssid%d", i); p.putString(k, g_set.ssid[i]);
    snprintf(k, sizeof(k), "pass%d", i); p.putString(k, g_set.pass[i]);
  }
  p.putString("host", g_set.host);
  p.putString("webpass", g_set.webPass);
  p.putUShort("port", g_set.port);
  p.putBytes("key", g_set.key, 16);
  p.putBool("keyset", g_set.keySet);
  p.putUShort("delay", g_set.delayMs);
  p.putUChar("wpm", g_set.wpm);
  p.putUShort("tone", g_set.toneHz);
  p.putUChar("vol", g_set.vol);
  p.putUChar("mode", (uint8_t)g_set.mode);
  p.putBool("swap", g_set.swap);
  p.putBool("sk", g_set.skEnable);
  p.putBool("hp", g_set.hpEnable);
  p.putBool("rot", g_set.rot180);
  p.end();
}
