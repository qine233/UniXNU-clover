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
// (scripts/qemu-entry/native-user.S), with one addition: it can exit, and only
// does so when something failed.
//
// Why exiting is the failure channel. The panel is the only output on this
// tablet, and a process that spins at EL0 looks from the outside exactly like
// a boot thread that never got there. exit() on the other hand runs through
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
// Round 66 spent it and got 0 back: the open returned a descriptor and the
// write returned clean, so EL0 can talk to the console tty. What the photo
// could not show is the line itself. The panic hook (xnu-panic-screen.c) clears
// [h/8, h/2) and prints from h/8+8; the strip above it is whatever the live
// console and the msgbuf replay left there, and tty output never enters the
// msgbuf, so the banner was drawn and then erased by the very panic that
// reported its success. Exiting is therefore only the failure channel now: on
// success this program fills the panel with the banner -- 240 lines, two whole
// screens, so no photograph can miss it -- and parks at EL0, leaving the live
// console on screen as the reading. A failure still exits with stage * 32 +
// errno, whose panic text is self-contained and needs no banner.
//
// Round 67 came back as a panel full of that line, parked: the tty paints, and
// fd 0 plus pagesize 4096 were the extra readings in it. Round 68 therefore
// spends the flood and asks the next wall real userland will hit: two user
// processes. After the banner the init forks; the child prints its own pid and
// ppid and exits with 7, the parent waits with wait4(-1) and prints the reaped
// pid plus the raw status, then parks. That one dance exercises the pmap copy
// (the child's first store is a copy-on-write fault), the scheduler picking the
// child up while the parent sleeps, and exit/wait4 reaping it -- and the status
// the parent prints decodes whatever happened instead: 1792 is exit(7) reached,
// a small number is the signal that killed the child, and code << 8 is the
// child's own stage * 32 + errno (stage 7 being its writes).
//
// Round 70 brought those five lines back and 1792 with them, so the process
// model works on this silicon: the pmap copy faulted, the second core picked the
// child up, the child exited, the parent woke and reaped it. Round 71 therefore
// replaces the child's exit(7) with the thing a real init does first:
// execve("/sbin/launchd"). On a mockfs root that path is this same image, so a
// successful exec is a child that reappears as a second copy of this program --
// only pid 1 forks now, and the second banner is the proof. It also moves the
// console descriptor: the exec preserves fd 0, so the fresh copy's own
// open("/dev/console") returns 1, and "console fd=1" on that line is the
// descriptor table surviving the swap. A refused exec prints its errno and
// exits with 9, which the parent reports as 2304.
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

  // "[clover] el0 init: pid=" … one line, once. Round 67 wrote it 240 times to
  // prove the tty paints; the flood is spent, and from here on a round prints
  // its evidence once and parks, leaving the live console on screen.
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

  // A failure leaves through _exit_now with stage * 32 + errno; the panic text
  // that follows it is self-contained and needs no banner on the panel.
  cbnz x23, _exit_now

  // Only pid 1 forks.
  //
  // The child of this round execs /sbin/launchd, and on a mockfs root that path
  // is this very image: an exec that works replaces the child's address space
  // with a second copy of this program, which starts again at _start with the
  // pid the kernel gave it. Without this guard that copy forks, its child execs,
  // and the pair multiplies until the process table is full -- a fork bomb whose
  // only evidence is a panel scrolling banners. Parking on "I am not pid 1" is
  // also the reading: a second banner line means the exec happened.
  cmp x19, #1
  b.ne _checkpoint

  // pid = fork()                                    (SYS_fork = 2)
  // The one question of round 68: can this kernel run two user processes on
  // this hardware? fork copies the pmap, so the child's first store is a
  // copy-on-write fault; the scheduler has to pick the child up while the
  // parent sleeps in wait4; exit and wait4 have to reap it again. Everything
  // real userland does later stands on those four, and none of them has been
  // on this silicon yet.
  //
  // Round 68 came back with that banner as the last line on the panel and nothing
  // under it: no child line, no waited line, no panic, no reset. That is three
  // different stories wearing the same photo -- fork never returned in the parent,
  // the child was created but never picked up by the scheduler, or the child ran
  // and hung before its first write landed. The tty paints a character the moment
  // it arrives, so even half a line would have been evidence. Round 69 therefore
  // split the fork point into three separate marks: the parent printed "forked
  // pid=" the instant fork returns, the child printed one word "alive" as its
  // very first instruction, and the wait4 line stayed where it was.
  //
  // Round 69 read two identical `[clover] el0 init: forked pid=2` lines, no alive
  // and no waited. That is not a kernel failure: it is our own ABI mistake, and the
  // kernel says so in two lines. thread_set_child (osfmk/arm64/status.c) hands the
  // child x0 = the child's pid and x1 = 1, and fork() in bsd/kern/kern_fork.c sets
  // retval[1] = 0 for the parent, so both processes retire the same svc with the
  // same pid in x0 and x1 is the only thing that tells them apart. libSystem's
  // fork() is what turns the child's x0 back into 0; a hand-written svc gets no
  // such service, so `cbnz x0, _parent` sent both processes down the parent path.
  // The child then called wait4(-1) with no children of its own, got ECHILD, and
  // fell into _fail -- which was reached by a branch, so its `ret` returned to a
  // stale x30 and the two instructions there formed a silent spin. That is why
  // round 68 showed nothing either: the same wrong branch, one round earlier.
  //
  // So the child is dispatched on x1 -- mov does not touch the carry flag, so the
  // syscall's error still reaches b.cs -- both processes print the x0/x1 pair they
  // were handed, and _fail now exits instead of returning: a failure that cannot
  // print is worth a panic with a number, and a failure that spins is worth
  // nothing at all.
  mov x16, #2
  svc #0x80
  mov x26, x0                         // both processes see the child's pid here
  mov x25, x1                         // 1 in the child, 0 in the parent
  mov x1, #160
  b.cs _fail
  cmp x25, #1
  b.ne _parent

  // Child: the descriptor table is inherited, so x22 is the console here too.
  // The first thing it does is write one word. The console tty paints each
  // character as it arrives, so this line also distinguishes a child that ran
  // and blocked mid-write (half a line on the panel) from one that was never
  // scheduled at all (nothing at all).
  adrp x1, _alive@PAGE
  add x1, x1, _alive@PAGEOFF
  adrp x2, _alive_end@PAGE
  add x2, x2, _alive_end@PAGEOFF
  sub x2, x2, x1
  bl _write_console
  mov x1, #224
  b.cs _fail
  mov x16, #20
  svc #0x80
  mov x1, #32
  b.cs _fail
  mov x19, x0
  mov x16, #39                        // getppid
  svc #0x80
  mov x1, #32
  b.cs _fail
  mov x20, x0

  adrp x1, _child_banner@PAGE
  add x1, x1, _child_banner@PAGEOFF
  adrp x2, _child_banner_end@PAGE
  add x2, x2, _child_banner_end@PAGEOFF
  sub x2, x2, x1
  bl _write_console
  mov x1, #224
  b.cs _fail
  mov x0, x19
  bl _print_dec
  mov x1, #224
  b.cs _fail

  adrp x1, _child_mid@PAGE
  add x1, x1, _child_mid@PAGEOFF
  adrp x2, _child_mid_end@PAGE
  add x2, x2, _child_mid_end@PAGEOFF
  sub x2, x2, x1
  bl _write_console
  mov x1, #224
  b.cs _fail
  mov x0, x20
  bl _print_dec
  mov x1, #224
  b.cs _fail

  // The two numbers the kernel actually handed this process. x0 equals getpid
  // and x1 is 1, which is the whole fork ABI reading in one line.
  adrp x1, _child_x0@PAGE
  add x1, x1, _child_x0@PAGEOFF
  adrp x2, _child_x0_end@PAGE
  add x2, x2, _child_x0_end@PAGEOFF
  sub x2, x2, x1
  bl _write_console
  mov x1, #224
  b.cs _fail
  mov x0, x26
  bl _print_dec
  mov x1, #224
  b.cs _fail

  adrp x1, _child_x1@PAGE
  add x1, x1, _child_x1@PAGEOFF
  adrp x2, _child_x1_end@PAGE
  add x2, x2, _child_x1_end@PAGEOFF
  sub x2, x2, x1
  bl _write_console
  mov x1, #224
  b.cs _fail
  mov x0, x25
  bl _print_dec
  mov x1, #224
  b.cs _fail

  adrp x1, _newline@PAGE
  add x1, x1, _newline@PAGEOFF
  adrp x2, _newline_end@PAGE
  add x2, x2, _newline_end@PAGEOFF
  sub x2, x2, x1
  bl _write_console
  mov x1, #224
  b.cs _fail

  // execve("/sbin/launchd", argv, envp)             (SYS_execve = 59)
  //
  // The next wall after "two processes" is "a process that becomes another one".
  // exec is what every real init does first, and on this port it has one extra
  // hurdle: the file is the root block device reinterpreted as a Mach-O, the
  // cdhash has to pass AMFI again, and the new address space has to be built
  // where the CoW'd one just was.
  //
  // Number 59 comes from this tree's bsd/kern/syscalls.master:
  //     59  AUE_EXECVE  ALL  { int execve(char *fname, char **argp, char **envp)
  //                             NO_SYSCALL_STUB; }
  // NO_SYSCALL_STUB does not mean the kernel lacks the call -- it means only
  // that no libSystem stub is generated for it -- and four of the syscalls this
  // program has already retired on real hardware (exit, fork, open, wait4) carry
  // exactly that marker.
  //
  // argv and envp are two NULL-terminated pointer arrays, and they are filled in
  // at run time rather than written as `.quad _exec_path`: this image links
  // -static, and an absolute address in the data section is a relocation the
  // linker would have to resolve for a load address it does not know.
  adrp x0, _argv@PAGE
  add x0, x0, _argv@PAGEOFF
  adrp x8, _exec_path@PAGE
  add x8, x8, _exec_path@PAGEOFF
  str x8, [x0, #0]
  str xzr, [x0, #8]
  adrp x0, _envp@PAGE
  add x0, x0, _envp@PAGEOFF
  str xzr, [x0, #0]

  // One word before the call, so a child that dies inside the exec (a killed
  // task, not an errno) is still distinguishable from one that never got there.
  adrp x1, _exec_try@PAGE
  add x1, x1, _exec_try@PAGEOFF
  adrp x2, _exec_try_end@PAGE
  add x2, x2, _exec_try_end@PAGEOFF
  sub x2, x2, x1
  bl _write_console
  mov x1, #224
  b.cs _fail

  adrp x0, _exec_path@PAGE
  add x0, x0, _exec_path@PAGEOFF
  adrp x1, _argv@PAGE
  add x1, x1, _argv@PAGEOFF
  adrp x2, _envp@PAGE
  add x2, x2, _envp@PAGEOFF
  mov x16, #59
  svc #0x80

  // Reaching here means the kernel refused: a successful exec never returns to
  // the caller's instructions, it returns to the new image's _start. The carry
  // flag says which of the two happened, and x0 is the errno on this path only.
  mov x26, x0
  adrp x1, _exec_fail@PAGE
  add x1, x1, _exec_fail@PAGEOFF
  adrp x2, _exec_fail_end@PAGE
  add x2, x2, _exec_fail_end@PAGEOFF
  sub x2, x2, x1
  bl _write_console
  mov x1, #224
  b.cs _fail
  mov x0, x26
  bl _print_dec
  mov x1, #224
  b.cs _fail
  adrp x1, _newline@PAGE
  add x1, x1, _newline@PAGEOFF
  adrp x2, _newline_end@PAGE
  add x2, x2, _newline_end@PAGEOFF
  sub x2, x2, x1
  bl _write_console
  mov x1, #224
  b.cs _fail

  // 9, not 7: round 70's reading was the parent printing 7 << 8 = 1792, and the
  // parent must not be able to mistake "the child exited after a refused exec"
  // for the old plan. 9 << 8 is 2304.
  mov x0, #9
  mov x16, #1
  svc #0x80
  b _checkpoint

_parent:
  mov x19, x26                      // the child's pid, as both processes saw it

  // fork gave the parent a number, so say so before sleeping in wait4. A panel
  // with this line and no alive line is a child that was never scheduled; a
  // panel with neither is a parent that never came back from fork itself. The
  // x1 here is the flag the kernel did not set for us, and it is the point of
  // the round: 0 on this line, 1 on the child's.
  adrp x1, _forked@PAGE
  add x1, x1, _forked@PAGEOFF
  adrp x2, _forked_end@PAGE
  add x2, x2, _forked_end@PAGEOFF
  sub x2, x2, x1
  bl _write_console
  mov x1, #128
  b.cs _fail
  mov x0, x19
  bl _print_dec
  mov x1, #128
  b.cs _fail

  adrp x1, _forked_mid@PAGE
  add x1, x1, _forked_mid@PAGEOFF
  adrp x2, _forked_mid_end@PAGE
  add x2, x2, _forked_mid_end@PAGEOFF
  sub x2, x2, x1
  bl _write_console
  mov x1, #128
  b.cs _fail
  mov x0, x25
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

  // wait4(-1, &_scratch, 0, 0)                      (SYS_wait4 = 7)
  mov x0, #-1
  adrp x1, _scratch@PAGE
  add x1, x1, _scratch@PAGEOFF
  mov x2, #0
  mov x3, #0
  mov x16, #7
  svc #0x80
  mov x1, #192
  b.cs _fail
  mov x20, x0                       // the reaped pid
  adrp x0, _scratch@PAGE
  add x0, x0, _scratch@PAGEOFF
  ldr x24, [x0]                     // raw wait status: exit code << 8, or signal

  adrp x1, _wait_banner@PAGE
  add x1, x1, _wait_banner@PAGEOFF
  adrp x2, _wait_banner_end@PAGE
  add x2, x2, _wait_banner_end@PAGEOFF
  sub x2, x2, x1
  bl _write_console
  mov x1, #128
  b.cs _fail
  mov x0, x20
  bl _print_dec
  mov x1, #128
  b.cs _fail

  adrp x1, _wait_mid@PAGE
  add x1, x1, _wait_mid@PAGEOFF
  adrp x2, _wait_mid_end@PAGE
  add x2, x2, _wait_mid_end@PAGEOFF
  sub x2, x2, x1
  bl _write_console
  mov x1, #128
  b.cs _fail
  mov x0, x24
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

  // Both processes did their whole dance: stay here with the three lines on
  // the panel. A parked parent behind its child's obituary is the reading.
  b _checkpoint

_exit_now:
  mov x0, x23
  mov x16, #1
  svc #0x80
  b _checkpoint

  // exit returned, which userland never does: park where a debugger (or a
  // future photo of a stuck panel) can find the registers.
_checkpoint:
  b _checkpoint

// Record the first failure only: later ones are consequences of the first, and
// a sum of stage codes would decode as a different stage.
// x0 = errno as returned by the syscall, x1 = that call's stage * 32.
//
// The exit, not a return: every call site reaches this by a branch, so there is
// no frame to go back to, and rounds 68 and 69 showed what a `ret` does here --
// x30 still points at the last helper call, the two instructions after it still
// see the failing carry flag, and the process spins without writing another
// character. A parked process is the one reading this port cannot tell apart
// from a dead one, so a failure leaves and lets the panic screen carry the code.
_fail:
  cbnz x23, _exit_now
  and x0, x0, #31
  add x23, x1, x0
  b _exit_now

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
_forked:
  .ascii "[clover] el0 init: forked pid="
_forked_end:
_forked_mid:
  .ascii " x1="
_forked_mid_end:
_alive:
  .ascii "[clover] el0 child: alive\n"
_alive_end:
_child_banner:
  .ascii "[clover] el0 child: pid="
_child_banner_end:
_child_mid:
  .ascii " ppid="
_child_mid_end:
_child_x0:
  .ascii " x0="
_child_x0_end:
_child_x1:
  .ascii " x1="
_child_x1_end:
_exec_path:
  .asciz "/sbin/launchd"
_exec_try:
  .ascii "[clover] el0 child: exec /sbin/launchd\n"
_exec_try_end:
_exec_fail:
  .ascii "[clover] el0 child: exec errno="
_exec_fail_end:
.balign 8
_argv:
  .space 16                         // { "/sbin/launchd", NULL }, filled in at run time
_envp:
  .space 8                          // { NULL }, same
_wait_banner:
  .ascii "[clover] el0 init: waited pid="
_wait_banner_end:
_wait_mid:
  .ascii " status="
_wait_mid_end:
_newline:
  .ascii "\n"
_newline_end:
.balign 8
_numbuf:
  .space 24
