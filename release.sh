#!/usr/bin/env bash
#
# Release Paddle2IP. Every step asks first (Enter = yes, n = skip):
#   1. version: FW_REV in version.h from today's date (YYYYMMDD[.N])
#   2. build:   ./build.sh -> out/
#   3. upload:  flash a connected Dial (choose one if there are more)
#   4. commit:  all changes
#   5. push:    current branch to origin
#   6. pages:   web flasher (ESP Web Tools) on the gh-pages branch, push
#
#   ./release.sh        ask before each step
#   ./release.sh -y     do everything without asking

set -euo pipefail
cd "$(dirname "$0")"
export PATH="$HOME/bin:$PATH"

FQBN=$(sed -n 's/^FQBN="\([^"]*\)".*/\1/p' build.sh)   # one place: build.sh
BIN=out/paddle2ip.ino.bin
YES=0
[[ "${1:-}" == "-y" ]] && YES=1

ask() {                                   # ask "question" -> 0 = yes (default)
  local a
  if (( YES )); then echo "$1 [Y/n] y"; return 0; fi
  read -r -p "$1 [Y/n] " a
  [[ ! "$a" =~ ^[Nn] ]]
}
step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
rev()  { sed -n 's/^#define FW_REV "\(.*\)"/\1/p' version.h; }
bin_is_rev() { [[ -f "$BIN" ]] && grep -qaF "Paddle2IP $(rev)" "$BIN"; }
need_bin() {                              # out/ must hold the current revision
  bin_is_rev && return 0
  echo "warning: $BIN is missing or not built from FW_REV $(rev)"
  ask "Continue anyway?"
}

# ---------------- 1. version ----------------
step "1/6 version"
cur=$(rev)
today=$(date +%Y%m%d)
if   [[ "$cur" == "$today" ]];   then new="$today.2"
elif [[ "$cur" == "$today".* ]]; then new="$today.$(( ${cur##*.} + 1 ))"
else                                  new="$today"
fi
if ask "Bump FW_REV $cur -> $new?"; then
  sed -i "s/^#define FW_REV \".*\"/#define FW_REV \"$new\"/" version.h
  echo "FW_REV = $(rev)"
fi
REV=$(rev)

# ---------------- 2. build ----------------
step "2/6 build"
if ask "Build firmware $REV?"; then ./build.sh; fi

# ---------------- 3. upload ----------------
step "3/6 upload"
if ask "Upload to a connected device?" && need_bin; then
  ports=(); descs=()
  for p in /dev/ttyACM* /dev/ttyUSB*; do
    [[ -e "$p" ]] || continue
    d=""
    for l in /dev/serial/by-id/*; do
      [[ -e "$l" && "$(readlink -f "$l")" == "$p" ]] && d="${l##*/}"
    done
    ports+=("$p"); descs+=("${d:-?}")
  done
  port=""
  if (( ${#ports[@]} == 0 )); then
    echo "no serial device found, upload skipped"
  elif (( ${#ports[@]} == 1 )); then
    port=${ports[0]}
  else
    for i in "${!ports[@]}"; do printf '  %d) %-14s %s\n' $((i + 1)) "${ports[i]}" "${descs[i]}"; done
    if (( YES )); then n=1; else read -r -p "Device [1]: " n; n=${n:-1}; fi
    if [[ "$n" =~ ^[0-9]+$ ]] && (( n >= 1 && n <= ${#ports[@]} )); then port=${ports[n - 1]}
    else echo "invalid choice, upload skipped"; fi
  fi
  if [[ -n "$port" ]]; then
    echo "uploading to $port"
    arduino-cli upload -b "$FQBN" -p "$port" --input-dir out .
  fi
fi

# ---------------- 4. commit ----------------
step "4/6 commit"
if [[ -z "$(git status --porcelain)" ]]; then
  echo "nothing to commit"
else
  git status --short
  if ask "Commit all changes?"; then
    msg="Paddle2IP $REV"
    if (( ! YES )); then read -r -p "Message [$msg]: " m; msg=${m:-$msg}; fi
    git add -A
    git commit -m "$msg"
  fi
fi

# ---------------- 5. push ----------------
step "5/6 push"
branch=$(git rev-parse --abbrev-ref HEAD)
if ask "Push $branch to origin?"; then git push -u origin "$branch"; fi

# ---------------- 6. GitHub Pages ----------------
step "6/6 web flasher (gh-pages)"
if ask "Publish the web flasher for $REV to gh-pages and push?" && need_bin; then
  props=$(arduino-cli compile -b "$FQBN" --show-properties .)
  platform=$(sed -n 's/^runtime\.platform\.path=//p' <<<"$props")
  bootAddr=$(( $(sed -n 's/^build\.bootloader_addr=//p' <<<"$props") ))

  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' EXIT
  site=$tmp/site; fw=firmware/$REV
  mkdir -p "$site/$fw"
  cp out/paddle2ip.ino.bootloader.bin  "$site/$fw/bootloader.bin"
  cp out/paddle2ip.ino.partitions.bin  "$site/$fw/partitions.bin"
  cp "$platform/tools/partitions/boot_app0.bin" "$site/$fw/boot_app0.bin"
  cp "$BIN"                             "$site/$fw/paddle2ip.bin"
  touch "$site/.nojekyll"

  commit=$(git rev-parse --short HEAD)
  [[ -n "$(git status --porcelain)" ]] && commit="$commit-dirty"
  sed -e "s/@REV@/$REV/g" -e "s/@DATE@/$(date -u '+%Y-%m-%d %H:%M UTC') from $commit/g" \
      tools/webflash/index.html >"$site/index.html"

  # Same offsets as the arduino-cli upload. The parts are written separately,
  # not as one merged image: NVS (0x9000-0xDFFF) lies between them and keeps
  # the settings unless the user chooses "erase".
  cat >"$site/manifest.json" <<EOF
{
  "name": "Paddle2IP",
  "version": "$REV",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "$fw/bootloader.bin", "offset": $bootAddr },
        { "path": "$fw/partitions.bin", "offset": 32768 },
        { "path": "$fw/boot_app0.bin",  "offset": 57344 },
        { "path": "$fw/paddle2ip.bin",  "offset": 65536 }
      ]
    }
  ]
}
EOF

  # Commit the generated site onto gh-pages without touching the working tree.
  gitdir=$(git rev-parse --absolute-git-dir)
  parent=""
  if git fetch -q origin gh-pages 2>/dev/null; then parent=$(git rev-parse FETCH_HEAD)
  else parent=$(git rev-parse -q --verify refs/heads/gh-pages || true)
  fi
  export GIT_INDEX_FILE=$tmp/index
  git --git-dir="$gitdir" --work-tree="$site" -C "$site" add -A -f .
  tree=$(git --git-dir="$gitdir" write-tree)
  unset GIT_INDEX_FILE
  c=$(git commit-tree "$tree" ${parent:+-p "$parent"} -m "Web flasher $REV")
  git update-ref refs/heads/gh-pages "$c"
  echo "gh-pages = $(git rev-parse --short "$c") (firmware $REV)"
  git push origin gh-pages

  # GitHub Pages must be enabled once (Settings -> Pages, branch gh-pages).
  url=$(git remote get-url origin)
  if [[ "$url" =~ github\.com[/:]([^/]+)/([^/]+)$ ]]; then
    owner=${BASH_REMATCH[1]}; repo=${BASH_REMATCH[2]%.git}
    if command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1 &&
       ! gh api "repos/$owner/$repo/pages" >/dev/null 2>&1 &&
       ask "GitHub Pages is not enabled for $owner/$repo. Enable it (branch gh-pages)?"; then
      gh api -X POST "repos/$owner/$repo/pages" -f "source[branch]=gh-pages" -f "source[path]=/" >/dev/null ||
        echo "warning: enabling Pages failed, do it in Settings -> Pages"
    fi
    echo "web flasher: https://${owner,,}.github.io/$repo/  (Chrome / Edge)"
  fi
fi

echo
echo "done, FW_REV $REV"
