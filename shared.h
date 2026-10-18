// shared.h — pins, settings and the interfaces between the three tasks.
//
//   core 1  keyer task  (1 ms tick)  paddle -> iambic -> sidetone + edge ring
//   core 0  net task    (1 ms poll)  edge ring -> KEY packets, STATUS -> link state
//   core 0  ui task                  display, encoder, button, web config, NVS
#pragma once
#include <Arduino.h>

#define FW_VERSION "Paddle2IP 1.1"

// ---------------- pins (M5Dial v1.1) ----------------
#define PIN_DIT    2    // Port B G2  (paddle common -> GND)
#define PIN_DAH    1    // Port B G1
#define PIN_SK     13   // Port A G13 optional straight key (to GND)
#define PIN_HP     15   // Port A G15 optional PWM tone for headphones (via RC)
#define PIN_BUZZ   3    // internal buzzer
#define PIN_ENC_A  41
#define PIN_ENC_B  40

// PCNT counts per encoder detent (x4 quadrature decoding). Verify on hardware.
#define ENC_COUNTS_PER_DETENT 4
#define ENC_DIRECTION (-1)      // +1 / -1: clockwise = increase

#define WEB_USER         "admin"
#define WEB_PASS_DEFAULT "paddle2ip"
#define WIFI_SLOTS       3

// ---------------- settings (NVS) ----------------
struct Settings {
  char     ssid[WIFI_SLOTS][33];   // empty = slot unused
  char     pass[WIFI_SLOTS][65];
  char     host[64];       // server (OI3) IP or hostname
  uint16_t port;           // server UDP port (OI3: RkeyPort)
  uint8_t  key[16];        // shared SipHash key (= RkeyKey in oi3.cfg)
  bool     keySet;
  uint16_t delayMs;        // requested playout delay, 0 = server default (OI3: RkeyDelayMs)
  char     webPass[33];    // web UI password, also the setup AP password
  // live (encoder) values, read by the keyer task
  volatile uint8_t  wpm;
  volatile uint16_t toneHz;
  volatile uint8_t  vol;   // buzzer duty 1..50 %
  volatile char     mode;  // 'A' / 'B'
  volatile bool     swap;  // swap dit/dah paddle
  volatile bool     skEnable;   // straight key on PIN_SK
  volatile bool     hpEnable;   // tone on PIN_HP
  volatile bool     rot180;     // display upside down
};
extern Settings g_set;

void settings_load();
void settings_save();
void settings_defaults();
bool settings_parse_key(const char* hex, uint8_t out[16]);

#define WPM_MIN 5
#define WPM_MAX 60
#define TONE_MIN 300
#define TONE_MAX 1200
#define VOL_MIN 1
#define VOL_MAX 50
#define DELAY_MAX 1000

// WiFi signal quality on the display
#define RSSI_GOOD  (-65)        // >= : green
#define RSSI_EDGE  (-75)        // >= : orange (at the edge), below: red (drop-outs likely)

// ---------------- keyer (keyer.cpp, core 1) ----------------
void     keyer_begin(TaskHandle_t notifyOnEdge);
void     keyer_set_tone(uint16_t hz);           // any task
// Edges for the next KEY packet: those of the last 500 ms (max 8), and always
// the newest one while the key is down. Returns count, fills idx0.
uint8_t  keyer_collect(uint32_t nowUs, uint32_t* ev, uint16_t* idx0);
uint32_t keyer_idle_us(uint32_t nowUs);         // time since the last edge (0xFFFFFFFF = never)
bool     keyer_key_down();

// ---------------- WiFi (wlan.cpp, ui task) ----------------
// Saved networks are found with ONE scan of all channels; the strongest one
// is joined directly by channel + BSSID (no trying the slots one by one).
struct WlanNet { char ssid[33]; int8_t rssi; uint8_t ch; uint8_t bssid[6]; bool open; };
void        wlan_begin(bool setupMode);
void        wlan_service();
bool        wlan_setup_mode();          // forced setup (button at boot / nothing saved)
bool        wlan_ap_on();
const char* wlan_ap_name();
void        wlan_ap_restart();          // apply a new AP password
bool        wlan_is_ap_client(IPAddress local);   // request came in through the AP
void        wlan_connect(int8_t slot);  // -1 = best saved network in the air
const char* wlan_status();              // short text for the display
bool        wlan_scan_start();          // refresh the list for the web page
bool        wlan_scanning();
uint8_t     wlan_nets(WlanNet* out, uint8_t max);

// ---------------- serial console (console.cpp, ui task) ----------------
void console_service();

// ---------------- net (net.cpp, core 0) ----------------
struct LinkState {
  bool     wifi;           // STA connected
  bool     peer;           // server address known
  bool     valid;          // a STATUS was ever received
  bool     fresh;          // last STATUS younger than 1 s
  uint8_t  flags;          // RKEY_SF_*
  uint8_t  civMode, seqLevel;
  uint32_t freqHz;
  uint16_t delayMs, lateEv, lostEv;
  bool     sessionOk;      // OI3 echoes our session
  int32_t  rttUs;          // last RTT, -1 = unknown
  int32_t  rttMinUs;       // min over ~5 s
  uint8_t  upLossPct, dnLossPct;   // 255 = unknown
  uint32_t badTag;         // STATUS packets with a bad tag
  uint32_t txPkts;
};
TaskHandle_t net_begin();                       // creates the task, returns its handle
void     net_get(LinkState* out);
void     net_set_peer(uint32_t ipNetOrder);     // 0 = unknown
uint16_t net_session();
