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
  switches the duty 0 / vol % (1–50). M5.Speaker is NOT used (it buffers);
  `cfg.internal_spk = false`. Backlight uses LEDC channel 7, we use 0/1.
- Keyer task pinned to core 1, `vTaskDelayUntil(…, 1)` (CONFIG_FREERTOS_HZ =
  1000), priority configMAX_PRIORITIES−2. Network, display, web on core 0.
- NVS writes stall both cores (flash cache off) → settings are saved only
  after the keyer has been idle for 2 s. `WiFi.persistent(false)`.
- WiFi: `setSleep(false)`; 3 saved networks; ONE async scan of all channels,
  then join the strongest saved network by channel + BSSID (the user asked
  explicitly not to try the networks one by one). No scanning while connected
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
- Encoder via PCNT (x4), `ENC_DIRECTION = −1` (the first hardware feedback
  said it was reversed). `ENC_COUNTS_PER_DETENT = 4` still to verify.

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

## Open items

- NOTHING has run on hardware yet. Check: encoder counts per detent, buzzer
  loudness at low duty, sidetone and edge timing under WiFi load, captive
  portal on Android / iOS, display layout on the round panel (bottom line
  with RTT > 99 ms may touch the red ring), RSSI thresholds vs. real loss.
- Choose `RkeyDelayMs` / D on real links (LAN 30, internet 80–150, LTE 200+).
- OI3 `readSDSettings()` prints the cfg including `RkeyKey` to serial —
  consider masking.
- Nothing has been pushed to GitHub; both repos only have local commits.
  GitHub Pages has to be enabled once (release.sh offers it via `gh`).
- Web flasher not tried on a real Dial yet (auto-reset into download mode
  over USB-Serial-JTAG, QR readability on the round panel).

## Working with the user

- The user writes in Czech; all code comments and documentation are in English.
- Match the existing style: short comments that explain why, compact C++,
  no new dependencies.
- Commit only when asked; never push without asking.
