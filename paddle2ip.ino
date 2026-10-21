// Paddle2IP — remote CW paddle over IP, operator side (M5Stack Dial v1.1, ESP32-S3).
//
// Paddle -> local iambic keyer + buzzer sidetone (no network in the path) ->
// key edges with ideal sender timestamps -> UDP -> server (OI3, rkey.ino),
// which replays them on the rig's CW key line with a fixed playout delay.
//
// Board: M5Stack Arduino core -> "M5Dial" (m5stack:esp32:m5stack_dial),
//        USB CDC On Boot: Enabled (serial console), USB Mode: Hardware CDC and JTAG
//        (the USB network mode switches to TinyUSB at run time, usbnet.cpp)
// Libs:  M5Dial (M5Unified, M5GFX)
// Protocol: rkey_proto.h, identical to the copy in the OI3 firmware.
//
// Hardware
//   Port B: G2 = dit, G1 = dah, paddle common = GND. Internal pull-ups; put
//           100 R in series and 10 nF to GND at the connector. Do NOT connect
//           the red 5 V wire.
//   Port A: G13 optional straight key, G15 optional PWM tone (RC + headphones).
//   Buzzer G3 (LEDC), encoder G40/G41 (PCNT), button under the display (G42).
//
// Hold the button while powering on to enter setup mode (WiFi AP + web page).
// Serial console (USB, 115200): type help. See README.md.
// USB network: a computer on the cable can carry the traffic instead of WiFi
// (chosen on the display, usbnet.cpp).
#include <M5Dial.h>
#include "shared.h"

void ui_begin(bool forceSetup);

void setup() {
  usbnet_early();                  // USB PHY back to the Serial-JTAG console after a warm reset
  auto cfg = M5.config();
  cfg.internal_spk = false;       // G3 is driven by our LEDC sidetone (M5.Speaker buffers)
  cfg.internal_mic = false;
  cfg.external_imu = false;       // keep Port A (G13/G15) free
  cfg.external_rtc = false;
  M5Dial.begin(cfg, false, false); // also sets HOLD (GPIO46) = 1, so the Dial stays on from battery
  Serial.begin(115200);

  pinMode(42, INPUT);
  bool forceSetup = digitalRead(42) == LOW;   // button held at power-on

  settings_load();
  Serial.printf("%s  wpm=%u D=%u key=%s host=%s\n", FW_VERSION, g_set.wpm, g_set.delayMs,
                g_set.keySet ? "set" : "NOT SET", g_set.host);
  Serial.println("serial console: type 'help' (show / resetpass / pass / wifi / reboot)");

  TaskHandle_t net = net_begin();  // core 0
  keyer_begin(net);                // core 1, 1 ms tick
  ui_begin(forceSetup);            // core 0: display, encoder, WiFi, web
}

void loop() {
  vTaskDelete(nullptr);            // everything runs in the tasks above
}
