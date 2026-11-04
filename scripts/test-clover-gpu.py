#!/usr/bin/env python3
"""Build and run the host model of drivers/CloverGpu/GpuCore.h.

Ported in shape from scripts/test-clover-block.py and scripts/test-clover-power.py, which do the same for
the manifest walk and the SPMI arbiter: compile the decode on a Mac against words this repository already
has, and assert the answers, so a wrong one costs a rebuild rather than a boot.

WHAT THIS ROUND IS DEFENDING AGAINST
====================================

The GPU line's whole output is decodings of MMIO words, so its failure mode is not corrupted data -- it is
a reading that means something it does not mean. Round294 produced one of those and this runner exists
because of it: boot 81's thirteen-word LDO census was compared against the vendor's nine-entry
``sdm660_ldo_config[]`` by eye, and the recorded verdict was "not one bit matches". Compared by a function
the arithmetic is 8 comparable words, 1 that agrees (``LDO_LD_DATA_REG`` expects zero and reads zero) and 7
that differ -- and the finding, "nobody has ever run this init list on this board", is supported by
7-of-7 non-zero expectations reading zero rather than by every word being blank, which is false:
PWRSWITCH_CTL and LDO_STATUS1 both read non-zero.

That is the whole argument for a pure layer. An eye-count made the finding look stronger than the numbers
were, and it silently dropped the one table entry whose expected value is zero -- the entry that would
have shown the count was not zero-for-all.

The other arms here exist to show that a guard is load-bearing by turning it off: a page of 0xffffffff
counted as a disagreement, a MEM_ACC field masked before the no-answer test, a duplicated register offset,
and the same-beat ``cx_on`` control set to 0. Each asserts the wrong answer and the right answer beside it.

check-clover.py runs this file and refuses the tree if it does not pass.
"""
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'drivers/CloverGpu/tests/clover_gpu_host.cpp'
OUT = ROOT / 'build/clover-gpu-host'
BIN = OUT / 'clover_gpu_host'


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
        print(f'test-clover-gpu: {SOURCE} is missing', file=sys.stderr)
        return 1
    if compile_harness():
        print('test-clover-gpu: the host harness does not compile', file=sys.stderr)
        return 1
    # Every loop in the layer walks a table of fixed size, so a hang here would mean a table whose
    # sentinel or count does not terminate -- the same class of bug that on the board would read past a
    # register block and hand a later round a census of something else's bytes.
    try:
        run = subprocess.run([str(BIN)], capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        print('test-clover-gpu: the harness hung -- a table walk did not terminate inside its span',
              file=sys.stderr)
        return 1
    (OUT / 'run.log').write_text(run.stdout + run.stderr)
    if run.returncode or 'PASS:' not in run.stdout:
        print(f'test-clover-gpu: failed with exit {run.returncode}\n{run.stdout}{run.stderr}',
              file=sys.stderr)
        return 1
    print(run.stdout.strip())
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
