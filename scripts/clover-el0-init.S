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
// Round 71 came back with exactly that: six lines, the last one the new image's
// own banner with fd 1, and no waited line, because the exec'd child never
// exited. So namei, the mockfs read, the second AMFI check and the address space
// swap all work. What has still never been attempted from EL0 on this hardware
// is a mach trap -- and launchd is nothing but a mach message loop, so initproc
// cannot be replaced until one retires. v0.2.42 therefore asks that one
// question, in the exec'd copy only: three self ports (thread_self, task_self,
// host_self), a reply port made from nothing, and one plain message sent to it
// and received back. Each step prints its own line, so the panel says where the
// IPC path stopped, and the copy then exits with 11 so the parent's waited line
// confirms the probe finished rather than the round merely running out of
// screen.
//
// Round 72 came back with the four port names -- thread, task, host and a reply
// port made from nothing -- and then silence in the middle of the mach_msg send,
// with the parent reading status 9. Round 73 added markers on both sides of that
// send and passed allow_legacy_mach_msg=1 to open the legacy-trap policy gate the
// source blamed, and the panel was identical: presend, no send=. v0.2.44 therefore
// stops arguing with that one call and asks the question of the API Apple
// intends: mach_msg2_trap, index 47, whose send leg never enters the legacy
// policy at all. Its packed eight-register form is worked out in the probe
// below, along with the two option checks it does have to pass -- and because
// failing either of them is the same silent SIGKILL, a panel that shows the
// return code after it is proof by itself. The legacy call is retried at the
// very end, after msg2 has answered, so that whichever of the two dies, the
// other one's reading survives on the screen.
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

  // The mach probe, in the exec'd copy only.
  //
  // Round 71 proved a process can replace itself. What it did not prove is the
  // thing launchd actually is: a mach message server. Every launchd syscall is
  // either a BSD call or a mach trap, and this process is unusual in the first
  // place -- it was not posix_spawn'ed by a parent, it is the task
  // bsd_utaskbootstrap built, reached through our own fork+exec. Whether its IPC
  // space answers at all is untested, and if it does not, nothing above initproc
  // can ever run.
  //
  // The ABI is not the BSD one and this tree says so. handle_svc
  // (osfmk/arm64/sleh.c) reads x16 through get_saved_state_svc_number, which
  // truncates it to a *signed int*: negative goes to mach_syscall, non-negative
  // to unix_syscall. mach_syscall (osfmk/arm64/bsd_arm64.c) then does
  // call_number = -call_number and indexes mach_trap_table, so x16 must hold the
  // negated table position: thread_self is /* 27 */, task_self /* 28 */,
  // host_self /* 29 */, mach_reply_port /* 26 */, mach_msg /* 31 */.
  //
  // And the return is not the BSD return either: arm_set_mach_syscall_ret writes
  // x0 and nothing else -- no x1, and the carry flag is never touched, so the
  // b.cs habit from the BSD path would read a stale flag here. For the four
  // traps marked .mach_trap_returns_port the value in x0 is the port name
  // itself, and MACH_PORT_NULL is how they report failure.
  cmp x19, #1
  b.eq _skip_mach

  mov x16, #-27                       // thread_self
  svc #0x80
  mov x24, x0
  mov x16, #-28                       // task_self
  svc #0x80
  mov x21, x0                         // pagesize was in x21 and has been printed
  mov x16, #-29                       // host_self
  svc #0x80
  mov x26, x0

  adrp x1, _mach_self@PAGE
  add x1, x1, _mach_self@PAGEOFF
  adrp x2, _mach_self_end@PAGE
  add x2, x2, _mach_self_end@PAGEOFF
  sub x2, x2, x1
  bl _write_console
  mov x1, #224
  b.cs _fail

  // Three non-zero names is the whole answer to "is this task's IPC space on".
  mov x0, x24
  bl _print_dec
  mov x1, #224
  b.cs _fail
  adrp x1, _mach_task@PAGE
  add x1, x1, _mach_task@PAGEOFF
  adrp x2, _mach_task_end@PAGE
  add x2, x2, _mach_task_end@PAGEOFF
  sub x2, x2, x1
  bl _write_console
  mov x1, #224
  b.cs _fail
  mov x0, x21
  bl _print_dec
  mov x1, #224
  b.cs _fail
  adrp x1, _mach_host@PAGE
  add x1, x1, _mach_host@PAGEOFF
  adrp x2, _mach_host_end@PAGE
  add x2, x2, _mach_host_end@PAGEOFF
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

  // A port to talk to, with no parent and no namespace: mach_reply_port takes
  // zero arguments and hands back a fresh receive right in this task's space.
  mov x16, #-26
  svc #0x80
  mov x25, x0
  adrp x1, _mach_reply@PAGE
  add x1, x1, _mach_reply@PAGEOFF
  adrp x2, _mach_reply_end@PAGE
  add x2, x2, _mach_reply_end@PAGEOFF
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
  cbz x25, _mach_done                 // no right: there is nothing to send to

  // The message buffer lives in x20, not x7, for the whole probe.
  //
  // The buffer used to sit in x7 because x7 was not an argument of the legacy
  // mach_msg trap and the trap had no reason to touch it. mach_msg2_trap takes
  // eight arguments (/* 47 */ MACH_TRAP(mach_msg2_trap, 8, 16, munge_llllllll),
  // osfmk/kern/syscall_sw.c:154) and its eighth is the timeout, so x7 is now
  // live input and the pointer has to move somewhere the trap never reads: x20,
  // which _write_console (x0/x16/x22) and _print_dec (x27-x29, x9-x11) leave
  // alone and x19 is already spent on the pid.
  adrp x20, _msgbuf@PAGE
  add x20, x20, _msgbuf@PAGEOFF
  bl _mheader                         // x20 buffer, x25 reply port

  // Round 73 kept every marker and still came back empty-handed: msg=<address>,
  // presend, and no send= line, with the parent's wait4 at status 9 again. The
  // boot-arg did not open the gate.
  //
  // What v0.2.43 assumed was that ipc_policy_allow_legacy_send_trap() (osfmk/ipc
  // /ipc_policy.c:269) is what fired, and that allow_legacy_mach_msg=1 would
  // short-circuit it. Both halves of that story have now been checked against
  // the bytes that are actually running on this tablet, and both hold up:
  //
  //   - the kernel is DEVELOPMENT_ARM64_BCM2837 (out/clover/logs/kernel.log), and
  //     objdump of out/clover/kernel/kernel.macho at _ipc_policy_allow_legacy_
  //     send_trap+0x20 reads the tunable and branches on bit 0:
  //         tbnz x1, #0x37, <guard>           // MACH64_POLICY_HARDENED, bit 55
  //         adrp x8, 0xfffffff007158000
  //         ldrb w8, [x8, #0x1a8]             // _allow_legacy_mach_msg
  //         tbnz w8, #0x0, <return 0>         // boot-arg branch, compiled in
  //     so #if DEVELOPMENT || DEBUG is true here and the knob is live code;
  //   - the argument string in the payload really is
  //         -v serial=3 debug=0x14e kt_fixed_zones=120 boot-progress=2 rd=md0 \
  //         allow_legacy_mach_msg=1
  //     and PE_boot_args() (pexpert/arm/pe_bootargs.c:11) hands back exactly
  //     ((boot_args *)PE_state.bootArgs)->CommandLine, which is the buffer
  //     DarwinBoot fills -- not the device tree: PE_init_platform
  //     (pexpert/arm/pe_init.c:416) sets bootArgs straight from the booter's
  //     args and never looks for a chosen/boot-args property. rd=md0 arriving as
  //     "BSD root: md0, major 3, minor 0" is the proof of that channel;
  //   - the parser handles a trailing token of the form name=1: the name scan
  //     stops at '=' so c == '=' and the "no value" shortcut at bootargs.c:126
  //     does not fire, getval() eats the '=' (bootargs.c:416) and returns NUM
  //     with val=1, argnumcpy writes one int8_t (bootargs.c:380), and
  //     kernel_startup_tunable_init normalises it to a bool (startup.c:291). The
  //     token is 21 characters, the same length as the name, which is what the
  //     strncmp-plus-length test at bootargs.c:105 wants.
  //
  // So the legacy gate should have been open, and the process still died inside
  // the send. That is a contradiction, and it means the kill's origin is not
  // established -- so the gate is no longer treated as the answer, and the round
  // is designed so the panel says which of the two readings is the real one.
  //
  // mach_msg2_trap is the call to make: it is the one Apple intends userspace to
  // use, and it never walks the legacy path at all. mach_msg_copyin_user_header,
  // the only caller of ipc_policy_allow_legacy_send_trap, lives inside
  // #if IPC_HAS_LEGACY_MACH_MSG_TRAP (mach_msg.c:555-617) and is reached only
  // from mach_msg_overwrite_trap (mach_msg.c:859); mach_msg2_trap synthesizes
  // its send_uctx straight out of the packed arguments (mach_msg.c:972-1010).
  //
  // msg2 has two gates of its own, and both kill silently rather than return,
  // which is what makes the reading self-proving:
  //
  //   - ipc_preflight_msg_option64 (ipc_policy.c:144): with SEND_MSG and
  //     MACH64_MACH_MSG2, opts & MACH64_MSG_OPTION_CFI_MASK must have exactly
  //     one bit set (SEND_ANY is masked out on non-macOS builds), and
  //   - ipc_validate_kmsg_dest_from_user (ipc_policy.c:612): the destination
  //     must match the CFI bit -- a message queue, which is what a reply port
  //     is, requires MACH64_SEND_MQ_CALL.
  //
  // Each one ends in mach_port_guard_exception(..., kGUARD_EXC_INVALID_OPTIONS),
  // and 3 < MAX_FATAL_kGUARD_EXC_CODE (mach/port.h:526), so a wrong option word
  // is the same silent SIGKILL as round 72 and 73. Get the options right and the
  // trap answers with a mach_msg_return_t instead: which is precisely why a
  // panel that shows m2send=<number> proves EL0 got past every policy layer, with
  // no boot-arg and no whitelist involved.
  //
  // The packing is struct mach_msg2_trap_args (osfmk/mach/mach_traps.h:465) read
  // through MACH_MSG2_SHIFT_ARGS(lo, hi) = hi << 32 | lo (message.h:1505), and
  // mach_msg2_trap's own unpacking (mach_msg.c:937-942, 972-1010, 1020-1045)
  // fixes which half of each register means what:
  //
  //   x0 data (the buffer), x1 options, x2 msgh_bits | send_size << 32,
  //   x3 msgh_remote_port | msgh_local_port << 32,
  //   x4 msgh_voucher_port | msgh_id << 32,
  //   x5 desc_count | rcv_name << 32, x6 rcv_size | priority << 32, x7 timeout.
  //
  // Note what that means for the buffer: the header fields of a msg2 send come
  // from the registers, not from memory. ipc_kmsg_get_and_inflate_from_user
  // (ipc_kmsg.c:2953) copies the user buffer in from byte 24 onward, so the
  // first three words written by _mheader below are what the *legacy* call reads
  // and nothing else. They stay filled anyway, because the receive copies its
  // result back over them and the legacy test needs to know it is being handed
  // the header it expects rather than the kernel's leftovers.
  // "[clover] el0 mach: msg=<address>": where the buffer actually is.
  adrp x1, _mach_msg@PAGE
  add x1, x1, _mach_msg@PAGEOFF
  adrp x2, _mach_msg_end@PAGE
  add x2, x2, _mach_msg_end@PAGEOFF
  sub x2, x2, x1
  bl _puts
  mov x0, x20
  bl _print_line

  // ---- the question of this round: mach_msg2_trap, index 47 ----
  //
  // "m2 presend" goes out before the kernel is entered, so from here on every
  // missing line is a kill, and after v0.2.43's analysis a kill can only be a
  // guard the option word or the destination provoked -- not the legacy gate.
  adrp x1, _m2_presend@PAGE
  add x1, x1, _m2_presend@PAGEOFF
  adrp x2, _m2_presend_end@PAGE
  add x2, x2, _m2_presend_end@PAGEOFF
  sub x2, x2, x1
  bl _puts

  mov x0, x20                         // data: the message buffer
  mov x1, #1                          // MACH_SEND_MSG
  movk x1, #4, lsl #32                // | MACH64_SEND_MQ_CALL (4 << 32): a reply
                                      // port is a message queue, and
                                      // ipc_validate_kmsg_dest_from_user
                                      // (ipc_policy.c:686) demands the bit match
  mov x2, #20                         // msgh_bits = MACH_MSGH_BITS(MAKE_SEND, 0)
  movk x2, #24, lsl #32               // | send_size << 32
  mov w3, w25                         // msgh_remote_port, and the local half --
                                      // which is msgh_local_port, i.e. where a
                                      // reply would go -- stays MACH_PORT_NULL
  mov x4, #0                          // voucher 0 | msgh_id 0
  mov x5, #0                          // desc_count 0 | rcv_name 0
  mov x6, #0                          // rcv_size 0 | priority 0
  mov x7, #0                          // timeout: nothing to wait for on a send
  mov x16, #-47
  svc #0x80
  mov x21, x0                         // mach_msg_return_t of the send leg

  // If this line appears at all, the option64 survived both preflight
  // (ipc_policy.c:144: exactly one CFI bit) and the destination check, and what
  // follows it is a real return code: 0 = the message is queued, 268435459 =
  // MACH_SEND_INVALID_DEST, 268435475 = MACH_SEND_INVALID_OPTIONS, and anything
  // in the 268-million range means the trap answered instead of killing us.
  adrp x1, _m2_send@PAGE
  add x1, x1, _m2_send@PAGEOFF
  adrp x2, _m2_send_end@PAGE
  add x2, x2, _m2_send_end@PAGEOFF
  sub x2, x2, x1
  bl _puts
  mov x0, x21
  bl _print_line

  adrp x1, _m2_prercv@PAGE
  add x1, x1, _m2_prercv@PAGEOFF
  adrp x2, _m2_prercv_end@PAGE
  add x2, x2, _m2_prercv_end@PAGEOFF
  sub x2, x2, x1
  bl _puts

  // A pure receive needs no CFI bit: ipc_preflight_msg_option64 only asks for
  // one when MACH_SEND_MSG is set (ipc_policy.c:148). rcv_name is the high half
  // of x5 and rcv_size the low half of x6, per mach_msg2_trap's own unpacking at
  // mach_msg.c:1021 and :1044.
  mov x0, x20                         // data: the answer is copied here
  mov x1, #0x102                      // MACH_RCV_MSG | MACH_RCV_TIMEOUT
  mov x2, #0
  mov x3, #0
  mov x4, #0
  mov w5, w25
  lsl x5, x5, #32                     // rcv_name = the reply port
  mov x6, #48                         // rcv_size = the whole buffer, header included
  mov x7, #3000                       // timeout in ms: a stuck receive must not eat the round
  mov x16, #-47
  svc #0x80
  mov x21, x0                         // the receive's mach_msg_return_t
  ldr w24, [x20, #4]                  // msgh_size as it came back

  // rcv=0 size=24 is the round trip: one message made from nothing, queued to
  // this task's own port, and pulled back out of it. Anything else in rcv= is
  // still an answer -- 268451842 = MACH_RCV_INVALID_NAME, 268451843 =
  // MACH_RCV_TIMED_OUT, 268451844 = MACH_RCV_TOO_LARGE -- and 268451843 would
  // mean the send queued the message somewhere other than the port we are
  // reading.
  adrp x1, _m2_rcv@PAGE
  add x1, x1, _m2_rcv@PAGEOFF
  adrp x2, _m2_rcv_end@PAGE
  add x2, x2, _m2_rcv_end@PAGEOFF
  sub x2, x2, x1
  bl _puts
  mov x0, x21
  bl _print_dec
  mov x1, #224
  b.cs _fail
  adrp x1, _mach_size@PAGE
  add x1, x1, _mach_size@PAGEOFF
  adrp x2, _mach_size_end@PAGE
  add x2, x2, _mach_size_end@PAGEOFF
  sub x2, x2, x1
  bl _puts
  mov x0, x24
  bl _print_line

  // ---- the second question, asked last because only its death is informative
  //
  // The legacy trap, index 31, with the same header and the same destination as
  // round 73, once the msg2 pair has already been answered. lghsend= with a
  // number after it means allow_legacy_mach_msg=1 was honored and round 73's
  // death was something else entirely; lghpresend as the last line on the panel
  // means the gate is still firing, and then the tunable -- not the policy, not
  // the table, not the signature -- is the thing that is not doing what this
  // tree's source says it does.
  bl _mheader                         // the receive overwrote the buffer
  adrp x1, _lgh_presend@PAGE
  add x1, x1, _lgh_presend@PAGEOFF
  adrp x2, _lgh_presend_end@PAGE
  add x2, x2, _lgh_presend_end@PAGEOFF
  sub x2, x2, x1
  bl _puts

  mov x0, x20
  mov x1, #1                          // MACH_SEND_MSG, and nothing else
  mov x2, #24                         // bytes to send: the header
  mov x3, #0
  mov x4, #0
  mov x5, #0                          // timeout
  mov x6, #0                          // priority
  mov x16, #-31
  svc #0x80
  mov x21, x0

  adrp x1, _lgh_send@PAGE
  add x1, x1, _lgh_send@PAGEOFF
  adrp x2, _lgh_send_end@PAGE
  add x2, x2, _lgh_send_end@PAGEOFF
  sub x2, x2, x1
  bl _puts
  mov x0, x21
  bl _print_line

_mach_done:
  // Leave, so the parent's wait4 line confirms the probe ran to the end rather
  // than the panel merely showing the last line it got to. 11 is not a stage
  // code: every stage is a multiple of 32 plus an errno, so the parent's status
  // of 11 << 8 = 2816 can only mean "the exec'd copy finished its mach probe".
  mov x0, #11
  mov x16, #1
  svc #0x80

_skip_mach:

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

// x1 = bytes, x2 = length: write them and check the carry in one go, so a line
// of labels does not have to repeat the two-instruction test after every call.
_puts:
  bl _write_console
  mov x1, #224
  b.cs _fail
  ret

// x0 = value: decimal, then the newline. Two writes, one failure stage, and a
// return to the line that asked for it -- _print_dec tail-branches into
// _write_console, so its `ret` lands here rather than one frame deeper.
_print_line:
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
  ret

// The 24-byte mach_msg_header_t this round sends, written into the buffer at x20
// with the reply port at x25 as its destination. Used twice: once before the
// msg2 pair, so the buffer is a real message even though msg2 takes its header
// from registers, and once again before the legacy call, because the receive
// copied the kernel's answer over the first one and the legacy trap reads its
// header from memory (osfmk/ipc/mach_msg.c:587).
//
// msgh_bits is MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0) = 20: making a send
// right out of nothing requires holding the receive right, and the reply port is
// exactly that (osfmk/mach/message.h). msgh_local_port stays MACH_PORT_NULL --
// this message has no reply to receive, the receiver is us, through the same
// port. msgh_id 0 is deliberate: it is not on the legacy whitelist
// (ipc_policy.c:257, which admits only task_threads, task_info,
// thread_get_state, mach_vm_read and mach_vm_read_overwrite), so if the
// boot-arg really is live the legacy call has one way to say so.
_mheader:
  mov w8, #20
  str w8, [x20, #0]                   // msgh_bits
  mov w8, #24
  str w8, [x20, #4]                   // msgh_size
  str w25, [x20, #8]                  // msgh_remote_port
  str wzr, [x20, #12]                 // msgh_local_port
  str wzr, [x20, #16]                 // msgh_voucher_port
  str wzr, [x20, #20]                 // msgh_id
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
_mach_self:
  .ascii "[clover] el0 mach: thread="
_mach_self_end:
_mach_task:
  .ascii " task="
_mach_task_end:
_mach_host:
  .ascii " host="
_mach_host_end:
_mach_reply:
  .ascii "[clover] el0 mach: reply="
_mach_reply_end:
_mach_msg:
  .ascii "[clover] el0 mach: msg="
_mach_msg_end:
_m2_presend:
  .ascii "[clover] el0 m2: presend\n"
_m2_presend_end:
_m2_send:
  .ascii "[clover] el0 m2: send="
_m2_send_end:
_m2_prercv:
  .ascii "[clover] el0 m2: prercv\n"
_m2_prercv_end:
_m2_rcv:
  .ascii "[clover] el0 m2: rcv="
_m2_rcv_end:
_mach_size:
  .ascii " size="
_mach_size_end:
_lgh_presend:
  .ascii "[clover] el0 legacy: presend\n"
_lgh_presend_end:
_lgh_send:
  .ascii "[clover] el0 legacy: send="
_lgh_send_end:
.balign 8
_argv:
  .space 16                         // { "/sbin/launchd", NULL }, filled in at run time
_envp:
  .space 8                          // { NULL }, same
_msgbuf:
  .space 48                         // 24-byte mach_msg_header_t, then room for what comes back
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
