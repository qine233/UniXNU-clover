#!/usr/bin/env python3
"""Negative tests for the #716 needles in scripts/check-clover.py.

Each case breaks one shape the gate now pins, runs the gate, and restores in a finally block: a
negative test that leaves its edit behind is worse than no negative test, because the next gate run
then judges a repository that is not the one under review (round333's own negative script left a
change behind exactly this way). The strings matched here carry the files' real indentation, and a
pattern that misses is an exception rather than a pass -- a mutation that applies to nothing would
otherwise report a green gate as "the needle works".
"""
import subprocess
import sys
import traceback
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SCRIPT = REPO / "scripts/run-clover-unattended.sh"
READER = REPO / "scripts/read-clover-log.py"
LOG = Path.home() / "clover-out/round335b-gate-evidence/negatives.log"

SILENT_AFTER = '''      timeline_close "neither-$(usb_landing_class)" "" ""
      return 2
    fi
    sleep 2'''

CASES = [
    ("N1 classifier removed", SCRIPT,
     "usb_landing_class() {", "usb_landing_class_unused() {"),
    ("N2 bulk node no longer grepped", SCRIPT,
     "grep -qE 'QUSB_BULK|QPS6000|HS-USB.*9008'", "grep -qE 'NOTHING_MATCHES_THIS'"),
    ("N3 after-deadline left classless", SCRIPT, SILENT_AFTER,
     '      timeline_close neither "" ""\n      return 2\n    fi\n    sleep 2'),
    ("N4 verdict re-measures instead of quoting the close line", SCRIPT,
     'closing=$(tail -n 1 "$TIMELINE"', 'closing=$(usb_landing_class'),
    ("N5 reader loses one watch key", READER, '    "wait-adb": {', '    "wait-adb-renamed": {'),
    ("N6 reader stops running its timeline leg", READER, "    timeline_selftest()\n", ""),
]


def run_gate():
    result = subprocess.run([sys.executable, str(REPO / "scripts/check-clover.py")],
                            capture_output=True, text=True)
    return result.returncode, result.stdout + result.stderr


def main():
    LOG.parent.mkdir(parents=True, exist_ok=True)
    lines = []
    problems = 0
    for name, path, old, new in CASES:
        original = path.read_text(encoding="utf-8")
        if old not in original:
            lines.append(f"{name}: NOT TESTED -- {path.name} does not contain {old[:60]!r}")
            problems += 1
            continue
        try:
            path.write_text(original.replace(old, new, 1))
            code, out = run_gate()
            first = next((line for line in out.splitlines()
                          if line.startswith("[check-clover] ")
                          and "notice:" not in line and "clover ok" not in line
                          and "host model: PASS" not in line), "")
            if code == 0:
                problems += 1
                lines.append(f"{name}: GREEN (exit 0) -- the needle does not judge")
            else:
                lines.append(f"{name}: RED (exit {code})\n    {first[:240]}")
        except Exception as error:
            problems += 1
            lines.append(f"{name}: EXCEPTION {error}")
            traceback.print_exc()
        finally:
            path.write_text(original)
        code, out = run_gate()
        if code != 0 or path.read_text(encoding="utf-8") != original:
            problems += 1
            lines.append(f"    RESTORE BROKEN: exit={code}")
        else:
            lines.append("    restored: gate exit=0, file byte-identical")
    LOG.write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    print(f"negatives: {len(CASES)} cases, {problems} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
