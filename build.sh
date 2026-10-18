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
log=$(mktemp)
trap 'rm -f "$log"' EXIT
arduino-cli compile -b "$FQBN" --warnings default --output-dir out . 2>&1 | tee "$log"
ls -l out/paddle2ip.ino.bin

# memory usage summary -> out/memory.txt (release.sh shows it again at the end)
bar() {                                   # bar used max -> "[#####-----]  71 %"
  local p=$(( $1 * 100 / $2 )) n=$(( $1 * 30 / $2 ))
  printf '[%s%s] %3d %%' "$(printf '%*s' "$n" '' | tr ' ' '#')" "$(printf '%*s' $(( 30 - n )) '' | tr ' ' '-')" "$p"
}
fu=$(sed -n 's/^Sketch uses \([0-9]*\) bytes.*Maximum is \([0-9]*\) bytes.*/\1 \2/p' "$log")
ru=$(sed -n 's/^Global variables use \([0-9]*\) bytes.*Maximum is \([0-9]*\) bytes.*/\1 \2/p' "$log")
if [[ -n "$fu" && -n "$ru" ]]; then
  read -r fUsed fMax <<<"$fu"; read -r rUsed rMax <<<"$ru"
  {
    printf 'flash (app)   %8d / %8d B  %s\n' "$fUsed" "$fMax" "$(bar "$fUsed" "$fMax")"
    printf 'RAM (static)  %8d / %8d B  %s\n' "$rUsed" "$rMax" "$(bar "$rUsed" "$rMax")"
  } >out/memory.txt
  echo; cat out/memory.txt
fi
