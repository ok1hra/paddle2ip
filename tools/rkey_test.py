#!/usr/bin/env python3
"""Remote CW key — PC test tool for the OI3 server (rkey.ino) and the Paddle2IP client.

  send      act as Paddle2IP: key TEXT into the OI3 as KEY packets
  attack    check that forged, replayed and stale-nonce packets are refused
  rig       act as the OI3: receive KEY packets from Paddle2IP, print the edges
  selftest  check the SipHash-2-4 implementation against the reference vector

The protocol is rkey_proto.h (little-endian). This file is kept identical in
the OI3 repo (test/) and the Paddle2IP repo (tools/). The key is the 32-hex-char
RkeyKey: --key HEX, environment RKEY_KEY, or --cfg path/to/oi3.cfg.

Examples
  ./rkey_test.py send 192.168.1.50 --cfg ../oi3.cfg --wpm 25 --text "CQ TEST"
  ./rkey_test.py send 192.168.1.50 --key 00112233445566778899aabbccddeeff --loss 0.2 --jitter 30
  ./rkey_test.py attack 192.168.1.50 --cfg ../oi3.cfg
  ./rkey_test.py rig --key 00112233445566778899aabbccddeeff      (Paddle2IP server = this PC)
"""
import argparse
import os
import random
import re
import socket
import struct
import sys
import threading
import time

MAGIC, VER, T_KEY, T_STATUS = 0x4B, 1, 1, 2
MAX_EV, TAG_LEN, PORT = 8, 8, 5690
KF_ACTIVE = 0x01
SF = [(0x01, "CIV"), (0x02, "CW"), (0x04, "INTERLOCK"), (0x08, "KEYDOWN"),
      (0x10, "PTT"), (0x20, "NONCE_BAD"), (0x40, "MUTED"), (0x80, "TUNE")]
SF_CW_MODE, SF_NONCE_BAD = 0x02, 0x20

KEY_HDR = struct.Struct("<BBBBHBBIIIHH")          # 24 B
STATUS = struct.Struct("<BBBBHBBIIIIIHHHH")       # 36 B
assert KEY_HDR.size == 24 and STATUS.size == 36

# ---------------------------------------------------------------- SipHash-2-4
M64 = 0xFFFFFFFFFFFFFFFF


def _rotl(x, b):
    return ((x << b) | (x >> (64 - b))) & M64


def siphash24(key: bytes, m: bytes) -> int:
    k0, k1 = struct.unpack("<QQ", key)
    v0, v1 = 0x736F6D6570736575 ^ k0, 0x646F72616E646F6D ^ k1
    v2, v3 = 0x6C7967656E657261 ^ k0, 0x7465646279746573 ^ k1

    def rnd():
        nonlocal v0, v1, v2, v3
        v0 = (v0 + v1) & M64; v1 = _rotl(v1, 13); v1 ^= v0; v0 = _rotl(v0, 32)
        v2 = (v2 + v3) & M64; v3 = _rotl(v3, 16); v3 ^= v2
        v0 = (v0 + v3) & M64; v3 = _rotl(v3, 21); v3 ^= v0
        v2 = (v2 + v1) & M64; v1 = _rotl(v1, 17); v1 ^= v2; v2 = _rotl(v2, 32)

    n = len(m) - len(m) % 8
    for i in range(0, n, 8):
        mi, = struct.unpack_from("<Q", m, i)
        v3 ^= mi; rnd(); rnd(); v0 ^= mi
    b = ((len(m) & 0xFF) << 56) | int.from_bytes(m[n:], "little")
    v3 ^= b; rnd(); rnd(); v0 ^= b
    v2 ^= 0xFF
    rnd(); rnd(); rnd(); rnd()
    return v0 ^ v1 ^ v2 ^ v3


def sign(key, body):
    return body + struct.pack("<Q", siphash24(key, body))


def verify(key, pkt):
    return len(pkt) >= TAG_LEN and struct.unpack("<Q", pkt[-TAG_LEN:])[0] == siphash24(key, pkt[:-TAG_LEN])


def selftest():
    # Reference vector from the SipHash paper (Appendix A): key 00..0f, msg 00..0e
    got = siphash24(bytes(range(16)), bytes(range(15)))
    ok = got == 0xA129CA6149BE45E5
    print(f"siphash24 reference vector: {got:#018x} {'OK' if ok else 'FAIL'}")
    return 0 if ok else 1

# ---------------------------------------------------------------- helpers


def now_us():
    return (time.monotonic_ns() // 1000) & 0xFFFFFFFF


def load_key(args):
    hexkey = args.key or os.environ.get("RKEY_KEY")
    if not hexkey and args.cfg:
        with open(args.cfg, encoding="utf-8", errors="replace") as f:
            m = re.search(r"\[RkeyKey=([0-9A-Fa-f]{32})\]", f.read())
        if not m:
            sys.exit(f"no [RkeyKey=...] in {args.cfg}")
        hexkey = m.group(1)
    if not hexkey or not re.fullmatch(r"[0-9A-Fa-f]{32}", hexkey):
        sys.exit("need a 32-hex-char key: --key, RKEY_KEY or --cfg")
    key = bytes.fromhex(hexkey)
    if not any(key):
        sys.exit("all-zero key = not configured on the OI3")
    return key


def flags_str(f):
    return ",".join(n for b, n in SF if f & b) or "-"


def parse_status(key, pkt):
    if len(pkt) != STATUS.size + TAG_LEN or not verify(key, pkt):
        return None
    v = STATUS.unpack_from(pkt)
    if v[0] != MAGIC or v[1] != VER or v[2] != T_STATUS:
        return None
    names = ("magic ver type flags session civMode seqLevel seq echoTSend echoAgeUs "
             "nonce freqHz delayMs lateEv lostEv rxPkts").split()
    return dict(zip(names, v))


MORSE = {
    "A": ".-", "B": "-...", "C": "-.-.", "D": "-..", "E": ".", "F": "..-.", "G": "--.", "H": "....",
    "I": "..", "J": ".---", "K": "-.-", "L": ".-..", "M": "--", "N": "-.", "O": "---", "P": ".--.",
    "Q": "--.-", "R": ".-.", "S": "...", "T": "-", "U": "..-", "V": "...-", "W": ".--", "X": "-..-",
    "Y": "-.--", "Z": "--..", "0": "-----", "1": ".----", "2": "..---", "3": "...--", "4": "....-",
    "5": ".....", "6": "-....", "7": "--...", "8": "---..", "9": "----.", "/": "-..-.", "?": "..--..",
    ".": ".-.-.-", ",": "--..--", "=": "-...-", "+": ".-.-.",
}
DECODE = {v: k for k, v in MORSE.items()}


def text_edges(text, wpm, t0):
    """Ideal edges [(t_us, down)] like the Paddle2IP keyer generates them."""
    dit = 1200000 // wpm
    t, out = t0, []
    for ch in text.upper():
        if ch == " ":
            t += 4 * dit                      # 3 (letter) + 4 = 7 dits word space
            continue
        for el in MORSE.get(ch, ""):
            out.append((t, True)); t += dit if el == "." else 3 * dit
            out.append((t, False)); t += dit
        t += 2 * dit                          # 1 + 2 = 3 dits letter space
    return out

# ---------------------------------------------------------------- CTRL (Paddle2IP) side


class Sender:
    def __init__(self, args, key):
        self.key, self.args = key, args
        self.addr = (args.host, args.port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("", args.local_port))
        self.sock.settimeout(0.05)
        self.session = random.randint(1, 0xFFFF)
        self.seq = 0
        self.nonce = 0
        self.edges = []                       # [(t_us, down)], index = position
        self.last_status = None
        self.statuses = 0
        self.rtt = []
        self.lock = threading.Lock()
        self.running = True
        self.sent = []                        # raw packets (for the attack test)
        threading.Thread(target=self.rx_loop, daemon=True).start()

    def rx_loop(self):
        prev = None
        while self.running:
            try:
                pkt, _ = self.sock.recvfrom(256)
            except socket.timeout:
                continue
            except OSError:
                return
            arrive = now_us()
            st = parse_status(self.key, pkt)
            if not st:
                print("! STATUS with bad length/tag ignored")
                continue
            with self.lock:
                self.nonce = st["nonce"]
                self.last_status = st
                self.statuses += 1
                if st["session"] == self.session and st["echoTSend"]:
                    r = (arrive - st["echoTSend"] - st["echoAgeUs"]) & 0xFFFFFFFF
                    if r < 5000000:
                        self.rtt.append(r)
            sig = (st["flags"], st["civMode"], st["freqHz"], st["delayMs"], st["lateEv"], st["lostEv"])
            if sig != prev and not self.args.quiet:
                print(f"  STATUS flags={flags_str(st['flags'])} mode={st['civMode']:#04x} "
                      f"f={st['freqHz']} Hz seq={st['seqLevel']} D={st['delayMs']} ms "
                      f"late={st['lateEv']} lost={st['lostEv']} rx={st['rxPkts']} "
                      f"sess={'ours' if st['session'] == self.session else hex(st['session'])}")
                prev = sig

    def build(self, t, nonce=None, session=None, seq=None):
        with self.lock:
            n = len(self.edges)
            first = n
            while first > 0 and n - first < MAX_EV:
                et, ed = self.edges[first - 1]
                newest_down = first == n and ed
                if not newest_down and ((t - et) & 0xFFFFFFFF) >= 500000:
                    break
                first -= 1
            ev = self.edges[first:n]
            active = n and ((t - self.edges[-1][0]) & 0xFFFFFFFF) < 2000000
            if seq is None:
                self.seq += 1
                seq = self.seq
            hdr = KEY_HDR.pack(MAGIC, VER, T_KEY, KF_ACTIVE if active else 0,
                               self.session if session is None else session,
                               self.args.wpm, len(ev), seq, now_us(),
                               self.nonce if nonce is None else nonce,
                               first & 0xFFFF, self.args.delay)
        body = hdr + b"".join(struct.pack("<I", (et & ~1 & 0xFFFFFFFF) | int(ed)) for et, ed in ev)
        return sign(self.key, body)

    def send(self, pkt):
        self.sent.append(pkt)
        if random.random() < self.args.loss:
            return
        if self.args.jitter > 0:
            threading.Timer(random.random() * self.args.jitter / 1000.0,
                            self.sock.sendto, (pkt, self.addr)).start()
        else:
            self.sock.sendto(pkt, self.addr)

    def sync(self, timeout=2.0):
        """Heartbeat until the OI3 answers with a nonce and accepts our session."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.send(self.build(now_us()))
            time.sleep(0.1)
            with self.lock:
                st = self.last_status
            if st and st["session"] == self.session and not st["flags"] & SF_NONCE_BAD:
                return True
        return False

    def run_text(self, text):
        t0 = (now_us() + 300000) & 0xFFFFFFFF
        todo = text_edges(text, self.args.wpm, t0)
        last_send = 0
        i = 0
        while i < len(todo) or ((now_us() - todo[-1][0]) & 0xFFFFFFFF) < 1000000:
            t = now_us()
            fire = False
            while i < len(todo) and ((t - todo[i][0]) & 0xFFFFFFFF) < 0x80000000:
                with self.lock:
                    self.edges.append(todo[i])
                i += 1
                fire = True
            active = self.edges and ((t - self.edges[-1][0]) & 0xFFFFFFFF) < 2000000
            hb = 20000 if active else 100000
            if fire or ((t - last_send) & 0xFFFFFFFF) >= hb:
                self.send(self.build(t))
                last_send = t
            time.sleep(0.0005)
        return len(todo)


def cmd_send(args):
    key = load_key(args)
    s = Sender(args, key)
    print(f"-> {args.host}:{args.port} session={s.session:#06x} wpm={args.wpm} "
          f"D={'OI3 default' if not args.delay else str(args.delay) + ' ms'} "
          f"loss={args.loss:.0%} jitter=0-{args.jitter} ms")
    if not s.sync():
        print("no valid STATUS from the OI3 (RkeyEnable, RkeyKey, IP/port, firewall?)")
        return 1
    with s.lock:
        st = s.last_status
    if not st["flags"] & SF_CW_MODE:
        print("! OI3 reports TRX not in CW/CW-R: edges are accepted but not keyed")
    for r in range(args.repeat):
        n = s.run_text(args.text)
        print(f"sent '{args.text}' ({n} edges), packets so far {s.seq}")
    time.sleep(0.3)
    s.running = False
    with s.lock:
        st, rtt = s.last_status, sorted(s.rtt)
    if rtt:
        print(f"RTT min/median/max = {rtt[0] / 1000:.1f} / {rtt[len(rtt) // 2] / 1000:.1f} / "
              f"{rtt[-1] / 1000:.1f} ms ({len(rtt)} samples)")
    if st:
        print(f"OI3: rxPkts={st['rxPkts']} (sent {s.seq}) late={st['lateEv']} lost={st['lostEv']} "
              f"D={st['delayMs']} ms flags={flags_str(st['flags'])}")
    return 0


def cmd_attack(args):
    """Every check sends something the OI3 must NOT accept and verifies, via the
    rxPkts counter of the next genuine STATUS, that it did not."""
    key = load_key(args)
    args.loss, args.jitter, args.quiet = 0.0, 0, True
    s = Sender(args, key)
    if not s.sync():
        print("no valid STATUS from the OI3")
        return 1

    def rx_count():
        time.sleep(0.15)
        s.send(s.build(now_us()))            # genuine heartbeat -> fresh STATUS
        time.sleep(0.15)
        with s.lock:
            return s.last_status["rxPkts"], s.last_status

    fails = 0

    def check(name, inject, expect_delta=1):
        nonlocal fails
        before, _ = rx_count()
        inject()
        after, _ = rx_count()
        delta = (after - before) & 0xFFFF
        ok = delta == expect_delta
        print(f"{'PASS' if ok else 'FAIL'}  {name}: rxPkts +{delta} (expected +{expect_delta})")
        fails += not ok

    good = s.build(now_us())
    s.sock.sendto(good, s.addr)

    def bad_tag():
        p = bytearray(s.build(now_us())); p[-1] ^= 0x55
        s.sock.sendto(bytes(p), s.addr)

    def flipped_body():
        p = bytearray(s.build(now_us())); p[12] ^= 0x01   # tSend bit, tag unchanged
        s.sock.sendto(bytes(p), s.addr)

    def wrong_key():
        p = s.build(now_us())
        s.sock.sendto(sign(bytes(16 * [0x42]), p[:-TAG_LEN]), s.addr)

    def replay():
        for _ in range(3):
            s.sock.sendto(good, s.addr)        # old seq within the running session

    def new_session_stale_nonce():
        p = s.build(now_us(), nonce=0x12345678, session=(s.session ^ 0x5A5A) or 1)
        s.sock.sendto(p, s.addr)

    def keydown_old_session():
        # a captured packet of another session, re-sent later: needs a fresh nonce
        p = s.build(now_us(), nonce=0, session=(s.session ^ 0x1111) or 1)
        s.sock.sendto(p, s.addr)

    # the genuine heartbeat in rx_count() itself adds +1
    check("bad tag", bad_tag)
    check("modified body", flipped_body)
    check("wrong key", wrong_key)
    check("replayed packet (seq not increasing)", replay)
    check("new session with stale nonce", new_session_stale_nonce)
    check("new session without nonce", keydown_old_session)
    s.running = False
    print("all checks passed" if not fails else f"{fails} check(s) FAILED")
    return 1 if fails else 0

# ---------------------------------------------------------------- RIG (fake OI3)


def cmd_rig(args):
    """Minimal OI3 stand-in: verifies packets, issues nonces, answers STATUS,
    prints each new edge with its mark/space length from the sender clock."""
    key = load_key(args)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("", args.port))
    sock.settimeout(0.05)
    nonce, nonce_prev, nonce_t = random.getrandbits(32) or 1, 0, time.monotonic()
    session, last_seq, next_idx = None, 0, 0
    last_ev = None
    st_seq, last_st, echo_t, rx_t, rx_pkts, bad = 0, 0.0, 0, 0, 0, 0
    skew_min = None
    elem, letter = "", ""
    print(f"fake OI3 listening on UDP {args.port}, D={args.delay or 120} ms")
    while True:
        if time.monotonic() - nonce_t > 5:
            nonce_prev, nonce, nonce_t = nonce, random.getrandbits(32) or 1, time.monotonic()
        try:
            pkt, peer = sock.recvfrom(256)
        except socket.timeout:
            continue
        arrive = now_us()
        if len(pkt) < KEY_HDR.size + TAG_LEN:
            continue
        h = KEY_HDR.unpack_from(pkt)
        magic, ver, typ, kflags, sess, wpm, nev, seq, tsend, knonce, idx0, dly = h
        if magic != MAGIC or ver != VER or typ != T_KEY or nev > MAX_EV or \
                len(pkt) != KEY_HDR.size + 4 * nev + TAG_LEN:
            continue
        if not verify(key, pkt):
            bad += 1
            print(f"! bad tag from {peer[0]} ({bad})")
            continue
        nonce_ok = knonce != 0 and knonce in (nonce, nonce_prev)
        accepted = False
        if session != sess:
            if nonce_ok:
                print(f"new session {sess:#06x} from {peer[0]}:{peer[1]} wpm={wpm} delay={dly}")
                session, next_idx, accepted = sess, (idx0 + nev) & 0xFFFF, True
        elif ((seq - last_seq) & 0xFFFFFFFF) < 0x80000000 and seq != last_seq:
            accepted = True
        if accepted:
            last_seq, echo_t, rx_t = seq, tsend, arrive
            rx_pkts = (rx_pkts + 1) & 0xFFFF
            s = (arrive - tsend) & 0xFFFFFFFF
            s = s - (1 << 32) if s >= 0x80000000 else s
            skew_min = s if skew_min is None else min(skew_min, s)
            if nonce_ok:
                for i in range(nev):
                    idx = (idx0 + i) & 0xFFFF
                    rel = (idx - next_idx) & 0xFFFF
                    if rel >= 0x8000:
                        continue
                    e, = struct.unpack_from("<I", pkt, KEY_HDR.size + 4 * i)
                    if rel:
                        print(f"! {rel} edge(s) lost")
                    next_idx = (idx + 1) & 0xFFFF
                    t, down = e & ~1, e & 1
                    jit = (s - skew_min) / 1000
                    if last_ev:
                        dur = ((t - last_ev[0]) & 0xFFFFFFFF) / 1000
                        dit = 1200 / max(wpm, 1)
                        if last_ev[1]:                        # a mark ended
                            elem += "." if dur < 2 * dit else "-"
                            kind = "mark"
                        else:
                            kind = "space"
                            if dur > 2 * dit and elem:
                                letter += DECODE.get(elem, "*"); elem = ""
                            if dur > 5 * dit:
                                letter += " "
                        print(f"  {kind:5s} {dur:7.1f} ms ({dur / dit:4.2f} dit)  pkt jitter {jit:5.1f} ms  {letter[-30:]}")
                    last_ev = (t, down)
        # STATUS (rate-limited like the OI3, immediately when the nonce was bad)
        if time.monotonic() - last_st >= 0.1 or not nonce_ok:
            last_st = time.monotonic()
            st_seq += 1
            fl = 0x02 | (0x20 if not nonce_ok else 0) | 0x01
            body = STATUS.pack(MAGIC, VER, T_STATUS, fl, session or 0, 0x03, 0, st_seq,
                               echo_t, (now_us() - rx_t) & 0xFFFFFFFF, nonce, 14025000,
                               dly or args.delay or 120, 0, 0, rx_pkts)
            sock.sendto(sign(key, body), peer)

# ---------------------------------------------------------------- main


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p, host=True):
        if host:
            p.add_argument("host", help="OI3 IP address or hostname")
        p.add_argument("--port", type=int, default=PORT, help="UDP port (RkeyPort), default 5690")
        p.add_argument("--key", help="32 hex chars (RkeyKey)")
        p.add_argument("--cfg", help="read [RkeyKey=...] from this oi3.cfg")
        p.add_argument("--delay", type=int, default=0, help="requested playout delay ms, 0 = OI3 default")

    p = sub.add_parser("send", help="key text into the OI3")
    common(p)
    p.add_argument("--text", default="CQ CQ TEST")
    p.add_argument("--wpm", type=int, default=25)
    p.add_argument("--repeat", type=int, default=1)
    p.add_argument("--loss", type=float, default=0.0, help="simulated packet loss 0..1")
    p.add_argument("--jitter", type=float, default=0.0, help="simulated extra delay 0..N ms")
    p.add_argument("--local-port", type=int, default=0)
    p.add_argument("--quiet", action="store_true")

    p = sub.add_parser("attack", help="forged / replayed / stale packets must be refused")
    common(p)
    p.add_argument("--wpm", type=int, default=25)
    p.add_argument("--local-port", type=int, default=0)

    p = sub.add_parser("rig", help="fake OI3 for testing Paddle2IP")
    common(p, host=False)

    sub.add_parser("selftest", help="SipHash reference vector")

    args = ap.parse_args()
    if args.cmd == "selftest":
        return selftest()
    if selftest() != 0:
        return 1
    try:
        return {"send": cmd_send, "attack": cmd_attack, "rig": cmd_rig}[args.cmd](args)
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
