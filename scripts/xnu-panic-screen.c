/* Draw a kernel panic message, with its arguments, on the frame buffer, mark the
 * board's restart reason so XBL comes back up in fastboot, then reset.
 * With no usable vinfo yet (panic before the video console exists) there is
 * nothing to draw on, so that case resets after twelve seconds instead; a panic
 * after arm_vm_init dropped the bootstrap identity map, where the seeded vinfo
 * address no longer translates, resets after three seconds.
 *
 * Taken from the sheng port (UniXNU/UniXNU-sheng, scripts/xnu-panic-screen.c) and
 * built by this repository's scripts/patch-xnu-entry-probe.py. The layout of the
 * kernel image is board-specific; this file is not.
 *
 * Built freestanding by patch-xnu-entry-probe.py and placed in unused padding
 * of a COPY of the kernel; panic() and its common panic funnel are redirected
 * here. It runs on the kernel's
 * own mapping of the frame buffer. With the board observer it uses the retained
 * GOP mapping prefix; otherwise it uses vinfo.v_baseaddr.
 *
 * panic(fmt, ...) is variadic and the Darwin arm64 ABI passes every variadic
 * argument in an 8-byte stack slot, starting at the stack pointer on entry.
 * A small printf walks the format string over those slots (%s %x %p, with
 * 0-flag, width, precision and l/ll/z). Kernel strings are checked with an
 * active hardware translation query before reading each byte;
 * unmapped/device/user pointers print as pointers. VINFO names that mapping prefix.
 *
 * Below the message it prints RA, FP and LR (three registers that lead back to
 * the call site), the format pointer, the argument pointer and the raw slots
 * around it. A photo of that block identifies the panic's caller even when the
 * argument list does not look like the source would suggest.
 */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

struct vc_info {
  u32 v_height, v_width, v_depth, v_rowbytes;
  u64 v_baseaddr;
  u32 v_type;
};

/* Written into v_type by the entry probe's vinfo seed. It marks a console whose
 * frame buffer is reached through the kernel's identity map. */
#define VINFO_SEED 0xC10C10C1u

#define SCALE   3

extern const u64 gFont[];

/* All state lives on the stack: this code sits in the kernel's read-only text,
 * so a writable global would fault on its first store. */
struct ctx {
  u8  *base;
  u32 rb, w, h, x, y;
  u8  *log;   /* buffer that also receives the formatted screen text, 0 = none */
  u32 logn;
  u32 logcap;
  u32 only_log; /* draw into the log copy and skip the panel; used for the record's own preface */
};

#ifndef PANIC_TEXT
#define PANIC_TEXT 0UL
#endif
#define PANIC_TEXT_BYTES 256

/* The board may give the panic screen a place to put text that outlives the panic. On clover this
 * is drivers/CloverSdhci's clover_emmc_panic_append, resolved by the patcher: the screen hands it
 * everything it drew, the driver appends it to logfs as a record, and the next boot reads it back.
 * 0 means this build has no sink and the screen behaves exactly as before. */
#ifndef PANIC_SINK
#define PANIC_SINK 0UL
#endif
#define PANIC_LOG_BYTES 480

/* Round147: the unattended contract. drivers/CloverSdhci maps IMEM once per boot
 * (clover-unattended=<seconds>), proves the restart-reason cell reads back, and
 * publishes its address through this global. The patcher resolves the global's
 * address here, not the cell's: the cell is volatile (never written to eMMC) and
 * only exists once that driver has run, so a panic before it leaves the pointer
 * zero and the board resets normally instead of writing to a guess.
 * 0x77665501 is "bootloader" in the low cell XBL reads on this board; the stock
 * kernel writes the same word for `reboot bootloader`. */
#ifndef FASTBOOT_REASON
#define FASTBOOT_REASON 0UL
#endif
#define FASTBOOT_WORD 0x77665501u
/* How long the finished screen stays up before PSCI SYSTEM_RESET. The record is
 * already on the card by then; this is time for a photograph. */
#define PANIC_RESET_SECONDS 20

/* Round125 asks the panic screen what the console itself was doing. The patcher resolves these four
 * kernel addresses the same way it resolves the counters above; a kernel whose symbol table lacks one
 * leaves the macro at 0 and the corresponding line is drawn as dashes. */
#ifndef CONSOLE_RING
#define CONSOLE_RING 0UL
#endif
#ifndef KM_TTY
#define KM_TTY 0UL
#endif
#ifndef DIS_CONS
#define DIS_CONS 0UL
#endif
#ifndef CONS_SUSP
#define CONS_SUSP 0UL
#endif

/* console_ring (osfmk/console/serial_console.c:62), read off this kernel's own disassembly of
 * _console_ring_try_empty: it loads len at +0x8, used at +0xc, nreserved at +0x10, read_ptr at
 * +0x20, takes flush_lock at +0x28 and write_lock at +0x38, and _console_suspended sits at +0x48. */
#define RING_BUFFER      0x00
#define RING_LEN         0x08
#define RING_USED        0x0c
#define RING_NRESERVED   0x10
#define RING_WRITE       0x18
#define RING_READ        0x20
#define RING_FLUSH       0x28
#define RING_WLOCK       0x38

/* struct tty, likewise from _ttwrite/_tty_lock in this image: c_cc at +0x80, t_state at +0xc0,
 * t_hiwat/t_lowat/t_timeout at +0x1cc/+0x1d0/+0x1d4 (msleep0's timo argument is the last of those),
 * and t_locked_thread at +0x1e8. */
#define TTY_CC              0x80
#define TTY_STATE           0xc0
#define TTY_HIWAT           0x1cc
#define TTY_LOWAT           0x1d0
#define TTY_TIMEOUT         0x1d4
#define TTY_LOCKED_THREAD   0x1e8

/* The one TS_* bit (bsd/sys/tty.h) this round asks about by name. */
#define TS_SO_OLOWAT        0x1u

__attribute__((noreturn)) static void park (void)
{
  for (;;) {
    __asm__ volatile ("wfe");
  }
}

/* The seeded console is reached through the kernel's identity map, which is
 * Normal-WB, so the display engine does not see the pixels until they are
 * cleaned to the point of coherency. A vinfo filled by vc_init describes a
 * device mapping and needs none of this. */
static void clean_seeded (volatile struct vc_info *v)
{
  u64 p = v->v_baseaddr;
  u64 end = p + (u64)v->v_rowbytes * v->v_height;

  for (; p < end; p += 64) {
    __asm__ volatile ("dc civac, %0" : : "r" (p) : "memory");
  }
  __asm__ volatile ("dsb sy" : : : "memory");
}

static int mapped (u64 a);

/* Ask the board for fastboot on the next reset: write FASTBOOT_WORD into the
 * restart-reason cell the driver published, then read it back. Returns 1 only
 * when the cell actually holds the word afterwards, which is what the screen's
 * fb= line reports. Both addresses are queried first: FASTBOOT_REASON lives in
 * the driver's data, which may not be mapped if the panic happened before that
 * driver initialized the VM, and the cell lives in IMEM outside the kernel map. */
static int fastboot_mark (void)
{
  volatile u32 *cell;

  if (FASTBOOT_REASON == 0UL || !mapped (FASTBOOT_REASON)) {
    return 0;
  }
  cell = *(volatile u32 *volatile *)FASTBOOT_REASON;
  if ((u64)cell == 0 || !mapped ((u64)cell)) {
    return 0;
  }
  *cell = FASTBOOT_WORD;
  __asm__ volatile ("dsb sy" : : : "memory");
  __asm__ volatile ("dmb sy" : : : "memory");
  return *cell == FASTBOOT_WORD;
}

/* Wait `seconds` (on the 19.2 MHz counter) and reboot through PSCI. The reason
 * is marked before the wait, so the reset that follows is a fastboot reset even
 * when the caller could not draw (reset_unseen/reset_unmapped). */
__attribute__((noreturn)) static void reset_after (u64 seconds)
{
  u64 deadline;

  (void)fastboot_mark ();
  __asm__ volatile ("mrs %0, cntpct_el0" : "=r" (deadline));
  deadline += seconds * 19200000ULL;
  for (;;) {
    u64 now;
    __asm__ volatile ("isb\n\tmrs %0, cntpct_el0" : "=r" (now));
    if (now >= deadline) {
      break;
    }
  }
  __asm__ volatile ("movz w0, #0x0009\n\t"
                    "movk w0, #0x8400, lsl #16\n\t"
                    "mov x1, xzr\n\t"
                    "mov x2, xzr\n\t"
                    "mov x3, xzr\n\t"
                    "smc #0");
  park ();
}

/* vinfo stays zero until the kernel's own video console exists, so a panic in
 * the first calls of arm_init cannot draw. Report that case by time instead:
 * twelve seconds then PSCI SYSTEM_RESET, which is distinguishable from the
 * checkpoint reset and proves the stop was a panic, not a spin. */
__attribute__((noreturn)) static void reset_unseen (void)
{
  reset_after (12);
}

/* Does EL1 currently translate this address writably? AT reports a fault in
 * PAR_EL1.F instead of raising one. */
static int mapped (u64 a)
{
  u64 par;
  __asm__ volatile ("at s1e1w, %1\n\tisb\n\tmrs %0, par_el1" : "=r" (par) : "r" (a) : "memory");
  return !(par & 1);
}

/* arm_vm_init installs the kernel's own translation tables, and the seeded
 * vinfo address is a physical address that only the bootstrap identity map
 * translated. Once that map is gone, every pixel of this screen would fault and
 * turn the panic into a silent freeze. Report it by time instead: three
 * seconds, which the ten-second arm_vm_init checkpoint reset cannot mimic. */
__attribute__((noreturn)) static void reset_unmapped (void)
{
  reset_after (3);
}

static void put (struct ctx *g, u8 c)
{
  const u32 cell = 6 * SCALE, line = 14 * SCALE;
  u32 row, col, dy, dx;

  if (g->log && g->logn < g->logcap && c >= 9 && c <= 126) {
    /* Newlines are kept: this copy becomes a text file on the card. Other control bytes have no
     * meaning to a reader of that file and are stored as one space. */
    g->log[g->logn++] = (c == '\n') ? '\n' : (c >= 32) ? c : ' ';
    g->log[g->logn] = 0;
  }
  if (g->only_log) {
    return;
  }

  if (c == '\n') {
    g->x = 0;
    g->y += line;
    return;
  }
  if (c == '\t') {
    g->x += 4 * cell;
    return;
  }
  if (c < 32 || c > 126) {
    return;
  }
  if (g->x + cell > g->w) {
    g->x = 0;
    g->y += line;
  }
  if (g->y + line > g->h) {
    return;
  }
  u64 glyph = gFont[c - 32];
  u32 top = (u32)(glyph >> 32), bottom = (u32)glyph;
  for (row = 0; row < 12; row++) {
    u32 data = (row < 6) ? (top >> (row * 5)) : (bottom >> ((row - 6) * 5));
    for (col = 0; col < 5; col++) {
      if ((data >> col) & 1) {
        for (dy = 0; dy < SCALE; dy++) {
          for (dx = 0; dx < SCALE; dx++) {
            *(volatile u32 *)(g->base + (u64)(g->y + row * SCALE + dy) * g->rb + (u64)(g->x + col * SCALE + dx) * 4) = 0xFFFFFFFFu;
          }
        }
      }
    }
  }
  g->x += cell;
}

static void number (struct ctx *g, u64 value, u32 base, u32 width, int zero)
{
  u8  digits[24];
  u32 n = 0, i;

  do {
    u32 d = (u32)(value % base);
    digits[n++] = (u8)(d < 10 ? '0' + d : 'a' + d - 10);
    value /= base;
  } while (value && n < 24);
  for (i = n; i < width; i++) {
    put (g, zero ? '0' : ' ');
  }
  while (n) {
    put (g, digits[--n]);
  }
}

/* Probe each string byte before dereferencing it. Heap strings (process name
 * and exit reason) live outside the linked image. AT performs a hardware
 * translation query only: no table edits, mappings, or exception bypass.
 * Refuse user pointers, translation faults and device-memory attributes. */
static int readable (const char *s, const u64 *args)
{
  u64 a = (u64)s, par;
  (void)args;
  if (a < 0xffff000000000000UL) return 0;
  __asm__ volatile ("at s1e1r, %1\n\tisb\n\tmrs %0, par_el1" : "=r" (par) : "r" (a) : "memory");
  return !(par & 1) && ((par >> 60) != 0);
}

static void format (struct ctx *g, const char *fmt, const u64 *args)
{
  u32 i, used = 0;

  for (i = 0; i < 2000 && fmt[i]; i++) {
    u8 c = (u8)fmt[i];
    if (c != '%') {
      put (g, c);
      continue;
    }
    u32 width = 0;
    int zero = 0, longs = 0;
    c = (u8)fmt[++i];
    if (c == '0') {
      zero = 1;
      c = (u8)fmt[++i];
    }
    while (c >= '0' && c <= '9') {
      width = width * 10 + (c - '0');
      c = (u8)fmt[++i];
    }
    u32 precision = 4000;
    if (c == '.') {
      precision = 0;
      c = (u8)fmt[++i];
      if (c == '*') {
        precision = (u32)args[used++];
        c = (u8)fmt[++i];
      } else {
        while (c >= '0' && c <= '9') {
          if (precision < 4000) precision = precision * 10 + (c - '0');
          c = (u8)fmt[++i];
        }
      }
      if (precision > 4000) precision = 4000;
    }
    if (width > 256) width = 256;
    while (c == 'l' || c == 'z' || c == 'h') {
      longs += (c != 'h');
      c = (u8)fmt[++i];
    }
    if (c == '%') {
      put (g, '%');
      continue;
    }
    if (c == 0) {
      break;
    }
    u64 v = args[used++];
    if (c == 's') {
      const char *s = (const char *)v;
      u32 k;
      for (k = 0; k < precision; k++) {
        if (!readable (s + k, args)) {
          put (g, '<');
          number (g, (u64)(s + k), 16, 0, 0);
          put (g, '>');
          break;
        }
        u8 ch = (u8)s[k];
        if (!ch) break;
        put (g, ch);
      }
    } else if (c == 'x' || c == 'X') {
      number (g, longs ? v : (v & 0xFFFFFFFFu), 16, width, zero);
    } else if (c == 'u' || c == 'd') {
      number (g, longs ? v : (v & 0xFFFFFFFFu), 10, width, zero);
    } else if (c == 'p') {
      put (g, '0');
      put (g, 'x');
      number (g, v, 16, width, zero);
    }
  }
}

static void text (struct ctx *g, const char *s)
{
  while (*s) {
    put (g, (u8)*s++);
  }
}

/* Round195: the card keeps only the first PANIC_LOG_BYTES (480) bytes of this text, and the panel's own
 * fb= row is drawn after every reading -- so round194's deadline record carried no fb= at all, and
 * whether that reset reached fastboot was again a question only a photograph gone twenty seconds later
 * could answer. The same reading goes into the log copy as the record's second line, where it fits;
 * the panel keeps its fb= row at the bottom for a photograph. */
static void logonly (struct ctx *g, const char *label, u64 v)
{
  g->only_log = 1;
  text (g, label);
  number (g, v, 10, 0, 0);
  g->only_log = 0;
}

static void hex (struct ctx *g, const char *label, u64 v)
{
  text (g, label);
  put (g, '0');
  put (g, 'x');
  number (g, v, 16, 16, 1);
}

/* One raw argument slot, read only after a translation query. A slot whose
 * address does not translate is drawn as dashes, so a bad va_list still
 * leaves a screen instead of a fault. */
static void slot (struct ctx *g, const char *label, const u64 *args, int index)
{
  u64 address = (u64)(args + index);

  text (g, label);
  if (!readable ((const char *)address, 0)) {
    text (g, "----------------");
    return;
  }
  put (g, '0');
  put (g, 'x');
  number (g, *(volatile const u64 *)address, 16, 16, 1);
}

/* One kernel field, read only after a translation query, in the same spirit as slot(): an address
 * that does not translate is drawn as dashes instead of faulting, so a stale tty pointer still
 * leaves a readable screen. size is 1, 4 or 8. */
static void word (struct ctx *g, const char *label, u64 address, u32 size)
{
  text (g, label);
  if (address == 0 || (address & (size - 1)) || !readable ((const char *)address, 0)) {
    text (g, "----------------");
    return;
  }
  put (g, '0');
  put (g, 'x');
  if (size == 8) {
    number (g, *(volatile const u64 *)address, 16, 16, 1);
  } else if (size == 4) {
    number (g, *(volatile const u32 *)address, 16, 16, 1);
  } else {
    number (g, *(volatile const u8 *)address, 16, 16, 1);
  }
}

/* x0 = fmt as for panic(); x1 = the first variadic slot; x2 = the caller's LR
 * (x30 on the panic() hook, x6 on the trap funnel); x3 = the caller's frame
 * pointer, whose saved LR names the assert call site that came before. */
__attribute__((noreturn, used, section(".text.b_main"))) void panic_screen_main (const char *fmt, const u64 *args, u64 caller, u64 frame)
{
  volatile struct vc_info *v = (volatile struct vc_info *)VINFO;
  struct ctx g;
  u8 logbuf[PANIC_LOG_BYTES + 1];
  u32 row, col, fb;

  __asm__ volatile ("msr daifset, #15");
  g.base = (u8 *)v->v_baseaddr;
  g.rb   = v->v_rowbytes;
  g.w    = v->v_width;
  g.h    = v->v_height;
  if (g.base == 0 || v->v_depth != 32 || fmt == 0) {
    reset_unseen ();
  }
  if (!mapped ((u64)g.base)) {
    reset_unmapped ();
  }
  /* Mark before drawing anything: a fault later in this function must still
   * leave the board pointed at fastboot, and the reset paths below mark again. */
  fb = fastboot_mark () ? 1 : 0;

  /* First panic wins: a marker in the last row's first two pixels means a panic already drew
   * the screen; a later panic (another thread or exception) must not overwrite it -- but it
   * must not freeze the board either, so it only brings the reset forward. */
  volatile u32 *mark = (volatile u32 *)(g.base + (u64)(g.h - 1) * g.rb);
  if (mark[0] == 0xC0FFEE11u && mark[1] == 0x5CEEDA7Au) {
    reset_after (PANIC_RESET_SECONDS);
  }
  g.x = 0;
  g.y = g.h / 8;
  g.log = logbuf;
  g.logcap = PANIC_LOG_BYTES;
  g.logn = 0;
  g.only_log = 0;
  /* Clear the middle of the screen only: the entry probe's trace bands live in
   * the lower half and must survive the panic for a photograph. */
  for (row = g.y; row < g.h / 2; row++) {
    for (col = 0; col < g.w; col++) {
      *(volatile u32 *)(g.base + (u64)row * g.rb + (u64)col * 4) = 0;
    }
  }
  g.y += 8;
  /* The board's live banner keeps showing this text after the next swap paints over the screen (BootProgress.c,
   * boot_progress_panic_text). The first panic's text is kept: a buffer that already holds one is left alone. */
  if (PANIC_TEXT && *(volatile u8 *)PANIC_TEXT == 0) {
    g.log = (u8 *)PANIC_TEXT;
    g.logcap = PANIC_TEXT_BYTES - 1;
  }
  format (&g, fmt, args);
  logonly (&g, "\nfb=", fb);
  __asm__ volatile ("dsb sy");
  /* Round125's question, drawn ahead of every other reading: twice in a row the board wedged inside
   * one write(2) to /dev/console and the clock watchdog's panic took the panel, so the screen now
   * reports what the console layer looked like at that moment. The ring words say whether a writer
   * was staging bytes (used, nreserved) or a drainer was left waiting for reservations to close;
   * fl= and wt= are the two locks console_ring_try_empty holds while it spins -- _lck_mtx_try_lock
   * stores the holder's cpu number in the mutex word, so a non-zero fl= names it and 0 means free
   * (or held by cpu 0, which this image cannot tell apart), and the ticket is free only when its
   * tix and ticket bytes agree. dis= and susp= are the two flags that make console_write() return
   * without storing anything: the other way a write can finish and still print nothing. */
  {
    u64 tp = 0;
    if (KM_TTY != 0 && readable ((const char *)KM_TTY, 0)) {
      tp = *(volatile const u64 *)KM_TTY;
    }
    word (&g, "\nring len=", CONSOLE_RING + RING_LEN, 4);
    word (&g, " used=", CONSOLE_RING + RING_USED, 4);
    word (&g, " nres=", CONSOLE_RING + RING_NRESERVED, 4);
    word (&g, "\nbuf=", CONSOLE_RING + RING_BUFFER, 8);
    word (&g, " wr=", CONSOLE_RING + RING_WRITE, 8);
    word (&g, " rd=", CONSOLE_RING + RING_READ, 8);
    word (&g, "\nfl=", CONSOLE_RING + RING_FLUSH + 8, 4);
    word (&g, " wt=", CONSOLE_RING + RING_WLOCK + 8, 4);
    word (&g, " dis=", DIS_CONS, 4);
    word (&g, " susp=", CONS_SUSP, 1);
    /* state= carries the TS_* bits; TS_SO_OLOWAT (0x1) with cc= at the queue's 1024-byte capacity is
     * the shape of a writer parked in ttysleep waiting for a wakeup that km.c never sends, and to= is
     * that sleep's timo -- 0 forever. owner= is the thread that holds tty_lock, which is the answer to
     * whether anyone is inside kmwrite at all. */
    word (&g, "\ntty=", KM_TTY, 8);
    word (&g, " state=", tp + TTY_STATE, 4);
    word (&g, " cc=", tp + TTY_CC, 4);
    word (&g, "\nlow=", tp + TTY_LOWAT, 4);
    word (&g, " hi=", tp + TTY_HIWAT, 4);
    word (&g, " to=", tp + TTY_TIMEOUT, 4);
    word (&g, " owner=", tp + TTY_LOCKED_THREAD, 8);
    if (tp != 0 && readable ((const char *)(tp + TTY_STATE), 0)) {
      /* Ask the bit directly: decoding a hex t_state by hand off a photo is error-prone. */
      text (&g, (*(volatile const u32 *)(tp + TTY_STATE) & TS_SO_OLOWAT) ? "\nOLOWAT yes" : "\nOLOWAT no");
    }
  }
  /* Resolved from this candidate's symbols by the patcher, never fixed addresses.
   * Values are sampled at panic time and need not form an atomic VM snapshot. */
  (void)MAX_MEM;
  hex (&g, "\npages=", *(volatile const u32 *)PAGE_TOTAL);
  hex (&g, "\nfree=", *(volatile const u32 *)PAGE_FREE);
  /* The clock, in the kernel's own counts, because no process on this board has ever been able to report
   * one: round77 measured that no timed wait anywhere -- in EL0 or in the kernel -- ever returns. ticks=0
   * next to intid=19 (the INTID clover's device tree gives the EL1 virtual timer) would mean the line is
   * armed and raised but never reaches ICC_IAR1_EL1, which is a different fault from a hung console.
   * typer is GICD_TYPER as the driver read it back at boot: how many INTIDs this GIC implements. */
  hex (&g, "\nticks=", *(volatile const u64 *)ARM_TIMER);
  hex (&g, "\nipi=", *(volatile const u64 *)ARM_IPI);
  hex (&g, " sent=", *(volatile const u64 *)ARM_IPI_SENT);
  hex (&g, "\nirq=", *(volatile const u32 *)ARM_IRQ_ID);
  hex (&g, " phase=", *(volatile const u32 *)ARM_IRQ_PHASE);
  hex (&g, " intid=", *(volatile const u32 *)ARM_TIMER_INTID);
  hex (&g, " typer=", *(volatile const u32 *)(ARM_GIC_BOOT + 3 * 4));
  /* Round83: drivers/CloverPMGR publishes an ApplePMGRNub before AMFI's thread-call deadline
   * can fire, and records every step in these sixteen slots. stage is the step it reached, so
   * a panic here that never got to publish is distinguishable from the AMFI one by sight.
   * read=-> is the original AON pair that AMFI reads through slot 320 then writes through 321. */
  hex (&g, "\npmgr=", *(volatile const u64 *)PMGR_OBS);
  hex (&g, " status=", *(volatile const u64 *)(PMGR_OBS + 8));
  hex (&g, " mode=", *(volatile const u64 *)(PMGR_OBS + 120));
  hex (&g, "\nnub=", *(volatile const u64 *)(PMGR_OBS + 40));
  hex (&g, " attached=", *(volatile const u64 *)(PMGR_OBS + 104));
  hex (&g, " registered=", *(volatile const u64 *)(PMGR_OBS + 112));
  hex (&g, "\naon=", *(volatile const u64 *)(PMGR_OBS + 56));
  hex (&g, "=>", *(volatile const u64 *)(PMGR_OBS + 64));
  /* The panel is a fixed height and characters that would land below its last row are
   * dropped silently, so the readings this board asked for are drawn first and the
   * caller evidence last: round82's photograph lost its final two lines (intid= and
   * typer=) exactly that way, and this round adds three more of its own. */
  /* Caller evidence. The panic screen can only show what the formatter read,
   * so it also prints the registers that lead back: RA is the LR on entry
   * (in _Assert: its return into panic()), FP the caller's frame pointer, and
   * LR the return address saved in that frame, which names the function that
   * called _Assert. The slots around the va_list make a shifted argument
   * list visible instead of silently wrong. */
  hex (&g, "\nRA=", caller);
  hex (&g, "\nFP=", frame);
  {
    u64 saved = 0;
    if (frame != 0 && readable ((const char *)(frame + 8), 0)) {
      saved = *(volatile u64 *)(frame + 8);
    }
    hex (&g, "\nLR=", saved);
  }
  hex (&g, "\nfmt=", (u64)fmt);
  hex (&g, "\nargs=", (u64)args);
  slot (&g, "\nm2=", args, -2);
  slot (&g, " m1=", args, -1);
  slot (&g, "\na0=", args, 0);
  slot (&g, " a1=", args, 1);
  slot (&g, "\na2=", args, 2);
  slot (&g, " a3=", args, 3);
  slot (&g, "\na4=", args, 4);
  slot (&g, " a5=", args, 5);
  /* Sample the active CPU translation regime, without changing it. This is
   * panic-time evidence only; it does not establish early mappings or EL0 ABI. */
  {
    u64 tcr, sctlr;
    __asm__ volatile ("mrs %0, tcr_el1" : "=r" (tcr));
    __asm__ volatile ("mrs %0, sctlr_el1" : "=r" (sctlr));
    hex (&g, "\nTCR=", tcr);
    hex (&g, "\nSCTLR=", sctlr);
  }
  /* Whether the reset that follows will land in fastboot. 1 means the reason
   * cell read back FASTBOOT_WORD, 0 means no cell this boot (the driver did not
   * run) and the reset is an ordinary one. Round194 draws it before the sink, so
   * the record on the card carries it: round193's two deadline panics were the
   * same image and only the second was reported to have come back to fastboot,
   * and a reading that exists only on a panel gone twenty seconds later cannot
   * say which one it was. */
  text (&g, "\nfb=");
  number (&g, fb, 10, 0, 0);
  /* Everything above was also copied into logbuf. Hand that copy to the board's sink -- the eMMC
   * driver appends it to a logfs record -- and draw its verdict. 0 means this build has no sink;
   * 1 means the sink declined (the card was never selected); 100 + slot means both blocks of that
   * slot were written; 2..5 name a scan or write that failed. elog= is drawn after the copy is
   * detached, because that one really is the record's outcome about itself. */
  if (PANIC_SINK) {
    u8 *capture = g.log;
    u32 captured = g.logn;
    u32 written;
    g.log = 0;
    written = ((u32 (*) (const char *, u32)) PANIC_SINK) ((const char *)capture, captured);
    text (&g, "\nelog=");
    number (&g, written, 10, 0, 0);
  }
  /* The raw argument slots are not dumped: the formatted message above already carries them (room for the PANIC_TEXT copy). */
  mark[0] = 0xC0FFEE11u;
  mark[1] = 0x5CEEDA7Au;
  __asm__ volatile ("dsb sy");
  if (v->v_type == VINFO_SEED) {
    clean_seeded (v);
  }
  reset_after (PANIC_RESET_SECONDS);
}

/* Entry from panic(): keep the stack pointer, which addresses the variadic
 * arguments, and hand over the caller's LR and frame pointer before the C
 * prologue can overwrite them. */
__attribute__((naked, noreturn, section(".text.a_entry"))) void panic_screen (const char *fmt)
{
  __asm__ volatile ("mov x2, x30\n\tmov x3, x29\n\tmov x1, sp\n\tb panic_screen_main");
}

/* The real panic_trap_to_debugger signature takes (fmt, va_list *, ...).
 * Darwin ARM64 va_list is a stack pointer; its address is passed in x1.
 * The wrapper has already established its frame, so SP is NOT the argument
 * list here. Follow exactly the one pointer supplied by the real wrapper.
 */
__attribute__((naked, noreturn, section(".text.a_common"))) void panic_screen_common (void)
{
  __asm__ volatile ("mov x2, x6\n\tmov x3, x29\n\tldr x1, [x1]\n\tb panic_screen_main");
}

#include "Font.h"
__attribute__((section(".text.z_font"))) const u64 gFont[] = GLYPH_FONT;
