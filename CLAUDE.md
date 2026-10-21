# CLAUDE.md — Paddle2IP development context

Context for AI-assisted development of this repository. User-facing
documentation is in README.md; this file records how the project came to be,
the decisions behind it and the open work.

## What this is

Remote CW keying over IP, two repositories:

| Repository | Local path | Role |
|---|---|---|
| **paddle2ip** (this one), github.com/ok1hra/paddle2ip | `/home/dan/Arduino/hra/ok1hra/esp32/paddle2ip` | client "CTRL": M5Stack Dial v1.1 (ESP32-S3) + paddle, local iambic keyer + sidetone, sends key edges |
| **oi3**, github.com/ok1hra/oi3 | `/home/dan/Arduino/hra/ok1hra/oi3` | server "RIG": Open Interface III (ATmega2560 + W5500) replays the edges on CW1/CW2 through its PTT sequencer (`rkey.ino`) |

Principle: only key EDGES travel, each with the sender's IDEAL keyer time (not
a measured one). The server plays each edge at `t_sender + min_latency + D`
(playout delay), so mark/space lengths are exact regardless of network jitter.

## Shared files (keep identical in both repositories)

- `rkey_proto.h` — packet formats + SipHash-2-4. Identifiers stay `RKEY_*` /
  `Rkey*` (wire protocol name; OI3 config keys are `Rkey*`). `build.sh` fails
  if it differs from `$OI3_DIR/rkey_proto.h` (default `../../oi3`).
- `tools/rkey_test.py` here = `test/rkey_test.py` in OI3.
- Incompatible protocol change → bump `RKEY_VER`, update both repos together.

## Decisions (do not change without a reason)

- Own UDP socket, port 5690, NOT TrxNet. TrxNet has no authentication, retries
  CON only after 2 s, discovers peers by broadcast only (fails over routers /
  WireGuard) and publish() goes to everyone. Do not use TrxNet here either.
- OI3 emits edges from a Timer1 compare ISR (4 µs); blocking keyer timing in
  the OI3 would add jitter.
- Late key-down shifts the rest of the over (gap stretches, mark keeps its
  length). Clock drift is corrected only in gaps, max 1.5 ms per mark start.
  Both devices have crystals (OI3 confirmed: crystal, not a resonator).
- Safety on the server: immediate key-up + mute on link timeout, max
  key-down, edge gap beyond redundancy, stale packet, interlock, rig not in
  CW/CW-R, tune. Unmute only at a word boundary (newest edge a key-up older
  than 5 dits) or when the sender is idle.
- Anti-spoofing/replay: SipHash-2-4 (128-bit shared key, 8 B tag); nonce
  issued by the OI3 in STATUS, rotated every 5 s (current + previous valid);
  a new session is accepted only with a valid nonce; `seq` must increase.
- Client send rules: packet on every edge; heartbeat 20 ms if any edge in the
  last 2 s, else 100 ms; edges of the last 500 ms, max 8, **plus always the
  newest edge while the key is down** (otherwise a dah at 5 WPM, 720 ms, would
  produce `nev = 0` = "idle" mid-mark); `tSend` taken after collecting the
  edges (OI3 compares `tSend − t_last` signed for the word gap).
- Session id is generated after WiFi is up (true RNG with the radio on). A
  repeated session id after a reboot would make the OI3 drop all packets
  (seq restarts at 1).
- Sidetone: LEDC on G3 runs continuously at the tone frequency, keying only
  switches the duty 0 / vol_duty(vol): fundamental ∝ sin(π·duty), so vol 1–100 is
  linear in dB (40 dB range, 2 dB per encoder step 5, 100 = 50 % duty, 0 = off),
  LEDC 14 bits, NVS key `vol2` (old `vol` was the raw duty 0–50). M5.Speaker is NOT used (it buffers);
  `cfg.internal_spk = false`. Backlight uses LEDC channel 7, we use 0/1.
- Keyer task pinned to core 1, `vTaskDelayUntil(…, 1)` (CONFIG_FREERTOS_HZ =
  1000), priority configMAX_PRIORITIES−2. Network, display, web on core 0.
- NVS writes stall both cores (flash cache off) → settings are saved only
  after the keyer has been idle for 2 s. `WiFi.persistent(false)`.
- WiFi: `setSleep(false)`; 4 saved networks (`WIFI_SLOTS`, the web JS gets it as `SLOTS`); ONE async scan of all channels,
  then join the strongest saved network by channel + BSSID (the user asked
  explicitly not to try the networks one by one). Exception at power-on: >= 2
  saved networks in range → `W_CHOOSE`, display menu (strongest first, auto
  join of the strongest after 10 s unless the encoder was turned); only until
  the first join, re-joins later are automatic. No scanning while connected
  unless the web page asks. `setAutoReconnect(false)`; own re-join logic.
- Web: basic auth `admin` / webPass; clients of the own AP skip auth (the AP
  already needs the same WPA2 password, and captive-portal windows cannot do
  basic auth). WiFi/web passwords are prefilled (user wants show/hide);
  the shared key is prefilled too (user's choice: the page is LAN-only), and
  the serial command `key` prints it as `[RkeyKey=...]`.
- Version: `FW_REV` in `version.h` = release date `YYYYMMDD[.N]`, set only
  by `release.sh`; `FW_VERSION` = "Paddle2IP " FW_REV (release.sh greps the
  binary for it to detect a stale out/).
- Web flasher on gh-pages (ESP Web Tools, pinned version): 4 separate parts at
  the arduino-cli upload offsets, NOT a merged image — NVS at 0x9000–0xDFFF
  sits between them and must survive. Firmware in `firmware/<rev>/` so a
  cached manifest never points at newer binaries. gh-pages is regenerated
  from `tools/webflash/index.html` with git plumbing (temp index), never
  edited by hand.
- Update check on the web page is done by the BROWSER: it fetches
  `FLASH_URL manifest.json` (GitHub Pages sends `Access-Control-Allow-Origin: *`)
  and compares `version` with FW_REV as YYYYMMDD*1000+N. No OTA (user's
  decision), the update itself stays the USB web flasher.
- Encoder via PCNT (x4), `ENC_DIRECTION = −1` (the first hardware feedback
  said it was reversed). `ENC_COUNTS_PER_DETENT = 4` still to verify.
- USB network (`usbnet.cpp`): the build stays `USBMode=hwcdc` (console, web
  flasher, panic output unchanged). Network mode is chosen at run time
  (display menu when SOF frames show a computer, 10 s → saved `usbNet`, NVS
  key `usbnet`): `USB.begin()` moves the PHY to USB-OTG, composite TinyUSB
  CDC (console via `con()`) + CDC-NCM. Back to serial = reboot;
  `usbnet_early()` clears the RTC PHY select (it survives warm resets).
  The user chose this over a fixed TinyUSB build.
- NCM driver: `ncm_device.c` vendored from TinyUSB 0.15.0 (the arduino lib is
  built without it), registered via the weak `usbd_app_driver_get_cb`, all
  entries under one mutex (USB task vs. TCP/IP task); padding added so an
  NTB never ends on a 64 B boundary (no ZLP from the controller).
- Paddle2IP is a DHCP client, the computer shares (NM `ipv4.method shared`).
  esp_netif route_prio 200. While USB has an address WiFi is OFF (user's
  choice; also lwIP would route the WiFi subnet over WiFi). No STATUS over USB
  for 10 s → USB netif down, WiFi back until the NCM link re-attaches.
  Session id is generated as soon as the radio is on, not with the link.
- mDNS `paddle2ip.local` (ESPmDNS, started in web_begin) for WiFi, AP and
  USB. IDF 4.4 mDNS only knows predefined netif keys, so the USB netif uses
  if_key `ETH_DEF` (no real Ethernet here) and posts IP_EVENT_ETH_GOT_IP.
  Shown on the service page only if the 4 lines below it still fit.
- Crash guard: RTC_NOINIT flag set when network mode starts, cleared after
  60 s; a panic/WDT/brownout reset with it set → that boot stays serial.
- The arduino "usbd" task is unpinned at max−1; lowered to max−3 (below the keyer).

## Toolchain facts

- Board package m5stack:esp32 **2.1.3** (arduino-esp32 2.x, ESP-IDF 4.4.6):
  `ledcSetup/ledcAttachPin` API, legacy `driver/pcnt.h`, **C++ is gnu++11**
  (no generic lambdas, no C++14 features).
- FQBN `m5stack:esp32:m5stack_dial:CDCOnBoot=cdc,USBMode=hwcdc`; the serial
  console needs USB CDC On Boot.
- Libraries: M5Dial 1.0.2, M5Unified 0.2.2, M5GFX 0.2.3.
- Sketch folder name must equal the .ino name: `paddle2ip/paddle2ip.ino`.
  `tools/` is not compiled by Arduino.
- arduino-cli lives in `~/bin` (build.sh adds it to PATH).
- OI3: `./build.sh` in the OI3 repo builds PCB_REV_3_141 and 3_1415. arduino-cli
  emits function prototypes above `#include <Ethernet.h>`, so `IPAddress` must
  not appear in function signatures of `rkey.ino`.

## Verification available without hardware

- `./build.sh` — firmware builds with no warnings in our sources.
- `tools/keyer_sim/` — keyer state machine on the host (mocked GPIO): exact
  1.00 / 3.00 dit timing, squeeze, iambic A vs B, CMOS timing.
- `tools/rkey_test.py selftest` — SipHash reference vector; the Python
  SipHash also matched the C implementation for 0–72 byte messages.
- `tools/rkey_test.py rig` + `send` on localhost — 20 % loss, 20 ms jitter:
  no lost edges, lengths exactly 1.00 / 3.00 dits. `attack` vs `rig` passes.
- OI3 repo `test/rkey_sim/` — server timing simulation (loss, jitter, drift,
  blackout).
- The embedded web JS can be syntax-checked with `node --check`.

## History

1. Server side in OI3 (`rkey.ino`, `rkey_proto.h`, cfg keys, docs, host
   simulation) was written first and delivered as a patch on OI3 commit
   050cc7f. Applied on OI3 branch `rkey`; build fix (IPAddress prototype),
   signed word-gap compare.
2. Client 1.0 written inside the OI3 repo as `rkey_dial/`: keyer, sidetone,
   KEY/STATUS, display, PCNT encoder, web config, NVS; `test/rkey_test.py`.
3. 1.1 after the first user feedback: captive portal, login show/reset over
   serial, network list on the web page, setup menu on the display, 3 saved
   networks with single-scan join, show/hide passwords, encoder reversed, own
   IP on the display, 180° display rotation, WiFi signal bars with colour.
4. Moved to this repository and renamed Paddle2IP (AP `Paddle2IP-XXXX`,
   hostname `paddle2ip`, NVS namespace `paddle2ip`, default web password
   `paddle2ip`, tasks `p2ip-*`). The server stays in OI3.
5. Info item in the display menu (FW revision + GitHub QR), `version.h`,
   `release.sh` (bump / build / upload / commit / push / gh-pages web flasher).
6. TEST item in the display menu: turn = keyer plays "TEST" (keyer.cpp
   `msg`, same ideal timing, paddle/straight key aborts); keyer_sim tests.
7. MONITOR item: CONN/DISC (fresh STATUS for our session, nonce ok) and a
   trace of the elements built from the same edges as the KEY packets
   (keyer.cpp `trace_edge`: mark < 2 dits = '.', gap >= 2 dits ' ',
   >= 5 dits " / "). It shows what was SENT, not what the server keyed: the
   protocol has no echo of played edges (that would need a STATUS extension
   in both repos).
8. After hardware feedback: MONITOR item removed (no new information); the
   trace is shown in the main screen bottom line while keying (until 10 s
   after the last edge, then cleared), unless a warning takes the line; the
   status line R/D/L moved above it. TX ring glow halved (solid 116..119,
   fade to 91). Sequencer digits light grey instead of red. Web save is
   post/redirect/get (the browser stayed on /save); reboot / AP restart
   after a save run from web_loop 1 s later, once the answer is out.
9. USB network (CDC-NCM) to a computer, preferred over WiFi: run-time
   switch from the Serial-JTAG (menu on plug-in, function USB, web), WiFi
   off while USB has an address, 10 s fallback, crash guard. No OI3 change:
   `rkey.ino` takes the peer address from every authenticated packet.

## Open items

- NOTHING has run on hardware yet. Check: encoder counts per detent, buzzer
  loudness at low duty, sidetone and edge timing under WiFi load, captive
  portal on Android / iOS, display layout on the round panel (trace in the
  bottom line, 13 chars of FreeMonoBold9pt7b), RSSI thresholds vs. real loss.
- Choose `RkeyDelayMs` / D on real links (LAN 30, internet 80–150, LTE 200+).
- OI3 `readSDSettings()` prints the cfg including `RkeyKey` to serial —
  consider masking.
- Nothing has been pushed to GitHub; both repos only have local commits.
  GitHub Pages has to be enabled once (release.sh offers it via `gh`).
- USB network not run on hardware: SOF host detection, PHY switch both ways
  (incl. after a crash), NCM with Linux cdc_ncm / Windows / macOS, DHCP from
  NM shared, edge timing over USB, web flasher from network mode (CDC reset →
  bootloader). If the TinyUSB 0.15 NCM driver proves flaky, try the rewritten
  one from TinyUSB ≥ 0.16 (if it builds against the 0.15 core).
- USB network fallback returns to USB only on re-attach (no periodic retry).
- mDNS over USB not verified: re-announce after the USB netif goes down/up
  (no ETH_EVENT is posted), name conflict handling with two Dials.
- Web flasher not tried on a real Dial yet (auto-reset into download mode
  over USB-Serial-JTAG, QR readability on the round panel).

## Working with the user

- The user writes in Czech; all code comments and documentation are in English.
- Match the existing style: short comments that explain why, compact C++,
  no new dependencies.
- Commit only when asked; never push without asking.
