#!/usr/bin/env python3
"""Build and run the host model of drivers/CloverDsp/DspCore.h.

Ported in shape from scripts/test-clover-power.py, which does the same thing for the SPMI arbiter
header: compile the decode logic on a Mac against a synthesized buffer and assert the answers, so
that the bounds decisions are proven before they are asked of a heap written by a processor this
port does not control. The reason the SMEM walk needs that treatment more than the arbiter did is
that every offset this parser follows comes out of the heap itself -- the partition table's row
offset, an item's size, a cached list's canary -- and one unchecked field is the difference between
a report and a scan of another processor's memory.

WHAT THIS ROUND IS DEFENDING AGAINST
====================================

Three failure modes, none of which a board run can distinguish after the fact.

* The magic-word form. VEND's structs declare `u8 magic[4]` and memcmp it, while any aligned reader
  of the same four bytes compares one 32-bit word. Get that conversion backwards and a live heap
  with a perfectly good partition table reports "no partition table" -- which is exactly the reading
  that would send the next round into loading the ADSP when it was already up. The model writes the
  ASCII bytes and the assertions read them back as words, so the constants in DspRegs.h have to agree
  with the encoding rather than with the vendor's spelling.
* The bound. Every refusal (DS_E_RANGE, DS_E_CANARY, DS_E_PART) exists because some field said
  "there are 0x1800 bytes here" inside a 128 KiB region. Each of those is built as its own case with
  its own arithmetic, because the driver on the board cannot tell a truncated table from a hostile
  one and the report must.
* The pairing. This board's shipped DTB names no SMEM items at all, so an edge is only an edge when
  "$SMP", version 1, sixteen entries and both pid fields agree. The model therefore has an item
  wearing the wrong pids, an item of the wrong version and an item of the wrong size, and asserts
  that each is refused for its own reason rather than lumped into "not found".

WHY THE READER IS A PLAIN BYTE SPAN
===================================

Because that is what makes it testable here. DspCore.h takes a `volatile uint8_t *` and a size and
never calls IOKit; CloverDsp.cpp owns the mapping and the log lines. The harness synthesizes a heap
in a `uint8_t` array, which is the same split sheng uses for its own SMEM walker (package/QcomSmem.c
is a walker over a byte pointer with no IOKit in it, and it is the implementation the offsets in this
header were checked against).

check-clover.py runs this file and refuses the tree if it does not pass.
"""
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'drivers/CloverDsp/tests/clover_dsp_host.cpp'
OUT = ROOT / 'build/clover-dsp-host'
BIN = OUT / 'clover_dsp_host'


def compile_harness () -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which('clang++') or shutil.which('c++') or 'xcrun'
    command = ([compiler] if compiler != 'xcrun' else ['xcrun', 'clang++']) + [
        '-std=gnu++17', '-O1', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-parameter', '-fno-omit-frame-pointer', '-o', str(BIN), str(SOURCE)]
    result = subprocess.run(command, capture_output=True, text=True)
    (OUT / 'compile.log').write_text(result.stdout + result.stderr)
    if result.returncode:
        print(result.stdout + result.stderr, file=sys.stderr)
    return result.returncode


def main() -> int:
    if not SOURCE.is_file():
        print(f'test-clover-dsp: {SOURCE} is missing', file=sys.stderr)
        return 1
    if compile_harness():
        print('test-clover-dsp: the host harness does not compile', file=sys.stderr)
        return 1
    # There is no poll loop anywhere in DspCore.h -- every walk is bounded by the span it was given --
    # so a timeout here is not a tuning question the way it is for the arbiter. A hung run means a
    # loop whose terminating condition depends on data the model wrote wrong, which is the same bug
    # that on the board would spin inside somebody else's heap.
    try:
        run = subprocess.run([str(BIN)], capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        print('test-clover-dsp: the harness hung -- a walk did not terminate inside its span',
              file=sys.stderr)
        return 1
    (OUT / 'run.log').write_text(run.stdout + run.stderr)
    if run.returncode or 'PASS:' not in run.stdout:
        print(f'test-clover-dsp: failed with exit {run.returncode}\n{run.stdout}{run.stderr}',
              file=sys.stderr)
        return 1
    print(run.stdout.strip())
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
