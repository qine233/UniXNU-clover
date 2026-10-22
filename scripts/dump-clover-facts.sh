#!/usr/bin/env bash
# Collect the facts UniXNU needs for the Xiaomi Mi Pad 4 8" LTE (clover / xiaomi,clover-lte, SDM660).
# Every value the ADT, board.toml and the UEFI port must carry comes from the device,
# not from a datasheet. Run with the tablet in Android over adb.
#
#   scripts/dump-clover-facts.sh [serial]
#
# Run against the tablet in Android. Plain adb works for most of it; the raw
# image pulls (dtb, dtbo, boot) and /proc/iomem need root, so a Magisk-equipped
# tablet gives a complete dump. brew install dtc on the host to decode the dtb.
#
# Non-root readable items are collected normally; anything that needs root is retried
# through `su -c` and recorded in needs-root.txt when that fails.
set -uo pipefail

SERIAL="${1:-}"
if [ -z "$SERIAL" ]; then
  n=$(adb devices | sed -n '2,$p' | grep -cw device)
  if [ "$n" -ne 1 ]; then
    echo "expected exactly one adb device, found $n. List: $(adb devices | sed -n '2,$p' | tr '\n' ' ')" >&2
    echo "pass the serial explicitly: $0 <serial>" >&2
    exit 1
  fi
  SERIAL=$(adb devices | sed -n '2,$p' | awk '/\tdevice/{print $1}')
fi

OUT="facts/$SERIAL-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT/proc" "$OUT/sys" "$OUT/dt"
S() { adb -s "$SERIAL" shell "$@" 2>/dev/null | tr -d '\r'; }
# capture to file; the second argument is the root-required marker
cap() { # cap <path> <shell command...>
  local dest="$1"; shift
  local out; out=$(S "$@")
  if [ -z "${out//[[:space:]]/}" ]; then
    out=$(S "su -c '$*'")
    if [ -z "${out//[[:space:]]/}" ]; then
      echo "$dest" >> "$OUT/needs-root.txt"
      return 1
    fi
  fi
  printf '%s\n' "$out" > "$OUT/$dest"
}
# decode a devicetree property as 32-bit little-endian words
dtwords() { # dtwords <relative dt path>
  local p="$1" o
  o=$(S "od -An -tx4 '/proc/device-tree/$p'")
  [ -n "${o//[[:space:]]/}" ] || o=$(S "od -An -tx1 '/proc/device-tree/$p'")
  printf '%s\n' "$o" | tr -s ' \n' ' '
}

echo "== clover ($SERIAL) -> $OUT"

# --- identity -------------------------------------------------------------
cap identity.getprop.txt "getprop"
adb -s "$SERIAL" shell getprop | grep -E \
  'ro\.product\.(device|model|name|board)|ro\.build\.(version\.release|fingerprint)|ro\.boot\.hardware|ro\.surfaceflinger|ro\.rif.' \
  > "$OUT/identity.key-props.txt" 2>/dev/null
cap kernel.txt "uname -a"
cap cmdline.txt "cat /proc/cmdline"

# --- CPU and memory -------------------------------------------------------
cap proc/cpuinfo.txt "cat /proc/cpuinfo"
cap proc/meminfo.txt "head -6 /proc/meminfo"
cap cpu/possible.txt "cat /sys/devices/system/cpu/possible"
for c in 0 1 2 3 4 5 6 7; do
  cap "cpu/cpu$c.txt" "cat /sys/devices/system/cpu/cpu$c/{online,cpu_capacity} 2>/dev/null; ls /sys/devices/system/cpu/cpu$c/cpufreq/ 2>/dev/null; cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_available_frequencies 2>/dev/null"
done

# --- device tree: the numbers the ADT generator needs ---------------------
for p in model compatible "#address-cells" "#size-cells" qcom,msm-id qcom,board-id qcom,serial-number; do
  v=$(S "cat '/proc/device-tree/$p' 2>/dev/null | tr '\\0' ' '")
  [ -n "${v//[[:space:]]/}" ] && echo "$p = $v" >> "$OUT/dt/root.strings.txt"
  printf '%-18s %s\n' "$p" "$(dtwords "$p")" >> "$OUT/dt/root.words.txt"
done
echo "-- cpus"
cap dt/cpus.list.txt "ls /proc/device-tree/cpus/"
for c in $(S "ls /proc/device-tree/cpus/ 2>/dev/null" | tr -d '\r'); do
  echo "$c reg: $(dtwords "cpus/$c/reg")  enable-method: $(S "cat /proc/device-tree/cpus/$c/enable-method 2>/dev/null | tr -d '\0'")"
done > "$OUT/dt/cpus.txt"
S "for f in /proc/device-tree/cpus/cpu@*/reg; do echo \$f \$(od -An -tx4 \$f); done" > "$OUT/dt/cpu-reg.raw.txt"
S "cat /proc/device-tree/cpus/timebase-frequency 2>/dev/null | od -An -tx4; ls -d /proc/device-tree/timer* 2>/dev/null; for f in /proc/device-tree/timer*/*; do echo \$f; done" > "$OUT/dt/timer.txt"
echo "-- psci"; cap dt/psci.txt "cat /proc/device-tree/psci/method; cat /proc/device-tree/psci/compatible"
echo "-- interrupt controller"
cap dt/gic.raw.txt "ls -d /proc/device-tree/interrupt-controller*; for f in /proc/device-tree/interrupt-controller*/reg /proc/device-tree/interrupt-controller*/redistributor-stride /proc/device-tree/interrupt-controller*/#redistributor-regions; do echo \$f \$(od -An -tx4 \$f); done"
echo "-- memory banks"
cap dt/memory.raw.txt "ls -d /proc/device-tree/memory*; for f in /proc/device-tree/memory*/reg; do echo \$f \$(od -An -tx4 \$f); done"
echo "-- reserved-memory (panel splash + framebuffer regions)"
cap dt/reserved-memory.raw.txt "for d in /proc/device-tree/reserved-memory/*; do echo \"== \$d\"; cat \$d/label 2>/dev/null | tr -d '\0'; echo; od -An -tx4 \$d/reg 2>/dev/null; done"

# --- display --------------------------------------------------------------
echo "-- display"
cap graphics.txt "for d in /sys/class/graphics/*; do echo == \$d; for f in name modes virtual_size xres yres bits_per_pixel framebuffer; do [ -e \$d/\$f ] && echo \"\$f: \$(cat \$d/\$f 2>/dev/null | tr '\n' ' ')\"; done; done"
cap drm.txt "for d in /sys/class/drm/*; do echo == \$d; cat \$d/modes 2>/dev/null; done"
cap backlight.txt "for d in /sys/class/backlight/*; do echo == \$d; cat \$d/{actual_brightness,max_brightness,brightness} 2>/dev/null; done"
S "ls -d /proc/device-tree/soc/qcom*mdss* /proc/device-tree/soc*/qcom,mdss* 2>/dev/null; for f in /proc/device-tree/soc*/qcom,mdss_dsi_panel_name /proc/device-tree/soc*/panel*; do echo \$f; cat \$f 2>/dev/null | tr -d '\0'; echo; done" > "$OUT/dt/panel.txt"

# --- storage: userdata GPT window, eMMC identity --------------------------
echo "-- storage"
cap proc/partitions.txt "cat /proc/partitions"
cap block.txt "for b in /sys/block/*; do echo == \$b; cat \$b/size \$b/queue/logical_block_size 2>/dev/null; done"
cap byname.txt "ls -l /dev/block/bootdevice/by-name/ /dev/block/platform/*/by-name/ 2>/dev/null"
cap mmc_device.txt "cat /sys/block/mmcblk0/device/{name,cid,csd,manufacturing_date} 2>/dev/null; echo -- ; cat /sys/class/mmc_host/mmc*/mmc*:{name,cid} 2>/dev/null"
cap mmc_partitions.txt "for p in /sys/block/mmcblk0/mmcblk0p*; do echo \"\$p start=\$(cat \$p/start 2>/dev/null) size=\$(cat \$p/size 2>/dev/null)\"; done"
# map the partition names onto the sysfs entries so first_lba/last_lba are computable
S "for l in /dev/block/bootdevice/by-name/* /dev/block/platform/*/by-name/*; do echo \"\$(basename \$l) -> \$(readlink \$l)\"; done" > "$OUT/partition-names.txt"
cap sdhci.txt "ls -d /sys/devices/platform/soc/*/*.mmch /sys/devices/platform/soc/*/*sdhci* 2>/dev/null; for f in /proc/device-tree/soc*/*sdcc*/reg /proc/device-tree/soc*/*sdhci*/reg; do echo \$f \$(od -An -tx4 \$f 2>/dev/null); done"

# --- input (touch, for a later driver) -----------------------------------
echo "-- input"
cap proc/bus-input-devices.txt "cat /proc/bus/input/devices"
cap input.txt "for i in /sys/class/input/*; do echo == \$i; cat \$i/device/name 2>/dev/null; cat \$i/name 2>/dev/null; done"
S "ls /proc/device-tree/soc*/spi*/ /proc/device-tree/soc*/spi*/* 2>/dev/null | head -60" > "$OUT/dt/spi-touch.txt"

# --- iomem: MDP, GPU, GIC, SDHCI physical bases --------------------------
echo "-- iomem"
cap proc/iomem.txt "cat /proc/iomem"
cap dt/soc-reg.txt "for f in /proc/device-tree/soc*/*/reg; do echo \$f \$(od -An -tx4 \$f 2>/dev/null | tr -s '\n' ' '); done"

# --- raw images: the authoritative source for this unit's layout -----------
# The board dts is public (mainline xiaomi,clover-lte plus Xiaomi's clover CAF
# source), but this unit's merged dtb/dtbo, its userdata GPT window and the ABL
# boot-image parameters can only come off the device. These pulls need root.
mkdir -p "$OUT/img"
cap img/has-root.txt "id"
for part in dtb dtbo boot vendor_boot recovery vbmeta; do
  for base in /dev/block/bootdevice/by-name /dev/block/platform/soc//*/by-name; do
    src="$base/$part"
    if [ -n "$(S "ls $src 2>/dev/null")" ]; then
      echo "pulling $src"
      adb -s "$SERIAL" shell "su -c 'dd if=$src of=/sdcard/unixnu-$part.img bs=1M 2>/dev/null || true'" >/dev/null 2>&1
      adb -s "$SERIAL" pull "/sdcard/unixnu-$part.img" "$OUT/img/" >/dev/null 2>&1 && {
        adb -s "$SERIAL" shell "rm /sdcard/unixnu-$part.img" >/dev/null 2>&1
        ls -l "$OUT/img/unixnu-$part.img" >> "$OUT/img/pulled.txt" 2>&1
      } || echo "$part: pull failed" >> "$OUT/img/pulled.txt"
      break
    fi
  done
done
# the userdata window, both in 512-byte sectors (what sysfs reports) and in the
# device's logical block size (what board.toml [userdata] wants for first/last_lba)
{
  link=$(S 'readlink /dev/block/bootdevice/by-name/userdata' | tr -d '\r')
  echo "userdata -> ${link:-not found}"
  part=${link##*/}
  lba_bytes=$(S 'cat /sys/block/mmcblk0/queue/logical_block_size' | tr -d '\r')
  [ -n "${lba_bytes//[!0-9]/}" ] || lba_bytes=512
  echo "lba_bytes = $lba_bytes"
  echo "whole disk: $(S 'cat /sys/block/mmcblk0/size' | tr -d '\r') x 512 bytes"
  if [ -n "$part" ]; then
    start=$(S "cat /sys/block/mmcblk0/$part/start" | tr -d '\r')
    size=$(S "cat /sys/block/mmcblk0/$part/size" | tr -d '\r')
    echo "raw: $part start=$start size=$size (512-byte sectors)"
    if [ -n "${start//[!0-9]/}" ] && [ -n "${size//[!0-9]/}" ]; then
      echo "-- as 512-byte sectors"
      echo "first_lba = 0x$(printf '%x' "$start")"
      echo "last_lba  = 0x$(printf '%x' $((start + size - 1)))"
      if [ "$lba_bytes" != 512 ]; then
        echo "-- as $lba_bytes-byte LBAs (this is what UniXNU's [userdata] expects)"
        echo "first_lba = 0x$(printf '%x' $((start * 512 / lba_bytes)))"
        echo "last_lba  = 0x$(printf '%x' $(((start + size) * 512 / lba_bytes) - 1))"
      fi
    else
      echo "!! could not read start/size for $part; dump the GPT header instead"
    fi
  fi
} > "$OUT/userdata-window.txt"

# --- build-host side checks ----------------------------------------------
echo "-- host toolchain"
{
  echo "xcode-select: $(xcode-select -p 2>&1)"
  echo "xcode license check: $(clang --version 2>&1 | head -1)"
  for t in git python3 clang dtc mkfs.fat mcopy llvm-objcopy fastboot adb; do
    printf '%-14s %s\n' "$t" "$(command -v $t || echo MISSING)"
  done
  echo "brew llvm: $(ls -d /opt/homebrew/opt/llvm 2>/dev/null || echo MISSING)"
} > "$OUT/host-toolchain.txt"

echo
echo "== done: $OUT"
echo "   root still needed for:"; sed 's/^/     /' "$OUT/needs-root.txt" 2>/dev/null || echo "     (nothing)"
echo
echo "Next, with the tablet in fastboot (unlocked), also record:"
echo "  fastboot -s $SERIAL getvar all | tee $OUT/fastboot-getvar-all.txt"
