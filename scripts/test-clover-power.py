#!/usr/bin/env python3
"""Build and run the host model of drivers/CloverPower/PowerArb.h.

Ported in shape from UniXNU-sheng, which keeps its arbiter logic in package/BootResetCore.h and
drives it from package/tests/boot_reset_host.c on a Mac against a software arbiter with a device
thread serving the channels. The reason for copying the split rather than trusting a compile is the
one sheng wrote down: the model catches the ordering questions -- which APID wins when two map to one
PPID, what a denied transaction looks like, in what byte order a five-byte burst comes back -- on a
machine where the wrong answer costs nothing, before they are asked of a PMIC where the wrong answer
costs a report that is confidently wrong.

This round is read-only, so the failure mode it is defending against is not a dead rail but a dead
end: a phantom APID or a byte-swapped revid would send the next round after something that never
existed. check-clover.py runs this file and refuses the tree if it does not pass.
"""
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'drivers/CloverPower/tests/clover_power_host.cpp'
OUT = ROOT / 'build/clover-power-host'
BIN = OUT / 'clover_power_host'


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
        print(f'test-clover-power: {SOURCE} is missing', file=sys.stderr)
        return 1
    if compile_harness():
        print('test-clover-power: the host harness does not compile', file=sys.stderr)
        return 1
    # The timeout is the assertion: the poll bound has to end a transaction nobody answers, so a hung
    # run is a failure of that bound rather than of the machine.
    try:
        run = subprocess.run([str(BIN)], capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        print('test-clover-power: the harness hung -- the bounded poll did not end a dead channel',
              file=sys.stderr)
        return 1
    (OUT / 'run.log').write_text(run.stdout + run.stderr)
    if run.returncode or 'PASS:' not in run.stdout:
        print(f'test-clover-power: failed with exit {run.returncode}\n{run.stdout}{run.stderr}',
              file=sys.stderr)
        return 1
    print(run.stdout.strip())
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
