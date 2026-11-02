#!/usr/bin/env python3
"""Name frames that live in a process's MAIN executable (not the shared cache).

Why this exists: scripts/decode-clover-slide.py can resolve shared-cache frames,
because the cache's slide is recoverable from the log itself. A main binary's frames
cannot be named without its load address, which is why r274 asks the kernel to print
the task's first vm_map entries. This tool does the job from the log alone -- and does
it before the board answers:

  1. Every stack frame is a return address, so `frame - slide - 4` must decode as a
     call (bl/blr/b/br). Intersecting [frame-__text_end, frame-__text_start] over all
     frames from one thread leaves only a few page-aligned slides, and that call test
     usually leaves exactly one.
  2. The containing function comes from LC_FUNCTION_STARTS (delta-ULEB), which every
     Apple binary carries even when `nm` reports "no symbols".
  3. The function's identity comes from what it reaches: branch targets in __stubs
     resolve to their import name through the image's bind entries, and adrp/adr sites
     landing in __objc_selrefs / __objc_classrefs / __cfstring name the ObjC call site.

So the output says "this frame is 0x14 into the function at __text+0x9d10, and that
function calls dispatch_semaphore_wait with x1 = -1" -- without a symbol table.

  scripts/clover-name-main-image.py IMAGE --frame 0x101177d50 --frame ... [--base L]
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys

CALLS = {"bl", "blr", "b", "br"}
INS = re.compile(r"^([0-9a-f]+):\s+([0-9a-f]{8})\s+(\S+)(.*)$", re.I)
NUM = re.compile(r"0x([0-9a-f]+)", re.I)
FIXUP = re.compile(r"^\s+(\S+)\s+(\S+)\s+(0x[0-9a-f]+)\s+(bind|rebase|export)\s+(.*)$", re.I)


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True).stdout


def vm_range(image: str):
    """(__TEXT vmaddr, __text start, __text size, __TEXT fileoff) from otool -l."""
    out = run(["otool", "-l", image])
    pref = start = size = fileoff = None
    seg = None
    in_text = False
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("segname "):
            seg = s.split()[1]
        elif seg == "__TEXT" and s.startswith("vmaddr ") and pref is None:
            # the first vmaddr after "segname __TEXT" is __TEXT's own
            pref = int(s.split()[1], 16)
        elif seg == "__TEXT" and s.startswith("fileoff ") and fileoff is None:
            fileoff = int(s.split()[1])
        if s.startswith("sectname "):
            in_text = s.split()[1] == "__text"
        elif in_text:
            if s.startswith("addr "):
                start = int(s.split()[1], 16)
            elif s.startswith("size "):
                size = int(s.split()[1], 16)
                in_text = False
    return pref, start, size, fileoff


def function_starts(image: str, text_lo: int, text_hi: int):
    """Every LC_FUNCTION_STARTS address inside __text.

    dyld_info prints each start as "0xADDR  <nearest preceding name>+N"; the names are
    only offsets from whatever exported symbol happens to precede it (this binary has
    five T symbols), so they are NOT function names. What is trustworthy is the address
    list itself: it is the same boundary set the kernel's profilers use.
    """
    out = run(["dyld_info", "-function_starts", image])
    starts = []
    for line in out.splitlines():
        m = re.search(r"(0x[0-9a-f]+)\s+\S", line, re.I)
        if m:
            a = int(m.group(1), 16)
            if text_lo <= a < text_hi:
                starts.append(a)
    return sorted(starts)


def bind_table(image: str):
    """({addr: (kind, target)}, {selref slot: selector vmaddr}) from dyld_info -fixups."""
    out = run(["dyld_info", "-fixups", image])
    table, selrefs = {}, {}
    for line in out.splitlines():
        m = FIXUP.match(line)
        if not m:
            continue
        sect, addr, kind, target = m.group(2), int(m.group(3), 16), m.group(4), m.group(5).strip()
        table[addr] = (kind, target)
        if sect == "__objc_selrefs":
            selrefs[addr] = target
    return table, selrefs


def disasm(image: str, section: str):
    ins = {}
    for line in run(["objdump", "-d", "-j", section, image]).splitlines():
        m = INS.match(line)
        if m:
            ins[int(m.group(1), 16)] = (m.group(3).lower(), m.group(4))
    return ins


def stub_targets(image: str):
    """{stub address: imported name} by reading each stub's adrp+ldr GOT slot.

    A stub is keyed by the address of its *first* instruction (the adrp), because that
    is the branch target a caller uses; the ldr that follows only names the slot.
    """
    table, out = {}, run(["objdump", "-d", "-j", "__stubs", image])
    stub_at = None
    got_page = None
    for line in out.splitlines():
        m = INS.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        op, rest = m.group(3).lower(), m.group(4)
        if op == "adrp":
            n = NUM.search(rest)
            stub_at, got_page = addr, (int(n.group(1), 16) if n else None)
        elif op == "ldr" and got_page is not None:
            n = re.search(r"\[x16, #0x([0-9a-f]+)\]", rest)
            if n:
                entry = bind_cache.get(got_page + int(n.group(1), 16))
                if entry:
                    table[stub_at] = "%s [%s]" % (entry[1], entry[0])
            stub_at, got_page = None, None
    return table


bind_cache = {}
selref_cache = {}


def read_cstring(image: str, vmaddr: int, pref: int, fileoff: int):
    """Read a NUL-terminated C string at a __TEXT vmaddr (methname/cstring sections)."""
    if pref is None or fileoff is None:
        return None
    with open(image, "rb") as fh:
        data = fh.read()
    off = fileoff + (vmaddr - pref)
    if not 0 <= off < len(data):
        return None
    end = data.find(b"\0", off)
    return data[off:end].decode(errors="replace") if end > 0 else None


def objc_stub_names(image: str, pref: int, fileoff: int):
    """{objc stub address: "objc_msgSend$selector"} from its selref slot and IMP bind.

    An ObjC stub loads two things: the selector into x1 (from __objc_selrefs, which
    rebases to a string in __objc_methname) and the messaging function into x16 (a
    __got bind). Both are needed: the pair is what the linker called a "named stub".
    Stubs are separated by padding, so a stub is delimited by its trailing br x16.
    """
    table = {}
    start = None
    sel = imp = None
    page = None
    for line in run(["objdump", "-d", "-j", "__objc_stubs", image]).splitlines():
        m = INS.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        op, rest = m.group(3).lower(), m.group(4)
        if op == "adrp":
            n = NUM.search(rest)
            page = int(n.group(1), 16) if n else None
            if start is None:
                start = addr
        elif op == "ldr" and page is not None:
            n = re.search(r"\[x(\d+), #0x([0-9a-f]+)\]", rest, re.I)
            if not n:
                continue
            slot = page + int(n.group(2), 16)
            if n.group(1) == "1" and slot in selref_cache:
                target = selref_cache[slot]
                if target:
                    sel = read_cstring(image, int(target, 16), pref, fileoff)
            elif n.group(1) == "16":
                entry = bind_cache.get(slot)
                imp = entry[1] if entry else None
        elif op == "br":
            if start is not None and imp and sel:
                table[start] = "%s$%s" % (imp.lstrip("_"), sel)
            start, sel, imp, page = None, None, None, None
    return table


def name_of(target, text_lo, text_hi, stubs, got, objc_stubs, starts):
    if target in stubs:
        return stubs[target]
    if target in objc_stubs:
        return objc_stubs[target]
    if target in got:
        kind, nm = got[target]
        return "%s [%s]" % (nm, kind)
    if text_lo <= target < text_hi:
        idx = 0
        while idx + 1 < len(starts) and starts[idx + 1] <= target:
            idx += 1
        return "func __text+%#x +%d" % (starts[idx] - text_lo, target - starts[idx]) if starts \
            else "in __text"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--frame", action="append", default=[])
    ap.add_argument("--base", help="known runtime load address; skips the slide search")
    ap.add_argument("--page", type=int, default=4096)
    ap.add_argument("--context", type=int, default=120,
                    help="how many instructions of each function to scan for calls")
    args = ap.parse_args()

    global bind_cache, selref_cache
    pref, text_lo, text_size, text_file = vm_range(args.image)
    if text_lo is None:
        print("no __text", file=sys.stderr)
        return 1
    text_hi = text_lo + text_size
    starts = function_starts(args.image, text_lo, text_hi)
    got, selrefs = bind_table(args.image)
    bind_cache, selref_cache = got, selrefs
    ins = disasm(args.image, "__text")
    stubs = stub_targets(args.image)
    objc_stubs = objc_stub_names(args.image, pref, text_file)
    print("__TEXT vm=%#x __text=%#x..%#x functions=%d binds=%d stubs=%d objc_stubs=%d"
          % (pref or 0, text_lo, text_hi, len(starts), len(got), len(stubs), len(objc_stubs)))

    frames = sorted(set(int(f, 0) for f in args.frame))
    if not frames:
        print("no frames given", file=sys.stderr)
        return 1

    if args.base:
        base = int(args.base, 0)
        slides = [base - pref] if pref else [base - text_lo]
    else:
        lo = max(f - text_hi for f in frames)
        hi = min(f - text_lo for f in frames)
        start = lo if lo % args.page == 0 else lo + (args.page - lo % args.page)
        slides = [s for s in range(start, hi + 1, args.page)
                  if all((f - s - 4) in ins and ins[f - s - 4][0] in CALLS and (f - s) in ins
                         for f in frames)]
        print("slide window %#x..%#x -> %d candidate(s)" % (lo, hi, len(slides)))
        if not slides:
            print("no slide makes every frame follow a call: the frames are not from one "
                  "boot of this binary, or the base is not page-aligned")
            return 1

    for slide in slides[:1]:
        print("\nload address = %#x (slide %#x)" % ((pref or 0) + slide, slide))
        for f in frames:
            a = f - slide
            idx = 0
            while idx + 1 < len(starts) and starts[idx + 1] <= a:
                idx += 1
            fs = starts[idx] if starts else text_lo
            print("\nframe %#x -> vm=%#x  func __text+%#x (%#x) +%d"
                  % (f, a, fs - text_lo, fs, a - fs))
            reach = []
            for addr in sorted(x for x in ins if fs <= x <= a)[-args.context:]:
                op, rest = ins[addr]
                n = NUM.search(rest)
                if not n:
                    continue
                nm = name_of(int(n.group(1), 16), text_lo, text_hi, stubs, got,
                             objc_stubs, starts)
                if op in CALLS:
                    reach.append("%s %#x -> %s" % (op, addr, nm or hex(int(n.group(1), 16))))
                elif nm:
                    reach.append("%s %#x -> %s" % (op, addr, nm))
            for r in reach[-8:]:
                print("      %s" % r)
    return 0


if __name__ == "__main__":
    sys.exit(main())
