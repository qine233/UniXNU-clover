#!/usr/bin/env python3.13
"""Name the function an address from the panic screen belongs to.

Round86 makes the timer's tick panic report ELR_EL1 -- the address the interrupted context was
executing when the interrupt was taken. If that context is the hung one, the address is the hang.
A bare number is not a reading, so this turns it into the three things a reading needs:

  which function it is in,      from the symbol table of the built kernel;
  how far into that function,   and where the next symbol starts, so an offset can be compared
                                across rounds even when the whole kernel has moved;
  what the instructions there   an address inside a backward branch is a spin, and whether that
  are doing,                    loop contains a call or only loads decides what it is spinning on.

Addresses come from /Users/zhouzhou/UniXNU/out/clover/kernel/kernel.macho, the *pre-link* kernel,
not the in-volume copy the board runs. That is deliberate and verified rather than assumed:
scripts/patch-xnu-entry-probe.py writes its probe and its panic screen over existing bytes and
verify-clover-payload.py proves the patch is localized (it counts the patched runs and hashes the
result), so no section moves. Checked on round86: `--kernel ~/clover-out/round86-in-volume-kernel.macho`
gives `_ttwrite` at 0xfffffff00772c9a0 and `_console_write` at 0xfffffff0073b9bd8, the same two
addresses the build product gives. Pass --kernel to name the other file if a round disagrees.

llvm-addr2line gives a name but not a line -- this build carries no DWARF line table, every query
comes back `??:0` -- so the decode rests on the function name plus the instructions, which is why
the window is on by default.

Two tool-chain facts this script exists around, both measured on this machine:
  * `llvm-objdump --macho -d --start-address=...` returns nothing. The Mach-O reader only looks at
    a section literally named `__TEXT,__text`, and this kernel's code is in `__TEXT_EXEC,__text`
    (there are four other `__text` sections: __KLD, __PRELINK_TEXT, and two more from the prelink
    linkmap). Ask for the section explicitly.
  * `--section __TEXT_EXEC,__text --dis-symname _name` works and takes 0.1 s; the same call with
    only --start-address dumps 2.1 million lines in 4 s. So the disassembly is always pulled by
    symbol name, and the window is cut here in python.

Usage:
  decode-clover-address.py 0xfffffff0073e0c30 ...   # one or more addresses, as printed by elr=
  decode-clover-address.py --suspects               # the tty/console ranges for this kernel
  decode-clover-address.py --no-dis 0xADDR          # name only
  decode-clover-address.py --all 0xADDR             # the whole function, not a window
"""

from __future__ import annotations

import argparse
import bisect
import pathlib
import re
import shutil
import subprocess
import sys

KERNEL = pathlib.Path('/Users/zhouzhou/UniXNU/out/clover/kernel/kernel.macho')

# What round86's elr= can mean, grouped the way the three readings separate. Every name here was
# checked against this kernel's symbol table; `ttwait`, `gc_putchar` and `vc_scroll_up` came up
# absent on the first run and were replaced by what the image actually defines (ttwait is inlined,
# the glyph paths are gc_putc_normal/gc_putc_gotpars, the framebuffer copy is video_scroll_up).
SUSPECTS = [
    ('the write path: it is inside the thing under trial', 'ttwrite',
     'write(2) on the console tty'),
    ('the write path: it is inside the thing under trial', 'tputchar',
     'tty putc, called from ttwrite'),
    ('the write path: it is inside the thing under trial', 'ttywait',
     'tty output drain at close'),
    ('the write path: it is inside the thing under trial', 'ttstart',
     'tty start output after the hiwat exit'),
    ('the write path: it is inside the thing under trial', 'ttwakeup',
     'tty output wake, the wakeup that would free a ttywri sleeper'),
    ('the write path: it is inside the thing under trial', 'ttysleep',
     'the only sleep in ttwrite, tty.c:2656 of the ovhiwat exit'),
    ('the write path: it is inside the thing under trial', 'cnwrite',
     'the /dev/console write shim in front of km'),
    ('the write path: it is inside the thing under trial', 'kmwrite',
     'the console line discipline write'),
    ('the write path: it is inside the thing under trial', 'kmoutput',
     'the batch that draws the characters, 80 at a time'),
    ('the write path: it is inside the thing under trial', 'kmputc',
     'one character out of kmoutput'),
    ('the write path: it is inside the thing under trial', 'kmstart',
     'the deferred drain, whose timeout is #if 0 out'),
    ('the write path: it is inside the thing under trial', 'kmtimeout',
     'that disabled timeout callback, if anything still calls it'),
    ('the write path: it is inside the thing under trial', 'console_write',
     'the serial-console ring buffer entry'),
    ('the write path: it is inside the thing under trial', 'console_printbuf_putc',
     'reserve a slot in that ring'),
    ('the write path: it is inside the thing under trial', 'console_printbuf_clear',
     'the flush that a full ring waits for'),
    ('the write path: it is inside the thing under trial', 'console_ring_try_empty',
     'the unbounded `while (nreserved > 0) cpu_pause()`'),
    ('the write path: it is inside the thing under trial', 'console_write_unbuffered',
     'the bypass path, straight to the device'),
    ('the write path: it is inside the thing under trial', 'console_write_char',
     'one character to the console device'),
    ('the write path: it is inside the thing under trial', 'vcputc',
     'video console putc without options'),
    ('the write path: it is inside the thing under trial', 'vcputc_options',
     'video console putc, holds vcputc_lock'),
    ('the write path: it is inside the thing under trial', 'vc_paint_char',
     'the blit of one glyph into the framebuffer'),
    ('the write path: it is inside the thing under trial', 'gc_putc_normal',
     'the glyph renderer, normal console'),
    ('the write path: it is inside the thing under trial', 'gc_putc_gotpars',
     'the glyph renderer, while boot args are up'),
    ('the write path: it is inside the thing under trial', 'gc_scroll_up',
     'the buffered scroll'),
    ('the write path: it is inside the thing under trial', 'video_scroll_up',
     'the framebuffer copy, the slowest thing a full screen can ask for'),
    ('the write path: it is inside the thing under trial', 'initialize_screen',
     'screen reset, once'),
    ('the write path: it is inside the thing under trial', 'vc_clear_screen',
     'screen clear, what a panic screen does before printing'),
    ('idle, so the child is scheduled out rather than hogging the core', 'cpu_idle',
     'WFI between ticks'),
    ('idle, so the child is scheduled out rather than hogging the core', 'machine_idle',
     'the arm64 idle loop'),
    ('asleep in a lock or a sleep queue, not spinning', 'msleep',
     'the generic sleep'),
    ('asleep in a lock or a sleep queue, not spinning', 'tsleep',
     'the timed sleep'),
    ('asleep in a lock or a sleep queue, not spinning', 'lck_mtx_lock',
     'a mutex, the only way to block on write_lock itself'),
    ('asleep in a lock or a sleep queue, not spinning', 'wakeup_one',
     'the wake side, stuck here means something is being woken badly'),
    ('the reporting path itself, which would mean the panic is the hang', 'panic',
     'panic entry'),
    ('the reporting path itself, which would mean the panic is the hang', 'panic_with_options',
     'panic entry with options, reboot(2) RB_PANIC lands here'),
    ('the reporting path itself, which would mean the panic is the hang', 'kprintf',
     'kernel printf, goes through the console under trial'),
    ('the reporting path itself, which would mean the panic is the hang', 'printf',
     'the console write kprintf ends in'),
]

# `fffffff0073e0bfc:\tff 83 01 d1\tsub\tsp, sp, #0x60` -- address, four encodings separated by
# single spaces with no trailing one, then the mnemonic, which has tabs inside it.
INSTRUCTION = re.compile(r'^([0-9a-f]+):\s+([0-9a-f]{2}(?:\s[0-9a-f]{2}){0,7})\t(.*)$')
# A branch back to an address at or below itself is the only way to stay somewhere for 60000 ticks.
BRANCH = re.compile(r'^(b|b\.[a-z]{2}|bl|blr|cbnz|cbz|tbz|tbnz)\b')
TARGET = re.compile(r'0x([0-9a-f]+)')
# Mnemonics that say what a loop is waiting on. WFI waits for an interrupt, an exclusive load is a
# lock retry, a plain load refreshed each pass is a polled flag or a polled device.
PARKS = ('wfi', 'wfe', 'sev', 'sevl')
ATOMIC = ('ldaxr', 'stlxr', 'ldxr', 'stxr', 'ldapr', 'swpb', 'casp', 'casa', 'casal')


def tool(names):
    for name in names:
        path = shutil.which(name)
        if path:
            return path
    raise SystemExit(f'none of {names} is installed')


def symbols(kernel):
    """(addresses, names) for every defined symbol with text or data in the kernel.

    Addresses are kept with duplicates. XNU aliases entries -- `_lck_mtx_lock` shares its address
    with another symbol -- and dropping a collision to keep the list strictly increasing loses the
    name a reading is asked about, which is how the first run of `--suspects` reported
    `lck_mtx_lock` absent from an image that has it.
    """
    nm = tool(['/opt/homebrew/opt/llvm/bin/llvm-nm', 'llvm-nm', 'nm'])
    out = subprocess.run([nm, '--defined-only', '--numeric-sort', str(kernel)],
                         check=True, stdout=subprocess.PIPE).stdout.decode()
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(' ', 2)
        if len(parts) != 3:
            continue
        addr, kind, name = parts
        if kind not in 'sStT dD':
            continue
        try:
            value = int(addr, 16)
        except ValueError:
            continue
        name = name if name.startswith('_') else f'_{name}'
        if addrs and value == addrs[-1] and name == names[-1]:
            continue
        addrs.append(value)
        names.append(name)
    return addrs, names


def locate(addrs, names, address):
    """The symbol at the last address at or below `address`, the offset into it, its address, and
    the next symbol's address, which bounds its size from above."""
    index = bisect.bisect_right(addrs, address) - 1
    if index < 0:
        return None
    base = addrs[index]
    following = bisect.bisect_right(addrs, base)
    end = addrs[following] if following < len(addrs) else None
    return names[index], address - base, base, end


def text_sections(kernel):
    """`segment,section` for every `__text`, outermost first, from otool's load commands.

    objdump's `--section` wants this pair, and the pair is not in objdump's own section listing, so
    the segment names come from `otool -l` instead. Both are on every Xcode install.
    """
    otool = tool(['/usr/bin/otool', 'otool'])
    out = subprocess.run([otool, '-l', str(kernel)], check=True,
                         stdout=subprocess.PIPE).stdout.decode()
    specs, sect, seg, addr, size = [], None, None, None, None
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 2:
            continue
        key, value = parts
        if key == 'sectname':
            sect, seg, addr, size = value, None, None, None
        elif key == 'segname' and sect is not None and seg is None:
            seg = value
        elif key == 'addr' and sect is not None and addr is None:
            addr = int(value, 16)
        elif key == 'size' and sect is not None and addr is not None:
            size = int(value, 16)
            if sect == '__text':
                specs.append((f'{seg},{sect}', addr, size))
            sect, seg, addr, size = None, None, None, None
    return specs


def function(kernel, spec, name):
    """[(address, text)] for one symbol inside one `segment,section`, or [] if it is not there."""
    objdump = tool(['/opt/homebrew/opt/llvm/bin/llvm-objdump', 'llvm-objdump'])
    text = subprocess.run([objdump, '--macho', '-d', '--section', spec, '--dis-symname', name,
                           str(kernel)], check=True,
                          stdout=subprocess.PIPE).stdout.decode()
    body = []
    for line in text.splitlines():
        match = INSTRUCTION.match(line.strip())
        if match:
            body.append((int(match.group(1), 16), ' '.join(match.group(3).split())))
    return body


def window(body, address, before=12, after=12):
    """The instructions around the address, marked, plus what they say about why it is there."""
    index = None
    for position, (where, _) in enumerate(body):
        if where == address:
            index = position
            break
    if index is None:
        # elr= is always an instruction address, but a patched byte or a misaligned sample would
        # land between two; show the instruction below it and let reading() say what it is.
        index = max((p for p, (where, _) in enumerate(body) if where <= address), default=0)
    low, high = max(0, index - before), min(len(body), index + after + 1)
    lines = []
    for position in range(low, high):
        where, text = body[position]
        lines.append(('<== ' if position == index else '     ') + f'0x{where:x}  {text}')
    return lines, reading(body, index)


def branches(body):
    """(from, to, text) for every branch in the function whose target is a resolved address."""
    out = []
    for where, text in body:
        head = text.split(' ', 1)[0]
        if head in ('bl', 'blr') or not BRANCH.match(text):
            continue
        # The last hex token: a bit-test branch prints `tbnz w8, #0x0, 0xTARGET` and the first 0x
        # is the bit number, not the destination.
        found = TARGET.findall(text)
        if found:
            out.append((where, int(found[-1], 16), text))
    return out


def reading(body, index):
    """What the address is doing, from the loop structure rather than from a guess."""
    notes = []
    where, text = body[index]
    head = text.split(' ', 1)[0]
    if head in PARKS:
        notes.append(f'{head}: parked until an event or an interrupt, this is not a spin')
    if head == 'bl':
        notes.append(f'the sample is the call itself ({text}), so the callee never returned;'
                     ' name the callee, not this function')
    loops = [(target, source, text) for source, target, text in branches(body)
             if target <= source and target <= where <= source]
    if loops:
        # The tightest enclosing loop: the highest back-edge target, then the lowest back source.
        target, source, text = max(loops, key=lambda loop: (loop[0], -loop[1]))
        span = [entry for entry in body if target <= entry[0] <= source]
        heads = {entry[1].split(' ', 1)[0] for entry in span}
        calls = sorted({entry[1].split(' ', 1)[1].strip()
                        for entry in span if entry[1].startswith('bl ')})
        waited = sorted(heads & set(ATOMIC))
        parked = sorted(heads & set(PARKS))
        detail = f'{len(span)} instructions, 0x{target:x} .. 0x{source:x} ({text})'
        if parked:
            detail += f'; the body parks on {", ".join(parked)}, so it is a wait loop, not a spin'
        elif calls:
            detail += f'; the body calls {", ".join(calls)}, so it waits on the callee'
        elif waited:
            detail += f'; exclusive access {", ".join(waited)}, a lock retry'
        else:
            detail += ('; no call inside, so it re-reads the same memory every pass'
                       ' -- a polled flag or a polled device register')
        notes.append(f'inside a loop: {detail}')
    if not notes:
        notes.append('straight-line code: an interrupt lands anywhere, so one sample is not a'
                     ' verdict -- the reading is whether this address repeats across rounds')
    return notes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('addresses', nargs='*', help='hex addresses, as printed by elr=')
    parser.add_argument('--kernel', default=str(KERNEL), type=pathlib.Path)
    parser.add_argument('--no-dis', action='store_true', help='name only, skip objdump')
    parser.add_argument('--all', action='store_true', help='print the whole function')
    parser.add_argument('--suspects', action='store_true', help='print the tty/console range table')
    args = parser.parse_args()

    kernel = args.kernel
    if not kernel.is_file():
        raise SystemExit(f'no kernel at {kernel}')
    addrs, names = symbols(kernel)

    if args.suspects:
        print(f'# {kernel}  ({len(addrs)} defined symbols)')
        print('# An elr= inside one of these ranges names that function; the offset between the'
              ' address and the range start separates call sites within it.')
        group = None
        missing = []
        for what, symbol, role in SUSPECTS:
            at = _find(addrs, names, symbol)
            if at is None:
                missing.append(symbol)
                continue
            if what != group:
                group = what
                print(f'\n# {what}')
            following = bisect.bisect_right(addrs, at)
            # The gap to the next defined symbol bounds the function from above; local labels the
            # linker keeps can sit inside a body, so the body may be shorter.
            bound = (f'< 0x{addrs[following] - at:x} B'
                     if following < len(addrs) and addrs[following] > at else 'size unknown')
            print(f'  0x{at:x}  {bound:14s} {symbol:26s} {role}')
        if missing:
            print(f'\n# absent from this image: {", ".join(missing)}')

    wanted = []
    for text in args.addresses:
        try:
            wanted.append(int(text, 16))
        except ValueError:
            raise SystemExit(f'not a hex address: {text}')
    if not wanted:
        if not args.suspects:
            parser.print_usage()
        return 0

    specs = None if args.no_dis else text_sections(kernel)
    for address in wanted:
        found = locate(addrs, names, address)
        if found is None:
            print(f'0x{address:x}: below every defined symbol -- not kernel text')
            continue
        name, offset, base, end = found
        span = f'0x{base:x} .. 0x{end:x} (< 0x{end - base:x} B)' if end else f'0x{base:x} ..'
        print(f'\n0x{address:x}  {name} + 0x{offset:x}   {span}')
        if args.no_dis:
            continue
        body = []
        for spec in specs:
            if spec[1] <= base < spec[1] + spec[2]:
                body = function(kernel, spec[0], name)
                if body:
                    print(f'  from {spec[0]}')
                    break
        if not body and specs:
            for spec in specs:
                body = function(kernel, spec[0], name)
                if body and body[0][0] == base:
                    print(f'  from {spec[0]}')
                    break
        if not body:
            print('  no instructions: the address is in data, not in a __text section')
            continue
        if body[0][0] != base:
            print(f'  caution: {name} is an outlined helper, this image defines it first at'
                  f' 0x{body[0][0]:x}; the window is that copy, not this one')
        if args.all:
            index = next((p for p, (where, _) in enumerate(body) if where == address), 0)
            lines = [('<== ' if position == index else '     ')
                     + f'0x{where:x}  {text}' for position, (where, text) in enumerate(body)]
            notes = reading(body, index)
        else:
            lines, notes = window(body, address)
        for line in lines:
            print(f'  {line}')
        for note in notes:
            print(f'  note: {note}')
    return 0


def _find(addrs, names, symbol):
    wanted = f'_{symbol}'
    for index, name in enumerate(names):
        if name == wanted:
            return addrs[index]
    return None


if __name__ == '__main__':
    sys.exit(main())
