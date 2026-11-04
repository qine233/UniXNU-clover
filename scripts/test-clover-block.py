#!/usr/bin/env python3
"""Build and run the host model of drivers/CloverBlock/BlockCore.h.

Ported in shape from scripts/test-clover-dsp.py and scripts/test-clover-power.py, which do the same for
the SMEM walker and the SPMI arbiter: compile the decode on a Mac against a span the harness writes
itself, and assert the answers, so the decisions are proven where a wrong one costs a rebuild rather than
a boot.

WHAT THIS ROUND IS DEFENDING AGAINST
====================================

One thing, with two faces. CloverBlock's output is not a report -- it is the LBAs the kernel mounts over.
A manifest row read one stride too far, or a partition-relative offset published without the partition's
first LBA added, produces a filesystem over bytes that belong to something else, and every errno above it
then describes a disk this port never asked for. The other face is round157's: a command that half
succeeded has to say how much it delivered, because r156 counted blocks the card never returned and the
completion lied about data that had not arrived.

Both faces are arithmetic on numbers the card supplied, which makes them exactly the kind of question a
software model can answer. Every refusal the driver panics on has a manifest built here to cause it, and
each asserts both its reason and the number the frame will print beside it -- something a board can only
ever show once per boot, always by dying.

check-clover.py runs this file and refuses the tree if it does not pass.
"""
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'drivers/CloverBlock/tests/clover_block_host.cpp'
OUT = ROOT / 'build/clover-block-host'
BIN = OUT / 'clover_block_host'


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
        print(f'test-clover-block: {SOURCE} is missing', file=sys.stderr)
        return 1
    if compile_harness():
        print('test-clover-block: the host harness does not compile', file=sys.stderr)
        return 1
    # No walk here is bounded by a device that may never answer -- every loop is bounded by the span it
    # was handed -- so a hang would mean a row stride or a count that does not terminate inside the bytes,
    # which is the same bug that on the board would walk off the end of the manifest read.
    try:
        run = subprocess.run([str(BIN)], capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        print('test-clover-block: the harness hung -- a walk did not terminate inside its span',
              file=sys.stderr)
        return 1
    (OUT / 'run.log').write_text(run.stdout + run.stderr)
    if run.returncode or 'PASS:' not in run.stdout:
        print(f'test-clover-block: failed with exit {run.returncode}\n{run.stdout}{run.stderr}',
              file=sys.stderr)
        return 1
    print(run.stdout.strip())
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
