#!/usr/bin/env bash
# Decompile the device tree that actually ships in the clover firmware image, so a reading can be
# cited against a file instead of against a vendor dtsi that this board may have changed.
#
#   scripts/dump-clover-dtb-dts.sh [output-path]
#
# Default output: /tmp/clover-dtb.dts. Notes quote line numbers from this dump, so re-running it
# after any change to ~/mu-clover/Resources/DTBs/clover.dtb invalidates those line numbers -- which
# is the point of printing both sha256 values: a note's reading is only citable while the DTB hash
# still matches. Round312's Wi-Fi line (§1 of notes/wifi-bringup.md) is the reason this script
# exists: the phandle resolution it needed was being done in an untracked /tmp file.
#
# dtc comes from brew (`brew install dtc`); the DTB itself was taken from the device and placed by
# task #18, so this script reads a file the firmware build also reads -- it changes nothing.
set -uo pipefail

DTB="${DTB:-$HOME/mu-clover/Resources/DTBs/clover.dtb}"
OUT="${1:-/tmp/clover-dtb.dts}"

if [ ! -f "$DTB" ]; then
  echo "no device tree at $DTB (set DTB= to point at the one the firmware build uses)" >&2
  exit 1
fi
if ! command -v dtc >/dev/null 2>&1; then
  echo "dtc not on PATH; brew install dtc" >&2
  exit 1
fi

dtc -I dtb -O dts "$DTB" -o "$OUT" 2>/dev/null || {
  echo "dtc failed to decompile $DTB" >&2
  exit 1
}

printf 'DTB    %s\n' "$(shasum -a 256 "$DTB" | cut -d' ' -f1)  $DTB"
printf 'dts    %s\n' "$(shasum -a 256 "$OUT" | cut -d' ' -f1)  $OUT"
printf 'lines  %s\n' "$(wc -l < "$OUT" | tr -d ' ')"
