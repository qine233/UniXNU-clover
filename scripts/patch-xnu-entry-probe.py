#!/usr/bin/env python3
"""Put boot-progress probes into a COPY of the clover XNU kernel (never the original).

There is no serial console on this tablet and the kernel cannot draw before its
own video console exists, so the early boot stages are reported by side
channels. The stage code lives in the zero padding after the kernel entry
(_start is one `b`) and displaced instructions are re-executed there:

  stage 1 (kernel entry, MMU off)       blue bar, then continues.
  stage 2 (just before the MMU is on)   magenta bar, then maps the frame buffer
                                        identity in TTBR0 (the V=P bootstrap
                                        map only covers DarwinBoot's window,
                                        which cannot reach the frame buffer),
                                        and seeds the kernel's vc_info
                                        (_vinfo) with the frame buffer geometry
                                        and a marker in v_type, so both the
                                        panic screen and the stage 3 paint have
                                        a console before vc_init exists. The
                                        mapper lives in its own cave and reuses
                                        the window's L2 table when the frame
                                        buffer shares its 1 GiB slot, otherwise
                                        it builds one from the window's last
                                        page. It finds the bootstrap map from
                                        its own PC, because boot_args.physBase
                                        is anchored on __PRELINK_TEXT and is
                                        not itself mapped. It returns a code in
                                        x8; on failure a WHITE band at index
                                        5+code names the check that failed, so a
                                        silent "no yellow" is not possible.
                                        Immediately after the SCTLR write it
                                        paints band 4 CYAN through that
                                        mapping: the MMU is on and the frame
                                        buffer translates.
  stage 3 (right after the MMU is on)   waits --reset-delay seconds, then PSCI
                                        SYSTEM_RESET. A reboot by itself after
                                        that time shows the CPU got through the
                                        MMU enable; nothing draws in that window,
                                        so time is the signal.
  panic screen (--panic-screen)         panic() and its common panic funnel draw
                                        their format string on the kernel's own
                                        frame buffer mapping and stops
                                        (scripts/xnu-panic-screen.c).
                                        --no-reset leaves out stages 3 and 4.
  stage 4 (--reset-after SYMBOL)        the same wait and reset, but placed after
                                        the call to SYMBOL in --in FUNCTION (the
                                        Nth call, --occurrence). It replaces
                                        stage 3, so one run asks one question:
                                        did the kernel get past that call?
  stage 3 (--paint-after SYMBOL)        paint one bar when the call to SYMBOL is
                                        reached and keep booting, from the seeded
                                        vc_info. The store is guarded by an AT
                                        S1E1W query, so an unmapped frame buffer
                                        skips the bar instead of faulting; this
                                        asks whether the frame buffer is still
                                        reachable once the MMU is on.
  stage 4 (--paint2-after SYMBOL)       a second, independent bar: it paints from
                                        the boot_args pointer in x0 and uses no
                                        stack, no AT and no vc_info. It defaults
                                        to _arm_init's first call, so arm_init
                                        being entered is visible separately from
                                        the vc_info painter working. Both bars
                                        are on by default; --no-paint/--no-paint2
                                        turn them off.

The bars are the 1/16th bands of the frame buffer below the green one that
DarwinBoot paints (band index = stage). Each patch site is checked against its
expected original instruction first, and the tool refuses a kernel that does
not match.

Unlike the sheng tool this port does not shift fixed virtual addresses: the
clover kernel keeps its symbol table, so the entry, _start_first_cpu, _panic,
_vinfo and the panic counter symbols are read from the kernel itself. Only the
SCTLR/ISB/CPACR sequence inside the entry body is found by pattern, because the
bootstrap table-capacity checks before it may change size between builds.

Everything is board-specific by construction: patches the device repository's
board.toml asks for, onto the kernel that repository builds.
"""
import argparse
import os
import struct
import subprocess
import shutil
import sys
import tempfile
from pathlib import Path

CAVE_OFFSETS = {1: 0x78, 2: 0x178, 3: 0x378, 4: 0x478}
CAVE_SIZE = 0x100
# The frame buffer mapper is too large for the stage 2 cave and is called there.
HELPER_OFFSET = 0x600


def cave_limits():
    """Bytes available to each stage between its cave and the next one."""
    ordered = sorted(CAVE_OFFSETS.items(), key=lambda item: item[1])
    return {stage: (ordered[index + 1][1] if index + 1 < len(ordered) else offset + CAVE_SIZE) - offset
            for index, (stage, offset) in enumerate(ordered)}


# The stage 2 cave installs page-table entries, so it gets the gap up to
# stage 3 (0x200 bytes); the others keep one 0x100 slot each.
CAVE_LIMITS = cave_limits()
TIMEBASE = 19200000
# Written into vinfo.v_type by the seed below; the panic screen cleans the data
# cache only for a frame buffer it knows is Normal-WB (see xnu-panic-screen.c).
VINFO_SEED = 0xC10C10C1


def paint(args, lo, hi, band):
    """Fill one 1/16th band of boot_args.Video; `band` is a number or an instruction that sets x4."""
    return f"""
  ldr x1, [{args}, #0x28]
  cbz x1, 2f
  ldr x2, [{args}, #0x38]
  ldr x3, [{args}, #0x48]
  mul x2, x2, x3
  lsr x2, x2, #4
  bic x2, x2, #7
  cbz x2, 2f
  {f'mov x4, #{band}' if isinstance(band, int) else band}
  mul x5, x2, x4
  add x1, x1, x5
  movz w6, #{lo}
  movk w6, #{hi}, lsl #16
1:
  stp w6, w6, [x1], #8
  subs x2, x2, #8
  b.hi 1b
  dsb sy
2:
"""

# Same band but sourced from a boot_args pointer held in x0, after the MMU is on
# (so the pointer is a KVA). No stack, no AT, no vinfo: this is the independent
# probe used to tell "never reached arm_init" from "reached it but the vinfo
# paint cannot draw". Caller-saved x1-x6 are clobbered, x0 is preserved for the
# displaced call.
PAINT_BOOTARGS = """
  ldr x1, [x0, #0x28]
  cbz x1, 2f
  ldr x2, [x0, #0x38]
  ldr x3, [x0, #0x48]
  mul x2, x2, x3
  lsr x2, x2, #4
  bic x2, x2, #7
  cbz x2, 2f
  mov x4, #{band}
  mul x5, x2, x4
  add x1, x1, x5
  mov x4, x1
  mov x5, x2
  movz w6, #{lo}
  movk w6, #{hi}, lsl #16
1:
  stp w6, w6, [x1], #8
  subs x2, x2, #8
  b.hi 1b
  dsb sy
3:
  dc civac, x4
  add x4, x4, #64
  subs x5, x5, #64
  b.hi 3b
  dsb sy
2:
"""

# The V=P bootstrap map covers DarwinBoot's window only, and the window is
# allocated above the display reservation, so growing memSize can never reach
# the frame buffer. Install an identity mapping for it in TTBR0 instead, with
# the MMU still off. Called with `bl` from the stage 2 cave; boot_args are in
# x20 and x0 holds the SCTLR value, so neither may be touched.
#
#   * the address used to find the bootstrap tables and the attribute template
#     is the mapper's own PC (adr), not boot_args.physBase. The kernel image
#     starts at __PRELINK_TEXT, which sits ~45 MB below _mh_execute_header, so
#     DarwinBoot's PhysicalBase describes a window address that start.s never
#     maps: start.s re-anchors the bootstrap map on _mh_execute_header. The PC
#     is inside that map by construction;
#   * the L1 entry for the PC's slot must be a table, and the block under it is
#     copied as the attribute template, so the frame buffer is Normal-WB,
#     EL1 RW, like kernel memory (the painters clean to PoC);
#   * when the frame buffer shares the window's 1 GiB slot, its entries are
#     added to that L2 table; otherwise an L2 table is built on the window's
#     last page, which is free because DarwinBoot keeps 2 MB of the window
#     below the top, and published in the frame buffer's L1 slot;
#   * no stack: SP is a KVA and the MMU is off; no lr: the caller saves it.
#
# x8 returns 0 on success or the number of the check that refused:
#   1 no Video base, 2 zero geometry, 3 window L1 not a table, 4 window entry
#   not a block, 5 reserved L1 descriptor or no free page, 6 L2 entry in use.
MAPFB_HELPER = """
.text
.global _start
_start:
  mov x8, #1
  ldr x9, [x20, #0x28]
  cbz x9, 8f
  mov x8, #2
  ldp x10, x11, [x20, #0x38]
  ldr x12, [x20, #0x48]
  mul x10, x10, x12
  cbz x10, 8f
  add x10, x10, x9
  mov x15, #0x1FFFFF
  add x10, x10, x15
  lsr x10, x10, #21
  lsl x10, x10, #21
  bic x9, x9, #0x1FFFFF
  mrs x11, ttbr0_el1
  ldr x12, =0x0000fffffffff000
  and x11, x11, x12
  adr x14, .
  mov x8, #3
  lsr x15, x14, #30
  and x15, x15, #0x1FF
  ldr x7, [x11, x15, lsl #3]
  tst x7, #0x2
  b.eq 8f
  and x7, x7, x12
  mov x8, #4
  lsr x15, x14, #21
  and x15, x15, #0x1FF
  ldr x13, [x7, x15, lsl #3]
  and x15, x13, #3
  cmp x15, #1
  b.ne 8f
  and x13, x13, #0x1FFFFF
  lsr x1, x9, #30
  and x1, x1, #0x1FF
  mov x8, #5
  ldr x2, [x11, x1, lsl #3]
  and x3, x2, #3
  and x2, x2, x12
  cmp x3, #1
  b.eq 5f
  cmp x3, #3
  b.eq 4f
  cbnz x3, 8f
  ldr x4, [x20, #0x10]
  ldr x5, [x20, #0x18]
  add x4, x4, x5
  sub x4, x4, #0x1000
  ldr x5, [x20, #0x20]
  add x5, x5, #0x2000
  add x6, x4, #0x1000
  cmp x5, x6
  b.hi 8f
  mov x5, x4
  mov x6, #512
1:
  str xzr, [x5], #8
  subs x6, x6, #1
  b.ne 1b
  mov x2, x4
  orr x3, x4, #3
  str x3, [x11, x1, lsl #3]
  b 4f
5:
  mov x8, #0
  b 8f
4:
  mov x15, #0x200000
  mov x8, #6
1:
  lsr x3, x9, #21
  and x3, x3, #0x1FF
  ldr x4, [x2, x3, lsl #3]
  cbnz x4, 8f
  orr x4, x9, x13
  str x4, [x2, x3, lsl #3]
  add x9, x9, x15
  cmp x9, x10
  b.lo 1b
  dsb sy
  tlbi vmalle1
  dsb sy
  isb
  mov x8, #0
8:
  ret
.ltorg
"""

# Just before the MMU goes on, publish the frame buffer in the kernel's own
# vc_info so the panic screen has something to draw on long before vc_init
# exists. The seed happens with the MMU off, so vinfo is written at its
# physical address. baseaddr stays the physical address: it is valid as an
# address only through the V=P map, which the mapper has just extended to it --
# so seed only when the mapping succeeded (x8), otherwise leave vinfo zero and
# let the panic screen take its timed reset instead of faulting on the marker
# read. No stack here: SP is a KVA and the MMU is off.
SEED = """
  cbz x8, 9f
  ldr x9, ={vinfo}
  ldr x10, [{args}, #0x08]
  sub x9, x9, x10
  ldr x10, [{args}, #0x10]
  add x9, x9, x10
  ldp x10, x11, [{args}, #0x48]
  ldp x12, x13, [{args}, #0x38]
  stp w10, w13, [x9, #0x00]
  stp w11, w12, [x9, #0x08]
  ldr x10, [{args}, #0x28]
  str x10, [x9, #0x10]
  movz w11, #{marker_lo}
  movk w11, #{marker_hi}, lsl #16
  str w11, [x9, #0x18]
9:
"""

# Post-MMU paint, sourced from the seeded vc_info instead of boot_args: no
# register of the interrupted kernel code holds boot_args, and the vinfo copy is
# exactly what the panic screen will use, so a visible bar also proves the panic
# screen has a usable frame buffer. AT asks the hardware and reports a fault in
# PAR_EL1.F instead of raising one, so an unmapped frame buffer costs a skipped
# bar, not a fault. The pixels are Normal-WB here, so they are cleaned to the
# point of coherency before the display engine can see them.
PAINT_INFO = """
  stp x1, x2, [sp, #-64]!
  stp x3, x4, [sp, #16]
  stp x5, x6, [sp, #32]
  str x9, [sp, #48]
  ldr x9, ={vinfo}
  ldr x1, [x9, #0x10]
  cbz x1, 2f
  at s1e1w, x1
  isb
  mrs x6, par_el1
  tbnz x6, #0, 2f
  ldr w2, [x9, #0x0C]
  ldr w3, [x9, #0x00]
  mul x2, x2, x3
  lsr x2, x2, #4
  bic x2, x2, #7
  mov x4, #{band}
  mul x5, x2, x4
  add x1, x1, x5
  mov x4, x1
  mov x5, x2
  movz w6, #{lo}
  movk w6, #{hi}, lsl #16
1:
  stp w6, w6, [x1], #8
  subs x2, x2, #8
  b.hi 1b
  dsb sy
3:
  dc civac, x4
  add x4, x4, #64
  subs x5, x5, #64
  b.hi 3b
  dsb sy
2:
  ldr x9, [sp, #48]
  ldp x5, x6, [sp, #32]
  ldp x3, x4, [sp, #16]
  ldp x1, x2, [sp], #64
"""

SOURCES = {
    1: (".text\n.global _start\n_start:\n"
        + paint('x0', '0x00FF', '0xFF00', band=1)
        + "  b target\n"),
    # vinfo='{vinfo}' leaves the symbol as a placeholder for main(), which is the
    # first place the kernel's symbol table (and so the address) exists.
    # lr holds the KVA of arm_init, so it is parked in x16 (which the mapper
    # leaves alone) across the call; a failed map paints white at 5+code.
    2: (".text\n.global _start\n_start:\n"
        + paint('x20', '0x00FF', '0xFFFF', band=2)
        + "  mov x16, lr\n  bl helper\n  mov lr, x16\n"
        + "  cbz x8, 3f\n"
        + paint('x20', '0xFFFF', '0xFFFF', band='add x4, x8, #5')
        + "3:\n"
        + SEED.format(args='x20', vinfo='{vinfo}',
                      marker_lo=f'0x{VINFO_SEED & 0xFFFF:04X}', marker_hi=f'0x{VINFO_SEED >> 16:04X}')
        # Band 4 is painted after the SCTLR write, through the mapping the helper
        # just published: it proves the MMU is on and the frame buffer
        # translates. The address and the band size are preloaded while boot_args
        # is still readable -- with the MMU on, boot_args' physical address is
        # below the bootstrap map's start and cannot be dereferenced. Skipped when
        # the mapper failed, because the address would not translate.
        + """  cbnz x8, 7f
  ldr x9, [{args}, #0x28]
  cbz x9, 7f
  ldr x10, [{args}, #0x38]
  ldr x11, [{args}, #0x48]
  mul x10, x10, x11
  lsr x10, x10, #4
  bic x10, x10, #7
  cbz x10, 7f
  add x9, x9, x10, lsl #2
  b 8f
7:
  mov x9, xzr
8:
""".format(args='x20')
        + """  msr SCTLR_EL1, x0
  isb
  cbz x9, 9f
  movz w6, #0xFFFF
  movk w6, #0xFF00, lsl #16
  mov x1, x9
  mov x2, x10
  mov x4, x9
1:
  stp w6, w6, [x1], #8
  subs x2, x2, #8
  b.hi 1b
  dsb sy
  mov x5, x10
2:
  dc civac, x4
  add x4, x4, #64
  subs x5, x5, #64
  b.hi 2b
  dsb sy
9:
  b target
.ltorg
"""),
    3: """.text
.global _start
_start:
  mov x0, #0x300000
  mrs x1, cntpct_el0
  ldr x2, ={ticks}
  add x2, x1, x2
1:
  isb
  mrs x1, cntpct_el0
  cmp x1, x2
  b.lo 1b
  movz w0, #0x0009
  movk w0, #0x8400, lsl #16
  mov x1, xzr
  mov x2, xzr
  mov x3, xzr
  smc #0
2:
  wfe
  b 2b
""" + ".ltorg\n",
    4: """.text
.global _start
_start:
  bl callee
  mrs x1, cntpct_el0
  ldr x2, ={ticks}
  add x2, x1, x2
1:
  isb
  mrs x1, cntpct_el0
  cmp x1, x2
  b.lo 1b
  movz w0, #0x0009
  movk w0, #0x8400, lsl #16
  mov x1, xzr
  mov x2, xzr
  mov x3, xzr
  smc #0
2:
  wfe
  b 2b
""" + ".ltorg\n",
}

RESET_BEFORE = """.text
.global _start
_start:
  movz w0, #0x0009
  movk w0, #0x8400, lsl #16
  mov x1, xzr
  mov x2, xzr
  mov x3, xzr
  smc #0
1:
  wfe
  b 1b
"""

ROOT = Path(__file__).resolve().parents[1]
SEGMENTS = []   # (vmaddr, vmsize, fileoff, filesize) of the kernel being patched


def tool(name, hint):
    # Prefer the Homebrew LLVM over Apple's: the cross aarch64-none-elf objects and
    # ld.lld live there, and /usr/bin/clang would otherwise win.
    for directory in ('/opt/homebrew/opt/llvm/bin', '/opt/homebrew/opt/lld/bin'):
        candidate = Path(directory) / name
        if candidate.exists():
            return str(candidate)
    path = shutil.which(name)
    if not path:
        raise SystemExit(f'missing {name}: {hint}')
    return path


def kernel_symbols(kernel):
    """Address -> name for the kernel's symbols, from llvm-nm."""
    table = {}
    for line in subprocess.run([tool('llvm-nm', 'brew install llvm'), '-n', kernel],
                               capture_output=True, text=True, check=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            table.setdefault(int(parts[0], 16), parts[2])
    return table


def function_range(symbols, name):
    """Start and end (next symbol) of a function by its symbol name."""
    starts = sorted(symbols)
    for start, symbol in symbols.items():
        if symbol == name:
            return start, next(address for address in starts if address > start)
    raise SystemExit(f'No symbol {name} in the kernel')


def find_call(data, symbols, function, name, occurrence):
    """The VA and target of the Nth `bl name` inside FUNCTION."""
    hits = []
    low, high = function_range(symbols, function)
    for va in range(low, high, 4):
        word = struct.unpack_from('<I', data, file_offset(va))[0]
        if word >> 26 == 0b100101:
            delta = word & 0x3FFFFFF
            delta -= (1 << 26) if delta & (1 << 25) else 0
            target = va + delta * 4
            if symbols.get(target) == name:
                hits.append((va, target))
    if len(hits) < occurrence:
        raise SystemExit(f'{function} has {len(hits)} call(s) to {name}, not {occurrence}')
    return hits[occurrence - 1]


def assemble_c(source, va, workdir, symbols):
    """Build a freestanding C file for VA; returns (bytes, load address of the first byte, address of panic_screen).

    The linker may start .text a little after VA (it keeps the ELF headers in front), so the
    load address is read back from the ELF and is what the caller must write the bytes to."""
    clang = tool('clang', 'xcode-select --install')
    lld = tool('ld.lld', 'brew install llvm')
    objcopy = tool('llvm-objcopy', 'brew install llvm')
    nm = tool('llvm-nm', 'brew install llvm')
    font_dir = Path(os.environ.get('UNIXNU_UEFI') or '') / 'Silicon/Silicium/SiliciumPkg/Library/FrameBufferSerialPortLib'
    if not (font_dir / 'Font.h').is_file():
        raise SystemExit(f'no Font.h under {font_dir}; build the payload with --uefi pointing at the Mu-Silicium checkout')
    obj, elf, binary = (workdir / name for name in ('panic.o', 'panic.elf', 'panic.bin'))
    definitions = []
    for macro, name, width in (('MAX_MEM', '_max_mem', 8),
                               ('PAGE_TOTAL', '_vm_page_pages', 4),
                               ('PAGE_FREE', '_vm_page_free_count', 4)):
        address = symbols.get(name)
        if address is None or address % width or not any(
                base <= address and address + width <= base + size
                for base, size, _, _ in SEGMENTS):
            raise SystemExit(f'Missing or invalid diagnostic symbol {name}')
        definitions.append(f'-D{macro}={address:#x}UL')
    text = symbols.get('_boot_progress_panic_text')
    if text is not None:
        if not any(base <= text and text + 256 <= base + size for base, size, _, _ in SEGMENTS):
            raise SystemExit('Invalid _boot_progress_panic_text')
        definitions.append(f'-DPANIC_TEXT={text:#x}UL')
    subprocess.run([clang, '--target=aarch64-none-elf', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                    '-mgeneral-regs-only', '-fno-pic', '-mcmodel=tiny', '-Oz', f'-DVINFO={symbols["_vinfo"]:#x}UL',
                    *definitions, f'-I{font_dir}', '-c', source, '-o', obj], check=True)
    script = workdir / 'panic.ld'
    script.write_text('SECTIONS { . = %#x; .text : { *(.text.a_entry) *(.text.a_common) *(.text*) *(.rodata*) } /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.ARM.*) } }\n' % va)
    size = tool('llvm-size', 'brew install llvm')
    sizes = subprocess.run([size, '--format=berkeley', obj], capture_output=True, text=True, check=True).stdout.splitlines()[-1].split()
    if int(sizes[1]) or int(sizes[2]):
        # The code runs from the kernel's read-only text: a global would fault on its first store.
        raise SystemExit(f'{source.name} has writable data (data {sizes[1]}, bss {sizes[2]}); use the stack')
    subprocess.run([lld, '-T', script, '-e', 'panic_screen', obj, '-o', elf], check=True)
    # Only .text: a plain binary dump would start at the ELF headers kept in front of it.
    subprocess.run([objcopy, '-O', 'binary', '-j', '.text', elf, binary], check=True)
    readelf = tool('llvm-readelf', 'brew install llvm')
    load = None
    for line in subprocess.run([readelf, '-S', elf], capture_output=True, text=True, check=True).stdout.splitlines():
        parts = line.replace('[', ' ').replace(']', ' ').split()
        if len(parts) >= 4 and parts[1] == '.text':
            load = int(parts[3], 16)
    if load is None or load < va:
        raise SystemExit('Cannot find where the linker put .text')
    entry = None
    for line in subprocess.run([nm, elf], capture_output=True, text=True, check=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == 'panic_screen':
            entry = int(parts[0], 16)
    if entry != load:
        raise SystemExit(f'panic_screen at {entry:#x} is not the first byte {load:#x}')
    return binary.read_bytes(), load, entry


def panic_common_entry(workdir, code, load):
    """Resolve the real alternate entry and verify its distinct argument ABI."""
    names = {parts[2]: int(parts[0], 16)
             for line in subprocess.check_output([tool('llvm-nm', 'brew install llvm'), workdir / 'panic.elf'], text=True).splitlines()
             if len(parts := line.split()) == 3}
    entry = names.get('panic_screen_common')
    main = names.get('panic_screen_main')
    if entry is None or main is None or not load <= entry <= load + len(code) - 8:
        raise SystemExit('Common panic renderer entry is missing or out of bounds')
    at = entry - load
    if code[at:at + 8] != struct.pack('<II', 0xf9400021, branch(entry + 4, main)):
        raise SystemExit('Common panic entry does not dereference the Darwin va_list pointer and branch to the renderer')
    return entry


def load_segments(data):
    """Read the Mach-O segment table; the file layout differs between kernel builds."""
    magic, _, _, _, ncmds = struct.unpack_from('<IiiII', data, 0)
    if magic != 0xFEEDFACF:
        raise SystemExit('Not a 64-bit Mach-O')
    offset = 32
    SEGMENTS.clear()
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, offset)
        if cmd == 0x19:
            _, vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<16sQQQQ', data, offset + 8)
            if filesize:
                SEGMENTS.append((vmaddr, vmsize, fileoff, filesize))
        offset += size


def unixthread_pc(data):
    """LC_UNIXTHREAD entry pc (arm64 thread state: flavor, count, x0..x28, fp, lr, sp, pc)."""
    offset = 32
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        cmd, size = struct.unpack_from('<II', data, offset)
        if cmd == 0x5:
            return struct.unpack_from('<Q', data, offset + 16 + 32 * 8)[0]
        offset += size
    raise SystemExit('No LC_UNIXTHREAD in kernel')


def file_offset(va):
    for vmaddr, vmsize, fileoff, filesize in SEGMENTS:
        if vmaddr <= va < vmaddr + min(vmsize, filesize):
            return fileoff + (va - vmaddr)
    raise SystemExit(f'{va:#x} is not in a file-backed segment of this kernel')


def branch(from_va, to_va):
    delta = (to_va - from_va) // 4
    if (to_va - from_va) % 4 or not -(1 << 25) <= delta < (1 << 25):
        raise SystemExit('Branch out of range')
    return 0x14000000 | (delta & 0x3FFFFFF)


def zero_runs(data, va):
    """(VA, length) of every run of zero words from VA to its segment's end."""
    for vmaddr, vmsize, fileoff, filesize in SEGMENTS:
        if vmaddr <= va < vmaddr + min(vmsize, filesize):
            at = file_offset(va)
            stop = fileoff + min(vmsize, filesize)
            break
    else:
        raise SystemExit(f'{va:#x} is not in a file-backed segment')
    run = None
    while at < stop:
        empty = data[at:at + 4] == b'\0\0\0\0'
        if empty and run is None:
            run = at
        elif not empty and run is not None:
            yield va + (run - file_offset(va)), at - run
            run = None
        at += 4
    if run is not None:
        yield va + (run - file_offset(va)), stop - run


def padding_fits(data, load, size):
    """Whether the zero run that contains load is long enough for size bytes."""
    return any(start <= load and load + size <= start + length for start, length in zero_runs(data, load))


def entry_body(symbols):
    """The entry body after _start_first_cpu; the SCTLR/ISB/CPACR sequence lives inside.

    The sequence is in common_start (the next label), not in _start_first_cpu
    itself, so this is a fixed window, not a symbol range. Bootstrap
    table-capacity checks can grow before it, so the sequence is found by pattern.
    """
    low = symbols['_start_first_cpu']
    return low, low + 0x1000


def mmu_site(data, names):
    """The verified `msr SCTLR_EL1, x0; isb; ... mov x0, #0x300000; msr CPACR_EL1, x0` sequence."""
    low, high = entry_body(names)

    def instruction(at):
        return struct.unpack_from('<I', data, file_offset(at))[0]

    matches = [va for va in range(low, high, 4)
               if instruction(va) == 0xD5181000 and instruction(va + 4) == 0xD5033FDF
               and instruction(va + 0x20) == 0xD2A00600 and instruction(va + 0x24) == 0xD5181040]
    if len(matches) != 1:
        raise SystemExit(f'Require one verified SCTLR/ISB/CPACR entry sequence, found {len(matches)}')
    return matches[0], matches[0] + 0x20


def assemble(source, va, symbols, workdir):
    asm, obj, elf, binary = (workdir / name for name in ('cave.S', 'cave.o', 'cave.elf', 'cave.bin'))
    asm.write_text(source)
    subprocess.run([tool('clang', 'xcode-select --install'), '--target=aarch64-none-elf', '-c', asm, '-o', obj], check=True)
    subprocess.run([tool('ld.lld', 'brew install llvm'), f'-Ttext={va:#x}',
                    *[f'--defsym={name}={value:#x}' for name, value in symbols.items()],
                    '-e', '_start', obj, '-o', elf], check=True)
    subprocess.run([tool('llvm-objcopy', 'brew install llvm'), '-O', 'binary', elf, binary], check=True)
    return binary.read_bytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('kernel', type=Path, help='the COPY of the kernel to patch in place')
    parser.add_argument('--reset-delay', type=int, default=6, help='seconds before the reset')
    parser.add_argument('--reset-after', metavar='SYMBOL', help='place the reset after this call in arm_init instead of stage 3')
    parser.add_argument('--reset-before', action='store_true', help='reset at the call site before invoking SYMBOL (requires --reset-after)')
    parser.add_argument('--paint-after', metavar='SYMBOL', help='paint a bar before this call and keep booting')
    parser.add_argument('--no-paint', action='store_true', help='leave out the stage 3 bar')
    parser.add_argument('--paint-in', dest='paint_function', default='_arm_init', help='function that makes the painted call (default _arm_init)')
    parser.add_argument('--paint-band', type=int, default=3, help='which 1/16th band the painted bar fills (default 3)')
    parser.add_argument('--paint-occurrence', type=int, default=1, help='which call to SYMBOL to paint after (1 = first)')
    parser.add_argument('--paint2-after', metavar='SYMBOL',
                        help='paint an independent bar before this call; the call site must hold boot_args in x0')
    parser.add_argument('--no-paint2', action='store_true', help='leave out the independent stage 4 bar')
    parser.add_argument('--paint2-in', dest='paint2_function', default='_arm_init', help='function that makes the second painted call (default _arm_init)')
    parser.add_argument('--paint2-band', type=int, default=12, help='which 1/16th band the second bar fills (default 12)')
    parser.add_argument('--paint2-occurrence', type=int, default=1, help='which call to SYMBOL the second bar precedes (1 = first)')
    parser.add_argument('--panic-screen', action='store_true', help='draw panic() messages on the kernel frame buffer')
    parser.add_argument('--no-reset', action='store_true', help='no reset probe at all')
    parser.add_argument('--occurrence', type=int, default=1, help='which call to SYMBOL (1 = first)')
    parser.add_argument('--in', dest='function', default='_arm_init', help='function that makes the call (default _arm_init)')
    args = parser.parse_args()
    if args.reset_before and not args.reset_after:
        raise SystemExit('--reset-before requires --reset-after')
    # Defaults make both post-MMU bars part of this board's kernel without a
    # local change to the base repository's payload step. Stage 3 paints from the
    # seeded vinfo; stage 4 paints from the boot_args pointer in x0. A cave that
    # a reset probe claimed is left alone.
    if not args.no_paint and not args.paint_after and (args.no_reset or args.reset_after):
        args.paint_after = '_cpu_data_init'
    if not args.no_paint2 and not args.paint2_after and (args.no_reset or not args.reset_after):
        args.paint2_after = '_arm_set_kernel_tbi'
    data = bytearray(args.kernel.read_bytes())
    load_segments(data)
    symbols_by_address = kernel_symbols(args.kernel)
    names = {name: va for va, name in symbols_by_address.items()}
    for name in ('_start_first_cpu', '_panic', '_vinfo'):
        if name not in names:
            raise SystemExit(f'{args.kernel} has no {name}; this probe needs the symbol table')
    entry = unixthread_pc(data)
    start_first_cpu = names['_start_first_cpu']
    mmu, after_mmu = mmu_site(data, names)
    if struct.unpack_from('<I', data, file_offset(entry))[0] != branch(entry, start_first_cpu):
        raise SystemExit(f'{entry:#x} is not `b {start_first_cpu:#x}`')

    def word(va):
        return struct.unpack_from('<I', data, file_offset(va))[0]

    caves = {stage: entry + offset for stage, offset in CAVE_OFFSETS.items()}
    helper_va = entry + HELPER_OFFSET
    expected = {entry: branch(entry, start_first_cpu), mmu: 0xD5181000, after_mmu: 0xD2A00600}
    for va, value in expected.items():
        if word(va) != value:
            raise SystemExit(f'{va:#x} holds {word(va):#010x}, expected {value:#010x}: not the kernel these probes are for')
    for stage, va in caves.items():
        if any(data[file_offset(va):file_offset(va) + CAVE_LIMITS[stage]]):
            raise SystemExit(f'Stage {stage} padding at {va:#x} is not empty')

    sites = [(entry, 1, {'target': start_first_cpu}),
             (mmu, 2, {'target': mmu + 4, 'helper': helper_va})]
    if args.no_reset:
        description = 'no reset probe'
    elif args.reset_after:
        call_va, callee = find_call(data, symbols_by_address, args.function, args.reset_after, args.occurrence)
        sites.append((call_va, 4, {'callee': callee, 'target': call_va + 4}))
        description = f'stage 4 reset {"before" if args.reset_before else "after"} call #{args.occurrence} to {args.reset_after} in {args.function} (at {call_va:#x})'
    else:
        sites.append((after_mmu, 3, {'target': after_mmu + 4}))
        description = f'stage 3 reset {args.reset_delay} s after the MMU is on'

    # Stage 3 doubles as the post-MMU paint when asked: the cave paints first and
    # then really executes the displaced `bl`, so booting continues untouched.
    paint_source = None
    if args.paint_after:
        paint_va, paint_callee = find_call(data, symbols_by_address, args.paint_function, args.paint_after,
                                           args.paint_occurrence)
        if any(site == paint_va for site, _, _ in sites):
            raise SystemExit(f'--paint-after and the reset site are both {paint_va:#x}')
        sites.append((paint_va, 3, {'callee': paint_callee, 'target': paint_va + 4}))
        paint_source = ('.text\n.global _start\n_start:\n'
                        + PAINT_INFO.format(vinfo=f'{names["_vinfo"]:#x}UL', band=args.paint_band,
                                            lo='0xFF00', hi='0x00FF')
                        + '  bl callee\n  b target\n.ltorg\n')
        description += (f', stage 3 paints band {args.paint_band} on reaching call #{args.paint_occurrence}'
                        f' to {args.paint_after} in {args.paint_function} (at {paint_va:#x}) and continues')

    # The stage 4 cave carries an independent painter: it reads boot_args through
    # x0 and never uses the vinfo copy, the stack, or AT. It exists to separate
    # "the kernel never reached this call" from "the vinfo painter cannot draw".
    paint2_source = None
    if args.paint2_after:
        paint2_va, paint2_callee = find_call(data, symbols_by_address, args.paint2_function, args.paint2_after,
                                             args.paint2_occurrence)
        if any(site == paint2_va for site, _, _ in sites):
            raise SystemExit(f'--paint2-after is already a probe site: {paint2_va:#x}')
        if any(stage == 4 for _, stage, _ in sites):
            raise SystemExit('--paint2-after needs the stage 4 cave, which the reset probe is using')
        sites.append((paint2_va, 4, {'callee': paint2_callee, 'target': paint2_va + 4}))
        paint2_source = ('.text\n.global _start\n_start:\n'
                         + PAINT_BOOTARGS.format(band=args.paint2_band, lo='0x8000', hi='0xFFFF')
                         + '  bl callee\n  b target\n.ltorg\n')
        description += (f', stage 4 paints band {args.paint2_band} from boot_args on reaching call '
                        f'#{args.paint2_occurrence} to {args.paint2_after} in {args.paint2_function} '
                        f'(at {paint2_va:#x}) and continues')

    with tempfile.TemporaryDirectory() as tmp:
        for site, stage, symbols in sites:
            if paint2_source is not None and site == paint2_va:
                source = paint2_source
            elif paint_source is not None and site == paint_va:
                source = paint_source
            else:
                source = RESET_BEFORE if (stage == 4 and args.reset_before) else SOURCES[stage].format(
                    ticks=TIMEBASE * args.reset_delay, vinfo=f'{names["_vinfo"]:#x}UL')
            code = assemble(source, caves[stage], symbols, Path(tmp))
            if len(code) > CAVE_LIMITS[stage]:
                raise SystemExit(f'Stage {stage} code is too large')
            data[file_offset(caves[stage]):file_offset(caves[stage]) + len(code)] = code
        helper = assemble(MAPFB_HELPER, helper_va, {}, Path(tmp))
        if not padding_fits(data, helper_va, len(helper)):
            raise SystemExit(f'No room for the {len(helper):#x}-byte mapper at {helper_va:#x}')
        data[file_offset(helper_va):file_offset(helper_va) + len(helper)] = helper
    for site, stage, _ in sites:
        struct.pack_into('<I', data, file_offset(site), branch(site, caves[stage]))
    if args.panic_screen:
        panic_va = names['_panic']
        if word(panic_va) in (0, 0xFFFFFFFF):
            raise SystemExit('panic() does not look like code')
        text = ROOT / 'scripts/xnu-panic-screen.c'
        if not text.is_file():
            raise SystemExit(f'{text} is missing')
        at = max(max(caves[stage] + CAVE_LIMITS[stage] for _, stage, _ in sites),
                 helper_va + len(helper))
        at = (at + 15) & ~15
        with tempfile.TemporaryDirectory() as tmp:
            code, load, panic_entry = assemble_c(text, at, Path(tmp), names)
            if not padding_fits(data, load, len(code)):
                # Three staged caves can leave too little room next to the entry; the
                # renderer is position-independent, so take the next zero run that fits.
                at = next((start for start, length in zero_runs(data, at)
                           if start > at and length >= len(code)), None)
                if at is None:
                    raise SystemExit(f'No {len(code):#x}-byte padding for the panic screen')
                code, load, panic_entry = assemble_c(text, at, Path(tmp), names)
                if not padding_fits(data, load, len(code)):
                    raise SystemExit(f'No padding for the panic screen ({len(code):#x} bytes) at {load:#x}')
            common_entry = panic_common_entry(Path(tmp), code, load)
        data[file_offset(load):file_offset(load) + len(code)] = code
        struct.pack_into('<I', data, file_offset(panic_va), branch(panic_va, panic_entry))
        common = names.get('_panic_trap_to_debugger')
        if common is None or word(common) in (0, 0xFFFFFFFF):
            raise SystemExit('Common panic funnel is missing; refuse a partially covered panic screen')
        struct.pack_into('<I', data, file_offset(common), branch(common, common_entry))
        description += f', panic screen ({len(code):#x} bytes at {panic_entry:#x}; common funnel at {common:#x})'
    args.kernel.write_bytes(data)
    print(f'patched {args.kernel.name}: stage 1 blue bar at {entry:#x}, stage 2 magenta bar at {mmu:#x}, {description}')


if __name__ == '__main__':
    main()
