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
import os
import pathlib
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'drivers/CloverPower/tests/clover_power_host.cpp'
OUT = ROOT / 'build/clover-power-host'
BIN = OUT / 'clover_power_host'

# A busy child that burns one core. Used by the loaded leg below; killed in a finally block, so a
# failing model never leaves spinners behind on the machine that runs the gate.
HOG = 'import time\nend = time.time() + 120\nwhile time.time() < end:\n    pass\n'


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


def run_harness (label: str, hogs: list) -> int:
    """Run the compiled model once, optionally with the host already busy.

    Round331b added the loaded leg. The first version of this file ran the model once on an idle
    machine and treated a red result as a verdict about the arbiter's arithmetic, but the model's
    fake PMIC answers from a second thread, so the read's poll budget and the answer's wake latency
    are two clocks: on a host already running a kernel build the wake landed late enough that the
    poll gave up first, and the gate went red over nothing in the driver. The claim 'every served
    read answers inside kPolls' is now tested on a loaded machine instead of assumed, which is the
    same rule the card-side instruments apply to a wait on hardware.
    """
    started = []
    if hogs:
        for _ in hogs:
            started.append(subprocess.Popen([sys.executable, '-c', HOG]))
        time.sleep(0.4)
    try:
        run = subprocess.run([str(BIN)], capture_output=True, text=True, timeout=240)
    except subprocess.TimeoutExpired:
        print(f'test-clover-power ({label}): the harness hung -- the bounded poll did not end a '
              'dead channel', file=sys.stderr)
        return 1
    finally:
        for proc in started:
            proc.kill()
        for proc in started:
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
    (OUT / f'run-{label.replace(":", "-")}.log').write_text(run.stdout + run.stderr)
    if run.returncode or 'PASS:' not in run.stdout:
        print(f'test-clover-power ({label}): failed with exit {run.returncode}\n'
              f'{run.stdout}{run.stderr}', file=sys.stderr)
        return 1
    print(f'test-clover-power ({label}): {run.stdout.strip()}')
    return 0


def main() -> int:
    if not SOURCE.is_file():
        print(f'test-clover-power: {SOURCE} is missing', file=sys.stderr)
        return 1
    if compile_harness():
        print('test-clover-power: the host harness does not compile', file=sys.stderr)
        return 1
    cores = os.cpu_count() or 4
    for label, hogs in (('idle', []), (f'loaded:{cores}', [None] * cores)):
        if run_harness(label, hogs):
            return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
