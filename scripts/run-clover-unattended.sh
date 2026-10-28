#!/bin/bash
# Unattended round trip: wait for clover, bring up recovery, read the log window.
#
# The kernel side of this contract (round147) is:
#   * boot arg clover-unattended=<seconds>  -- ArmCpu panics once that much CNTVCT
#     time has passed without the timer interrupt being served; the message starts
#     with "clover r147: unattended deadline".
#   * CloverSdhci maps IMEM's restart-reason cell at boot (0x146bf000 + 0x65c),
#     writes 0x77665501 ("bootloader") and publishes it to the panic screen only
#     after the read-back agreed. Every panic screen therefore marks the cell and
#     resets through PSCI after 20 s, and XBL comes back up in fastboot.
#   * The panic screen still writes its text to its own log window first (kind 20), so
#     what it showed is on the card even though the panel is gone after the reset. Since
#     round185 that window is the partition named logdump, not logfs -- logfs is this
#     boot's root container and nothing of ours is written into it.
#
# This script does the host half, in order:
#   1. wait for the tablet to appear in fastboot (it returns there by itself);
#   2. --flash-cache IMG optionally cold-flashes the payload to the cache partition
#      (a declared eMMC write, off unless asked for). With --boot-after-flash it
#      then reboots normally, so the firmware boots that payload from the partition
#      and the deadline brings the board back to fastboot on its own; without it the
#      card is read as it stands, which is the *previous* boot's log window, not the one
#      just written;
#   3. ask the bootloader for recovery; this may write the boot command into the
#      misc partition (BCB), which is why it needs --allow-misc-bcb;
#   4. wait for TWRP's adb, dump the first 1216 sectors of the log window (logdump)
#      through /dev/block,
#      and decode the 16 report slots plus both rings in it (the driver's verdicts
#      and, since round176, the kernel-log lines the tee copied onto the card);
#   5. optionally return to fastboot.
#
# Nothing here uses `fastboot boot`: every boot of an image is a cold boot from a
# partition. `fastboot reboot recovery` is a bootloader reboot, not a boot of a
# host-supplied image.
#
# Exit codes: 0 records decoded, 1 dump saved but no records, 2 no fastboot, 3
# consent missing, 4 recovery reboot refused, 5 no adb in recovery, 6 the flashed
# payload never came back to fastboot.

set -u

WAIT=900
ADB_WAIT=180
OUT="$HOME/clover-out/clover-log-window.bin"
FLASH_CACHE=""
ALLOW_BCB=0
SKIP_RECOVERY=0
BOOT_AFTER_FLASH=0
REBOOT_BOOTLOADER=0
DECODE=1

say() { printf '[unattended] %s\n' "$*" >&2; }
warn() { printf '[unattended] WARNING: %s\n' "$*" >&2; }
die() { local code=$1; shift; printf '[unattended] %s\n' "$*" >&2; exit "$code"; }

usage() {
  awk 'NR > 1 && /^#/ {sub(/^# ?/, ""); print; next} NR > 1 {exit}' "$0"
  cat <<'EOF'

Options:
  --wait SECONDS         how long to wait for fastboot (default 900)
  --adb-wait SECONDS     how long to wait for recovery adb (default 180)
  --out PATH             dump file (default ~/clover-out/clover-log-window.bin)
  --flash-cache IMG      cold-flash IMG to the cache partition before recovery;
                         this is an eMMC write and is printed before it runs
  --boot-after-flash     after flashing, reboot normally and wait for the board to
                         come back to fastboot by itself, so the log window read in
                         step 4 is the one this payload wrote
  --allow-misc-bcb       consent to the misc-partition BCB write that
                         `fastboot reboot recovery` may perform
  --skip-recovery        the tablet is already in TWRP; just dump and decode
  --reboot-bootloader    after decoding, `adb reboot bootloader`
  --no-decode            keep the raw dump only
  -h, --help             this text
EOF
}

while [ $# -gt 0 ]; do
  case $1 in
    --wait) WAIT=$2; shift 2 ;;
    --adb-wait) ADB_WAIT=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    --flash-cache) FLASH_CACHE=$2; shift 2 ;;
    --boot-after-flash) BOOT_AFTER_FLASH=1; shift ;;
    --allow-misc-bcb) ALLOW_BCB=1; shift ;;
    --skip-recovery) SKIP_RECOVERY=1; shift ;;
    --reboot-bootloader) REBOOT_BOOTLOADER=1; shift ;;
    --no-decode) DECODE=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die 3 "unknown argument: $1" ;;
  esac
done

command -v fastboot >/dev/null || die 2 "fastboot is not on PATH"
command -v adb >/dev/null || die 2 "adb is not on PATH"
mkdir -p "$(dirname "$OUT")" || die 2 "cannot create $(dirname "$OUT")"

fastboot_serial() {
  fastboot devices 2>/dev/null | awk '$2 == "fastboot" {print $1; exit}'
}
adb_serial() {
  # TWRP's adbd reports the state as "recovery", not "device". Round147 through round184 matched only
  # "device" here, so a board sitting in TWRP looked like a board that never booted recovery -- the
  # 180 s wait in the first round184 flash test timed out with recovery already on the USB bus. Both
  # states mean the same thing to this script: there is a shell to dd the log window through.
  adb devices 2>/dev/null | awk 'NR > 1 && ($2 == "device" || $2 == "recovery") {print $1; exit}'
}
wait_fastboot() {
  local deadline=$(( $(date +%s) + WAIT ))
  local found
  say "waiting up to ${WAIT}s for fastboot (the device returns there by itself)"
  while :; do
    found=$(fastboot_serial)
    if [ -n "$found" ]; then
      say "fastboot: $found"
      return 0
    fi
    if [ "$(date +%s)" -ge "$deadline" ]; then
      return 1
    fi
    sleep 2
  done
}
wait_adb() {
  local deadline=$(( $(date +%s) + ADB_WAIT ))
  local found
  say "waiting up to ${ADB_WAIT}s for recovery adb"
  while :; do
    found=$(adb_serial)
    if [ -n "$found" ]; then
      say "adb: $found"
      return 0
    fi
    if [ "$(date +%s)" -ge "$deadline" ]; then
      return 1
    fi
    sleep 2
  done
}

say "contract: clover-unattended=<s> deadline, panic screen marks IMEM 0x146bf65c and resets after 20 s"

# A device already sitting in TWRP is the shortest path: dump from it. This also
# covers a rerun after a failed recovery reboot.
if [ -n "$(adb_serial)" ]; then
  SKIP_RECOVERY=1
  say "an adb device is already up; treating it as recovery"
fi

if [ "$SKIP_RECOVERY" = 0 ] && [ -n "$FLASH_CACHE" ]; then
  [ -f "$FLASH_CACHE" ] || die 2 "no image at $FLASH_CACHE"
  wait_fastboot || die 2 "no fastboot; nothing was flashed"
  say "declared eMMC write: fastboot flash cache $FLASH_CACHE ($(wc -c < "$FLASH_CACHE" | tr -d ' ') bytes into the cache partition only)"
  fastboot flash cache "$FLASH_CACHE" || die 2 "fastboot flash cache failed"
  if [ "$BOOT_AFTER_FLASH" = 1 ]; then
    say "booting the payload just written: fastboot reboot (the firmware enters XNU after its 3 s default)"
    fastboot reboot >/dev/null 2>&1 || die 2 "fastboot reboot failed"
    wait_fastboot || die 6 "the board did not come back to fastboot within ${WAIT}s: either the unattended deadline did not fire or its reset does not reach this host"
    say "the board returned to fastboot by itself, so the deadline fired"
  else
    warn "the card still holds the boot that ran BEFORE this flash; pass --boot-after-flash to read the payload just written"
  fi
fi

if [ "$SKIP_RECOVERY" = 0 ]; then
  if [ -z "$(fastboot_serial)" ]; then
    wait_fastboot || die 2 "no fastboot after ${WAIT}s; power-cycle and hold Vol- to enter fastboot, then rerun with --skip-recovery off"
  fi
  if [ "$ALLOW_BCB" = 0 ]; then
    cat >&2 <<EOF
[unattended] refusing to send 'fastboot reboot recovery' without consent.
[unattended] Declared eMMC write: the bootloader may store the recovery boot
[unattended] command in the misc partition (BCB, about 1 KiB). No other partition
[unattended] is touched. Rerun with --allow-misc-bcb to allow exactly that, or
[unattended] boot TWRP yourself and rerun with --skip-recovery.
EOF
    exit 3
  fi
  say "declared eMMC write: bootloader writes the recovery boot command into misc (BCB)"
  reboot_out=$(fastboot reboot recovery 2>&1)
  reboot_status=$?
  if [ $reboot_status -ne 0 ] || printf '%s' "$reboot_out" | grep -qiE 'unknown|not support|fail|error'; then
    printf '%s\n' "$reboot_out" >&2
    cat >&2 <<EOF
[unattended] the bootloader did not accept 'reboot recovery' (exit $reboot_status).
[unattended] Fallbacks, in order:
[unattended]   1. power-cycle (IMEM is volatile, a power cycle clears the reason),
[unattended]      hold Vol- while powering on to reach fastboot, then rerun this
[unattended]      script with --allow-misc-bcb;
[unattended]   2. boot TWRP from its own partition, then rerun with --skip-recovery.
EOF
    exit 4
  fi
fi

wait_adb || die 5 "no adb in recovery after ${ADB_WAIT}s; TWRP has adbd, so check that recovery actually started (the panel shows its splash)"

twrp=$(adb shell getprop ro.twrp.version 2>/dev/null | tr -d '\r')
device=$(adb shell getprop ro.product.device 2>/dev/null | tr -d '\r')
say "recovery: device=${device:-?} twrp=${twrp:-?}"

# Round176 reads 1216 sectors. The first 32 blocks are the report/panic slots, the driver's verdict
# ring runs 64..191, and round176's kernel-log ring -- the lines the tee copies out of msgbuf, so the
# APFS, code-signing, shared-region and exec/exit readings that used to share 32 records with the
# driver -- runs 192..1215. One dd still brings back everything the card was told, which is the point
# of the round: a boot's whole story, not the last 32 lines of it.
dump_ok=0
# Round185: the window is logdump. logfs stays in the list because a card flashed before
# round185 has its rings -- if any had ever been written -- at the start of logfs, and the
# point of reading that partition is now to show it is empty.
for source in /dev/block/bootdevice/by-name/logdump /dev/block/by-name/logdump \
              /dev/block/mmcblk0p56 /dev/block/bootdevice/by-name/logfs \
              /dev/block/mmcblk0p39; do
  say "reading $source"
  # Two host-side traps, both found by the first real run of this script against a board sitting in
  # TWRP. (1) TWRP's adbd puts dd's own statistics on the same stream as the block data, so a plain
  # `adb exec-out dd` came back 85 bytes long and no length check could be trusted; redirecting dd's
  # stderr inside the shell command keeps the stream at exactly 1216 sectors. (2) This used to require
  # "UXNULOG1" at file offset 0, which is not a property of a logfs window -- it is a property of slot
  # 0 holding a live record. The report window appends (slot k at sector 2k), so the newest record sits
  # wherever the cursor reached, and the card's first sectors can legitimately hold anything. Whether
  # this window carries records is a question for the decoder below, not for a magic number.
  if adb exec-out "dd if=$source bs=512 count=1216 2>/dev/null" > "$OUT" 2>/dev/null; then
    size=$(wc -c < "$OUT" | tr -d ' ')
    if [ "$size" = 622592 ]; then
      dump_ok=1
      break
    fi
    say "  $source: $size bytes, expected 622592"
  else
    say "  $source: read failed"
  fi
done
[ "$dump_ok" = 1 ] || die 1 "no readable log window (last file: $OUT)"

say "dump: $OUT ($(wc -c < "$OUT" | tr -d ' ') bytes)"
if [ "$DECODE" = 0 ]; then
  exit 0
fi

decoder="$(cd "$(dirname "$0")" && pwd)/read-clover-log.py"
{
  echo "== report and panic window"
  python3 "$decoder" "$OUT"
  echo "== verdict ring (driver lines, sectors 64..191)"
  python3 "$decoder" --verdict "$OUT" 2>/dev/null || echo "    no verdict records: this boot predates round174"
  echo "== kernel-log ring (lines the tee copied out of msgbuf, sectors 192..1215)"
  python3 "$decoder" --tee "$OUT" 2>/dev/null || echo "    no kernel-log records: this boot predates round176"
} | tee "$OUT.txt"
count=$(python3 "$decoder" --count "$OUT" 2>/dev/null || echo 0)
verdicts=$(python3 "$decoder" --verdict --count "$OUT" 2>/dev/null || echo 0)
# Round175 counts the kernel's own lines separately, because zero of them means two different things
# and the ring cannot tell them apart on its own: the tee never armed (no verdict lines either), or it
# armed and nothing in msgbuf matched the allowlist this boot.
tee_lines=$(python3 "$decoder" --tee --count "$OUT" 2>/dev/null || echo 0)
# Round176's invariant: the tee drops every line that starts with the driver's name, so the kernel
# ring has exactly one producer in it. A nonzero count here means the tee is copying its own readings
# out of msgbuf -- which it can only do if a line the driver said reached the kernel log at all.
echo_lines=$(python3 "$decoder" --tee --driver --count "$OUT" 2>/dev/null || echo 0)
say "decoded: $OUT.txt ($count report record(s), $verdicts driver verdict line(s), $tee_lines kernel log line(s))"
if [ "$verdicts" -gt 0 ] && [ "$tee_lines" = 0 ]; then
  warn "the ring armed but brought no kernel lines: the tee ran and msgbuf held nothing it keeps"
fi
if [ "$echo_lines" != 0 ]; then
  warn "the kernel-log ring holds $echo_lines driver line(s): the tee is echoing what it also wrote itself"
fi

if [ "$REBOOT_BOOTLOADER" = 1 ]; then
  say "returning to fastboot (adb reboot bootloader)"
  adb reboot bootloader >/dev/null 2>&1 || warn "adb reboot bootloader failed"
fi

[ "$count" -gt 0 ] || [ "$verdicts" -gt 0 ] || [ "$tee_lines" -gt 0 ] || exit 1
exit 0
