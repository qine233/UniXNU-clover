#!/bin/bash
# Unattended round trip: wait for clover, bring up recovery, read the log window.
#
# The kernel side of this contract (round147) is:
#   * boot arg clover-unattended=<seconds>  -- ArmCpu panics once that much CNTVCT
#     time has passed without the timer interrupt being served; the message starts
#     with "clover r147: unattended deadline".
#   * CloverSdhci maps IMEM's restart-reason cell at boot (0x146bf000 + 0x65c),
#     writes 0x77665501 ("bootloader") and publishes it to the panic screen only
#     after the read-back agreed. Round195 proved this XBL does not consult that
#     word: the panic left fb=1 in SRAM and the board came up in Android. Since
#     round196 the panic path also writes a boot-recovery BCB into misc, which is
#     what this bootloader does read, so a panic while the unattended contract is
#     armed lands in TWRP. The panic screen still writes its text to its own log
#     window first (kind 20), so what it showed is on the card even though the
#     panel is gone after the reset. Since round185 that window is the partition
#     named logdump, not logfs -- logfs is this boot's root container and nothing
#     of ours is written into it.
#
# This script does the host half, in order:
#   1. wait for the tablet to appear in fastboot (it returns there by itself);
#   2. --flash-cache IMG optionally cold-flashes the payload to the cache partition
#      (a declared eMMC write, off unless asked for). With --boot-after-flash it
#      then reboots normally and watches for the landing the deadline produces.
#      Round196 is why that landing is two-way: the panic path now writes its own
#      recovery BCB into misc, so the reset lands in TWRP with nothing from the
#      host. Round195 measured the older shape -- fb=1 in IMEM's restart-reason
#      cell, and the board still came up in Android -- so an arrival in fastboot
#      is no longer read as "the deadline fired and its intent was honored": the
#      script says the BCB did not take and writes the host's own BCB instead.
#      Without --boot-after-flash the card is read as it stands, which is the
#      *previous* boot's log window, not the one just written;
#   3. write the recovery boot command into the misc partition (Android BCB) and
#      reboot. This bootloader has no working recovery reboot of its own -- measured
#      on round193: `fastboot reboot recovery` is accepted and performed as an
#      ordinary reboot, `fastboot oem reboot-recovery` is `unknown command`, and
#      `fastboot oem help` does not exist -- so the BCB is the only route that was
#      seen to land in TWRP (14 s, twice). It is an eMMC write to misc alone, which
#      is why it needs --allow-misc-bcb. The landing is still proved by TWRP's adb,
#      and a board that comes back to fastboot instead is reported, never retried;
#   4. wait for TWRP's adb, dump the first 1217 sectors of the log window (logdump)
#      through /dev/block,
#      and decode the 16 report slots plus both rings in it (the driver's verdicts
#      and, since round176, the kernel-log lines the tee copied onto the card);
#   5. optionally return to fastboot.
#
# Nothing here uses `fastboot boot`: every boot of an image is a cold boot from a
# partition. The BCB write is not a boot of a host-supplied image either; it only
# tells the bootloader which partition to boot next.
#
# Exit codes: 0 records decoded, 1 dump saved but no records, 2 no fastboot, 3
# consent missing, 4 the board did not land in recovery, 5 no adb in recovery, 6 the
# board never left fastboot when asked to reboot.

set -u

WAIT=900
ADB_WAIT=180
LANDING_WAIT=420
# Round202. The payload's deadline is 900 s of its own uptime, not 900 s of the host's
# wall clock: after the reboot it still has to reach that instant, sit on the panic screen
# for its reset delay, boot the bootloader and start TWRP's adb. Round202's frame reached
# SpringBoard at 358 s and the script's plain WAIT window expired before the landing, so
# the deadline's own reset was read as "nothing came back". The slack covers exactly those
# three steps and nothing else.
DEADLINE_SLACK=120
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
  --wait SECONDS         how long to wait for fastboot at the start (default 900;
                         after --boot-after-flash the same budget is spent waiting
                         for the deadline's landing, plus a fixed 120 s for the
                         payload's reset delay and TWRP's adb coming up)
  --adb-wait SECONDS     how long to wait for recovery adb (default 180)
  --landing-wait SECONDS how long to watch for either TWRP's adb or the board
                         returning to fastboot after a recovery reboot (default 420:
                         a bootloader that quietly turned it into an ordinary reboot
                         only shows that once the payload's own deadline resets)
  --out PATH             dump file (default ~/clover-out/clover-log-window.bin)
  --flash-cache IMG      cold-flash IMG to the cache partition before recovery;
                         this is an eMMC write and is printed before it runs
  --boot-after-flash     after flashing, reboot normally and watch for the deadline's
                         landing: TWRP's adb (the panic path wrote its own BCB into
                         misc) or fastboot (it did not, and this script then writes
                         the BCB itself so the window is still read)
  --allow-misc-bcb       consent to flashing a 2048-byte Android BCB
                         (command=boot-recovery) into the misc partition so the
                         bootloader boots recovery next; no other partition is
                         written
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
    --landing-wait) LANDING_WAIT=$2; shift 2 ;;
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
# A board that was just told to reboot is still enumerated by `fastboot devices` for a
# moment, and wait_fastboot() answers on its first poll. So "it came back by itself"
# has to start from a board that was seen to leave.
wait_leave_fastboot() {
  local deadline=$(( $(date +%s) + 60 ))
  while :; do
    [ -z "$(fastboot_serial)" ] && return 0
    [ "$(date +%s)" -ge "$deadline" ] && return 1
    sleep 1
  done
}
# After a reboot command has been accepted and the board has left fastboot, what comes
# up decides what the command meant: TWRP's adb (0), fastboot again (1, i.e. an ordinary
# reboot), or nothing at all within LANDING_WAIT (2).
watch_landing() {
  local deadline=$(( $(date +%s) + LANDING_WAIT ))
  while [ "$(date +%s)" -lt "$deadline" ]; do
    [ -n "$(adb_serial)" ] && return 0
    [ -n "$(fastboot_serial)" ] && return 1
    sleep 2
  done
  return 2
}
# Round196. After --boot-after-flash the deadline fires inside the payload's own boot, so
# the landing arrives WAIT seconds after the reboot rather than LANDING_WAIT -- plus the
# reset delay and the bootloader's own path into TWRP, which is what round202's missing
# landing measured. This reports which of the two landings happened (0 TWRP's adb, 1
# fastboot, 2 neither).
watch_after_deadline() {
  local deadline=$(( $(date +%s) + WAIT + DEADLINE_SLACK ))
  local found
  while :; do
    found=$(adb_serial)
    if [ -n "$found" ]; then
      return 0
    fi
    found=$(fastboot_serial)
    if [ -n "$found" ]; then
      return 1
    fi
    if [ "$(date +%s)" -ge "$deadline" ]; then
      return 2
    fi
    sleep 2
  done
}
# Round193 flash test. This step used to be one `fastboot reboot recovery` whose output
# was grepped for "unknown": a reboot word this bootloader does not know is not reported
# there, it performs an ordinary reboot -- the host sees exit 0 and the board leaves
# fastboot either way, so the step whose only purpose is to land in TWRP booted XNU and
# the window read afterwards belonged to another boot. The vocabulary was then measured
# on the board: `fastboot reboot recovery` is a plain reboot, `oem reboot-recovery` and
# `oem help` are unknown commands. What does land in TWRP, in 14 s and reproducibly, is
# the Android bootloader message the firmware's own recovery path reads.
#
# struct bootloader_message is 2048 bytes; only `command` (32 bytes) is used here. The
# rest stays zero, which is what the two live tests flashed.
BCB_COMMAND="boot-recovery"
BCB_SIZE=2048
make_bcb() {
  local path=$1
  { printf '%s' "$BCB_COMMAND"; head -c $(( BCB_SIZE - ${#BCB_COMMAND} )) /dev/zero; } > "$path" \
    || return 1
  [ "$(wc -c < "$path" | tr -d ' ')" = "$BCB_SIZE" ]
}

say "contract: clover-unattended=<s> deadline, panic writes a boot-recovery BCB into misc and resets after 20 s"

# A device already sitting in TWRP is the shortest path: dump from it. This also
# covers a rerun after a failed recovery entry.
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
    wait_leave_fastboot || die 6 "the board never left fastboot after 'fastboot reboot': it is still the same boot, so nothing was read"
    say "the board left fastboot; watching up to ${WAIT}s for the deadline's landing (TWRP's adb or fastboot)"
    watch_after_deadline
    landing=$?
    case $landing in
      0)
        say "the panic path's own BCB took: TWRP adb $(adb_serial) after the deadline reset"
        SKIP_RECOVERY=1
        ;;
      1)
        say "the board came back to fastboot: its reset ran, but misc held no recovery command"
        say "writing the host's BCB next so the window is still read from TWRP"
        ;;
      *)
        die 6 "neither TWRP's adb nor fastboot within $(( WAIT + DEADLINE_SLACK ))s: the deadline did not fire, the panic screen never reset, or the reset does not reach this host"
        ;;
    esac
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
[unattended] refusing to write the recovery boot command without consent.
[unattended] Declared eMMC write: a 2048-byte Android BCB with command
[unattended] "$BCB_COMMAND" is flashed into the misc partition, and nothing else.
[unattended] The bootloader reads it on the next boot and boots recovery instead of
[unattended] the payload. Rerun with --allow-misc-bcb to allow exactly that, or boot
[unattended] TWRP yourself and rerun with --skip-recovery.
EOF
    exit 3
  fi
  BCB="$(dirname "$OUT")/clover-bcb-recovery.img"
  make_bcb "$BCB" || die 3 "could not write a ${BCB_SIZE}-byte BCB image at $BCB"
  say "declared eMMC write: fastboot flash misc $BCB (${BCB_SIZE} bytes, command='$BCB_COMMAND', the misc partition only)"
  fastboot flash misc "$BCB" || die 2 "fastboot flash misc failed: the misc partition may now hold a truncated BCB"
  say "rebooting into the recovery the BCB names: fastboot reboot"
  fastboot reboot >/dev/null 2>&1 || die 2 "fastboot reboot failed"
  wait_leave_fastboot || die 6 "the board never left fastboot after 'fastboot reboot': it is still the same boot, so nothing was read"
  say "the board left fastboot; watching up to ${LANDING_WAIT}s for TWRP's adb"
  watch_landing
  landing=$?
  case $landing in
    0) say "landed in recovery: adb $(adb_serial)" ;;
    1)
      cat >&2 <<EOF
[unattended] the BCB did not take: the board came back to fastboot and TWRP's adb
[unattended] never appeared, so the payload booted and its own deadline reset it.
[unattended] Nothing was read. Fallbacks, in order:
[unattended]   1. power-cycle (IMEM is volatile, a power cycle clears the reason),
[unattended]      hold Vol- while powering on to reach fastboot, then rerun this
[unattended]      script with --allow-misc-bcb;
[unattended]   2. boot TWRP from its own partition, then rerun with --skip-recovery.
EOF
      exit 4 ;;
    *) say "neither TWRP's adb nor fastboot within ${LANDING_WAIT}s; the board may be sitting on the panic screen, which resets after 20 s" ;;
  esac
fi

wait_adb || die 5 "no adb in recovery after ${ADB_WAIT}s; TWRP has adbd, so check that recovery actually started (the panel shows its splash)"

twrp=$(adb shell getprop ro.twrp.version 2>/dev/null | tr -d '\r')
device=$(adb shell getprop ro.product.device 2>/dev/null | tr -d '\r')
say "recovery: device=${device:-?} twrp=${twrp:-?}"

# Round176 reads 1216 sectors. The first 32 blocks are the report/panic slots, the driver's verdict
# ring runs 64..191, and round176's kernel-log ring -- the lines the tee copies out of msgbuf, so the
# APFS, code-signing, shared-region and exec/exit readings that used to share 32 records with the
# driver -- runs 192..1215. Round212 adds one more block at 1216: the boot counter the driver reads
# back and increments, which is what tells this boot's records from the last boot's. One dd still
# brings back everything the card was told, which is the point of the round: a boot's whole story,
# not the last 32 lines of it.
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
  # stderr inside the shell command keeps the stream at exactly 1217 sectors. (2) This used to require
  # "UXNULOG1" at file offset 0, which is not a property of a logfs window -- it is a property of slot
  # 0 holding a live record. The report window appends (slot k at sector 2k), so the newest record sits
  # wherever the cursor reached, and the card's first sectors can legitimately hold anything. Whether
  # this window carries records is a question for the decoder below, not for a magic number.
  if adb exec-out "dd if=$source bs=512 count=1217 2>/dev/null" > "$OUT" 2>/dev/null; then
    size=$(wc -c < "$OUT" | tr -d ' ')
    if [ "$size" = 623104 ]; then
      dump_ok=1
      break
    fi
    say "  $source: $size bytes, expected 623104"
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
  echo "== verdict ring (driver lines and relayed probe lines, sectors 64..191)"
  python3 "$decoder" --verdict "$OUT" 2>/dev/null || echo "    no verdict records: this boot predates round174"
  echo "== probe lines (round212 relay from the kernel, kind 25)"
  python3 "$decoder" --probe "$OUT" 2>/dev/null || echo "    no probe lines: this boot predates round212 or no kill was issued"
  echo "== kernel-log ring (lines the tee copied out of msgbuf, sectors 192..1215)"
  python3 "$decoder" --tee "$OUT" 2>/dev/null || echo "    no kernel-log records: this boot predates round176"
  echo "== boot counter (round212, sector 1216)"
  python3 "$decoder" --boot "$OUT" 2>/dev/null || echo "    no boot record: this boot predates round212"
} | tee "$OUT.txt"
# The decoder exits 1 when it counts zero records (an empty ring), so "|| echo 0" would
# capture "0\n0" whenever a ring is empty and every numeric test below would fail on it --
# found by the round193 leg test, whose kernel ring was full and whose driver filter was
# correctly empty.
count_records() {
  local n
  n=$(python3 "$decoder" "$@" 2>/dev/null || true)
  printf '%s' "${n:-0}"
}
count=$(count_records --count "$OUT")
verdicts=$(count_records --verdict --count "$OUT")
# Round175 counts the kernel's own lines separately, because zero of them means two different things
# and the ring cannot tell them apart on its own: the tee never armed (no verdict lines either), or it
# armed and nothing in msgbuf matched the allowlist this boot.
tee_lines=$(count_records --tee --count "$OUT")
# Round212 counts the relayed probe lines separately: they are the only kernel readings that survive
# a long boot in the driver's own ring, so a nonzero count here is the kill-point evidence itself.
probe_lines=$(count_records --probe --count "$OUT")
boot_id=$(python3 "$decoder" --boot "$OUT" 2>/dev/null | sed -n 's/^boot \([0-9][0-9]*\).*/\1/p' | head -1)
# Round176's invariant: the tee drops every line that starts with the driver's name, so the kernel
# ring has exactly one producer in it. A nonzero count here means the tee is copying its own readings
# out of msgbuf -- which it can only do if a line the driver said reached the kernel log at all.
echo_lines=$(count_records --tee --driver --count "$OUT")
say "decoded: $OUT.txt ($count report record(s), $verdicts driver verdict line(s), $probe_lines probe line(s), $tee_lines kernel log line(s), boot ${boot_id:-?})"
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
