// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The init program of the clover md0/mockfs root.
//
// mockfs (bsd/miscfs/mockfs) turns the root block device into a three-node
// filesystem: "/" , "/dev" for devfs, and "/sbin/launchd", which is the device
// itself reinterpreted as one Mach-O. XNU's boot thread execs that path from
// bsd_utaskbootstrap and never returns: the thread becomes the process. So this
// file is the whole userland of this round -- static, no dyld, no libraries and
// no Apple userspace bytes, exactly like the upstream QEMU fixture
// (scripts/qemu-entry/native-user.S), with one addition: it exits.
//
// Why exiting is the point. The panel is the only output on this tablet, and a
// process that spins at EL0 looks from the outside exactly like a boot thread
// that never got there. exit() on the other hand runs through
// proc_handle_critical_exit (bsd/kern/kern_exit.c), which panics with
// "initproc failed to start -- no exit reason available -- (signal %d, exit
// status %d)": white text on the panic screen, which can only appear if the root
// mounted, the exec succeeded, the cdhash passed AMFI and the first user
// instruction retired. "failed to start" rather than "exited" because
// initproc_spawned is not set by the kernel -- launchd writes
// kern.initproc_spawned from its own main -- so every exit of an init that never
// wrote that sysctl reads as a failed start, including this one.
//
// The exit status is the reading, so it is spent carefully: 0 means every step
// below worked, and anything else is stage * 32 + errno, where the stage is 1
// for getpid, 2 for sysctl, 3 for open("/dev/console") and 4 for write.
//
// Why the console has to be opened by us. Round 65 retired the assumption that
// fd 1 is a console: this XNU has no open_console() at all, bsd_init hands
// initproc an empty descriptor table, and the console is just a devfs node --
// devfs_init (bsd/miscfs/devfs/devfs_vfsops.c) makes makedev(0, 0) under the
// name "console", cnopen (bsd/dev/arm/cons.c) forwards it to km_tty[0], which
// kminit (bsd/dev/arm/km.c) allocated during bsd_autoconf, and kmopen sizes the
// tty from PE_current_console. So open("/dev/console") is the first time this
// port puts a character on the panel from EL0, and the banner it writes is the
// payload of this round: pid, the fd the open returned, and the page size the
// sysctl read back.
.text

.global _start, _checkpoint
_start:
  mov x23, #0                       // first failure: stage * 32 + errno

  // pid = getpid()                                  (SYS_getpid = 20)
  mov x16, #20
  svc #0x80
  mov x1, #32
  b.cs _fail
  mov x19, x0

  // sysctl({CTL_HW, HW_PAGESIZE}) -> _scratch       (SYS_sysctl = 202)
  // Read back as a number, not just a return code: a page size that comes out
  // as 4096 means the copy-out into the user stack page worked too, which is a
  // different mapping than the one the text of this process runs from.
  adrp x0, _mib@PAGE
  add x0, x0, _mib@PAGEOFF
  mov x1, #2
  adrp x2, _scratch@PAGE
  add x2, x2, _scratch@PAGEOFF
  adrp x3, _scratch_size@PAGE
  add x3, x3, _scratch_size@PAGEOFF
  mov x4, #0
  mov x5, #0
  mov x16, #202
  svc #0x80
  mov x1, #64
  b.cs _fail
  adrp x0, _scratch@PAGE
  add x0, x0, _scratch@PAGEOFF
  ldr x21, [x0]

  // fd = open("/dev/console", O_RDWR)               (SYS_open = 5)
  // O_RDWR is 2 in the Darwin fcntl.h this image is linked against.
  adrp x0, _console_path@PAGE
  add x0, x0, _console_path@PAGEOFF
  mov x1, #2
  mov x2, #0
  mov x16, #5
  svc #0x80
  mov x1, #96
  b.cs _fail                        // no fd: the banner has nowhere to go
  mov x22, x0

  // "[clover] el0 init: pid="
  // The length is computed at run time from the two labels: the assembler
  // cannot fold a label difference into a mov (unknown AArch64 fixup kind), and
  // hardcoding the count here would be a second source of truth for the string.
  adrp x1, _banner@PAGE
  add x1, x1, _banner@PAGEOFF
  adrp x2, _banner_end@PAGE
  add x2, x2, _banner_end@PAGEOFF
  sub x2, x2, x1
  bl _write_console
  mov x1, #128
  b.cs _fail
  mov x0, x19
  bl _print_dec
  mov x1, #128
  b.cs _fail

  adrp x1, _middle@PAGE
  add x1, x1, _middle@PAGEOFF
  adrp x2, _middle_end@PAGE
  add x2, x2, _middle_end@PAGEOFF
  sub x2, x2, x1
  bl _write_console
  mov x1, #128
  b.cs _fail
  mov x0, x22
  bl _print_dec
  mov x1, #128
  b.cs _fail

  adrp x1, _tail@PAGE
  add x1, x1, _tail@PAGEOFF
  adrp x2, _tail_end@PAGE
  add x2, x2, _tail_end@PAGEOFF
  sub x2, x2, x1
  bl _write_console
  mov x1, #128
  b.cs _fail
  mov x0, x21
  bl _print_dec
  mov x1, #128
  b.cs _fail

  adrp x1, _newline@PAGE
  add x1, x1, _newline@PAGEOFF
  adrp x2, _newline_end@PAGE
  add x2, x2, _newline_end@PAGEOFF
  sub x2, x2, x1
  bl _write_console
  mov x1, #128
  b.cs _fail

  // exit(x23): 0 here means EL0 wrote to the console and every step agreed.
  mov x0, x23
  mov x16, #1
  svc #0x80

  // exit returned, which userland never does: park where a debugger (or a
  // future photo of a stuck panel) can find the registers.
_checkpoint:
  b _checkpoint

// Record the first failure only: later ones are consequences of the first, and
// a sum of stage codes would decode as a different stage.
// x0 = errno as returned by the syscall, x1 = that call's stage * 32.
_fail:
  cbnz x23, _fail_done
  and x0, x0, #31
  add x23, x1, x0
_fail_done:
  ret

// x22 = fd, x1 = bytes, x2 = length. Leaves the carry flag as the kernel set it,
// so the caller can test the write without a second register dance.
_write_console:
  mov x0, x22
  mov x16, #4
  svc #0x80
  ret

// x0 = unsigned value, printed in decimal through _write_console. Digits are
// filled from the end of the buffer backwards, and the cursor starts one past
// the last byte so that a value of 0 still emits a single "0".
_print_dec:
  adrp x27, _numbuf@PAGE
  add x27, x27, _numbuf@PAGEOFF
  add x28, x27, #24
  mov x29, x28
  mov x9, #10
1:
  udiv x10, x0, x9
  msub x11, x10, x9, x0
  sub x29, x29, #1
  add x11, x11, #'0'
  sturb w11, [x29, #0]
  mov x0, x10
  cbnz x0, 1b
  mov x1, x29
  sub x2, x28, x29
  b _write_console

.data
.balign 8
_mib:
  .word 6, 7                        // CTL_HW, HW_PAGESIZE (Darwin 23 sys/sysctl.h)
.balign 8
_scratch:
  .quad 0
_scratch_size:
  .quad 8
_console_path:
  .asciz "/dev/console"
_banner:
  .ascii "[clover] el0 init: pid="
_banner_end:
_middle:
  .ascii " console fd="
_middle_end:
_tail:
  .ascii " pagesize="
_tail_end:
_newline:
  .ascii "\n"
_newline_end:
.balign 8
_numbuf:
  .space 24
