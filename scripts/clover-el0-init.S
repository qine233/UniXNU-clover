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
// that never got there. exit(1) on the other hand runs through kern_exit, where
// the kernel panics with "initproc exited (pid 1) ...": white text on the panic
// screen, which can only appear if the root mounted, the exec succeeded, the
// cdhash passed AMFI, and the first user instruction retired. A refusal earlier
// than that panics with a different message, so the screen still names the wall.
//
// The writes to fd 1 are a bonus channel, not the reading: bsd_init gives init
// /dev/console, and whether a write there reaches the panel depends on a console
// tty being registered, which this port has never had (there is no UART driver).
.text

.global _start, _checkpoint
_start:
  // pid = getpid()                                  (SYS_getpid = 20)
  mov x16, #20
  svc #0x80
  mov x19, x0

  // sysctl({CTL_HW, HW_PAGESIZE}) -> scratch        (SYS_sysctl = 202)
  // The value itself is not printed; the return code is, because it is the
  // second distinct kernel entry this process makes after getpid.
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
  mov x20, x0

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

  // the pid in decimal, written from the end of the buffer backwards
  adrp x21, _numbuf@PAGE
  add x21, x21, _numbuf@PAGEOFF
  add x22, x21, #24                 // one past the last byte of the buffer
  mov x23, x22                      // cursor: nothing written yet
  mov x24, x19
1:
  // A pid of 0 still has to print one digit, so the division runs first.
  mov x10, #10
  udiv x11, x24, x10
  msub x12, x11, x10, x24           // x12 = x24 % 10
  sub x23, x23, #1
  add x12, x12, #'0'
  sturb w12, [x23, #0]
  mov x24, x11
  cbnz x24, 1b
  // x23..x22 holds the digits
  mov x1, x23
  sub x2, x22, x23
  bl _write_console

  // " sysctl=" + the sysctl return code + "\n"
  adrp x1, _middle@PAGE
  add x1, x1, _middle@PAGEOFF
  adrp x2, _middle_end@PAGE
  add x2, x2, _middle_end@PAGEOFF
  sub x2, x2, x1
  bl _write_console
  adrp x21, _numbuf@PAGE
  add x21, x21, _numbuf@PAGEOFF
  add x22, x21, #24
  mov x23, x22
  mov x24, x20
  cbnz x24, 2f
  // Zero prints as one digit.
  sub x23, x23, #1
  mov w12, #'0'
  sturb w12, [x23, #0]
  b 3f
2:
  // The return code is 0 on success; anything else is an errno, which is small
  // and positive here, so the same unsigned loop serves.
  mov x10, #10
4:
  udiv x11, x24, x10
  msub x12, x11, x10, x24
  sub x23, x23, #1
  add x12, x12, #'0'
  sturb w12, [x23, #0]
  mov x24, x11
  cbnz x24, 4b
3:
  mov x1, x23
  sub x2, x22, x23
  bl _write_console
  adrp x1, _newline@PAGE
  add x1, x1, _newline@PAGEOFF
  adrp x2, _newline_end@PAGE
  add x2, x2, _newline_end@PAGEOFF
  sub x2, x2, x1
  bl _write_console

  // exit(0)                                         (SYS_exit = 1)
  mov x0, #0
  mov x1, #0
  mov x16, #1
  svc #0x80

  // exit returned, which userland never does: park where a debugger (or a
  // future photo of a stuck panel) can find the registers.
_checkpoint:
  b _checkpoint

// x1 = bytes, x2 = length; write(1, ...). The return value is ignored on
// purpose: with no console tty registered the kernel may refuse this, and the
// reading of this round must not depend on it.
_write_console:
  mov x0, #1
  mov x16, #4
  svc #0x80
  ret

.data
.balign 8
_mib:
  .word 6, 7                        // CTL_HW, HW_PAGESIZE (Darwin 23 sys/sysctl.h)
.balign 8
_scratch:
  .quad 0
_scratch_size:
  .quad 8
_banner:
  .ascii "[clover] el0 init: pid="
_banner_end:
_middle:
  .ascii " sysctl="
_middle_end:
_newline:
  .ascii "\n"
_newline_end:
.balign 8
_numbuf:
  .space 24
