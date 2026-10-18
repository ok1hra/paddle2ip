#!/usr/bin/env bash
#
# Build the Paddle2IP firmware (M5Stack Dial) into out/.
#
# rkey_proto.h is the wire protocol shared with the server (OI3 firmware,
# https://github.com/ok1hra/oi3). If an OI3 checkout is found (OI3_DIR, default
# ../../oi3), the build fails when the two copies differ.

set -euo pipefail
cd "$(dirname "$0")"
export PATH="$HOME/bin:$PATH"

FQBN="m5stack:esp32:m5stack_dial:CDCOnBoot=cdc,USBMode=hwcdc"   # Serial = USB (console)
OI3_DIR="${OI3_DIR:-../../oi3}"

if [[ -f "$OI3_DIR/rkey_proto.h" ]]; then
  if ! cmp -s rkey_proto.h "$OI3_DIR/rkey_proto.h"; then
    echo "error: rkey_proto.h differs from $OI3_DIR/rkey_proto.h (protocol mismatch)" >&2
    exit 1
  fi
  echo "rkey_proto.h matches $OI3_DIR/rkey_proto.h"
else
  echo "note: no OI3 checkout at $OI3_DIR, protocol header not cross-checked"
fi

command -v arduino-cli >/dev/null 2>&1 || { echo "error: arduino-cli not found in PATH" >&2; exit 1; }

rm -rf out
arduino-cli compile -b "$FQBN" --warnings default --output-dir out .
ls -l out/paddle2ip.ino.bin
