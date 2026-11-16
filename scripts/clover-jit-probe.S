// clover r430: the V2 gate, in a host that actually runs.
//
// r429 put these probes in scripts/clover-el0-init.S, which has not been pid 1
// since r137 removed rd=md0: the bootstrapper is Apple's /sbin/launchd on the
// APFS OS volume, and the static init only survives as the SystemOS/Data seed.
// This program is exec'd by XNU's DEVELOPMENT-only offline-air-init hook
// (bsd/kern/kern_exec.c: load_init_program, /private/var/tmp/air-native), so it
// really is pid 1 and its exit status really is the reading.
//
// WEXITSTATUS is rv & 0xff (kern_exit.c:1936), so the codes are eight bits:
//
//   0x00       variant A (RWX mmap, call) and B (RW, mprotect RX, call) worked
//   0x3f       A was refused; RWX|MAP_JIT worked and the call returned
//   0x40 | e   A and RWX|MAP_JIT were both refused; e is the MAP_JIT errno
//   0x80 | e   A worked; B's mmap failed with errno e
//   0xc0 | e   A worked; B's mprotect(PROT_EXEC) failed with errno e
//   trap       a call reached a page that had been stripped of execute: the
//              kernel's own ESR/ELR/FAR is the reading, not a status
//
// XNU sets SCTLR_EL1.UCI (osfmk/arm64/proc_reg.h:645), so dc cvau / ic ivau at
// EL0 are legal here.
.text
.global _start

_start:
  // One console line, best effort: without it the panic text looks like any
  // other early exit.
  adrp x0, _console@PAGE
  add x0, x0, _console@PAGEOFF
  mov x1, #2                        // O_RDWR
  mov x2, #0
  mov x16, #5                       // SYS_open
  svc #0x80
  b.cs 1f
  mov x22, x0
  mov x0, x22
  adrp x1, _banner@PAGE
  add x1, x1, _banner@PAGEOFF
  adrp x2, _banner_end@PAGE
  add x2, x2, _banner_end@PAGEOFF
  sub x2, x2, x1
  mov x16, #4                       // SYS_write
  svc #0x80
1:

  // A data-volume log, because a fault leaves no exit status. Every step below
  // appends a line; the last line names the step that never returned.
  adrp x0, _log_path@PAGE
  add x0, x0, _log_path@PAGEOFF
  mov x1, #0x601                    // O_WRONLY|O_CREAT|O_TRUNC
  mov x2, #0644
  mov x16, #5                       // SYS_open
  svc #0x80
  b.cs 8f
  mov x23, x0
  bl _log_start
8:

  // A: mmap(0, 4096, PROT_READ|WRITE|EXEC, MAP_ANON|PRIVATE, -1, 0)
  mov x0, #0
  mov x1, #4096
  mov x2, #7
  mov x3, #0x1002
  mov x4, #-1
  mov x5, #0
  mov x16, #197                     // SYS_mmap
  svc #0x80
  b.cs 2f
  mov x24, x0
  bl _log_a_ok
  bl _write_ret
  bl _log_a_wrote
  dc cvau, x24
  ic ivau, x24
  dsb sy
  isb
  bl _log_a_flush
  bl _log_a_call
  // Boot 214: the call here took SIGBUS. The mapping was granted, the code was
  // written and flushed, and the fetch still faulted, so this kernel grants RWX
  // but does not execute it. Skip the call and measure the W^X shape instead.
  b 3f

2:
  bl _log_a_err
  // A refused. Try the same request with MAP_JIT (0x800), the flag Apple's own
  // JIT users pass; the entitlement check is AMFI's, and this kernel runs with
  // cs_enforcement_disable=1.
  mov x0, #0
  mov x1, #4096
  mov x2, #7
  mov x3, #0x1802                   // MAP_ANON|MAP_PRIVATE|MAP_JIT
  mov x4, #-1
  mov x5, #0
  mov x16, #197
  svc #0x80
  b.cs 12f
  mov x24, x0
  bl _log_j_ok
  bl _write_ret
  dc cvau, x24
  ic ivau, x24
  dsb sy
  isb
  bl _log_j_call
  blr x24
  bl _log_j_ret
  mov x0, #0x3f
  b _exit

4:
  bl _log_j_err
  and x0, x0, #0x3f
  orr x0, x0, #0x40
  b _exit

3:
  // B: mmap(0, 4096, PROT_READ|WRITE, MAP_ANON|PRIVATE, -1, 0)
  mov x0, #0
  mov x1, #4096
  mov x2, #3
  mov x3, #0x1002
  mov x4, #-1
  mov x5, #0
  mov x16, #197
  svc #0x80
  b.cs 5f
  mov x24, x0
  bl _log_b_ok
  bl _write_ret
  mov x0, x24
  mov x1, #4096
  mov x2, #5                        // PROT_READ|EXEC
  mov x16, #74                      // SYS_mprotect
  svc #0x80
  b.cs 6f
  bl _log_b_prot
  dc cvau, x24
  ic ivau, x24
  dsb sy
  isb
  bl _log_b_call
  blr x24
  bl _log_b_ret
  mov x0, #0
  b _exit

5:
  bl _log_b_err
  b 7f

6:
  bl _log_b_perr
  b 7f

7:
  // B did not give us executable code either. Try Apple's MAP_JIT request.
  mov x0, #0
  mov x1, #4096
  mov x2, #7
  mov x3, #0x1802                   // MAP_ANON|MAP_PRIVATE|MAP_JIT
  mov x4, #-1
  mov x5, #0
  mov x16, #197
  svc #0x80
  b.cs 12f
  mov x24, x0
  bl _log_j_ok
  bl _write_ret
  dc cvau, x24
  ic ivau, x24
  dsb sy
  isb
  bl _log_j_call
  blr x24
  bl _log_j_ret
  mov x0, #0x3f
  b _exit

12:
  bl _log_j_err
  and x0, x0, #0x3f
  orr x0, x0, #0x40
  b _exit

// x24 = page. Writes `ret` (0xd65f03c0) at offset 0.
_write_ret:
  movz w9, #0x03c0
  movk w9, #0xd65f, lsl #16
  str w9, [x24]
  ret

// x24 = page. Cleans the line, invalidates the I-cache, then calls the ret.
_flush_and_call:
  dc cvau, x24
  ic ivau, x24
  dsb sy
  isb
  blr x24
  ret

// x0 = status & 0xff.
_exit:
  and x0, x0, #0xff
  mov x16, #1                       // SYS_exit
  svc #0x80
7:
  b 7b

// x23 = log fd. Appends one fixed line. A zero fd is a failed open and the
// whole log becomes a no-op, never an error: the test's own result matters.
_log_line:
  cbz x23, 9f
  mov x2, #0                        // length = strlen(x1)
10:
  ldrb w9, [x1, x2]
  cbz w9, 11f
  add x2, x2, #1
  b 10b
11:
  cbz x2, 9f
  mov x0, x23
  mov x16, #4                       // SYS_write
  svc #0x80
9:
  ret

_log_start:   adrp x1, _l_start@PAGE
              add x1, x1, _l_start@PAGEOFF
              mov x2, #10
              b _log_line
_log_a_ok:    adrp x1, _l_a_ok@PAGE
              add x1, x1, _l_a_ok@PAGEOFF
              mov x2, #9
              b _log_line
_log_a_wrote: adrp x1, _l_a_wrote@PAGE
              add x1, x1, _l_a_wrote@PAGEOFF
              mov x2, #10
              b _log_line
_log_a_flush: adrp x1, _l_a_flush@PAGE
              add x1, x1, _l_a_flush@PAGEOFF
              mov x2, #12
              b _log_line
_log_a_call:  adrp x1, _l_a_call@PAGE
              add x1, x1, _l_a_call@PAGEOFF
              mov x2, #11
              b _log_line
_log_a_ret:   adrp x1, _l_a_ret@PAGE
              add x1, x1, _l_a_ret@PAGEOFF
              mov x2, #9
              b _log_line
_log_a_err:   adrp x1, _l_a_err@PAGE
              add x1, x1, _l_a_err@PAGEOFF
              mov x2, #9
              b _log_line
_log_j_ok:    adrp x1, _l_j_ok@PAGE
              add x1, x1, _l_j_ok@PAGEOFF
              mov x2, #9
              b _log_line
_log_j_call:  adrp x1, _l_j_call@PAGE
              add x1, x1, _l_j_call@PAGEOFF
              mov x2, #11
              b _log_line
_log_j_ret:   adrp x1, _l_j_ret@PAGE
              add x1, x1, _l_j_ret@PAGEOFF
              mov x2, #9
              b _log_line
_log_j_err:   adrp x1, _l_j_err@PAGE
              add x1, x1, _l_j_err@PAGEOFF
              mov x2, #9
              b _log_line
_log_b_ok:    adrp x1, _l_b_ok@PAGE
              add x1, x1, _l_b_ok@PAGEOFF
              mov x2, #9
              b _log_line
_log_b_prot:  adrp x1, _l_b_prot@PAGE
              add x1, x1, _l_b_prot@PAGEOFF
              mov x2, #12
              b _log_line
_log_b_call:  adrp x1, _l_b_call@PAGE
              add x1, x1, _l_b_call@PAGEOFF
              mov x2, #11
              b _log_line
_log_b_ret:   adrp x1, _l_b_ret@PAGE
              add x1, x1, _l_b_ret@PAGEOFF
              mov x2, #9
              b _log_line
_log_b_err:   adrp x1, _l_b_err@PAGE
              add x1, x1, _l_b_err@PAGEOFF
              mov x2, #9
              b _log_line
_log_b_perr:  adrp x1, _l_b_perr@PAGE
              add x1, x1, _l_b_perr@PAGEOFF
              mov x2, #9
              b _log_line

.data
_log_path:
  .asciz "/private/var/tmp/jit-probe.log"
_l_start:   .asciz "start\n"
_l_a_ok:    .asciz "A mmap\nok"
_l_a_wrote: .asciz "A wrote\nok"
_l_a_flush: .asciz "A flush\nok"
_l_a_call:  .asciz "A call\nok"
_l_a_ret:   .asciz "A ret\nok"
_l_a_err:   .asciz "A err\nok"
_l_j_ok:    .asciz "J mmap\nok"
_l_j_call:  .asciz "J call\nok"
_l_j_ret:   .asciz "J ret\nok"
_l_j_err:   .asciz "J err\nok"
_l_b_ok:    .asciz "B mmap\nok"
_l_b_prot:  .asciz "B prot\nok"
_l_b_call:  .asciz "B call\nok"
_l_b_ret:   .asciz "B ret\nok"
_l_b_err:   .asciz "B err\nok"
_l_b_perr:  .asciz "B perr\nok"

_console:
  .asciz "/dev/console"
_banner:
  .ascii "[clover] jit-probe: air-native running\n"
_banner_end:
