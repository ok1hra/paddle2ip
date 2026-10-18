# Paddle2IP

Remote CW paddle over IP. Paddle2IP runs on an **M5Stack Dial v1.1** (ESP32-S3):
you plug a paddle into it, it keys a local iambic keyer with zero-latency
sidetone, and sends the **key edges with their exact timestamps** over WiFi/UDP
to a server at the transceiver. The server replays every edge a fixed time
later (the playout delay), so the marks and spaces on air are as long as the
keyer made them, whatever the network jitter is.

The reference server is the **Open Interface III (OI3)** firmware,
https://github.com/ok1hra/oi3 (`rkey.ino`). Any other device can be a server;
see [Implementing a server](#implementing-a-server).

```
paddle ─► Paddle2IP: iambic keyer ─► sidetone (buzzer / headphones, 0 ms)
                      │ KEY: edges (t_us, up/down), 8× redundant, SipHash tag
                      ▼ UDP 5690
          server (OI3): jitter buffer ─► CW key line (+ PTT sequencer)
                      │ STATUS: nonce, RTT echo, rig freq/mode, counters
                      ▼
          Paddle2IP display: WPM, link, delay, rig freq/mode, WiFi signal
```

> Status: the firmware builds and its keyer, protocol and test tools are
> verified on a PC; it has **not been tested on hardware yet**.

## Contents

- [Hardware](#hardware)
- [Build and flash](#build-and-flash)
- [First setup](#first-setup)
- [Operation](#operation)
  - [Display](#display)
  - [Encoder and button](#encoder-and-button)
  - [Setup mode](#setup-mode)
  - [WiFi](#wifi)
  - [Web page](#web-page)
  - [Serial console](#serial-console)
  - [Choosing the playout delay](#choosing-the-playout-delay)
- [Testing without a rig](#testing-without-a-rig)
- [Firmware structure](#firmware-structure)
- [Implementing a server](#implementing-a-server)

## Hardware

| What | Where | Notes |
|---|---|---|
| Paddle dit | Port B **G2** | paddle common to GND; internal pull-up |
| Paddle dah | Port B **G1** | swap on the web page if needed |
| Paddle filter | at the connector | 100 Ω in series with each contact, 10 nF to GND |
| Straight key (optional) | Port A **G13** | to GND, enable on the web page |
| Headphone tone (optional) | Port A **G15** | PWM square wave: RC low-pass + series resistor, enable on the web page |
| Sidetone | internal buzzer G3 | LEDC, volume = duty 1–50 % |
| Encoder / button | G40/G41, G42 | |

**Do not connect the red 5 V wire** of the Grove cable to the paddle.
The Dial keeps itself powered from the battery (HOLD = GPIO46, set by `M5Dial.begin()`).

## Build and flash

**Without building:** open the web flasher https://ok1hra.github.io/paddle2ip/
in Chrome or Edge, connect the Dial over USB-C and click *Flash firmware*.
Do not erase the device when asked if you want to keep the settings.

To build it yourself: Arduino IDE 2.x or arduino-cli with:

- board package **M5Stack** (tested with 2.1.3), board **M5Dial**
  (`m5stack:esp32:m5stack_dial`)
- *Tools → USB CDC On Boot: **Enabled*** (the serial console runs over USB)
- library **M5Dial** (pulls in M5Unified and M5GFX; tested with M5Dial 1.0.2,
  M5Unified 0.2.2, M5GFX 0.2.3)

Open `paddle2ip.ino` (the folder must be called `paddle2ip`), or run:

```
./build.sh                 # -> out/paddle2ip.ino.bin
arduino-cli upload -b m5stack:esp32:m5stack_dial -p /dev/ttyACM0 --input-dir out .
```

`build.sh` also checks that `rkey_proto.h` is identical to the copy in an OI3
checkout next to this one (`OI3_DIR`, default `../../oi3`).

### Release

`./release.sh` runs the release steps and asks before each one (Enter = yes,
`n` = skip; `./release.sh -y` does all of them without asking):

1. set `FW_REV` in `version.h` to today's date, `YYYYMMDD` (a second release
   on the same day becomes `YYYYMMDD.2`, …)
2. build (`build.sh`)
3. upload to a connected Dial; with more serial devices it lists them to choose
4. commit all changes (the message can be edited)
5. push the current branch
6. web flasher: generate the page (`tools/webflash/index.html`, ESP Web Tools),
   `manifest.json` and the firmware parts, commit them to the `gh-pages`
   branch (without touching the working tree) and push it. If GitHub Pages is
   not enabled yet and `gh` is logged in, it offers to enable it.

Steps 3 and 6 warn if `out/` was not built from the current `FW_REV`. The
web flasher writes bootloader, partition table, `boot_app0` and the app
separately (the same offsets as `arduino-cli upload`), so the settings in NVS
survive unless the user chooses erase.

## First setup

1. Power on with nothing configured (or hold the button under the display
   while powering on). The display shows **SETUP**, the AP name
   `Paddle2IP-XXXX`, its password (default `paddle2ip`) and `http://192.168.4.1`.
2. Join that WiFi with a phone or laptop. The configuration page opens by
   itself (captive portal); if not, open http://192.168.4.1.
3. Up to three WiFi networks: the page scans and lists the networks in range,
   a click fills the SSID, type the password.
4. Server: the OI3 IP address or hostname and UDP port (OI3 `RkeyPort`, default 5690).
5. Shared key: either paste the 32-hex-char key from the server, or press
   *Generate new key* and copy the shown `[RkeyKey=...]` line into `oi3.cfg`
   on the OI3 SD card. The key is shown only this once.
6. On the OI3 set `[RkeyEnable=1]` and the same `RkeyKey`, restart it.
7. *Save*. Paddle2IP reboots, joins the strongest saved network and connects.
   The ring turns green when the server answers.

## Operation

### Display

| Area | Content |
|---|---|
| top | own IP address (`AP 192.168.4.1` when only the AP runs) |
| below | rig frequency and mode, sequencer level (from the server) |
| centre | big value of the selected encoder function (normally WPM) |
| warning line | NO KEY, NO WIFI, NO SERVER, NO LINK, SYNC (stale nonce), INTERLOCK, TUNE, TRX NOT CW, MUTED |
| bottom 1 | `RTT last/min ms`, playout delay D in use, server `L lost/late` edge counters |
| bottom 2 | WiFi signal (bars + dBm), uplink/downlink packet loss in % |
| ring | **red** = the server has the key down, green = live link, grey = no link |

WiFi signal colour: green ≥ −65 dBm, orange −65 … −75 dBm (at the edge), red
below −75 dBm (drop-outs likely). Thresholds: `RSSI_GOOD` / `RSSI_EDGE` in `shared.h`.

MUTED: after any abort (link loss, interlock, …) the server keeps the stream
muted until the next word gap or until you stop sending, so a broken
character is never transmitted. Just continue with the next word.

### Encoder and button

| Action | Key screen | Setup screen |
|---|---|---|
| turn | change the selected value | select a network |
| short press | next function | join the selected network |
| long press | info page (IP, RSSI, server, session, counters, login) | info page |

Functions (one detent = one step; the display returns to WPM 6 s after the last change):

| Function | Range | Step |
|---|---|---|
| WPM | 5–60 | 1 |
| Tone | 300–1200 Hz | 10 Hz |
| Volume | 1–50 % (buzzer duty) | 1 |
| Delay D | 0 (= server default) or 10–1000 ms | 10 ms |
| Iambic | A / B | toggle |
| Rotate | 0 / 180° | toggle |
| Info | FW revision, QR code + text of the GitHub URL (stays 30 s) | – |

Values are saved to flash 3 s after the last change, but only after the keyer
has been idle for 2 s (a flash write stalls both CPU cores for a moment).

### Setup mode

Entered by holding the button at power-on, or when no network is saved.
Paddle2IP runs its own AP `Paddle2IP-XXXX` with a captive portal and the
display shows a menu: *best in range* or one of the saved networks. Turn to
select, press to join. On success Paddle2IP leaves setup mode and closes the
AP 30 s after the last client has left.

Outside setup mode the same AP is opened as a fallback when no saved network
could be joined for 20 s; it closes again 30 s after the WiFi is back and
nobody uses it.

### WiFi

Up to three networks are saved. Paddle2IP does **one** scan of all channels
(about 1.5 s) and joins the strongest saved network in range directly by
channel and BSSID; it never waits for networks that are not there. A lost
link is re-joined on the same AP for 6 s, then a new scan picks the best
network again. Scans never run while connected unless the web page asks
(a scan takes the radio off the channel for a moment). Modem sleep is off.

### Web page

`http://<ip>/`, login user `admin`, password = the web password (default
`paddle2ip`, also the AP password). Clients of the Paddle2IP AP need no login,
because joining the AP already took the same password.

- WiFi networks: list of networks in range (*Scan*), three SSID/password slots.
- Server: IP or hostname, port, shared key (write-only), playout delay D.
- Keyer: WPM, tone, volume, iambic A/B, paddle swap, straight key on G13,
  headphone tone on G15, display rotation.
- Login: the web/AP password.
- *Generate new key*: stores a random key and shows it once for `oi3.cfg`.
- `/status`: link state as JSON.

Every password field has a show/hide button. WiFi and web passwords are filled
in; the shared key is never sent back to the browser.

### Serial console

USB, 115200 baud. Use it when the login is lost.

| Command | |
|---|---|
| `show` | web login and password, AP name, IP addresses, saved networks, server |
| `resetpass` | web/AP password back to `paddle2ip` |
| `pass <new>` | set the web/AP password (8–32 chars) |
| `wifi` | rescan and join the best saved network |
| `reboot` | |

### Choosing the playout delay

D must be larger than the network jitter (the spread of the one-way delay),
otherwise edges arrive late; the server then shifts the rest of the over and
raises D by itself (`L` late counter on the display). Start with:

| Network | D |
|---|---|
| LAN | 30 ms |
| good internet / VPN | 80–150 ms |
| LTE | 200 ms and more |

D = 0 uses the server's default (OI3 `RkeyDelayMs`). The OI3 also raises D on
the first element of an over to cover its PTT sequencer lead times.

## Testing without a rig

`tools/rkey_test.py` (Python 3, no dependencies; identical copy in the OI3 repo):

```
tools/rkey_test.py rig --key <hex>        # fake server on UDP 5690: point Paddle2IP at this PC
tools/rkey_test.py send <server-ip> --key <hex> --text "CQ TEST" --wpm 25
tools/rkey_test.py attack <server-ip> --key <hex>   # forged / replayed packets must be refused
tools/rkey_test.py selftest               # SipHash-2-4 reference vector
```

`rig` verifies the packets, issues nonces, answers STATUS, and prints every
mark/space length from the sender clock (should be exactly 1.00 / 3.00 dits),
the packet jitter and the decoded text.

Keyer state machine on the PC (mocked GPIO, 1 ms tick):

```
cd tools/keyer_sim && g++ -std=gnu++17 -w -Imock -I../.. keyer_sim.cpp -o keyer_sim && ./keyer_sim
```

## Firmware structure

| Task | Core | Priority | Job | File |
|---|---|---|---|---|
| keyer | 1 | max−2 | 1 ms tick: paddle, iambic A/B, sidetone, edge ring | `keyer.cpp` |
| p2ip-net | 0 | 10 | KEY packets (on every edge + heartbeat), STATUS: nonce, RTT, loss | `net.cpp` |
| p2ip-ui | 0 | 2 | display, encoder (PCNT), button, WiFi, web page, console, NVS | `ui.cpp`, `wlan.cpp`, `web.cpp`, `console.cpp`, `settings.cpp` |

`shared.h` holds pins, settings and the interfaces between the tasks;
`rkey_proto.h` is the wire protocol.

The iambic logic is the OI3 keyer (K3NG port: modes A/B, CMOS Super Keyer
timing) rewritten as a non-blocking state machine. Edge timestamps are the
keyer's **ideal** times: consecutive elements are chained exactly (1 or 3 dits
+ 1 dit gap); only the first element after idle starts at the tick that saw
the paddle. The sidetone follows within one tick (1 ms). The buzzer runs LEDC
continuously at the tone frequency, keying only switches the duty (M5.Speaker
is not used, it buffers audio).

---

## Implementing a server

This section is for anyone who wants to build another receiver (a different
interface board, a PC program, an SDR, …). The OI3 implementation
(`rkey.ino` in https://github.com/ok1hra/oi3) is the reference; its host
simulation (`test/rkey_sim/`) shows the expected timing behaviour.

### Principle

The client's keyer generates the elements, so it knows their ideal edge
times. It sends those times (its own microsecond clock), never sampled key
state. The server plays every edge at

```
playAt = t_sender + offset,   offset = min(arrival − t_sender) + D
```

`min(arrival − t_sender)` over a sliding window (1–2 s) is the network latency
without jitter plus the clock offset between the two devices; `D` absorbs the
jitter. Because every edge of a character gets the same offset, element
lengths are reproduced exactly.

### Transport

- UDP, server port default **5690**. The server sends STATUS back to the
  source address and port of the last authenticated KEY packet (works
  through NAT/VPN; no discovery, no broadcast).
- All multi-byte fields are **little-endian**. Structures are packed.
- `magic = 0x4B`, `ver = 1`. Drop anything else.

### KEY packet (client → server)

24-byte header, then `nev` × 4-byte edges, then the 8-byte tag.
Total length must be exactly `24 + 4·nev + 8`, `nev ≤ 8`.

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 1 | `magic` | 0x4B |
| 1 | 1 | `ver` | 1 |
| 2 | 1 | `type` | 1 = KEY |
| 3 | 1 | `flags` | bit 0 ACTIVE: an edge in the last 2 s |
| 4 | 2 | `session` | random per client boot, never 0 |
| 6 | 1 | `wpm` | sender speed (PTT hang time, word-gap detection) |
| 7 | 1 | `nev` | edges in this packet; 0 = client idle |
| 8 | 4 | `seq` | packet counter, strictly increasing within a session |
| 12 | 4 | `tSend` | client µs clock when sending |
| 16 | 4 | `nonce` | last nonce from a STATUS (0 = none yet) |
| 20 | 2 | `evIdx0` | index of the first edge; edge i has index `evIdx0 + i` (wraps at 16 bits) |
| 22 | 2 | `delayMs` | requested playout delay, 0 = server default; servers accept 10–1000 |
| 24 | 4·n | edges | `(t_us & ~1) | down` — client µs time, bit 0 = new key state (1 = down) |
| end | 8 | tag | SipHash-2-4 of all preceding bytes |

Client send rules (what a server can rely on):

- a packet immediately on every edge;
- a heartbeat every 20 ms while ACTIVE, every 100 ms when idle;
- each packet repeats the edges of the last 500 ms (max 8), and always the
  newest edge while the key is down, so up to ~7 consecutive lost packets lose
  nothing;
- edge indices grow for the whole session; `tSend` is never older than any
  edge in the packet.

### STATUS packet (server → client)

36 bytes + 8-byte tag. Send it in reply to KEY packets, at most every 100 ms,
and immediately when the client's nonce was not valid.

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 1 | `magic` | 0x4B |
| 1 | 1 | `ver` | 1 |
| 2 | 1 | `type` | 2 = STATUS |
| 3 | 1 | `flags` | see below |
| 4 | 2 | `session` | the session the server is running (client checks "ours") |
| 6 | 1 | `civMode` | rig mode byte (CI-V: 0x03 CW, 0x07 CW-R, …) |
| 7 | 1 | `seqLevel` | PTT sequencer level, 0 = idle (display only) |
| 8 | 4 | `seq` | status counter (client derives downlink loss) |
| 12 | 4 | `echoTSend` | `tSend` of the last accepted KEY packet |
| 16 | 4 | `echoAgeUs` | server µs since that packet arrived; client: `RTT = now − echoTSend − echoAgeUs` |
| 20 | 4 | `nonce` | current nonce |
| 24 | 4 | `freqHz` | rig frequency, 0 = unknown |
| 28 | 2 | `delayMs` | D actually in use (incl. adaptive extra) |
| 30 | 2 | `lateEv` | edges that arrived too late |
| 32 | 2 | `lostEv` | edges never received |
| 34 | 2 | `rxPkts` | accepted KEY packets (wraps; client derives uplink loss) |
| 36 | 8 | tag | SipHash-2-4 of bytes 0–35 |

Flags: 0x01 CIV_VALID (freq/mode valid), 0x02 CW_MODE (keying allowed),
0x04 INTERLOCK, 0x08 KEY_DOWN (key line closed now), 0x10 PTT (sequencer up),
0x20 NONCE_BAD (last KEY had a stale nonce), 0x40 MUTED, 0x80 TUNE.

### Authentication

- **SipHash-2-4**, 128-bit shared key (32 hex chars in the configuration;
  all-zero = not configured = server off), 64-bit tag appended little-endian.
  Reference vector: key `00 01 … 0f`, message `00 01 … 0e` → `0xa129ca6149be45e5`.
  `rkey_proto.h` contains a portable implementation (~1.2 ms per packet on a
  16 MHz AVR).
- **Nonce**: the server issues a random 32-bit nonce in every STATUS and
  rotates it every 5 s; it accepts the current and the previous one. Derive it
  from something unpredictable (OI3: SipHash of a counter, µs time, boot
  counter and ADC noise).
- **Sessions and replay**: a KEY packet with a session the server is not
  running is accepted only with a valid nonce, so old captures cannot start a
  session. Within a session `seq` must increase (`(int32)(seq − lastSeq) > 0`).

### Receive algorithm

```
on KEY packet:
  check magic/ver/type, nev <= 8, exact length          else drop
  verify tag                                            else drop (count)
  remember source address/port for STATUS
  nonceOk = nonce != 0 && nonce in {current, previous}
  if session != running session:
      if !nonceOk: send STATUS now (carries a fresh nonce); drop
      start session: nextIdx = evIdx0 + nev   (do not replay its history)
                     muted = true, extra = 0, reset latency window
  else if (int32)(seq - lastSeq) <= 0: drop             (duplicate / replay)
  lastSeq = seq; lastPacketTime = now; rxPkts++
  latency sample s = arrival - tSend  -> sliding-window minimum
  send STATUS (rate-limited; immediately if !nonceOk)
  if !nonceOk: stop here (edges come again in the next packets)
  if muted and keying allowed: unmute when
      nev == 0  (client idle)  or
      all edges known, newest edge is key-up and (int32)(tSend - t_last) > 5 dits
  for each edge with index >= nextIdx:
      if index > nextIdx: edges lost -> abort + mute
      nextIdx = index + 1
      if muted or packet stale (s - minLatency > 400 ms): skip (abort + mute if stale)
      ignore if it does not change the key state
      schedule it
```

### Playout

- First edge of an over (key idle): `offset = minLatency + D`. Make D at
  least your PTT/sequencer lead time, so the first mark is not cut.
- Later key-down edges: move `offset` toward `minLatency + D` by at most
  ~1.5 ms per mark start. That corrects clock drift (crystals, < 100 ppm
  typically; handle up to 0.5 %) only inside gaps.
- A key-down that would play less than ~1.5 ms in the future is **late**:
  shift the offset of the rest of the over by the lateness (the gap absorbs
  it, the mark keeps its length) and add the lateness to an adaptive extra
  delay (cap ~500 ms) that decays by ¼ at each new over.
- A late key-up is played immediately.
- Emit edges from a hardware timer, not from the main loop (OI3: Timer1
  compare ISR, 4 µs resolution). Keep the queue monotonic.

### Safety rules (all mandatory)

Release the key immediately, drop queued edges and **mute** when:

- no packet for the link timeout while the key is down (OI3: 300 ms);
- one key-down lasts longer than the maximum (OI3: 3 s);
- edges are lost beyond the redundancy (index gap);
- a stale packet arrives;
- keying stops being allowed: wrong mode (not CW/CW-R), interlock, tune,
  network down, server disabled;
- the PTT sequencer does not become ready in time (OI3: 3 s).

Muted means: nothing is keyed until the client is idle (`nev = 0`) or its
newest edge is a key-up older than 5 dits (word gap). Never resume inside a
character.

PTT: request it when the first edge is pending, key only after the sequencer
reports ready (+ PTT lead), hold it for ~7 dits at the sender's WPM after the
last key-up, then let the sequencer apply its tail.

### Checklist

- [ ] `tools/rkey_test.py selftest` and your SipHash give the same tag
- [ ] `tools/rkey_test.py attack <server>` passes all checks
- [ ] `tools/rkey_test.py send <server> --loss 0.2 --jitter 40` keys clean CW
      (check with a second receiver or a scope on the key line)
- [ ] pulling the network cable mid-mark releases the key within the link timeout
- [ ] a Paddle2IP shows the server's mode, frequency, RTT and a green ring

### Protocol changes

`rkey_proto.h` must stay identical in this repository and in the OI3
repository. Any incompatible change needs a new `RKEY_VER`.
