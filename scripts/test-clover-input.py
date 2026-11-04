#!/usr/bin/env python3
"""Build and run the host model of drivers/CloverInput/InputCore.h.

Same shape as scripts/test-clover-block.py (round316) and scripts/test-clover-gpu.py (round317): compile
the decode on a Mac against words this repository already has, assert the answers, and let a wrong one cost
a rebuild instead of a boot. check-clover.py's ``# Round318.`` block runs this file and refuses the tree if
it does not pass.

WHAT THIS ROUND IS DEFENDING AGAINST
====================================

The touch line's verdict has been stable for fourteen boots and it is quoted everywhere as "electrically
alive, nothing answers": boot 48's beat row reads ``xfers=4 timeout=0 nack=4 verdict=102`` with
``status=10000c8``. That sentence is produced from ONE field -- ``clover_input_observation[6]``, filled by
``qupRead (kQupI2cStatus) & kI2cStatusErrorMask`` -- and writing the mask down as a function showed the
field cannot carry two of the conditions the area is actually waiting on. The vendor's own bit map
(``notes/sources/sdm660/i2c-msm-v2.h:131-132``) puts QUP_I2C_SDA at BIT(26) and QUP_I2C_SCL at BIT(27);
``I2C_STATUS_ERROR_MASK`` is 0x38000fc, whose highest bit is BIT(25). So the line sensors are erased by the
mask, in every boot, whatever the hardware did -- and the alternative explanation the driver prints itself
("a bus whose SDA/SCL are held low by a chip mid-transfer answers nothing at all") has never once been
testable from that field. The model asserts the erasure and the fix in the same case:
``held & kI2cStatusErrorMask`` keeps only the NACK bit, while decoding the raw word reports both sensors.

Three more things only became visible once the arithmetic was callable:

* The two trees this driver cites disagree about which bits count as an error. MAIN's status mask is
  0x38000fc, the vendor's QUP_MSTR_STTS_ERR_MASK is 0x380003c; the pair between them is exactly
  BIT(6) ``QUP_FAILED`` and BIT(7), and BIT(7) is named in neither source. A word carrying only those two
  is a failure to one tree and nothing to the other, so the layer keeps both masks and says which produced
  an answer, and the residue is printed instead of being silently folded into "error".
* The wire encoding -- the thing a NACK is blamed on before anyone blames power -- matches
  ``i2c-qup-mainline.c:569-613`` byte for byte for both candidates, with the 256-byte length trap refused
  on both sides of the API. That converts "we think the tags are right" into a checkable claim, which is
  what lets notes/input-bringup.md §2 close the door on it.
* The pin index. Before the layer, the window was chosen by ``pin <= 11`` and the offset by
  ``(pin - 66) * stride``; run that expression for pin 12 and it wraps to roughly four tebibytes, past the
  end of a 0x2000 window. Nothing passes such a pin today, and the model keeps the wrapped value as a
  witness while asserting the new guard refuses it.

An earlier round's mistake is kept as a case too: treating this board's 19,267,584 Hz tick count as
nanoseconds compresses ``t=`` by about 51.9x (round170), so the time helpers are asserted against the
board's real frequency and the mis-scaled version is asserted to read as zero.
"""
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'drivers/CloverInput/tests/clover_input_host.cpp'
OUT = ROOT / 'build/clover-input-host'
BIN = OUT / 'clover_input_host'


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
        print(f'test-clover-input: {SOURCE} is missing', file=sys.stderr)
        return 1
    if compile_harness():
        print('test-clover-input: the host harness does not compile', file=sys.stderr)
        return 1
    # Both FIFO cursors advance only when the hardware would have advanced, so a hang here means a cursor
    # whose stop condition depends on a value that never changes -- the same class of bug that on the board
    # turns a bounded poll into a boot that never returns.
    try:
        run = subprocess.run([str(BIN)], capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        print('test-clover-input: the harness hung -- a cursor did not terminate inside its bound',
              file=sys.stderr)
        return 1
    (OUT / 'run.log').write_text(run.stdout + run.stderr)
    if run.returncode or 'PASS:' not in run.stdout:
        print(f'test-clover-input: failed with exit {run.returncode}\n{run.stdout}{run.stderr}',
              file=sys.stderr)
        return 1
    print(run.stdout.strip())
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
