// SPDX-License-Identifier: BSD-2-Clause-Patent
// clover (Mi Pad 4, SDM660): the eMMC host driver, one question per boot argument.
//
// WHY THIS FILE EXISTS IN THIS SHAPE
//
// The tablet boots from eMMC. Everything the kernel has read so far -- the kernel itself, the
// prelinked image, the AMFI trust cache, the root ramdisk, the flat device tree -- came out of
// that card, through the host at 0xc0c4000, under a driver this port cannot read: the firmware's
// only SDHCI driver is `SdccDxe`, shipped as a binary blob (uefi-clover/Binaries/clover/QcomPkg/
// Drivers/SdccDxe/SdccDxe.inf). So there is no init sequence to copy, no known-good clock tree,
// no known-good tuning state. What there is, is a controller that was demonstrably working a few
// milliseconds before the kernel took over, and a card that was demonstrably left in transfer
// state by it.
//
// That was the thing worth one photograph: **does the host still answer, and what state did the
// firmware leave it in?** Round87 answered it -- 8-bit bus, 1.8 V, 200 MHz base clock with the
// divider at 0, the SDCC v5 DLL locked, the last firmware command a CMD12 after a multi-block
// read. Mode 1, the dump that asked it, stays: it is the control whenever a later reading looks
// wrong. Mode 2 asked the next question, and it did have to write: whether a command issued from
// the kernel is answered at all. Round88 answered it -- CMD13 to RCA 1 came back as the
// controller's own timeout, with no CRC error and no index error, i.e. nothing drove the CMD
// line. The command word was right, so the two candidates left are a different RCA and a card
// that is not in a state which answers. Mode 3 asked which RCA: the same three writes, once per
// address. Round89 got silence from 1..15 and 0, so either the RCA is above 15 or the host stopped
// clocking the card after handoff. Mode 4 stops asking and re-identifies the card from CMD0.
//
// Mode 1 therefore issues no commands, sets no clock, touches no reset line, enables no
// interrupt, and writes nothing anywhere. It maps one 4 KiB window, reads, formats, and panics.
// Modes 2 and 3 write three registers -- INT_STATUS (write-1-to-clear), ARGUMENT and COMMAND --
// for CMD13 SEND_STATUS and nothing else. Mode 4 is the first one allowed to change state: it
// resets the command/data part of the host, drops the card clock to 392 kHz, and issues CMD0,
// CMD1, CMD2 and CMD3. It touches no power rail and no bus width, and it moves no data.
//
// WHY IT RUNS FROM sheng_pmgr_run()
//
// An in-kernel UniXNU driver normally starts from a personality compiled into
// iokit/KernelConfigTables.cpp by a patch (docs/drivers/xnu-drivers.md:14, :149;
// docs/guide/build-kernel.md:135). Until round314 this paragraph claimed a device repository could not
// add one. Round314 measured that and it is false: 73 clover patches are tracked in the base
// repository right now (`git ls-files patches/xnu | grep -c clover` = 73 against ~/UniXNU), and the
// assembled tree links both out/clover/tree/patches/xnu -> the base directory and
// out/clover/tree/patches/xnu-clover -> this repository's. What is actually true is narrower, and is
// why the two directories carry different names: the kernel applies the *base* series, so a clover
// patch has to be committed there to run, and this repository keeps the copy that gets reviewed
// (a device-side directory named `patches/xnu` would shadow the upstream set rather than join it --
// the wall round86 hit from the other side, for osfmk/console).
//
// Three concrete things, not a blocked mechanism, are what keep this driver off the documented path
// (recorded as notes/unixnu-doc-conformance.md §5):
//
//   1. There is no provider to match. scripts/make-clover-adt.py:316-321 publishes exactly one
//      /arm-io child, intc, so IOPlatformDevice + IONameMatch (docs/drivers/device-tree-matching.md
//      :20-28) would match nothing for sdhci, spmi, gpu, i2c or dsp on this board.
//   2. Probe order cannot express what this port needs. IOProbeScore orders candidates
//      (docs/drivers/device-tree-matching.md:151-159); it cannot say "the DSP census reads a heap
//      whose rings the block driver has to have armed first", which is why CloverPMGR chains
//      emmc -> input -> power -> dsp.
//   3. The archives are linked without -Wl,-force_load. docs/drivers/xnu-drivers.md:128-145 asks for
//      it and the base script appends CONFIG_XNU_EXTRA_LD_ARCHIVES raw
//      (build-xnu-4k-baseline.sh:1002), so a class nobody calls -- which is precisely what a
//      personality-started driver is -- would be dropped by the linker. Round314 measured the
//      consequence today: every Clover object's own exported symbols are in the linked kernel, so
//      nothing of ours is lost; check-clover.py now checks that instead of trusting it.
//
// So the driver exports `clover_emmc_run()` and CloverPMGR calls it at the end of
// `sheng_pmgr_run()`, the bsd_init.c hook this board has already been running through since
// round83 (CONFIG_XNU_SHENG_PMGR, after bsd_bufferinit, with IOKit up and the panic screen
// armed). That is a scheduling decision, not a design one: closing it means the three items above,
// in that order, and none of them is missing an upstream permission.
//
// WHAT IT DOES NOT CLAIM
//
// No block device, no IOMedia, no root volume, and no read of a single byte of user data. The
// next rounds add those, in this order, one flash each: get an answer at all (mode 4: CMD0 twice
// to separate a gated clock from a silent card, then CMD1/CMD2/CMD3 to identify the card and own
// its RCA), then one block with FIFO data (CMD17 at LBA 1, whose first five bytes are the GPT
// magic "EFI PART" -- an answer that checks itself), then whatever the log-in-a-partition plan
// needs of it.
//
// CMD13 is addressed to a relative card address, and the firmware's RCA is not recorded anywhere
// this port has found. Round88 asked 1 (the Linux mmc core's fixed choice) and got silence; an
// eMMC whose RCA does not match simply does not drive the line, which is indistinguishable from
// a dead card until a different address is tried. EDK2's SdMmcPciHcDxe computes `Rca = Slot + 1`
// (EmmcDevice.c, EmmcIdentification), so slot 0 is 1; Qualcomm's own stack could use anything, and
// the SD spec allows 1..65535. Round89 swept 1..15 and 0 and got silence from all sixteen, so a
// sweep cannot settle this: mode 4 stops asking and re-identifies the card instead.
//
// Mode 4 then proved that both CMD0 probes complete, at the firmware's clock and after writing the
// SDHCI 10-bit divider -- and that CMD1 still times out. sdhci-msm.c:1766 is the missing fact:
// this controller has no internal divider ("MSM controller do not use clock divider"), its card
// clock is the GCC root clock, and the firmware left that root clock at the 200 MHz it used for
// HS200. 200 MHz is not an identification clock, and div=255 was a number no hardware read. Mode 5
// changes the clock where the hardware takes it: the SDM660 GCC sdcc1_apps_clk_src
// (gcc-sdm660.c:747) is programmed to the table's own 400 kHz row (gcc-sdm660.c:735), the host's
// vendor clock mux is pointed back at the default MCLK (msm_hc_select_default, sdhci-msm.c:779),
// the host is reset to legacy timing, and CMD0/CMD1/CMD2/CMD3 start over.
//
// Mode 5's photograph split the result in two: the GCC half worked (cfg=00002017, m=01 n=fc d=fb,
// up=1 -- the RCG took the 400 kHz row and the update bit self-cleared), but the host half never
// ran, because RESET_ALL never self-cleared (rst=0) and every command was skipped. It was written
// while the firmware's HS200 card clock was still running. Mode 6 stops that clock first
// (CLOCK_CONTROL = 0, the state MSM's set_clock(0) leaves), resets only the command and data
// machines -- the pair round90 already saw self-clear -- and then does the GCC and legacy host
// bring-up unchanged. `rst=` and `clr=` in the report are the two readings that separate
// "the reset bit is the problem" from "the clock will not stop".
//
// Mode 6's photograph is the first one with a live card: `clk=0007` before, `clr=0002` after the
// stop (INT_EN and CARD_EN both clear, so the HS200 card clock really was what held RESET_ALL),
// `rst=1`, GCC still at 400 kHz, `cmd0 ok`, and `cmd1 ok t=3 to=0 rsp=c0ff8080` -- the card's own
// OCR, bit 31 raised on the third poll. CMD2, the first command whose response the host checks,
// came back as `crc` with RESPONSE[0] = 0, and CMD3 was skipped.
//
// Mode 7 asks CMD2 twice, because the two answers point at different hardware. A is mode 6's
// command byte for byte, and its raw INT_STATUS and all four response words are printed instead
// of a lumped verdict. B is the same command with the CRC check off: if the CID arrives intact,
// the host's CRC gate is what failed and identification can go on; if B also fails, the response
// path itself is broken and the next round is about sampling, not about CMD2. CMD2 is legal only
// in the ready state, and A moves the card to identification even when the host rejects the
// answer, so B is preceded by CMD0 and the CMD1 poll loop again. CMD3 is sent whenever either
// attempt reached the card: an R1 that passes index+CRC checks separates "the response path is
// corrupt" from "only the 136-bit CRC check disagrees".
//
// Mode 7's photograph answered half of that: A failed with INT_STATUS 0x00028000 (error + CRC),
// and its four response words were the *previous* CMD1 OCR -- a rejected command latches nothing;
// B with the CRC check off returned the whole CID (int=0x00000001) and CMD3 was accepted. So the
// one failing part is the host's CRC7 check on CMD2's response. But A and B differed in two
// things, the check and the settle time after CMD1's ready bit (B came after CMD0 and a
// three-poll CMD1), and round93 cannot say which one mattered. Mode 8 swaps them: X is CMD2 with
// the check off in A's slot, immediately after ready; Y is CMD2 with the check on in B's slot,
// after the same re-arm. Then CMD3, CMD9 (SEND_CSD, the second 136-bit response, check on) and
// CMD13 at the assigned RCA. X ok + Y ok  => round93's A was early and the check itself is fine.
// X ok + Y crc => not a settle-time effect: the check fails on the CID at this clock.
// X crc/to     => the first slot is bad whatever the check says; the re-arm is what fixed B.
// CMD9's verdict then separates "every R2 fails the check" from "only the CID does".
//
// Mode 8's photograph picked the second row: X (check off, first slot) returned the whole CID with
// int=0x00000001, and Y (check on, after the re-arm) failed with the same 0x00028000 as round93's
// A. So the settle time does not matter; the CRC gate does. X's CID was byte-identical to round93's
// B on a second boot. Two more facts came with that photograph. Y's words were no longer the
// previous OCR: RESPONSE[1..0] and RESPONSE[2]'s low three bytes were the true CID tail (88 bits)
// and only RESPONSE[2]'s top byte plus all of RESPONSE[3] were the old OCR shifted along -- the
// 136-bit frame did arrive and shift in, and the controller simply stops latching when the check
// fails. And the CMD3 that followed that rejected frame timed out (int=0x00018000), although the
// code did reset the command circuit first (0x06), so a failed 136-bit response leaves the command
// path unusable for the next command in a way RESET_CMD does not repair. Mode 9 never takes that
// path: CMD2 with the check off, CMD3 to take RCA 1, CMD13 to see stand-by, CMD9 with the check on
// to ask whether the gate rejects only the CID or every R2, CMD7 to select the card, CMD13 again
// to see transfer. Each step is gated on the one before it, so the first failure names the next
// question.
//
// Mode 9's photograph answered the whole chain: c2 ok, c3 ok st=2, c13 ok st=3, c9 ok (CSD with the
// check on), c7 ok, c13b ok st=4 -- the card is in transfer for the first time. Against the stock
// dump in facts/81ffcaaa-20261022-210928/mmc_device.txt, the captured CSD shifted left 8 bits
// (SDHCI drops the CRC byte of a 136-bit response) is bit-identical to the card's real CSD, and the
// captured CID differs in exactly one bit (0xb6 where the stock CID has 0x36). So the gate is
// right, the CID is the only frame that ever mismatches, and the data commands do not need it.
//
// Mode 10 is therefore mode 9 minus CMD9, plus the first data command this port has ever sent:
// CMD16 (SET_BLOCKLEN 512, non-fatal -- 512 is already the reset length), then CMD17 READ with
// PIO and a single 512-byte block from byte 512, which on a GPT disk is LBA 1: the header that
// starts with "EFI PART". Two words of the FIFO and an efi flag are the reading.
//
// Mode 10's photograph: c2/c3/c7/c13/c16 all ok, then `c17 nodata int=00000021 ps=01f80af6 bs=0200`
// -- the response was accepted and DATA_AVAIL fired, but DATA_END never set, the two FIFO words
// were zero, and PRESENT_STATE still showed DATA_INHIBIT, DOING_READ, DATA_AVAILABLE and DAT3..0
// high. The direction is right; the card is not delivering. Two host-side differences from Linux's
// own path on this controller are visible in its sources. sdhci-msm does not set
// SDHCI_QUIRK2_SUPPORT_SINGLE, so Linux always sets BLK_CNT_EN even for one block
// (sdhci.c:1483), while mode 10 sent TRANSFER_MODE 0x10 alone. And the OCR has bit 30
// (rsp=c0ff8080), so mmc core marks the card block-addressed and the block layer passes the LBA
// through unshifted (ds-mmc.c:2092, ds-block.c:1661), while mode 10 asked for byte 512 -- LBA 512.
// Mode 11 applies both and keeps every step as a reading: CMD16 still sent so a rejection is seen,
// the FIFO drained only while PRESENT_STATE's live level says a word is there (INT_STATUS's
// DATA_AVAIL is a sticky event this driver never acknowledges, which is why `int=00000021` still
// carried it after two reads), the word count and sum printed so "zeros" and "nothing sent" cannot
// be confused, and -- if DATA_END still does not come -- CMD12 then CMD13, because the stop command
// is the one command Linux lets past DATA_INHIBIT (sdhci.c:1675). The data phase at 392 kHz lasts
// about 10 ms, hence the same bounded polls as everywhere else.
//
// Mode 11's photograph moved the failure to this driver's own pace. c17 came back `nodata` again
// (int=00000021), but the first word was 0x20494645 -- "EFI " -- so sector 1 and BLK_CNT_EN are
// both right and the card did start the block. The other 127 words were 0x20494645 as well:
// sm=24a32280 is exactly 128 times that word. PRESENT_STATE was 01f802f6 when the command
// completed (level bit clear: no data yet) and 01f80af6 at the end (level bit still set), so the
// loop had read 128 words in the first microseconds after the level first rose, while the card at
// 400 kHz single-bit delivers one word every ~80 us. 127 of those reads hit a buffer that was
// already empty -- and a full buffer is the only thing that stops this card. CMD12 then timed out
// (0x00018000, command timeout) and CMD13 said st=4 with READY_FOR_DATA: the card had finished the
// block, so the stall is entirely on the host side.
//
// Mode 12 keeps the chain and replaces only the drain: each FIFO read waits on CNTVCT_EL0 for
// 160 us (twice the 80 us a word takes at 400 kHz, and a slower reader can never lose data -- a
// full buffer backpressures the card), records when the first and last words arrived, counts how
// many words differed from the one before (`nd`, so one word read 128 times reads as 0), and
// decodes the GPT header's own fields. It also prints HOST_CONTROL, HOST_CONTROL2 and the final
// CLOCK_CONTROL, so a stopped clock or an unexpected bus width is visible on the same photograph.
//
// Mode 12's photograph separated arrival from consumption: the first word came at 10.4 ms, about
// when a 512-byte block at 400 kHz has finished, and the last read at 30.7 ms -- the 128 reads
// really were 160 us apart -- yet every one returned 0x20494645, nd stayed 0, and the level bit
// was still set at the end. So the buffer port never advanced, and reading slower cannot fix it.
// The one register this driver has left at its firmware value that decides which FIFO that port
// reads is VENDOR_SPEC3 bit 10, CORE_FIFO_ALT_EN (round87 read 0x02626440, bit 10 set). Linux
// clears it in sdhci_msm_init "to ensure the SDHCI FIFO is enabled" (sdhci-msm.c:5586). Mode 13
// clears it with the data machine already reset and runs mode 12's drain unchanged, printing the
// selector before and after (`v3`) in place of `hc`. If the port advances now, nd=127 and efi=1
// and the data path is PIO, which the rest of the driver can use; if it still returns one word,
// the FIFO question is closed the other way and the next round is ADMA, the path both the
// firmware and Linux use on this controller.
//
// Every path that is not the success path panics, because on this board silence is not a reading.
// Round87's photograph also settled what the watchdog is worth here: ticks=0 at this scheduling
// point. sheng_pmgr_run() runs right after bsd_bufferinit, about ten seconds of boot before
// ml_set_interrupts_enabled(), so the timer never reaches ICC_IAR1_EL1 and a hang inside this
// driver produces no panic at all. Every wait below therefore has its own bound, and reaching it
// is a reading ("no completion"), not a fallthrough.
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <kern/debug.h>
#include <stdarg.h>
// Round175: the kernel log buffer. printf() reaches msgbuf through putchar -> log_putc ->
// log_putc_locked(msgbufp, c) (bsd/kern/subr_prf.c:388, bsd/kern/subr_log.c:554); IOLog() reaches it
// through _IOLogv -> console_printbuf_putc (iokit/Kernel/IOLib.cpp, write_through=TRUE), not through
// log_putc. Boot 105's "r181-i2c capped at 24" proved the tee copied 24+ IOLog rows off msgbuf, so
// round319's first cut ("IOLog never reaches msgbuf") was wrong and is withdrawn; what the card
// showed instead is that a one-time early line is overwritten by this ring's laps long before
// readback. The header is
// exported to drivers (EXPORT_HDRS/bsd/sys/msgbuf.h) and msgbufp is declared under
// XNU_KERNEL_PRIVATE, which the kernel's own compile record for this driver already defines -- which
// is why this round needs no XNU patch at all.
#include <sys/msgbuf.h>
// Round305: the kernel's own uptime clock, the one every log line's [ secs ] is printed from
// (osfmk/kern/task.c's clover_now_seconds(), same two calls). The flight region has always carried the
// driver's clock -- flightCounterMs(gFlightStart), milliseconds since this driver armed the region --
// and the kernel log carries the other, and no record on the card has ever held both at one code
// point, which is why boot 94's panic marker could be bounded in kernel seconds but not placed in
// them. Reading the pair here, in the writer, is what turns the difference into a measurement.
#include <kern/clock.h>
#include "SdhciRegs.h"

extern "C" {
// Raw readings, for the offline decoder and for any later panic screen line. The order is
// documented at the fill site; slots stay zero if the run stopped before reaching them.
volatile uint64_t clover_emmc_observation[16];
// Round159's link readings, in the order fastMode() fills them: HS_TIMING verdict/status/response/
// polls, BUS_WIDTH the same four, then the raised clock, HOST_CONTROL, the verification header's
// two words, and a stage word that says which step the sequence stopped at. Zero means the fast
// path never ran, which is the shape of a boot that fell back to 400 kHz 1-bit.
volatile uint64_t clover_emmc_link[16];
// The formatted report, one %s argument, so the panic screen's argument list cannot shift.
char clover_emmc_report[512];
volatile uint32_t clover_emmc_stage = 0;
// Round147: the restart-reason cell, once clover_unattended_begin() has mapped IMEM and proved the
// cell reads back. The panic screen resolves this pointer's own address (patch-xnu-entry-probe.py
// passes it as FASTBOOT_REASON) and writes the word through it just before PSCI SYSTEM_RESET.
// Null means the cell is not available this boot: the screen then resets normally.
volatile uint32_t *clover_fastboot_reason = nullptr;
// Round283: the reset record above, published once clover_unattended_begin() has mapped IMEM. The
// kernel's reset funnels and the panic screen write through these entry points; both check this
// pointer, so a boot that never armed the instrument writes nothing anywhere. clover_imem_note is
// reached by the panic screen through the patcher (IMEM_NOTE), which resolves this function's
// address rather than the slot's, so the layout stays in one place.
volatile uint64_t *clover_imem_record = nullptr;
void clover_imem_note (unsigned int kind, unsigned long pid, unsigned long aux, unsigned long pc,
                       const char *text);
void clover_imem_heartbeat (void);
// drivers/CloverBlock's entry points: mode 21 asks for the boot's block device, and the block run
// itself is deferred to the IOBSD resource publication because it needs devfs and the vnode tables
// vfsinit() builds (round113 proved the early run gets no devfs node at all).
void clover_block_request (void);
}

// One 512-byte PIO block at a time, in BSS rather than on a kernel stack: mode 14 reads the GPT
// entry array block by block and inspects each entry before issuing the next command. Mode 16
// needs the written block and the block read back side by side, so it gets its own two buffers.
static uint32_t gEntryBlock[128];
static uint32_t gWriteBlock[128];
static uint32_t gReadBlock[128];

// Mode 21 publishes this same path as a block device through drivers/CloverBlock. That device
// reports the card's own EXT_CSD sector count and serves reads one block at a time, so both the
// geometry and the data come from the card this boot measured, not from a constant. The published
// count is gated on the logfs extent's last LBA: a card that does not even reach the partition
// this port has been reading is not a geometry to hand to the storage stack.
static uint32_t gExtCsd[128];
static const uint32_t kLogfsFirstLba = 12320768u;
static const uint32_t kLogfsLastLba = 12337151u;   // sysfs: start 12320768, 16384 sectors
// The Data partition as the unit numbers it (mmcblk0p64), from the same dump the logfs extent came
// from: 106347199 sectors, ending 33 sectors short of the card so the backup GPT stays outside it.
// Round128 puts the iPadOS root image here, so its entry is a window the way logfs's is.
static const uint32_t kUserDataFirstLba = 15795488u;
static const uint32_t kUserDataLastLba = 122142686u;
// Round185: the log window is a different partition from the root window. Everything this driver
// writes for the host to read -- the panic report slots, the verdict ring, the kernel tee ring and
// the write gate's sample run -- sat at the start of logfs, which since round127 is the partition
// XNU mounts as root: report slot 0 is the container superblock's own region and the ring offsets
// are not free space either. The card has a partition named for logs that this boot uses for
// nothing: "logdump", mmcblk0p56, 131072 sectors from 13320216 (64 MiB), from the same sysfs dump
// the logfs and userdata extents came from -- facts/81ffcaaa-20261022-210928/mmc_partitions.txt.
static const uint32_t kLogDumpFirstLba = 13320216u;
static const uint32_t kLogDumpLastLba = 13451287u;   // start + 131072 - 1
static volatile uint32_t gBlockReady = 0;
static volatile uint32_t gBlockSectors = 0;
// Round171: one command engine and one FIFO, so a request owns the host from its first command to
// its last. What the ownership is made of changed. Until now it was a flag and a compare-and-swap
// retry loop: a second caller spun, and on this board -- where the boot-time readers are APFS and
// the daemon pager threads, and where a spinning caller does not hand the core back -- a waiter
// burned the cycles the holder needed to finish, so every contended request made every other
// request slower. sheng's own block device does not do that: NativeUfs allocates an IOLock per
// window and takes it around the whole read (its RootWindowRead.h guard is the same shape), which
// puts the waiter to sleep instead of into a loop. This is the same instrument, on the one host
// these reads share. Allocated at bring-up; the device is not published if it is missing.
// Round213: what the ownership is made of changed a second time. Round171 made it an IOLock so a
// waiter slept instead of burning the holder's core, and that was right until the r212 frame: one
// read entered at 194.18 s and never came back, so every later reader slept on that lock until the
// boot died, and the panic sink -- which takes the same guard -- could not write one record at the
// 900 s deadline. An IOLock has no timeout; a ticket word does. The owner is visible to the CPU0
// beat, a waiter can bound its own wait, and the beat can take the ticket back from a holder whose
// stage has not moved. That is what makes a stuck request cost one request instead of the boot.
static volatile uint32_t gHostOwner = 0;       // 0 is free; anything else is the holder's ticket
static volatile uint32_t gHostArmed = 0;       // set once the device is published
static uint32_t gHostTickets = 0;              // ticket generator, unique per acquire
static const uint64_t kGuardWaitMs = 60000;    // a waiter gives up, reports, and lets APFS retry
static const uint64_t kGuardStuckMs = 75000;   // a stage that has not moved this long is stuck
// Round178: what round171's lock made possible but did not itself do -- give a waiter a turn *during*
// a request, not only between requests. The r170 frame measured the cost of owning the host for a
// whole request: `tx_flush: disk5 tx xid 526 took 102001275 us to flush` on one volume, and on the
// other eight reads that failed with `st=00000000`, no host error bit at all, because they never got
// the guard. pid 1 then died of SIGBUS in a page-in. Waiting is not the same as being refused, so
// round171 removed the refusal; a waiter still sleeps for the holder's whole run, and a run is 128
// blocks at a time. These counters are the hand-off below's own readings: how many times a holder
// gave the lock away mid-request, how many waits that produced, and the longest of them.
static volatile uint32_t gGuardHandoffs = 0;
static volatile uint32_t gGuardWaits = 0;
static volatile uint64_t gGuardWorstWait = 0;   // ticks, the worst wait any caller has measured
// Round190: the last wait, in milliseconds, for any caller. The worst of them is already reported at
// a request's tail, but a tail is exactly what a caller waiting on the guard does not reach -- so the
// number has to be readable from someone else's record. The flush below prints it before it starts.
static volatile uint64_t gGuardLastWaitMs = 0;
// Round127's second export: the logfs partition as a window, first LBA and length, taken from the
// GPT entry this boot read rather than from the constants above -- those two only decide whether
// the entry is the one this port has been reading. A block device described as this window has
// its first sector at media byte zero, which is what lets a filesystem that expects to own a whole
// disk (APFS finds its container superblock at offset 0) attach to a partition without the card's
// partition table being touched. Zero until the entry walk says the extent is exactly right.
static volatile uint32_t gBlockWindowFirst = 0;
static volatile uint32_t gBlockWindowSectors = 0;
// Round128's third export, same shape for userdata (entry 64). The image this port writes at the
// partition's byte zero is a manifest, not a filesystem: drivers/CloverBlock reads the first 4 KiB
// through the window below, finds the OS window inside it, and re-scopes the media to that.
static volatile uint32_t gBlockUserDataFirst = 0;
static volatile uint32_t gBlockUserDataSectors = 0;
// Round185's fourth window, and the only one this driver writes into. CloverBlock never sees it:
// it is the panic report's slots 0..31, the two rings and the write gate's run, all of it at these
// same offsets from a base the card itself reported. Zero until the entry walk names logdump.
static volatile uint32_t gLogWindowFirst = 0;
static volatile uint32_t gLogWindowSectors = 0;

// Round147, unattended boot. clover.dtb has restart_reason@65c under qcom,msm-imem@146bf000, and
// the stock kernel's `reboot bootloader` writes 0x77665501 there before its PSCI reset; XBL reads
// that word on the way up and comes back in fastboot instead of Android. IMEM is SRAM, so arming
// the cell touches no partition and no card: unlike a `misc` BCB it costs nothing and cannot be
// left half-written. This driver owns the mapping because the panic screen is freestanding and
// cannot map anything; it publishes the cell's address only after the read-back below agrees.
static const uint64_t kImemPhys = 0x146bf000ULL;
static const uint64_t kImemSize = 0x1000ULL;
static const uint32_t kRestartReasonOffset = 0x65c;
static const uint32_t kRestartFastboot = 0x77665501u;
static OSSharedPtr<IOMemoryMap> gImemMap;

// Round283, who reset the board. clover.dtb claims restart_reason@65c (4 bytes) and boot_stats@6b0
// (0x20), leaving 0x660..0x6af -- twenty words -- unclaimed by any child node. XBL on this board
// ignores the restart-reason cell (round195), so nothing in the boot chain writes here and the words
// survive a warm reset. A record in this gap is the only reading that does not need the card:
// rounds 280/281 showed the flight ring can sit 11 s behind the last kernel line when the card is
// wedged, so "the log stopped" is not the same as "the board stopped".
//
// Layout, twenty 64-bit words, little endian:
//   0 magic | 1 kind | 2 pid | 3 aux | 4 pc | 5 CNTVCT | 6..8 text[24]
//   9 beat magic | 10 beat count | 11 beat CNTVCT | 12..19 zero
// The first writer wins (kind 1 halt, 2 restart, 3 reboot(2), 4 panic screen); the heartbeat owns
// words 9..11 and never touches the magic, so a boot can end either with a named reset (kind != 0)
// or with only a heartbeat (the reset did not pass through any hook this kernel knows).
static const uint32_t kResetRecordOffset = 0x660;
static const uint64_t kResetRecordMagic = 0x00333832564f4c43ULL;   // "CLOV283" as stored bytes
static const uint64_t kResetBeatMagic = 0x0033383254414542ULL;     // "BEAT283" as stored bytes

// Round196. The IMEM word above reads back on this board but the reset that follows does not
// consult it: round195's panic left fb=1 in SRAM and the board came up in Android, not fastboot.
// XBL on clover does honor the Android bootloader control block, so the panic path writes one
// into `misc` instead. 13180952 is that partition's first LBA in the card's own GPT (2048
// sectors), and the block is the standard 32-byte command followed by zeros -- the same 2048
// bytes the host's make_bcb produces and round193 proved lands in TWRP in 14 s.
static const uint32_t kMiscFirstLba = 13180952u;
static const uint32_t kBcbBlocks = 2;   // command/status/recovery/stage fit in the first 1 KiB

// Mode 17 stores the report as text, so the first bytes of the block identify it when read on
// another OS. The newline is part of the stored length: the block reads as a small text file.
// Mode 18 only reads that record: the magic and the byte sum decide whether this boot can see
// what a previous one wrote, without writing anything itself. Mode 19 appends: slot k is two
// blocks at 2k, and the scan stops at the first header without the magic, so each boot's record
// lands beside the last one instead of on top of it.
static const char kLogPrefix[] = "unixnu clover emmc log\n";
static const uint32_t kLogPrefixLength = sizeof (kLogPrefix) - 1;
// Mode 19 treats logfs start as a sequence of fixed two-block slots: header at 2k, text at 2k+1.
// The scan is bounded, so a partition whose slots are all taken reports "ap f=" instead of
// quietly wrapping onto the oldest record.
static const uint32_t kLogMagic0 = 0x554e5855u;
static const uint32_t kLogMagic1 = 0x31474f4cu;
static const uint32_t kLogSlots = 16;
// Round174: the verdict ring. Every round since r165 has had the same ending -- the one line that
// decides the next round was on the screen for a moment and then scrolled off, so a frame came back
// with `disk5: general error` but not `write cache ... off=`, or with the panic but not the
// `dmacheck` that would have explained it. The card already has a place for text (the two-block
// records above), so this round stops asking the panel to hold a reading: the driver's verdict lines
// are written as records of their own, in a ring that starts at sector 64 -- past the 32 blocks the
// panic writer owns -- and `read-clover-log.py` reads the whole window in one dd. The ring is 32
// records, so a boot that reaches every verdict still fits, and the writer wraps rather than
// refusing: the newest line is the one that matters, and a wrap is visible as a slot index jump.
// The RAM ring is what a line goes to first. Writing the card from the place that decided
// the line would mean issuing two block writes in the middle of the failed transfer that produced
// it -- a second owner for a host with one command engine, and a new way to hang a boot. So saying
// something costs a memcpy, and the card is written only from a path that already holds the block
// guard (the tails of the read, write and cache-flush requests). Until that happens the reading
// lives in memory, which is also where the panel-less panic screen reads nothing but the host sees
// it anyway. Each ring overwrites its oldest entry, so the newest line is never the one that gets
// dropped.
//
// Round176: two rings, because one was not a channel, it was a queue for whoever talked last. The
// 32 records round174 sized for nine verdict lines are shared with round175's tee since last round,
// and a chatty daemon evicts the driver's reading before the card ever sees it -- the exact failure
// this ring exists to prevent, reintroduced by its own second producer. logfs has 16384 sectors and
// the two rings together now use 1216 of them, so widening is a matter of geometry, not of a second
// pointer fighting for the same 32 slots: the driver keeps its own ring (grown to 64 records, since
// the tee's state lines are still said there), and the kernel log gets a ring of its own 512 records
// deep at sector 192. A whole boot's allowlisted kernel text is a few hundred lines, which fits;
// the driver's readings cannot be pushed out of them any more, and the host reads both with one dd.
static const uint32_t kRingDriver = 0;   // what CloverSdhci measured (round174's ring)
static const uint32_t kRingKernel = 1;   // what the kernel said (round175's tee, round176's ring)
static const uint32_t kRingCount = 2;
static const uint32_t kVerdictTextBytes = 320;
static const uint32_t kRingRamSlots = 128;   // staging depth; deeper than either card ring
static const uint32_t kRingCardSlots[kRingCount] = {64, 512};
static const uint32_t kRingOffset[kRingCount] = {64, 192};   // sectors past logfs start
// Round212: the boot id's home, one record past the kernel ring (sector 1216). It is not part of
// either ring: it is rewritten in place once per boot, before any record that carries its number,
// and the host reads it in the same dd window that reads the rings.
static const uint32_t kBootRecordOffset = 192 + 2 * 512;
static const uint32_t kBootKind = 24;
// Round212: the probe ring's records keep a kind of their own. They share the driver ring's card
// slots -- 64 records of two blocks -- but not its origin: a kind-21 row is a line the driver said,
// and telling the host which of the two RAM rings it came from is the difference between "the
// kernel read this out of msgbuf" and "the kernel wrote this into our BSS before it died".
static const uint32_t kProbeKind = 25;
// Records flushed per guarded pass, per ring. Two block writes each, and a flush must not hold the
// guard long: eight is 16 writes, which at the round159 rates is a few milliseconds.
static const uint32_t kRingFlushPerPass[kRingCount] = {4, 8};
static char gRingText[kRingCount][kRingRamSlots][kVerdictTextBytes];
static uint32_t gRingLength[kRingCount][kRingRamSlots];
static uint32_t gVerdictFirst = 0;       // the logfs window's first LBA; both rings live in it
static uint32_t gRingWrite[kRingCount];     // next RAM slot to fill
static uint32_t gRingPending[kRingCount];   // RAM slots holding text not yet on the card
static uint32_t gRingDropped[kRingCount];   // records the ring overwrote before they could flush
static uint32_t gRingCursor[kRingCount];    // card slot counter; slot = cursor % kRingCardSlots
static uint32_t gRingSeq[kRingCount];       // stamped in word 8 so a wrapped ring stays ordered
static uint32_t gRingWritten[kRingCount];
static volatile uint32_t gRingBusy[kRingCount];
static volatile uint32_t gVerdictReady = 0;   // driver ring: armed with the logfs window

// Round212: every record carries the boot that wrote it. Round211's frame could not tell the r184
// drain line -- which says a drain ran and wrote four records -- from a line an earlier boot left in
// the same slot, and the two readings lead to opposite next rounds. The id is the low half of the
// generic counter at the moment the rings were armed: the same number for every line this boot
// writes, and no other boot's. It is stamped in word 9 of every record, kind 20..23 alike.
static uint32_t gBootId = 0;

// Round212: where the kernel's probe lines land when the block path is dead. Round211's frame put
// the SIGKILL at 457 s and the block guard's loss at 341 s, and the panic tail read could not carry
// the kill forward because a 16 KiB msgbuf holds a few seconds of a chatty boot. So the kernel probe
// writes straight into this RAM ring (a memcpy into the driver's BSS: no block request, no guard,
// no msgbuf, no tee), and the panic drain is what puts it on the card. 64 slots of 256 bytes are
// what a kill's thread list needs with round228's rcdump/rce tail on top; the seal word is written
// last and compared on read, so a drain that races a writer skips the one record rather than
// writing half of it. Round228 doubles the ring from 32: r228's dump adds four lines per kill, and
// one generation's kill can now carry r207/r208/r211/r226 plus three generations' dumps before a
// drain runs.
static const uint32_t kProbeSlots = 64;
static const uint32_t kProbeTextBytes = 256;
static char gProbeText[kProbeSlots][kProbeTextBytes];
static uint32_t gProbeArm[kProbeSlots];   // slot number + 1; written after the text, read as a seal
static volatile uint32_t gProbeSeq = 0;   // next slot number to hand out
static volatile uint32_t gProbeDropped = 0;   // records the ring overwrote before a drain
static uint32_t gProbeFlushed = 0;        // records any consumer put on the card

/*
 * Round304. The probe ring had exactly one consumer -- the panic drain -- and boot 93 never reached
 * it: the board stopped at ~555 s with no sink, no `drain enter` line and zero kind-25 records on the
 * card, which is what a relay that only empties at a panic costs. A row the kernel handed over at
 * 555.481 s is worth more in driver BSS than in msgbuf, but BSS nobody reads is the same silence.
 * So the ring gets a cursor of its own and two more consumers that do not need the sink: the block
 * tails that already flush both RAM rings, and CPU0's four-second beat.
 *
 * One cursor, advanced only after a record the card accepted, means a row is never written twice and
 * a refused write costs a retry rather than the reading. The seal word doubles as the staleness test:
 * if the producer overran an undelivered slot, that slot now carries a newer sequence number, the
 * compare says "not the row I am looking for", and the pass skips it after a bounded wait instead of
 * wedging the queue behind a row the writer never finished.
 *
 * All three consumers hold the block host while they run -- a tail holds the guard, the beat takes it
 * with a try, the drain runs with gPanicActive set and beats excluded -- so the cursor has one writer
 * at a time by construction, not by a lock invented here.
 */
static volatile uint32_t gProbeNext = 0;      // the sequence number the card has not received yet
static uint32_t gProbeSkipped = 0;            // rows dropped after their seal never arrived
static uint32_t gProbeStalled = 0;            // passes that stopped on an unsealed row
static uint32_t gProbeRefused = 0;            // passes that stopped on a card that said no
static uint32_t gProbeCalls = 0;              // flush passes of any consumer
static uint32_t gProbeSealWait = 0;           // consecutive passes stopped on the same unsealed row
static const uint32_t kProbeFlushCap = 16;    // records per pass; two block writes each
static const uint32_t kProbeSealRetries = 4;  // passes to wait for a seal before dropping the row
static const uint32_t kProbeBacklogEvery = 64;  // passes between backlog reports
static const uint32_t kProbeReportsMax = 64;  // per speaker since round307

/*
 * Round306. Boot 95 answered with the relay's own silence -- no kind-25 record, no report line -- and
 * three different causes read identically: the window was never armed, the host was taken every time a
 * consumer looked, and the kernel never called. The first two are the guard this function returns
 * before doing anything, so on those two boots the pass counter that the report line reads stays at
 * zero and the line never runs at all; the third is indistinguishable from either. Count each exit
 * where it happens and say the line from the exit itself, the way round289's abort row is said by the
 * handler rather than by a later pump: a status row that prints whatever it wrote is a status row that
 * can only be silent because nothing ran.
 *
 * The producer's two early exits are counted beside them. `clover_emmc_probe_say` refuses an empty
 * line and cuts a long one, and a cut row is not a missing row -- it is a row whose tail is missing,
 * which is the difference between "the census stopped" and "the census row was too wide for the relay".
 */
static const uint32_t kProbeBlockUnarmed = 0;   // no verdict window yet
static const uint32_t kProbeBlockHost = 1;      // the block host belongs to somebody else
static const uint32_t kProbeBlockBusy = 2;      // this ring's own flush is in progress
static const uint32_t kProbeBlockCount = 3;
static volatile uint32_t gProbeBlocked[kProbeBlockCount] = {0, 0, 0};
static volatile uint32_t gProbeEmptySay = 0;    // callers that handed the relay nothing to carry
static volatile uint32_t gProbeCutSay = 0;      // callers whose row exceeded the relay's text slot
/*
 * Round307. Boot 96 paid r306's two budgets back as a blindness of this instrument's own: both were
 * single counters shared by three speakers, and `why=tail` -- the consumer that runs most often -- spent
 * all four `nothing` slots, so `why=drain`, the one consumer that runs at the scene of the stop, never
 * got to say it. A bounded line said by whoever asks first is a line that reports its own queue order
 * and not the boot: the budget has to belong to the speaker, so a silent drain means a silent drain.
 * The guard-exit line had the same shape one branch above, so both counters become arrays.
 *
 * The row tokens move to r307 with them. Both lines gained a column (`calls=`), and a reader that cannot
 * tell which generation of image wrote the record it is holding would read an r306 line's missing column
 * as a zero -- the round293 lesson, applied to a field this round added.
 */
static const uint32_t kProbeWhyTail = 0;
static const uint32_t kProbeWhyBeat = 1;
static const uint32_t kProbeWhyDrain = 2;
static const uint32_t kProbeWhyOther = 3;   // a speaker this table does not know: its own slot, not a shared one
static const uint32_t kProbeWhyCount = 4;
static uint32_t gProbeSilenceSay[kProbeWhyCount] = {0, 0, 0, 0};   // guard-exit lines said, per speaker
static uint32_t gProbeNothingSay[kProbeWhyCount] = {0, 0, 0, 0};   // "a consumer ran and nothing has ever been delivered" lines, per speaker
static uint32_t gProbeReports[kProbeWhyCount] = {0, 0, 0, 0};      // r304 self-report lines said, per speaker
static const uint32_t kProbeSilenceSayMax = 4;  // per speaker: this is a state line, not a stream
static const uint32_t kProbeNothingSayMax = 4;

static uint32_t probeWhySlot (const char *why)
{
  if (why == nullptr) {
    return kProbeWhyOther;
  }
  if (why[0] == 't' && why[1] == 'a' && why[2] == 'i' && why[3] == 'l') {
    return kProbeWhyTail;
  }
  if (why[0] == 'b' && why[1] == 'e' && why[2] == 'a' && why[3] == 't') {
    return kProbeWhyBeat;
  }
  if (why[0] == 'd' && why[1] == 'r' && why[2] == 'a' && why[3] == 'i' && why[4] == 'n') {
    return kProbeWhyDrain;
  }
  return kProbeWhyOther;
}

// Round210: the flight region. Round209 proved the kernel was alive at the 900 s deadline while the
// card accepted nothing -- the report window got no record and the rings got no drain -- so the one
// question left is which transfer stopped, and whether the controller still held CMD/DAT inhibit
// when it did. The answer has to be on the card *before* the stop, because everything after it
// fails. Sectors 32..63 are the gap between the report window and the driver's ring; twelve
// one-block records there name the last requests, and four at 56..63 carry a beat from CPU0's timer
// every four seconds. A beat record's own presence says the guard was free and the controller took
// a command; the beats that stop say it was not. Each entry record carries the previous request's
// completion count and duration, so a record with no successor names the request that never came
// back. One block per record, not two: this runs at every block request, and the second text block
// would double a cost the transfer itself has to absorb.
//
// Gated on clover-unattended: a shipped boot pays nothing for a diagnostic this driver does not
// need after the port reaches the desktop.
static const uint32_t kFlightEntryOffset = 32;   // sectors past logfs start; 12 one-block records
static const uint32_t kFlightEntrySlots = 12;
static const uint32_t kFlightBeatOffset = 56;    // four one-block records, 4 s apart
static const uint32_t kFlightBeatSlots = 4;
// Round236: the two sectors the beat leaves unused. The deadline record is written from the timer
// interrupt before panic(), so the fact that the deadline fired reaches the card even when the panic
// sink cannot (boot 24's frame: no kind-20 record, empty BCB, and the card busy recovering). The
// panic marker is written on both sides of the report append, so an empty report window names the
// gate that refused instead of leaving a silence that looks like a card failure.
static const uint32_t kFlightDeadlineOffset = 60; // one record, one write, once per boot
static const uint32_t kFlightDeadlineSlots = 1;
static const uint32_t kFlightPanicOffset = 61;    // enter and exit, one sector each
static const uint32_t kFlightPanicSlots = 2;
// Round302: boot 91's panic slot is a hybrid -- event/dir 5/3 with eight extras that were never
// written, while the same record's header names a live 8-block request from osanalyt, and boot 88
// carries the identical shape. That is only reachable because every flight event builds its record
// in the one gFlightBlock, which writePioBlock then streams out of that same buffer while the request
// path is free to zero and rebuild it. Two things follow, and they are the two halves of this round:
// a verdict record owns its buffer, and it says so itself -- a magic word right after its own extras,
// so "this sector was written as a panic marker" is a fact the record carries instead of one the
// reader infers from an offset (r293's boot stamp was being printed as boot 91's stage).
//
// The twelve sectors between the entry ring and the beats (44..55) have never had an owner. They
// become the panic sentence's own record: one block, indexed by boot id, written inside the sink with
// no read beforehand -- because r301's RAM copy still reaches the card only through a later flush,
// and boot 91 is a boot that never got one. Blind indexing is what sheng does (slots_written modulo
// the slot count, the window base computed once), and it is what this append has never done:
// panicAppendText() reads all sixteen report slots before writing one of them, and boot 90 lost its
// sentence exactly there (append_rc=2 on that read, one attempt, no retry).
static const uint32_t kFlightTextOffset = 44;     // twelve one-block records, one per boot
static const uint32_t kFlightTextSlots = 12;
static const uint32_t kFlightEventText = 6;
/*
 * Round309: the one sector 32..63 still without an owner becomes the drain's own first breath.
 * Boot 99 searched the whole window for a `drain` row and found none -- not the r212 entry row, not the
 * r308 exit row this round added, not the tee's report line -- while the same boot's panic entry marker
 * is on the card. The only mechanism left standing is that everything the drain says reaches the card
 * through a path that wants the host ticket, and that is a candidate, not a reading, until a row exists
 * which is written before the drain reads a single gate and by the path r302 proved needs no ticket.
 * One slot, overwritten each boot, and stamped with the boot id the way r293 requires: a boot that
 * never got here shows the previous boot's record, and the decoder is told to say STALE rather than
 * to print it as this ending.
 */
static const uint32_t kFlightDrainOffset = 63;    // one record, written before the drain's own gates
static const uint32_t kFlightDrainSlots = 1;
static const uint32_t kFlightEventDrainIn = 7;
static const uint32_t kFlightMarkerMagic = 0x434C564Du;   // 'CLVM', the flight verdict magic
static const uint32_t kFlightKind = 23;
static const uint32_t kFlightEventRead = 1;
static const uint32_t kFlightEventWrite = 2;
static const uint32_t kFlightEventBeat = 3;
static const uint32_t kFlightEventDeadline = 4;
static const uint32_t kFlightEventPanic = 5;
static volatile uint32_t gFlightArmed = 0;
static volatile uint32_t gPanicActive = 0;       // the panic sink is writing; no beat may cut in
static uint32_t gFlightEntrySeq = 0;
static uint32_t gFlightBeatSeq = 0;
static uint32_t gFlightDeadlineSeq = 0;
static uint32_t gFlightPanicSeq = 0;
static uint32_t gFlightDrainSeq = 0;   // round309: the drain-in record's own cursor
static volatile uint32_t gFlightDeadlineDone = 0; // the deadline fires on every CPU; one record
static uint32_t gFlightPrevDone = 0;             // result of the last finished request
static uint32_t gFlightPrevMs = 0;
static uint32_t gFlightPrevDir = 0;
static uint32_t gFlightPrevBlocks = 0;
static uint32_t gFlightCurDir = 0;
static uint32_t gFlightCurBlocks = 0;
static uint32_t gFlightOps = 0;                  // requests that reached their tail this boot
static uint32_t gFlightBeats = 0;
static volatile uint32_t gFlightBeatMisses = 0;  // beats that found the guard held
static uint64_t gFlightStart = 0;                // counter value when the region was armed
static uint64_t gFlightOpStart = 0;
static uint64_t gFlightLastBeat = 0;
// Round305. A beat that returns early is a beat nobody counts: boot 94 carried five reports and every
// one said why=tail, so the second flush consumer -- the one wired behind the beat -- never ran for the
// whole boot, and the only evidence of that was an absence. The four exits of clover_emmc_flight_beat
// are four different facts (the region is not armed, a panic owns the card, the last beat is four
// seconds old, somebody else holds the host), so each one increments its own counter and every report
// that lands -- the beat's own record and the block tail's probe line -- carries all four as they stand.
// They are never cleared: two consumers read them, and the one that can be starved must not be the one
// that decides what the other sees. Counting starvation is the only thing a consumer that
// cannot move ahead of its own guard can still do.
static const uint32_t kBeatBlind = 0;   // no window base, no host, region or host not armed
static const uint32_t kBeatPanic = 1;   // the panic sink owns the card
static const uint32_t kBeatYoung = 2;   // the last beat is under four seconds old
static const uint32_t kBeatHeld = 3;    // somebody else holds the host ticket
static volatile uint32_t gBeatStarved[4] = {0, 0, 0, 0};

// Round305, the other half of the same defect: every writer on this driver ends in writePioBlock, and
// writePioBlock has three distinguishable exits that say no -- no command slot, the command was
// refused, the data phase did not finish -- and all three throw their detail away. Boot 94 could say
// the sink's later rows did not reach the card but not whether they were refused or never attempted,
// because the refusal had no voice. The last refusal is kept here and travels with the next record
// that does land; the total is since the driver armed, the detail is only the most recent refusal, so a
// boot that was refused twice names one of them and counts both. A 0 in the code means never refused.
static volatile uint32_t gPioFails = 0;          // refusals since the driver armed
static volatile uint32_t gPioFailCode = 0;       // 1 slot, 2 command, 3 data, 4 data errors, 5 short
static volatile uint32_t gPioFailStatus = 0;     // SDHCI_INT_STATUS as the failing exit saw it
static volatile uint32_t gPioFailWords = 0;      // words handed to the data port
static volatile uint32_t gPioFailMs = 0;         // flight ms of the refusal, 0 before the region armed
static volatile uint32_t gPioFailLba = 0;        // the block the refusal happened on
static volatile uint32_t gPioFailAttempt = 0;    // what was being written: 21/22/25 ring, 20 screen,
                                                 // 24 boot id, 100+event flight record
static volatile uint32_t gPioAttempt = 0;        // who asked: ring kind, 20 screen, 100+flight event
static uint32_t flightCounterMs (uint64_t since);

// Round213: the stage the guard holder is in. r212's frame ended with an entry record and no
// successor, and every wait inside this driver is bounded, so the next question is not "how long"
// but "where": the last stage a request wrote is the one it did not come back from. Stages are
// written by the holder only -- a waiter leaves the holder's stage alone -- and the beat compares
// the stage and its stamp against its own clock: a stage that has not changed for kGuardStuckMs is
// the evidence that the holder is not progressing.
static const uint32_t kStageIdle = 0;
static const uint32_t kStageEntry = 1;
static const uint32_t kStageIdleWait = 2;
static const uint32_t kStageCheck = 3;
static const uint32_t kStageAdma = 4;
static const uint32_t kStageBatch = 5;
static const uint32_t kStageSingle = 6;
static const uint32_t kStageSettle = 7;
static const uint32_t kStageTail = 8;
static const uint32_t kStageTee = 9;
static const uint32_t kStageFlush = 10;
static const uint32_t kStageCommit = 11;
// Round214: the watchdog is inside reinitializeCard(). A frame that ends here says the recovery
// itself did not finish, which is a different reading from one that ends in the request it took
// the card back from.
static const uint32_t kStageReinit = 12;
static volatile uint32_t gFlightStage = kStageIdle;
static volatile uint64_t gFlightStageSince = 0;
static volatile uint32_t gFlightStageDir = 0;
static volatile uint32_t gFlightStageLba = 0;
static volatile uint32_t gFlightStageBlocks = 0;
static volatile uint32_t gFlightStagePid = 0;
static volatile uint32_t gFlightStuck = 0;        // recovery attempts this boot, any trigger
static volatile uint32_t gFlightStuckStage = 0;   // the stage the last trip found
static volatile uint32_t gFlightRecovered = 0;    // recoveries that brought the card back
static volatile uint32_t gFlightSteals = 0;       // tickets taken back from a stuck holder
// Round215: r214's frame ended the 75 s watchdog as a theory -- a request's own deadline always ends
// the hold first, so the beat never saw a stage that old. The card is what goes quiet (the last
// write lands at 408 s and TWRP then boots from the same card, so the wedge dies with a power
// cycle). So the trigger moves to the request that met the silence: this records why the last card
// operation failed (0 ok, 1 DATA_INHIBIT never dropped, 2 CMD13 never answered, 3 card busy past
// the bound), and a recovery is rate-limited so a card that is really gone costs one ladder per
// cooldown instead of one per request.
static volatile uint32_t gIdleFailReason = 0;
static const uint32_t kIdleOk = 0;
static const uint32_t kIdleInhibit = 1;
static const uint32_t kIdleSilent = 2;
static const uint32_t kIdleBusy = 3;
static const uint32_t kCardRecoverCap = 16;
static const uint64_t kCardRecoverCooldownMs = 15000;
static uint64_t gCardRecoverLastTick = 0;
static uint32_t gFlightCurLba = 0;
static uint32_t gFlightCurPid = 0;
static uint32_t gFlightCurComm0 = 0;
static uint32_t gFlightCurComm1 = 0;
// Round213: who asked for the request. Both live in bsd/kern/kern_proc.c and both are called only
// from flightEnter(), i.e. from a thread that is about to issue block I/O, never from the beat or
// the panic sink. proc_selfname is null-safe; the pid is only asked for once the name came back.
extern "C" int proc_selfpid (void);
extern "C" void proc_selfname (char *buf, int size);

// Round175: the tee. The verdict ring made the driver's own readings survive the panel; the readings
// that actually decide the next round are printed by other people -- the r153/r155 shared-region
// probes, APFS's jhash and ntree errors, the code-signing line that kills every process, the r170
// exec/exit pairs -- and those go to printf, which has always ended on the panel and nowhere else.
// They do have one home the driver can reach: msgbufp, the circular kernel log buffer that
// printf/IOLog already append to. So instead of patching XNU again (and fighting the rule that a
// probe may not be inserted inside an earlier patch's own added block), this round just reads the
// buffer the kernel already keeps and copies the interesting lines into the ring that already writes
// to the card. Nothing new is measured; nothing new has to be linked.
//
// The whole buffer is not copied. A 1 MiB log at 25 KB/s of card would be a boot-long file transfer,
// and most of it is noise, so the tee consumes bytes from its own cursor and keeps only lines that
// contain one of a few strings the port is currently trying to learn about. The cursor is a byte
// index, not a line number, because the producer wraps the buffer out from under it; anything the tee
// has not reached before the writer laps it is gone, which is why it is worth reading a chunk per
// request rather than one line.
static const uint32_t kTeeScanPerPass = 4096;   // bytes read per pass; a chunk, not the whole buffer
// Round189 doubles the push budget: the r188 census (seen=1598 throttled=694) says the per-pass cap,
// not the ring, was dropping the tail of a burst that this boot needs to see.
static const uint32_t kTeePushPerPass = 16;     // matching lines handed to the kernel ring per pass
static const uint32_t kTeeLineBytes = kVerdictTextBytes;   // a longer line is truncated, not split
// What a kernel line has to contain before it is worth two block writes. Round176 grows this on
// purpose: the tee used to spend the driver's 32 records, so every entry here cost a verdict line,
// and the list was kept short for that reason alone. Now it has its own 512-record ring, and the
// entries below are the three families the port is actually blocked on -- the code signature that
// returns 85 (ubc_subr.c:4702 prints `cs_blob`), the root mount (`mountroot`, vfs_subr.c:1468), and
// anything that names dyld, which is where the shared cache story lives.
static const char *const kTeeAllowlist[] = {
  "clover r",             // this port's own probes (r151/r153/r155/r170), whatever they print
  "CloverBlock",          // the volume/owner driver
  "apfs", "APFS",         // container scan, jhash, ntree reads
  "decmpfs",              // compressed-data read failures
  "load code signature",  // the error 4 that has been killing every process since r162
  "cs_blob",              // the validation call that returns 85 before anyone can kill a process
  "AMFI", "amfi",         // who actually decided to kill it
  "launchd", "initproc",  // pid 1, and what happened to it
  "dyld",                 // shared cache loader, by name or in a path
  "vm_fault", "shared_region",   // the empty shared region this boot still dies in
  "mountroot", "rootvp",  // whether the root was accepted, and by which filesystem
  "panic",                // the panic string, if a panic got here first
  // Round272 admits the kernel's own secondary-cpu print, osfmk/arm64/cpu.c:909
  // kprintf("cpu_start() cpu: %d"). Boot 58 measured that only the boot cpu ever registered
  // (clover r271: cpu real=1 reg=1), and the kernel image carries no AppleARMSMP at all, so the
  // question left is whether XNU ever *attempted* a bring-up. That attempt already speaks; the only
  // thing missing was a token to let its line through. It is also the line that would appear next
  // to a panic("enableCPUCore is unimplemented") (IOPMGR.cpp:45) if an attempt reached the missing
  // PMGR hook -- "panic" is on this list, so that half was never blind and did not appear.
  "cpu_start()",
};
static const uint32_t kTeeAllowlistCount =
    sizeof (kTeeAllowlist) / sizeof (kTeeAllowlist[0]);
// Round194: the r187/r189 syscall probes already answered their question -- on round193's frame every
// mach-out in the ring carried ret=0 -- but they say three lines per call, and 2021 of the kernel
// ring's 512 records were theirs: the ring covered 101 s of a 313 s boot and nothing before 211 s.
// They are a sample now, not a transcript. Enough survive to show the last calls before a stop; the
// rest stay out so the ring spans the boot it describes.
//
// Round195 gives every chatty class its own budget instead of one shared one. On round194's frame the
// flood had simply moved: fairplayd.A2 crash-respawns and re-reads the dyld cache, so r155 (three lines
// per read), r153 and r170's opens of dyld_shared_cache_arm64.symbols took 292/100/99 of 512 slots and
// the ring still covered only 110 s of a 912 s boot. Each class reports its own first drop, so a ring
// that stops early now names which producer filled it. r170's exec/exit lines stay uncapped: a process
// dying is what a stop is read from.
struct TeeProbeClass {
  const char *token;
  const char *name;
  uint32_t keep_max;
  uint32_t kept;
  uint32_t dropped;
  uint32_t reported;
};
static TeeProbeClass kTeeProbeClasses[] = {
  { "clover r153:",       "r153",      24, 0, 0, 0 },
  { "clover r155:",       "r155",      32, 0, 0, 0 },
  { "clover r170: open ", "r170-open", 32, 0, 0, 0 },
  { "clover r187:",       "r187",      24, 0, 0, 0 },
  { "clover r189:",       "r189",      24, 0, 0, 0 },
  // Round196's abort descriptions: usermanagerd respawns faster than anything else in the ring, so
  // its own three lines are the budget this class exists to protect -- a looping daemon must not
  // push the earlier lines out again.
  { "clover r196: sig ",  "r196-sig",  48, 0, 0, 0 },
  // Round200's user-breakpoint reading: every trap prints two lines (r199's identity line and
  // r200's map/code/register line), SpringBoard alone repeats it eight times a frame, and the
  // kernel caps itself at 24 traps. 64 lines leave slack for both without letting the producer
  // push the exec/exit pairs out of the ring.
  { "clover r200: ubrk", "r200-ubrk", 64, 0, 0, 0 },
  // Round203's shared-cache half: a trap inside the dyld shared cache prints four more lines (x4-x7,
  // x19-x28, the stack words, the object x0 points at), once per pid. Four pids' worth of slack over
  // the two lines that same pid would have spent, and the two classes together stay under a
  // hundred records of the kernel ring.
  { "clover r203: ", "r203-ubrk", 64, 0, 0, 0 },
  // Round204 reads the mode object from x19 instead of x0, so SpringBoard's detailed reading is seven
  // lines per pid. It gets eight slots of its own in the kernel, and this class matches the eight of
  // them that can print.
  { "clover r204: ", "r204-ubrk", 64, 0, 0, 0 },
  // Round207: the sender line prints for SIGKILL/SIGTERM only, at most four per command name and
  // forty overall. One SpringBoard death is what the ring is read for, so a class of its own keeps
  // that line from being pushed out by a daemon that is killed in a loop.
  { "clover r207: ", "r207-kill", 48, 0, 0, 0 },
  // Round208 adds two lines to the same delivery: the sender's user pc plus the instruction word
  // at it, and up to ten lines naming every SpringBoard thread's pc, cpsr, state and wait event.
  // The kernel caps itself at 64 lines, so the class is the same 64 and one SpringBoard death
  // still fits the ring beside r207's line -- and r207's own class of 48 cannot be starved by it.
  { "clover r208: ", "r208-kill", 64, 0, 0, 0 },
  // Round211 keeps the same delivery but adds a second line per thread (the kernel stack returned
  // into) and one line for the sender's own kernel stack. Twelve lines at most per kill, so 64
  // leaves room for three deaths without letting the class crowd out the exec/exit pairs.
  { "clover r211: ", "r211-kill", 64, 0, 0, 0 },
  // Round217 stops reading the corpse and samples the living process instead: one line every eight
  // seconds while SpringBoard's thread states change, then a heartbeat every two minutes. The kernel
  // caps the changes at 32 and the boot ends by 900 s, so 64 is the whole conversation with room to
  // spare, and the class cannot crowd the exec/exit pairs.
  { "clover r217: ", "r217-sbs", 64, 0, 0, 0 },
  // Round218 adds three lines to a kill (sender code and user stack) and up to four frames of user
  // chain per sampled thread, five threads at most, every eight seconds while the states change.
  // The kernel caps the changes at 32, so 32 x 5 + 2 fits in 192: the frame before the kill cannot
  // be truncated, and the class still stops a looping daemon from pushing the exec/exit pairs out.
  { "clover r218: ", "r218-sbs", 192, 0, 0, 0 },
  // Round221 keeps round220's one line per thread parked in mach_msg2 and adds
  // two: the blocked thread's pc/lr translated to dyld shared-cache file
  // offsets with eight instructions at the pc, and the printable strings of a
  // send body. One batch is at most three lines per thread, five threads, and
  // it prints only when the batch changes, so 192 covers a realistic boot with
  // room to spare.
  { "clover r221: ", "r221-sbs", 192, 0, 0, 0 },
  // Round222 adds the blocked thread's user frames and the full send
  // body. Two lines per picked thread per change, five threads, and the
  // batch prints only when it changes, so 192 covers a realistic boot.
  { "clover r222: ", "r222-sbs", 192, 0, 0, 0 },
  // Round223 reads both ends of the blocked send: two port-state lines per
  // picked thread per change (destination and reply port), five threads, so
  // 192 covers the batch and the lines just before the kill.
  { "clover r223: ", "r223-sbs", 192, 0, 0, 0 },
  // Round224 moves the subject from SpringBoard to launchd: one line every
  // eight seconds with five threads at most, so a full 900 s boot is at most
  // ~115 lines. 192 keeps the whole conversation and still bounds a looping
  // producer.
  { "clover r224: ", "r224-peer", 192, 0, 0, 0 },
  // Round225 adds launchd's own mach_msg2 reading: one state line per change,
  // then at most 24 probes of up to five lines each (call words, user frames,
  // body hex, two port states) over a full boot. 192 holds the worst case and
  // still bounds a producer that loops harder than pid 1 does.
  { "clover r225: ", "r225-p1", 192, 0, 0, 0 },
  // Round226 adds the killed thread's registers: seven lines per kill (four
  // kreg, one kreg4, two kraw) capped at four kills, plus one-line sbrcv
  // updates from the receive path (32 changes) and a single sbgone. 192 holds
  // four full kills and still leaves room for the receive side.
  { "clover r226: ", "r226-die", 192, 0, 0, 0 },
  // Round228 keeps the ring dump but prints it from a spin-locked snapshot,
  // relabels it, and adds a compact rcdump/rce tail that travels the probe
  // ring. The full dump is one summary plus at most 64 state lines and 64
  // body lines -- 129 lines per generation's kill, and boot 16 watched three
  // generations -- so the budget covers three dumps plus the sbtask lines and
  // still bounds a producer that loops harder than SpringBoard does.
  { "clover r228: ", "r228-rcv", 480, 0, 0, 0 },
  // Round229 reads the receiver of the blocked send instead of guessing it: one thread line and one
  // queue line per batch that names backboardd, and the kernel fires it at most 24 times. 64 holds
  // the whole conversation and still bounds a producer that loops harder than a boot does.
  { "clover r229: ", "r229-bb", 64, 0, 0, 0 },
  // Round230 adds up to four hop lines and one queue line to each receiver reading the kernel is now
  // willing to print: the kernel only prints when the receiver changed, and it caps itself at 16 of
  // those. 96 is 16 fires' worth of hops and queues, so the tail of a wedged boot keeps its own hop
  // lines instead of losing them to the head of the same class.
  { "clover r230: ", "r230-hop", 96, 0, 0, 0 },
  // Round231 changes the hop row to the receive name and lets the sampler fire on a changed set or a
  // two-minute heartbeat, capped at 96 fires, with up to six waiting-thread rows beside each one.
  // 192 holds the whole conversation -- the heartbeat tail included -- and still bounds a producer
  // that loops harder than the receiver does.
  { "clover r231: ", "r231-bb", 192, 0, 0, 0 },
  // Round232 stops tying the receiver sample to SpringBoard's batch signature: once a batch names
  // backboardd, the kernel latches it and samples on its own 20 s beat while the trap keeps being
  // entered. Each hop name is resolved, and a name that is a port set is walked for its members in
  // one row. Three hop names at 120 s heartbeat over a 1800 s boot is under fifty rows per set, so
  // 128 holds the whole conversation and still bounds a producer that loops harder than a receiver.
  { "clover r232: ", "r232-set", 128, 0, 0, 0 },
  // Round233 asks four questions at once: which member of a named port set is the blocked
  // destination (the star row), who owns each hop and receiver name (the owner rows), whether the
  // kernel is still sampling after the trap goes quiet (the beat thread), and who holds the unfair
  // lock an idle thread waits on (the word read).
  // Round234 splits what round233 put in one class. Boot 22's r233-bb budget of 256 was spent by
  // 808 s while the set rows reached t=1764, so the reading that proves the kernel is alive and the
  // reading that names the blocked member were competing for the same 256 records. Each row gets
  // its own budget sized for a 30-minute boot, the set row is exempt from the 16-line per-pass
  // push cap, and the per-member bbsep rows are gone: every member now rides the set row.
  { "clover r234: bbset", "r234-set", 384, 0, 0, 0 },
  { "clover r234: bbhop", "r234-hop", 384, 0, 0, 0 },
  { "clover r234: sbown", "r234-own", 192, 0, 0, 0 },
  { "clover r234: bblock", "r234-lock", 96, 0, 0, 0 },
  { "clover r234: beat", "r234-beat", 96, 0, 0, 0 },
  // Round236 adds one row per waiting thread that a printed sample already names: the user frame
  // chain, sp, fp and lr. It has the same cadence as the bbwait rows and one row per thread, so the
  // same 192 covers it and the frame before the kill cannot be pushed out by the head of the boot.
  { "clover r236: bbustk", "r236-ustk", 192, 0, 0, 0 },
  // Round238 filters the ulock accounting by process name inside the hook, so no wait can be lost
  // to the pid latch, and prints all 12 slots blocked-first plus a counter row: 13 lines per beat,
  // 15 beats in a 1800 s boot, 224 holds the whole series. The counter row gets its own class so a
  // later row never pushes out the seen/filtered/stored/overflow attribution. The object-header row
  // still rides each printed sample (at most 33 of them).
  { "clover r238: ulock", "r238-ulock", 224, 0, 0, 0 },
  { "clover r238: ulockc", "r238-ulockc", 96, 0, 0, 0 },
  { "clover r237: bbobj", "r237-obj", 96, 0, 0, 0 },
  // Round239 adds two rows per blocked group wait to the sample that already prints bbobj: the
  // dispatch group's own state, and either the block->controller->definition chain the host proved
  // or its explicit absence. They have the same cadence as bbobj -- at most one per printed sample
  // per waiting thread -- so 96 covers a 30-minute boot. The positive and negative chain rows split
  // the budget because only one of the two is ever produced: whichever the device answers with can
  // fill its class, and neither can push the other out.
  { "clover r239: bbgrp", "r239-grp", 96, 0, 0, 0 },
  { "clover r239: bbchain", "r239-chain", 96, 0, 0, 0 },
  { "clover r239: bbnochain", "r239-nochain", 96, 0, 0, 0 },
  // Round243 prints the IODeviceTree plane once -- every node name the plane
  // iterator sees plus the four properties the homeButtonType question turns on
  // -- from the first printed backboardd sample. One-shot by a static guard,
  // so one beat's worth of lines: the whole census cannot exceed 64 records.
  { "clover r243: ", "r243", 64, 0, 0, 0 },
  // Round251 is the same instrument with its own truncation bug fixed: the 8-slot ring wrapped over
  // the earliest asks, which on boot 38 were exactly the ones that would have named backboardd. The
  // kernel now keeps 24 rows, prints them oldest first, and folds every ask into a per-process tally
  // printed before the rows. The burst is therefore one counter line, up to eight who lines and up
  // to 24 rows -- 33 at worst, so 48 leaves the same headroom r243 gives the census.
  // Round252 adds a second list to the same burst: every registry name a button-asking process
  // reached for and this board does not have. Boot 40 read that list back as "misses total=153
  // shown=16" -- sixteen rows that were four names repeated, soc-generation eight times over, so the
  // instrument spent its budget saying the same absence eight ways and still hid the other names.
  // Round254 keeps one row per distinct name with a count, up to 32 of them, and moves the live print
  // off the button rows onto the miss rows, because the census replays the rows anyway and the order
  // a name first went missing is the part no later dump can restore. Worst case is therefore one
  // total line, eight who lines, 24 rows, one misses line, 32 miss lines and 32 live miss lines --
  // 118 records, so the class budget goes from 64 to 128.
  { "clover r252: ", "r252-mgq", 128, 0, 0, 0 },
  // Round256 splits the same instrument's late reading into a class of its own, because boot 43 proved
  // the early one is unreadable: the card keeps the last 512 kernel-log rows, those rows started at
  // t=1208 s, and every verdict this probe printed came at t=25 s -- msgbuf had lapped it 24 times.
  // The beat line is one per 120 s of the generic ARM counter for the rest of the boot, so an 1800 s
  // deadline holds fifteen of them, and 24 bounds a producer that ticks harder (the driver calls the
  // checkpoint ten times a second while a press is held, which is why the gate is a clock and not a
  // count). It sits AHEAD of the bare "clover r255: " token below on purpose: teeProbeClass returns the
  // first class whose token appears in the line, so the specific row has to be tested first or the two
  // would spend each other's budget -- which is the whole point of splitting them.
  { "clover r255: beat", "r256-beat", 24, 0, 0, 0 },
  // Round255's input device speaks once at the end of its probe and then once per second from its own
  // timer while it is looking for the event driver, so the early lines are capped inside the probe at
  // eight: one stage line, two refusals and eight checkpoints -- eleven, so 16 leaves headroom.
  { "clover r255: ", "r255-hid", 16, 0, 0, 0 },
  // Round259 puts the touch bus instrument back in the boot, and it gets a class of its own for the
  // same reason the r254 census lesson says every producer needs a number: an unbounded line type is a
  // ring that starts late. The I2C probe's whole answer is a geometry line, an ACK line per candidate
  // address and at most one "rails unproven" verdict, so 24 is more than the burst can be, and the
  // cap keeps a bus that talks back from spending the boot's last 512 rows on itself.
  { "clover r181: ", "r181-i2c", 24, 0, 0, 0 },
  // Round260 re-publishes the touch bus verdict from state on every beat, and this is the class that
  // says how many times it was said. It is the reading that survived boot 47 when the burst above did
  // not: the instrument spoke at t=12 s, the ring window on that boot began at t=1194 s, and the card
  // came back with no answer at all. Fifteen beats fit the 1800 s deadline, so 16 bounds the same
  // producer without cutting it short.
  { "clover r260: bus", "r260-bus", 16, 0, 0, 0 },
  // Round262 adds the unfiltered census of the same task's threads: one header and up to fourteen rows,
  // re-stated every two minutes. It exists because r261's host forensics narrowed the boot's last dead
  // wait to one question the filtered wait rows cannot answer -- whoever owes that dispatch group its
  // leave is either asleep on that very word, which the census shows as a second sleeper on the same
  // address, or idle and never handed the block. Fifteen lines per beat inside an 1800 s deadline is
  // about 225 of them; a class that runs out mid-boot drops exactly the late readings the ring window
  // needs, so the budget bounds the producer without trimming its tail. Round263 widens each row (the
  // block deadline, the mach_msg2 names, and the owner of the send right -- the service an answer is
  // owed by) and the cap from fourteen rows to twenty with a hidden count, so the line count per beat
  // is still one header plus the rows, but twenty of them: boot 49 spent 5 beats and 75 lines in a window
  // that ends at 1786 s, and the same cadence against the 1800 s deadline with the widened cap is 273
  // lines -- 315 if the beat holds its nominal two minutes. 384 so the widest legal census cannot
  // reach its own tail.
  { "clover r262: ", "r262-th", 384, 0, 0, 0 },
  // Round265 fires a second census on the same 120 s clock, against the task that owes the answer boot 51
  // named. Its rows are capped at twelve, so a fire is one header plus at most twelve rows, thirteen lines:
  // the same fifteen fires that budgeted r262-th at 384 give 195 here, and 224 keeps the widest legal peer
  // census off its own tail without spending half again a ring on a process the boot may never even reach.
  { "clover r265: ", "r265-peer", 224, 0, 0, 0 },
  // Round266 walks one user stack per distinct continuation of the debtor, so the count per fire is the
  // candidate cap (three), not the row cap: fifteen fires on the census clock is 45 lines, and 64 leaves the
  // widest legal peer census short of its own tail.
  { "clover r266: ", "r266-stack", 64, 0, 0, 0 },
  // Round267 prints one row per waiter that carries a block hint, beside the census row it belongs to, so the
  // count per fire is bounded by the two row caps together (20 + 12) and by how many of those rows are actually
  // waiting. Boot 53 observed four census fires with 67 rows and two peer fires with 32, and waiters are a
  // subset of those: 192 covers that load twice over while still leaving two thirds of the card's last 512
  // kernel rows for r262-th and r265-peer, which are the rows this class is read alongside.
  { "clover r267: ", "r267-owner", 192, 0, 0, 0 },
  // Round268 adds a row only where round267's answer was incomplete: one per thread whose hint says semaphore,
  // because that is the kind whose owner field names a creator rather than a signaller. Boot 54 showed one such
  // waiter in the debtor and none in backboardd, so a fire costs a row or two; 64 covers twenty fires of the
  // pair of censuses, which is more beats than the 1800 s deadline allows.
  { "clover r268: ", "r268-sema", 64, 0, 0, 0 },
  // Round269 prints one row per census fire, not one per waiter: the sender-right search is latched, because
  // the scan it costs is a proc_find over pids 1..400 with an entry-table walk in each, and paying that for
  // every semaphore row would starve the beat. Two censuses times twenty fires is forty rows inside the
  // 1800 s deadline, so 64 is the same headroom r268 was given.
  { "clover r269: ", "r269-srhost", 64, 0, 0, 0 },
  // Round270 prints exactly one row per census fire -- the counters are global, so a copy per thread row
  // would be the same nine numbers printed fifteen times. Twenty fires inside the 1800 s deadline is half
  // of 64, which leaves the same headroom the last two rounds were given.
  { "clover r270: ", "r270-ipi", 64, 0, 0, 0 },
  // Round271 prints exactly one row per census fire, like r270: the cpu bookkeeping is global, so a copy
  // per thread row would print the same eight start/running bits fifteen times. The row is short (~110
  // bytes, longer once the per-cpu timer counters grow), and twenty fires inside the 1800 s deadline is
  // half of 64 -- the same headroom the last two rounds were given.
  { "clover r271: ", "r271-cpu", 64, 0, 0, 0 },
  // Round299 prints one row per census fire, beside r271's: the cpu row says how many cores XNU has, this one
  // says what this board's CPU driver did about it. Sixteen numbers per core would be a lot of rows if it were
  // one per thread, but it is not -- the arrays are global, so a fire costs exactly one row, and twenty fires
  // inside the 1800 s deadline is the same half of 64 r270 and r271 were given.
  { "clover r299: ", "r299-smp", 64, 0, 0, 0 },
  // Round300 prints two rows per census fire: one when the fire opens, one when it closes. Twenty fires inside
  // the 1800 s deadline is forty rows -- the same half of 64 r270, r271 and r299 were given for the same reason,
  // because these are per-fire producers and a fire costs a fixed two rows however many threads it walks. The
  // class is separate from r299's so a boot that reached the opening row but never wrote the closing one is not
  // read as a boot whose smp row went missing: the pair's job is exactly to say how far one fire got, and that
  // claim cannot be carried by a class that also holds the cpu row.
  { "clover r300: ", "r300-fire", 64, 0, 0, 0 },
  // Round272 spends a class on a line the kernel already prints, because the question boot 58 left
  // open is exactly "did XNU try": cpu_start() speaks once per attempted cpu, seven attempts is the
  // whole story of this board, and a class that never fills is itself the reading. 16 leaves room
  // for a retry loop without letting this producer crowd out the census.
  { "cpu_start()", "r272-cpustart", 16, 0, 0, 0 },
  // Round273 prints one row per thread the census finds parked on a workloop, which on boot 59 was two of the
  // debtor's seven threads and on backboardd's side none at all. It is a per-row producer like r267 rather than a
  // once-per-fire summary, so it gets the census budget: twenty fires times a couple of waiters fits in 64 with
  // headroom for the day a workloop has five waiters, which is the shape boot 58 reported for pid 58.
  { "clover r273: ", "r273-wl", 64, 0, 0, 0 },
  // Round274 prints one row per census fire, before the thread rows it exists to resolve: the peer's first
  // four address-space ranges. Four fires inside a deadline is the whole budget's worth of rows, so 32 is
  // generous by design -- the row is the only way to read the frames that are not in the shared cache.
  { "clover r274: ", "r274-map", 32, 0, 0, 0 },
  // Round275 measures the precondition of the only proposed fix, so it is a per-boot producer of a different
  // shape: one line for the two metaclass lookups, up to eight lines for the services whose name carries
  // "KeyStore", and one summary row carrying the walk's own reached/stopped count. The census is latched by
  // `static volatile int ran` and fires once past 1500 s, so the whole round is at most ten rows per boot --
  // 64 is the same headroom the r243 census got, not a per-beat budget, and deliberately wide because a row
  // that says nothing (class=0 and walked>0 with keystore_services=0) is the reading being bought here, and
  // it has to survive the ring rotation to be read at all.
  { "clover r275: ", "r275-aks", 64, 0, 0, 0 },
  // Round276 publishes the service and reports what publishing it did, again from inside the one census fire:
  // one stage/status row, up to eight per-service rows, one tally. Same 64 for the same reason -- the rows are
  // bounded by the census, not by the beat -- and the class is separate from r275-aks on purpose: a round that
  // runs is distinguishable from a round that was capped, and if the keystore rows ever starve the census rows
  // the two counters say which producer lost.
  { "clover r276: ", "r276-ks", 64, 0, 0, 0 },
  // Round281 replaces round280's single late table with a schedule, because boot 68 measured the other half
  // of the same lesson: the table spoke at 1600 s and the board was gone at 1324.217 s, so a reading with one
  // fixed speaking time only describes boots that outlive it. The probe now says a state row and a watchdog
  // row every 120 s from 240 s, and the keystore tables every fifth row (600 s). Wherever the boot stops, its
  // last row is at most two minutes old, which fits inside the 355 s the 512-record ring kept on boot 67.
  // 128 covers the whole schedule with room to spare: at most 15 state rows and 15 watchdog rows to the 1800 s
  // deadline, plus two table dumps of twenty-five rows each (four named selectors, eight opens, twelve calls,
  // one honesty row) is 105, and the cap is here so a runaway timer shows up as its own capped line instead of
  // crowding the census rows out. Separate from r276-ks so the two producers' counters stay apart.
  // Round292 samples the watchdog every four seconds and prints on a change, with a repeated control row on
  // every state fire so "no change" cannot be mistaken for "the timer died". The row moves out of r281-aks
  // into its own class because the schedule it now keeps is the only one on the board that can be made of
  // one-shot events: a bark that flips EN or STS once has to survive no matter how many keystore rows the
  // same boot produced. 48 covers fifteen control rows and a thirty-row burst, which is more flips than a
  // bite-and-reset boot can print, and the cap is where a chattering status register becomes its own reading.
  { "clover r281: wdt", "r281-wdt", 48, 0, 0, 0 },
  { "clover r281: ", "r281-aks", 128, 0, 0, 0 },
  // Round286 is the GPU line's first writer, and like r281 it is a schedule rather than one
  // speaking time: one GCC row every fire plus, on the boot that runs the power sequence, the
  // gcc-post/gpucc/gdsc/chipid rows, then the steady gpucc+chipid pair every later fire. At most a
  // handful of rows per fire and 15 fires to the deadline, so 64 is headroom rather than a promise;
  // separate from r281-aks so a GPU probe that starves cannot be mistaken for a keystore that went
  // quiet.
  { "clover r286: ", "r286-gpu", 64, 0, 0, 0 },
  // Round290 is the same schedule with one more row per fire: the branch row, which reads the halt
  // bit the r286 row never carried. Two rows per fire times fifteen fires to the deadline, so 32 is
  // headroom; the class is separate so a boot whose branches starved reads as that and not as a GPU
  // power sequence that never ran.
  { "clover r290: ", "r290-gpu", 32, 0, 0, 0 },
  // Round291 adds the LDO row to the same schedule: one more row per fire, so two minutes of headroom
  // again. Its class is separate because a boot that printed branches but not the rail row says the
  // GPUCC window answered at 0x1000-range offsets and not at 0x9000 -- which is a fact about the
  // window, and would be invisible if the row shared a budget with the one that proves it.
  { "clover r291: ", "r291-gpu", 32, 0, 0, 0 },
  // Round294 widens the rail reading to the whole LDO block, on the same schedule and with its own class
  // for the same reason r291 gave: a boot that printed the three words and not the thirteen says the
  // census was capped, not that the page is silent -- and the census is the baseline the next round's
  // writes would be judged against, so losing it silently is the one outcome this class exists to prevent.
  { "clover r294: ", "r294-gpu", 32, 0, 0, 0 },
  // Round311 adds the MEM_ACC corner row to the same schedule: one more row per fire, own class, own
  // exemption -- for the reason r291 gave. This row is the only place the L1 select is read, and it
  // has three shapes (map=0, gpucc=0, the full row) whose difference is the whole reading; a class
  // that capped the full row but kept the map=0 one would leave a boot saying "the page was refused"
  // when the page answered fifteen times.
  { "clover r311: ", "r311-gpu", 32, 0, 0, 0 },
  // Round317 adds one verdict row to the same schedule -- the census the thirteen words above are now
  // counted into, printed in the same fire from the same reads. Its own class and its own exemption, for
  // the reason r294 gave: this row is the interpretation of round294's baseline, and a boot that shows
  // the thirteen words and not the shape would read as "the census never ran" when what happened is that
  // a capped pass ate it. 32 covers fifteen fires of one row with a margin.
  { "clover r317: ", "r317-gpu", 32, 0, 0, 0 },
  // Round318 adds two rows per I2C segment: the raw I2C_STATUS/ERROR_FLAGS words with the layer's
  // decode beside them, and the verdict class that both failure branches and the retry policy are keyed
  // on. Own class for r291's reason -- and the pair is one reading, because round181's bus= column was
  // the masked word and could not show a held line; a boot that kept xfer and lost core would say the
  // bus was fine and never show why the branch reset. A boot runs a handful of segments, so 32 is
  // headroom for the pair at sixteen segments, not a promise about how many probes it takes.
  { "clover r318: ", "r318-input", 32, 0, 0, 0 },
  // Round319 re-says the r318 xfer/core raw words from state on the beat (the r260 pattern): the
  // one-time rows print at driver start and are overwritten by this ring's five or six laps before
  // readback, so the last sub-transfer's raw I2C_STATUS / ERROR_FLAGS / SDA / SCL are latched in
  // clover_input_last_xfer and printed once per beat by package/CloverHidInputProbe.cpp. One row per
  // 120 s beat, 16 bounds the same producer without cutting it short.
  { "clover r319: last", "r319-last", 16, 0, 0, 0 },
  // Round324's arm line: package/CloverKeyStoreProbe.cpp prints `clover r324: wdt-arm ...` exactly once,
  // on the first four-second fire past 300 s, when it writes BARK/BITE/EN to the APSS watchdog. Its own
  // class so an arm that speaks is never mistaken for the keystore census it rides beside; one line per
  // boot, so 8 is headroom not an expectation -- and if the boot never reaches 300 s the absence IS the
  // reading (the board died before arming, so there was no window for a hang-bite to matter). The prefix
  // is the specific `clover r324: wdt`, not `clover r324: `, so it does not shadow the bbwhy class below.
  { "clover r324: wdt", "r324-wdt", 8, 0, 0, 0 },
  // Round324's second half: osfmk/kern/task.c re-states, every 120 s beat through the driver relay AND
  // here on the console, who backboardd (pid 58) itself blocks on -- the reading boot 109's hops=2 row
  // promised and its change-gated bbhopo child never delivered to the tail. It is a state line, so the
  // budget is the beat count a WDT-shortened boot can produce (a little over 1800 s / 120 s), and 24
  // covers every fire without letting a repeating row crowd the census beside it.
  { "clover r324: bbwhy", "r324-bbwhy", 24, 0, 0, 0 },
  // Round326 placed the in-band debt row and boot 112 read it back on both channels, so the placement is
  // settled; round327 changes only what its four owner columns say. Boot 112's row read hn=4 with four
  // dashes and rem=0, and a dash then covered "no remote to name", "asked and found no owner" and "named
  // but the copy lost it" at once -- the one distinction the next hand needs. The slots are now literals,
  // asked=/named= count the two branches, and the row carries the four remote port names in the raw. The
  // channel and the budget are unchanged: same change-gated fire, capped by clover_bb_fired at 96, and a
  // board that survives to the 1800 s deadline can fire the census a dozen times, so 32 is headroom.
  { "clover r327: bbnow", "r327-bbnow", 32, 0, 0, 0 },
  // Round328 is a second row, not a rewrite: boot 113 read r327 back and its literals are honest, but they
  // describe the send column (x[3]'s low half), and a thread parked in a receive has no send destination --
  // so two nulls and one fail answer a question the row was never positioned to ask. This row asks the
  // receive column (hrn, the same pairing r221 prints as rn=) for the same hops and adds what a register
  // cannot give: who owns that port, whether mail already sits in its queue, and whether the helper reached
  // an object at all. It prints in the same burst as r327, so the two rows are each other's control, and the
  // budget matches because the cadence is identical (one row per change-gated fire, capped at 96).
  { "clover r328: bbrcv", "r328-bbrcv", 32, 0, 0, 0 },

  // Round331 asks, outward from the port r328 just named, who holds a right to it. Three rows, because the
  // answer is a list and a list has to say when it is short: one header per fire (32 -- the same cadence and
  // the same clover_bb_fired cap as r327/r328, so the three rows are each other's control), then at most four
  // names from the kernel's reverse lookup and at most four names from a complete walk of the waiter's own
  // table. The two holder classes are 128 because twenty-something fires times four is the arithmetic bound,
  // and the same arithmetic that sized r295's 128: a class that is exactly the product says nothing when the
  // product is what the cap ate. The three keys share no prefix (`bbsh` and `bbsend` differ at the fourth
  // letter), so the containment r329 had to order around does not exist here.
  { "clover r331: bbsend", "r331-bbsend", 32, 0, 0, 0 },
  { "clover r331: bbsh", "r331-bbsh", 128, 0, 0, 0 },
  { "clover r331: bbown", "r331-bbown", 128, 0, 0, 0 },

  // Round329 publishes the display client's state on the display's own 30 s beat, and the two rows here are
  // the widest that row can ever be plus the branch that says it did not fit. Order matters and is the only
  // defence: the cut row's token starts with the state row's token, so a table that listed the shorter key
  // first would send every `dispstatecut` into the `dispstate` budget -- the same containment r313 met with
  // two address tables, where reading the wrong one answers instead of refusing. 96 is arithmetic, not
  // optimism: the unattended deadline is 1800 s, so the beat can fire at most sixty times, and the probe
  // fires one extra row for each client it interposes (at most eight). 8 for the cut branch, which the
  // probe expects never to reach and prints only if the row outgrows the 320-byte record.
  { "clover r329: dispstatecut", "r329-dispstatecut", 8, 0, 0, 0 },
  { "clover r329: dispstate", "r329-dispstate", 96, 0, 0, 0 },

  // Round333 reads the swap-wait state once per selector-6 call, from the probe's own wrapper. Same
  // containment as r329 and the same defence: the cut row's token starts with the state row's token, so the
  // longer key has to be listed first or every `bbswapcut` is counted as a `bbswap`. The budget is boot 115's
  // count times headroom, not an expectation -- that boot had seven selector-6 calls by the last beat row and
  // reset at 814 s, so 64 covers a client that polls this wait an order of magnitude harder without letting a
  // runaway client crowd out the census rows sharing the pass. If the cap does fill, the tee's own "capped at"
  // line says so; the alternative, routing these rows through the probe's 96-line budget, would let the swap
  // rows spend the budget the beat row exists to keep.
  { "clover r333: bbswapcut", "r333-bbswapcut", 8, 0, 0, 0 },
  { "clover r333: bbswap", "r333-bbswap", 64, 0, 0, 0 },
  
  // Round313 is the first boot that runs drivers/CloverPower at all -- the instrument shipped in
  // round182 and has never been on a board -- so its lines have no class and no budget of their own
  // yet. The report is one-shot, printed in the PMGR tail beside the other bring-up bursts, and its
  // worst-case length is the APID listing (up to eight rows, a header and a self-counting summary),
  // the version block (three renderings of the same word), the floor-plan line, the window line, two
  // scan rows, two owner/ACC rows and the access-word pair: twenty-something lines, so 48 is
  // headroom, not an expectation. The class matters for the same reason r283's does: a report the cap
  // cut in half reads as a driver that stopped early, and here the two are different verdicts -- one
  // says the arbiter answered part of the question, the other says it answered none.
  //
  // Two classes, not one, because the report is printed under two tokens: round313's own lines carry
  // the new stamp and the round182 lines keep theirs, which is how a reader tells which round wrote
  // which number. Splitting the burst across two budgets is what the house rule costs, and it is
  // worth paying -- but then both halves need the same protection, or the untouched half is the one
  // the per-pass cap eats. "clover r182" appears in no other driver, so the token is unambiguous.
  { "clover r182: ", "r182-power", 48, 0, 0, 0 },
  { "clover r313: ", "r313-power", 48, 0, 0, 0 },
  // Round295 walks the send chain, at most one row per hop and five hops per fire, on the same clock as the
  // two censuses it is read beside. Twenty fires to the deadline is a hundred rows, so 128 is the same
  // headroom r265-peer was given rather than a promise about how deep this boot's chain goes: the row that
  // ends a chain is the one worth having, and a class that fills before the fifth hop would make depth_limit
  // and a dropped row read alike. Separate class, because a boot whose chain was capped has to say so without
  // taking the debtor census down with it.
  { "clover r295: ", "r295-chain", 128, 0, 0, 0 },
  // Round296 rewrites that row rather than adding one, so the r295 class above is kept as the history of the
  // prefix and the new rows land here with the same headroom for the same reason -- the row that ends a chain
  // is the one worth having, and a class that fills before the fifth hop would make depth_limit and a dropped
  // row read alike. The hop count did not change between the two rounds, so neither did the budget.
  { "clover r296: ", "r296-chain", 128, 0, 0, 0 },
  // Round297 added a second row per hop instead of widening the first, and Round298 rewrites that row
  // in place -- one row per hop, so the pair with r296 still joins on hop= and t=. The measured reason
  // for the separate row survives: the longest r296 row on boot 84 measured 227 bytes, and r297's three
  // pointers plus link=unknown added 79 of the 320-byte record. r298's row is that same row with eight more
  // columns (two return codes, vname=, tname=, tspace=, space=, otype=, tlink=), which formats at 225
  // bytes with every field at its widest (five 16-digit pointers, two -1 codes, link=unknown,
  // tlink=other_space) and about 205 with boot 86's own values -- still one row under the 320-byte
  // record, still truncated rather than split if that ever changes. The budget stays 128 and the
  // exemption below stays, for the reason
  // boot 86 found: the leaf's identity row is the one worth keeping, and qmc=/qsr= share a translation
  // with it, so a class that lost hop=3's row would leave the card holding a zero that cannot be
  // attributed to anything.
  { "clover r298: ", "r298-chain", 128, 0, 0, 0 },
  // Round303 prints one row per chain hop whose owner resolves and whose sender name translated -- the receiving
  // task's own entry census, the row that answers what tlink=no left open. It is a per-hop producer exactly like
  // r298 (at most one row per hop, so a fire of depth 4 costs 4 rows), and it shares r298's fate if throttled: this
  // is the reading the whole round was written for, and a capped r303 row reads as a hop that never reached the
  // receiving side -- the same false-empty r298's 128 budget and exemption were set to stop. Same budget, same
  // exemption below.
  { "clover r303: ", "r303-hop", 128, 0, 0, 0 },
  // Round306 prints one row per census fire, between the three counters and the fire row, and it is the
  // only copy of "the walk reached this statement" that does not ride the relay being audited. Its value
  // is entirely in being present or absent beside the fire row, so a throttled step row would restore the
  // exact ambiguity r304 and r305 were written to remove. One row per fire, so 64 is headroom for an
  // 1800 s deadline, and the class is separate so a missing step row cannot read as a missing census.
  { "clover r306: ", "r306-step", 64, 0, 0, 0 },
  // Round308 speaks on the relay first and prints the same bytes as its second leg: at most one beat
  // row every two minutes (fifteen to the deadline) and two rows per census fire from inside the core
  // count block. The printf copy is the relay row's control, so the per-pass cap must not be allowed to
  // eat it -- a boot that lost the printed copy to throttling would read as a relay that sealed a row
  // the console never saw, which is the exact ambiguity this round exists to remove rather than rejoin.
  { "clover r308: ", "r308-relay", 64, 0, 0, 0 },
  // Round289 speaks only at a fatal data abort, and it is the reading the panic record cannot carry:
  // the kind-20 slot is 480 bytes, and boot 74 and boot 75 both showed it cut off inside the register
  // list, before esr, far, fp and sp -- the four fields that name a fault. At most one abt row and two
  // bt rows per abort, so 32 is not a cap but a headroom; the class is separate so a truncated trace
  // reads as a truncated trace rather than as a GPU or keystore row missing.
  { "clover r289: ", "r289-abt", 32, 0, 0, 0 },
  // Round283 prints at most one line per boot, during the bring-up pass that arms the slot, and it
  // is the whole reading: a reset that got a name, or the proof that none did. That pass carries the
  // census and the GPT walk, so the per-pass cap can eat the one row the frame exists for -- and a
  // dropped row would read exactly like a boot whose slot was never touched.
  { "clover r283: ", "r283-reset", 16, 0, 0, 0 },
};
static const uint32_t kTeeProbeClassCount =
    sizeof (kTeeProbeClasses) / sizeof (kTeeProbeClasses[0]);

static volatile uint32_t gTeeReady = 0;         // arms round176's kernel-log ring
static uint32_t gTeeIndex = 0;                  // next byte of msgbuf to consume
static uintptr_t gTeeBuffer = 0;                // msg_bufc the cursor belongs to
static uint32_t gTeePartial = 0;                // bytes held for a line that has not ended yet
static char gTeeLine[kTeeLineBytes];
static uint32_t gTeeSeen = 0;                   // complete lines examined
static uint32_t gTeeKept = 0;                   // lines put into the ring
static uint32_t gTeeThrottled = 0;              // matches dropped by the per-pass cap
static uint32_t gTeeLap = 0;                    // passes where the writer was behind our cursor
static uint32_t gTeeRefused = 0;                // reason code: see teeValidate below
static uint32_t gTeeRefusedShown = 0;
// Round171's cache state, declared here because round209's panic sink reports it and the sink is
// emitted above the cache bring-up; both are written once, by that bring-up.
static volatile uint32_t gCacheSize = 0;        // the card's write cache in 512-byte units
static volatile uint32_t gCacheOn = 0;          // whether bring-up managed to switch it on
static uint32_t gTeeNextReport = 256;           // emit a tee census every this many lines examined
static uint32_t gTeeCensus = 0;                 // census lines said so far this boot
// The census is the tee talking about itself, so it lands in the driver's ring -- which round176 grew
// to 64 records, but still only holds the readings this driver made. Twelve is enough to see the
// counters climb across a boot and is the difference between that ring holding the nine verdicts and
// holding thirty copies of the same four numbers.
static const uint32_t kTeeCensusMax = 12;
// Kernel virtual addresses on this board all sit above the Darwin arm64e base (the kernel links at
// VM_KERNEL_LINK_ADDRESS, 0xfffffff007004000). A msg_bufc below it is not a kernel pointer at all, and
// following it would take a data abort in the middle of a block tail -- which is the one place in this
// driver that must not fault, because the card is holding the caller's I/O.
static const uint64_t kTeeKernelVaBase = 0xfffffff000000000ull;

// Both are declared here because the first verdict line is printed well below the definition; the
// round174 section installs them. sdhciSay is the driver's only way of speaking to a reading: it
// formats once and hands the same string to the panel and to the RAM ring.
static void sdhciSay (const char *fmt, ...);
static void ringPush (uint32_t which, const char *line, uint32_t length);
static void flushVerdictRing (void);
// Round175: drain the kernel log buffer into the kernel ring. Declared with the rings it feeds
// because every call site is a line above its own flush, in the same guarded tail.
static void teeKernelLog (void);
// Round184: the panic sink drains both RAM rings before the board resets itself into fastboot.
// Defined below the tee it calls, used by `clover_emmc_panic_append` at the bottom of this file.
static void panicDrainRings (void);
// Round309: the drain's first breath, written before that function reads a single gate.
static bool flightPanicDrainIn (void);
// Round310: lay the controller down before the sink asks it for anything. Defined with the sink.
static void panicQuiesceCard (void);
// Round304: the probe ring's consumer. Declared with the drain it replaces as the only caller, because
// its two new call sites -- a guarded block tail and CPU0's beat -- are both far above its definition.
static uint32_t probeFlush (const char *why, uint32_t cap);
// Round198: the BCB is written here, at the end of the GPT walk, and only re-checked by the panic
// sink. Defined further down with the card-write helpers it uses.
static unsigned int panicMarkRecovery (void);
static uint32_t gPanicDrained = 0;              // one drain per boot; the sink can be called again
static uint32_t gPanicDrainWritten = 0;         // records this drain put on the card
static uint32_t gPanicDrainDropped = 0;         // oldest records it gave up to keep the newest
// Round211: the panic text's own return code, carried into the status line. Round210's frame wrote the
// rings but no kind-20 record, and without this number the reader could not tell a failed slot scan
// from a write that never got its turn.
static uint32_t gPanicAppendRc = 0xffffffffu;
// Round301: the sink's own sentence, kept in RAM whatever the append returns.
//
// Boots 89 and 90 both reached this function -- each carries drain enter boot=NN append_rc=2, and
// panicDrainRings() has exactly one caller -- and neither left a kind-20 record, because
// panicAppendText() reads the report window before it writes anything (it has to find the oldest
// slot) and that read was refused while the host was owned (boot 90's marker says host=0x20fdf).
// The return code 2 is therefore "the card said no to the read", not "a gate was closed" (rc=1),
// and it is a one-shot: the sink tries once and never comes back. The sentence the screen passed in
// is the one reading that names this board's endings, and on the two boots that had it, it was lost
// at the last step.
//
// The RAM ring is the copy that does not use the tested pipe: a row queued here is retried by every
// later flush, and those rows demonstrably reach the card -- boot 90's own drain rows (drain enter,
// panic tail, r184, tee filter, drain exit) are on it. So the text is copied before anything can
// refuse, and the drain below decides whether to say it.
static const uint32_t kPanicStashBytes = 480;   // the same cap the report record's body carries
static const uint32_t kPanicStashPart = 240;    // a verdict row is kVerdictTextBytes=320 wide
// Round302: the blind record is one 512-byte block, and three words of it are header (characters,
// the append's return code, the boot id), so the sentence's own room is 88 words. 352 of the 480
// characters is what fits in the block that needs no second chance; the ring copy keeps all 480,
// and a record that truncates says so by carrying characters=480 with a length that fits.
static const uint32_t kPanicStashBlindBytes = 352;
static char gPanicStash[kPanicStashBytes + 1];
static uint32_t gPanicStashLength = 0;          // characters copied out of the sink's argument
static uint32_t gPanicStashSaid = 0;            // parts this drain queued (0, 1 or 2)
static uint32_t gPanicBlindChars = 0;           // characters the blind record carried
static uint32_t gPanicBlindOk = 0;              // 1 once the blind block reached the controller
// Round309: whether the drain's own first record reached the card. The absence of the record is a
// reading only if something says whether the code that writes it ran, and this is that something --
// r305 put the last refusal's words in the marker, and this word names whether this write was it.
static uint32_t gPanicDrainInOk = 0;            // 1 once the drain-in record reached the controller
// Round310: what the sink did to the card before it asked the card for anything. Boot 102 entered the
// sink, wrote its first marker and never wrote a second one: every step after that first marker rides
// the same forced raw-PIO path that had just succeeded, so the four absences name one call that did
// not return rather than four refusals. The last request record and that marker carry the same
// millisecond, and the beat path that would have ended that request had not written for 81 s, so the
// sink's own first poll was waiting on a request whose holder the panic had just stopped. These two
// words say whether the sink put the controller down first and what it found when it did.
static volatile uint32_t gPanicQuiesce = 0;     // 7 no host, 0 nothing outstanding, 1/2 stole+ok/refused, 3/4 no holder+ok/refused, 5 steal lost, 6 holder appeared
static volatile uint32_t gPanicQuiesceStage = 0;  // the stage the sink found at its own door
static volatile uint32_t gPanicQuiesceOwner = 0;  // the ticket held at that door, 0 when nobody did

// ------------------------------------------------------------------ the window and its reads
static OSSharedPtr<IOMemoryMap> gHostMap;
static uintptr_t gHost = 0;
// Mode 5 also needs the SDCC1 clock source, which is in GCC, not in this 4 KiB window.
static OSSharedPtr<IOMemoryMap> gGccMap;
static uintptr_t gGcc = 0;

// The panic screen can hand this driver text this boot never assembled itself: after formatting
// the panic message it calls clover_emmc_panic_append by address. That call is only safe while
// the card is still selected and the host is exactly as the last logfs pass left it, so the
// function refuses unless this boot has both matched the logfs extent and completed a logfs
// read. A panic before that point writes nothing instead of driving an unselected card.
static uint32_t gPanicLogFirst = 0;
static volatile uint32_t gPanicLogReady = 0;

// Round196. The unattended half that makes a panic become a recovery landing: the contract is only
// on when the boot argument armed it, and the BCB is written at most once per boot so a panic that
// is reported twice cannot turn two attempts into two rewrites of the same partition.
static volatile uint32_t gUnattendedArmed = 0;
static uint32_t gBcbWritten = 0;

static uintptr_t mapRange (OSSharedPtr<IOMemoryMap> &slot, uint64_t phys, uint64_t length)
{
  IODeviceMemory *memory = IODeviceMemory::withRange (phys, length);
  if (!memory) {
    return 0;
  }
  slot = OSSharedPtr<IOMemoryMap> (memory->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  memory->release ();
  return slot ? static_cast<uintptr_t> (slot->getVirtualAddress ()) : 0;
}

static uint32_t read32 (unsigned offset)
{
  uint32_t value = *reinterpret_cast<volatile uint32_t *> (gHost + offset);
  __asm__ volatile ("dsb sy" ::: "memory");
  return value;
}

static uint16_t read16 (unsigned offset)
{
  uint16_t value = *reinterpret_cast<volatile uint16_t *> (gHost + offset);
  __asm__ volatile ("dsb sy" ::: "memory");
  return value;
}

static uint8_t read8 (unsigned offset)
{
  uint8_t value = *reinterpret_cast<volatile uint8_t *> (gHost + offset);
  __asm__ volatile ("dsb sy" ::: "memory");
  return value;
}

// Mode 2 is the first code here that writes. Same window, same rule: the barrier follows every
// access, so a command register write cannot be reordered ahead of the argument it sends.
static void write32 (unsigned offset, uint32_t value)
{
  *reinterpret_cast<volatile uint32_t *> (gHost + offset) = value;
  __asm__ volatile ("dsb sy" ::: "memory");
}

static void write16 (unsigned offset, uint16_t value)
{
  *reinterpret_cast<volatile uint16_t *> (gHost + offset) = value;
  __asm__ volatile ("dsb sy" ::: "memory");
}

static void write8 (unsigned offset, uint8_t value)
{
  *reinterpret_cast<volatile uint8_t *> (gHost + offset) = value;
  __asm__ volatile ("dsb sy" ::: "memory");
}

// Base clock and divider as SDHCI defines them, not as round87's report printed them. That line
// was wrong three ways: it masked 13:8 (the 6-bit SDHCI 1.0 field) so 0xc8 read back as 8, it
// scaled MHz by 100000 instead of 1000000, and it called divider 0 "0 MHz" when 0 is the base
// clock itself. The raw registers were right all along; only the label lied.
static void clockFacts (uint32_t capabilities, uint16_t clockControl,
                        unsigned &baseMhz, unsigned &divider, unsigned &sdClockHz)
{
  baseMhz = static_cast<unsigned>((capabilities & SDHCI_CLOCK_BASE_MASK) >> SDHCI_CLOCK_BASE_SHIFT);
  // 10-bit divided clock mode: 15:8 is the low eight bits, 7:6 are the top two.
  divider = static_cast<unsigned>((clockControl >> SDHCI_DIVIDER_SHIFT) & 0xff) |
            static_cast<unsigned>(((clockControl >> 6) & 0x3) << 8);
  sdClockHz = divider ? (baseMhz * 1000000U) / (2U * divider) : baseMhz * 1000000U;
}

// ------------------------------------------------------------------ the report, by hand
//
// Freestanding on purpose: this runs before any process exists, so there is no os_printf into a
// buffer to lean on, and the panic screen reads the slots of a format string rather than a
// stream. Widths are fixed so a photograph compares against the next one digit for digit.
static unsigned gReportLength = 0;

static void emitByte (char c)
{
  if (gReportLength < sizeof(clover_emmc_report) - 1) {
    clover_emmc_report[gReportLength++] = c;
    clover_emmc_report[gReportLength] = 0;
  }
}

static void emitText (const char *text)
{
  while (*text) {
    emitByte (*text++);
  }
}

static void emitHex (uint64_t value, unsigned digits)
{
  for (unsigned index = digits; index-- > 0;) {
    unsigned nibble = static_cast<unsigned>((value >> (index * 4)) & 0xf);
    emitByte (static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + nibble - 10));
  }
}

static void emitDec (uint32_t value)
{
  char digits[10];
  unsigned count = 0;
  do {
    digits[count++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value && count < sizeof(digits));
  while (count) {
    emitByte (digits[--count]);
  }
}

static void emitField (const char *label, uint64_t value, unsigned digits)
{
  emitByte (' ');
  emitText (label);
  emitByte ('=');
  emitHex (value, digits);
}

__attribute__((noreturn)) static void emmcFail (unsigned stage, uint64_t detail)
{
  clover_emmc_observation[15] = detail;
  clover_emmc_stage = stage;
  panic ("CloverSdhci: failed at stage %u detail %llx", stage, static_cast<unsigned long long> (detail));
}

// ------------------------------------------------------------------- mode 1: look, do not touch
//
// The register set is the standard SDHCI system address block, which is what a qcom,sdhci-msm-v5
// controller keeps at its hc_mem base, plus the two candidate Qualcomm version registers. The
// response words and the argument registers are read too: they are the firmware's last
// transaction, and they cost nothing.
static void inspectHost (void)
{
  uint32_t versionWord = read32 (SDHCI_SLOT_INT_STATUS);   // 15:0 slot interrupt, 31:16 host version
  uint16_t hostVersion = static_cast<uint16_t> (versionWord >> 16);
  uint16_t slotStatus = static_cast<uint16_t> (versionWord & 0xffff);
  uint32_t capabilities = read32 (SDHCI_CAPABILITIES);
  uint32_t capabilities1 = read32 (SDHCI_CAPABILITIES_1);
  uint32_t presentState = read32 (SDHCI_PRESENT_STATE);
  uint32_t interruptStatus = read32 (SDHCI_INT_STATUS);
  uint32_t interruptEnable = read32 (SDHCI_INT_ENABLE);
  uint32_t signalEnable = read32 (SDHCI_SIGNAL_ENABLE);
  uint8_t hostControl = read8 (SDHCI_HOST_CONTROL);
  uint8_t powerControl = read8 (SDHCI_POWER_CONTROL);
  uint16_t clockControl = read16 (SDHCI_CLOCK_CONTROL);
  uint16_t hostControl2 = read16 (SDHCI_HOST_CONTROL2);
  uint8_t timeoutControl = read8 (SDHCI_TIMEOUT_CONTROL);
  uint8_t softwareReset = read8 (SDHCI_SOFTWARE_RESET);
  uint16_t blockSize = read16 (SDHCI_BLOCK_SIZE);
  uint16_t blockCount = read16 (SDHCI_BLOCK_COUNT);
  uint16_t transferMode = read16 (SDHCI_TRANSFER_MODE);
  uint16_t command = read16 (SDHCI_COMMAND);
  uint32_t argument = read32 (SDHCI_ARGUMENT);
  uint32_t argument2 = read32 (SDHCI_ARGUMENT2);
  uint32_t response0 = read32 (SDHCI_RESPONSE);
  uint32_t response1 = read32 (SDHCI_RESPONSE + 4);
  uint32_t response2 = read32 (SDHCI_RESPONSE + 8);
  uint32_t response3 = read32 (SDHCI_RESPONSE + 12);
  uint32_t autoCommand = read32 (SDHCI_AUTO_CMD_STATUS);
  uint32_t maxCurrent = read32 (SDHCI_MAX_CURRENT);
  // The Qualcomm version register in both of the two layouts sdhci-msm.c knows
  // (sdhci-msm.c:206 and :239). Whichever one is a register answers plausibly: an SDCC version is
  // a small number with the major in 15:12-ish and the minor lower down, not a pointer.
  uint32_t mciVersionHere = read32 (0x050);
  uint32_t mciVersionThere = read32 (0x318);
  uint32_t dllConfigHere = read32 (0x100);
  uint32_t dllStatusHere = read32 (0x108);
  uint32_t dllConfigThere = read32 (0x200);
  uint32_t dllStatusThere = read32 (0x208);
  uint32_t genericsHere = read32 (0x070);
  uint32_t genericsThere = read32 (0x320);

  // Base clock in MHz and the divider, from clockFacts(): round87's report had all three of its
  // derived numbers wrong (bad mask, bad unit, divider 0 called stopped), which this one shares
  // instead of repeating.
  unsigned baseClock = 0, divider = 0, sdClockHz = 0;
  clockFacts (capabilities, clockControl, baseClock, divider, sdClockHz);

  clover_emmc_observation[0] = capabilities;
  clover_emmc_observation[1] = capabilities1;
  clover_emmc_observation[2] = presentState;
  clover_emmc_observation[3] = interruptStatus;
  clover_emmc_observation[4] = hostVersion;
  clover_emmc_observation[5] = clockControl;
  clover_emmc_observation[6] = argument;
  clover_emmc_observation[7] = response0;
  clover_emmc_observation[8] = response3;
  clover_emmc_observation[9] = mciVersionHere;
  clover_emmc_observation[10] = mciVersionThere;
  clover_emmc_observation[11] = sdClockHz;
  clover_emmc_observation[12] = signalEnable;
  clover_emmc_observation[13] = interruptEnable;
  clover_emmc_observation[14] = hostControl2;

  gReportLength = 0;
  // panic("CloverSdhci: %s") already prints the driver's name, so the report starts with what it
  // measured instead of repeating it.
  emitText ("look-only dump of c0c4000\n");
  emitField ("cap", capabilities, 8);
  emitField ("cap2", capabilities1, 8);
  emitField ("ver", hostVersion, 4);
  emitText ("\npst");
  emitHex (presentState, 8);
  emitField ("int", interruptStatus, 8);
  emitField ("en", interruptEnable, 8);
  emitField ("sig", signalEnable, 8);
  emitText ("\nhcl");
  emitHex (hostControl, 2);
  emitField ("pw", powerControl, 2);
  emitField ("clk", clockControl, 4);
  emitField ("hcn", hostControl2, 4);
  emitField ("tmo", timeoutControl, 2);
  emitField ("rst", softwareReset, 2);
  emitText ("\nblk");
  emitHex (blockSize, 4);
  emitField ("cnt", blockCount, 4);
  emitField ("tm", transferMode, 4);
  emitField ("cmd", command, 4);
  emitText ("\narg");
  emitHex (argument, 8);
  emitField ("a2", argument2, 8);
  emitField ("acmd", autoCommand, 8);
  emitField ("max", maxCurrent, 8);
  emitText ("\nrsp");
  emitHex (response0, 8);
  emitByte (' ');
  emitHex (response1, 8);
  emitByte (' ');
  emitHex (response2, 8);
  emitByte (' ');
  emitHex (response3, 8);
  emitText ("\nclk");
  emitHex (baseClock, 3);
  emitText ("MHz div");
  emitDec (divider);
  emitText (" sd");
  emitDec (sdClockHz / 1000000U);
  emitText ("MHz slot");
  emitHex (slotStatus, 4);
  emitText ("\nmsm v50/318 ");
  emitHex (mciVersionHere, 8);
  emitByte ('/');
  emitHex (mciVersionThere, 8);
  emitText ("\ndll 100/200 ");
  emitHex (dllConfigHere, 8);
  emitByte ('/');
  emitHex (dllConfigThere, 8);
  emitText ("\ndsts 108/208 ");
  emitHex (dllStatusHere, 8);
  emitByte ('/');
  emitHex (dllStatusThere, 8);
  emitText ("\ngen 70/320 ");
  emitHex (genericsHere, 8);
  emitByte ('/');
  emitHex (genericsThere, 8);
  emitByte ('\n');
  clover_emmc_stage = 3;
}

// ------------------------------------------------------------------- mode 2: one command, no data
//
// CMD13 SEND_STATUS is the smallest command an eMMC answers: a 48-bit R1 response, no data phase,
// no card state change. That makes it the one measurement that separates the three things a first
// write to this controller could break -- the command register path, the response path, and the
// card's willingness to talk -- from the data path, which comes after the RCA is known. The host
// is left exactly as the firmware configured it (200 MHz, 8-bit, 1.8 V, DLL locked); the only
// registers written are the three a command requires. Round88's photograph read back
// "timeout: no rsp", so this mode stays as the recorded control for address 1.
static const uint32_t kProbeRca = 1;
// The controller's own command timeout is 2^14 TMCLK. This host's capabilities low byte is 0xb2:
// bits 5:0 = 0x32 = 50, bit 7 = the unit = MHz (Linux SDHCI_TIMEOUT_CLK_UNIT, sdhci.h), so TMCLK
// is 50 MHz and the timeout is about 0.33 ms. The bound has to outlast that, or a real card
// timeout would be reported as our loop giving up. Interrupts are masked this early, so this loop
// is the only clock there is: a bound, not a duration.
static const uint32_t kCommandSpinLimit = 1u << 22;
// The pause between two CMD1 polls while a card powers up. It is a spin, so it is a lower bound
// and not 10 ms; EDK2's equivalent is a real timer stall.
static const uint32_t kCmd1Gap = 1u << 18;

static void cmd13Probe (void)
{
  uint32_t capabilities = read32 (SDHCI_CAPABILITIES);
  uint16_t clockControl = read16 (SDHCI_CLOCK_CONTROL);
  uint32_t presentBefore = read32 (SDHCI_PRESENT_STATE);
  uint32_t statusBefore = read32 (SDHCI_INT_STATUS);
  uint32_t status = 0, response0 = 0, response1 = 0, spins = 0;
  const char *verdict = "answered";

  clover_emmc_stage = 4;
  if (presentBefore & (SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT)) {
    // Round87 read both inhibit bits clear. If that changed, the firmware left a transfer open
    // after all and the honest move is to say so rather than write a command into it.
    verdict = "not sent: busy";
  } else {
    // Write-1-to-clear only the command group. A stale Command Complete would otherwise be read
    // as this command's response; everything else in the register is left for the report.
    write32 (SDHCI_INT_STATUS, SDHCI_INT_RESPONSE | SDHCI_INT_TIMEOUT | SDHCI_INT_CRC |
                               SDHCI_INT_END_BIT | SDHCI_INT_INDEX);
    write32 (SDHCI_ARGUMENT, kProbeRca << 16);
    clover_emmc_stage = 5;
    // R1 (48-bit, checked): what a SEND_STATUS answer has to be, and letting the controller check
    // the CRC and the echoed index is what makes "garbage on the bus" distinguishable from a card
    // that simply did not answer.
    write16 (SDHCI_COMMAND, SDHCI_MAKE_CMD (13, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                                SDHCI_CMD_INDEX));
    for (;;) {
      status = read32 (SDHCI_INT_STATUS);
      if (status & SDHCI_INT_CMD_MASK) {
        break;
      }
      if (++spins >= kCommandSpinLimit) {
        break;
      }
    }
    response0 = read32 (SDHCI_RESPONSE);
    response1 = read32 (SDHCI_RESPONSE + 4);
    if (status & SDHCI_INT_TIMEOUT) {
      verdict = "timeout: no rsp";
    } else if (status & (SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX)) {
      verdict = "rsp error";
    } else if (!(status & SDHCI_INT_RESPONSE)) {
      verdict = "no completion";
    }
  }

  uint32_t presentAfter = read32 (SDHCI_PRESENT_STATE);
  unsigned baseClock = 0, divider = 0, sdClockHz = 0;
  clockFacts (capabilities, clockControl, baseClock, divider, sdClockHz);

  gReportLength = 0;
  clover_emmc_observation[0] = status;
  clover_emmc_observation[1] = response0;
  clover_emmc_observation[2] = response1;
  clover_emmc_observation[3] = presentBefore;
  clover_emmc_observation[4] = presentAfter;
  clover_emmc_observation[5] = spins;

  emitText ("CMD13 rca=1 ");
  emitText (verdict);
  emitText ("\n int");
  emitHex (status, 8);
  emitText (" before");
  emitHex (statusBefore, 8);
  emitText (" spins");
  emitDec (spins);
  emitText ("\n rsp");
  emitHex (response0, 8);
  emitByte (' ');
  emitHex (response1, 8);
  // R1 bits 12:9 are CURRENT_STATE (4 = transfer, 7 = ready-for-data), bit 8 is READY_FOR_DATA,
  // 15:0 carry the error and status flags. Decoded here so the photograph does not need the spec.
  emitText ("\n R1 state=");
  emitDec ((response0 >> 9) & 0xf);
  emitText (" ready=");
  emitDec ((response0 >> 8) & 1);
  emitText (" err=");
  emitHex (response0 & 0xffff, 4);
  emitText ("\n clk");
  emitHex (baseClock, 3);
  emitText ("MHz div");
  emitDec (divider);
  emitText (" sd");
  emitDec (sdClockHz / 1000000U);
  emitText ("MHz\n pst");
  emitHex (presentBefore, 8);
  emitText (" -> ");
  emitHex (presentAfter, 8);
  emitText ("\n cmd0d1a arg");
  emitHex (kProbeRca << 16, 8);
  emitByte ('\n');
  clover_emmc_stage = 6;
}

// ------------------------------------------------------------------- mode 3: which RCA
//
// Round88 wrote CMD13 to RCA 1 and the controller reported INT_TIMEOUT: the command went out,
// nothing drove the CMD line back, and no CRC/index error says the line was noisy. For eMMC that
// is exactly what a non-matching RCA looks like -- the card compares the address and stays quiet.
// The firmware assigned the RCA during its own identification and leaves no register holding it,
// so the only way to read it is to ask each address in turn. CMD13 is read-only and legal in
// stand-by and transfer, so the sweep cannot change the card's state.
//
// EDK2's SdMmcPciHcDxe computes `Rca = Slot + 1` (EmmcDevice.c, EmmcIdentification), so slot 0
// would be 1 -- the address round88 already tried. Either this firmware's SdccDxe is not that
// stack, or the card is not in a state that answers. The sweep cannot tell those apart on its
// own, but a hit at any other address settles the RCA question outright; no hit rules out every
// plausible address at this clock and sends the next round to a full re-identification.
//
// Order: 1..15, then 0. 1 goes first so this run also retests round88's address under exactly
// the same conditions -- its timeout could have been the first command after handoff rather than
// the address, and one more timeout is cheap. 0 is last because it is not a valid RCA; a card that
// answered it must not mask a real address earlier in the list. Every attempt clears only the
// command group of INT_STATUS, and each is preceded by a bounded wait for CMD_INHIBIT to drop,
// because a timed-out command holds the register for a moment and a second command written into it
// would be a different experiment.
//
// The spin bound is mode 2's: the controller's own command timeout is 2^14 TMCLK at 50 MHz,
// about 0.33 ms, and 1 << 22 reads is orders of magnitude past that. An attempt that hits the
// bound without an event is therefore our loop giving up, not the card timing out, and the
// report keeps those two apart.
static void cmd13Sweep (void)
{
  static const uint8_t kAddresses[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0};
  uint32_t capabilities = read32 (SDHCI_CAPABILITIES);
  uint16_t clockControl = read16 (SDHCI_CLOCK_CONTROL);
  uint32_t presentBefore = read32 (SDHCI_PRESENT_STATE);
  uint32_t statusBefore = read32 (SDHCI_INT_STATUS);
  uint32_t response0 = 0, response1 = 0, lastStatus = 0, spins = 0, attempts = 0;
  uint32_t timeouts = 0, responseErrors = 0, silent = 0, busySkipped = 0;
  int found = -1;

  clover_emmc_stage = 4;
  for (unsigned index = 0; index < sizeof (kAddresses); index++) {
    uint32_t rca = kAddresses[index];
    uint32_t guard = 0;
    while (read32 (SDHCI_PRESENT_STATE) & SDHCI_CMD_INHIBIT) {
      if (++guard >= kCommandSpinLimit) {
        break;
      }
    }
    if (read32 (SDHCI_PRESENT_STATE) & (SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT)) {
      busySkipped++;
      continue;
    }
    // Only the command group: a stale Command Complete would otherwise be read as this
    // address's answer, and the data/interrupt groups are not ours to clear.
    write32 (SDHCI_INT_STATUS, SDHCI_INT_RESPONSE | SDHCI_INT_TIMEOUT | SDHCI_INT_CRC |
                               SDHCI_INT_END_BIT | SDHCI_INT_INDEX);
    write32 (SDHCI_ARGUMENT, rca << 16);
    clover_emmc_stage = 5;
    write16 (SDHCI_COMMAND, SDHCI_MAKE_CMD (13, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                                SDHCI_CMD_INDEX));
    uint32_t status = 0;
    spins = 0;
    for (;;) {
      status = read32 (SDHCI_INT_STATUS);
      if (status & SDHCI_INT_CMD_MASK) {
        break;
      }
      if (++spins >= kCommandSpinLimit) {
        break;
      }
    }
    attempts++;
    lastStatus = status;
    if ((status & SDHCI_INT_RESPONSE) &&
        !(status & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX))) {
      found = static_cast<int> (rca);
      response0 = read32 (SDHCI_RESPONSE);
      response1 = read32 (SDHCI_RESPONSE + 4);
      break;
    }
    if (status & SDHCI_INT_TIMEOUT) {
      timeouts++;
    } else if (status & (SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX)) {
      responseErrors++;
    } else {
      silent++;
    }
  }

  uint32_t presentAfter = read32 (SDHCI_PRESENT_STATE);
  unsigned baseClock = 0, divider = 0, sdClockHz = 0;
  clockFacts (capabilities, clockControl, baseClock, divider, sdClockHz);

  gReportLength = 0;
  clover_emmc_observation[0] = found < 0 ? 0xffffffffULL : static_cast<uint64_t> (found);
  clover_emmc_observation[1] = response0;
  clover_emmc_observation[2] = response1;
  clover_emmc_observation[3] = presentBefore;
  clover_emmc_observation[4] = presentAfter;
  clover_emmc_observation[5] = lastStatus;
  clover_emmc_observation[6] = attempts;
  clover_emmc_observation[7] = timeouts;
  clover_emmc_observation[8] = responseErrors;
  clover_emmc_observation[9] = silent;
  clover_emmc_observation[10] = busySkipped;
  clover_emmc_observation[11] = spins;

  emitText ("CMD13 sweep ");
  if (found >= 0) {
    emitText ("answered rca=");
    emitDec (static_cast<uint32_t> (found));
  } else {
    emitText ("none: 0..15 silent");
  }
  emitText ("\n int last");
  emitHex (lastStatus, 8);
  emitText (" before");
  emitHex (statusBefore, 8);
  emitText (" tries");
  emitDec (attempts);
  emitText (" spins");
  emitDec (spins);
  emitText ("\n rsp");
  emitHex (response0, 8);
  emitByte (' ');
  emitHex (response1, 8);
  emitText ("\n R1");
  if (found >= 0) {
    emitText (" state=");
    emitDec ((response0 >> 9) & 0xf);
    emitText (" ready=");
    emitDec ((response0 >> 8) & 1);
    emitText (" err=");
    emitHex (response0 & 0xffff, 4);
  } else {
    emitText (" none");
  }
  emitText ("\n clk");
  emitHex (baseClock, 3);
  emitText ("MHz div");
  emitDec (divider);
  emitText (" sd");
  emitDec (sdClockHz / 1000000U);
  emitText ("MHz\n pst");
  emitHex (presentBefore, 8);
  emitText (" -> ");
  emitHex (presentAfter, 8);
  emitText ("\n cmd0d1a to");
  emitDec (timeouts);
  emitText (" re");
  emitDec (responseErrors);
  emitText (" si");
  emitDec (silent);
  emitText (" busy");
  emitDec (busySkipped);
  emitByte ('\n');
  clover_emmc_stage = 6;
}

// ------------------------------------------------------------------- mode 4: stop asking, start over
//
// Round89 asked every address 1..15 and 0 and got silence from all sixteen. Two readings survive:
// the firmware's RCA is above 15 -- the SD spec allows 1..65535 and nothing on this board records
// which one it chose -- or the host is not clocking the card any more. Round87's clk=0007 was true
// when it was read; the MMCC gate behind that clock is not something this port drives, and a gate
// is exactly the kind of state a firmware handoff can close.
//
// Both readings are answered by taking the card through identification from scratch, which is also
// what the next rounds need: CMD0, CMD1 until the OCR says power-up, CMD2 for the CID, CMD3 to
// assign an RCA this port chooses. No previous address has to be known (EDK2's order:
// EmbeddedPkg/Universal/MmcDxe/MmcIdentification.c:572-619 and :141).
//
// CMD0 is the discriminator, because it asks for no response at all: Command Complete then means
// the SD clock is running and the command path works, while a timeout means nothing is shifting
// the command out. It is sent twice -- once at the firmware's 200 MHz, untouched, and once after
// dropping to the identification clock -- so "the clock was already gone" and "the change below
// broke it" cannot be confused.
//
// Identification runs at 100..400 kHz. The base clock is 200 MHz (capabilities 15:8 = 0xc8) and
// the divider is the 10-bit SDHCI 3.0 field (bits 15:8 plus 7:6), so 255 gives 200e6/(2*255) =
// 392 kHz. CMD0 also leaves any HS200/HS400 mode and any old selection behind. The card is
// expected to answer CMD1 with R3 (no CRC, no index) and to set bit 31 once it is ready; the OCR
// value is EmbeddedPkg's EMMC_CMD1_CAPACITY_GREATER_THAN_2GB (MmcHost.h:65).
static const uint32_t kEmmcOcr = 0x40FF8080;
static const uint32_t kIdentifyDivider = 255;
static const uint32_t kCmd1Retries = 200;
// One 512-byte block is 128 32-bit FIFO words; the buffer register is 32-bit access only.
static const uint32_t kBlockWords = 128;
// Round156: how many blocks one CMD18 asks for. It used to be 32 -- 16 KiB, small enough that the
// FIFO drain stays inside a single deadline. Round171 raises it to 128 once the batch is usable:
// the r169 and r170 frames show the batch never being trusted, so every read runs the single-block
// sequence and pays one command, one response and one FIFO service loop per 512 bytes. SDHCI's
// block-count register is 16 bit, so 128 fits and a 64 KiB run costs one command where it used to
// cost four.
static const uint32_t kReadChunkBlocks = 128;
// Round157: DATA_END can be set in the same INT_STATUS word as a CRC, end-bit or
// timeout error. r156's return expressions only asked for DATA_END, so a transfer
// that ended in error could be reported as a clean one and the corrupt words went
// on to the caller. Any error bit overrides the end bit.
static const uint32_t kDataErrorMask = SDHCI_INT_DATA_TIMEOUT | SDHCI_INT_DATA_CRC |
                                       SDHCI_INT_DATA_END_BIT | SDHCI_INT_ADMA_ERROR;
// Round171: the subset of those bits that says the bytes themselves are suspect. A data timeout
// cannot say that once the word count is complete -- see batchDelivered().
static const uint32_t kContentErrorMask = SDHCI_INT_DATA_CRC | SDHCI_INT_DATA_END_BIT |
                                          SDHCI_INT_ADMA_ERROR;

// Round171: the end-of-transfer test for a multi-block command. The two batch paths used to demand
// SDHCI_INT_DATA_END, and this host does not raise it for a multi-block PIO transfer: the
// controller's notion of "transfer complete" is the bus going idle, and an eMMC that never got a
// stop keeps clocking DAT0 after the last block. So the drain ends on the deadline or the spin
// bound with the status register holding SDHCI_INT_RESPONSE | SDHCI_INT_DATA_AVAIL -- the reading
// the r165 gate reported as "multi read fail lba=... st=0x21 w=1024/1024". All the words arrived.
// The batch was nonetheless judged a failure, fell back to single blocks, and never got trusted,
// which is why the r169 and r170 frames read the card one 512-byte block at a time.
//
// What replaces the end bit is the word count plus the content-error bits, which is the same
// evidence the single-block path has been using: the controller checks the block CRC on the bus,
// and DATA_CRC / DATA_END_BIT / ADMA_ERROR would name a bad transfer. A data timeout with a
// complete word count is this host waiting for a bus idle that CMD12 has not yet produced, so it
// does not veto the batch -- and CMD12 is sent before this returns, which puts the card back in
// TRAN. Durability is still proved separately: writeSettled() asks CMD13, and the r167 three-sample
// gate still compares a batch against the single-block sequence at the same LBA before trusting it.
static bool batchDelivered (uint32_t status, uint32_t words, uint32_t total)
{
  return words == total && !(status & kContentErrorMask);
}

// The generic counter, used to pace the FIFO drain. Round97 read the buffer port as fast as the
// loop could run: the first word was the real first word ("EFI ") but the other 127 reads came
// back identical, because the card delivers one word every ~80 us at 400 kHz single-bit while
// the loop emptied the controller's buffer in a few microseconds. Reading slower than the card
// can never lose data -- a full buffer backpressures the card -- so mode 12 waits on this clock
// instead of trusting the level bit to mean "another word is really here".
static inline uint64_t readCounter (void)
{
  uint64_t value;
  __asm__ volatile ("mrs %0, cntvct_el0" : "=r" (value));
  return value;
}

static inline uint64_t counterFrequency (void)
{
  uint64_t value;
  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (value));
  return value;
}

static bool waitCommandSlot (bool ignoreDataInhibit)
{
  uint32_t guard = 0;
  const uint32_t mask = ignoreDataInhibit
                        ? SDHCI_CMD_INHIBIT
                        : (SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT);
  while (read32 (SDHCI_PRESENT_STATE) & mask) {
    if (++guard >= kCommandSpinLimit) {
      return false;
    }
  }
  return true;
}

static bool waitCommandSlot (void)
{
  return waitCommandSlot (false);
}

// Round165: a transfer that ends early leaves words in the controller FIFO. The next command
// then reads those stale words ahead of its own first word, and every later word is shifted by
// however many were left. The controller's CRC does not catch this: it checks the bits on the
// bus, not the words we assembled afterwards. APFS found exactly this shape on the device --
// "Data hash mismatch ... expected jhash ..., got ..." -- so every read now starts from a
// proven-empty FIFO and every failed transfer resets the data path before the caller retries.
//
// drainReadFifo is bounded: the level bit is the only source of truth, and a stuck level would
// otherwise spin forever with interrupts masked at this point in the boot.
static uint32_t drainReadFifo (void)
{
  uint32_t words = 0;
  while ((read32 (SDHCI_PRESENT_STATE) & SDHCI_DATA_AVAILABLE) && words < 65536) {
    (void) read32 (SDHCI_BUFFER);
    words++;
  }
  return words;
}

// SDHCI_RESET_DATA self-clears when the controller has dropped the data phase and the FIFO.
// Like every other wait in this file, the bound is a counter and not a timer.
static bool resetReadPath (void)
{
  write8 (SDHCI_SOFTWARE_RESET, SDHCI_RESET_DATA);
  for (uint32_t guard = 0; guard < kCommandSpinLimit; guard++) {
    if (!(read8 (SDHCI_SOFTWARE_RESET) & SDHCI_RESET_DATA)) {
      write32 (SDHCI_INT_STATUS, 0xffffffffu);
      return true;
    }
  }
  return false;
}

// Round169: resetting the host does not tell the card anything. A CMD17 that ended on a data
// error, or on a refused response, can leave the card in the data state, and from then on it
// ignores the commands that follow -- which reads as a cascade of EIO at unrelated block numbers.
// CMD12 is the only command that returns the card itself to TRAN; the write batch has sent it on
// every failure since r157, the single-block read never did.
static bool sendStopCommand (void);

static void abortReadTransfer (void)
{
  sendStopCommand ();
  resetReadPath ();
}

static bool prepareReadPath (void)
{
  drainReadFifo ();
  if (read32 (SDHCI_PRESENT_STATE) & SDHCI_DATA_AVAILABLE) {
    return resetReadPath ();
  }
  return true;
}

// One whole 512-byte block, read the way mode 12's drain proved works: an adtc command with a
// short R1 and the block count enabled, then one FIFO word per counter gap and a final level-gated
// sweep for whatever DATA_END raced with. Mode 14 uses this to walk the GPT entry array block by
// block; the first block (LBA 1) keeps its own inline path, so the sequence that round99 proved is
// not disturbed. Mode 21 passes CMD8 here, which is the same command shape and one register field
// larger. `status` is the INT_STATUS group the data phase ended on, `wordCount` how many words
// were stored, and the return value is true only for a clean command response, DATA_END and all
// 128 words.
static bool readDataBlock (uint16_t command, uint32_t argument, uint32_t *words,
                           uint32_t &wordCount, uint32_t &status)
{
  wordCount = 0;
  status = 0;
  // Round165: the previous transfer may have stopped with words still queued; starting here
  // with them in the FIFO would shift this block without any error bit moving.
  if (!prepareReadPath ()) {
    return false;
  }
  if (!waitCommandSlot ()) {
    return false;
  }
  write8 (SDHCI_TIMEOUT_CONTROL, 0x0e);
  write16 (SDHCI_BLOCK_SIZE, 512);
  write16 (SDHCI_BLOCK_COUNT, 1);
  write32 (SDHCI_INT_STATUS, 0xffffffffu);
  write32 (SDHCI_ARGUMENT, argument);
  write16 (SDHCI_TRANSFER_MODE, SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN);
  write16 (SDHCI_COMMAND, SDHCI_MAKE_CMD (command, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                          SDHCI_CMD_INDEX | SDHCI_CMD_DATA));
  uint32_t spins = 0;
  for (;;) {
    status = read32 (SDHCI_INT_STATUS);
    if (status & SDHCI_INT_CMD_MASK) {
      break;
    }
    if (++spins >= kCommandSpinLimit) {
      break;
    }
  }
  if (!(status & SDHCI_INT_RESPONSE) ||
      (status & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX))) {
    // Round166: a refused command can leave the data path mid-transfer; the next block must not
    // start on top of it. This was the only refusal that skipped the reset. Round169 also stops
    // the card, not just the host.
    abortReadTransfer ();
    return false;
  }
  uint64_t frequency = counterFrequency ();
  if (frequency < 1000000ULL || frequency > 1000000000ULL) {
    frequency = 19200000ULL;
  }
  const uint64_t deadline = frequency / 5ULL;      // 200 ms
  const uint64_t start = readCounter ();
  uint32_t guard = 0;
  for (;;) {
    status = read32 (SDHCI_INT_STATUS);
    if (status & (SDHCI_INT_DATA_END | SDHCI_INT_DATA_TIMEOUT | SDHCI_INT_DATA_CRC |
                  SDHCI_INT_DATA_END_BIT | SDHCI_INT_ADMA_ERROR)) {
      break;
    }
    if (readCounter () - start > deadline) {
      break;
    }
    // Round156: drain as fast as the level bit allows. The 160 us gap this loop
    // used to keep came from mode 12's 400 kHz identification clock, where one
    // word really did arrive every 80 us; it made a 512-byte block cost 20.5 ms
    // (25 KB/s) at every later clock too. The firmware leaves this card at
    // HS200, 8-bit, 200 MHz, and SDHCI backpressures the card whenever the
    // buffer is full, so reading only while DATA_AVAILABLE is set can never
    // outrun it. This is the same rule the final level-gated sweep already
    // used -- the gap was the only thing making it slow.
    if (wordCount < kBlockWords &&
        (read32 (SDHCI_PRESENT_STATE) & SDHCI_DATA_AVAILABLE)) {
      words[wordCount++] = read32 (SDHCI_BUFFER);
    }
    if (++guard >= kCommandSpinLimit) {
      break;
    }
  }
  while (wordCount < kBlockWords &&
         (read32 (SDHCI_PRESENT_STATE) & SDHCI_DATA_AVAILABLE)) {
    words[wordCount++] = read32 (SDHCI_BUFFER);
  }
  if (!((status & SDHCI_INT_DATA_END) && !(status & kDataErrorMask) &&
        wordCount == kBlockWords)) {
    // Round165: leave the data path clean for whoever retries this block. Round169 also stops
    // the card: a card left in the data state ignores the retry, which turns one bad block into
    // EIO at every block that follows.
    abortReadTransfer ();
    return false;
  }
  return true;
}

static bool readPioBlock (uint32_t lba, uint32_t *words, uint32_t &wordCount,
                          uint32_t &status)
{
  return readDataBlock (MMC_READ_SINGLE_BLOCK, lba, words, wordCount, status);
}

// Round156: one command for several consecutive blocks. The data path is the same
// level-gated FIFO drain readDataBlock now uses; the only new pieces are the block
// count, the multi-block transfer mode bit and the explicit CMD12 that takes the
// card out of data mode. AUTO_CMD12 is deliberately not used: this host's vendor
// state was left by the firmware and has never been asked to issue it, whereas
// CMD12 is the same command shape every earlier round already proved.
//
// Round157: CMD12's response is R1b, so the command is issued with the busy-check
// spelling and its completion is waited for with the same counter bound the other
// commands use. r156 sent it, ignored the response and cleared the interrupt bits,
// which meant a stop that never completed -- the one failure that leaves the card
// in data mode and makes every later block read time out -- looked exactly like a
// clean stop. One retry covers the ordinary case where the card was still busy at
// the first attempt; if both fail the caller learns it.
static bool sendStopCommand (void)
{
  uint64_t frequency = counterFrequency ();
  if (frequency < 1000000ULL || frequency > 1000000000ULL) {
    frequency = 19200000ULL;
  }
  const uint64_t deadline = frequency / 5ULL;      // 200 ms, busy period included
  for (uint32_t attempt = 0; attempt < 2; attempt++) {
    if (!waitCommandSlot (true)) {
      return false;
    }
    write32 (SDHCI_INT_STATUS, 0xffffffffu);
    write32 (SDHCI_ARGUMENT, 0);
    write16 (SDHCI_TRANSFER_MODE, 0);
    write16 (SDHCI_COMMAND, SDHCI_MAKE_CMD (MMC_STOP_TRANSMISSION,
                                            SDHCI_CMD_RESP_SHORT_BUSY | SDHCI_CMD_CRC |
                                            SDHCI_CMD_INDEX));
    uint32_t status = 0;
    const uint64_t start = readCounter ();
    for (;;) {
      status = read32 (SDHCI_INT_STATUS);
      if (status & SDHCI_INT_CMD_MASK) {
        break;
      }
      if (readCounter () - start > deadline) {
        break;
      }
    }
    write32 (SDHCI_INT_STATUS, 0xffffffffu);
    if ((status & SDHCI_INT_RESPONSE) &&
        !(status & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                    SDHCI_INT_INDEX))) {
      return true;
    }
  }
  return false;
}

// Round159's batch paths fall back to the single-block commands the earlier rounds proved, so a
// refused CMD18/CMD25 costs speed but never data. That silence is exactly the r156 failure mode:
// the batch could be failing on every chunk and the boot would look merely slow. Report the first
// refusal of each direction, once, with the evidence needed to tell a command refusal from a data
// one; later refusals stay quiet so a 32 MiB walk cannot flood the log.
static bool multiFailure (unsigned which, uint32_t lba, uint32_t blocks, uint32_t status,
                          uint32_t words, uint32_t total)
{
  static bool reported[2];
  if (!reported[which]) {
    reported[which] = true;
    sdhciSay ("CloverSdhci r159: multi %s fail lba=%u blocks=%u st=0x%x w=%u/%u (once)\n",
           which ? "write" : "read", lba, blocks, status, words, total);
  }
  return false;
}

static bool readMultiBlock (uint32_t lba, uint32_t *words, uint32_t blocks,
                            uint32_t &wordCount, uint32_t &status)
{
  const uint32_t totalWords = blocks * kBlockWords;
  wordCount = 0;
  status = 0;
  // Round165: same rule as readDataBlock. A refused command can still have queued words from an
  // earlier transfer, and the single-block fallback must not inherit them.
  if (!prepareReadPath ()) {
    return multiFailure (0, lba, blocks, status, 0, totalWords);
  }
  if (!waitCommandSlot ()) {
    return multiFailure (0, lba, blocks, status, 0, totalWords);
  }
  write8 (SDHCI_TIMEOUT_CONTROL, 0x0e);
  write16 (SDHCI_BLOCK_SIZE, 512);
  write16 (SDHCI_BLOCK_COUNT, static_cast<uint16_t>(blocks));
  write32 (SDHCI_INT_STATUS, 0xffffffffu);
  write32 (SDHCI_ARGUMENT, lba);
  write16 (SDHCI_TRANSFER_MODE, SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN |
                                SDHCI_TRNS_MULTI);
  write16 (SDHCI_COMMAND, SDHCI_MAKE_CMD (MMC_READ_MULTIPLE_BLOCK,
                                          SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                          SDHCI_CMD_INDEX | SDHCI_CMD_DATA));
  uint32_t spins = 0;
  for (;;) {
    status = read32 (SDHCI_INT_STATUS);
    if (status & SDHCI_INT_CMD_MASK) {
      break;
    }
    if (++spins >= kCommandSpinLimit) {
      break;
    }
  }
  if (!(status & SDHCI_INT_RESPONSE) ||
      (status & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX))) {
    sendStopCommand ();
    resetReadPath ();
    return multiFailure (0, lba, blocks, status, 0, totalWords);
  }
  uint64_t frequency = counterFrequency ();
  if (frequency < 1000000ULL || frequency > 1000000000ULL) {
    frequency = 19200000ULL;
  }
  // 200 ms plus 50 ms per block: one block at the slowest identification clock is 10.4 ms, and a
  // card that just finished programming can hold the data line busy before the first token. r168
  // widened the per-block share so a busy card finishes the batch instead of falling back to a
  // single-block sequence that meets the same delay once per block.
  const uint64_t deadline = frequency / 5ULL +
                            static_cast<uint64_t>(blocks) * (frequency / 20ULL);
  const uint64_t start = readCounter ();
  uint32_t guard = 0;
  bool clean = false;
  for (;;) {
    status = read32 (SDHCI_INT_STATUS);
    if (status & kContentErrorMask) {
      // INT_STATUS is write-1-to-clear and nothing here clears it during the drain, so this read
      // still holds every fault bit the transfer raised at any point.
      break;
    }
    if (wordCount >= totalWords) {
      if (status & SDHCI_INT_DATA_END) {
        clean = true;
      }
      break;
    }
    // Round171: a host that says the transfer is finished while its FIFO is handing over nothing
    // has nothing left to give. Without this the drain would spin out the whole request deadline --
    // seconds -- on a batch that is already short, and the failure reading would arrive late.
    if ((status & SDHCI_INT_DATA_END) &&
        !(read32 (SDHCI_PRESENT_STATE) & SDHCI_DATA_AVAILABLE)) {
      break;
    }
    if (readCounter () - start > deadline) {
      break;
    }
    if (read32 (SDHCI_PRESENT_STATE) & SDHCI_DATA_AVAILABLE) {
      words[wordCount++] = read32 (SDHCI_BUFFER);
    }
    if (++guard >= kCommandSpinLimit) {
      break;
    }
  }
  while (wordCount < totalWords &&
         (read32 (SDHCI_PRESENT_STATE) & SDHCI_DATA_AVAILABLE)) {
    words[wordCount++] = read32 (SDHCI_BUFFER);
  }
  // Round171: take the verdict from the register as it stands at the end of the drain, before the
  // stop command clears it.
  status = read32 (SDHCI_INT_STATUS);
  const bool delivered = clean || batchDelivered (status, wordCount, totalWords);
  const bool stopped = sendStopCommand ();
  if (delivered) {
    if (!stopped) {
      // The words are usable but the card may still be driving DAT0. Give the stop another try
      // from here rather than handing the next caller a card in data mode; the next read also
      // waits on waitCardIdle() before its first command.
      static bool stopReported = false;
      if (!stopReported) {
        stopReported = true;
        sdhciSay ("CloverSdhci r171: batch stop refused lba=%u blocks=%u (once)\n", lba, blocks);
      }
      sendStopCommand ();
    }
    // A clean transfer should have no tail, but the stop command is where a vendor controller
    // could still hand back the last word; drop anything left so the next read starts empty.
    drainReadFifo ();
    return true;
  }
  // Round165: the fallback single-block read that follows must not see the words this failed
  // transfer left behind, or it will assemble a shifted block that still looks clean.
  resetReadPath ();
  return multiFailure (0, lba, blocks, status, wordCount, totalWords);
}

// The symbols drivers/CloverBlock links against. Ready and the sector count are only what
// mode 21 published after a clean EXT_CSD read whose SEC_COUNT contains the logfs extent, so the
// block device can never describe a card this port has not measured. A block read is gated on
// that geometry, on the same single-block PIO path, and on a guard that keeps a second caller out
// while the first owns the host; the deadline is the counter, because at this scheduling point the
// watchdog does not tick. `blocks` is capped by what is left of the card, so no caller can walk
// past the end.
extern "C" uint32_t clover_emmc_block_ready (void)
{
  return __atomic_load_n (&gBlockReady, __ATOMIC_ACQUIRE);
}

extern "C" uint32_t clover_emmc_block_sectors (void)
{
  return __atomic_load_n (&gBlockSectors, __ATOMIC_ACQUIRE);
}

// The window drivers/CloverBlock can instead describe: logfs's own first LBA and sector count, or
// zero when the card's GPT entry 39 is not the extent this port reads. A caller that takes this
// geometry has to add `first` to every block number it hands to clover_emmc_block_read, which the
// caller does and this function deliberately does not.
extern "C" void clover_emmc_block_window (uint32_t *first, uint32_t *sectors)
{
  if (first) {
    *first = __atomic_load_n (&gBlockWindowFirst, __ATOMIC_ACQUIRE);
  }
  if (sectors) {
    *sectors = __atomic_load_n (&gBlockWindowSectors, __ATOMIC_ACQUIRE);
  }
}

// Round128's counterpart: userdata (entry 64) as a window, zero until this boot's card reports that
// entry as exactly the extent board.toml recorded. The geometry above the logfs one is a separate
// pair rather than a replacement, so a release that asks for the wrong window reads the wrong bytes
// and says so, instead of silently describing a different partition.
extern "C" void clover_emmc_block_userdata_window (uint32_t *first, uint32_t *sectors)
{
  if (first) {
    *first = __atomic_load_n (&gBlockUserDataFirst, __ATOMIC_ACQUIRE);
  }
  if (sectors) {
    *sectors = __atomic_load_n (&gBlockUserDataSectors, __ATOMIC_ACQUIRE);
  }
}

// One command engine and one FIFO: the guard has to be held for the whole request. Round171 changed
// how a second caller waits -- from a refusal (round126 and earlier) and a bounded spin (round127
// through round170) to an IOLock, the same way sheng's NativeUfs block device serializes its window
// reads. Round213 keeps the sleeping waiter but not the lock: see gHostOwner. What matters here is
// that every wait now has a bound of its own. The holder's bound is the request deadline plus the
// stage watchdog; the waiter's bound is kGuardWaitMs. A waiter that runs out reports "guard", the
// request fails, and APFS asks again -- which is recoverable, unlike a boot that stops reading.
//
// The wait is still kept as a reading: it is measured, and the first long one is reported once per
// boot, because "a read was waiting on a write" is exactly the fact the r167 and r168 frames needed.
// On success the caller gets the ticket it holds and the tick to measure its own deadline from.

// Round213: stage bookkeeping for the guard holder. The stamp is written before the stage, and the
// beat reads stage, stamp, stage again -- so a beat that catches a transition in progress sees the
// stage change and does nothing, instead of comparing a new stage with an old stamp.
static void flightStage (uint32_t stage)
{
  if (stage == kStageIdle) {
    __atomic_store_n (&gFlightStageSince, 0, __ATOMIC_RELEASE);
    __atomic_store_n (&gFlightStage, kStageIdle, __ATOMIC_RELEASE);
    return;
  }
  __atomic_store_n (&gFlightStageDir, gFlightCurDir, __ATOMIC_RELAXED);
  __atomic_store_n (&gFlightStageLba, gFlightCurLba, __ATOMIC_RELAXED);
  __atomic_store_n (&gFlightStageBlocks, gFlightCurBlocks, __ATOMIC_RELAXED);
  __atomic_store_n (&gFlightStagePid, gFlightCurPid, __ATOMIC_RELAXED);
  __atomic_store_n (&gFlightStageSince, readCounter (), __ATOMIC_RELEASE);
  __atomic_store_n (&gFlightStage, stage, __ATOMIC_RELEASE);
}

// True when this request no longer owns the host: the ticket was taken back by the watchdog (or the
// request never had one). A request that sees this must stop touching the controller -- somebody
// else owns it now, and its own release must not clear that owner's word.
static bool guardLost (uint32_t token)
{
  return token == 0 || __atomic_load_n (&gHostOwner, __ATOMIC_ACQUIRE) != token;
}

// Free the ticket without touching the stage. The beat uses this: it borrows the host for one
// record and must not erase the stage of whatever it found the host in.
static void dropHostTicket (uint32_t &token)
{
  if (token) {
    uint32_t expected = token;
    (void)__atomic_compare_exchange_n (&gHostOwner, &expected, 0u, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    token = 0;
  }
}

// The beat's acquire: one attempt, no wait. A beat never queues behind a request.
static bool tryTakeHost (uint32_t &token)
{
  token = 0;
  uint32_t ticket = __atomic_add_fetch (&gHostTickets, 1u, __ATOMIC_RELAXED);
  if (!ticket) {
    ticket = __atomic_add_fetch (&gHostTickets, 1u, __ATOMIC_RELAXED);
  }
  uint32_t expected = 0;
  if (!__atomic_compare_exchange_n (&gHostOwner, &expected, ticket, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    return false;
  }
  token = ticket;
  return true;
}

static bool acquireBlockGuard (uint64_t &frequency, uint64_t &start, uint32_t &token)
{
  token = 0;
  frequency = counterFrequency ();
  if (frequency < 1000000ULL || frequency > 1000000000ULL) {
    frequency = 19200000ULL;
  }
  if (!__atomic_load_n (&gHostArmed, __ATOMIC_ACQUIRE)) {
    return false;
  }
  const uint64_t waitStart = readCounter ();
  const uint64_t waitLimit = frequency * kGuardWaitMs / 1000ULL;
  uint32_t ticket = __atomic_add_fetch (&gHostTickets, 1u, __ATOMIC_RELAXED);
  if (!ticket) {
    ticket = __atomic_add_fetch (&gHostTickets, 1u, __ATOMIC_RELAXED);
  }
  for (;;) {
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n (&gHostOwner, &expected, ticket, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      token = ticket;
      break;
    }
    if (readCounter () - waitStart > waitLimit) {
      // One line per boot: a wait this long means a holder did not finish, and the stage word says
      // which one and where it was. The beat's own trip reports the recovery separately.
      static bool waitReported = false;
      if (!waitReported) {
        waitReported = true;
        sdhciSay ("CloverSdhci r213: guard wait gave up after %llu ms owner=%u stage=%u\n",
               static_cast<unsigned long long> (kGuardWaitMs),
               __atomic_load_n (&gHostOwner, __ATOMIC_ACQUIRE),
               __atomic_load_n (&gFlightStage, __ATOMIC_ACQUIRE));
      }
      return false;
    }
    IOSleep (1);
  }
  const uint64_t waited = readCounter () - waitStart;
  // Round178: every wait is counted, and the worst one kept, so the hand-off report at the tail of a
  // request can say what the host actually did rather than what it was expected to do. This is a
  // racy maximum across the boot's threads and that is fine: it is a reading, and an occasional lost
  // increment only understates it. The wait is measured before the holder's deadline starts.
  gGuardWaits = __atomic_add_fetch (&gGuardWaits, 1u, __ATOMIC_RELAXED);
  __atomic_store_n (&gGuardLastWaitMs, waited * 1000ULL / frequency, __ATOMIC_RELAXED);
  if (waited > __atomic_load_n (&gGuardWorstWait, __ATOMIC_RELAXED)) {
    __atomic_store_n (&gGuardWorstWait, waited, __ATOMIC_RELAXED);
  }
  // Round168: one line, once per boot. A frame that shows a long wait next to the APFS errors
  // proves contention; a frame with no line and the same errors moves the wall back into the card.
  static bool guardReported = false;
  if (!guardReported && waited > frequency / 4ULL) {
    guardReported = true;
    sdhciSay ("CloverSdhci r168: block guard waited %llu ms\n",
           static_cast<unsigned long long> (waited * 1000ULL / frequency));
  }
  start = readCounter ();
  return true;
}

// Round213: release is a compare-and-swap on the ticket, not an unlock. A request whose host was
// taken back by the watchdog must not clear the new owner's word when it finally returns -- that is
// exactly the case the ticket exists for. The stage only returns to idle when this release was the
// one that actually freed the host.
static void releaseBlockGuard (uint32_t &token)
{
  if (!token) {
    return;
  }
  uint32_t expected = token;
  const bool owned = __atomic_compare_exchange_n (&gHostOwner, &expected, 0u, false,
                                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
  token = 0;
  if (owned) {
    flightStage (kStageIdle);
  }
}

// Round178: hand the host to whoever is waiting, then take it back for the rest of this request.
// Called between chunks, so a request that needs eight 64 KiB runs is eight owners rather than one
// owner for the whole transfer -- which is what a page fault at the other volume needs. `owned`
// accumulates guarded time so the request keeps round169's deadline (a bound on the transfer, not on
// how long the rest of the boot took around it) while no single hold outlives one chunk. Round213:
// losing the re-acquire no longer means "the guard was never armed" -- it can also mean a waiter
// that waited kGuardWaitMs -- so the caller now treats it as a failed request and stops.
static bool handOffBlockGuard (uint64_t &frequency, uint64_t &start, uint64_t &owned,
                               uint32_t &token)
{
  owned += readCounter () - start;
  releaseBlockGuard (token);
  if (!acquireBlockGuard (frequency, start, token)) {
    return false;
  }
  __atomic_add_fetch (&gGuardHandoffs, 1u, __ATOMIC_RELAXED);
  return true;
}

// Round178: the hand-off says so, a few times per boot, from the holder that did it. The number this
// frame has to answer is whether a reader ever cut in between two chunks of someone else's request
// and what it waited: `handoff` rows plus the r168 `block guard waited` row are the two sides of it.
static void reportGuardHandoff (const char *dir, uint32_t lba, uint32_t blocks, uint32_t done)
{
  static uint32_t reported = 0;
  if (reported >= 4) {
    return;
  }
  reported++;
  const uint64_t frequency = counterFrequency ();
  const uint64_t worst = __atomic_load_n (&gGuardWorstWait, __ATOMIC_RELAXED);
  sdhciSay ("CloverSdhci r178: %s handoff lba=%u blocks=%u done=%u cuts=%u waits=%u worst=%llu ms\n",
         dir, lba, blocks, done,
         __atomic_load_n (&gGuardHandoffs, __ATOMIC_RELAXED),
         __atomic_load_n (&gGuardWaits, __ATOMIC_RELAXED),
         static_cast<unsigned long long> (frequency ? worst * 1000ULL / frequency : 0));
}

// Round165's second half: the device itself proves the two read paths agree. The first multi-block
// request reads the same run three ways -- two CMD18 batches and the same number of CMD17 blocks --
// and compares the words. If the batch and the single-block path disagree, or two batches disagree
// with each other, the batch is not trusted for the rest of the boot and every later read uses the
// single-block sequence. The check costs one extra double read and runs once per boot.
//
// Round171 sizes the sample from the request that triggered it instead of from kReadChunkBlocks.
// The r170 frame's reads are 16 KiB -- that is what APFS asks for -- and a gate that only opened on
// a whole 64 KiB batch would never open, leaving the batch untrusted and the boot back on one
// command per 512 bytes, which is the failure this round exists to remove.
static uint32_t gCheckBatchA[kReadChunkBlocks * kBlockWords];
static uint32_t gCheckBatchB[kReadChunkBlocks * kBlockWords];
static uint32_t gCheckSingle[kReadChunkBlocks * kBlockWords];
static bool gReadCheckDone = false;
// Round167: starts untrusted; checkReadPath promotes it only when two CMD18 batches and the
// block-by-block sequence agree word for word over the same run, so the first real request still
// reads exactly like every round before r156.
static bool gReadBatchTrusted = false;

static uint32_t countWordDiff (const uint32_t *a, const uint32_t *b, uint32_t words,
                               uint32_t &first)
{
  uint32_t diff = 0;
  for (uint32_t i = 0; i < words; i++) {
    if (a[i] != b[i]) {
      if (diff == 0) {
        first = i;
      }
      diff++;
    }
  }
  return diff;
}

static void checkReadPath (uint32_t lba, uint32_t sample)
{
  gReadCheckDone = true;
  const uint32_t words = sample * kBlockWords;
  uint32_t gotA = 0, statusA = 0, gotB = 0, statusB = 0;
  // Round168: the check runs on the first multi-block request of the boot, which can now arrive
  // while the card is still programming. A single refusal here must not retire the batch for the
  // whole boot, so each sample gets the same one clean retry the read loop uses.
  bool batchA = readMultiBlock (lba, gCheckBatchA, sample, gotA, statusA);
  if (!batchA) {
    batchA = readMultiBlock (lba, gCheckBatchA, sample, gotA, statusA);
  }
  bool batchB = readMultiBlock (lba, gCheckBatchB, sample, gotB, statusB);
  if (!batchB) {
    batchB = readMultiBlock (lba, gCheckBatchB, sample, gotB, statusB);
  }
  bool single = true;
  uint32_t blocks = 0;
  for (; blocks < sample; blocks++) {
    uint32_t got = 0, status = 0;
    bool ok = readPioBlock (lba + blocks, gCheckSingle + blocks * kBlockWords, got, status);
    if (!ok || got != kBlockWords) {
      ok = readPioBlock (lba + blocks, gCheckSingle + blocks * kBlockWords, got, status);
      if (!ok || got != kBlockWords) {
        single = false;
        break;
      }
    }
  }
  uint32_t first = 0, firstB = 0;
  const uint32_t diff = (batchA && single)
                        ? countWordDiff (gCheckBatchA, gCheckSingle, words, first)
                        : words;
  const uint32_t diffB = (batchA && batchB)
                         ? countWordDiff (gCheckBatchA, gCheckBatchB, words, firstB)
                         : words;
  // Round167: r166 ran the whole boot on single blocks to rule the batch out, and the next frame
  // still failed inside APFS. The batch therefore stays -- without it the 3.7 GiB dyld cache is
  // hours of reads at 512 bytes per command. It is trusted only when all three samples of this
  // 16 KiB run agree word for word: two CMD18 batches and the 32 CMD17 blocks every earlier round
  // used. Any difference retires it for the boot, exactly as r165 did.
  const bool trusted = batchA && batchB && single &&
                       gotA == words && gotB == words && diff == 0 && diffB == 0;
  gReadBatchTrusted = trusted;
  sdhciSay ("CloverSdhci r167: readcheck lba=%u sample=%u batch=%d/%d single=%d diff=%u/%u "
         "first=%u/%u m=%08x s=%08x trusted=%d\n",
         lba, sample, batchA ? gotA : 0, batchB ? gotB : 0, single ? blocks : 0,
         diff, diffB, first, firstB,
         gCheckBatchA[first < words ? first : 0],
         gCheckSingle[first < words ? first : 0],
         trusted ? 1 : 0);
  if (trusted) {
    sdhciSay ("CloverSdhci r167: batch reads trusted for this boot\n");
  } else {
    sdhciSay ("CloverSdhci r167: single-block reads for this boot\n");
  }
}

static void waitCardReady (uint32_t &response, uint32_t &status, uint32_t &polls);

// Round210: the flight record of one block request. Defined beside the record writers, below the
// PIO path they use; the read and write entry points call these before their first command.
static void flightEnter (uint32_t event, uint32_t dir, uint32_t lba, uint32_t blocks);
static void flightExit (uint32_t done);
// Round215: the request-context recovery, defined beside reinitializeCard below. The watchdog calls
// it too, after it has taken the ticket from a holder that stopped.
// Round310 adds `force`, and only the panic sink passes it: the per-boot cap and the cooldown exist
// to keep one dead card from turning every request into a power cycle, and a boot that is ending has
// no later request to protect. Without the flag the sink would read a refusal as a failed reset.
static bool cardRecoverHeld (const char *why, uint32_t detail, bool force = false);

// Round169: the r168 frame turned the write-path wall into a read-path cascade -- APFS read 16 KiB
// buffers from every daemon it decompressed and each one came back as EIO, while the card was
// still working off the persona writes. A write is counted as stored when CMD13 reports
// READY_FOR_DATA in TRAN, but the card can pull DAT0 busy again for its own housekeeping right
// after; a read issued in that window is refused or answered late. Before the first command of a
// read, wait for the host to drop DATA_INHIBIT and for CMD13 to say the card is ready, bounded by
// the same thirty seconds the guard hand-over uses. A healthy card pays one CMD13. The same wait
// runs before re-sending a write batch whose settlement timed out.
static bool waitCardIdle (uint64_t frequency)
{
  const uint64_t start = readCounter ();
  const uint64_t bound = frequency * 30ULL;
  static bool reported = false;
  __atomic_store_n (&gIdleFailReason, kIdleOk, __ATOMIC_RELAXED);
  while (read32 (SDHCI_PRESENT_STATE) & SDHCI_DATA_INHIBIT) {
    const uint64_t waited = readCounter () - start;
    if (!reported && waited > frequency / 4ULL) {
      reported = true;
      sdhciSay ("CloverSdhci r169: card busy %llu ms\n",
             static_cast<unsigned long long> (waited * 1000ULL / frequency));
    }
    if (waited > bound) {
      __atomic_store_n (&gIdleFailReason, kIdleInhibit, __ATOMIC_RELAXED);
      return false;
    }
  }
  // Round215: CMD13 is legal while the card programs, so "no answer" and "still programming" are
  // different facts. Six unanswered polls in a row is the first; it ends the wait in milliseconds
  // instead of the thirty seconds the busy bound allows, and the caller's recovery turns on it.
  uint32_t silent = 0;
  for (;;) {
    uint32_t response = 0;
    uint32_t status = 0;
    uint32_t polls = 0;
    waitCardReady (response, status, polls);
    const uint64_t waited = readCounter () - start;
    if (!reported && waited > frequency / 4ULL) {
      reported = true;
      sdhciSay ("CloverSdhci r169: card busy %llu ms\n",
             static_cast<unsigned long long> (waited * 1000ULL / frequency));
    }
    if ((response & R1_READY_FOR_DATA) && R1_CURRENT_STATE (response) == R1_STATE_TRAN) {
      return true;
    }
    if (!response && !(status & SDHCI_INT_RESPONSE)) {
      if (++silent >= 6) {
        __atomic_store_n (&gIdleFailReason, kIdleSilent, __ATOMIC_RELAXED);
        return false;
      }
    } else {
      silent = 0;
    }
    if (waited > bound) {
      __atomic_store_n (&gIdleFailReason, kIdleBusy, __ATOMIC_RELAXED);
      return false;
    }
  }
}

// Round169: the r168 give-up line was once per boot, so a frame that showed a cascade of EIO
// could not say which stage produced the first one or whether later ones differed. Eight lines
// are enough for a frame and bounded enough for a boot.
static void reportReadFailure (const char *stage, uint32_t lba, uint32_t blocks,
                               uint32_t done, uint32_t status)
{
  static uint32_t reported = 0;
  if (reported >= 8) {
    return;
  }
  reported++;
  sdhciSay ("CloverSdhci r169: read %s lba=%u blocks=%u done=%u st=%08x\n",
         stage, lba, blocks, done, status);
}

// ---------------------------------------------------------------- round172: the ADMA2 read path
//
// WHY THERE IS ONE
//
// Every byte this port has read out of the card has been carried by the CPU: read32 (SDHCI_BUFFER)
// into a word, one word at a time, with the level bit saying when the next is there. That ceiling is
// what makes a 3.7 GiB dyld shared cache unreadable at boot -- r156 measured 16 KiB in 7 ms on the
// batch path (about 2 MB/s), and the same card on the same link moves far more when the host empties
// its own FIFO into memory. The controller says it can do exactly that: `cap=0x742dc8b2` has
// SDHCI_CAN_DO_ADMA2 (0x00080000) and SDHCI_CAN_64BIT (0x10000000) set and SDHCI_CAN_DO_SDMA
// (0x00400000) clear, and the firmware's own last HOST_CONTROL was `hcl=38`, which is ADMA64 (0x18)
// plus the 8-bit bus (0x20). The path this round adds is the one the boot loader was already using.
//
// WHAT MAKES IT SAFE TO ADD AT ALL
//
// Three things, in this order. The descriptor layout is transcribed from the tablet's own 4.19 CAF
// tree rather than recalled (see SdhciRegs.h); the DMA reads into a bounce buffer that this driver
// allocated itself, is page-aligned, physically contiguous and used by nothing else, so no caller's
// buffer has to be walkable by the bus; and the path is only used after the device has proved it.
// checkDmaPath() reads the same run twice -- once through the batch PIO path the r167 gate already
// trusted, once through ADMA -- and compares every word. ADMA is used for the rest of the boot only
// when the two agree word for word; anything else and the boot reads exactly as it did in round171.
// That also means an unknown in this round is answered rather than suffered: if the ICE or an SMMU
// translation the firmware left behind sits between this host and raw DDR addresses, the ADMA read
// comes back wrong or with ADMA_ERROR, the gate says so on the screen, and the DMA verdict is one
// number in the frame instead of a corrupted read.
//
// WHAT THIS ROUND DOES NOT TOUCH
//
// Writes. They stay on the PIO path r171 fixed here, because a DMA write that is wrong does not read
// back as an EIO -- it changes the card. The write version of this path is round173, and it is the
// same file, below writeMultiBlock(): it rewrites a run inside the logfs window with the bytes it
// just read from that run and only trusts ADMA when the partition reads back exactly as it was.
static const uint32_t kAdmaEntryBytes = SDHCI_ADMA2_64_DESC_SZ;
// 32 KiB per descriptor, not the field's own 64 KiB: 65536 is the length field's zero spelling, and
// a table whose every entry is a plain byte count is one that can be checked on the host.
static const uint32_t kAdmaEntryMaxBytes = 32768u;
static const uint32_t kAdmaDataBytes = kReadChunkBlocks * 512u;   // 65536, one whole chunk
static const uint32_t kAdmaTableBytes = 4096u;                    // 5 entries' worth, one page
static const uint64_t kAdmaAlign = 4096ULL;

struct AdmaPath {
  // One retained reference each, held for the whole boot: this driver is never unloaded, and the
  // bus master can be mid-descriptor-fetch when a later round would otherwise free it. sheng's
  // NativeUfs keeps its DMA buffer the same way (its UfsHost::dma is a raw IOBufferMemoryDescriptor).
  IOBufferMemoryDescriptor *data = nullptr;
  IOBufferMemoryDescriptor *table = nullptr;
  uint8_t *dataVirt = nullptr;
  uint8_t *tableVirt = nullptr;
  uint64_t dataPhys = 0;
  uint64_t tablePhys = 0;
  uint64_t dataContiguous = 0;
  bool wide = false;
  bool tried = false;
  bool available = false;
};
static AdmaPath gAdma;
static bool gDmaTrusted = false;
static bool gDmaCheckDone = false;
static uint32_t gDmaFailures = 0;

// The two cache operations a bus master makes the CPU care about, taken from sheng's NativeUfs
// (drivers/NativeUfs/NativeUfs.cpp:848-868) because that is the shape this codebase already trusts:
// clean-and-invalidate over the range before the device writes it, invalidate after it lands, each
// bounded by dsb sy. 64-byte lines are the smallest line any arm64 core in this family has, and the
// bounce is page-aligned with transfers that are whole multiples of 512, so no line here is shared
// with anything the CPU also wants.
static void cacheCleanInvalidate (const void *start, uint32_t length)
{
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~uintptr_t (63);
  const uintptr_t end = reinterpret_cast<uintptr_t> (start) + length;
  for (; address < end; address += 64) {
    __asm__ volatile ("dc civac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

static void cacheInvalidate (const void *start, uint32_t length)
{
  __asm__ volatile ("dsb sy" ::: "memory");
  uintptr_t address = reinterpret_cast<uintptr_t> (start);
  const uintptr_t end = address + length;
  for (; address < end; address += 64) {
    __asm__ volatile ("dc ivac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

// The buffers the host will read and write, and the address it will read them from. Every refusal is
// a reason printed once, because "the DMA path never came up" has to be distinguishable from "the
// DMA path came up and lost the comparison" without another round.
static bool dmaBringUp (void)
{
  if (gAdma.tried) {
    return gAdma.available;
  }
  gAdma.tried = true;
  const uint32_t cap = read32 (SDHCI_CAPABILITIES);
  if (!(cap & SDHCI_CAN_DO_ADMA2)) {
    sdhciSay ("CloverSdhci r172: host has no ADMA2, cap=%08x\n", cap);
    return false;
  }
  gAdma.wide = (cap & SDHCI_CAN_64BIT) != 0;
  const IOOptionBits options = static_cast<IOOptionBits> (kIODirectionInOut) |
                               static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
                               static_cast<IOOptionBits> (kIOMemoryMapperNone);
  gAdma.data = IOBufferMemoryDescriptor::inTaskWithOptions (kernel_task, options,
                                                            kAdmaDataBytes, kAdmaAlign);
  gAdma.table = IOBufferMemoryDescriptor::inTaskWithOptions (kernel_task, options,
                                                             kAdmaTableBytes, kAdmaAlign);
  if (!gAdma.data || !gAdma.table) {
    sdhciSay ("CloverSdhci r172: bounce allocation failed data=%p table=%p\n",
           (void *) gAdma.data, (void *) gAdma.table);
    return false;
  }
  gAdma.dataVirt = static_cast<uint8_t *> (gAdma.data->getBytesNoCopy ());
  gAdma.tableVirt = static_cast<uint8_t *> (gAdma.table->getBytesNoCopy ());
  IOByteCount contiguous = 0;
  gAdma.dataPhys = gAdma.data->getPhysicalSegment (0, &contiguous, kIOMemoryMapperNone);
  gAdma.dataContiguous = static_cast<uint64_t> (contiguous);
  IOByteCount tableContiguous = 0;
  gAdma.tablePhys = gAdma.table->getPhysicalSegment (0, &tableContiguous, kIOMemoryMapperNone);
  // The same four checks sheng applies to its own DMA buffer: a virtual address the CPU can use, a
  // physical one the bus can use, the whole run in one segment, and no address above 4 GiB unless the
  // host is the 64-bit kind. A bounce that is not contiguous would need one descriptor per segment,
  // which is a different round.
  if (!gAdma.dataVirt || !gAdma.tableVirt || !gAdma.dataPhys || !gAdma.tablePhys ||
      (gAdma.dataPhys & (kAdmaAlign - 1ULL)) ||
      gAdma.dataContiguous < kAdmaDataBytes ||
      static_cast<IOByteCount> (tableContiguous) < kAdmaTableBytes ||
      (!gAdma.wide && gAdma.dataPhys + kAdmaDataBytes > 0x100000000ULL)) {
    sdhciSay ("CloverSdhci r172: unusable DMA addresses data=%llx contig=%llu table=%llx wide=%d\n",
           static_cast<unsigned long long> (gAdma.dataPhys),
           static_cast<unsigned long long> (gAdma.dataContiguous),
           static_cast<unsigned long long> (gAdma.tablePhys), gAdma.wide ? 1 : 0);
    return false;
  }
  gAdma.available = true;
  sdhciSay ("CloverSdhci r172: dma up data=%llx table=%llx cap=%08x en=%08x hctl=%02x wide=%d\n",
         static_cast<unsigned long long> (gAdma.dataPhys),
         static_cast<unsigned long long> (gAdma.tablePhys), cap,
         read32 (SDHCI_INT_ENABLE), read8 (SDHCI_HOST_CONTROL), gAdma.wide ? 1 : 0);
  return true;
}

// One ADMA2 descriptor, in the CAF 4.19 byte order: cmd u16, length u16, address low u32, address
// high u32. Written as bytes rather than through a struct because the layout is the host's, not the
// CPU's, and this way the six stores are the documentation.
static void admaWriteEntry (uint32_t index, uint64_t address, uint32_t length, uint16_t cmd)
{
  uint8_t *entry = gAdma.tableVirt + static_cast<uint64_t> (index) * kAdmaEntryBytes;
  entry[0] = static_cast<uint8_t> (cmd & 0xff);
  entry[1] = static_cast<uint8_t> (cmd >> 8);
  entry[2] = static_cast<uint8_t> (length & 0xff);
  entry[3] = static_cast<uint8_t> ((length >> 8) & 0xff);
  for (unsigned byte = 0; byte < 8; byte++) {
    entry[4 + byte] = static_cast<uint8_t> ((address >> (byte * 8)) & 0xff);
  }
}

// The whole table for one run of bytes, and how many entries it took. sdhci-msm.c:5646 sets
// SDHCI_QUIRK_NO_ENDATTR_IN_NOPDESC for this controller family, so the last transfer entry carries
// the END attribute itself and there is no terminating NOP.
static uint32_t admaBuildTable (uint32_t bytes)
{
  uint32_t entries = 0;
  uint32_t left = bytes;
  uint64_t address = gAdma.dataPhys;
  while (left > 0) {
    const uint32_t chunk = left > kAdmaEntryMaxBytes ? kAdmaEntryMaxBytes : left;
    const bool last = (chunk == left);
    admaWriteEntry (entries, address, chunk,
                    static_cast<uint16_t> (SDHCI_ADMA2_TRAN_VALID |
                                           (last ? SDHCI_ADMA2_END : 0)));
    entries++;
    address += chunk;
    left -= chunk;
  }
  // The host fetches this table over the bus, so its dirty lines have to be in memory first.
  cacheCleanInvalidate (gAdma.tableVirt, entries * kAdmaEntryBytes);
  return entries;
}

// Which part of a DMA read refused. Eight lines is the r169 rule: enough for a frame, bounded enough
// for a boot. The ADMA_ERROR byte is printed because it names the descriptor the host gave up on --
// a zero-length or misaligned table looks different from an address the bus refused.
//
// Round293 adds how long the wait actually ran and how many polls it took, because the stage word
// could not say which of two very different things happened: the card sitting on the data line past
// the per-request deadline, or this loop spending its own spin budget before that deadline arrived.
// Boot 79 refused seven transfers and its frame cannot tell the two apart -- both were printed as
// "deadline", which reads as an accusation against the card whether or not the card was ever given
// its 600 ms.
static void reportDmaFailure (const char *stage, uint32_t lba, uint32_t blocks, uint32_t status,
                              uint64_t elapsed = 0, uint64_t budget = 0, uint32_t polls = 0,
                              uint64_t frequency = 19200000ULL)
{
  static uint32_t reported = 0;
  if (reported >= 8) {
    return;
  }
  reported++;
  sdhciSay ("CloverSdhci r172: dma %s lba=%u blocks=%u st=%08x err8=%02x hctl=%02x "
            "ms=%llu budget=%llu polls=%u\n",
         stage, lba, blocks, status, read8 (SDHCI_ADMA_ERROR), read8 (SDHCI_HOST_CONTROL),
         elapsed * 1000ULL / frequency, budget * 1000ULL / frequency, polls);
}

// One CMD18 with the data phase handed to the host: same command, same block count, same explicit
// CMD12 as readMultiBlock -- the only difference is SDHCI_TRNS_DMA in the transfer mode and the
// descriptor table the host reads instead of the FIFO the CPU drains. On success the bytes are in the
// bounce and copied out; `ticks` carries the transfer's own length so the caller can print a rate.
static bool readAdmaRun (uint32_t lba, uint32_t *dst, uint32_t blocks, uint32_t &status,
                         uint64_t &ticks)
{
  status = 0;
  ticks = 0;
  if (!dmaBringUp () || !blocks || blocks * 512u > kAdmaDataBytes) {
    return false;
  }
  const uint32_t bytes = blocks * 512u;
  // Round165's rule holds for the DMA path too: words left in the buffer by a refused transfer would
  // be the first thing this transfer writes into memory.
  if (!prepareReadPath ()) {
    reportDmaFailure ("path", lba, blocks, 0);
    return false;
  }
  if (!waitCommandSlot ()) {
    reportDmaFailure ("slot", lba, blocks, 0);
    return false;
  }
  admaBuildTable (bytes);
  // Tell the host which scheme ADMA_ADDRESS means, keeping the link's own HISPD and bus-width bits
  // (sdhci.c's sdhci_init: ctrl &= ~DMA_MASK, then ADMA32, then ADMA64 when the DMA is 64-bit).
  uint8_t control = read8 (SDHCI_HOST_CONTROL);
  control &= static_cast<uint8_t> (~SDHCI_CTRL_DMA_MASK);
  control |= static_cast<uint8_t> (gAdma.wide ? SDHCI_CTRL_ADMA64 : SDHCI_CTRL_ADMA32);
  write8 (SDHCI_HOST_CONTROL, control);
  write32 (SDHCI_ADMA_ADDRESS, static_cast<uint32_t> (gAdma.tablePhys & 0xffffffffULL));
  if (gAdma.wide) {
    write32 (SDHCI_ADMA_ADDRESS_HI, static_cast<uint32_t> (gAdma.tablePhys >> 32));
  }
  // Nothing the CPU wrote to the bounce should survive into the read it is about to receive.
  cacheCleanInvalidate (gAdma.dataVirt, bytes);
  write8 (SDHCI_TIMEOUT_CONTROL, 0x0e);
  write16 (SDHCI_BLOCK_SIZE, 512);
  write16 (SDHCI_BLOCK_COUNT, static_cast<uint16_t> (blocks));
  write32 (SDHCI_INT_STATUS, 0xffffffffu);
  write32 (SDHCI_ARGUMENT, lba);
  write16 (SDHCI_TRANSFER_MODE, SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN | SDHCI_TRNS_MULTI |
                                SDHCI_TRNS_DMA);
  write16 (SDHCI_COMMAND, SDHCI_MAKE_CMD (MMC_READ_MULTIPLE_BLOCK, SDHCI_CMD_RESP_SHORT |
                                          SDHCI_CMD_CRC | SDHCI_CMD_INDEX | SDHCI_CMD_DATA));
  uint32_t spins = 0;
  for (;;) {
    status = read32 (SDHCI_INT_STATUS);
    if (status & SDHCI_INT_CMD_MASK) {
      break;
    }
    if (++spins >= kCommandSpinLimit) {
      break;
    }
  }
  if (!(status & SDHCI_INT_RESPONSE) ||
      (status & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX))) {
    abortReadTransfer ();
    reportDmaFailure ("cmd", lba, blocks, status);
    return false;
  }
  uint64_t frequency = counterFrequency ();
  if (frequency < 1000000ULL || frequency > 1000000000ULL) {
    frequency = 19200000ULL;
  }
  // The same per-block share readMultiBlock uses: the card may hold the data line busy before the
  // first block, and a batch that is merely slow has to stay a batch.
  const uint64_t deadline = frequency / 5ULL +
                            static_cast<uint64_t> (blocks) * (frequency / 20ULL);
  const uint64_t start = readCounter ();
  bool ended = false;
  bool spun = false;
  uint32_t guard = 0;
  for (;;) {
    status = read32 (SDHCI_INT_STATUS);
    if (status & (kContentErrorMask | SDHCI_INT_ADMA_ERROR)) {
      break;
    }
    if (status & SDHCI_INT_DATA_END) {
      ended = true;
      break;
    }
    if (readCounter () - start > deadline) {
      break;
    }
    if (++guard >= kCommandSpinLimit) {
      spun = true;
      break;
    }
  }
  ticks = readCounter () - start;
  status = read32 (SDHCI_INT_STATUS);
  const bool stopped = sendStopCommand ();
  if (!ended || (status & (kContentErrorMask | SDHCI_INT_ADMA_ERROR)) || !stopped) {
    abortReadTransfer ();
    reportDmaFailure (!ended ? (spun ? "spin" : "deadline") : (stopped ? "error" : "stop"),
                      lba, blocks, status, ticks, deadline, guard, frequency);
    return false;
  }
  // The host wrote these lines behind the CPU's back; drop whatever the cache still holds of them.
  cacheInvalidate (gAdma.dataVirt, bytes);
  const uint32_t *source = reinterpret_cast<const uint32_t *> (gAdma.dataVirt);
  const uint32_t words = bytes / 4u;
  for (uint32_t word = 0; word < words; word++) {
    dst[word] = source[word];
  }
  return true;
}

// The verdict this round exists to produce: the same run through the path the r167 gate already
// trusted, and through ADMA, compared word for word. Both timings print, which is the number the
// schedule asked for -- 64 KiB per request at the PIO rate measured two bytes at a time against the
// same request when the host empties its own FIFO.
static void checkDmaPath (uint32_t lba, uint32_t sample)
{
  gDmaCheckDone = true;
  uint64_t frequency = counterFrequency ();
  if (frequency < 1000000ULL || frequency > 1000000000ULL) {
    frequency = 19200000ULL;
  }
  const uint32_t words = sample * kBlockWords;
  const uint32_t bytes = words * 4u;
  uint32_t got = 0, status = 0;
  uint64_t pioStart = readCounter ();
  bool pio = readMultiBlock (lba, gCheckBatchB, sample, got, status);
  if (!pio) {
    pio = readMultiBlock (lba, gCheckBatchB, sample, got, status);
  }
  const uint64_t pioTicks = readCounter () - pioStart;
  uint32_t dmaStatus = 0;
  uint64_t dmaTicks = 0;
  bool dma = readAdmaRun (lba, gCheckSingle, sample, dmaStatus, dmaTicks);
  if (!dma) {
    dma = readAdmaRun (lba, gCheckSingle, sample, dmaStatus, dmaTicks);
  }
  uint32_t first = 0;
  const uint32_t diff = (pio && dma && got == words)
                        ? countWordDiff (gCheckBatchB, gCheckSingle, words, first)
                        : words;
  gDmaTrusted = pio && dma && got == words && diff == 0;
  const uint64_t pioMs = pioTicks * 1000ULL / frequency;
  const uint64_t dmaMs = dmaTicks * 1000ULL / frequency;
  // KB/s from a run that took at least a millisecond; a zero-ms transfer is a rate this line cannot
  // state honestly, so it prints the milliseconds and lets the reader divide.
  const uint64_t pioRate = pioMs > 0 ? (bytes / 1024ULL) * 1000ULL / pioMs : 0;
  const uint64_t dmaRate = dmaMs > 0 ? (bytes / 1024ULL) * 1000ULL / dmaMs : 0;
  sdhciSay ("CloverSdhci r172: dmacheck lba=%u sample=%u pio=%d dma=%d got=%u diff=%u first=%u "
         "m=%08x s=%08x ms=%llu/%llu kbs=%llu/%llu trusted=%d\n",
         lba, sample, pio ? 1 : 0, dma ? 1 : 0, got, diff, first,
         gCheckBatchB[first < words ? first : 0],
         gCheckSingle[first < words ? first : 0],
         static_cast<unsigned long long> (pioMs), static_cast<unsigned long long> (dmaMs),
         static_cast<unsigned long long> (pioRate), static_cast<unsigned long long> (dmaRate),
         gDmaTrusted ? 1 : 0);
  if (!gDmaTrusted) {
    sdhciSay ("CloverSdhci r172: reads stay on the PIO path for this boot\n");
  } else {
    sdhciSay ("CloverSdhci r172: ADMA2 reads trusted for this boot\n");
  }
}

extern "C" uint32_t clover_emmc_block_read (uint32_t lba, uint32_t *words, uint32_t blocks)
{
  if (!words || !blocks) {
    return 0;
  }
  const uint32_t sectors = __atomic_load_n (&gBlockSectors, __ATOMIC_ACQUIRE);
  if (!__atomic_load_n (&gBlockReady, __ATOMIC_ACQUIRE) || !sectors ||
      lba >= sectors || blocks > sectors - lba) {
    return 0;
  }
  uint64_t frequency = 0;
  uint64_t start = 0;
  uint32_t token = 0;
  if (!acquireBlockGuard (frequency, start, token)) {
    reportReadFailure ("guard", lba, blocks, 0, 0);
    return 0;
  }
  // Round210: the record goes down before the first command this request sends, so a read that
  // hangs in waitCardIdle or in its transfer still has its own name on the card.
  flightEnter (kFlightEventRead, 0, lba, blocks);
  // Round169: the card can still be programming after the write that owned the guard was counted
  // as stored; a read that starts then is refused. Wait for the card, then start the deadline.
  flightStage (kStageIdleWait);
  bool cardReady = waitCardIdle (frequency);
  if (!cardReady) {
    // The card never said ready. Stop it explicitly, or every later read inherits the same state.
    const uint32_t reason = __atomic_load_n (&gIdleFailReason, __ATOMIC_RELAXED);
    abortReadTransfer ();
    reportReadFailure ("idle", lba, blocks, 0, 0);
    // Round215: a card that answered CMD13 is busy and will come back on its own; one that did not
    // answer at all, or a host that never dropped DATA_INHIBIT, is the wedge this round powers out.
    // The ladder proves itself with LBA 1, so the wait is retried once against a link it verified.
    if (reason != kIdleBusy && cardRecoverHeld ("idle", reason)) {
      flightStage (kStageIdleWait);
      cardReady = waitCardIdle (frequency);
    }
  }
  if (!cardReady) {
    abortReadTransfer ();
    flightStage (kStageTail);
    flightExit (0);
    releaseBlockGuard (token);
    return 0;
  }
  // Round165: the first request long enough to be a batch proves batch and single-block reads agree
  // before any caller relies on the batch. Round171: "long enough" is the same four blocks the batch
  // path itself demands, and the sample is the request's own length, so a boot whose biggest read is
  // 16 KiB still gets its one check instead of waiting forever for a 64 KiB one.
  if (!gReadCheckDone && blocks >= 4) {
    flightStage (kStageCheck);
    checkReadPath (lba, blocks > kReadChunkBlocks ? kReadChunkBlocks : blocks);
  }
  // Round172: the second gate, one run later. It only opens once the first has proved the multi-block
  // command itself, because ADMA2 rides on exactly that command with one bit added -- a boot whose
  // batch reads are untrusted has no business comparing a DMA read against anything.
  if (!gDmaCheckDone && gReadBatchTrusted && blocks >= 4 && dmaBringUp ()) {
    flightStage (kStageCheck);
    checkDmaPath (lba, blocks > kReadChunkBlocks ? kReadChunkBlocks : blocks);
  }
  // Round169: the request deadline covers the transfer, not the wait above or the one-shot check.
  start = readCounter ();
  // Round178: `owned` is the guarded time already spent, so the deadline below means the same thing
  // it did when one owner held the host for the whole run -- but the clock stops while the host is
  // with somebody else. `cutIn` records that this request gave its host away at least once.
  uint64_t owned = 0;
  uint32_t sinceCut = 0;
  bool cutIn = false;
  bool held = true;
  bool recovered = false;
  // Round167: the whole-request deadline scales with the run. A flat two seconds was sized for a
  // batch that finishes in milliseconds; a request that falls back to single blocks over a large
  // APFS or dyld read can exceed it, and a truncated read is an EIO the filesystem reports as
  // "failed to read". Round168 widens the base to five seconds and the per-block share to 20 ms:
  // the r167 frame showed both disks returning EIO at fixed block numbers while a checkpoint flush
  // held the card for 22.7 seconds, and these are the blocks APFS re-asked for.
  const uint64_t deadline = frequency * 5ULL +
                            static_cast<uint64_t>(blocks) * (frequency / 50ULL);
  uint32_t done = 0;
  while (done < blocks) {
    // Round213: the watchdog only takes the host back from a holder that has stopped. If it did,
    // this request must stop too, and must not touch the controller somebody else now owns.
    if (guardLost (token)) {
      reportReadFailure ("guard", lba, blocks, done, 0);
      held = false;
      break;
    }
    if (owned + readCounter () - start > deadline) {
      reportReadFailure ("deadline", lba, blocks, done, 0);
      // Round213: a request that ran out of time leaves the controller mid-transfer unless it says
      // so; the next reader would then inherit a data state nobody reset.
      abortReadTransfer ();
      // Round215: the card said it was ready and then moved no block at all. The ladder runs once
      // per request and the chunk is retried on the link its LBA 1 read just verified; a second
      // deadline ends the request, so a card that stays gone cannot hold this loop.
      if (done == 0 && !recovered && cardRecoverHeld ("deadline", 0)) {
        recovered = true;
        start = readCounter ();
        owned = 0;
        continue;
      }
      break;
    }
    uint32_t chunk = blocks - done;
    if (chunk > kReadChunkBlocks) {
      chunk = kReadChunkBlocks;
    }
    // Round157: `done * kBlockWords` was a 32-bit product, so past 16 GiB the
    // staging pointer wrapped. The caller's contract allows any run the card
    // owns; compute the word offset wide.
    uint32_t *target = words + static_cast<uint64_t>(done) * kBlockWords;
    // One CMD18 for the chunk; a failed multi-block attempt falls back to the
    // single-block sequence every earlier round proved, so the device can never
    // become less readable than it was before this change. Round165 adds the
    // trusted flag: the self-check above disables the batch for this boot if
    // batch and single-block reads disagree. Round168 gives the batch one clean
    // retry (readMultiBlock resets the data circuit on failure) before the
    // single-block fallback, because a card that was programming when CMD18
    // arrived refuses once and answers the second time.
    bool chunkRead = false;
    // Round172: the DMA path first, once the device has said its words agree with the PIO path's.
    // A refused DMA chunk costs one report and falls through to the PIO batch below, so the worst
    // case is the round171 behaviour with a line next to it; eight failures retire DMA for the boot.
    if (chunk >= 4 && gDmaTrusted) {
      uint32_t dmaStatus = 0;
      uint64_t dmaTicks = 0;
      flightStage (kStageAdma);
      if (readAdmaRun (lba + done, target, chunk, dmaStatus, dmaTicks)) {
        chunkRead = true;
        static bool timingReported = false;
        if (!timingReported) {
          timingReported = true;
          sdhciSay ("CloverSdhci r172: first dma read lba=%u blocks=%u ms=%llu\n", lba + done, chunk,
                 static_cast<unsigned long long> (dmaTicks * 1000ULL / frequency));
        }
      } else {
        gDmaFailures++;
        if (gDmaFailures >= 8 && gDmaTrusted) {
          gDmaTrusted = false;
          sdhciSay ("CloverSdhci r172: dma failed %u times, reads go back to the PIO path\n",
                 gDmaFailures);
        }
      }
    }
    if (!chunkRead && chunk >= 4 && gReadBatchTrusted) {
      for (uint32_t attempt = 0; attempt < 2 && !chunkRead; attempt++) {
        uint32_t got = 0;
        uint32_t status = 0;
        flightStage (kStageBatch);
        chunkRead = readMultiBlock (lba + done, target, chunk, got, status);
      }
    }
    uint32_t moved = 0;
    if (chunkRead) {
      moved = chunk;
    } else {
      // Round168: one block, three attempts, each from a reset data circuit. The old shape reported
      // EIO after the first refusal, and APFS re-read the same block forever.
      bool blockRead = false;
      uint32_t lastStatus = 0;
      for (uint32_t attempt = 0; attempt < 3 && !blockRead; attempt++) {
        uint32_t got = 0;
        uint32_t status = 0;
        flightStage (kStageSingle);
        blockRead = readPioBlock (lba + done, target, got, status);
        lastStatus = status;
      }
      if (!blockRead) {
        reportReadFailure ("gave up", lba + done, blocks, done, lastStatus);
        abortReadTransfer ();
        // Round215: three single-block attempts from a reset circuit all refused. The card is not
        // slow, it is not there; the ladder runs once and the same block is asked again.
        if (!recovered && cardRecoverHeld ("gaveup", lastStatus)) {
          recovered = true;
          start = readCounter ();
          owned = 0;
          continue;
        }
        break;
      }
      moved = 1;
    }
    done += moved;
    sinceCut += moved;
    // Round178: a full chunk of progress is the unit of ownership. Release the host here and a page
    // fault at the other volume gets its read in the middle of this run instead of after it -- the
    // r170 frame's `read guard ... st=00000000` rows are reads that were never refused by the card,
    // only by the holder ahead of them. Falling back to single blocks counts the same way, so a boot
    // whose batch is untrusted still cuts every 128 blocks instead of every request.
    if (done < blocks && sinceCut >= kReadChunkBlocks) {
      sinceCut = 0;
      if (!handOffBlockGuard (frequency, start, owned, token)) {
        reportReadFailure ("guard", lba + done, blocks, done, 0);
        held = false;
        break;
      }
      cutIn = true;
    }
  }
  flightStage (kStageTail);
  flightExit (done);
  if (cutIn) {
    reportGuardHandoff ("read", lba, blocks, done);
  }
  // Round174: this is one of the places the guard is held and the card is idle, so it is also where
  // pending verdict lines get written out. Four records per pass; the rest wait for the next request.
  // Round175: the kernel log tee runs first, so a line it accepts here can be flushed in this pass.
  if (held) {
    // Round213: after a recovered steal this request no longer owns the host, so it must not write
    // to the card through the tee or the verdict ring; it only gives back its own ticket.
    if (!guardLost (token)) {
      flightStage (kStageTee);
      teeKernelLog ();
      flightStage (kStageFlush);
      flushVerdictRing ();
    }
    releaseBlockGuard (token);
  }
  return done;
}

// After a write the card programs, and its R1 reports READY_FOR_DATA in TRAN only when the block
// is actually stored. Nothing above this layer retries a write it was told succeeded, and every
// boot here ends in a hard reset, so the confirmation is part of the write. DATA_INHIBIT first
// covers the data line the card holds busy while programming; CMD13 then names the state. The
// whole wait is bounded by the same 200 ms counter the other post-command waits use, and `polls`
// counts the CMD13s that said "still programming" so a slow card reads as a number, not a hang.
static void waitCardReady (uint32_t &response, uint32_t &status, uint32_t &polls);

static bool writeSettled (uint32_t &response, uint32_t &status, uint32_t &polls)
{
  response = 0;
  status = 0;
  polls = 0;
  uint64_t frequency = counterFrequency ();
  if (frequency < 1000000ULL || frequency > 1000000000ULL) {
    frequency = 19200000ULL;
  }
  // Round168: 200 ms was the smallest wait that covered a batch on a fast link, and the r167 frame
  // showed the card taking far longer than that for a whole checkpoint (tx_flush 22.7 s). A batch
  // that is still programming is not a failed write -- aborting it was what turned the same blocks
  // into "disk5: general error" and made APFS re-send them. Round169 raises the bound to thirty
  // seconds, the same bound the guard and the read pre-wait use: the r168 frame showed the card
  // still busy long after four seconds, and every early abort left a card programming while the
  // host was already rewriting the same blocks.
  const uint64_t deadline = frequency * 30ULL;      // 30 s
  const uint64_t start = readCounter ();
  while (read32 (SDHCI_PRESENT_STATE) & SDHCI_DATA_INHIBIT) {
    if (readCounter () - start > deadline) {
      return false;
    }
  }
  for (;;) {
    uint32_t cardPolls = 0;
    waitCardReady (response, status, cardPolls);
    polls += cardPolls;
    if ((response & R1_READY_FOR_DATA) && R1_CURRENT_STATE (response) == R1_STATE_TRAN) {
      // Round168: the card's own programming time, once per boot. A number here explains a slow
      // flush without another measurement round.
      static bool slowReported = false;
      const uint64_t elapsed = readCounter () - start;
      if (!slowReported && elapsed > frequency / 2ULL) {
        slowReported = true;
        sdhciSay ("CloverSdhci r169: write settle took %llu ms polls=%u\n",
               static_cast<unsigned long long> (elapsed * 1000ULL / frequency), polls);
      }
      return true;
    }
    if (readCounter () - start > deadline) {
      return false;
    }
  }
}

// One whole 512-byte block, written the mirror image of readPioBlock: CMD24 with the block count
// enabled and the direction bit clear, a 200 ms deadline and a final level-gated sweep so a
// controller that still had room takes the rest. Round167 drops the 160 us gap between words that
// was copied from mode 12's 400 kHz identification clock: at 50 MHz it made a block cost 20.5 ms
// for no reason, and SDHCI backpressures the card whenever the buffer is full, so writing only
// while SPACE_AVAILABLE can never outrun it. A refused command or a failed data phase resets the
// data circuit, because a half-written block left active would wedge the card for the reads that
// follow. `wordCount` is how many words were handed to the data port; the return value is true
// only for a clean command response, DATA_END and all 128 words handed over.
// Round305: the refusal keeps its own name. Every card write on this driver -- a flight record, a ring
// row, the panic screen's two blocks -- ends in writePioBlock, and until now the answer to "why did
// that row not reach the card" was a bare false. The three exits below are three different causes, and
// the last one is only readable in the two out-params that the callers throw away, so they are copied
// here first.
static void pioFail (uint32_t code, uint32_t status, uint32_t words, uint32_t lba)
{
  __atomic_add_fetch (&gPioFails, 1u, __ATOMIC_RELAXED);
  gPioFailCode = code;
  gPioFailStatus = status;
  gPioFailWords = words;
  gPioFailLba = lba;
  gPioFailAttempt = gPioAttempt;
  gPioFailMs = flightCounterMs (gFlightStart);
}

static bool writePioBlock (uint32_t lba, const uint32_t *words, uint32_t &wordCount,
                           uint32_t &status)
{
  wordCount = 0;
  status = 0;
  if (!waitCommandSlot ()) {
    pioFail (1, 0, 0, lba);
    resetReadPath ();
    return false;
  }
  write8 (SDHCI_TIMEOUT_CONTROL, 0x0e);
  write16 (SDHCI_BLOCK_SIZE, 512);
  write16 (SDHCI_BLOCK_COUNT, 1);
  write32 (SDHCI_INT_STATUS, 0xffffffffu);
  write32 (SDHCI_ARGUMENT, lba);
  write16 (SDHCI_TRANSFER_MODE, SDHCI_TRNS_BLK_CNT_EN);   // READ clear: host to card
  write16 (SDHCI_COMMAND, SDHCI_MAKE_CMD (MMC_WRITE_BLOCK, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                                          SDHCI_CMD_INDEX | SDHCI_CMD_DATA));
  uint32_t spins = 0;
  for (;;) {
    status = read32 (SDHCI_INT_STATUS);
    if (status & SDHCI_INT_CMD_MASK) {
      break;
    }
    if (++spins >= kCommandSpinLimit) {
      break;
    }
  }
  if (!(status & SDHCI_INT_RESPONSE) ||
      (status & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX))) {
    pioFail (2, status, 0, lba);
    resetReadPath ();
    return false;
  }
  uint64_t frequency = counterFrequency ();
  if (frequency < 1000000ULL || frequency > 1000000000ULL) {
    frequency = 19200000ULL;
  }
  const uint64_t deadline = frequency / 5ULL;      // 200 ms
  const uint64_t start = readCounter ();
  uint32_t guard = 0;
  for (;;) {
    status = read32 (SDHCI_INT_STATUS);
    if (status & (SDHCI_INT_DATA_END | SDHCI_INT_DATA_TIMEOUT | SDHCI_INT_DATA_CRC |
                  SDHCI_INT_DATA_END_BIT | SDHCI_INT_ADMA_ERROR)) {
      break;
    }
    if (readCounter () - start > deadline) {
      break;
    }
    if (wordCount < kBlockWords &&
        (read32 (SDHCI_PRESENT_STATE) & SDHCI_SPACE_AVAILABLE)) {
      write32 (SDHCI_BUFFER, words[wordCount++]);
    }
    if (++guard >= kCommandSpinLimit) {
      break;
    }
  }
  while (wordCount < kBlockWords &&
         (read32 (SDHCI_PRESENT_STATE) & SDHCI_SPACE_AVAILABLE)) {
    write32 (SDHCI_BUFFER, words[wordCount++]);
  }
  if ((status & SDHCI_INT_DATA_END) && !(status & kDataErrorMask) &&
      wordCount == kBlockWords) {
    return true;
  }
  // Round305: which of the three data-phase conditions said no. A card that never raised DATA_END and
  // a host that raised an error bit and a transfer that ran out of its deadline with words still in
  // the buffer are three different boards, and until now all three reached the caller as false.
  pioFail ((status & kDataErrorMask) ? 4u : ((status & SDHCI_INT_DATA_END) ? 5u : 3u),
           status, wordCount, lba);
  resetReadPath ();
  return false;
}

// ------------------------------------------------- round174: the verdicts outlive the panel
//
// WHY THE PANEL IS NOT ENOUGH
//
// Every round since round165 has ended the same way: the one line that decides the next round was
// on the screen for a fraction of a second and then scrolled under the lines it was supposed to
// explain. Round167 lost `write cache ... off=` next to `disk5: general error`; round169 lost the
// driver's own `read <stage>` verdicts behind the panic; round170 exists because r153's line
// repacked the panel so completely that a whole frame could not say whether launchd was looping.
// This round stops asking the panel to hold a reading. The card already stores text records the
// host can dd back (mode 17 wrote the first one), so the driver now writes its verdict lines there
// too, in kind 21 records in a 32-slot ring at `logfs start + 64` -- past the 32 blocks the panic
// writer owns, and well inside the 16384-sector window round127 matched. One `dd` reads them all.
//
// WHY THE FORMATTER IS HAND-WRITTEN
//
// `vsnprintf` is not declared in the header set this driver is compiled against (IOLib.h,
// libkern.h, pexpert.h, debug.h), and no sheng driver has a precedent for pulling in libc's. The
// alternative -- calling IOLog and reconstructing the text from a second argument list -- would
// need the same formatter anyway and give the screen and the card two chances to disagree. So one
// formatter runs once, and its single string goes to both. It covers exactly the specifiers this
// driver's lines use: %s, %u, %d, %x, %08x, %02x, %llu, %llx, %p. Anything else is copied verbatim,
// which is the wrong-but-visible answer, and better than silently dropping a reading.
//
// WHY THE CARD WRITE IS NOT AT THE DECISION POINT
//
// A verdict is usually produced *inside* a transfer that just failed, on a host with one command
// engine and a caller that is about to retry. Starting two more block writes there would nest a
// second owner around the failing request and add a new way to hang the boot. So saying something
// only fills a RAM slot, and the ring is written to the card from the tails of the read, write and
// cache-flush requests -- places that already hold the block guard, own an idle card, and run
// repeatedly. Records per pass are capped per ring (four for the driver's, eight for the kernel's),
// because a flush must not become the long guard hold this driver has spent three rounds removing.

static uint32_t gRingBlock[kBlockWords];
// Round210: its own block, because a beat may run on top of a request that is halfway through
// filling gRingBlock at the tail it was interrupted in.
static uint32_t gFlightBlock[kBlockWords];
// Round302: and a third one, for the records that are verdicts rather than traffic -- the deadline,
// the panic marker and the panic sentence. boot 91 proved the traffic buffer cannot hold a verdict:
// the entry path and a beat rebuild it while writePioBlock is still streaming it, so the sector ends
// up carrying one event's header and another's stamp. The forced events own this buffer alone, which
// is what makes their magic word mean something.
static uint32_t gFlightMarker[kBlockWords];

// One byte at a time, clamped: a verdict that overruns its slot is truncated rather than written
// past the array, and the cursor stops advancing so the reported length is the stored length.
static char *verdictPut (char *p, char *end, char c)
{
  if (p < end) {
    *p = c;
    return p + 1;
  }
  return p;
}

static char *verdictPutText (char *p, char *end, const char *s)
{
  while (s && *s) {
    p = verdictPut (p, end, *s++);
  }
  return p;
}

// Digits are produced least significant first into a stack buffer, then reversed, so padding can be
// appended to the high end and lands in front. No division library: 64-bit div is native on arm64.
static char *verdictPutNumber (char *p, char *end, unsigned long long value, unsigned base,
                               unsigned width, bool zeroPad)
{
  const char *set = "0123456789abcdef";
  char digits[24];
  uint32_t count = 0;
  if (!value) {
    digits[count++] = set[0];
  }
  while (value) {
    digits[count++] = set[value % base];
    value /= base;
  }
  while (count < width && count < sizeof (digits)) {
    digits[count++] = zeroPad ? set[0] : ' ';
  }
  while (count) {
    p = verdictPut (p, end, digits[--count]);
  }
  return p;
}

static uint32_t verdictFormat (char *out, uint32_t cap, const char *fmt, va_list args)
{
  char *p = out;
  char *end = out + cap;
  while (*fmt) {
    char c = *fmt++;
    if (c != '%') {
      p = verdictPut (p, end, c);
      continue;
    }
    c = *fmt++;
    if (c == '%') {
      p = verdictPut (p, end, '%');
      continue;
    }
    bool zeroPad = false;
    unsigned width = 0;
    while (c == '0' || (c >= '1' && c <= '9')) {
      if (c == '0' && !width) {
        zeroPad = true;
      } else {
        width = width * 10u + (unsigned) (c - '0');
      }
      c = *fmt++;
    }
    bool wide = false;
    if (c == 'l') {
      if (*fmt == 'l') {
        fmt++;
      }
      wide = true;
      c = *fmt++;
    }
    if (c == 's') {
      p = verdictPutText (p, end, va_arg (args, const char *));
      continue;
    }
    if (c == 'p') {
      const void *pointer = va_arg (args, const void *);
      if (!pointer) {
        p = verdictPutText (p, end, "nil");
      } else {
        p = verdictPutNumber (p, end,
                              static_cast<unsigned long long> (reinterpret_cast<uintptr_t> (pointer)),
                              16u, wide ? 16u : 0u, false);
      }
      continue;
    }
    unsigned long long value = 0;
    bool negative = false;
    if (c == 'd') {
      if (wide) {
        const long long signedValue = va_arg (args, long long);
        if (signedValue < 0) {
          negative = true;
        }
        value = negative ? static_cast<unsigned long long> (-signedValue)
                         : static_cast<unsigned long long> (signedValue);
      } else {
        const int signedValue = va_arg (args, int);
        if (signedValue < 0) {
          negative = true;
        }
        value = negative ? static_cast<unsigned long long> (-static_cast<long long> (signedValue))
                         : static_cast<unsigned long long> (signedValue);
      }
      if (negative) {
        p = verdictPut (p, end, '-');
      }
      p = verdictPutNumber (p, end, value, 10u, width, zeroPad);
      continue;
    }
    if (c == 'u') {
      value = wide ? va_arg (args, unsigned long long)
                   : static_cast<unsigned long long> (va_arg (args, unsigned int));
      p = verdictPutNumber (p, end, value, 10u, width, zeroPad);
      continue;
    }
    if (c == 'x' || c == 'X') {
      value = wide ? va_arg (args, unsigned long long)
                   : static_cast<unsigned long long> (va_arg (args, unsigned int));
      p = verdictPutNumber (p, end, value, 16u, width, zeroPad);
      continue;
    }
    // Unknown specifier: print it as written. A dropped argument would shift every later number on
    // the line, which turns one bad reading into a page of plausible wrong ones.
    p = verdictPut (p, end, '%');
    p = verdictPut (p, end, c);
  }
  uint64_t length = static_cast<uint64_t> (p - out);
  return length > cap ? cap : static_cast<uint32_t> (length);
}

// Put one finished line into a RAM ring without printing it. sdhciSay uses this after IOLog; the
// msgbuf tee uses it alone, because the tee's input is the very buffer IOLog writes into -- printing a
// line it just read would append a second copy of itself, and the tee would then match its own echo
// forever. The trailing newline is dropped by both callers: the record writer adds exactly one.
// `which` is the ring, not the producer: round176's two card rings are the same record shape with
// different geometry, and the only difference in behaviour is which one gets to be crowded out.
static void ringPush (uint32_t which, const char *line, uint32_t length)
{
  if (length > kVerdictTextBytes) {
    length = kVerdictTextBytes;
  }
  if (gRingPending[which] == kRingRamSlots) {
    // Full: the oldest unread line gives its slot to the newest one. Which reading was dropped is
    // counted, so a frame whose ring wrapped says so instead of looking like a quiet boot.
    gRingWrite[which]++;
    gRingPending[which]--;
    gRingDropped[which]++;
  }
  const uint32_t slot = gRingWrite[which] % kRingRamSlots;
  for (uint32_t i = 0; i < length; i++) {
    gRingText[which][slot][i] = line[i];
  }
  gRingLength[which][slot] = length;
  gRingWrite[which]++;
  gRingPending[which]++;
}

// The driver's voice. Same string to the panel and to the ring, so a frame and a dd can never
// disagree about what was measured.
static void sdhciSay (const char *fmt, ...)
{
  char line[kVerdictTextBytes + 1];
  va_list args;
  va_start (args, fmt);
  uint32_t length = verdictFormat (line, kVerdictTextBytes, fmt, args);
  va_end (args);
  while (length && line[length - 1] == '\n') {
    length--;
  }
  line[length] = '\0';
  IOLog ("%s\n", line);
  ringPush (kRingDriver, line, length);
}

// The one way into the kernel ring. Both producers go through it: the tee, which lifts the kernel's
// own lines out of msgbuf, and round209's panic sink, which reads the same buffer backwards when no
// block I/O is left to run the tee. Keeping the raw push in one place is also what lets the checker
// assert the ring has exactly one entry point.
static void kernelRingPush (const char *line, uint32_t length)
{
  ringPush (kRingKernel, line, length);
}

// Round212: the one way into the probe ring. The kernel probe (patched print sites in kern_sig.c
// and task.c) calls this by name; the driver is linked into the kernel, so there is no address to
// patch and nothing to keep in sync. A caller can be any thread on any CPU, which is why the only
// shared state is the slot counter: each caller takes a distinct number, writes its text, then
// seals the slot with that number. A slot whose seal is not the number the reader expects was
// overwritten between the two steps, and the drain skips it rather than writing half a line.
extern "C" unsigned int clover_emmc_probe_say (const char *text, unsigned int length)
{
  if (!text || !length) {
    __atomic_fetch_add (&gProbeEmptySay, 1u, __ATOMIC_RELAXED);
    return 0;
  }
  if (length > kProbeTextBytes - 1) {
    // Round306: counted, because a row that arrived at the card with its tail missing is a different
    // fact from a row that never arrived, and only this side of the copy can tell them apart.
    __atomic_fetch_add (&gProbeCutSay, 1u, __ATOMIC_RELAXED);
    length = kProbeTextBytes - 1;
  }
  const uint32_t seq = __atomic_fetch_add (&gProbeSeq, 1u, __ATOMIC_RELAXED);
  const uint32_t slot = seq % kProbeSlots;
  if (seq >= kProbeSlots) {
    __atomic_fetch_add (&gProbeDropped, 1u, __ATOMIC_RELAXED);
  }
  for (uint32_t i = 0; i < length; i++) {
    gProbeText[slot][i] = text[i];
  }
  gProbeText[slot][length] = '\0';
  __atomic_store_n (&gProbeArm[slot], seq + 1, __ATOMIC_RELEASE);
  return seq + 1;
}

// One LOG1 record, same shape as mode 17 and the panic writer: header block then text block, byte
// sum over prefix + body + newline. Word 8 carries the sequence number, which is what makes a
// wrapped ring orderable: slot index alone stops being time once a ring has more records than slots.
// Word 5 carries the ring's own kind -- 21 for the driver's verdicts, 22 for the kernel lines the tee
// lifted -- because the host reads two rings out of one dd and a record has to say which one it came
// from before anything else about it is trusted. Word 9 is the boot id, so a record left by an
// earlier boot in a slot this one has not reached is not read as this boot's.
static bool ringWriteRecord (uint32_t which, const char *text, uint32_t length,
                             uint32_t kind = 0)
{
  const uint32_t body = length > 480 ? 480 : length;
  const uint32_t total = kLogPrefixLength + body + 1;
  uint32_t sum = 0;
  for (uint32_t i = 0; i < kLogPrefixLength; i++) {
    sum += static_cast<uint8_t> (kLogPrefix[i]);
  }
  for (uint32_t i = 0; i < body; i++) {
    sum += static_cast<uint8_t> (text[i]);
  }
  sum += static_cast<uint8_t> ('\n');

  const uint32_t lba = gVerdictFirst + kRingOffset[which] +
                       2 * (gRingCursor[which] % kRingCardSlots[which]);
  uint32_t words = 0, status = 0;
  for (uint32_t i = 0; i < kBlockWords; i++) {
    gRingBlock[i] = 0;
  }
  gRingBlock[0] = kLogMagic0;
  gRingBlock[1] = kLogMagic1;
  gRingBlock[2] = lba;
  gRingBlock[3] = total;
  gRingBlock[4] = sum;
  gRingBlock[5] = kind ? kind : ((which == kRingKernel) ? 22 : 21);
  gRingBlock[6] = body;
  gRingBlock[7] = gRingCursor[which] % kRingCardSlots[which];
  gRingBlock[8] = gRingSeq[which];
  gRingBlock[9] = gBootId;
  uint32_t rsp = 0, rstatus = 0, polls = 0;
  // Round305: name the channel before writing it, so a refusal says which one (21 driver verdicts,
  // 22 the tee's kernel rows, 25 the relayed probe lines).
  gPioAttempt = kind ? kind : ((which == kRingKernel) ? 22u : 21u);
  if (!writePioBlock (lba, gRingBlock, words, status)) {
    return false;
  }
  waitCardReady (rsp, rstatus, polls);

  uint8_t *block = reinterpret_cast<uint8_t *> (gRingBlock);
  for (uint32_t i = 0; i < 512; i++) {
    block[i] = 0;
  }
  for (uint32_t i = 0; i < kLogPrefixLength; i++) {
    block[i] = static_cast<uint8_t> (kLogPrefix[i]);
  }
  for (uint32_t i = 0; i < body; i++) {
    uint8_t c = static_cast<uint8_t> (text[i]);
    block[kLogPrefixLength + i] =
        (c == '\n') ? static_cast<uint8_t> ('\n')
                    : (c >= 0x20 && c < 0x7f) ? c : static_cast<uint8_t> ('.');
  }
  block[kLogPrefixLength + body] = '\n';
  if (!writePioBlock (lba + 1, gRingBlock, words, status)) {
    return false;
  }
  waitCardReady (rsp, rstatus, polls);
  gRingCursor[which]++;
  gRingSeq[which]++;
  gRingWritten[which]++;
  return true;
}

// Round212: the boot id, one record forward from the last boot's. A counter in RAM cannot survive
// the reset the port is chasing, and the generic counter's low half wraps every 223 s at 19.2 MHz
// -- a window two boots in a row can land in. So the previous id is read back from the record past
// the kernel ring, this boot takes one more, and writes it back before any record carries it. A
// card that has never held one, or a record that fails its own magic, starts the count at 1. The
// record is one block and its payload is the number in the header's word 9, which is all a nonzero
// byte sum needs; a second block would add a write to a boot that already costs one.
static uint32_t bootIdClaim (uint32_t lba)
{
  uint32_t words = 0, status = 0, rsp = 0, rstatus = 0, polls = 0;
  uint32_t boot = 1;
  if (readPioBlock (lba, gReadBlock, words, status) &&
      gReadBlock[0] == kLogMagic0 && gReadBlock[1] == kLogMagic1 &&
      gReadBlock[5] == kBootKind) {
    boot = gReadBlock[9] + 1;
    if (boot == 0) {
      boot = 1;
    }
  }
  for (uint32_t i = 0; i < kBlockWords; i++) {
    gRingBlock[i] = 0;
  }
  gRingBlock[0] = kLogMagic0;
  gRingBlock[1] = kLogMagic1;
  gRingBlock[2] = lba;
  gRingBlock[5] = kBootKind;
  gRingBlock[9] = boot;
  gPioAttempt = 24;    // round305: the boot-id block is its own channel
  if (!writePioBlock (lba, gRingBlock, words, status)) {
    return boot;
  }
  waitCardReady (rsp, rstatus, polls);
  return boot;
}

// ------------------------------------------------------------- round210: the flight region
//
// WHAT IT IS FOR
//
// Round208's ring went quiet at 167 s and round209's at 357.9 s, both times with the kernel still
// printing, and round209 added the fact that the card refused the panic sink too. Two readings fit
// that shape: a transfer stopped in flight with the controller inhibited, or the block layer parked
// somewhere that never reaches the controller. The channel that explains it has to be written
// *before* the stop, because after it nothing reaches the card -- so this round writes a small
// record at the entry of every block request (kind 23, one block, twelve slots at sector 32) that
// names the request and the result and duration of the one before it. A boot that stops has, at
// worst, a last entry record with no successor: that is the request that never came back.
//
// The second half is the beat. CPU0's timer calls clover_emmc_flight_beat() every four seconds; it
// takes the block guard with IOLockTryLock and, when the guard is free, writes a beat record (four
// slots at sector 56) carrying SDHCI_PRESENT_STATE and the pending counts. The presence of a beat is
// itself the reading: the guard was free and the controller accepted a command. When the beats stop,
// the last entry record names the request, and the last beat bounds the time between the two.
// A beat never writes while the panic sink is running (gPanicActive) or while the guard is held.
static uint32_t flightCounterMs (uint64_t since)
{
  uint64_t frequency = counterFrequency ();
  if (frequency < 1000000ULL || frequency > 1000000000ULL) {
    frequency = 19200000ULL;
  }
  const uint64_t now = readCounter ();
  if (!since || now <= since) {
    return 0;
  }
  return static_cast<uint32_t> ((now - since) * 1000ULL / frequency);
}

// Round305: the other clock on this board, read in the same breath as the one above. Every kernel log
// line is stamped with it (osfmk/kern/task.c's clover_now_seconds() is these same two calls), and so is
// every msgbuf row this driver tees onto the card -- which is why the two have always looked
// commensurate and never were. Boot 94's panic marker is the case in point: its `t=` is milliseconds
// since the driver armed the flight region, an event that happens some unknown seconds after the
// kernel's own zero, so the marker could be bounded in kernel seconds but never placed in them. From
// this round every beat carries the pair at one code point, and the lag between the two is a number on
// the card rather than a subtraction between two records that may not even name the same instant.
static uint32_t kernelSeconds ()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds (mach_absolute_time (), &ns);
  return static_cast<uint32_t> (ns / 1000000000ULL);
}

static bool flightWrite (uint32_t event, uint32_t dir, uint32_t lba, uint32_t blocks,
                         const uint32_t *extra = nullptr, uint32_t extraWords = 0)
{
  // Round236: the deadline record and the panic marker are the two readings that must survive the
  // death of everything else, so they do not depend on the flight region having been armed by a
  // boot that reached its block-driver phase. They still need the window base and the host: without
  // those there is nowhere to write.
  // Round302 adds the third forced event, the panic sentence, and a rule that comes with it: a forced
  // record is built in gFlightMarker, never in gFlightBlock, because boot 91 showed that the traffic
  // buffer can be rebuilt by an entry or a beat while writePioBlock is still streaming it, which is
  // how a sector ends up with one event's header and another event's boot stamp.
  const bool forced = (event == kFlightEventDeadline) || (event == kFlightEventPanic) ||
                      (event == kFlightEventText) || (event == kFlightEventDrainIn);
  if (!gVerdictFirst || !gHost) {
    return false;
  }
  if (!forced && !__atomic_load_n (&gFlightArmed, __ATOMIC_ACQUIRE)) {
    return false;
  }
  const bool beat = (event == kFlightEventBeat);
  uint32_t offset = kFlightEntryOffset, slots = kFlightEntrySlots;
  uint32_t *seqp = &gFlightEntrySeq;
  if (beat) {
    offset = kFlightBeatOffset;
    slots = kFlightBeatSlots;
    seqp = &gFlightBeatSeq;
  } else if (event == kFlightEventDeadline) {
    offset = kFlightDeadlineOffset;
    slots = kFlightDeadlineSlots;
    seqp = &gFlightDeadlineSeq;
  } else if (event == kFlightEventPanic) {
    offset = kFlightPanicOffset;
    slots = kFlightPanicSlots;
    seqp = &gFlightPanicSeq;
  } else if (event == kFlightEventDrainIn) {
    offset = kFlightDrainOffset;
    slots = kFlightDrainSlots;
    seqp = &gFlightDrainSeq;
  } else if (event == kFlightEventText) {
    offset = kFlightTextOffset;
    slots = kFlightTextSlots;
  }
  // Round302: the sentence's slot is chosen by the boot id the card already keeps across resets, so
  // the sink can write it without reading anything first. That pre-read is where boot 90 lost the
  // text: panicAppendText() reads all sixteen report slots to find the oldest one, boot 90's read was
  // refused with the controller held (host=0x20fdf), and the sink tries once.
  const uint32_t seq = (event == kFlightEventText) ? gBootId : *seqp;
  const uint32_t slot = seq % slots;
  const uint32_t record = gVerdictFirst + offset + slot;
  const uint32_t present = read32 (SDHCI_PRESENT_STATE);
  uint32_t *buf = forced ? gFlightMarker : gFlightBlock;
  for (uint32_t i = 0; i < kBlockWords; i++) {
    buf[i] = 0;
  }
  buf[0] = kLogMagic0;
  buf[1] = kLogMagic1;
  buf[2] = record;
  buf[3] = kFlightKind;
  buf[4] = seq;
  buf[5] = slot;
  buf[6] = event;
  buf[7] = dir;
  buf[8] = lba;
  buf[9] = blocks;
  buf[10] = flightCounterMs (gFlightStart);
  buf[11] = gFlightPrevDone;
  buf[12] = gFlightPrevMs;
  buf[13] = present;
  buf[14] = ((present & SDHCI_CMD_INHIBIT) ? 1u : 0u) |
                     ((present & SDHCI_DATA_INHIBIT) ? 2u : 0u) |
                     ((event == kFlightEventBeat) ? 4u : 0u);
  buf[15] = gFlightOps;
  buf[16] = __atomic_load_n (&gGuardWaits, __ATOMIC_RELAXED);
  buf[17] = __atomic_load_n (&gFlightBeatMisses, __ATOMIC_RELAXED);
  buf[18] = gRingPending[kRingKernel];
  buf[19] = gRingPending[kRingDriver];
  buf[20] = gFlightPrevDir;
  buf[21] = gFlightPrevBlocks;
  // Round213: the stage the holder was in when this record went down, how long it had been there,
  // and the watchdog's counters. A beat whose stage has outlived the next beat is the request that
  // stopped; the counters say whether the watchdog has already been there.
  buf[22] = __atomic_load_n (&gFlightStage, __ATOMIC_ACQUIRE);
  buf[23] = flightCounterMs (__atomic_load_n (&gFlightStageSince, __ATOMIC_ACQUIRE));
  buf[24] = __atomic_load_n (&gFlightStuck, __ATOMIC_RELAXED);
  buf[25] = __atomic_load_n (&gFlightRecovered, __ATOMIC_RELAXED);
  buf[26] = __atomic_load_n (&gFlightSteals, __ATOMIC_RELAXED);
  buf[27] = __atomic_load_n (&gFlightStuckStage, __ATOMIC_RELAXED);
  buf[28] = __atomic_load_n (&gFlightStageLba, __ATOMIC_RELAXED);
  buf[29] = __atomic_load_n (&gFlightStageBlocks, __ATOMIC_RELAXED);
  buf[30] = __atomic_load_n (&gFlightStagePid, __ATOMIC_RELAXED);
  buf[31] = __atomic_load_n (&gFlightStageDir, __ATOMIC_RELAXED);
  // Round213: the caller of this record's own request (entry) or of the last one (beat).
  buf[32] = gFlightCurPid;
  buf[33] = gFlightCurComm0;
  buf[34] = gFlightCurComm1;
  // Round236: the deadline's own numbers and the panic marker's gates ride after the round210
  // header, so the two readings keep one decoder instead of two record formats.
  for (uint32_t i = 0; i < extraWords && 35 + i < kBlockWords; i++) {
    buf[35 + i] = extra[i];
  }
  // Round293: which boot wrote this. Boot 79's frame is the reason the field exists: the deadline
  // slot and the panic slots are the only two groups a boot may never reach, and when it does not
  // the previous boot's copies stay on the card and read, word for word, as this boot's ending. An
  // entry or a beat carries no extras, so its stamp is the first word after the header; the two
  // events that do carry extras put the stamp after their own (the deadline's is extra[10], and the
  // panic marker already named gBootId as extra[6] since round236).
  if (extraWords == 0) {
    buf[35] = gBootId;
  }
  // Round302: the verdict record's own signature, in the word right after its extras and written
  // after them, so a record that never got this far fails its magic instead of being decoded from
  // whatever a previous writer left in the sector. This is what separates 'this boot entered the sink'
  // from 'this sector happens to be the one the sink would have used'.
  if (forced && 35 + extraWords < kBlockWords) {
    buf[35 + extraWords] = kFlightMarkerMagic;
  }
  uint32_t words = 0, status = 0;
  gPioAttempt = 100 + event;    // round305: whose refusal this is, if writePioBlock says no
  if (!writePioBlock (record, buf, words, status)) {
    return false;
  }
  // The card may still be programming; a beat does not wait for it, because a beat that spends
  // 200 ms inside the timer interrupt is its own problem. The deadline and panic records are taken
  // in contexts that are about to end the boot, so they do not wait either.
  if (!beat && !forced) {
    uint32_t rsp = 0, rstatus = 0, polls = 0;
    waitCardReady (rsp, rstatus, polls);
  }
  if (event != kFlightEventText) {
    // The sentence's slot is the boot id, so it has no sequence of its own to advance -- and the entry
    // ring's cursor must not move, because a write at the end of a boot would otherwise rotate the
    // request records that follow it on the next boot.
    (*seqp)++;
  }
  if (beat) {
    gFlightBeats++;
  }
  return true;
}

static void flightEnter (uint32_t event, uint32_t dir, uint32_t lba, uint32_t blocks)
{
  gFlightOpStart = readCounter ();
  gFlightCurDir = dir;
  gFlightCurLba = lba;
  gFlightCurBlocks = blocks;
  // Round213: who asked. The r212 frame knew the request -- 8 blocks from LBA 31076568 -- but not
  // which thread had asked for them, and the thread is the other half of "why this read and no
  // other". proc_selfname is null-safe; the pid is only asked once a name came back, so a context
  // with no process cannot turn into a null dereference here.
  char comm[16];
  for (uint32_t i = 0; i < sizeof comm; i++) {
    comm[i] = 0;
  }
  proc_selfname (comm, sizeof comm);
  gFlightCurPid = comm[0] ? static_cast<uint32_t> (proc_selfpid ()) : 0;
  const uint32_t c0 = static_cast<unsigned char> (comm[0]);
  const uint32_t c1 = static_cast<unsigned char> (comm[1]);
  const uint32_t c2 = static_cast<unsigned char> (comm[2]);
  const uint32_t c3 = static_cast<unsigned char> (comm[3]);
  const uint32_t c4 = static_cast<unsigned char> (comm[4]);
  const uint32_t c5 = static_cast<unsigned char> (comm[5]);
  const uint32_t c6 = static_cast<unsigned char> (comm[6]);
  const uint32_t c7 = static_cast<unsigned char> (comm[7]);
  gFlightCurComm0 = c0 | (c1 << 8) | (c2 << 16) | (c3 << 24);
  gFlightCurComm1 = c4 | (c5 << 8) | (c6 << 16) | (c7 << 24);
  flightStage (kStageEntry);
  (void)flightWrite (event, dir, lba, blocks);
}

static void flightExit (uint32_t done)
{
  gFlightPrevDone = done;
  gFlightPrevMs = flightCounterMs (gFlightOpStart);
  gFlightPrevDir = gFlightCurDir;
  gFlightPrevBlocks = gFlightCurBlocks;
  gFlightOps++;
}

// Round214: what the watchdog's card bring-up measured. One of these goes into the beat's report
// line as a single record: a recovery that stopped before the card answered CMD1 and one that
// stopped after the 50 MHz verification read refused are different next steps, and the stage word
// alone cannot tell them apart.
struct CardReinit {
  bool clockStopped;
  bool resetDone;
  bool clockStable;
  bool sent0;
  bool sent1;
  bool ready;
  bool cmd2;
  bool cmd3;
  bool cmd7;
  bool hs;
  bool bw;
  bool ok;
  uint32_t tries;
  uint16_t clockAfter;
  uint8_t powerAfter;
};

static CardReinit reinitializeCard (void);

// Round213: the watchdog's whole action, taken from the CPU0 beat when the holder's stage has not
// moved for kGuardStuckMs. It cannot wait for the holder -- the holder is the thing that stopped --
// so it stops the transfer the way the driver's own failure paths do: a command and data reset,
// then CMD12 to put the card back in TRAN, then the status the reset raised is cleared.
//
// Round214: r213's frame says resetting the host around the card is not enough. The last line on the
// card is at 418 s, no guard reported a trip, and the panic sink never wrote, so the card itself is
// what stopped answering. This now powers it down and brings it back up: CMD0/CMD1 at 400 kHz, the
// unchecked CMD2 round94 proved this host accepts, CMD3/CMD7 at kProbeRca, then fastMode's EXT_CSD
// switches and the LBA 1 read that proves the 50 MHz link is the one the requests above it expect.
// The ticket is taken before anything is touched and held through the bring-up: two recoveries at
// once would be one too many, and a holder that returns finds its word gone at its next chunk.
static void recoverStuckRequest (uint32_t stage, uint32_t owner)
{
  uint64_t frequency = counterFrequency ();
  if (frequency < 1000000ULL || frequency > 1000000000ULL) {
    frequency = 19200000ULL;
  }
  const uint64_t since = __atomic_load_n (&gFlightStageSince, __ATOMIC_ACQUIRE);
  const uint64_t now = readCounter ();
  const uint64_t held = (since && now > since) ? (now - since) * 1000ULL / frequency : 0;
  __atomic_add_fetch (&gFlightStuck, 1u, __ATOMIC_RELAXED);
  __atomic_store_n (&gFlightStuckStage, stage, __ATOMIC_RELAXED);
  sdhciSay ("CloverSdhci r213: guard stuck stage=%u dir=%u lba=%u blocks=%u pid=%u held=%llu ms\n",
            stage,
            __atomic_load_n (&gFlightStageDir, __ATOMIC_RELAXED),
            __atomic_load_n (&gFlightStageLba, __ATOMIC_RELAXED),
            __atomic_load_n (&gFlightStageBlocks, __ATOMIC_RELAXED),
            __atomic_load_n (&gFlightStagePid, __ATOMIC_RELAXED),
            static_cast<unsigned long long> (held));
  // Round214: the ticket first. If the holder released or somebody else took the host between the
  // beat's read and here, there is nothing stuck to recover -- and resetting a request that is
  // running is the one thing this function must never do.
  uint32_t ticket = __atomic_add_fetch (&gHostTickets, 1u, __ATOMIC_RELAXED);
  if (!ticket) {
    ticket = __atomic_add_fetch (&gHostTickets, 1u, __ATOMIC_RELAXED);
  }
  uint32_t expected = owner;
  if (!__atomic_compare_exchange_n (&gHostOwner, &expected, ticket, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    return;
  }
  __atomic_add_fetch (&gFlightSteals, 1u, __ATOMIC_RELAXED);
  // Round215: the ladder itself is shared with the request-context recovery; this path only adds
  // the steal above, because the holder it is taking the card from is the thing that stopped.
  (void) cardRecoverHeld ("stuck", stage);
  flightStage (kStageIdle);
  dropHostTicket (ticket);
}

// Called from ArmCpu's timer on CPU0. Taking the host is the point: a beat must never wait for a
// request, and the host being held is a fact the caller counts rather than a reason to block the
// interrupt. Round213 adds the other half -- if the host has been held and the holder's stage has
// not moved for kGuardStuckMs, the beat is the only context left that can end that request, so it
// resets the controller and takes the ticket back. `now - since` is compared in counter ticks, not
// in milliseconds, so the check does not depend on frequency rounding.
extern "C" void clover_emmc_flight_beat (void)
{
  if (!__atomic_load_n (&gFlightArmed, __ATOMIC_ACQUIRE) || !gVerdictFirst || !gHost ||
      !__atomic_load_n (&gHostArmed, __ATOMIC_ACQUIRE)) {
    __atomic_add_fetch (&gBeatStarved[kBeatBlind], 1u, __ATOMIC_RELAXED);
    return;
  }
  if (__atomic_load_n (&gPanicActive, __ATOMIC_ACQUIRE)) {
    __atomic_add_fetch (&gBeatStarved[kBeatPanic], 1u, __ATOMIC_RELAXED);
    return;
  }
  uint64_t frequency = counterFrequency ();
  if (frequency < 1000000ULL || frequency > 1000000000ULL) {
    frequency = 19200000ULL;
  }
  const uint64_t now = readCounter ();
  if (gFlightLastBeat && now - gFlightLastBeat < frequency * 4ULL) {
    __atomic_add_fetch (&gBeatStarved[kBeatYoung], 1u, __ATOMIC_RELAXED);
    return;
  }
  gFlightLastBeat = now;
  const uint32_t owner = __atomic_load_n (&gHostOwner, __ATOMIC_ACQUIRE);
  if (owner) {
    const uint32_t stage = __atomic_load_n (&gFlightStage, __ATOMIC_ACQUIRE);
    const uint64_t since = __atomic_load_n (&gFlightStageSince, __ATOMIC_ACQUIRE);
    const uint32_t stageAfter = __atomic_load_n (&gFlightStage, __ATOMIC_ACQUIRE);
    if (stage == stageAfter && since && now > since &&
        now - since > frequency * kGuardStuckMs / 1000ULL) {
      recoverStuckRequest (stage, owner);
    }
    __atomic_add_fetch (&gFlightBeatMisses, 1u, __ATOMIC_RELAXED);
    __atomic_add_fetch (&gBeatStarved[kBeatHeld], 1u, __ATOMIC_RELAXED);
    return;
  }
  uint32_t token = 0;
  if (!tryTakeHost (token)) {
    __atomic_add_fetch (&gFlightBeatMisses, 1u, __ATOMIC_RELAXED);
    __atomic_add_fetch (&gBeatStarved[kBeatHeld], 1u, __ATOMIC_RELAXED);
    return;
  }
  // Round305. Twelve words after the beat's own header, and all three of them are the same kind of
  // answer: what the exits that do not write would have said.
  //   [0] is the kernel's own second, taken in the same instruction stream as the record's `t=` at word
  //       10, so the difference between the two clocks is measured instead of inferred (the v0.7.14
  //       correction withdrew four such inferences, all of them circular).
  //   [1..4] are the four starvation exits, counted since the ring was armed and never cleared. Boot 94
  //       proved the cost of not counting: every one of its five probe reports said why=tail, because
  //       the flush wired behind the beat never ran -- and the only evidence of that was an absence,
  //       which is not a reading. They are cumulative rather than read-and-clear because the beat is not
  //       the only reader of them any more (the block tail reports them too), and a counter that one
  //       consumer empties steals the other's answer; the beat is also the consumer that can be
  //       starved, so a number it clears would be exactly as silent as the record it never wrote.
  //   [5..11] are the last refused block write, with the register and the word count the caller would
  //       have thrown away, and the refusals this boot has counted in all. 0 in [5] means none.
  uint32_t extra[13];
  extra[0] = kernelSeconds ();
  for (uint32_t i = 0; i < 4; i++) {
    extra[1 + i] = __atomic_load_n (&gBeatStarved[i], __ATOMIC_RELAXED);
  }
  extra[5] = gPioFailCode;
  extra[6] = gPioFailStatus;
  extra[7] = gPioFailWords;
  extra[8] = gPioFailMs;
  extra[9] = gPioFailAttempt;
  extra[10] = gPioFailLba;
  extra[11] = __atomic_load_n (&gPioFails, __ATOMIC_RELAXED);
  // Round293's stamp has to travel inside the extras now: flightWrite puts gBootId at word 35 only for
  // a record that carries none, and word 35 is this record's kernel second.
  extra[12] = gBootId;
  (void)flightWrite (kFlightEventBeat, 2, 0, 0, extra, 13);
  // Round304: the beat is the only writer left on a boot that never reaches the panic sink. Boot 93
  // stopped at ~555 s with the kernel's census rows still sitting in this driver's BSS and no drain to
  // read them: the beat runs every four seconds whether or not a block request ever finishes, and it is
  // already here holding the host, so it is where those rows get their second way out.
  (void)probeFlush ("beat", kProbeFlushCap);
  dropHostTicket (token);
}

// Round236: the deadline fact, written where the panic screen cannot lose it. Boot 24 reached its
// 1800 s deadline with no kind-20 record, no drain enter line and an empty BCB, while the driver's
// own ring carried six card recoveries in the same stretch -- so the sink's silence was the card's,
// and a reading that only exists behind the sink cannot say whether the deadline fired at all.
// This is called from ArmCpu's timer interrupt immediately before panic(): it takes the host the way
// a beat does (one try, plus the stuck-holder recovery when the stage has stopped moving) and writes
// one record after the panic has already been decided. It does not wait for the card afterwards and
// it does not depend on the flight region having been armed by bring-up -- only on the window base
// and the mapped host, which are the two things that make a write possible at all.
extern "C" void clover_emmc_flight_deadline (unsigned long long seconds, unsigned long long ticks,
                                             unsigned long long cnt0, unsigned long long cnt1,
                                             unsigned long long freq)
{
  if (__atomic_exchange_n (&gFlightDeadlineDone, 1u, __ATOMIC_ACQ_REL)) {
    return;
  }
  if (!gVerdictFirst || !gHost) {
    return;
  }
  const uint32_t owner = __atomic_load_n (&gHostOwner, __ATOMIC_ACQUIRE);
  if (owner) {
    const uint32_t stage = __atomic_load_n (&gFlightStage, __ATOMIC_ACQUIRE);
    const uint64_t since = __atomic_load_n (&gFlightStageSince, __ATOMIC_ACQUIRE);
    uint64_t frequency = counterFrequency ();
    if (frequency < 1000000ULL || frequency > 1000000000ULL) {
      frequency = 19200000ULL;
    }
    const uint64_t now = readCounter ();
    if (since && now > since && now - since > frequency * kGuardStuckMs / 1000ULL) {
      recoverStuckRequest (stage, owner);
    }
  }
  uint32_t token = 0;
  (void)tryTakeHost (token);
  uint32_t extra[11];
  extra[0] = static_cast<uint32_t> (seconds);
  extra[1] = static_cast<uint32_t> (seconds >> 32);
  extra[2] = static_cast<uint32_t> (ticks);
  extra[3] = static_cast<uint32_t> (ticks >> 32);
  extra[4] = static_cast<uint32_t> (cnt0);
  extra[5] = static_cast<uint32_t> (cnt0 >> 32);
  extra[6] = static_cast<uint32_t> (cnt1);
  extra[7] = static_cast<uint32_t> (cnt1 >> 32);
  extra[8] = static_cast<uint32_t> (freq);
  extra[9] = static_cast<uint32_t> (freq >> 32);
  // Round293: the boot this record belongs to. A deadline record is the one reading a boot can
  // leave behind for the next one to mistake for its own, and boot 79 did exactly that.
  extra[10] = gBootId;
  (void)flightWrite (kFlightEventDeadline, 3, 0, 0, extra, 11);
  dropHostTicket (token);
}

// One ring's share of a pass: at most kRingFlushPerPass[which] records, oldest first, and a refused
// write stops this ring for this pass rather than retrying. Called only from paths that hold the
// block guard with an idle card, so a card that is really gone costs writes per request instead of a
// hang -- and the cost is bounded per ring, which is the point of having two of them: a kernel log
// that outruns the flush cannot delay the driver's own verdicts, and vice versa.
static void flushRing (uint32_t which)
{
  if (!gVerdictFirst || !gHost || !gRingPending[which] || gRingBusy[which]) {
    return;
  }
  if (which == kRingKernel ? !gTeeReady : !gVerdictReady) {
    return;
  }
  gRingBusy[which] = 1;
  for (uint32_t flushed = 0; flushed < kRingFlushPerPass[which] && gRingPending[which]; ) {
    const uint32_t slot = (gRingWrite[which] - gRingPending[which]) % kRingRamSlots;
    if (!ringWriteRecord (which, gRingText[which][slot], gRingLength[which][slot])) {
      break;
    }
    gRingPending[which]--;
    flushed++;
  }
  gRingBusy[which] = 0;
}

// The single entry point the three guarded tails call. Both rings drain here, driver first: on a
// card that is only occasionally idle, the readings that explain the card go before the readings
// that came out of it.
static void flushVerdictRing (void)
{
  flushRing (kRingDriver);
  flushRing (kRingKernel);
  // Round304: the same pass that empties the two RAM rings now empties the relay ring too. This is the
  // path that demonstrably runs on a boot like 93 -- its tee lifted seven kernel rows at 555.48 s -- so
  // a census row handed over at the moment it was printed no longer waits for a panic to be written.
  (void)probeFlush ("tail", kProbeFlushCap);
}

// ------------------------------------------------------------------ round175: the kernel log tee
static uint32_t teeStringLength (const char *s);
static bool teeHasPrefix (const char *line, uint32_t length, const char *prefix);

// Round229: the negative filters. Boot 17 counted the ring's residents: 141 copies of a bare
// "fl=0x14809021 vt=1 fs=apfs" tail (a lap or a writer that does not end its line leaves the tail
// of an r170 open line standing alone, and the tail carries "apfs", so the allowlist let every copy
// in), 97 "r170: exit" lines from bluetoothd and fairplayd.A2 respawning at t=17-19 s, and 14
// CoreAnalyticsHub nag lines. Together they ate half of a 512-record ring whose whole job is to
// cover the boot, and the reading this port needs -- what generation 2 did after 1482 s -- fell
// into the records the noise displaced. Each filter is keyed on what makes the noise repeatable,
// not on one day's magic value, and each drop is counted so the census can say what was filtered.
static uint32_t gTeeFlDropped = 0;      // bare fl=... fs=apfs tails
static uint32_t gTeeNagDropped = 0;     // nag lines past two per source
static uint32_t gTeeExitKept = 0;       // exit lines inside the per-comm budget
static uint32_t gTeeExitDropped = 0;    // exit lines past the per-comm budget

// Round230: boot 18 re-ran the census after the three filters above, and the ring's new largest
// resident was CloverBlock's own r190 barrier pair -- 241 of 504 records. Every sync is two lines
// carrying one number, so the ring again held a metronome instead of the boot.
static uint32_t gTeeSyncInKept = 0;     // sync-in lines inside the budget
static uint32_t gTeeSyncOutKept = 0;    // sync-out lines inside the budget
static uint32_t gTeeSyncDropped = 0;    // either direction past the budget
static const uint32_t kTeeSyncKeep = 8;

struct TeeExitComm {
  char name[32];
  uint32_t kept;
};
static const uint32_t kTeeExitComms = 16;      // distinct comm= values tracked
static const uint32_t kTeeExitPerComm = 4;     // exit lines kept per comm
static TeeExitComm gTeeExitTable[kTeeExitComms];
static uint32_t gTeeExitCommsUsed = 0;

// The definitions of teeStringLength and teeHasPrefix follow; these filters are placed above the
// allowlist matcher because teeConsumeLine calls them before it consults the allowlist.
static const char kTeeNagToken[] = "handleNagTimerExpiry";
static const uint32_t kTeeNagSources = 8;      // distinct producers tracked
static const uint32_t kTeeNagPerSource = 2;    // lines kept per producer
static uint32_t gTeeNagTable[kTeeNagSources];  // hash of the producer name, 0 = free row
static uint32_t gTeeNagKept[kTeeNagSources];

// A nag line names its producer before the callback ("CoreAnalyticsHub::handleNagTimerExpiry..."),
// and "per source" means per producer: the bytes before "::handleNag" are the key. The scan starts
// after a "[ <mono>]: " timestamp so different sources do not hash to their stamp's digits.
static uint32_t teeStampSkip (const char *line, uint32_t length)
{
  if (!length || line[0] != '[') {
    return 0;
  }
  uint32_t i = 0;
  while (i < length && line[i] != ']') {
    i++;
  }
  if (i >= length) {
    return 0;
  }
  i++;
  while (i < length && (line[i] == ':' || line[i] == ' ')) {
    i++;
  }
  return i;
}

// The value of comm=<name> in a line, as length bytes into `out`; zero if the line has no comm=
// or the table is full. Boot 17's names were short (bluetoothd, fairplayd.A2); 31 characters of a
// trailing name is the whole question "which process is looping", and a longer one is truncated,
// not dropped.
static bool teeExitCommKey (const char *line, uint32_t length, TeeExitComm **entry)
{
  const char *const token = "comm=";
  const uint32_t width = teeStringLength (token);
  uint32_t at = 0;
  while (at + width <= length) {
    if (teeHasPrefix (line + at, length - at, token)) {
      break;
    }
    at++;
  }
  if (at + width > length) {
    return false;
  }
  uint32_t start = at + width;
  uint32_t n = 0;
  while (start + n < length && n < sizeof ((*entry)->name) - 1 &&
         line[start + n] != ' ' && line[start + n] != '\r') {
    n++;
  }
  // Compare against the tracked names, and claim a free row for a new one.
  for (uint32_t i = 0; i < gTeeExitCommsUsed; i++) {
    TeeExitComm *cand = &gTeeExitTable[i];
    if (n == teeStringLength (cand->name)) {
      uint32_t j = 0;
      while (j < n && cand->name[j] == line[start + j]) {
        j++;
      }
      if (j == n) {
        *entry = cand;
        return true;
      }
    }
  }
  if (gTeeExitCommsUsed >= kTeeExitComms) {
    *entry = &gTeeExitTable[0];   // full table: reuse row 0's budget, counting its drops
    return true;
  }
  TeeExitComm *fresh = &gTeeExitTable[gTeeExitCommsUsed];
  for (uint32_t i = 0; i < n; i++) {
    fresh->name[i] = line[start + i];
  }
  fresh->name[n] = '\0';
  gTeeExitCommsUsed++;
  *entry = fresh;
  return true;
}

// The producer's own name, walked backwards from "::handleNagTimerExpiry" so a leading timestamp
// cannot enter the key: skip the ':' pair, collect at most 32 bytes until a space, '(' or ':', and
// FNV-1a the result. A line that carries the token without a name before it hashes nothing and is
// filtered as its source's surplus would be -- an anonymous nag is still a nag.
static bool teeNagSource (const char *line, uint32_t length, uint32_t *hash)
{
  const uint32_t width = teeStringLength (kTeeNagToken);
  uint32_t at = 0;
  bool found = false;
  while (at + width <= length) {
    if (teeHasPrefix (line + at, length - at, kTeeNagToken)) {
      found = true;
      break;
    }
    at++;
  }
  if (!found || at == 0) {
    return false;
  }
  uint32_t i = at;
  while (i > 0 && line[i - 1] == ':') {
    i--;
  }
  char name[32];
  uint32_t n = 0;
  while (i > 0 && n < sizeof (name) && line[i - 1] != ' ' && line[i - 1] != '(' &&
         line[i - 1] != ':' && line[i - 1] != '\t') {
    i--;
    name[n] = line[i];
    n++;
  }
  if (n == 0) {
    return false;
  }
  uint32_t h = 2166136261u;
  for (uint32_t j = 0; j < n; j++) {
    h = (h ^ (unsigned char) name[n - 1 - j]) * 16777619u;
  }
  if (h == 0) {
    h = 1;
  }
  *hash = h;
  return true;
}

static uint32_t teeStringLength (const char *s)
{
  uint32_t length = 0;
  while (s[length]) {
    length++;
  }
  return length;
}

static bool teeHasPrefix (const char *line, uint32_t length, const char *prefix)
{
  const uint32_t width = teeStringLength (prefix);
  if (width > length) {
    return false;
  }
  for (uint32_t i = 0; i < width; i++) {
    if (line[i] != prefix[i]) {
      return false;
    }
  }
  return true;
}

// Substring test, not prefix test, because the interesting lines are written by other people with
// their own leading text -- "[apfs_jhash:146] ...", "AMFI: ...", "load code signature error 4" behind a
// mount name. Cost is one pass per short line per allowlist entry; the lines are capped at
// kTeeLineBytes, so this is bounded without needing to be clever about it.
static bool teeTokensMatch (const char *line, uint32_t length,
                            const char *const *tokens, uint32_t count)
{
  for (uint32_t entry = 0; entry < count; entry++) {
    const uint32_t width = teeStringLength (tokens[entry]);
    if (!width || width > length) {
      continue;
    }
    for (uint32_t at = 0; at + width <= length; at++) {
      uint32_t i = 0;
      while (i < width && line[at + i] == tokens[entry][i]) {
        i++;
      }
      if (i == width) {
        return true;
      }
    }
  }
  return false;
}

static bool teeLineMatches (const char *line, uint32_t length)
{
  return teeTokensMatch (line, length, kTeeAllowlist, kTeeAllowlistCount);
}

// The tee's own voice: format and put in the driver's ring, nothing else. sdhciSay cannot be used
// here, because teeKernelLog runs at the tails of block requests -- inside the block guard -- and
// sdhciSay calls IOLog, which takes bsd_log_lock. Taking a kernel lock while holding the guard is a
// lock order this driver has no business inventing: a logger that holds bsd_log_lock and then issues
// disk I/O would wait for the guard we hold, and we would wait for the lock it holds. A tee state
// line is still a reading, so it still goes to the card; only the panel copy is given up. It goes to
// the driver ring rather than the kernel ring because the kernel ring is the answer to "what did the
// kernel say", and a line in it that the tee said about itself would make that question ambiguous.
static void teeSay (const char *fmt, ...)
{
  char line[kVerdictTextBytes + 1];
  va_list args;
  va_start (args, fmt);
  uint32_t length = verdictFormat (line, kVerdictTextBytes, fmt, args);
  va_end (args);
  while (length && line[length - 1] == '\n') {
    length--;
  }
  line[length] = '\0';
  ringPush (kRingDriver, line, length);
}

// Which chatty class, if any, owns this line. The tokens are substrings for the same reason the
// allowlist's are: the line may carry a timestamp or a prefix from whoever printed it.
static TeeProbeClass *teeProbeClass (const char *line, uint32_t length)
{
  for (uint32_t entry = 0; entry < kTeeProbeClassCount; entry++) {
    if (teeTokensMatch (line, length, &kTeeProbeClasses[entry].token, 1)) {
      return &kTeeProbeClasses[entry];
    }
  }
  return nullptr;
}

// One complete line out of the buffer. Leading blanks and the '\b' progress dots the boot writer
// emits are cut, an all-blank line is dropped, and anything this driver said is skipped: it went into
// the driver's own ring already, through sdhciSay or teeSay, in the same boot. That exclusion is what
// makes round176's kernel ring mean something -- every record in it is a line the kernel said.
static void teeConsumeLine (uint32_t &pushed)
{
  uint32_t length = gTeePartial;
  gTeePartial = 0;
  uint32_t start = 0;
  while (start < length && (gTeeLine[start] == ' ' || gTeeLine[start] == '\t' ||
                            gTeeLine[start] == '\b' || gTeeLine[start] == '\r')) {
    start++;
  }
  while (length > start && (gTeeLine[length - 1] == ' ' || gTeeLine[length - 1] == '\r')) {
    length--;
  }
  if (length == start) {
    return;
  }
  gTeeSeen++;
  if (teeHasPrefix (gTeeLine + start, length - start, "CloverSdhci ")) {
    return;
  }
  // Round229 filter 1: a line that *starts* with the fl=/vt=/fs= tail of an r170 open reading,
  // once its "[ <mono>]: " stamp is skipped, is that tail printed alone -- a lap, or a writer
  // whose line got split. The whole open reading exists elsewhere in the ring; the tail alone
  // carries no information beyond "apfs", which is exactly the token that let it in.
  {
    const uint32_t unstamped = teeStampSkip (gTeeLine + start, length - start);
    if (teeHasPrefix (gTeeLine + start + unstamped, length - start - unstamped, "fl=")) {
      gTeeFlDropped++;
      return;
    }
  }
  // Round229 filter 2: the analytics nag repeats on a timer for as long as anyone is alive to
  // service it. Two lines per producer proves the timer runs; the rest is a clock in the ring.
  {
    uint32_t hash = 0;
    if (teeNagSource (gTeeLine + start, length - start, &hash)) {
      uint32_t slot = 0;
      bool found = false;
      for (uint32_t i = 0; i < kTeeNagSources; i++) {
        if (gTeeNagTable[i] == hash) {
          slot = i;
          found = true;
          break;
        }
      }
      if (!found) {
        for (slot = 0; slot < kTeeNagSources; slot++) {
          if (gTeeNagTable[slot] == 0) {
            gTeeNagTable[slot] = hash;
            break;
          }
        }
        if (slot >= kTeeNagSources) {
          slot = 0;   // table full: fold a new source into slot 0's budget
          gTeeNagTable[0] = hash;
          gTeeNagKept[0] = 0;
        }
      }
      if (gTeeNagKept[slot] < kTeeNagPerSource) {
        gTeeNagKept[slot]++;
      } else {
        gTeeNagDropped++;
        return;
      }
    }
  }
  // Round229 filter 3: r170's exit lines. Round195 left them uncapped because "a process dying is
  // what a stop is read from" -- true of the first four, false of the ninety-seventh bluetoothd.
  // The first kTeeExitPerComm per comm survive with their pid; the surplus is counted and the
  // count reaches the ring as one tail line from the census block below.
  if (teeHasPrefix (gTeeLine + start + teeStampSkip (gTeeLine + start, length - start),
                    length - start - teeStampSkip (gTeeLine + start, length - start),
                    "clover r170: exit ")) {
    TeeExitComm *comm = nullptr;
    if (teeExitCommKey (gTeeLine + start, length - start, &comm)) {
      if (comm->kept >= kTeeExitPerComm) {
        gTeeExitDropped++;
        return;
      }
      comm->kept++;
      gTeeExitKept++;
    }
  }
  // Round230 filter 4: the r190 barrier. Each sync prints an in-line and an out-line whose only
  // content is the queue depth it waited on; the first kTeeSyncKeep of each direction say the
  // barrier runs and how deep it went, the rest of a boot's hundreds are a clock.
  {
    const uint32_t unstamped = teeStampSkip (gTeeLine + start, length - start);
    const uint32_t rest = length - start - unstamped;
    const char *const body = gTeeLine + start + unstamped;
    uint32_t *kept = nullptr;
    if (teeHasPrefix (body, rest, "CloverBlock r190: sync-in ")) {
      kept = &gTeeSyncInKept;
    } else if (teeHasPrefix (body, rest, "CloverBlock r190: sync-out ")) {
      kept = &gTeeSyncOutKept;
    }
    if (kept != nullptr) {
      if (*kept >= kTeeSyncKeep) {
        gTeeSyncDropped++;
        return;
      }
      (*kept)++;
    }
  }
  if (!teeLineMatches (gTeeLine + start, length - start)) {
    return;
  }
  TeeProbeClass *probe = teeProbeClass (gTeeLine + start, length - start);
  if (probe != nullptr) {
    if (probe->kept >= probe->keep_max) {
      if (probe->dropped++ == 0 && probe->reported++ == 0) {
        teeSay ("CloverSdhci r195: %s probe lines capped at %u, later ones stay out of the "
                "kernel ring\n", probe->name, probe->keep_max);
      }
      return;
    }
    probe->kept++;
  }
  // Round234 reserves one push per pass for the set row. Boot 22 lost members i=8..10 because
  // eight per-fire rows came first and the sixteenth push was the pass's last; the set row is the
  // one reading whose absence cannot be told from a set that did not change, so the cap does not
  // apply to it. Its class budget still bounds it.
  // Round249 gives the same exemption to the census. Boot 36 measured the loss exactly: the pass
  // that carried it had already pushed five bb rows at t=1514, the census then took eleven, and the
  // sixteenth push closed the pass -- so hbt len=, the three byname rows, the service walk's total
  // and census done were throttled, and the tee's cursor moved past them for good. A truncated
  // census is not an empty plane: every row it answers with is a question whose absence would
  // otherwise read as a verdict. It is printed once per boot behind a static guard, its class
  // budget of 64 still bounds it, and the burst is under forty rows.
  // Round250 rides the same burst: the recorder's dump is called from the last line of the census,
  // so it is one-shot for exactly the same reason and would be truncated by the same cap. The
  // recorder also prints a note the moment a question arrives, outside any burst -- those are the
  // first ring slots only, and they take the ordinary per-pass budget like any other probe.
  // Round251 renames that class rather than adding one: boot 38 showed the recorder losing its own
  // decisive rows to an 8-slot ring, and the fixed instrument prints from the same place.
  // Round275 rides the same burst as its own host: the three keystore rows are printed inside
  // clover_iodt_census's service walk, so they arrive in the pass that is already carrying the r243 census and
  // would be counted against a cap that pass has usually spent by then. Exempting them is not a favour to the
  // new round -- a truncated keystore row is the same false "none" that boot 36 nearly made of the plane, and
  // the class budget of 64 still bounds it.
  // Round276 is exempt for exactly the same reason and gets checked the same way: it publishes the service the
  // r275 rows ask about, so its own rows have to be readable or a capped round looks identical to an absent one.
  // Round281 rides the same burst and gets the same exemption for a sharper reason: its rows are printed from
  // an IOKit timer on the work loop, far from the passes that carry the census, so they arrive beside whatever
  // else shares that pass -- and a schedule whose row is dropped by the per-pass cap is indistinguishable from
  // a boot that did not reach the next fire. The class budget of 128 still bounds it.
  // Round286 rides the same burst for the same reason: the GPU schedule fires beside the census and
  // the keystore rows, and a capped gpucc row would read as a board whose GDSC write never ran.
  // Round289 is exempt for a sharper reason than any of these: its rows are the last lines in the log
  // buffer, because they are printed by the abort handler on the way to panic, and the panic drain is
  // exactly the pass that has already spent its sixteen pushes on everything older. Boot 74 and boot
  // 75 measured the shape -- the only thing that came back of either fault was the 480-byte kind-20
  // slot, which runs out of room in the middle of the register list. A trace the cap eats is a fault
  // with no name again, so the rows are checked the way r249's census is: absent must never look like
  // empty. The class budget of 32 still bounds it.
  // Round290 rides with round286 for the reason already written above: its branch row is the only
  // place the halt bit is read, and a capped one would leave the boot saying "gfx3d was asked for"
  // with no way to say whether it ran.
  // Round295 rides with the censuses for the reason r243 gave: the chain is worth reading only to its
  // last row, and the verdict that ends it (end=depth_limit, end=cycle, end=self_reference) is printed
  // there. A pass cap that ate the fifth hop would leave the boot looking as though the chain stopped
  // after four.
  const bool reserved = probe != nullptr &&
      (teeHasPrefix (probe->name, teeStringLength (probe->name), "r234-set") ||
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r243") ||
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r252") ||
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r275") ||
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r276") ||
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r281") ||
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r283") ||
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r286") ||
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r289") ||
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r290") ||
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r291") ||
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r294") ||
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r295") ||
       // Round296 rewrote the chain row in place, so it inherits r295's reason: a chain the cap cuts
       // short of its last hop cannot be told from a chain whose last hop reported depth_limit.
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r296") ||
       // Round298 rewrites round297's identity row in place, so it inherits that row's reason: the row
       // that answers the question is the leaf's, and a boot whose hop=3 identity row was throttled
       // would report nothing where link= and tlink= belong -- a silence that reads like "no link".
       // r298 makes that worse and better at once: worse because boot 86 showed qmc=/qsr= are printed
       // by the same translation that produced rvobj, so a lost row leaves a zero nobody can attribute;
       // better because tlink= needs no translate on this hop's side, so the row is worth exempting.
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r298") ||
       // Round299 prints from inside the same census burst, one row per fire, and it is the row that carries
       // the whole mechanism: req=/st=/fl=/rc= are what separates "the gate never opened" from "the gate
       // opened and the firmware said no" from "the core came up and its redistributor did not". With the cap
       // in front of it, a boot whose secondaries were asked for would return nothing at all and read like a
       // boot that never reached the next fire -- the same false-empty r249's census was exempted for, and
       // the class budget of 64 still bounds it.
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r299") ||
       // Round300 is the row that answers boot 88's question, and it only answers it if it is present: a fire
       // whose done= row was throttled away reads exactly like a fire the board never finished, which is the
       // false-empty this exemption exists to stop. The count it carries (th=, chain=) is the number of rows
       // this fire really pushed, so losing the row that holds the number would put us back where the absent
       // r262/r296/r298 thread rows left us -- an absence with nothing beside it to attribute.
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r300") ||
       // Round303 prints from inside the same chain walk as r296/r298 -- one row per hop whose owner resolves --
       // and it is the whole reading of this round: the receiving task's own entry for the name the sender wrote
       // to (rvbits=, rwait=). A capped r303 row leaves exactly the hole the round exists to fill: a hop that
       // translated a name on the r298 row but never reported what that name is in the receiver's space reads as
       // "the receiver has nothing", which is the false-empty r298 was exempted for, one layer onto the peer. The
       // class budget of 128 still bounds it, the same headroom r298 holds for the same per-hop producer.
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r303") ||
       // Round306's step row is the negative control for the relay: it says the walk reached the statement
       // between the counters and the fire row, and it is printed by the console path only, on purpose. If
       // the per-pass cap ate it, a boot would report neither copy of "got here" and read like a fire that
       // never started -- the false-empty this exemption exists to stop, one layer under the relay.
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r306") ||
       // Round308's rows are the relay's own second leg: the beat row is sealed on the driver ring and
       // printed by the console path in the same statement, and the pair is the reading (a relay row with
       // no printed copy says the console lost it; neither says the sampler never ran). A per-pass cap
       // that ate the printed half would turn "the tee is blind" into "the relay is dead", which is the
       // one confusion this round was written to keep out of the next control round.
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r308") ||
       // Round311 rides with the other GPU rows for the reason r286 gave: the schedule prints beside the
       // censuses, and its row is the only place the L1 MEM_ACC select is read. The sharper half is that
       // this row has three shapes and the difference between them is the reading -- a per-pass cap that
       // ate the answered shape and left the refused one would report a page that answered as a page that
       // was never mapped. The class budget of 32 still bounds it.
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r311") ||
       // Round317's verdict row is the interpretation of round294's thirteen words, and it prints from the
       // same fire. The exemption is r283's rule in its plainest form here: a shape the cap ate and a shape
       // the board never produced are two different claims about the same rail, and the difference between
       // "list-never-written" and "dead-page" is the one this line is still asking about.
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r317") ||
       // Round318's two rows are one segment's reading: the raw status words and the decoded fields,
       // printed from the same two reads. Exempting the class is r283's rule at its sharpest here --
       // round181's bus= column was the masked word, so a per-pass cap that kept the decoded row and
       // ate the raw one would restore exactly the blind spot this round exists to remove: a held SDA
       // line would once again be indistinguishable from a clean bus with a NACK. The class budget of
       // 32 still bounds it.
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r318") ||
       // Round319's beat row rides beside the census like round281's schedule: it fires on the HID
       // probe's 120 s beat, far from the passes that carry the census, and a row dropped by the
       // per-pass cap is indistinguishable from a probe that never ran. The class budget of 16 still
       // bounds it.
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r319-last") ||
       // Round327 keeps round326's console copy, and it prints from inside the same burst as the
       // r229/r231/r234 rows it summarizes, so the r249 reason applies directly: a pass that had already
       // spent its sixteen pushes would drop the one row that names the hops and leave the burst reading
       // as a sample that computed nothing. The relay copy does not ride this cap -- that is why the row
       // exists -- but the two copies are each other's control, and a console copy the cap ate silently
       // breaks that. The class budget of 32 still bounds it.
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r327-bbnow") ||
       // Round328's receive row prints from the same millisecond band as r327's send row, and the pair is
       // the reading: one null on the send column means nothing until the receive column says whether that
       // hop is a door at all. A pass that had already spent its sixteen pushes would keep one half of the
       // pair and drop the other, which is exactly the shape r249 refuses -- a row the cap ate is
       // indistinguishable from a row the kernel never wrote. The class budget of 32 still bounds it.
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r328-bbrcv") ||
       // Round331 prints from the same burst as r327/r328 and it is the row that turns "nobody sent" into
       // a named holder or an explicit zero-of-400. Its own per-pass exemption is r249's reason in its
       // strongest form here: the row's whole content is a count, and a pass that had spent its sixteen
       // pushes would leave `hits=0` indistinguishable from a pass that never got to count. The header and
       // the two name classes are exempted together on one token so half a list cannot survive the cap.
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r331") ||
       // Round329's state row rides no burst at all: it fires on the display's own 30 s work-loop beat, so
       // it lands beside whatever else happens to share that pass. This is r281's and r319's case exactly --
       // for a row whose whole job is to say "the client is quiet now", a pass cap that ate the row is
       // indistinguishable from a beat that never fired, and the two answer different questions. The class
       // budgets of 32 and 8 still bound it.
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r329-dispstate") ||
       // Round333's swap-wait rows ride the client's own calls, not a burst: a boot that stalls in this wait
       // prints its last row at the moment it stalls, and that row is the one the whole round is built to
       // read. Dropping it to a pass that had already spent its sixteen pushes would leave the reader with
       // the beat row alone -- which is r249's shape again, half of a pair surviving the cap and looking like
       // a reading. The token also covers the cut branch, which shares the cause. The class budgets of 64 and
       // 8 still bound both.
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r333-bbswap") ||
       // Round313's power report is one burst printed while the PMGR tail attaches, and it is the first
       // time this driver has ever run on a board. Exempting both of its classes is the r283 reason in
       // its plainest form: half a report and no report are different readings of the same bus, and a
       // per-pass cap that left the version block but ate the APID listing would make "the arbiter
       // answered" indistinguishable from "the arbiter was not reached". The two class budgets of 48
       // still bound the burst.
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r182-power") ||
       teeHasPrefix (probe->name, teeStringLength (probe->name), "r313-power"));
  if (pushed >= kTeePushPerPass && !reserved) {
    // The per-pass push cap is sixteen block writes on a card that is also trying to serve the
    // request this tail belongs to; a chatty boot gets its surplus counted rather than queued forever.
    gTeeThrottled++;
    return;
  }
  kernelRingPush (gTeeLine + start, length - start);
  gTeeKept++;
  pushed++;
}

// Drain the kernel log buffer into round176's kernel ring. Called from the same three guarded tails
// as flushVerdictRing, immediately before it, so a line the tee accepts in this pass can reach the
// card in the same pass. Nothing here is safe to call from an interrupt-stopped context or without
// the block guard; it only touches memory and the ring, and the writes happen in the flush.
static void teeKernelLog (void)
{
  if (!gTeeReady) {
    return;
  }
  // Re-read msgbufp every pass: XNU can reallocate the log (bsd/kern/subr_log.c:622 reassigns
  // msg_bufc and rewrites both pointers), so nothing about this buffer may be cached across calls.
  struct msgbuf *buffer = msgbufp;
  const int magic = buffer ? buffer->msg_magic : 0;
  const int size = buffer ? buffer->msg_size : 0;
  const int bufx = buffer ? buffer->msg_bufx : -1;
  char *raw = buffer ? buffer->msg_bufc : nullptr;
  const uint64_t base = static_cast<uint64_t> (reinterpret_cast<uintptr_t> (raw));

  uint32_t reason = 0;
  if (!buffer) {
    reason = 1;              // no log buffer this boot
  } else if (magic != MSG_MAGIC) {
    reason = 2;              // not a msgbuf
  } else if (size <= 0 || size > MAX_MSG_BSIZE) {
    reason = 3;              // implausible geometry
  } else if (bufx < 0 || bufx >= size) {
    reason = 4;              // write pointer outside the buffer
  } else if (base < kTeeKernelVaBase) {
    reason = 5;              // not a kernel address; reading it would fault
  }
  if (reason) {
    gTeeRefused = reason;
    if (!gTeeRefusedShown) {
      gTeeRefusedShown = 1;
      teeSay ("CloverSdhci r175: tee refuses reason=%u magic=0x%x size=%d bufx=%d bufc=%p\n",
                reason, magic, size, bufx, reinterpret_cast<void *> (base));
    }
    return;
  }
  gTeeRefused = 0;

  const uint32_t sizeu = static_cast<uint32_t> (size);
  uint32_t index = gTeeIndex;
  if (gTeeBuffer && gTeeBuffer != base) {
    // The buffer moved under the cursor. Restart at zero: two copies of a line is a readable card,
    // a cursor into freed memory is not.
    teeSay ("CloverSdhci r175: tee buffer moved old=%p new=%p idx=%u\n",
              reinterpret_cast<void *> (gTeeBuffer), reinterpret_cast<void *> (base), index);
    index = 0;
    gTeePartial = 0;
  }
  gTeeBuffer = base;
  if (index >= sizeu) {
    index = static_cast<uint32_t> (bufx);
  }
  const uint32_t stop = static_cast<uint32_t> (bufx);
  if (stop < index) {
    // The writer wrapped past our cursor since the last pass, so the oldest unread bytes are the ones
    // it has not reached yet. Counting it is the point: a frame with a high lap number is a boot whose
    // log outran the card, and its gaps are explained.
    gTeeLap++;
  }

  volatile char *data = reinterpret_cast<volatile char *> (base);
  uint32_t scanned = 0;
  uint32_t pushed = 0;
  while (index != stop && scanned < kTeeScanPerPass) {
    const char c = data[index];
    index++;
    if (index >= sizeu) {
      index = 0;
    }
    scanned++;
    if (c == '\n') {
      teeConsumeLine (pushed);
    } else if (gTeePartial < kTeeLineBytes) {
      gTeeLine[gTeePartial] = c;
      gTeePartial++;
    } else {
      // A line longer than a record: hand over what fits and start the next one. Splitting it would
      // put half a message on the card twice, which reads like two different events.
      teeConsumeLine (pushed);
      gTeePartial = 0;
    }
  }
  gTeeIndex = index;

  if (gTeeSeen >= gTeeNextReport) {
    gTeeNextReport = gTeeSeen + 256;
    if (gTeeCensus < kTeeCensusMax) {
      gTeeCensus++;
      teeSay ("CloverSdhci r175: tee seen=%u kept=%u throttled=%u lap=%u idx=%u size=%u\n",
                gTeeSeen, gTeeKept, gTeeThrottled, gTeeLap, gTeeIndex, sizeu);
    }
  }
}

// Round209: the tee is itself a block I/O tail, so a boot that stops completing block I/O takes the
// tee with it -- r208's kernel ring simply ends at 167.198 s and the deadline's eight passes find
// nothing new. The panic sink is the one writer left, so it reads the log buffer directly: walk
// backwards from msg_bufx over at most kPanicTailScanBytes, keep the newest complete lines, and take
// the ones the tee cursor never reached. The newest few are kept whatever their class, because at a
// deadline the question is what the last line was, not which probe printed it.
//
// `unseen` in the status line is the measurement that separates the two readings a silent frame
// cannot: zero means the kernel's own buffer also stopped growing (the kernel is not merely quiet on
// the card), and a large number with a tail that keeps going means the kernel was still logging and
// only the card path stopped.
static const uint32_t kPanicTailLines = 48;      // newest complete lines pushed to the kernel ring
static const uint32_t kPanicTailAlways = 3;      // newest lines kept even when their class is noise

// The panic sink's voice: formats once and pushes the line into the driver ring. Round209 pushed it
// into the kernel ring because the line is about the kernel's buffer, but the kernel ring's only
// producer is the tee -- a line pushed from here made the host read a driver row as a kernel one and
// report a phantom echo. The driver ring has no such contract: this module owns both halves of it.
static void panicTailSay (const char *fmt, ...)
{
  char line[kVerdictTextBytes + 1];
  va_list args;
  va_start (args, fmt);
  const uint32_t length = verdictFormat (line, kVerdictTextBytes, fmt, args);
  va_end (args);
  ringPush (kRingDriver, line, length);
}

static bool panicTailNoise (const char *line, uint32_t length)
{
  return teeHasPrefix (line, length, "clover r187") ||
         teeHasPrefix (line, length, "clover r189");
}

static void panicTailRead (void)
{
  struct msgbuf *buffer = msgbufp;
  const int magic = buffer ? buffer->msg_magic : 0;
  const int size = buffer ? buffer->msg_size : 0;
  const int bufx = buffer ? buffer->msg_bufx : -1;
  char *raw = buffer ? buffer->msg_bufc : nullptr;
  const uint64_t base = static_cast<uint64_t> (reinterpret_cast<uintptr_t> (raw));

  uint32_t reason = 0;
  if (!buffer) {
    reason = 1;
  } else if (magic != MSG_MAGIC) {
    reason = 2;
  } else if (size <= 0 || size > MAX_MSG_BSIZE) {
    reason = 3;
  } else if (bufx < 0 || bufx >= size) {
    reason = 4;
  } else if (base < kTeeKernelVaBase) {
    reason = 5;
  }
  if (reason) {
    panicTailSay ("CloverSdhci r212: panic tail refused reason=%u magic=0x%x size=%d bufx=%d "
                  "tee=%u boot=%u\n",
                  reason, magic, size, bufx,
                  static_cast<unsigned int> (gTeeReady),
                  static_cast<unsigned int> (gBootId));
    return;
  }

  const uint32_t sizeu = static_cast<uint32_t> (size);
  const uint32_t stop = static_cast<uint32_t> (bufx);
  volatile char *data = reinterpret_cast<volatile char *> (base);
  uint32_t unseen = 0;
  if (gTeeBuffer == base && gTeeIndex < sizeu) {
    unseen = (stop >= gTeeIndex) ? stop - gTeeIndex : sizeu - gTeeIndex + stop;
  }

  uint32_t start[kPanicTailLines];
  uint32_t length[kPanicTailLines];
  uint32_t taken = 0;
  char text[kVerdictTextBytes + 1];
  uint32_t pos = stop;
  uint32_t segEnd = stop;
  bool newest = true;                 // bytes past the last newline: start is not knowable
  uint32_t stepped = 0;
  while (stepped < sizeu && taken < kPanicTailLines) {
    if (pos == 0) {
      pos = sizeu;
    }
    pos--;
    stepped++;
    if (data[pos] != '\n') {
      continue;
    }
    if (newest) {
      newest = false;
    } else {
      const uint32_t lineStart = (pos + 1) % sizeu;
      const uint32_t lineLength = (segEnd >= lineStart) ? segEnd - lineStart
                                                        : sizeu - lineStart + segEnd;
      if (lineLength) {
        // A line longer than a ring record is kept from its start, where the timestamp and the
        // subsystem are, and cut to fit: dropping it whole would lose the one line that mattered.
        const uint32_t keep = lineLength < kVerdictTextBytes ? lineLength : kVerdictTextBytes - 1;
        for (uint32_t i = 0; i < keep; i++) {
          text[i] = data[(lineStart + i) % sizeu];
        }
        const uint32_t behind = (stop >= segEnd) ? stop - segEnd : sizeu - segEnd + stop;
        const bool fresh = behind <= unseen;
        if (taken < kPanicTailAlways || (fresh && !panicTailNoise (text, keep))) {
          start[taken] = lineStart;
          length[taken] = keep;
          taken++;
        }
      }
    }
    segEnd = pos;
  }

  for (uint32_t i = taken; i > 0; i--) {
    const uint32_t lineStart = start[i - 1];
    const uint32_t lineLength = length[i - 1];
    for (uint32_t j = 0; j < lineLength; j++) {
      text[j] = data[(lineStart + j) % sizeu];
    }
    kernelRingPush (text, lineLength);
  }

  panicTailSay ("CloverSdhci r212: panic tail lines=%u unseen=%u bufx=%d tee=%u boot=%u "
                "cursor=%u size=%u lap=%u refused=%u pend=%u drop=%u guard=%u last=%ums "
                "cache=%u append_rc=%u\n",
                taken, unseen, bufx,
                static_cast<unsigned int> (gTeeReady),
                static_cast<unsigned int> (gBootId),
                static_cast<unsigned int> (gTeeIndex), sizeu,
                static_cast<unsigned int> (gTeeLap), static_cast<unsigned int> (gTeeRefused),
                static_cast<unsigned int> (gRingPending[kRingKernel]),
                static_cast<unsigned int> (gRingDropped[kRingKernel]),
                static_cast<unsigned int> (__atomic_load_n (&gGuardWaits, __ATOMIC_RELAXED)),
                static_cast<unsigned int> (__atomic_load_n (&gGuardLastWaitMs, __ATOMIC_RELAXED)),
                static_cast<unsigned int> (__atomic_load_n (&gCacheOn, __ATOMIC_RELAXED)),
                static_cast<unsigned int> (gPanicAppendRc));
}

// Round184: the panic screen's sink is the last writer this boot gets, and it is called at a moment
// chosen by the failure rather than by the card. Everything the tee lifted since the last block
// request -- which, on a boot that hung, is the entire part worth reading -- is still in the RAM
// rings, and the board resets 20 s after that screen finishes drawing. So the sink drains the rings
// itself, with the same unguarded single-block primitive its own two writes already use and no lock:
// at panic time the block guard may be held by a thread that is no longer running, and waiting on it
// would trade a reading for a hang. That is the bargain round147 already struck for the panic text;
// this widens it from two blocks to as many records as the rings hold, and it is why this round puts
// `clover-unattended=` back into the boot -- a deadline is only useful if what happened before it
// survives the reset the deadline causes.
//
// Three bounds, because the reset clock is running. The tee gets kPanicTeePasses passes to catch up
// on the log buffer; each ring keeps its newest kPanicDrainCap records, dropping the oldest first,
// because the oldest are the ones an ordinary pass already had its chance at; and a refused write
// stops the drain rather than retrying it. Worst case here is 2 * 48 records at two blocks each on a
// card that costs ~20 ms a block, about four seconds, inside a twenty second window.
static const uint32_t kPanicTeePasses = 8;      // 8 * kTeeScanPerPass = 32 KiB of log buffer
static const uint32_t kPanicDrainCap = 48;      // newest records kept, per ring

// Round212: one ring's records to the card, oldest first, keeping the newest kPanicDrainCap and
// stopping at the first write the card refuses. Returns what it wrote.
static uint32_t panicRingFlush (uint32_t which)
{
  uint32_t wrote = 0;
  while (gRingPending[which] > kPanicDrainCap) {
    // Oldest first is the read order (slot = gRingWrite - gRingPending), so shrinking the pending
    // count from the front is what drops the front.
    gRingPending[which]--;
    gPanicDrainDropped++;
  }
  gRingBusy[which] = 1;
  while (gRingPending[which]) {
    const uint32_t slot = (gRingWrite[which] - gRingPending[which]) % kRingRamSlots;
    if (!ringWriteRecord (which, gRingText[which][slot], gRingLength[which][slot])) {
      break;
    }
    gRingPending[which]--;
    wrote++;
  }
  gRingBusy[which] = 0;
  return wrote;
}

// Round212: the probe ring straight to the card as driver-ring records. It runs before the RAM
// rings because these are the lines a dead block path cannot otherwise cost: the kernel wrote them
// into this driver's BSS, so nothing about them depends on the guard, the msgbuf or the tee. A
// slot whose seal is not the number this loop expects was overwritten between the writer's text
// copy and its seal store, and that one record is skipped rather than written half-formed.
//
// Round304 moves that same bounded skip into a consumer with a cursor. The drain used to re-read the
// whole window every time it ran -- harmless while it was the only caller, wrong the moment a second
// one exists, because the same row would land on the card twice and the host would read one probe as
// two. Now every consumer -- tail, beat, drain -- walks the same cursor and only advances it past a row
// the card accepted.
static uint32_t probeFlush (const char *why, uint32_t cap)
{
  uint32_t blocked = kProbeBlockCount;
  if (!gVerdictFirst) {
    blocked = kProbeBlockUnarmed;
  } else if (!gHost) {
    blocked = kProbeBlockHost;
  } else if (gRingBusy[kRingDriver]) {
    blocked = kProbeBlockBusy;
  }
  if (blocked != kProbeBlockCount) {
    /*
     * Round306. The exit is where the reading has to be said: a consumer that returns here never
     * reaches the report below, so its pass counter stays zero and a whole boot can be silent for a
     * reason that only this line is standing where it can name. teeSay is a producer -- it writes the
     * driver's own RAM ring, no lock, no sleep, no card -- which is what lets it be said from a path
     * that by definition could not do a block write. If the window is not armed, this row cannot reach
     * the card either, and then the frame is empty in a way that says so beside it.
     */
    __atomic_fetch_add (&gProbeBlocked[blocked], 1u, __ATOMIC_RELAXED);
    const uint32_t slot = probeWhySlot (why);
    if (gProbeSilenceSay[slot] < kProbeSilenceSayMax) {
      gProbeSilenceSay[slot]++;
      teeSay ("CloverSdhci r307: probe silent why=%s guard=%s calls=%u seen=%u next=%u flushed=%u"
              " blocked=%u/%u/%u empty=%u cut=%u\n",
              why, blocked == kProbeBlockUnarmed ? "unarmed"
                   : (blocked == kProbeBlockHost ? "host" : "busy"),
              static_cast<unsigned int> (__atomic_load_n (&gProbeSeq, __ATOMIC_RELAXED)),
              static_cast<unsigned int> (__atomic_load_n (&gProbeNext, __ATOMIC_RELAXED)),
              gProbeFlushed,
              static_cast<unsigned int> (__atomic_load_n (&gProbeBlocked[kProbeBlockUnarmed],
                                                           __ATOMIC_RELAXED)),
              static_cast<unsigned int> (__atomic_load_n (&gProbeBlocked[kProbeBlockHost],
                                                          __ATOMIC_RELAXED)),
              static_cast<unsigned int> (__atomic_load_n (&gProbeBlocked[kProbeBlockBusy],
                                                          __ATOMIC_RELAXED)),
              static_cast<unsigned int> (__atomic_load_n (&gProbeEmptySay, __ATOMIC_RELAXED)),
              static_cast<unsigned int> (__atomic_load_n (&gProbeCutSay, __ATOMIC_RELAXED)));
    }
    return 0;
  }
  gProbeCalls++;
  uint32_t wrote = 0;
  for (;;) {
    const uint32_t seen = __atomic_load_n (&gProbeSeq, __ATOMIC_RELAXED);
    const uint32_t next = __atomic_load_n (&gProbeNext, __ATOMIC_RELAXED);
    if (next >= seen || (cap && wrote >= cap)) {
      break;
    }
    const uint32_t slot = next % kProbeSlots;
    if (__atomic_load_n (&gProbeArm[slot], __ATOMIC_ACQUIRE) != next + 1) {
      // Two different things look identical here -- a producer that took this number and has not
      // sealed it yet, and a producer that wrapped and put a newer row in the slot. The bound is what
      // separates waiting for the first from being wedged behind it forever: after kProbeSealRetries
      // passes on the same number the row is dropped and named, because a relay that cannot pass a
      // half-written row must still not stop the rows behind it.
      if (++gProbeSealWait < kProbeSealRetries) {
        gProbeStalled++;
        break;
      }
      gProbeSealWait = 0;
      gProbeSkipped++;
      __atomic_store_n (&gProbeNext, next + 1, __ATOMIC_RELAXED);
      continue;
    }
    gProbeSealWait = 0;
    uint32_t length = 0;
    while (length < kProbeTextBytes - 1 && gProbeText[slot][length]) {
      length++;
    }
    if (!length) {
      __atomic_store_n (&gProbeNext, next + 1, __ATOMIC_RELAXED);
      continue;
    }
    if (!ringWriteRecord (kRingDriver, gProbeText[slot], length, kProbeKind)) {
      // The cursor does not move: this row is retried by the next pass rather than paid for by a card
      // that was busy for one write.
      gProbeRefused++;
      break;
    }
    __atomic_store_n (&gProbeNext, next + 1, __ATOMIC_RELAXED);
    wrote++;
  }
  gProbeFlushed += wrote;
  const uint32_t seen = __atomic_load_n (&gProbeSeq, __ATOMIC_RELAXED);
  const uint32_t next = __atomic_load_n (&gProbeNext, __ATOMIC_RELAXED);
  const uint32_t pend = seen - next;
  // Round307's half of the same branch. The sentence is the same -- "a consumer ran, and nothing has
  // ever been delivered" -- but the budget is now this speaker's own, so four tail passes cannot buy
  // the drain out of a voice. `calls=` rides along because the pair that makes the reading is
  // `calls` with `seen`: passes that ran beside rows that were never handed over.
  const uint32_t whySlot = probeWhySlot (why);
  const bool nothing = gProbeCalls && !gProbeFlushed &&
      gProbeNothingSay[whySlot] < kProbeNothingSayMax;
  // Two readings, and the pair is the point. `wrote` with nothing left pending says a burst the kernel
  // handed over reached the card, with the numbers to prove it. `pend` with a write that has not moved
  // says the opposite: the rows exist in BSS and are not on the card, which is what boot 93 could only
  // infer from the absence of everything. Both are bounded so a boot cannot fill its own ring with them.
  if (gProbeReports[whySlot] < kProbeReportsMax &&
      ((wrote && !pend) || (pend && gProbeCalls % kProbeBacklogEvery == 0) || nothing)) {
    if (nothing) {
      gProbeNothingSay[whySlot]++;
    }
    gProbeReports[whySlot]++;
    // Round305 appends the beat's starvation and the last refused block write to this line, in front of
    // nothing that was already here: the beat cannot report them when it is the thing being starved, and
    // boot 94's five reports are the only consumer on that boot that ever ran. A reader that sees the
    // same starve= pair twice -- here and in the next beat record -- is seeing one counter, not two.
    // Round306 appends the guard exits and the producer's own two exits. Round307 moves the token and
    // adds `calls=`, because a reader that cannot tell which image wrote a record would read an older
    // line's missing column as a zero.
    teeSay ("CloverSdhci r307: probe why=%s calls=%u wrote=%u seen=%u next=%u pend=%u flushed=%u skipped=%u"
            " stalled=%u refused=%u dropped=%u starve=%u/%u/%u/%u pio=%u/%x/%u@%u ch=%u"
            " blocked=%u/%u/%u empty=%u cut=%u\n",
            why, gProbeCalls, wrote, seen, next, pend, gProbeFlushed, gProbeSkipped, gProbeStalled,
            gProbeRefused, static_cast<unsigned int> (__atomic_load_n (&gProbeDropped,
                                                                       __ATOMIC_RELAXED)),
            static_cast<unsigned int> (__atomic_load_n (&gBeatStarved[kBeatBlind], __ATOMIC_RELAXED)),
            static_cast<unsigned int> (__atomic_load_n (&gBeatStarved[kBeatPanic], __ATOMIC_RELAXED)),
            static_cast<unsigned int> (__atomic_load_n (&gBeatStarved[kBeatYoung], __ATOMIC_RELAXED)),
            static_cast<unsigned int> (__atomic_load_n (&gBeatStarved[kBeatHeld], __ATOMIC_RELAXED)),
            gPioFailCode, gPioFailStatus, gPioFailWords, gPioFailMs, gPioFailAttempt,
            static_cast<unsigned int> (__atomic_load_n (&gProbeBlocked[kProbeBlockUnarmed],
                                                        __ATOMIC_RELAXED)),
            static_cast<unsigned int> (__atomic_load_n (&gProbeBlocked[kProbeBlockHost],
                                                        __ATOMIC_RELAXED)),
            static_cast<unsigned int> (__atomic_load_n (&gProbeBlocked[kProbeBlockBusy],
                                                        __ATOMIC_RELAXED)),
            static_cast<unsigned int> (__atomic_load_n (&gProbeEmptySay, __ATOMIC_RELAXED)),
            static_cast<unsigned int> (__atomic_load_n (&gProbeCutSay, __ATOMIC_RELAXED)));
  }
  return wrote;
}

static uint32_t panicProbeFlush (void)
{
  return probeFlush ("drain", 0);
}

static void panicDrainRings (void)
{
  // Round309: the drain's first statement is a record that does not wait for the gates below it, because boot 99
  // proved this function can be entered and still leave the card with none of the rows it says. Written blind and
  // forced (sector 63, this boot's stamp), it answers whether the drain ran and what the host ticket was at that
  // instant -- the reading the r212 entry row could not give, since that row itself is one of the rows missing.
  gPanicDrainInOk = flightPanicDrainIn () ? 1u : 0u;
  if (gPanicDrained || !gVerdictFirst || !gHost) {
    return;
  }
  gPanicDrained = 1;
  // Round212: the entry is said before anything that can refuse. Round211's frame read an r184
  // line claiming four records and could not tell whether that drain had run on this boot at all;
  // this line names every input the drain decides from, and the boot id in its record covers the
  // rest.
  panicTailSay ("CloverSdhci r212: drain enter boot=%u first=%u panic_first=%u verdict=%u tee=%u "
                "append_rc=%u probe_seen=%u\n",
                static_cast<unsigned int> (gBootId),
                static_cast<unsigned int> (gVerdictFirst),
                static_cast<unsigned int> (gPanicLogFirst),
                static_cast<unsigned int> (gVerdictReady),
                static_cast<unsigned int> (gTeeReady),
                static_cast<unsigned int> (gPanicAppendRc),
                static_cast<unsigned int> (__atomic_load_n (&gProbeSeq, __ATOMIC_RELAXED)));
  for (uint32_t pass = 0; pass < kPanicTeePasses; pass++) {
    teeKernelLog ();
  }
  // Round209: after the tee has had its passes, read the buffer from the sink's side. On a boot
  // whose block path stopped, these passes find nothing and this is the only reading left.
  panicTailRead ();
  /*
   * Round301: the sentence, if the report window refused it. It is queued ahead of the flushes below
   * so it rides this drain's own write, and a ring row whose write is refused stays queued for the
   * next flush -- which is precisely what panicAppendText() cannot do, and why boots 89 and 90
   * reached the sink with a name and left the card with none. The copy is only said when the append
   * did not land (rc < 100): the report record is the same sentence, and repeating it would make the
   * host read one panic as two.
   */
  if (gPanicStashLength != 0 && gPanicAppendRc < 100) {
    char part[kPanicStashPart + 1];
    for (uint32_t at = 0; at < gPanicStashLength; at += kPanicStashPart) {
      const uint32_t take = (gPanicStashLength - at) < kPanicStashPart
          ? (gPanicStashLength - at) : kPanicStashPart;
      for (uint32_t i = 0; i < take; i++) {
        part[i] = gPanicStash[at + i];
      }
      part[take] = '\0';
      panicTailSay ("CloverSdhci r301: panic text rc=%u chars=%u part=%u %s\n",
                    static_cast<unsigned int> (gPanicAppendRc),
                    static_cast<unsigned int> (gPanicStashLength),
                    gPanicStashSaid + 1, part);
      gPanicStashSaid++;
    }
  }
  // The status row is said whether or not there was anything to say: a frame with no r301 text rows
  // has to be able to tell "the append landed, so the record already carries it" from "the sink got
  // no text" from "the copy path broke". rc >= 100 with chars > 0 and said = 0 is the first, and the
  // kind-20 slot is then the reading.
  panicTailSay ("CloverSdhci r301: panic copy rc=%u chars=%u said=%u\n",
                static_cast<unsigned int> (gPanicAppendRc),
                static_cast<unsigned int> (gPanicStashLength), gPanicStashSaid);
  // Round302: and what the blind write did, said unconditionally for the same reason. The record in
  // sector kFlightTextOffset + boot % kFlightTextSlots is the card's answer to 'did the sentence
  // land'; this row is the driver's answer to 'did it get that far and what came back'. The pair is
  // what tells a refused block write apart from a drain that never ran -- boot 91 had no drain rows
  // at all, and the sector there was another writer's record wearing this one's offsets.
  panicTailSay ("CloverSdhci r302: blind ok=%u chars=%u total=%u slot=%u\n",
                gPanicBlindOk, gPanicBlindChars,
                static_cast<unsigned int> (gPanicStashLength),
                gBootId % kFlightTextSlots);
  gPanicDrainWritten += panicProbeFlush ();
  for (uint32_t which = 0; which < kRingCount; which++) {
    if (which == kRingKernel ? !gTeeReady : !gVerdictReady) {
      continue;
    }
    gPanicDrainWritten += panicRingFlush (which);
  }
  // The drain has to be readable as a drain: a host that finds a quiet ring cannot otherwise tell "it
  // was empty" from "it ran and the card said no". teeSay only queues, and the ring it queues into was
  // drained above, so the one record is written here rather than left for a flush that will not come.
  teeSay ("CloverSdhci r184: panic drain wrote=%u dropped=%u tee_passes=%u\n",
          gPanicDrainWritten, gPanicDrainDropped, kPanicTeePasses);
  // Round229: the filter counts, at the one moment they are final. Without this line a host that
  // finds the ring starting at 400 s cannot tell whether the earlier lines were lapped or dropped
  // on purpose, and the whole point of filtering was to make the ring's coverage readable.
  teeSay ("CloverSdhci r229: tee filter fl=%u nag=%u exit kept=%u dropped=%u comms=%u\n",
          gTeeFlDropped, gTeeNagDropped, gTeeExitKept, gTeeExitDropped, gTeeExitCommsUsed);
  teeSay ("CloverSdhci r230: tee sync-in=%u sync-out=%u dropped=%u\n",
          gTeeSyncInKept, gTeeSyncOutKept, gTeeSyncDropped);
  // Round309: the blind sector's second leg, on the ring. If the drain-in record is absent from the card this row
  // is what tells "the write was refused" apart from "the drain was never entered" -- in=1 with no sector is the
  // first, and no row at all is the second. It rides the pipe the round is measuring, so it is a copy and never a
  // substitute: the record is the reading, this is the attribution.
  teeSay ("CloverSdhci r309: drain legs in=%u boot=%u host=%x probe=%u\n",
          gPanicDrainInOk, static_cast<unsigned int> (gBootId),
          static_cast<unsigned int> (__atomic_load_n (&gHostOwner, __ATOMIC_ACQUIRE)),
          static_cast<unsigned int> (__atomic_load_n (&gProbeSeq, __ATOMIC_RELAXED)));
  // Round212: the exit line is queued before the last flush so it rides with it, and it repeats the
  // boot id: a pair of lines with the same id is one boot's drain seen from both ends.
  // Round308 renames one column and adds the column it was mistaken for. `gRingPending[kRingDriver]`
  // counts the driver's whole tee ring, so the row named it probe_pend while saying nothing about the
  // relay: boot 97 read a 0 there and could not tell an empty relay from a relay whose rows were never
  // drained. ring_pend= keeps that number under the name its producer owns, and probe_pend= is now the
  // relay's own backlog -- sealed minus consumed, the same pair the flush reports beside a write.
  panicTailSay ("CloverSdhci r308: drain exit boot=%u wrote=%u dropped=%u tee_passes=%u "
                "probe=%u probe_drop=%u ring_pend=%u probe_pend=%u\n",
                static_cast<unsigned int> (gBootId), gPanicDrainWritten, gPanicDrainDropped,
                kPanicTeePasses, gProbeFlushed,
                static_cast<unsigned int> (__atomic_load_n (&gProbeDropped, __ATOMIC_RELAXED)),
                static_cast<unsigned int> (gRingPending[kRingDriver]),
                static_cast<unsigned int> (__atomic_load_n (&gProbeSeq, __ATOMIC_RELAXED)) -
                static_cast<unsigned int> (__atomic_load_n (&gProbeNext, __ATOMIC_RELAXED)));
  gPanicDrainWritten += panicRingFlush (kRingDriver);
}

// Round159: the write mirror of readMultiBlock. One CMD25 for several consecutive blocks, then// CMD12 with the busy spelling. The FIFO is level-gated rather than paced: below 26 MHz the card
// clocks the host down whenever the buffer is empty, so SPACE_AVAILABLE alone can never overrun it,
// and at 50 MHz the 160 us gap would make a 32-block write cost 1.3 ms of dead time per block.
// CMD24 stays the single-block fallback; every DATA_* error bit still refuses the transfer.
static bool writeMultiBlock (uint32_t lba, const uint32_t *words, uint32_t blocks,
                             uint32_t &wordCount, uint32_t &status)
{
  const uint32_t totalWords = blocks * kBlockWords;
  wordCount = 0;
  status = 0;
  if (!waitCommandSlot ()) {
    return multiFailure (1, lba, blocks, status, 0, totalWords);
  }
  write8 (SDHCI_TIMEOUT_CONTROL, 0x0e);
  write16 (SDHCI_BLOCK_SIZE, 512);
  write16 (SDHCI_BLOCK_COUNT, static_cast<uint16_t>(blocks));
  write32 (SDHCI_INT_STATUS, 0xffffffffu);
  write32 (SDHCI_ARGUMENT, lba);
  write16 (SDHCI_TRANSFER_MODE, SDHCI_TRNS_BLK_CNT_EN | SDHCI_TRNS_MULTI);
  write16 (SDHCI_COMMAND, SDHCI_MAKE_CMD (MMC_WRITE_MULTIPLE_BLOCK,
                                          SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                          SDHCI_CMD_INDEX | SDHCI_CMD_DATA));
  uint32_t spins = 0;
  for (;;) {
    status = read32 (SDHCI_INT_STATUS);
    if (status & SDHCI_INT_CMD_MASK) {
      break;
    }
    if (++spins >= kCommandSpinLimit) {
      break;
    }
  }
  if (!(status & SDHCI_INT_RESPONSE) ||
      (status & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX))) {
    sendStopCommand ();
    return multiFailure (1, lba, blocks, status, 0, totalWords);
  }
  uint64_t frequency = counterFrequency ();
  if (frequency < 1000000ULL || frequency > 1000000000ULL) {
    frequency = 19200000ULL;
  }
  // Round168: 10 ms per block was the 400 kHz data time, but not the time a card needs to accept
  // words while it is still programming the previous batch. 50 ms per block keeps one slow batch a
  // batch instead of a fallback into 32 single-block writes that each hit the same wall.
  const uint64_t deadline = frequency / 5ULL +
                            static_cast<uint64_t>(blocks) * (frequency / 20ULL);
  const uint64_t start = readCounter ();
  uint32_t guard = 0;
  bool clean = false;
  for (;;) {
    status = read32 (SDHCI_INT_STATUS);
    if (status & kContentErrorMask) {
      break;
    }
    if (wordCount >= totalWords) {
      if (status & SDHCI_INT_DATA_END) {
        clean = true;
      }
      break;
    }
    // Round171: same early exit as the read drain -- an end bit beside an empty buffer port means
    // the card stopped taking words, and the remaining deadline would be spent proving it.
    if ((status & SDHCI_INT_DATA_END) &&
        !(read32 (SDHCI_PRESENT_STATE) & SDHCI_SPACE_AVAILABLE)) {
      break;
    }
    if (readCounter () - start > deadline) {
      break;
    }
    if (read32 (SDHCI_PRESENT_STATE) & SDHCI_SPACE_AVAILABLE) {
      write32 (SDHCI_BUFFER, words[wordCount++]);
    }
    if (++guard >= kCommandSpinLimit) {
      break;
    }
  }
  while (wordCount < totalWords &&
         (read32 (SDHCI_PRESENT_STATE) & SDHCI_SPACE_AVAILABLE)) {
    write32 (SDHCI_BUFFER, words[wordCount++]);
  }
  status = read32 (SDHCI_INT_STATUS);
  const bool delivered = clean || batchDelivered (status, wordCount, totalWords);
  // Round167: a stop that never completed leaves the card in data mode, and every later command
  // would look like a fresh refusal. The old shape ignored this return value, so a wedged card
  // read exactly like a clean one until the next request timed out. Both a refused stop and a
  // dirty data phase now reset the host's data circuit before the caller retries.
  //
  // Round171 keeps the stop in the verdict: a batch whose words all went out but whose CMD12 was
  // refused is a card still holding DAT0, and the caller would then be told "stored" about blocks
  // the card may still be mid-stream on. The caller retries such a chunk from a reset circuit.
  const bool stopped = sendStopCommand ();
  if (delivered && stopped) {
    return true;
  }
  resetReadPath ();
  return multiFailure (1, lba, blocks, status, wordCount, totalWords);
}

// ----------------------------------------------------------------- round173: the ADMA2 write path
//
// WHY THERE IS ONE
//
// The same ceiling as the read side, but costing the other thing: hold time on the block guard. A
// 64 KiB chunk goes to the card one word per SPACE_AVAILABLE poll, and while it is going nothing else
// can touch this host -- the r170 frame showed what a long write sequence does to the other volume's
// reads. The host can empty its own buffer without the CPU, which is what ADMA2 is.
//
// WHY IT IS ALLOWED TO TOUCH THE CARD AT ALL
//
// A wrong DMA read costs an EIO; a wrong DMA write changes what is on the eMMC. So this path does not
// get to write bytes anyone invented. checkWriteDmaPath() takes a run inside the log window -- since
// round185 the one partition this port may damage without damaging anything, because nothing mounts
// it -- reads it through PIO, and writes those same bytes back through ADMA2. If the host delivered
// exactly what the CPU handed it, the partition reads back byte
// for byte as it was found and the flag is set; if the table was wrong, the run now holds wrong bytes
// in a log area, which is the entire blast radius. Before that, the identical bytes go through the PIO
// write and read back as a control: a medium that will not round-trip its own bytes through PIO cannot
// produce a DMA verdict either, and the gate refuses instead of printing a meaningless zero.
//
// WHAT STAYS ON PIO
//
// The panic screen's own two-block writes (clover_emmc_panic_append) and the bring-up report's logfs
// writes never see this path: they call writePioBlock directly, and they are the last thing a dying
// boot does. Nothing about them should depend on a flag this round introduced.
static const uint32_t kWriteDmaFailureLimit = 8;
static bool gWriteDmaTrusted = false;
static bool gWriteDmaCheckDone = false;
static uint32_t gWriteDmaFailures = 0;

// The run the gate may rewrite. Round185 moved it: the reasoning that made logfs safe to scribble --
// "its contents are only the bring-up reports it rewrites every boot" -- stopped being true the day
// logfs became this boot's root container, and the gate has been sitting 4096 sectors inside a
// filesystem it must not damage. The log window is now 64 MiB of partition nobody mounts, so the
// blast radius is what the comment always claimed it was. Offset 4096 in is clear of the report
// slots and both rings, and 16 blocks is four times the minimum a batch is, so the sample itself is
// still a real batch.
static const uint32_t kWriteCheckBlocks = 16;
static const uint32_t kWriteCheckWords = kWriteCheckBlocks * kBlockWords;
static const uint32_t kWriteCheckOffset = 4096u;

// Which part of a DMA write refused, with the same eight-line bound the read reporter uses. The
// ADMA_ERROR byte names the descriptor the host gave up on; HOST_CONTROL is printed because a DMA
// scheme bit that did not stick looks different from a table the bus refused to read.
static void reportWriteDmaFailure (const char *stage, uint32_t lba, uint32_t blocks, uint32_t status,
                                   uint64_t elapsed = 0, uint64_t budget = 0, uint32_t polls = 0,
                                   uint64_t frequency = 19200000ULL)
{
  static uint32_t reported = 0;
  if (reported >= 8) {
    return;
  }
  reported++;
  sdhciSay ("CloverSdhci r173: dma write %s lba=%u blocks=%u st=%08x err8=%02x hctl=%02x "
            "ms=%llu budget=%llu polls=%u\n",
         stage, lba, blocks, status, read8 (SDHCI_ADMA_ERROR), read8 (SDHCI_HOST_CONTROL),
         elapsed * 1000ULL / frequency, budget * 1000ULL / frequency, polls);
}

// One block run read straight through PIO, block by block. The verification reads of this round's
// gate deliberately use the single-block path and not the batch: the batch is r171's subject, the DMA
// path is r173's, and a comparison is only worth printing if the thing it measures with is the oldest
// proved path in the file.
static bool readRunPio (uint32_t lba, uint32_t *dst, uint32_t blocks)
{
  for (uint32_t block = 0; block < blocks; block++) {
    uint32_t got = 0;
    uint32_t status = 0;
    if (!readPioBlock (lba + block, dst + static_cast<uint64_t> (block) * kBlockWords, got, status)) {
      return false;
    }
  }
  return true;
}

// One CMD25 with the data phase handed to the host. The bytes are copied into the bounce first because
// the caller's buffer is whoever's -- an IOMemoryDescriptor's staging array, a vnode pager's page --
// and none of it has to be physically contiguous for this path to work. Everything else is the r172
// read run mirrored: the same table, the same scheme bits, the same explicit CMD12, the same
// per-block share of the deadline. The difference is one bit in TRANSFER_MODE (READ clear) and the
// direction of the cache operation: here the CPU cleans the bounce before the host reads it, and
// there is nothing to invalidate afterwards.
static bool writeAdmaRun (uint32_t lba, const uint32_t *src, uint32_t blocks, uint32_t &status,
                          uint64_t &ticks)
{
  status = 0;
  ticks = 0;
  if (!dmaBringUp () || !blocks || blocks * 512u > kAdmaDataBytes) {
    return false;
  }
  // Round173: a DMA write pushes whatever the buffer holds out in order, so words left in the FIFO by
  // a refused read would shift the whole run toward the card without a single error bit moving. The
  // PIO batch has always been immune to this (the CPU writes each word when it is asked), the host is
  // not, so the drain that r165 added for reads is the first thing this path does.
  if (!prepareReadPath ()) {
    reportWriteDmaFailure ("path", lba, blocks, 0);
    return false;
  }
  if (!waitCommandSlot ()) {
    reportWriteDmaFailure ("slot", lba, blocks, 0);
    return false;
  }
  const uint32_t bytes = blocks * 512u;
  const uint32_t words = bytes / 4u;
  uint32_t *bounce = reinterpret_cast<uint32_t *> (gAdma.dataVirt);
  for (uint32_t word = 0; word < words; word++) {
    bounce[word] = src[word];
  }
  admaBuildTable (bytes);
  // The host will fetch these lines over the bus; anything still dirty only in the CPU's cache would
  // be bytes the card never sees.
  cacheCleanInvalidate (gAdma.dataVirt, bytes);
  uint8_t control = read8 (SDHCI_HOST_CONTROL);
  control &= static_cast<uint8_t> (~SDHCI_CTRL_DMA_MASK);
  control |= static_cast<uint8_t> (gAdma.wide ? SDHCI_CTRL_ADMA64 : SDHCI_CTRL_ADMA32);
  write8 (SDHCI_HOST_CONTROL, control);
  write32 (SDHCI_ADMA_ADDRESS, static_cast<uint32_t> (gAdma.tablePhys & 0xffffffffULL));
  if (gAdma.wide) {
    write32 (SDHCI_ADMA_ADDRESS_HI, static_cast<uint32_t> (gAdma.tablePhys >> 32));
  }
  write8 (SDHCI_TIMEOUT_CONTROL, 0x0e);
  write16 (SDHCI_BLOCK_SIZE, 512);
  write16 (SDHCI_BLOCK_COUNT, static_cast<uint16_t> (blocks));
  write32 (SDHCI_INT_STATUS, 0xffffffffu);
  write32 (SDHCI_ARGUMENT, lba);
  // READ stays clear: this is the only bit that says which way the words go.
  write16 (SDHCI_TRANSFER_MODE, SDHCI_TRNS_BLK_CNT_EN | SDHCI_TRNS_MULTI | SDHCI_TRNS_DMA);
  write16 (SDHCI_COMMAND, SDHCI_MAKE_CMD (MMC_WRITE_MULTIPLE_BLOCK, SDHCI_CMD_RESP_SHORT |
                                          SDHCI_CMD_CRC | SDHCI_CMD_INDEX | SDHCI_CMD_DATA));
  uint32_t spins = 0;
  for (;;) {
    status = read32 (SDHCI_INT_STATUS);
    if (status & SDHCI_INT_CMD_MASK) {
      break;
    }
    if (++spins >= kCommandSpinLimit) {
      break;
    }
  }
  if (!(status & SDHCI_INT_RESPONSE) ||
      (status & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX))) {
    // Round167's rule for the write side: a refused command still has to be stopped, or the card is
    // left in data mode and the next request inherits it.
    sendStopCommand ();
    resetReadPath ();
    reportWriteDmaFailure ("cmd", lba, blocks, status);
    return false;
  }
  uint64_t frequency = counterFrequency ();
  if (frequency < 1000000ULL || frequency > 1000000000ULL) {
    frequency = 19200000ULL;
  }
  const uint64_t deadline = frequency / 5ULL +
                            static_cast<uint64_t> (blocks) * (frequency / 20ULL);
  const uint64_t start = readCounter ();
  bool ended = false;
  bool spun = false;
  uint32_t guard = 0;
  for (;;) {
    status = read32 (SDHCI_INT_STATUS);
    if (status & (kContentErrorMask | SDHCI_INT_ADMA_ERROR)) {
      break;
    }
    if (status & SDHCI_INT_DATA_END) {
      ended = true;
      break;
    }
    if (readCounter () - start > deadline) {
      break;
    }
    if (++guard >= kCommandSpinLimit) {
      spun = true;
      break;
    }
  }
  ticks = readCounter () - start;
  status = read32 (SDHCI_INT_STATUS);
  const bool stopped = sendStopCommand ();
  if (!ended || (status & (kContentErrorMask | SDHCI_INT_ADMA_ERROR)) || !stopped) {
    resetReadPath ();
    reportWriteDmaFailure (!ended ? (spun ? "spin" : "deadline") : (stopped ? "error" : "stop"),
                           lba, blocks, status, ticks, deadline, guard, frequency);
    return false;
  }
  return true;
}

// The verdict this round exists to produce, in the only order that makes it mean something: read what
// is there, prove the PIO path round-trips those bytes, then prove ADMA2 delivers the same round trip,
// and time both. Everything prints on one line either way, including the refusal reasons, because
// "the gate never ran" and "the gate said no" have to be tellable apart from a single frame.
static void checkWriteDmaPath (void)
{
  gWriteDmaCheckDone = true;
  const uint32_t sectors = __atomic_load_n (&gLogWindowSectors, __ATOMIC_ACQUIRE);
  const uint32_t windowFirst = __atomic_load_n (&gLogWindowFirst, __ATOMIC_ACQUIRE);
  const uint32_t checkLba = windowFirst + kWriteCheckOffset;
  if (windowFirst == 0 ||
      checkLba + kWriteCheckBlocks - 1 > windowFirst + sectors - 1 ||
      checkLba + kWriteCheckBlocks > __atomic_load_n (&gBlockSectors, __ATOMIC_ACQUIRE)) {
    sdhciSay ("CloverSdhci r173: write gate refuses, log window first=%u blocks=%u sectors=%u\n",
           windowFirst, kWriteCheckBlocks, sectors);
    return;
  }
  if (!dmaBringUp ()) {
    // dmaBringUp printed its own refusal reason.
    return;
  }
  uint64_t frequency = counterFrequency ();
  if (frequency < 1000000ULL || frequency > 1000000000ULL) {
    frequency = 19200000ULL;
  }
  uint32_t firstPio = kWriteCheckWords;
  uint32_t firstDma = kWriteCheckWords;
  uint32_t diffPio = kWriteCheckWords;
  uint32_t diffDma = kWriteCheckWords;
  uint32_t pioStatus = 0;
  uint32_t dmaStatus = 0;
  uint32_t settleStatus = 0;
  uint32_t rsp = 0;
  uint32_t polls = 0;
  uint64_t pioTicks = 0;
  uint64_t dmaTicks = 0;

  const bool originalRead = readRunPio (checkLba, gCheckBatchA, kWriteCheckBlocks);
  if (!originalRead) {
    sdhciSay ("CloverSdhci r173: write gate stops, the log window run would not read back lba=%u\n",
           checkLba);
    return;
  }
  // Control run: the same bytes, through the path every earlier round proved, with the card's own
  // settle inside the timing so the number is comparable with the DMA run below.
  uint64_t start = readCounter ();
  bool pioWrite = false;
  for (uint32_t attempt = 0; attempt < 2 && !pioWrite; attempt++) {
    uint32_t handed = 0;
    if (!writeMultiBlock (checkLba, gCheckBatchA, kWriteCheckBlocks, handed, pioStatus)) {
      continue;
    }
    if (writeSettled (rsp, settleStatus, polls)) {
      pioWrite = true;
      break;
    }
    resetReadPath ();
    waitCardIdle (frequency);
  }
  pioTicks = readCounter () - start;
  const bool pioRead = readRunPio (checkLba, gCheckBatchB, kWriteCheckBlocks);
  if (pioRead) {
    diffPio = countWordDiff (gCheckBatchA, gCheckBatchB, kWriteCheckWords, firstPio);
  }
  // The DMA run only gets asked once the control proved the medium round-trips this run at all.
  bool dmaWrite = false;
  const bool controlHeld = pioWrite && pioRead && diffPio == 0;
  if (controlHeld) {
    start = readCounter ();
    for (uint32_t attempt = 0; attempt < 2 && !dmaWrite; attempt++) {
      uint64_t ticks = 0;
      if (!writeAdmaRun (checkLba, gCheckBatchA, kWriteCheckBlocks, dmaStatus, ticks)) {
        continue;
      }
      if (writeSettled (rsp, settleStatus, polls)) {
        dmaWrite = true;
        dmaTicks = ticks;
        break;
      }
      resetReadPath ();
      waitCardIdle (frequency);
    }
  }
  bool dmaRead = false;
  if (controlHeld) {
    dmaRead = readRunPio (checkLba, gCheckSingle, kWriteCheckBlocks);
    if (dmaRead) {
      diffDma = countWordDiff (gCheckBatchA, gCheckSingle, kWriteCheckWords, firstDma);
    }
  }
  gWriteDmaTrusted = controlHeld && dmaWrite && dmaRead && diffDma == 0;
  const uint64_t bytes = static_cast<uint64_t> (kWriteCheckBlocks) * 512ULL;
  const uint64_t pioMs = pioTicks * 1000ULL / frequency;
  const uint64_t dmaMs = dmaTicks * 1000ULL / frequency;
  const uint64_t pioRate = pioMs > 0 ? (bytes / 1024ULL) * 1000ULL / pioMs : 0;
  const uint64_t dmaRate = dmaMs > 0 ? (bytes / 1024ULL) * 1000ULL / dmaMs : 0;
  sdhciSay ("CloverSdhci r173: wrcheck lba=%u blocks=%u control=%d dma=%d diff=%u/%u first=%u/%u "
         "w=%08x/%08x g=%08x/%08x st=%08x/%08x ms=%llu/%llu kbs=%llu/%llu trusted=%d\n",
         checkLba, kWriteCheckBlocks, controlHeld ? 1 : 0, dmaWrite ? 1 : 0,
         diffPio, diffDma, firstPio, firstDma,
         gCheckBatchA[firstPio < kWriteCheckWords ? firstPio : 0],
         gCheckBatchA[firstDma < kWriteCheckWords ? firstDma : 0],
         gCheckBatchB[firstPio < kWriteCheckWords ? firstPio : 0],
         gCheckSingle[firstDma < kWriteCheckWords ? firstDma : 0],
         pioStatus, dmaStatus,
         static_cast<unsigned long long> (pioMs), static_cast<unsigned long long> (dmaMs),
         static_cast<unsigned long long> (pioRate), static_cast<unsigned long long> (dmaRate),
         gWriteDmaTrusted ? 1 : 0);
  if (!gWriteDmaTrusted) {
    sdhciSay ("CloverSdhci r173: writes stay on the PIO path for this boot\n");
  } else {
    sdhciSay ("CloverSdhci r173: ADMA2 writes trusted for this boot\n");
  }
}

// The write half of the pair above. Same gate: ready, measured geometry, one caller at a time with
// the same bounded hand-over, and the counter deadline, because at this point in boot no scheduler
// timer will rescue a stuck host. Callers hand it whole 512-byte blocks; nothing here touches the
// logfs format. Round159 batches up to kReadChunkBlocks per CMD25, exactly as the read path does,
// and falls back to the single-block CMD24 the earlier rounds proved when a batch fails.
extern "C" uint32_t clover_emmc_block_write (uint32_t lba, const uint32_t *words, uint32_t blocks)
{
  if (!words || !blocks) {
    return 0;
  }
  const uint32_t sectors = __atomic_load_n (&gBlockSectors, __ATOMIC_ACQUIRE);
  if (!__atomic_load_n (&gBlockReady, __ATOMIC_ACQUIRE) || !sectors ||
      lba >= sectors || blocks > sectors - lba) {
    return 0;
  }
  uint64_t frequency = 0;
  uint64_t start = 0;
  uint32_t token = 0;
  if (!acquireBlockGuard (frequency, start, token)) {
    return 0;
  }
  flightEnter (kFlightEventWrite, 1, lba, blocks);
  // Round167: the request deadline scales with the run. A flat two seconds was sized for one APFS
  // burst; a single-block fallback over 32 blocks, at the 400 kHz timing this card may still be
  // on, can exceed it, and a short write is a torn checkpoint. Round168 raised the base to ten
  // seconds and the per-block share to 200 ms. Round169 raises the base to the same thirty seconds
  // the settle wait uses: one chunk may legitimately spend that long in the card's programming
  // phase, and the second chunk of the request must not be cut off for it.
  const uint64_t deadline = frequency * 30ULL +
                            static_cast<uint64_t>(blocks) * (frequency / 5ULL);
  // Round173: the write gate, once, on the first request long enough to be a batch. It rewrites
  // nothing but bytes it read a moment earlier and only inside logfs, so a boot that ends up not
  // trusting DMA is a boot whose logfs is exactly what it was. The request deadline is re-stamped
  // afterwards: the gate is not the request's fault.
  if (!gWriteDmaCheckDone && blocks >= 4) {
    flightStage (kStageCheck);
    checkWriteDmaPath ();
    start = readCounter ();
  }
  uint32_t done = 0;
  bool settleReported = false;
  // Round178: same ownership accounting as the read path. The write path is the one the r170 frame
  // caught holding the host -- one APFS transaction flush ran 102 seconds and 120 seconds on the two
  // samples, and every read at the other volume failed with no error bit because it never got the
  // guard. A flush is not one request, so a per-request bound would not have touched it; cutting at
  // chunk boundaries means the longest a holder can keep a reader waiting is one chunk plus its
  // settle wait, and the reader goes between the chunks of the next one.
  uint64_t owned = 0;
  uint32_t sinceCut = 0;
  bool cutIn = false;
  bool held = true;
  bool recovered = false;
  while (done < blocks) {
    // Round213: same rule as the read path -- once the watchdog has taken the host back this
    // request must stop touching the controller.
    if (guardLost (token)) {
      sdhciSay ("CloverSdhci r213: write lost the host lba=%u blocks=%u done=%u\n", lba, blocks,
             done);
      held = false;
      break;
    }
    if (owned + readCounter () - start > deadline) {
      sdhciSay ("CloverSdhci r213: write deadline lba=%u blocks=%u done=%u\n", lba, blocks, done);
      // Round213: leave the data circuit stopped, not mid-transfer for the next request.
      abortReadTransfer ();
      // Round215: a run that never moved a block in thirty seconds is not programming time; the
      // same ladder the read side uses runs once, then the run is re-asked.
      if (done == 0 && !recovered && cardRecoverHeld ("deadline", 0)) {
        recovered = true;
        start = readCounter ();
        owned = 0;
        continue;
      }
      break;
    }
    // Round159: one CMD25 for a run of blocks. The pointer arithmetic is 64-bit for the same reason
    // the read side's is: the caller may hand a Data volume past 16 GiB.
    uint32_t chunk = blocks - done;
    if (chunk > kReadChunkBlocks) {
      chunk = kReadChunkBlocks;
    }
    uint32_t *target = const_cast<uint32_t *>(words) +
                       static_cast<uint64_t>(done) * kBlockWords;
    bool chunkWritten = false;
    // Round173: the DMA path first, once the read-back gate proved it delivers what it was handed. A
    // refused DMA chunk costs one report and falls through to the PIO batch below, so the worst case
    // here is round172's behaviour with a line beside it; eight failures retire DMA writes for the
    // boot. A chunk whose data phase ended but whose card never settled is not charged to the DMA
    // path -- that is the card's programming time, and the PIO batch below waits on it too.
    if (chunk >= 4 && gWriteDmaTrusted) {
      uint32_t dmaStatus = 0;
      uint64_t dmaTicks = 0;
      flightStage (kStageAdma);
      if (writeAdmaRun (lba + done, target, chunk, dmaStatus, dmaTicks)) {
        uint32_t rsp = 0, settleStatus = 0, polls = 0;
        flightStage (kStageSettle);
        if (writeSettled (rsp, settleStatus, polls)) {
          chunkWritten = true;
          static bool writeTimingReported = false;
          if (!writeTimingReported) {
            writeTimingReported = true;
            sdhciSay ("CloverSdhci r173: first dma write lba=%u blocks=%u ms=%llu\n", lba + done,
                   chunk, static_cast<unsigned long long> (dmaTicks * 1000ULL / frequency));
          }
        } else {
          resetReadPath ();
          waitCardIdle (frequency);
          if (!settleReported) {
            settleReported = true;
            sdhciSay ("CloverSdhci r173: dma write not ready lba=%u blocks=%u rsp=%08x st=%08x "
                   "polls=%u\n", lba + done, chunk, rsp, settleStatus, polls);
          }
        }
      } else {
        gWriteDmaFailures++;
        if (gWriteDmaFailures >= kWriteDmaFailureLimit && gWriteDmaTrusted) {
          gWriteDmaTrusted = false;
          sdhciSay ("CloverSdhci r173: dma writes failed %u times, writes go back to the PIO path\n",
                 gWriteDmaFailures);
        }
      }
    }
    if (!chunkWritten && chunk >= 4) {
      // Round168: a batch whose data phase completed but whose card never settled is re-sent once
      // from a reset circuit. Writing the same blocks twice is harmless; abandoning them was what
      // APFS saw as "disk5: general error" and re-sent itself.
      for (uint32_t attempt = 0; attempt < 2 && !chunkWritten; attempt++) {
        uint32_t handed = 0;
        uint32_t status = 0;
        flightStage (kStageBatch);
        if (!writeMultiBlock (lba + done, target, chunk, handed, status)) {
          continue;
        }
        // Round167: DATA_END is not durability. The card says READY_FOR_DATA in TRAN only after
        // the block reached its media, and a batch that never gets there is not counted as done.
        uint32_t rsp = 0, settleStatus = 0, polls = 0;
        flightStage (kStageSettle);
        if (writeSettled (rsp, settleStatus, polls)) {
          chunkWritten = true;
          break;
        }
        resetReadPath ();
        // Round169: a batch that is still programming cannot accept its replacement; wait for
        // the card to report ready before sending the same blocks again.
        waitCardIdle (frequency);
        if (!settleReported) {
          settleReported = true;
          sdhciSay ("CloverSdhci r167: write not ready lba=%u blocks=%u rsp=%08x st=%08x "
                 "polls=%u\n", lba + done, chunk, rsp, settleStatus, polls);
        }
      }
    }
    uint32_t moved = 0;
    if (chunkWritten) {
      moved = chunk;
    } else {
      bool blockWritten = false;
      for (uint32_t attempt = 0; attempt < 2 && !blockWritten; attempt++) {
        uint32_t handed = 0;
        uint32_t status = 0;
        flightStage (kStageSingle);
        // Round305: 0 names this channel as the block device's own write, not an instrument record, so
        // a refusal here cannot be read as the sink's or the tee's.
        gPioAttempt = 0;
        if (!writePioBlock (lba + done, target, handed, status)) {
          continue;
        }
        uint32_t rsp = 0, settleStatus = 0, polls = 0;
        flightStage (kStageSettle);
        if (writeSettled (rsp, settleStatus, polls)) {
          blockWritten = true;
          break;
        }
        resetReadPath ();
        waitCardIdle (frequency);
        if (!settleReported) {
          settleReported = true;
          sdhciSay ("CloverSdhci r167: write not ready lba=%u blocks=1 rsp=%08x st=%08x "
               "polls=%u\n", lba + done, rsp, settleStatus, polls);
        }
      }
      if (!blockWritten) {
        static bool giveUpReported = false;
        if (!giveUpReported) {
          giveUpReported = true;
          sdhciSay ("CloverSdhci r168: write gave up lba=%u blocks=%u done=%u\n",
               lba + done, blocks, done);
        }
        // Round215: both the batch and the single-block fallback were refused. A card that will not
        // take a block is the same wedge the read side recovers from; the ladder runs here, and the
        // caller re-asks for the run it was told was short.
        if (!recovered && cardRecoverHeld ("write", done)) {
          recovered = true;
          start = readCounter ();
          owned = 0;
          continue;
        }
        break;
      }
      moved = 1;
    }
    done += moved;
    sinceCut += moved;
    // Round178: hand the host over after a chunk of progress, exactly as the read side does. Nothing
    // is left half-sent: every branch above ended with the card saying READY_FOR_DATA for the blocks
    // it took, so the card is in TRAN and a reader that comes in next meets an idle bus.
    if (done < blocks && sinceCut >= kReadChunkBlocks) {
      sinceCut = 0;
      if (!handOffBlockGuard (frequency, start, owned, token)) {
        held = false;
        break;
      }
      cutIn = true;
    }
  }
  flightStage (kStageTail);
  flightExit (done);
  if (cutIn) {
    reportGuardHandoff ("write", lba, blocks, done);
  }
  // Round175: same as the read tail -- collect from the kernel log, then spend the guard on the card.
  if (held) {
    if (!guardLost (token)) {
      flightStage (kStageTee);
      teeKernelLog ();
      flightStage (kStageFlush);
      flushVerdictRing ();   // Round174: same reason as the read tail
    }
    releaseBlockGuard (token);
  }
  return done;
}

// Returns the command group of INT_STATUS. `sent` is false when the register was never written,
// which is a host verdict, not a card one. The response words are read even after an error: a CRC
// error still leaves the bits that arrived in the register, and that is evidence.
static uint32_t runCommandWith (uint16_t command, uint32_t argument, bool &sent,
                                uint32_t &response0, uint32_t &response1, uint32_t &spins,
                                bool ignoreDataInhibit)
{
  sent = waitCommandSlot (ignoreDataInhibit);
  if (!sent) {
    response0 = 0;
    response1 = 0;
    spins = 0;
    return 0;
  }
  write32 (SDHCI_INT_STATUS, 0xffffffffu);
  write32 (SDHCI_ARGUMENT, argument);
  write16 (SDHCI_COMMAND, command);
  uint32_t status = 0;
  spins = 0;
  for (;;) {
    status = read32 (SDHCI_INT_STATUS);
    if (status & SDHCI_INT_CMD_MASK) {
      break;
    }
    if (++spins >= kCommandSpinLimit) {
      break;
    }
  }
  response0 = read32 (SDHCI_RESPONSE);
  response1 = read32 (SDHCI_RESPONSE + 4);
  return status;
}

static uint32_t runCommand (uint16_t command, uint32_t argument, bool &sent,
                            uint32_t &response0, uint32_t &response1, uint32_t &spins)
{
  return runCommandWith (command, argument, sent, response0, response1, spins, false);
}

// The one command Linux lets through while the data circuit is still busy is the stop command
// (sdhci.c:1675, "We shouldn't wait for data inhibit for stop commands, even though they might
// use busy signaling"). CMD13 is the same kind of question: it uses the command line only, so a
// stuck DAT line must not keep it from being asked. Everything else still waits for both.
static uint32_t runCommandBusy (uint16_t command, uint32_t argument, bool &sent,
                                uint32_t &response0, uint32_t &response1, uint32_t &spins)
{
  return runCommandWith (command, argument, sent, response0, response1, spins, true);
}

// After a write the card programs, and its R1 says READY_FOR_DATA only when that is done. CMD13
// uses the command line alone, so the DAT busy state must not keep it from being asked; the loop
// is bounded because a card that never reports ready is itself the reading. `polls` is 0 when the
// first answer was already ready, which is the expected shape after a clean DATA_END.
static void waitCardReady (uint32_t &response, uint32_t &status, uint32_t &polls)
{
  response = 0;
  status = 0;
  for (polls = 0; polls < 100; polls++) {
    bool sent = false;
    uint32_t response1 = 0, spins = 0;
    status = runCommandBusy (SDHCI_MAKE_CMD (13, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                             SDHCI_CMD_INDEX),
                             kProbeRca << 16, sent, response, response1, spins);
    if (!sent || (status & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_INDEX)) ||
        (response & R1_READY_FOR_DATA)) {
      break;
    }
  }
}

static const char *commandVerdict (uint32_t status, bool sent)
{
  if (!sent) {
    return "busy";
  }
  if (status & SDHCI_INT_TIMEOUT) {
    return "to";
  }
  if (status & (SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX)) {
    return "crc";
  }
  if (!(status & SDHCI_INT_RESPONSE)) {
    return "noev";
  }
  return "ok";
}

// Same question as commandVerdict, but names which of the three response checks spoke. Mode 7
// prints the raw INT_STATUS next to it, so this only saves the reader from decoding bits.
static const char *probeVerdict (uint32_t status, bool sent)
{
  if (!sent) {
    return "busy";
  }
  if (status & SDHCI_INT_TIMEOUT) {
    return "to";
  }
  if (status & SDHCI_INT_END_BIT) {
    return "end";
  }
  if (status & SDHCI_INT_CRC) {
    return "crc";
  }
  if (status & SDHCI_INT_INDEX) {
    return "idx";
  }
  if (!(status & SDHCI_INT_RESPONSE)) {
    return "noev";
  }
  return "ok";
}

// Mode 10's verdict, which has to separate a command that failed from a command that worked and
// a data phase that did not finish. The command bits come first because a data error can only be
// real once the card accepted READ_SINGLE_BLOCK.
static const char *dataVerdict (uint32_t status, bool sent)
{
  if (!sent) {
    return "busy";
  }
  if (status & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX)) {
    return probeVerdict (status, sent);
  }
  if (status & SDHCI_INT_DATA_TIMEOUT) {
    return "dto";
  }
  if (status & SDHCI_INT_DATA_CRC) {
    return "dcrc";
  }
  if (status & SDHCI_INT_DATA_END_BIT) {
    return "dend";
  }
  if (status & SDHCI_INT_ADMA_ERROR) {
    return "adma";
  }
  if (!(status & SDHCI_INT_RESPONSE)) {
    return "noev";
  }
  if (!(status & SDHCI_INT_DATA_END)) {
    return "nodata";
  }
  return "ok";
}

// Linux resets the command circuit before issuing the next command after a command error
// (sdhci_reset_for_error -> SDHCI_RESET_CMD). Round90 saw this mask self-clear; the bound stays
// because "it did last time" is not a promise on this board.
static bool resetCommandCircuit (void)
{
  write8 (SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD);
  uint32_t guard = 0;
  while (guard++ < kCommandSpinLimit) {
    if (!(read8 (SDHCI_SOFTWARE_RESET) & SDHCI_RESET_CMD)) {
      return true;
    }
  }
  return false;
}

// A 136-bit response lives in all four words; the two-word reader above cannot see a CID.
static uint32_t runCommand4 (uint16_t command, uint32_t argument, bool &sent,
                             uint32_t *response, uint32_t &spins)
{
  sent = waitCommandSlot ();
  if (!sent) {
    for (unsigned i = 0; i < 4; i++) {
      response[i] = 0;
    }
    spins = 0;
    return 0;
  }
  write32 (SDHCI_INT_STATUS, 0xffffffffu);
  write32 (SDHCI_ARGUMENT, argument);
  write16 (SDHCI_COMMAND, command);
  uint32_t status = 0;
  spins = 0;
  for (;;) {
    status = read32 (SDHCI_INT_STATUS);
    if (status & SDHCI_INT_CMD_MASK) {
      break;
    }
    if (++spins >= kCommandSpinLimit) {
      break;
    }
  }
  for (unsigned i = 0; i < 4; i++) {
    response[i] = read32 (SDHCI_RESPONSE + 4 * i);
  }
  return status;
}

// CMD0 plus the CMD1 poll loop: the pair that puts any card back in the ready state. Mode 7 needs
// it between its two CMD2 attempts, and its readings are printed so a silent card is separable
// from a card that simply was not re-armed.
struct CardRearm {
  bool sent0;
  bool sent1;
  bool ready;
  uint32_t status0;
  uint32_t status1;
  uint32_t rsp1;
  uint32_t tries;
  uint32_t timeouts;
};

static CardRearm rearmCard (void)
{
  CardRearm r;
  r.sent0 = false;
  r.sent1 = false;
  r.ready = false;
  r.status0 = 0;
  r.status1 = 0;
  r.rsp1 = 0;
  r.tries = 0;
  r.timeouts = 0;

  uint32_t rsp0 = 0, rsp1 = 0, spins = 0;
  r.status0 = runCommand (SDHCI_MAKE_CMD (0, SDHCI_CMD_RESP_NONE), 0,
                          r.sent0, rsp0, rsp1, spins);
  if (!r.sent0) {
    return r;
  }
  for (r.tries = 1; r.tries <= kCmd1Retries; r.tries++) {
    r.status1 = runCommand (SDHCI_MAKE_CMD (1, SDHCI_CMD_RESP_SHORT), kEmmcOcr,
                            r.sent1, rsp0, rsp1, spins);
    r.rsp1 = rsp0;
    if (!r.sent1) {
      break;
    }
    if (r.status1 & SDHCI_INT_TIMEOUT) {
      r.timeouts++;
      continue;
    }
    if (r.status1 & (SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX)) {
      break;
    }
    if (!(r.status1 & SDHCI_INT_RESPONSE)) {
      break;
    }
    if (rsp0 & 0x80000000u) {
      r.ready = true;
      break;
    }
    for (volatile uint32_t spin = 0; spin < kCmd1Gap; spin = spin + 1) {
    }
  }
  return r;
}

// MSB word first, the order the 136-bit response sits in 0x1c..0x10.
static void emitCid (const uint32_t *response)
{
  for (unsigned i = 0; i < 4; i++) {
    if (i) {
      emitByte (' ');
    }
    emitHex (response[3 - i], 8);
  }
}

// ------------------------------------------------------------------- mode 5: a real card clock
//
// Round90 wrote the SDHCI 10-bit divider and CMD1 still timed out. sdhci-msm.c:1766 says why:
// this controller does not have an internal divider ("MSM controller do not use clock divider");
// the card clock is the GCC root clock, and the firmware left that root clock at the 200 MHz it
// used for HS200. 200 MHz is not an identification clock, so CMD1 never had a chance.
//
// 400 kHz is the table's own row for sdcc1_apps_clk_src (gcc-sdm660.c:735): XO 19.2 MHz / 12 *
// 1/4. clk-rcg.h:10 stores a divider h as 2h-1, so h=12 is 23 (0x17) in CFG[4:0]; the MND fields
// are M=1, N=~3=0xfc, D=~4=0xfb, with CFG[13:12] = 2 selecting the MND path (clk-rcg2.c:290-333).
//
// Mode 5's photograph proved every one of those GCC writes (cfg=00002017, m=01 n=fc d=fb, up=1)
// and then stopped at the host: RESET_ALL never self-cleared, so the legacy init and all four
// commands were skipped. Mode 6 (fullReset=false) leaves GCC alone and changes only the host
// entry: stop the card clock, reset CMD+DATA, then the same bring-up. Its report keeps the GCC
// half so a regression there is still visible.
static const uint64_t kGccPhys = 0x116000;   // GCC 0x100000 (clover.dts:4309) + SDCC1 0x16000
static const uint64_t kGccSize = 0x1000;
static const unsigned kGccAppsGate = 0x004;  // 0x16004: bit0 enable, bit31 off
static const unsigned kGccAhbGate = 0x008;   // 0x16008
static const unsigned kGccAppsCmd = 0x02c;   // 0x1602c: bit0 update, self-clearing
static const unsigned kGccAppsCfg = 0x030;   // 0x16030: 4:0 divider, 10:8 src, 13:12 mode
static const unsigned kGccAppsM = 0x034;
static const unsigned kGccAppsN = 0x038;
static const unsigned kGccAppsD = 0x03c;
static const uint32_t kGccGateEnable = 0x00000001u;
static const uint32_t kGccGateOff = 0x80000000u;
static const uint32_t kGccCfg400k = 0x00002017u;
static const uint32_t kGccCfgMask = 0x0010371fu;
static const uint32_t kGccM = 0x01u, kGccN = 0xfcu, kGccD = 0xfbu;
static const uint32_t kRcgUpdateLimit = 1u << 16;
// Round159: after identification the card runs high-speed SDR. The 50 MHz row of the same table is
// F(50000000, P_GPLL0_EARLY_DIV, 6, 0, 0): src 6, divider h=6 => CFG[4:0]=2h-1=11 (0x0b), and no
// MND (m=n=0), so M/N/D are cleared and CFG[13:12]=0. 50 MHz SDR is the fastest timing that needs
// neither tuning nor the DLL, and the HS_TIMING SWITCH below declares it to the card.
static const uint32_t kGccCfg50m = 0x0000060bu;
// CMD6 SWITCH, write-byte access: (3 << 24) | (EXT_CSD byte << 16) | (value << 8) | cmd_set.
// This is Linux's mmc_switch() spelling (mmc_ops.c), i.e. 0x03B90100 for HS_TIMING=1 and
// 0x03B70200 for BUS_WIDTH=8-bit. EXT_CSD bytes: 185 HS_TIMING, 183 BUS_WIDTH.
static const uint32_t kSwitchWriteByte = 3u << 24;
static const uint32_t kExtCsdHsTiming = 185u;
static const uint32_t kExtCsdBusWidth = 183u;
// Round167: CACHE_CTRL lives in byte 33 and CACHE_SIZE in bytes 249..252. This driver never issues
// FLUSH_CACHE, so a cache left enabled is write-back storage nobody ever commits, and every boot
// here ends in a hard reset.
static const uint32_t kExtCsdCacheCtrl = 33u;
static const uint32_t kHsTimingHighSpeed = 1u;
static const uint32_t kBusWidth8Bit = 2u;
// Vendor clock mux (sdhci-msm.c:78-85): 10:8 select the MCLK, bit18 enables the HS400-only
// selector, 21:19 name it. Default is 2, the free-running MCLK.
static const unsigned kSdhciVendorSpec = 0x20c;
static const unsigned kSdhciVendorSpec3 = 0x250;
static const uint32_t kHcMclkSelMask = 3u << 8;
static const uint32_t kHcMclkSelDefault = 2u << 8;
static const uint32_t kHcSelectInEn = 1u << 18;
static const uint32_t kHcSelectInMask = 7u << 19;
static const uint32_t kPwrSaveDll = 1u << 3;
// CORE_VENDOR_SPEC3 bit 10: "alternative FIFO enable" (sdhci-msm.c:144). Linux clears it in
// sdhci_msm_init with the comment "Ensure SDHCI FIFO is enabled by disabling alternative FIFO"
// (:5586), i.e. 0 leaves the SDHCI buffer port in charge and 1 hands the data path to Qualcomm's
// own FIFO. Round87 read vs3 = 0x02626440, so bit 10 is set on this board as the firmware left it.
static const uint32_t kFifoAltEn = 1u << 10;

static uint32_t readGcc32 (unsigned offset)
{
  uint32_t value = *reinterpret_cast<volatile uint32_t *> (gGcc + offset);
  __asm__ volatile ("dsb sy" ::: "memory");
  return value;
}

static void writeGcc32 (unsigned offset, uint32_t value)
{
  *reinterpret_cast<volatile uint32_t *> (gGcc + offset) = value;
  __asm__ volatile ("dsb sy" ::: "memory");
}

// The firmware was clocking this controller, so both gates should already be on; the enable path
// exists so that a gate lost at handoff is a reading (bit 31 still set) instead of a dead end.
static bool gateOn (unsigned offset)
{
  uint32_t value = readGcc32 (offset);
  if ((value & kGccGateEnable) && !(value & kGccGateOff)) {
    return true;
  }
  writeGcc32 (offset, value | kGccGateEnable);
  uint32_t guard = 0;
  do {
    value = readGcc32 (offset);
    if ((value & kGccGateEnable) && !(value & kGccGateOff)) {
      return true;
    }
  } while (++guard < kCommandSpinLimit);
  return false;
}

// ------------------------------------------------------ round159: the link after identification
//
// CMD1 only answers at 400 kHz, so every boot still identifies there. Up to round158 the driver
// then served the whole boot at 400 kHz on one lane, which is 200 KB/s at best and made the APFS
// mount and dyld's multi-gigabyte cache a multi-hour read. The card is the firmware's HS200 part,
// and high-speed SDR needs no tuning: two SWITCH writes and the root clock. The order is the
// standard init order -- timing, width, then the clock -- and every step is gated on the answer
// before it. A failure leaves the host at the last clock and width the card agreed to, which is
// the mode the previous rounds already proved.

struct FastLink {
  uint32_t hsStatus;
  uint32_t hsResponse;
  uint32_t hsPolls;
  uint32_t bwStatus;
  uint32_t bwResponse;
  uint32_t bwPolls;
  bool highSpeed;
  bool width8;
  bool raised;
  bool verified;
  uint16_t clockAfter;
  uint8_t hostControl;
  uint32_t header0;
  uint32_t header1;
  uint32_t stateStatus;
  uint32_t stateResponse;
  uint32_t statePolls;
};

// One CMD6 in write-byte access. The argument is Linux's mmc_switch() spelling: access 3 in
// [31:26], the EXT_CSD byte in [23:16], the value in [15:8]. R1b's busy signal is on DAT0, and
// the caller's CMD13 is what proves the card came back to transfer, so this only asks for the
// response and lets the next step decide.
static bool switchExtCsd (uint32_t index, uint32_t value, uint32_t &response, uint32_t &status,
                          uint32_t *spinsOut = nullptr)
{
  bool sent = false;
  uint32_t response1 = 0;
  uint32_t spins = 0;
  const uint32_t argument = kSwitchWriteByte | ((index & 0xffu) << 16) | ((value & 0xffu) << 8);
  status = runCommandBusy (SDHCI_MAKE_CMD (MMC_SWITCH, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                           SDHCI_CMD_INDEX),
                           argument, sent, response, response1, spins);
  // Round191: `spins` is how much of kCommandSpinLimit the completion took. A command that never
  // completed leaves st=0 spins=<limit>, which is a different reading from a timeout or a CRC.
  if (spinsOut) {
    *spinsOut = spins;
  }
  return sent && (status & SDHCI_INT_RESPONSE) &&
         !(status & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX));
}

// CMD13 until the card says transfer and ready-for-data. A SWITCH that the card is still
// programming looks exactly like one that finished until this answers.
static bool cardInTransfer (uint32_t &response, uint32_t &status, uint32_t &polls)
{
  waitCardReady (response, status, polls);
  return (response & R1_READY_FOR_DATA) &&
         R1_CURRENT_STATE (response) == R1_STATE_TRAN;
}

// ------------------------------------------------------ round191: what a cache commit costs
//
// The r190 frame ends at `flush-in` with nothing behind it, so the commit is now the wall. The two
// waits inside it are the ones that can be long: writing zero to CACHE_CTRL is an R1b command, and
// while the card writes the cache to media it holds DAT0 low and answers nothing. waitCardReady asks
// CMD13 a hundred times at that card, and each ask that gets no answer burns the whole
// kCommandSpinLimit on INT_STATUS -- a hundred of those is minutes of silence that reads exactly like
// a hang, which is what the r190 frame shows. The host's own press on data inhibit needs no answer
// from the card, so the wait belongs there; CMD13 is only worth asking once the card has let go.
//
// The bound is the card's own: EXT_CSD byte 246, CACHE_FLUSH_TIMEOUT, in units of 10 ms, which is
// how long JEDEC lets a commit run. A card that declares nothing gets the thirty seconds the other
// waits in this driver use, and the ceiling keeps a misread byte from parking the boot behind one
// flush.
static volatile uint32_t gCacheFlushUnits = 0;

static uint32_t flushBoundMs (void)
{
  const uint32_t units = __atomic_load_n (&gCacheFlushUnits, __ATOMIC_ACQUIRE);
  const uint32_t ms = units ? units * 10u : 30000u;
  if (ms < 1000u) {
    return 1000u;
  }
  return ms > 180000u ? 180000u : ms;
}

// One commit's wait, measured. `busyMs` is how long the card held the bus, `samples` how often the
// host looked, `marks` how many five-second marks went by, and `polls` the CMD13s sent after it let
// go. `timedOut` says the bound ran out with the card still busy, which is the one shape that is a
// card failure rather than a slow card.
struct FlushSettle {
  bool ready;
  bool timedOut;
  uint32_t polls;
  uint32_t response;
  uint32_t status;
  uint32_t samples;
  uint32_t marks;
  uint32_t present;
  uint64_t busyMs;
};

static FlushSettle flushSettle (uint64_t frequency, uint32_t boundMs)
{
  FlushSettle out = FlushSettle ();
  const uint64_t bound = frequency * boundMs / 1000ULL;
  const uint64_t start = readCounter ();
  for (;;) {
    out.present = read32 (SDHCI_PRESENT_STATE);
    out.samples++;
    if (!(out.present & SDHCI_DATA_INHIBIT)) {
      break;
    }
    const uint64_t waited = readCounter () - start;
    if (waited > static_cast<uint64_t> (out.marks + 1) * frequency * 5ULL) {
      out.marks++;
    }
    if (waited > bound) {
      out.timedOut = true;
      break;
    }
  }
  out.busyMs = (readCounter () - start) * 1000ULL / frequency;
  // The card is off the bus; this is the only point where CMD13 has something to answer with. Twelve
  // asks with a gap is enough for a card that drops busy a moment before it reports TRAN, and few
  // enough that a card that has stopped answering cannot cost another minute of spins.
  for (uint32_t tries = 0; !out.timedOut && tries < 12; tries++) {
    bool sent = false;
    uint32_t response1 = 0;
    uint32_t spins = 0;
    out.status = runCommandBusy (SDHCI_MAKE_CMD (13, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                                 SDHCI_CMD_INDEX),
                                 kProbeRca << 16, sent, out.response, response1, spins);
    out.polls++;
    if (sent && !(out.status & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_INDEX)) &&
        (out.response & R1_READY_FOR_DATA) &&
        R1_CURRENT_STATE (out.response) == R1_STATE_TRAN) {
      out.ready = true;
      break;
    }
    const uint64_t gap = readCounter () + frequency / 500ULL;    // 2 ms
    while (readCounter () < gap) {
    }
  }
  out.present = read32 (SDHCI_PRESENT_STATE);
  return out;
}

// Round171: the two numbers bring-up reads from EXT_CSD and the block layer needs afterwards.
// gCacheSize is the card's own write cache in 512-byte units (zero means the card has none, and a
// flush of nothing is a success); gCacheOn records whether bring-up managed to switch it on, which
// is what makes clover_emmc_flush_cache() a real commit rather than a command sent to a card that
// was never caching. Both are declared above, beside the tee state round209's sink reads them from.
// How many commits actually completed this boot. CloverBlock prints it in its own reading line; a
// frame that shows writes and zero flushes is a frame where the cache was never on.
static volatile uint32_t gCacheFlushes = 0;

// Round171: flush the card's write cache, then put it back on.
//
// r167 switched CACHE_CTRL off for the one reason it had: this driver never issued a flush, so a
// cache left enabled was storage nobody committed, and every boot here ends in a hard reset. The
// cost of that choice showed up in the r168 and later frames as the card's own programming time
// landing inside every write -- the checkpoint flush r167 measured at 22.7 seconds had grown to
// 102 and 122 seconds by r170, because with the cache off each 512-byte block is programmed before
// the command that wrote it is answered. That is what an eMMC flush is *for*: the card batches the
// writes and commits them while the host is doing something else.
//
// JEDEC gives the flush no command of its own. Writing zero to EXT_CSD byte 33 commits the cache
// contents to media (R1b's DAT0 busy is the card doing it), which is why this function writes 0,
// waits for the card to come back to transfer, and writes 1 again. The two CMD13 waits are the
// proof; without them a switch the card is still working on looks like one that finished.
//
// Returns 1 when there was nothing to do or the commit completed, 0 when a step was refused. The
// caller is CloverBlock::synchronize(), which turns that into the filesystem's sync error, so a
// failure here has to be a number the frame can see rather than a silent success; the first one
// prints.
extern "C" uint32_t clover_emmc_flush_cache (void)
{
  if (!__atomic_load_n (&gBlockReady, __ATOMIC_ACQUIRE)) {
    return 0;
  }
  if (!__atomic_load_n (&gCacheSize, __ATOMIC_ACQUIRE) ||
      !__atomic_load_n (&gCacheOn, __ATOMIC_ACQUIRE)) {
    // Round192: this is now the expected answer on this card, so it gets a line rather than a silent
    // success. A frame that shows sync-out with no flush-in beside it says the barrier had nothing to
    // commit, not that the flush path went missing.
    static bool skippedReported = false;
    if (!skippedReported) {
      skippedReported = true;
      sdhciSay ("CloverSdhci r192: commit skipped size=%u on=%u (once)\n",
                __atomic_load_n (&gCacheSize, __ATOMIC_RELAXED),
                __atomic_load_n (&gCacheOn, __ATOMIC_RELAXED));
    }
    return 1;
  }
  uint64_t frequency = 0;
  uint64_t start = 0;
  uint32_t token = 0;
  if (!acquireBlockGuard (frequency, start, token)) {
    return 0;
  }
  // Round213: a commit is a request too. It lands in the stage table, so a flush that hangs in CMD6
  // or in the cache-to-media wait is named by the same watchdog that names a block request, instead
  // of leaving the stage at idle and the card inhibited.
  gFlightCurDir = 2;
  gFlightCurLba = 0;
  gFlightCurBlocks = 0;
  flightStage (kStageCommit);
  // Round190: the r189 frame's last call is fcntl(3, F_BARRIERFSYNC) and the kernel ring says nothing
  // for the 187 s after it. Whether that barrier reached this driver is the first question the next
  // frame can answer, and the answer has to be on the card before the risky part runs, not after it:
  // this record and whatever the kernel ring is holding are written out here, ahead of the first
  // CMD6, so a flush that never returns still leaves the fact that it started. `wait` is the guard
  // wait that just ended -- a barrier starved by an earlier request reads as a number, not silence.
  const uint64_t flushStart = readCounter ();
  sdhciSay ("CloverSdhci r190: flush-in flushes=%u on=%u size=%u wait=%llu ms\n",
            __atomic_load_n (&gCacheFlushes, __ATOMIC_RELAXED),
            __atomic_load_n (&gCacheOn, __ATOMIC_RELAXED),
            __atomic_load_n (&gCacheSize, __ATOMIC_RELAXED),
            static_cast<unsigned long long> (__atomic_load_n (&gGuardLastWaitMs, __ATOMIC_RELAXED)));
  teeKernelLog ();
  flushVerdictRing ();
  // Round191: five records, each naming the step that comes next, each drained to the card before
  // that step runs -- the tail is exactly what a hang takes away. A frame whose last record is
  // flush-c1 went quiet inside the commit's own wait; one that stops at flush-go never got a command
  // out at all; one that stops at flush-w1 hung while the card was writing the cache to media.
  // `cf` is the card's CACHE_FLUSH_TIMEOUT in 10 ms units, `bound` the millisecond bound this driver
  // derived from it, and `inh` whether the host was already inhibited before the first CMD6.
  const uint32_t boundMs = flushBoundMs ();
  const uint32_t presentBefore = read32 (SDHCI_PRESENT_STATE);
  teeSay ("CloverSdhci r191: flush-go cf=%u bound=%u ps=%08x inh=%u\n",
          __atomic_load_n (&gCacheFlushUnits, __ATOMIC_RELAXED), boundMs, presentBefore,
          (presentBefore & SDHCI_DATA_INHIBIT) ? 1u : 0u);
  flushVerdictRing ();
  uint32_t response = 0;
  uint32_t status = 0;
  uint32_t spins = 0;
  const uint64_t commitStart = readCounter ();
  flightStage (kStageCommit);
  const bool switchOff = switchExtCsd (kExtCsdCacheCtrl, 0, response, status, &spins);
  teeSay ("CloverSdhci r191: flush-c1 ok=%d st=%08x rsp=%08x spins=%u ms=%llu\n",
          switchOff ? 1 : 0, status, response, spins,
          static_cast<unsigned long long> ((readCounter () - commitStart) * 1000ULL / frequency));
  flushVerdictRing ();
  FlushSettle waitOff = FlushSettle ();
  if (switchOff) {
    flightStage (kStageCommit);
    waitOff = flushSettle (frequency, boundMs);
  }
  teeSay ("CloverSdhci r191: flush-w1 ready=%d to=%d busy=%llu ms polls=%u rsp=%08x st=%08x "
          "smp=%u mk=%u ps=%08x ms=%llu\n",
          waitOff.ready ? 1 : 0, waitOff.timedOut ? 1 : 0,
          static_cast<unsigned long long> (waitOff.busyMs), waitOff.polls, waitOff.response,
          waitOff.status, waitOff.samples, waitOff.marks, waitOff.present,
          static_cast<unsigned long long> ((readCounter () - commitStart) * 1000ULL / frequency));
  flushVerdictRing ();
  // Round213: the watchdog may have taken the host back while one of the waits above was stuck. If
  // it did, this commit is over -- sending the next CMD6 would fight the recovery that is running.
  if (guardLost (token)) {
    sdhciSay ("CloverSdhci r213: commit lost the host after c1/w1\n");
    releaseBlockGuard (token);
    return 0;
  }
  const bool flushed = switchOff && waitOff.ready;
  uint32_t backResponse = 0;
  uint32_t backStatus = 0;
  uint32_t backSpins = 0;
  const uint64_t backStart = readCounter ();
  flightStage (kStageCommit);
  const bool switchOn = switchExtCsd (kExtCsdCacheCtrl, 1, backResponse, backStatus, &backSpins);
  teeSay ("CloverSdhci r191: flush-c2 ok=%d st=%08x rsp=%08x spins=%u ms=%llu\n",
          switchOn ? 1 : 0, backStatus, backResponse, backSpins,
          static_cast<unsigned long long> ((readCounter () - backStart) * 1000ULL / frequency));
  flushVerdictRing ();
  FlushSettle waitOn = FlushSettle ();
  if (switchOn) {
    flightStage (kStageCommit);
    waitOn = flushSettle (frequency, boundMs);
  }
  teeSay ("CloverSdhci r191: flush-w2 ready=%d to=%d busy=%llu ms polls=%u rsp=%08x st=%08x "
          "smp=%u mk=%u ps=%08x ms=%llu\n",
          waitOn.ready ? 1 : 0, waitOn.timedOut ? 1 : 0,
          static_cast<unsigned long long> (waitOn.busyMs), waitOn.polls, waitOn.response,
          waitOn.status, waitOn.samples, waitOn.marks, waitOn.present,
          static_cast<unsigned long long> ((readCounter () - backStart) * 1000ULL / frequency));
  flushVerdictRing ();
  const bool reenabled = switchOn && waitOn.ready;
  const uint32_t polls = waitOff.polls;
  const uint32_t backPolls = waitOn.polls;
  if (!flushed || !reenabled) {
    static bool reported = false;
    if (!reported) {
      reported = true;
      sdhciSay ("CloverSdhci r171: flush failed flushed=%d back=%d rsp=%08x st=%08x "
             "polls=%u rsp2=%08x st2=%08x (once)\n",
             flushed ? 1 : 0, reenabled ? 1 : 0, response, status, polls,
             backResponse, backStatus);
    }
  } else {
    __atomic_add_fetch (&gCacheFlushes, 1u, __ATOMIC_RELAXED);
  }
  // Round190's second half: the pair of flush-in and flush-out is what makes the first record
  // readable. A frame with an entry and no exit names this commit as the call that never came back;
  // a frame with both says the card took the commit and the wall is above this driver.
  sdhciSay ("CloverSdhci r190: flush-out flushed=%d back=%d rsp=%08x st=%08x polls=%u "
            "rsp2=%08x st2=%08x polls2=%u ms=%llu flushes=%u\n",
            flushed ? 1 : 0, reenabled ? 1 : 0, response, status, polls, backResponse, backStatus,
            backPolls,
            static_cast<unsigned long long> ((readCounter () - flushStart) * 1000ULL / frequency),
            __atomic_load_n (&gCacheFlushes, __ATOMIC_RELAXED));
  // Round175: same as the read tail -- collect from the kernel log, then spend the guard on the card.
  if (!guardLost (token)) {
    flightStage (kStageTee);
    teeKernelLog ();
    flightStage (kStageFlush);
    flushVerdictRing ();   // Round174: same reason as the read tail
  }
  releaseBlockGuard (token);
  return (flushed && reenabled) ? 1u : 0u;
}

// What CloverBlock prints next to its own read and write counts, so one frame can say "the cache
// was on and committed N times" instead of leaving the reader to guess whether the switch took.
extern "C" void clover_emmc_cache_stats (uint32_t *size, uint32_t *on, uint32_t *flushes)
{
  if (size) {
    *size = __atomic_load_n (&gCacheSize, __ATOMIC_ACQUIRE);
  }
  if (on) {
    *on = __atomic_load_n (&gCacheOn, __ATOMIC_ACQUIRE);
  }
  if (flushes) {
    *flushes = __atomic_load_n (&gCacheFlushes, __ATOMIC_ACQUIRE);
  }
}

// Stop the card clock, program the SDCC1 root clock, bring the card clock back and wait for the
// new rate to stabilise. `mnd` selects the table's divided rows (400 kHz); false is the plain
// divider the 50 MHz row uses, whose M/N/D fields are cleared.
static bool programRootClock (bool mnd, uint32_t cfg, uint16_t &clockAfter, bool &updated,
                              bool &stable)
{
  write16 (SDHCI_CLOCK_CONTROL, 0x0000);
  uint32_t stopGuard = 0;
  while ((read16 (SDHCI_CLOCK_CONTROL) & (SDHCI_CLOCK_INT_EN | SDHCI_CLOCK_CARD_EN)) &&
         ++stopGuard < kCommandSpinLimit) {
  }
  gateOn (kGccAppsGate);
  gateOn (kGccAhbGate);
  const uint32_t m = mnd ? kGccM : 0u;
  const uint32_t n = mnd ? kGccN : 0u;
  const uint32_t d = mnd ? kGccD : 0u;
  writeGcc32 (kGccAppsM, (readGcc32 (kGccAppsM) & ~0xffu) | m);
  writeGcc32 (kGccAppsN, (readGcc32 (kGccAppsN) & ~0xffu) | n);
  writeGcc32 (kGccAppsD, (readGcc32 (kGccAppsD) & ~0xffu) | d);
  const uint32_t before = readGcc32 (kGccAppsCfg);
  writeGcc32 (kGccAppsCfg, (before & ~kGccCfgMask) | cfg);
  writeGcc32 (kGccAppsCmd, readGcc32 (kGccAppsCmd) | 1u);
  updated = false;
  uint32_t updateGuard = 0;
  while (updateGuard++ < kRcgUpdateLimit) {
    if (!(readGcc32 (kGccAppsCmd) & 1u)) {
      updated = true;
      break;
    }
  }
  write16 (SDHCI_CLOCK_CONTROL, SDHCI_CLOCK_INT_EN);
  stable = false;
  uint32_t stableGuard = 0;
  while (stableGuard++ < kCommandSpinLimit) {
    if (read16 (SDHCI_CLOCK_CONTROL) & SDHCI_CLOCK_INT_STABLE) {
      stable = true;
      break;
    }
  }
  if (stable) {
    write16 (SDHCI_CLOCK_CONTROL, SDHCI_CLOCK_INT_EN | SDHCI_CLOCK_CARD_EN);
  }
  clockAfter = read16 (SDHCI_CLOCK_CONTROL);
  return updated && stable;
}

// The whole step, called once after EXT_CSD has been read and the sector count is known. The
// verification read is LBA 1 of the window this boot is about to hand the block stack: its first
// two words are the GPT's "EFI PART", so a clock the host accepted but the card cannot actually
// drive is a refused verification, not a later filesystem error.
static bool fastMode (FastLink &link)
{
  link = FastLink{};
  if (!switchExtCsd (kExtCsdHsTiming, kHsTimingHighSpeed, link.hsResponse, link.hsStatus)) {
    return false;
  }
  if (!cardInTransfer (link.hsResponse, link.hsStatus, link.hsPolls)) {
    return false;
  }
  link.highSpeed = true;
  // The host is still on one lane while CMD6 asks the card to widen. Switching it here, after
  // the response, is the order Linux uses: the card's answer travels on DAT0 either way.
  if (switchExtCsd (kExtCsdBusWidth, kBusWidth8Bit, link.bwResponse, link.bwStatus) &&
      cardInTransfer (link.bwResponse, link.bwStatus, link.bwPolls)) {
    link.width8 = true;
  }
  link.hostControl = static_cast<uint8_t> (SDHCI_CTRL_HISPD |
                                           (link.width8 ? SDHCI_CTRL_8BITBUS : 0));
  write8 (SDHCI_HOST_CONTROL, link.hostControl);
  bool updated = false;
  bool stable = false;
  link.raised = programRootClock (false, kGccCfg50m, link.clockAfter, updated, stable);
  if (link.raised) {
    if (cardInTransfer (link.stateResponse, link.stateStatus, link.statePolls)) {
      uint32_t words = 0;
      uint32_t status = 0;
      if (readPioBlock (1, gReadBlock, words, status)) {
        link.header0 = gReadBlock[0];
        link.header1 = gReadBlock[1];
        link.verified = link.header0 == 0x20494645u && link.header1 == 0x54524150u;
      }
    }
    if (link.verified) {
      return true;
    }
  }
  // Fall back to the clock and timing every previous round proved. The card keeps HS_TIMING and
  // the lane count it already accepted -- both are legal at 400 kHz -- but the host drops HISPD,
  // because high-speed signalling below 26 MHz is out of spec.
  link.hostControl = static_cast<uint8_t> (link.width8 ? SDHCI_CTRL_8BITBUS : 0);
  write8 (SDHCI_HOST_CONTROL, link.hostControl);
  bool backUpdated = false;
  bool backStable = false;
  programRootClock (true, kGccCfg400k, link.clockAfter, backUpdated, backStable);
  link.raised = false;
  link.verified = false;
  return false;
}

// Round214: the card, not just the controller. r213 reset the command and data machines and sent
// CMD12, and the frame still shows the card going quiet at 418 s, so this walks the ladder the cold
// bring-up walked: clock off, machines reset, power cycled, 400 kHz, CMD0/CMD1, identification with
// the check off (round94's CMD2), CMD3/CMD7 at kProbeRca, then fastMode -- the EXT_CSD switches and
// the LBA 1 read whose "EFI PART" is the proof that the 50 MHz link every later request assumes is
// the link the card is actually on. Everything is counter-bounded and nothing sleeps: the beat calls
// this from the timer interrupt and must come back even when the card is gone for good.
static CardReinit reinitializeCard (void)
{
  CardReinit r = {};
  // The host the stuck request left behind: 8-bit, high-speed, whatever timeout it had. One lane,
  // legacy timing and the longest timeout are what identification is specified at.
  write8 (SDHCI_TIMEOUT_CONTROL, 0x0e);
  write8 (SDHCI_HOST_CONTROL, 0x00);
  write16 (SDHCI_HOST_CONTROL2, 0x0000);
  write16 (SDHCI_CLOCK_CONTROL, 0x0000);
  for (uint32_t guard = 0; guard < kCommandSpinLimit; guard++) {
    if (!(read16 (SDHCI_CLOCK_CONTROL) & (SDHCI_CLOCK_INT_EN | SDHCI_CLOCK_CARD_EN))) {
      r.clockStopped = true;
      break;
    }
  }
  if (!r.clockStopped) {
    return r;
  }
  const uint8_t mask = SDHCI_RESET_CMD | SDHCI_RESET_DATA;
  write8 (SDHCI_SOFTWARE_RESET, mask);
  for (uint32_t guard = 0; guard < kCommandSpinLimit; guard++) {
    if (!(read8 (SDHCI_SOFTWARE_RESET) & mask)) {
      r.resetDone = true;
      break;
    }
  }
  if (!r.resetDone) {
    return r;
  }
  // Power cycle. A card reloads its default state only from a supply it has seen drop; the
  // 1.8 V -> 0 -> 1.8 V order is coldBringUp's, and the spins stand in for the delays an interrupt
  // cannot take.
  write8 (SDHCI_POWER_CONTROL, 0x00);
  for (volatile uint32_t spin = 0; spin < 1000; spin = spin + 1) {
  }
  write8 (SDHCI_POWER_CONTROL, SDHCI_POWER_180);
  for (volatile uint32_t spin = 0; spin < 1000; spin = spin + 1) {
  }
  write8 (SDHCI_POWER_CONTROL, SDHCI_POWER_180 | SDHCI_POWER_ON);
  bool updated = false;
  bool stable = false;
  programRootClock (true, kGccCfg400k, r.clockAfter, updated, stable);
  r.clockStable = stable;
  r.powerAfter = read8 (SDHCI_POWER_CONTROL);
  if (!updated || !stable) {
    return r;
  }
  uint32_t rsp0 = 0, rsp1 = 0, spins = 0;
  (void)runCommand (SDHCI_MAKE_CMD (0, SDHCI_CMD_RESP_NONE), 0, r.sent0, rsp0, rsp1, spins);
  if (!r.sent0) {
    return r;
  }
  // CMD1 is the one command that says whether power reached the card at all. The retries are held
  // to twenty, not the cold path's two hundred: this runs with interrupts off on CPU0, and a card
  // that has not answered in twenty poll gaps is not about to.
  for (r.tries = 1; r.tries <= 20; r.tries++) {
    uint32_t ocr = 0, ocr1 = 0, ocrSpins = 0;
    const uint32_t status = runCommand (SDHCI_MAKE_CMD (1, SDHCI_CMD_RESP_SHORT), kEmmcOcr,
                                        r.sent1, ocr, ocr1, ocrSpins);
    if (!r.sent1) {
      break;
    }
    if (status & SDHCI_INT_TIMEOUT) {
      continue;
    }
    if (status & (SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX)) {
      break;
    }
    if (!(status & SDHCI_INT_RESPONSE)) {
      break;
    }
    if (ocr & 0x80000000u) {
      r.ready = true;
      break;
    }
    for (volatile uint32_t spin = 0; spin < kCmd1Gap; spin = spin + 1) {
    }
  }
  if (!r.ready) {
    return r;
  }
  // CMD2 with the check off is round94's X: the one CMD2 this host accepts. The card it leaves in
  // identification is exactly the state CMD3 is legal in.
  uint32_t cid[4] = { 0, 0, 0, 0 };
  uint32_t cidSpins = 0;
  const uint32_t cidStatus = runCommand4 (SDHCI_MAKE_CMD (2, SDHCI_CMD_RESP_LONG), 0,
                                          r.cmd2, cid, cidSpins);
  if (!r.cmd2 || !(cidStatus & SDHCI_INT_RESPONSE) ||
      (cidStatus & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX))) {
    r.cmd2 = false;
    return r;
  }
  // CMD3 SET_RELATIVE_ADDR at kProbeRca: the address every request above this one already uses, so
  // the block path needs no change when the card comes back.
  uint32_t addr0 = 0, addr1 = 0, addrSpins = 0;
  const uint32_t addrStatus = runCommand (SDHCI_MAKE_CMD (3, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                                          SDHCI_CMD_INDEX),
                                          kProbeRca << 16, r.cmd3, addr0, addr1, addrSpins);
  if (!r.cmd3 || !(addrStatus & SDHCI_INT_RESPONSE) ||
      (addrStatus & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX))) {
    r.cmd3 = false;
    return r;
  }
  uint32_t sel0 = 0, sel1 = 0, selSpins = 0;
  const uint32_t selStatus = runCommand (SDHCI_MAKE_CMD (7, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                                         SDHCI_CMD_INDEX),
                                         kProbeRca << 16, r.cmd7, sel0, sel1, selSpins);
  if (!r.cmd7 || !(selStatus & SDHCI_INT_RESPONSE) ||
      (selStatus & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX))) {
    r.cmd7 = false;
    return r;
  }
  FastLink link;
  (void)fastMode (link);
  r.hs = link.highSpeed;
  r.bw = link.width8;
  r.ok = link.verified;
  r.clockAfter = link.clockAfter;
  r.powerAfter = read8 (SDHCI_POWER_CONTROL);
  return r;
}

// Round215: the recovery a request runs on itself. r214's frame says the stop is the card going
// quiet under a request that still owns the ticket -- so this needs no steal, only the same ladder
// (stop the machines, power the card down and up, identify it, restore the 50 MHz link). The two
// things that keep a dead card from turning every request into a power cycle are the per-boot cap
// and the cooldown. On success the tee's backlog goes to the card here, because this context still
// owns the host and that write is the frame's own proof that the card came back.
static bool cardRecoverHeld (const char *why, uint32_t detail, bool force)
{
  uint64_t frequency = counterFrequency ();
  if (frequency < 1000000ULL || frequency > 1000000000ULL) {
    frequency = 19200000ULL;
  }
  const uint64_t now = readCounter ();
  const uint32_t tries = __atomic_load_n (&gFlightStuck, __ATOMIC_RELAXED);
  if (!force && tries >= kCardRecoverCap) {
    return false;
  }
  if (!force && gCardRecoverLastTick && now > gCardRecoverLastTick &&
      now - gCardRecoverLastTick < frequency * kCardRecoverCooldownMs / 1000ULL) {
    return false;
  }
  gCardRecoverLastTick = now;
  __atomic_add_fetch (&gFlightStuck, 1u, __ATOMIC_RELAXED);
  __atomic_store_n (&gFlightStuckStage, kStageReinit, __ATOMIC_RELAXED);
  // The stage is moved by hand: flightStage() would replace the location words with this context's
  // own, and those words are the last thing that names the request the recovery is about.
  __atomic_store_n (&gFlightStageSince, readCounter (), __ATOMIC_RELEASE);
  __atomic_store_n (&gFlightStage, kStageReinit, __ATOMIC_RELEASE);
  write8 (SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
  for (uint32_t guard = 0; guard < kCommandSpinLimit; guard++) {
    if (!(read8 (SDHCI_SOFTWARE_RESET) & (SDHCI_RESET_CMD | SDHCI_RESET_DATA))) {
      break;
    }
  }
  (void) sendStopCommand ();
  write32 (SDHCI_INT_STATUS, 0xffffffffu);
  (void) resetReadPath ();
  const CardReinit re = reinitializeCard ();
  sdhciSay ("CloverSdhci r215: card recover why=%s d=%u n=%u stop=%u rst=%u pwr=%u clk=%u "
            "clkst=%u t=%u cmd0=%u cmd1=%u ready=%u cmd2=%u cmd3=%u cmd7=%u hs=%u bw=%u ok=%u\n",
            why, detail, tries + 1,
            re.clockStopped ? 1u : 0u, re.resetDone ? 1u : 0u, re.powerAfter,
            re.clockAfter, re.clockStable ? 1u : 0u, re.tries,
            re.sent0 ? 1u : 0u, re.sent1 ? 1u : 0u, re.ready ? 1u : 0u,
            re.cmd2 ? 1u : 0u, re.cmd3 ? 1u : 0u, re.cmd7 ? 1u : 0u,
            re.hs ? 1u : 0u, re.bw ? 1u : 0u, re.ok ? 1u : 0u);
  if (re.ok) {
    __atomic_add_fetch (&gFlightRecovered, 1u, __ATOMIC_RELAXED);
    __atomic_store_n (&gIdleFailReason, kIdleOk, __ATOMIC_RELAXED);
    teeKernelLog ();
    flushVerdictRing ();
  }
  flightStage (kStageTail);
  return re.ok;
}

static void coldBringUp (bool fullReset, unsigned probeMode)
{
  clover_emmc_stage = 4;
  uint32_t gateBefore = readGcc32 (kGccAppsGate);
  uint32_t cfgBefore = readGcc32 (kGccAppsCfg);
  uint16_t clockBefore = read16 (SDHCI_CLOCK_CONTROL);

  // Round91 wrote RESET_ALL while the firmware's HS200 card clock was still running, and the bit
  // never self-cleared. Mode 6 stops the card clock first and then resets only the command and
  // data machines, which round90 already saw self-clear. `clr=` is the clock read back after the
  // stop; `rst=` is the reset mask's own self-clear.
  bool clockStopped = false;
  uint16_t clockCleared = clockBefore;
  if (!fullReset) {
    write16 (SDHCI_CLOCK_CONTROL, 0x0000);
    uint32_t stopGuard = 0;
    while (stopGuard++ < kCommandSpinLimit) {
      clockCleared = read16 (SDHCI_CLOCK_CONTROL);
      if (!(clockCleared & (SDHCI_CLOCK_INT_EN | SDHCI_CLOCK_CARD_EN))) {
        clockStopped = true;
        break;
      }
    }
  }

  bool resetDone = false;
  uint8_t resetMask = fullReset ? SDHCI_RESET_ALL : (SDHCI_RESET_CMD | SDHCI_RESET_DATA);
  write8 (SDHCI_SOFTWARE_RESET, resetMask);
  uint32_t resetGuard = 0;
  while (resetGuard++ < kCommandSpinLimit) {
    if (!(read8 (SDHCI_SOFTWARE_RESET) & resetMask)) {
      resetDone = true;
      break;
    }
  }
  clover_emmc_stage = 5;

  // M/N/D first, then CFG, then the self-clearing update bit, the order clk-rcg2.c uses. Linux
  // allows 500 us for the update; `up=0` below is the reading if this bound expires instead.
  // The root clock is only touched once the card clock is off: a glitch on a live card clock is
  // not something this driver gets to risk, so a failed `clr=` also skips this whole block.
  bool updated = false;
  uint32_t mAfter = 0, nAfter = 0, dAfter = 0;
  uint32_t cfgAfter = cfgBefore, gateAfter = gateBefore;
  if (fullReset || clockStopped) {
    gateOn (kGccAppsGate);
    gateOn (kGccAhbGate);
    writeGcc32 (kGccAppsM, (readGcc32 (kGccAppsM) & ~0xffu) | kGccM);
    writeGcc32 (kGccAppsN, (readGcc32 (kGccAppsN) & ~0xffu) | kGccN);
    writeGcc32 (kGccAppsD, (readGcc32 (kGccAppsD) & ~0xffu) | kGccD);
    uint32_t cfg = readGcc32 (kGccAppsCfg);
    writeGcc32 (kGccAppsCfg, (cfg & ~kGccCfgMask) | kGccCfg400k);
    writeGcc32 (kGccAppsCmd, readGcc32 (kGccAppsCmd) | 1u);
    uint32_t updateGuard = 0;
    while (updateGuard++ < kRcgUpdateLimit) {
      if (!(readGcc32 (kGccAppsCmd) & 1u)) {
        updated = true;
        break;
      }
    }
    mAfter = readGcc32 (kGccAppsM);
    nAfter = readGcc32 (kGccAppsN);
    dAfter = readGcc32 (kGccAppsD);
    cfgAfter = readGcc32 (kGccAppsCfg);
    gateAfter = readGcc32 (kGccAppsGate);
  }

  // Legacy timing, 1-bit bus, 1.8 V bus power, the longest timeout: EDK2's cold order, including
  // the power toggle (SdMmcPciHci.c:975) and TIMEOUT_CONTROL = 0x0e (:1191).
  bool clockStable = false;
  // Mode 13's reading: the FIFO selector before and after clearing CORE_FIFO_ALT_EN. Both stay
  // zero when the cold bring-up itself did not get this far.
  uint32_t vendor3Before13 = 0, vendor3After13 = 0;
  if (resetDone && (fullReset || clockStopped)) {
    write8 (SDHCI_TIMEOUT_CONTROL, 0x0e);
    write8 (SDHCI_HOST_CONTROL, 0x00);
    write16 (SDHCI_HOST_CONTROL2, 0x0000);
    // The vendor mux sits in the clock path, so it goes back to the free-running MCLK while both
    // clocks are still off: the first edge the card sees must not come from the HS200 DLL path.
    uint32_t vendor = read32 (kSdhciVendorSpec);
    vendor &= ~kHcMclkSelMask;
    vendor |= kHcMclkSelDefault;
    vendor &= ~kHcSelectInEn;
    vendor &= ~kHcSelectInMask;
    write32 (kSdhciVendorSpec, vendor);
    uint32_t vendor3 = read32 (kSdhciVendorSpec3);
    write32 (kSdhciVendorSpec3, vendor3 & ~kPwrSaveDll);
    if (probeMode >= 13) {
      // Round98 paced the FIFO reads and still got the same word 128 times with the level bit
      // stuck, so the buffer port itself never advanced. This one bit is the difference between
      // the port the firmware left behind and the one Linux initialises before every transfer:
      // clear it with the data machine already reset and no transfer running, then read it back
      // so a write that did not land cannot be mistaken for a FIFO that did not switch.
      vendor3Before13 = read32 (kSdhciVendorSpec3);
      write32 (kSdhciVendorSpec3, vendor3Before13 & ~kFifoAltEn);
      vendor3After13 = read32 (kSdhciVendorSpec3);
    }
    write8 (SDHCI_POWER_CONTROL, SDHCI_POWER_180);
    for (volatile uint32_t spin = 0; spin < 1000; spin = spin + 1) {
    }
    write8 (SDHCI_POWER_CONTROL, SDHCI_POWER_180 | SDHCI_POWER_ON);
    // No divider: the MSM controller takes its card clock straight from the root clock above.
    write16 (SDHCI_CLOCK_CONTROL, SDHCI_CLOCK_INT_EN);
    uint32_t stableGuard = 0;
    while (stableGuard++ < kCommandSpinLimit) {
      if (read16 (SDHCI_CLOCK_CONTROL) & SDHCI_CLOCK_INT_STABLE) {
        clockStable = true;
        break;
      }
    }
  }
  if (clockStable) {
    write16 (SDHCI_CLOCK_CONTROL, SDHCI_CLOCK_INT_EN | SDHCI_CLOCK_CARD_EN);
  }
  uint16_t clockAfter = read16 (SDHCI_CLOCK_CONTROL);
  uint8_t powerAfter = read8 (SDHCI_POWER_CONTROL);
  uint32_t vendorAfter = read32 (kSdhciVendorSpec);
  uint32_t vendor3After = read32 (kSdhciVendorSpec3);
  clover_emmc_stage = 6;

  bool sent0 = false, sent1 = false, sent2 = false, sent3 = false;
  uint32_t status0 = 0, status1 = 0, status2 = 0, status3 = 0;
  uint32_t rsp0 = 0, rsp1 = 0, rsp10 = 0, rsp11 = 0, rsp20 = 0, rsp21 = 0, rsp30 = 0, rsp31 = 0;
  uint32_t spins = 0, spins1 = 0, tries = 0, timeouts = 0;
  bool ready = false;
  const char *verdict1 = "skip";

  bool hostReady = resetDone && (fullReset || clockStopped) && clockStable && updated;
  if (hostReady) {
    status0 = runCommand (SDHCI_MAKE_CMD (0, SDHCI_CMD_RESP_NONE), 0, sent0, rsp0, rsp1, spins);
  }
  // CMD1 is retried on the controller's own timeout as well: 0x0e is 2^14 TMCLK = about 0.33 ms,
  // and a card that has just been reset is allowed to be slower than that. Round90 broke on the
  // first timeout, which turned "not yet" into "never".
  if (hostReady) {
    clover_emmc_stage = 7;
    for (tries = 1; tries <= kCmd1Retries; tries++) {
      status1 = runCommand (SDHCI_MAKE_CMD (1, SDHCI_CMD_RESP_SHORT), kEmmcOcr,
                            sent1, rsp10, rsp11, spins1);
      if (!sent1) {
        break;
      }
      if (status1 & SDHCI_INT_TIMEOUT) {
        timeouts++;
        continue;
      }
      if (status1 & (SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX)) {
        break;
      }
      if (!(status1 & SDHCI_INT_RESPONSE)) {
        break;
      }
      if (rsp10 & 0x80000000u) {
        ready = true;
        break;
      }
      // R3 arrived without bit 31: powered, not ready. EDK2 stalls 10 ms between these polls;
      // this spin is the substitute, and its length is a bound, not a promise.
      for (volatile uint32_t spin = 0; spin < kCmd1Gap; spin = spin + 1) {
      }
    }
    verdict1 = commandVerdict (status1, sent1);
    if (ready) {
      verdict1 = "ok";
    } else if (sent1 && (status1 & SDHCI_INT_RESPONSE) &&
               !(status1 & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                            SDHCI_INT_INDEX))) {
      verdict1 = "no pwr";
    }
  }

  // Mode 7's two CMD2 attempts. Filled only on that path; on every other mode these stay zero
  // and the report never reads them.
  bool sentA = false, sentB = false, sentC = false;
  uint32_t statusA = 0, statusB = 0, statusC = 0;
  uint32_t rspA[4] = { 0, 0, 0, 0 }, rspB[4] = { 0, 0, 0, 0 }, rspC[4] = { 0, 0, 0, 0 };
  uint32_t spinsA = 0, spinsB = 0, spinsC = 0;
  uint32_t presentAfterB = 0;
  bool resetA = false;
  CardRearm rearm = {};
  const char *verdictA = "skip", *verdictB = "skip", *verdictC = "skip";

  // Mode 8's readings: same two slots, opposite order. The CRC check moves to the slot after the
  // re-arm (Y) and the first slot runs without it (X), so the two variables round93 conflated are
  // separated by construction.
  bool sentX = false, sentY = false, sentU = false, sentS = false, sentD = false;
  uint32_t statusX = 0, statusY = 0, statusU = 0, statusS = 0, statusD = 0;
  uint32_t rspX[4] = { 0, 0, 0, 0 }, rspY[4] = { 0, 0, 0, 0 }, rspU[4] = { 0, 0, 0, 0 };
  uint32_t rspS[4] = { 0, 0, 0, 0 }, rspD0 = 0, rspD1 = 0, spinsD = 0;
  uint32_t spinsX = 0, spinsY = 0, spinsU = 0, spinsS = 0;
  bool rearmAttempted = false;
  bool cmd3Ok = false;
  CardRearm rearm8 = {};
  const char *verdictX = "skip", *verdictY = "skip", *verdictU = "skip";
  const char *verdictS = "skip", *verdictD = "skip";

  // Mode 9's chain: identification with the check off (the pair round94 proved works), then the
  // commands that need the address this port assigns, in the order the state machine allows.
  bool sentCid = false, sentAddr = false, sentCsd = false;
  bool sentState = false, sentSel = false, sentState2 = false;
  uint32_t cidStatus = 0, addrStatus = 0, csdStatus = 0;
  uint32_t stateStatus = 0, selStatus = 0, state2Status = 0;
  uint32_t cidRsp[4] = { 0, 0, 0, 0 }, csdRsp[4] = { 0, 0, 0, 0 };
  uint32_t addrRsp0 = 0, addrRsp1 = 0, stateRsp0 = 0, stateRsp1 = 0;
  uint32_t selRsp0 = 0, selRsp1 = 0, state2Rsp0 = 0, state2Rsp1 = 0;
  uint32_t spinsCid = 0, spinsAddr = 0, spinsCsd = 0;
  uint32_t spinsState = 0, spinsSel = 0, spinsState2 = 0;
  bool cmd2Ok9 = false, cmd3Ok9 = false, cmd9Ok = false, cmd13Ok = false, cmd7Ok = false;
  const char *verdictCid = "skip", *verdictAddr = "skip", *verdictCsd = "skip";
  const char *verdictState = "skip", *verdictSel = "skip", *verdictState2 = "skip";

  // Mode 10's chain: mode 9's path up to the selection, with the CID and CSD reads dropped because
  // round95 measured both and only the selection is needed here, and then the first data command
  // this port has ever sent.
  bool sentId10 = false, sentAddr10 = false, sentSel10 = false, sentState10 = false;
  bool sentBlk = false, sentRead = false, drained = false;
  uint32_t idStatus10 = 0, addrStatus10 = 0, selStatus10 = 0, stateStatus10 = 0;
  uint32_t blkStatus = 0, readStatus = 0;
  uint32_t idRsp10 = 0, idRsp1 = 0, addrRsp10 = 0, addrRsp1b10 = 0;
  uint32_t selRsp10 = 0, selRsp1b10 = 0, stateRsp10 = 0, stateRsp1b10 = 0;
  uint32_t spinsId10 = 0, spinsAddr10 = 0, spinsSel10 = 0, spinsState10 = 0, spinsBlk = 0;
  uint32_t blkRsp0 = 0, blkRsp1 = 0, hdr0 = 0, hdr1 = 0;
  bool idOk10 = false, addrOk10 = false, selOk10 = false, stateOk10 = false, blkOk = false;
  const char *verdictId10 = "skip", *verdictAddr10 = "skip", *verdictSel10 = "skip";
  const char *verdictState10 = "skip", *verdictBlk = "skip", *verdictRead = "skip";

  // Mode 11: mode 10's chain with the two host-side differences from Linux's MSM path repaired,
  // and every step of the data phase kept as a reading. The names carry the mode so a later
  // photograph cannot be read against the wrong round's line.
  bool sentId11 = false, sentAddr11 = false, sentSel11 = false, sentState11 = false;
  bool sentBlk11 = false, sentRead11 = false, drained11 = false, attemptedStop11 = false;
  bool sentStop11 = false, sentAfter11 = false;
  bool idOk11 = false, addrOk11 = false, selOk11 = false, stateOk11 = false, blkOk11 = false;
  uint32_t idStatus11 = 0, addrStatus11 = 0, selStatus11 = 0, stateStatus11 = 0;
  uint32_t blkStatus11 = 0, readStatus11 = 0, stopStatus11 = 0, afterStatus11 = 0;
  uint32_t idRsp11 = 0, idRsp11b = 0, addrRsp11 = 0, addrRsp11b = 0;
  uint32_t selRsp11 = 0, selRsp11b = 0, stateRsp11 = 0, stateRsp11b = 0;
  uint32_t blkRsp11 = 0, blkRsp11b = 0, rdRsp11 = 0;
  uint32_t stopRsp11 = 0, stopRsp11b = 0, afterRsp11 = 0, afterRsp11b = 0;
  uint32_t spinsId11 = 0, spinsAddr11 = 0, spinsSel11 = 0, spinsState11 = 0, spinsBlk11 = 0;
  uint32_t spinsCmd11 = 0, spinsBrr11 = 0, spinsStop11 = 0, spinsAfter11 = 0;
  uint32_t statusCmd11 = 0, statusEnd11 = 0;
  uint32_t psCmd11 = 0, psEnd11 = 0, readArg11 = 0, transferMode11 = 0;
  uint32_t hdr11a = 0, hdr11b = 0, drainSum11 = 0, drainWords11 = 0;
  const char *verdictId11 = "skip", *verdictAddr11 = "skip", *verdictSel11 = "skip";
  const char *verdictState11 = "skip", *verdictBlk11 = "skip", *verdictRead11 = "skip";
  const char *verdictStop11 = "skip", *verdictAfter11 = "skip";

  // Mode 12: mode 11's chain with the drain paced by the counter above. `nd` counts the words
  // that differed from the one before, so one real word followed by 127 repeats reads as
  // nd=0 rather than as a block; the gpt fields are where a header says the entries live.
  uint32_t distinct12 = 0, firstUs12 = 0, lastUs12 = 0;
  uint32_t word6_12 = 0, word18_12 = 0, word20_12 = 0, word21_12 = 0;

  // Mode 14: the header's entry-array pointer, count and size drive a walk over the entry blocks,
  // and the partitions named "logfs" and "userdata" are reported by their GPT index and extent.
  uint32_t entryEl14 = 0, entryEn14 = 0, entryEs14 = 0;
  uint32_t entryBlocks14 = 0, entryFail14 = 0, entryGot14 = 0;
  uint32_t entryFound14 = 0, entryFirst14 = 0, entryLast14 = 0;
  uint32_t entryUdFound = 0, entryUdFirst = 0, entryUdLast = 0;
  uint32_t entryLdFound = 0, entryLdFirst = 0, entryLdLast = 0;
  uint32_t entryType14 = 0, entryTypeB14 = 0;
  uint32_t entryFirstIdx15 = 0, entryFirstA15 = 0, entryFirstW15[3] = { 0, 0, 0 };
  bool entryOk14 = false, entryBad14 = false;

  // Mode 16: mode 15's scan, then the logfs partition's first block is written with a
  // self-identifying payload and read back. The write is gated on the extent round101 confirmed
  // against sysfs, so a parse mistake cannot put a write outside the partition this round is
  // about. `old`/`new` sums are plain 32-bit additions, enough to tell "the block changed" from
  // "the read path returned something else" without a checksum library.
  uint32_t writeWords16 = 0, writeStatus16 = 0;
  uint32_t readWords16 = 0, readStatus16 = 0;
  uint32_t equalWords16 = 0, oldSum16 = 0, oldWord16 = 0, newSum16 = 0;
  uint32_t stateRsp16 = 0, stateStatus16 = 0, statePolls16 = 0;
  bool writeOk16 = false, readOk16 = false, writeGated16 = false;

  // Mode 17: the report, once assembled, is the record. A header block at logfs start says where
  // the text is, how long it is and what it should sum to; the next block holds the text itself.
  // Both go out through the round102 write path and both are read back for a byte count, so the
  // photograph says whether the card kept the text, not whether a write was issued.
  uint32_t logLen17 = 0, logChecksum17 = 0;
  uint32_t logHeaderWords17 = 0, logTextWords17 = 0;
  uint32_t logHeaderStatus17 = 0, logTextStatus17 = 0;
  uint32_t logReadyRspA17 = 0, logReadyStatusA17 = 0, logReadyPollsA17 = 0;
  uint32_t logReadyRspB17 = 0, logReadyStatusB17 = 0, logReadyPollsB17 = 0;
  uint32_t logHeaderReadWords17 = 0, logTextReadWords17 = 0;
  uint32_t logHeaderReadStatus17 = 0, logTextReadStatus17 = 0;
  uint32_t logHeaderEqual17 = 0, logTextEqual17 = 0;
  uint32_t logRb0_17 = 0, logRb1_17 = 0, logRb3_17 = 0, logRb4_17 = 0;
  bool logGated17 = false, logHeaderOk17 = false, logTextOk17 = false;
  bool logHeaderReadOk17 = false, logTextReadOk17 = false;

  // Mode 18 reads a record written by an earlier boot and writes nothing, so what the line says
  // is what survived on the card rather than what this run just put there. The length comes from
  // the header, so it is clamped before it drives a loop, and the byte sum is recomputed from the
  // text block instead of trusted.
  uint32_t logHeaderWords18 = 0, logTextWords18 = 0;
  uint32_t logHeaderStatus18 = 0, logTextStatus18 = 0;
  uint32_t logMagic0_18 = 0, logMagic1_18 = 0, logLen18 = 0, logSumStored18 = 0;
  uint32_t logSum18 = 0;
  bool logHeaderOk18 = false, logTextOk18 = false, logCheck18 = false;

  // Mode 19 appends the report to the next free two-block slot instead of rewriting slot 0, so
  // one boot's record cannot destroy the previous one. The scan is the whole experiment: if it
  // stops at slot 1 because slot 0 already carries the magic, then the card kept what the last
  // boot wrote, and the write below lands somewhere new.
  uint32_t logSlot19 = 0, logScan19 = 0, logSlotLba19 = 0;
  uint32_t logFindWords19 = 0, logFindStatus19 = 0;
  bool logFindOk19 = true;
  uint32_t logLen19 = 0, logChecksum19 = 0;
  uint32_t logHeaderWords19 = 0, logTextWords19 = 0;
  uint32_t logHeaderStatus19 = 0, logTextStatus19 = 0;
  uint32_t logReadyRsp19 = 0, logReadyStatus19 = 0, logReadyPolls19 = 0;
  uint32_t logHeaderReadWords19 = 0, logTextReadWords19 = 0;
  uint32_t logHeaderReadStatus19 = 0, logTextReadStatus19 = 0;
  uint32_t logHeaderEqual19 = 0, logTextEqual19 = 0;
  bool logHeaderOk19 = false, logTextOk19 = false;
  bool logHeaderReadOk19 = false, logTextReadOk19 = false;

  // Mode 20 reads the newest record and writes nothing itself: the scan keeps the last slot whose
  // header carries the magic, then the text block is read and its byte sum recomputed. `kind` is
  // the writer's mode field (19 = report, 20 = panic-screen text), so a record that this boot did
  // not assemble is still identifiable. The two previews (start and end of the stored text) are
  // what separates a panic-screen log from a report: the latter carries readings the report does
  // not, and the panic path's own "@CloverSdhci.cpp:line" suffix.
  uint32_t logNewest20 = 0, logNewestFound20 = 0, logNewestLba20 = 0;
  uint32_t logNewestLen20 = 0, logNewestKind20 = 0, logNewestSum20 = 0, logNewestCalc20 = 0;
  uint32_t logFindWords20 = 0, logFindStatus20 = 0;
  uint32_t logNewestWords20 = 0, logNewestStatus20 = 0;
  bool logFindOk20 = true, logNewestTextOk20 = false, logNewestCheck20 = false;

  // Mode 21 adds one reading to the chain: EXT_CSD, read through the same paced PIO as the GPT
  // header. Its SEC_COUNT is what the block device publishes and what gates every read it serves.
  bool extOk21 = false;
  uint32_t extWords21 = 0, extStatus21 = 0, extSectors21 = 0;
  FastLink link21 = {};
  bool fastOk21 = false;

  bool cmd2Ok = false;
  if (probeMode == 0) {
    if (ready) {
      status2 = runCommand (SDHCI_MAKE_CMD (2, SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC), 0,
                            sent2, rsp20, rsp21, spins);
      cmd2Ok = sent2 && (status2 & SDHCI_INT_RESPONSE) &&
               !(status2 & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                            SDHCI_INT_INDEX));
    }
    if (cmd2Ok) {
      status3 = runCommand (SDHCI_MAKE_CMD (3, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                               SDHCI_CMD_INDEX),
                            kProbeRca << 16, sent3, rsp30, rsp31, spins);
    }
  } else if (probeMode == 7 && hostReady && ready) {
    // A: mode 6's command, unchanged, so its verdict is comparable with that photograph.
    statusA = runCommand4 (SDHCI_MAKE_CMD (2, SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC), 0,
                           sentA, rspA, spinsA);
    verdictA = probeVerdict (statusA, sentA);
    bool aOk = sentA && (statusA & SDHCI_INT_RESPONSE) &&
               !(statusA & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                            SDHCI_INT_INDEX));
    if (aOk) {
      statusC = runCommand4 (SDHCI_MAKE_CMD (3, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                             SDHCI_CMD_INDEX), kProbeRca << 16,
                             sentC, rspC, spinsC);
    } else {
      // B: the same CMD2 with the CRC check off. CMD2 is legal only in ready, and A moved the
      // card to identification even though the host rejected the answer, so the card is put
      // back through CMD0 and the CMD1 poll loop first.
      resetA = resetCommandCircuit ();
      bool rearmed = false;
      if (resetA) {
        rearm = rearmCard ();
        rearmed = rearm.sent0 && rearm.ready;
      }
      if (rearmed) {
        statusB = runCommand4 (SDHCI_MAKE_CMD (2, SDHCI_CMD_RESP_LONG), 0,
                               sentB, rspB, spinsB);
        presentAfterB = read32 (SDHCI_PRESENT_STATE);
        verdictB = probeVerdict (statusB, sentB);
      }
      // CMD3 is legal in identification state, which is where the card is whether or not the
      // host accepted the CID. Sending it after a rejected answer turns "the responses are
      // corrupt" and "only the 136-bit CRC check disagrees" into different photographs.
      if (sentB) {
        bool bOk = sentB && (statusB & SDHCI_INT_RESPONSE) &&
                   !(statusB & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                                SDHCI_INT_INDEX));
        bool canSend = bOk ? true : resetCommandCircuit ();
        if (canSend) {
          statusC = runCommand4 (SDHCI_MAKE_CMD (3, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                                 SDHCI_CMD_INDEX), kProbeRca << 16,
                                 sentC, rspC, spinsC);
        }
      }
    }
    if (sentC) {
      verdictC = probeVerdict (statusC, sentC);
    }
  } else if (probeMode == 8 && hostReady && ready) {
    // X: CMD2 with the CRC check off, in mode 7's A slot -- the first command after CMD1's
    // ready bit, with nothing in between. If this returns a CID, the check is not what made
    // round93's A fail there.
    statusX = runCommand4 (SDHCI_MAKE_CMD (2, SDHCI_CMD_RESP_LONG), 0,
                           sentX, rspX, spinsX);
    verdictX = probeVerdict (statusX, sentX);
    bool xOk = sentX && (statusX & SDHCI_INT_RESPONSE) &&
               !(statusX & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                            SDHCI_INT_INDEX));
    // Y must start in ready, and X left the card in identification either way, so CMD0 and the
    // CMD1 poll loop run again. The reset is only needed when X's command circuit saw an error.
    bool canRearm = xOk ? true : resetCommandCircuit ();
    bool rearmed = false;
    if (canRearm) {
      rearmAttempted = true;
      rearm8 = rearmCard ();
      rearmed = rearm8.sent0 && rearm8.ready;
    }
    if (rearmed) {
      // Y: the same CMD2 as X with the CRC check on, in mode 7's B slot. The re-arm is the one
      // thing round93's B had that its A did not.
      statusY = runCommand4 (SDHCI_MAKE_CMD (2, SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC), 0,
                             sentY, rspY, spinsY);
      verdictY = probeVerdict (statusY, sentY);
    }
    // CMD3 is legal in identification, which is where Y put the card whether or not the host
    // accepted the CID. A rejected answer leaves the state machine needing a reset first.
    bool yOk = sentY && (statusY & SDHCI_INT_RESPONSE) &&
               !(statusY & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                            SDHCI_INT_INDEX));
    bool canSend3 = sentY ? (yOk ? true : resetCommandCircuit ()) : false;
    if (canSend3) {
      statusU = runCommand4 (SDHCI_MAKE_CMD (3, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                             SDHCI_CMD_INDEX), kProbeRca << 16,
                             sentU, rspU, spinsU);
      verdictU = probeVerdict (statusU, sentU);
      cmd3Ok = sentU && (statusU & SDHCI_INT_RESPONSE) &&
               !(statusU & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                            SDHCI_INT_INDEX));
    }
    if (cmd3Ok) {
      // CMD9 SEND_CSD: the second 136-bit response, addressed by the RCA just assigned. If the
      // host's check accepts this one, the failure above is about the CID read specifically.
      statusS = runCommand4 (SDHCI_MAKE_CMD (9, SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC),
                             kProbeRca << 16, sentS, rspS, spinsS);
      verdictS = probeVerdict (statusS, sentS);
      // CMD13 at the same address: an R1 that names the RCA this port assigned and the state
      // CMD3 should have reached.
      statusD = runCommand (SDHCI_MAKE_CMD (13, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                                SDHCI_CMD_INDEX), kProbeRca << 16,
                            sentD, rspD0, rspD1, spinsD);
      verdictD = probeVerdict (statusD, sentD);
    }
  } else if (probeMode == 9 && hostReady && ready) {
    // CMD2 with the check off: round94's X, the one CMD2 this host accepts. It leaves the card in
    // identification, the state CMD3 is legal in. Taking the checked path is exactly what round94
    // showed cannot be recovered from, so it is not taken at all here.
    cidStatus = runCommand4 (SDHCI_MAKE_CMD (2, SDHCI_CMD_RESP_LONG), 0,
                             sentCid, cidRsp, spinsCid);
    verdictCid = probeVerdict (cidStatus, sentCid);
    cmd2Ok9 = sentCid && (cidStatus & SDHCI_INT_RESPONSE) &&
              !(cidStatus & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                             SDHCI_INT_INDEX));
    if (cmd2Ok9) {
      // CMD3 SET_RELATIVE_ADDR: the card takes RCA 1 and moves to stand-by. Its R1 reports the
      // state it is leaving (2), so `st=2` on this line is the expected value, not a failure.
      addrStatus = runCommand (SDHCI_MAKE_CMD (3, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                               SDHCI_CMD_INDEX), kProbeRca << 16,
                               sentAddr, addrRsp0, addrRsp1, spinsAddr);
      verdictAddr = probeVerdict (addrStatus, sentAddr);
      cmd3Ok9 = sentAddr && (addrStatus & SDHCI_INT_RESPONSE) &&
                !(addrStatus & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                                SDHCI_INT_INDEX));
    }
    if (cmd3Ok9) {
      // CMD13 at the address CMD3 just assigned: stand-by is state 3, and this is also the first
      // R1 asked after a 136-bit response, so it separates "the address works" from anything the
      // CSD read does next.
      stateStatus = runCommand (SDHCI_MAKE_CMD (13, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                                SDHCI_CMD_INDEX), kProbeRca << 16,
                                sentState, stateRsp0, stateRsp1, spinsState);
      verdictState = probeVerdict (stateStatus, sentState);
      cmd13Ok = sentState && (stateStatus & SDHCI_INT_RESPONSE) &&
                !(stateStatus & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                                 SDHCI_INT_INDEX));
      // CMD9 SEND_CSD with the check on: the second 136-bit response, and the one that says
      // whether the CRC gate rejects every R2 or only the CID. CMD9 is legal in stand-by, and the
      // card stays there whether or not the host accepts the frame.
      bool canAskCsd = cmd13Ok ? true : resetCommandCircuit ();
      if (canAskCsd) {
        csdStatus = runCommand4 (SDHCI_MAKE_CMD (9, SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC),
                                 kProbeRca << 16, sentCsd, csdRsp, spinsCsd);
        verdictCsd = probeVerdict (csdStatus, sentCsd);
        cmd9Ok = sentCsd && (csdStatus & SDHCI_INT_RESPONSE) &&
                 !(csdStatus & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                                SDHCI_INT_INDEX));
      }
      // CMD7 SELECT_CARD: stand-by -> transfer, the state every data command needs. Sent on any
      // path that still owns the address, because the selection is the goal, not a bonus of the
      // CSD read; a rejected 136-bit answer gets the same reset round94 showed is not enough, and
      // the photograph then says so.
      bool canSelect = cmd9Ok ? true : resetCommandCircuit ();
      if (canSelect) {
        selStatus = runCommand (SDHCI_MAKE_CMD (7, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                               SDHCI_CMD_INDEX), kProbeRca << 16,
                                sentSel, selRsp0, selRsp1, spinsSel);
        verdictSel = probeVerdict (selStatus, sentSel);
        cmd7Ok = sentSel && (selStatus & SDHCI_INT_RESPONSE) &&
                 !(selStatus & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                                SDHCI_INT_INDEX));
      }
      if (cmd7Ok) {
        // CMD13 again: state 4 is transfer, the reading round96's CMD16/CMD17 will be built on.
        state2Status = runCommand (SDHCI_MAKE_CMD (13, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                                   SDHCI_CMD_INDEX), kProbeRca << 16,
                                   sentState2, state2Rsp0, state2Rsp1, spinsState2);
        verdictState2 = probeVerdict (state2Status, sentState2);
      }
    }
  } else if (probeMode == 10 && hostReady && ready) {
    // Mode 9's path with the CSD read removed: identification with the check off, the address this
    // port assigns, the selection, and one R1 to confirm transfer. Round95 measured every one of
    // these, so the gate here is a regression check, not a question.
    idStatus10 = runCommand (SDHCI_MAKE_CMD (2, SDHCI_CMD_RESP_LONG), 0,
                             sentId10, idRsp10, idRsp1, spinsId10);
    verdictId10 = probeVerdict (idStatus10, sentId10);
    idOk10 = sentId10 && (idStatus10 & SDHCI_INT_RESPONSE) &&
             !(idStatus10 & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                             SDHCI_INT_INDEX));
    if (idOk10) {
      addrStatus10 = runCommand (SDHCI_MAKE_CMD (3, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                                 SDHCI_CMD_INDEX), kProbeRca << 16,
                                 sentAddr10, addrRsp10, addrRsp1b10, spinsAddr10);
      verdictAddr10 = probeVerdict (addrStatus10, sentAddr10);
      addrOk10 = sentAddr10 && (addrStatus10 & SDHCI_INT_RESPONSE) &&
                 !(addrStatus10 & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                                   SDHCI_INT_INDEX));
    }
    if (addrOk10) {
      selStatus10 = runCommand (SDHCI_MAKE_CMD (7, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                                SDHCI_CMD_INDEX), kProbeRca << 16,
                                sentSel10, selRsp10, selRsp1b10, spinsSel10);
      verdictSel10 = probeVerdict (selStatus10, sentSel10);
      selOk10 = sentSel10 && (selStatus10 & SDHCI_INT_RESPONSE) &&
                !(selStatus10 & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                                 SDHCI_INT_INDEX));
    }
    if (selOk10) {
      stateStatus10 = runCommand (SDHCI_MAKE_CMD (13, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                                   SDHCI_CMD_INDEX), kProbeRca << 16,
                                  sentState10, stateRsp10, stateRsp1b10, spinsState10);
      verdictState10 = probeVerdict (stateStatus10, sentState10);
      // State 4 is the precondition for every data command. Round95 read 0x00000900 here.
      stateOk10 = sentState10 && (stateStatus10 & SDHCI_INT_RESPONSE) &&
                  !(stateStatus10 & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                                     SDHCI_INT_INDEX)) &&
                  (((stateRsp10 >> 9) & 0xfu) == 4);
    }
    if (stateOk10) {
      // SET_BLOCKLEN: the card's block length is 512 after reset, so a rejection is not fatal --
      // the data command runs either way, and a failed CMD16 is printed as its own reading.
      blkStatus = runCommand (SDHCI_MAKE_CMD (16, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                              SDHCI_CMD_INDEX), 512,
                              sentBlk, blkRsp0, blkRsp1, spinsBlk);
      verdictBlk = probeVerdict (blkStatus, sentBlk);
      blkOk = sentBlk && (blkStatus & SDHCI_INT_RESPONSE) &&
              !(blkStatus & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                             SDHCI_INT_INDEX));
      bool canRead = blkOk ? true : resetCommandCircuit ();
      if (canRead && !waitCommandSlot ()) {
        // The command line is still busy: no command was written, and saying so is the reading.
        verdictRead = "busy";
      } else if (canRead) {
        // One 512-byte block, PIO (the transfer-mode DMA bit clear), read. The card is byte
        // addressed, so LBA 1 is byte 512. The card clock is 392 kHz here, so the data phase
        // lasts about 10 ms; every wait below is bounded because interrupts are off at this
        // scheduling point and a lost completion would otherwise be silence.
        write8 (SDHCI_TIMEOUT_CONTROL, 0x0e);
        write16 (SDHCI_BLOCK_SIZE, 512);
        write16 (SDHCI_BLOCK_COUNT, 1);
        write16 (SDHCI_TRANSFER_MODE, SDHCI_TRNS_READ);
        write32 (SDHCI_INT_STATUS, 0xffffffffu);
        write32 (SDHCI_ARGUMENT, 512);
        write16 (SDHCI_COMMAND, SDHCI_MAKE_CMD (17, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                                 SDHCI_CMD_INDEX | SDHCI_CMD_DATA));
        sentRead = true;
        uint32_t status = 0;
        for (uint32_t guard = 0;; guard++) {
          status = read32 (SDHCI_INT_STATUS);
          if (status & SDHCI_INT_CMD_MASK) {
            break;
          }
          if (guard >= kCommandSpinLimit) {
            break;
          }
        }
        bool cmdOk = (status & SDHCI_INT_RESPONSE) &&
                     !(status & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                                 SDHCI_INT_INDEX));
        if (cmdOk) {
          for (uint32_t guard = 0;; guard++) {
            status = read32 (SDHCI_INT_STATUS);
            if (status & (SDHCI_INT_DATA_AVAIL | SDHCI_INT_DATA_END |
                          SDHCI_INT_DATA_TIMEOUT | SDHCI_INT_DATA_CRC |
                          SDHCI_INT_DATA_END_BIT | SDHCI_INT_ADMA_ERROR)) {
              break;
            }
            if (guard >= kCommandSpinLimit) {
              break;
            }
          }
          // Buffer Read Ready means one whole block is in the FIFO, so the drain needs no
          // per-word state check; only the two words the report keeps are stored.
          if ((status & SDHCI_INT_DATA_AVAIL) &&
              !(status & (SDHCI_INT_DATA_TIMEOUT | SDHCI_INT_DATA_CRC |
                          SDHCI_INT_DATA_END_BIT))) {
            hdr0 = read32 (SDHCI_BUFFER);
            hdr1 = read32 (SDHCI_BUFFER);
            for (unsigned i = 2; i < 128; i++) {
              (void) read32 (SDHCI_BUFFER);
            }
            drained = true;
            for (uint32_t guard = 0;; guard++) {
              status = read32 (SDHCI_INT_STATUS);
              if (status & (SDHCI_INT_DATA_END | SDHCI_INT_DATA_TIMEOUT |
                            SDHCI_INT_DATA_CRC | SDHCI_INT_DATA_END_BIT |
                            SDHCI_INT_ADMA_ERROR)) {
                break;
              }
              if (guard >= kCommandSpinLimit) {
                break;
              }
            }
          }
        }
        readStatus = status;
        verdictRead = dataVerdict (readStatus, sentRead);
      }
    }
  } else if (probeMode >= 11 && hostReady && ready) {
    // Modes 11-13 share the chain; only the data phase differs (11 burst, 12 paced, 13 paced with
    // the SDHCI FIFO selected first). Round96 accepted CMD17 (RESPONSE|DATA_AVAIL) but never saw
    // DATA_END, and the FIFO returned zeros. Two host-side differences from Linux's path on this
    // exact controller are repaired here, and every step is kept as a reading so a remaining
    // failure names itself:
    //   1. OCR bit 30 is set (rsp=c0ff8080), so the card is sector addressed: mmc core marks
    //      MMC_STATE_BLOCKADDR from that bit and the block layer sends `blk_rq_pos` unchanged,
    //      shifting by 9 only for byte-addressed cards. LBA 1 is therefore argument 1 here.
    //   2. sdhci-msm does not set SDHCI_QUIRK2_SUPPORT_SINGLE, so Linux always sets BLK_CNT_EN
    //      for a one-block transfer (sdhci.c:1483). Round96 sent TRANSFER_MODE 0x10 alone; the
    //      MSM data machine may simply have no single-block mode to fall back on.
    idStatus11 = runCommand (SDHCI_MAKE_CMD (2, SDHCI_CMD_RESP_LONG), 0,
                             sentId11, idRsp11, idRsp11b, spinsId11);
    verdictId11 = probeVerdict (idStatus11, sentId11);
    idOk11 = sentId11 && (idStatus11 & SDHCI_INT_RESPONSE) &&
             !(idStatus11 & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                             SDHCI_INT_INDEX));
    if (idOk11) {
      addrStatus11 = runCommand (SDHCI_MAKE_CMD (3, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                                 SDHCI_CMD_INDEX), kProbeRca << 16,
                                 sentAddr11, addrRsp11, addrRsp11b, spinsAddr11);
      verdictAddr11 = probeVerdict (addrStatus11, sentAddr11);
      addrOk11 = sentAddr11 && (addrStatus11 & SDHCI_INT_RESPONSE) &&
                 !(addrStatus11 & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                                   SDHCI_INT_INDEX));
    }
    if (addrOk11) {
      selStatus11 = runCommand (SDHCI_MAKE_CMD (7, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                                SDHCI_CMD_INDEX), kProbeRca << 16,
                                sentSel11, selRsp11, selRsp11b, spinsSel11);
      verdictSel11 = probeVerdict (selStatus11, sentSel11);
      selOk11 = sentSel11 && (selStatus11 & SDHCI_INT_RESPONSE) &&
                !(selStatus11 & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                                 SDHCI_INT_INDEX));
    }
    if (selOk11) {
      stateStatus11 = runCommand (SDHCI_MAKE_CMD (13, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                                  SDHCI_CMD_INDEX), kProbeRca << 16,
                                  sentState11, stateRsp11, stateRsp11b, spinsState11);
      verdictState11 = probeVerdict (stateStatus11, sentState11);
      stateOk11 = sentState11 && (stateStatus11 & SDHCI_INT_RESPONSE) &&
                  !(stateStatus11 & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                                     SDHCI_INT_INDEX)) &&
                  (((stateRsp11 >> 9) & 0xfu) == 4);
    }
    if (stateOk11) {
      // SET_BLOCKLEN is still sent: the reset length is the card's own READ_BL_LEN (9), but
      // saying it makes a rejection visible instead of assumed.
      blkStatus11 = runCommand (SDHCI_MAKE_CMD (16, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                                SDHCI_CMD_INDEX), 512,
                                sentBlk11, blkRsp11, blkRsp11b, spinsBlk11);
      verdictBlk11 = probeVerdict (blkStatus11, sentBlk11);
      blkOk11 = sentBlk11 && (blkStatus11 & SDHCI_INT_RESPONSE) &&
                !(blkStatus11 & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                                 SDHCI_INT_INDEX));
      bool canRead11 = blkOk11 ? true : resetCommandCircuit ();
      if (canRead11 && !waitCommandSlot ()) {
        verdictRead11 = "busy";
      } else if (canRead11) {
        // Sector 1 for a sector-addressed card, byte 512 otherwise; decided from the OCR this
        // boot actually returned rather than from a constant.
        readArg11 = (rsp10 & 0x40000000u) ? 1u : 512u;
        transferMode11 = SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN;
        // Same order as Linux: timeout, size, count, argument, transfer mode, command. INT_STATUS
        // is cleared last so the command's own completion is the first bit seen.
        write8 (SDHCI_TIMEOUT_CONTROL, 0x0e);
        write16 (SDHCI_BLOCK_SIZE, 512);
        write16 (SDHCI_BLOCK_COUNT, 1);
        write32 (SDHCI_INT_STATUS, 0xffffffffu);
        write32 (SDHCI_ARGUMENT, readArg11);
        write16 (SDHCI_TRANSFER_MODE, static_cast<uint16_t> (transferMode11));
        write16 (SDHCI_COMMAND, SDHCI_MAKE_CMD (17, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                                 SDHCI_CMD_INDEX | SDHCI_CMD_DATA));
        sentRead11 = true;
        uint32_t status = 0;
        for (spinsCmd11 = 0;; spinsCmd11++) {
          status = read32 (SDHCI_INT_STATUS);
          if (status & SDHCI_INT_CMD_MASK) {
            break;
          }
          if (spinsCmd11 >= kCommandSpinLimit) {
            break;
          }
        }
        statusCmd11 = status;
        // Present state here says whether the data machine had even started when the response
        // arrived, and the response the card actually sent is what round96 never read.
        psCmd11 = read32 (SDHCI_PRESENT_STATE);
        rdRsp11 = read32 (SDHCI_RESPONSE);
        bool cmdOk11 = (status & SDHCI_INT_RESPONSE) &&
                       !(status & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                                   SDHCI_INT_INDEX));
        statusEnd11 = status;
        if (cmdOk11) {
          // The FIFO's own level bit is PRESENT_STATE[11], not INT_STATUS[DATA_AVAIL]: the status
          // bit is a sticky event that this driver never acknowledges, which is why round96's
          // `int=00000021` still carried it after two words had been read. Round97 gated on the
          // level bit and read 128 words as fast as the loop ran, and 127 of them were the first
          // word again: the level says "the buffer has data", not "another word has arrived".
          if (probeMode >= 12) {
            // Paced drain. At 400 kHz single-bit a 32-bit word takes ~80 us, so one read every
            // 160 us is slower than any arrival rate this link can produce; a buffer that fills
            // up simply stops the card clock until the next read. The deadline bounds the whole
            // block well below SDHCI's own data timeout.
            uint64_t frequency = counterFrequency ();
            if (frequency < 1000000ULL || frequency > 1000000000ULL) {
              frequency = 19200000ULL;   // the counter's measured rate on this board
            }
            const uint64_t gap = frequency / 6250ULL;        // 160 us
            const uint64_t deadline = frequency / 5ULL;      // 200 ms
            uint64_t start = readCounter ();
            uint64_t lastRead = start;
            bool haveWord = false;
            uint32_t previous = 0;
            for (spinsBrr11 = 0;; spinsBrr11++) {
              status = read32 (SDHCI_INT_STATUS);
              if (status & (SDHCI_INT_DATA_END | SDHCI_INT_DATA_TIMEOUT |
                            SDHCI_INT_DATA_CRC | SDHCI_INT_DATA_END_BIT |
                            SDHCI_INT_ADMA_ERROR)) {
                break;
              }
              uint64_t now = readCounter ();
              if (now - start > deadline) {
                break;
              }
              if (drainWords11 < kBlockWords && (!haveWord || now - lastRead >= gap) &&
                  (read32 (SDHCI_PRESENT_STATE) & SDHCI_DATA_AVAILABLE)) {
                uint32_t word = read32 (SDHCI_BUFFER);
                uint32_t index = drainWords11;
                if (index == 0) {
                  hdr11a = word;
                } else if (index == 1) {
                  hdr11b = word;
                }
                if (index >= 1 && word != previous) {
                  distinct12++;
                }
                if (index == 6) {
                  word6_12 = word;
                } else if (index == 18) {
                  word18_12 = word;
                } else if (index == 20) {
                  word20_12 = word;
                } else if (index == 21) {
                  word21_12 = word;
                }
                previous = word;
                drainSum11 += word;
                drainWords11++;
                lastRead = now;
                lastUs12 = static_cast<uint32_t> ((now - start) * 1000000ULL / frequency);
                if (!haveWord) {
                  haveWord = true;
                  firstUs12 = lastUs12;
                }
              }
              if (spinsBrr11 >= kCommandSpinLimit) {
                break;
              }
            }
          } else {
            for (spinsBrr11 = 0;; spinsBrr11++) {
              status = read32 (SDHCI_INT_STATUS);
              if (status & (SDHCI_INT_DATA_END | SDHCI_INT_DATA_TIMEOUT |
                            SDHCI_INT_DATA_CRC | SDHCI_INT_DATA_END_BIT |
                            SDHCI_INT_ADMA_ERROR)) {
                break;
              }
              if ((read32 (SDHCI_PRESENT_STATE) & SDHCI_DATA_AVAILABLE) &&
                  drainWords11 < kBlockWords) {
                uint32_t word = read32 (SDHCI_BUFFER);
                if (drainWords11 == 0) {
                  hdr11a = word;
                } else if (drainWords11 == 1) {
                  hdr11b = word;
                }
                drainSum11 += word;
                drainWords11++;
              }
              if (spinsBrr11 >= kCommandSpinLimit) {
                break;
              }
            }
            // DATA_END can set in the same read that the last word lands, so what is still in
            // the FIFO is taken here; the cap is one block either way.
            while (drainWords11 < kBlockWords &&
                   (read32 (SDHCI_PRESENT_STATE) & SDHCI_DATA_AVAILABLE)) {
              uint32_t word = read32 (SDHCI_BUFFER);
              if (drainWords11 == 0) {
                hdr11a = word;
              } else if (drainWords11 == 1) {
                hdr11b = word;
              }
              drainSum11 += word;
              drainWords11++;
            }
          }
          drained11 = drainWords11 > 0;
          statusEnd11 = status;
        }
        psEnd11 = read32 (SDHCI_PRESENT_STATE);
        readStatus11 = statusEnd11;
        verdictRead11 = dataVerdict (readStatus11, sentRead11);
        // A data phase that did not end is asked to stop in the card's own protocol, and then
        // CMD13 says which state the card is in afterwards. Both wait on the command line only:
        // Linux does that for the stop command, and CMD13 does not touch DAT at all.
        if (cmdOk11 && !(readStatus11 & SDHCI_INT_DATA_END)) {
          attemptedStop11 = true;
          stopStatus11 = runCommandBusy (SDHCI_MAKE_CMD (12, SDHCI_CMD_RESP_SHORT |
                                                          SDHCI_CMD_CRC | SDHCI_CMD_INDEX), 0,
                                          sentStop11, stopRsp11, stopRsp11b, spinsStop11);
          verdictStop11 = probeVerdict (stopStatus11, sentStop11);
          afterStatus11 = runCommandBusy (SDHCI_MAKE_CMD (13, SDHCI_CMD_RESP_SHORT |
                                                           SDHCI_CMD_CRC | SDHCI_CMD_INDEX),
                                           kProbeRca << 16, sentAfter11, afterRsp11,
                                           afterRsp11b, spinsAfter11);
          verdictAfter11 = probeVerdict (afterStatus11, sentAfter11);
        }
        // Mode 14: the header just read says where the entry array is (`el`), how many entries
        // (`en`) and how large each one is (`es`). Walk those blocks with the same paced PIO and
        // report the GPT index and extent of the partition whose name is exactly "logfs" -- the
        // one this board's firmware calls LOGFS and Linux exposes as mmcblk0p39. Its extent is
        // independently known from sysfs (start 12320768, 16384 sectors), so the reading can be
        // checked against a source that did not come through this driver.
        if (probeMode >= 14 && (readStatus11 & SDHCI_INT_DATA_END) && drained11) {
          entryEl14 = word18_12;
          entryEn14 = word20_12;
          entryEs14 = word21_12;
          if (entryEl14 == 0 || entryEn14 == 0 || entryEs14 < 128 || entryEs14 > 512 ||
              (entryEs14 % 4) != 0 || (512 % entryEs14) != 0) {
            entryBad14 = true;
          } else {
            const uint32_t perBlock = 512 / entryEs14;
            const uint32_t entryWords = entryEs14 / 4;
            uint32_t blocks = (entryEn14 + perBlock - 1) / perBlock;
            if (blocks > 16) {
              blocks = 16;
            }
            entryOk14 = true;
            for (uint32_t b = 0; b < blocks; b++) {
              uint32_t got = 0;
              uint32_t st = 0;
              bool blockOk = readPioBlock (entryEl14 + b, gEntryBlock, got, st);
              entryBlocks14 = b + 1;
              if (!blockOk) {
                entryOk14 = false;
                entryFail14 = st;
                entryGot14 = got;
                break;
              }
              for (uint32_t e = 0; e < perBlock && (b * perBlock + e) < entryEn14; e++) {
                const uint32_t *entry = &gEntryBlock[e * entryWords];
                // Name is UTF-16LE at byte 56: each character is two bytes, so "logfs" plus
                // its terminator spans words 14..16 as 0x006f006c, 0x00660067, 0x00000073.
                // Round100 compared the first two against 0x6f6c/0x6667 -- the same letters
                // with the zero bytes squeezed out -- read all 16 entry blocks cleanly and
                // matched nothing. Matching the terminator too keeps "logfsx" out.
                const uint32_t index = b * perBlock + e + 1;
                if (probeMode >= 15 && entryFirstIdx15 == 0 && entryWords >= 17 &&
                    (entry[0] | entry[1] | entry[2] | entry[3]) != 0) {
                  // The first used entry, for calibrating the encoding if the name still does
                  // not match: its raw name words and start LBA say where and how names are
                  // stored independently of the one string this round is looking for.
                  entryFirstIdx15 = index;
                  entryFirstA15 = entry[8];
                  entryFirstW15[0] = entry[14];
                  entryFirstW15[1] = entry[15];
                  entryFirstW15[2] = entry[16];
                }
                if (entryWords >= 17 && entry[14] == 0x006f006cu && entry[15] == 0x00660067u &&
                    entry[16] == 0x00000073u) {
                  entryFound14 = index;   // 1-based, the way the kernel names it
                  entryFirst14 = entry[8];
                  entryLast14 = entry[10];
                  entryType14 = entry[0];
                  entryTypeB14 = ((entry[1] & 0xffffu) << 16) | (entry[2] & 0xffffu);
                }
                // "userdata", the same UTF-16LE encoding: eight characters span words 14..17 and
                // word 18 is the terminator. Round128 needs both names, and logfs (entry 39) comes
                // first, so the loop no longer stops at the one it used to find.
                if (entryWords >= 19 && entry[14] == 0x00730075u && entry[15] == 0x00720065u &&
                    entry[16] == 0x00610064u && entry[17] == 0x00610074u && entry[18] == 0) {
                  entryUdFound = index;
                  entryUdFirst = entry[8];
                  entryUdLast = entry[10];
                }
                // "logdump", round185's log window: seven characters span words 14..17, with 'p'
                // alone in word 17 and the terminator landing in word 18. It is the third name this
                // walk needs, and it is entry 56 on this card, inside the 16 blocks the walk is
                // allowed to read (entryEs14 = 128 gives four entries per block, so 64 entries).
                if (entryWords >= 19 && entry[14] == 0x006f006cu && entry[15] == 0x00640067u &&
                    entry[16] == 0x006d0075u && entry[17] == 0x00000070u && entry[18] == 0) {
                  entryLdFound = index;
                  entryLdFirst = entry[8];
                  entryLdLast = entry[10];
                }
              }
              if (entryFound14 && entryUdFound && entryLdFound) {
                break;
              }
            }
          }
          // The window for drivers/CloverBlock round127, taken from the entry this boot read. The
          // two constants decide nothing about the geometry: they only refuse a card whose entry 39
          // is not the extent every logfs round above has been reading, which would make the media
          // and the partition table disagree about where the filesystem starts.
          if (entryFound14 != 0 && entryFirst14 == kLogfsFirstLba &&
              entryLast14 == kLogfsLastLba) {
            __atomic_store_n (&gBlockWindowFirst, entryFirst14, __ATOMIC_RELEASE);
            __atomic_store_n (&gBlockWindowSectors, entryLast14 - entryFirst14 + 1,
                              __ATOMIC_RELEASE);
          }
          if (entryUdFound != 0 && entryUdFirst == kUserDataFirstLba &&
              entryUdLast == kUserDataLastLba) {
            __atomic_store_n (&gBlockUserDataFirst, entryUdFirst, __ATOMIC_RELEASE);
            __atomic_store_n (&gBlockUserDataSectors, entryUdLast - entryUdFirst + 1,
                              __ATOMIC_RELEASE);
          }
          // Round185: the log window, and the arming that used to live inside the mode-20 report.
          // It was unreachable there for two independent reasons and both are gone. (1) The block it
          // sat in ran only when probeMode == 20, and the boot path has been mode 21 since
          // round137 -- a normal boot walked the GPT, published the root window and never reached
          // the code that arms the rings. (2) It armed only after the boot read an existing record
          // out of the window, which a partition that has never been written to cannot produce, so
          // even a mode-20 boot after round127's flash stayed disarmed forever. What has to be true
          // now is only that this card's own GPT names logdump at the extent measured from sysfs.
          // Nothing about the partition's contents is a precondition: the first boot writes the
          // first record, and every later one reads what it finds.
          if (entryLdFound != 0 && entryLdFirst == kLogDumpFirstLba &&
              entryLdLast == kLogDumpLastLba) {
            const uint32_t logSectors = entryLdLast - entryLdFirst + 1;
            __atomic_store_n (&gLogWindowFirst, entryLdFirst, __ATOMIC_RELEASE);
            __atomic_store_n (&gLogWindowSectors, logSectors, __ATOMIC_RELEASE);
            // The tee ring is the deepest user: 512 records at two blocks each, from sector 192.
            // Round212 adds the boot-id record one sector past it, so the window has to cover that
            // too: a card two sectors short would take the id over the partition's end.
            const uint32_t needed = kBootRecordOffset + 1;
            if (logSectors >= needed) {
              gPanicLogFirst = entryLdFirst;
              gPanicLogReady = 1;
              gVerdictFirst = entryLdFirst;
              gVerdictReady = 1;
              gTeeReady = 1;
              // Round212: claimed before the first line this boot writes, and read back from the
              // card, so no two boots share it and a record left in a ring slot by an earlier boot
              // cannot pass for this one's.
              gBootId = bootIdClaim (entryLdFirst + kBootRecordOffset);
              // Said out loud because it is the precondition of every reading after it: a dd that
              // comes back with no verdict records has to be able to tell "never armed" from
              // "armed and then never flushed by a request that finished".
              sdhciSay ("CloverSdhci r185: log window armed from GPT first=%u sectors=%u report=%u driver=%u kernel=%u\n",
                        entryLdFirst, logSectors, entryLdFirst,
                        entryLdFirst + kRingOffset[kRingDriver],
                        entryLdFirst + kRingOffset[kRingKernel]);
              // The three lines every earlier round's reader looks for, said from here rather than
              // from the report they used to sit in: the ring addresses they name are now logdump's,
              // and they are armed on a mode-21 boot for the first time.
              sdhciSay ("CloverSdhci r174: verdict ring armed lba=%u slots=%u\n",
                        gVerdictFirst + kRingOffset[kRingDriver], kRingCardSlots[kRingDriver]);
              sdhciSay ("CloverSdhci r176: kernel ring armed lba=%u slots=%u\n",
                        gVerdictFirst + kRingOffset[kRingKernel], kRingCardSlots[kRingKernel]);
              // gTeeIndex stays at zero on purpose: the lines this port needs most -- the
              // shared-region probes, the root mount verdict -- were printed before now, and they
              // are still in the buffer. bufx= says how much history the first passes replay.
              sdhciSay ("CloverSdhci r175: tee armed bufc=%p size=%d bufx=%d\n",
                        msgbufp ? msgbufp->msg_bufc : nullptr,
                        msgbufp ? msgbufp->msg_size : 0,
                        msgbufp ? msgbufp->msg_bufx : -1);
              // Round212: the number every record of this boot carries, so the host can tell this
              // boot's drain line from one an earlier boot left in the same slot -- the ambiguity
              // round211's frame ended on.
              sdhciSay ("CloverSdhci r212: boot id=%u record=%u probe slots=%u text=%u\n",
                        gBootId, entryLdFirst + kBootRecordOffset, kProbeSlots, kProbeTextBytes);
              // Round198. Round197's frame reset itself at 238 s, mid-CloverBlock-write, and left
              // no panic record: the reset never reached the screen that marks it. So the mark is
              // written here instead, before anything that can reset the board runs. The card is
              // selected and no block driver exists yet -- this is the same state the panic sink
              // writes in -- and clover-unattended is what gates both halves. TWRP clears misc
              // when it boots, so a frame read from recovery leaves the next boot a clean slot.
              const unsigned int bcb = panicMarkRecovery ();
              sdhciSay ("CloverSdhci r198: recovery BCB pre-armed rc=%u written=%u\n",
                        bcb, gBcbWritten);
              // Round210: the flight region sits in the gap the rings leave (sectors 32..63), and
              // only a boot that asked for the unattended deadline pays for it. The entry records
              // start with the first block request after this line; a beat starts on CPU0's next
              // four-second boundary.
              unsigned unattendedSeconds = 0;
              if (PE_parse_boot_argn ("clover-unattended", &unattendedSeconds,
                                      sizeof (unattendedSeconds)) && unattendedSeconds) {
                gFlightStart = readCounter ();
                __atomic_store_n (&gFlightArmed, 1u, __ATOMIC_RELEASE);
                sdhciSay ("CloverSdhci r210: flight armed entry=%u slots=%u beat=%u slots=%u "
                          "window=%u..%u\n",
                          entryLdFirst + kFlightEntryOffset, kFlightEntrySlots,
                          entryLdFirst + kFlightBeatOffset, kFlightBeatSlots,
                          entryLdFirst + kFlightEntryOffset,
                          entryLdFirst + kFlightBeatOffset + kFlightBeatSlots - 1);
              }
            } else {
              sdhciSay ("CloverSdhci r185: logdump entry too small, rings stay disarmed first=%u sectors=%u needed=%u\n",
                        entryLdFirst, logSectors, needed);
            }
          } else {
            sdhciSay ("CloverSdhci r185: no logdump entry at %u..%u found=%u first=%u last=%u, rings stay disarmed\n",
                      kLogDumpFirstLba, kLogDumpLastLba, entryLdFound, entryLdFirst, entryLdLast);
          }
          if (probeMode == 16 && entryFound14 != 0) {
            // Writing into logfs is the step this whole chain has been for: the block carries
            // its own identity and a checksum, so a stale block, an erased block and a correct
            // write are three different photographs.
            writeGated16 = !(entryFirst14 == 12320768u && entryLast14 == 12337151u);
            if (!writeGated16) {
              // Read the block once before writing: if the read-back later differs from the
              // payload, this says whether the block held something else or the read is broken.
              uint32_t oldWords = 0, oldStatus = 0;
              bool oldOk = readPioBlock (entryFirst14, gReadBlock, oldWords, oldStatus);
              if (oldOk) {
                for (uint32_t i = 0; i < kBlockWords; i++) {
                  oldSum16 += gReadBlock[i];
                }
                oldWord16 = gReadBlock[0];
              }
              // "UXNU" in memory order, the LBA, the mode, a checksum over the rest, and a
              // sequence that is neither all zeros nor all ones in between.
              for (uint32_t i = 0; i < kBlockWords; i++) {
                gWriteBlock[i] = 0x554e5855u + i * 0x01000193u;
              }
              gWriteBlock[0] = 0x554e5855u;
              gWriteBlock[1] = entryFirst14;
              gWriteBlock[2] = 16;
              uint32_t checksum = 0;
              for (uint32_t i = 4; i < kBlockWords; i++) {
                checksum += gWriteBlock[i];
              }
              gWriteBlock[3] = checksum;
              writeOk16 = writePioBlock (entryFirst14, gWriteBlock, writeWords16, writeStatus16);
              waitCardReady (stateRsp16, stateStatus16, statePolls16);
              readOk16 = readPioBlock (entryFirst14, gReadBlock, readWords16, readStatus16);
              if (readOk16) {
                for (uint32_t i = 0; i < kBlockWords; i++) {
                  if (gReadBlock[i] == gWriteBlock[i]) {
                    equalWords16++;
                  }
                  newSum16 += gReadBlock[i];
                }
              }
            }
          }
        }
        // Mode 21: the sector count the block device publishes is the card's own, read with the
        // same paced PIO as the GPT header. SEC_COUNT lives at byte 212, i.e. word 53 of the
        // 512-byte EXT_CSD block. The count is gated on the logfs extent: a card that does not
        // reach the partition this port has been reading is not a geometry to publish, and
        // without the gate the block device would describe a card the next read disagrees with.
        if (probeMode == 21 && (readStatus11 & SDHCI_INT_DATA_END) && drained11) {
          extOk21 = readDataBlock (MMC_SEND_EXT_CSD, 0, gExtCsd, extWords21, extStatus21);
          extSectors21 = gExtCsd[53];
          if (!extOk21 || extSectors21 <= kLogfsLastLba) {
            extOk21 = false;
          } else {
            gBlockSectors = extSectors21;
            // Round167 switched the card's write cache off because nothing in this driver issued a
            // flush, and every boot here ends in a hard reset: an uncommitted cache was storage
            // nobody owned. Round171 paid for the flush instead of giving up the cache, because the
            // reading that justified r167's caution turned out to be the cost of it -- the checkpoint
            // flush that measured 22.7 s in the r167 frame measured 102 s and 122 s in r170's, each
            // 512-byte block being programmed before its own write command is answered.
            //
            // Round192 gives the cache back to the card, and the reason is a frame rather than a
            // preference: r191 turned the cache on here (ctrl=0 came from the card itself) and the
            // first commit never returned. That frame's last driver record is flush-go, and nothing
            // after it could reach the card either -- every r191 stage record is mirrored through a
            // block write, and a card still holding DAT0 for the commit refuses those too. So the
            // commit is not only unbounded, it is unobservable. The write path already waits for
            // READY_FOR_DATA in TRAN after every chunk, which is the same rule r167 ran on and the
            // same 102 s r170 measured; paying that again is cheaper than a barrier that never
            // returns. A card that ships with its cache already on still commits, so the flush path
            // stays live rather than dead.
            const uint32_t cacheSize = (gExtCsd[62] >> 8) | (gExtCsd[63] << 24);
            const uint32_t cacheCtrlBefore = (gExtCsd[8] >> 8) & 0xffu;
            // Round191: byte 246 is CACHE_FLUSH_TIMEOUT, the /10 ms the card may take to commit its
            // cache. The flush's wait is set from this number instead of from a guess, so the r171
            // line now carries it.
            const uint32_t cacheFlushUnits = (gExtCsd[61] >> 16) & 0xffu;
            uint32_t cacheRsp = 0, cacheStatus = 0;
            const bool cacheOn = cacheSize != 0 && cacheCtrlBefore != 0;
            __atomic_store_n (&gCacheSize, cacheSize, __ATOMIC_RELEASE);
            __atomic_store_n (&gCacheOn, cacheOn ? 1u : 0u, __ATOMIC_RELEASE);
            __atomic_store_n (&gCacheFlushUnits, cacheFlushUnits, __ATOMIC_RELEASE);
            sdhciSay ("CloverSdhci r171: write cache size=%u ctrl=%u on=%d rsp=%08x st=%08x cf=%u\n",
                   cacheSize, cacheCtrlBefore, cacheOn ? 1 : 0, cacheRsp, cacheStatus,
                   cacheFlushUnits);
            clover_emmc_link[14] = cacheSize;
            // Bit 0 of this slot used to spell "the cache is off"; r171 flips the switch, so it now
            // spells "the cache is on". The card's own previous setting stays in [15:8].
            clover_emmc_link[15] = (cacheCtrlBefore << 8) | (cacheOn ? 1u : 0u);
            // Round159, after the geometry is known and before the device is handed up: switch
            // the link to high-speed 8-bit 50 MHz and prove it with LBA 1. A failure here is a
            // reading, not a boot failure -- the same card then serves the same reads at the
            // 400 kHz 1-bit the previous rounds measured, only slower.
            fastOk21 = fastMode (link21);
            clover_emmc_link[0] = link21.hsStatus;
            clover_emmc_link[1] = link21.hsResponse;
            clover_emmc_link[2] = link21.hsPolls;
            clover_emmc_link[3] = link21.bwStatus;
            clover_emmc_link[4] = link21.bwResponse;
            clover_emmc_link[5] = link21.bwPolls;
            clover_emmc_link[6] = (link21.highSpeed ? 1u : 0u) | (link21.width8 ? 2u : 0u) |
                                  (link21.raised ? 4u : 0u) | (link21.verified ? 8u : 0u);
            clover_emmc_link[7] = link21.clockAfter;
            clover_emmc_link[8] = link21.hostControl;
            clover_emmc_link[9] = link21.header0;
            clover_emmc_link[10] = link21.header1;
            clover_emmc_link[11] = link21.stateStatus;
            clover_emmc_link[12] = link21.stateResponse;
            clover_emmc_link[13] = link21.statePolls;
            sdhciSay ("CloverSdhci r159: link hs=%d/%x bw=%d/%x clk=%x hc=%x hdr=%x/%x ok=%d\n",
                   link21.highSpeed ? 1 : 0, link21.hsStatus,
                   link21.width8 ? 1 : 0, link21.bwStatus,
                   link21.clockAfter, link21.hostControl,
                   link21.header0, link21.header1, fastOk21 ? 1 : 0);
            // Round213: the host is shared through an atomic ticket, not an IOLock. The ticket needs
            // no allocation and cannot be lost with its holder, so arming is just publishing it;
            // a block device that cannot serialize its callers is worse than no block device.
            __atomic_store_n (&gHostArmed, 1u, __ATOMIC_RELEASE);
            __atomic_store_n (&gBlockReady, 1, __ATOMIC_RELEASE);
            sdhciSay ("CloverSdhci r213: host ticket armed, wait=%llums stuck=%llums\n",
                   static_cast<unsigned long long> (kGuardWaitMs),
                   static_cast<unsigned long long> (kGuardStuckMs));
          }
        }
      }
    }
  }

  clover_emmc_observation[0] = cfgBefore;
  clover_emmc_observation[1] = cfgAfter;
  clover_emmc_observation[2] = readGcc32 (kGccAppsGate);
  clover_emmc_observation[3] = gateBefore;
  clover_emmc_observation[4] = readGcc32 (kGccAhbGate);
  clover_emmc_observation[5] = vendorAfter;
  clover_emmc_observation[6] = vendor3After;
  clover_emmc_observation[7] = status0;
  clover_emmc_observation[8] = status1;
  clover_emmc_observation[9] = rsp10;
  clover_emmc_observation[10] = status2;
  clover_emmc_observation[11] = rsp20;
  clover_emmc_observation[12] = status3;
  clover_emmc_observation[13] = rsp30;
  clover_emmc_observation[14] = (static_cast<uint64_t> (tries) << 16) | timeouts;
  clover_emmc_observation[15] = (static_cast<uint64_t> (clockBefore) << 32) |
                                (static_cast<uint64_t> (clockCleared) << 16) | clockAfter;
  if (probeMode == 7) {
    clover_emmc_observation[7] = statusA;
    clover_emmc_observation[8] = rspA[0];
    clover_emmc_observation[9] = rspA[3];
    clover_emmc_observation[10] = statusB;
    clover_emmc_observation[11] = rspB[0];
    clover_emmc_observation[12] = statusC;
    clover_emmc_observation[13] = rspC[0];
    // The re-arm's own readings, which otherwise only exist as a report line.
    clover_emmc_observation[14] = (static_cast<uint64_t> (presentAfterB) << 32) |
                                  (static_cast<uint64_t> (rearm.tries) << 16) | rearm.timeouts;
  } else if (probeMode == 8) {
    clover_emmc_observation[7] = statusX;
    clover_emmc_observation[8] = rspX[0];
    clover_emmc_observation[9] = rspX[3];
    clover_emmc_observation[10] = statusY;
    clover_emmc_observation[11] = rspY[0];
    clover_emmc_observation[12] = statusU;
    clover_emmc_observation[13] = rspU[0];
    // CMD9's status in the high half, CMD13's in the low: the two checks the CRC-on path gets
    // after the address is owned.
    clover_emmc_observation[14] = (static_cast<uint64_t> (statusS) << 32) | statusD;
    clover_emmc_observation[15] = (static_cast<uint64_t> (rspS[0]) << 32) | rspD0;
  } else if (probeMode == 9) {
    clover_emmc_observation[7] = cidStatus;
    clover_emmc_observation[8] = cidRsp[0];
    clover_emmc_observation[9] = cidRsp[3];
    clover_emmc_observation[10] = addrStatus;
    clover_emmc_observation[11] = addrRsp0;
    clover_emmc_observation[12] = csdStatus;
    clover_emmc_observation[13] = csdRsp[0];
    // CMD13's status beside CMD7's, then the second CMD13's status beside its R1.
    clover_emmc_observation[14] = (static_cast<uint64_t> (stateStatus) << 32) | selStatus;
    clover_emmc_observation[15] = (static_cast<uint64_t> (state2Status) << 32) | state2Rsp0;
  } else if (probeMode == 10) {
    // The chain's statuses in order, then the two header words the data path returned.
    clover_emmc_observation[7] = idStatus10;
    clover_emmc_observation[8] = addrStatus10;
    clover_emmc_observation[9] = selStatus10;
    clover_emmc_observation[10] = stateStatus10;
    clover_emmc_observation[11] = blkStatus;
    clover_emmc_observation[12] = readStatus;
    clover_emmc_observation[13] = hdr0;
    clover_emmc_observation[14] = hdr1;
    clover_emmc_observation[15] = read32 (SDHCI_PRESENT_STATE);
  } else if (probeMode >= 11) {
    // The chain, then the data phase's three statuses and the R1 the card gave CMD17, then the
    // two header words and the checksum of the whole drained block.
    clover_emmc_observation[7] = idStatus11;
    clover_emmc_observation[8] = addrStatus11;
    clover_emmc_observation[9] = selStatus11;
    clover_emmc_observation[10] = stateStatus11;
    clover_emmc_observation[11] = blkStatus11;
    clover_emmc_observation[12] = statusEnd11;
    clover_emmc_observation[13] = rdRsp11;
    clover_emmc_observation[14] = hdr11a;
    clover_emmc_observation[15] = hdr11b;
  }

  gReportLength = 0;
  // The header is compressed from round93's four lines to three: mode 8 spends ~290 bytes on its
  // two CMD2 slots plus CMD3/CMD9/CMD13, and the 511-byte report is the panel's real limit. What
  // stays is what a regression would move: the RCG update and its 400 kHz row, both gates, the
  // stopped clock, the reset, power, and the vendor mux/DLL selectors.
  emitText ("g cfg=");
  emitHex (cfgAfter, 8);
  emitText (" m=");
  emitHex (mAfter & 0xffu, 2);
  emitText (" n=");
  emitHex (nAfter & 0xffu, 2);
  emitText (" d=");
  emitHex (dAfter & 0xffu, 2);
  emitText (" up=");
  emitDec (updated ? 1 : 0);
  emitText (" br=");
  emitHex (gateAfter, 8);
  emitText (" ab=");
  emitHex (readGcc32 (kGccAhbGate), 8);

  emitText ("\nh clk=");
  emitHex (clockBefore, 4);
  emitText (" rst=");
  emitDec (resetDone ? 1 : 0);
  emitText (" clr=");
  emitHex (clockCleared, 4);
  emitText (" pw=");
  emitHex (powerAfter, 2);
  // Mode 11's data phase needs five more lines than mode 10's, and the report buffer is 511
  // bytes. The two vendor selectors have been stable since round92 and are only needed when the
  // clock itself is in question, which `clk=` and the CMD0/CMD1 row already answer.
  if (probeMode < 11) {
    emitText (" vs ");
    emitHex (vendorAfter, 8);
    emitText (" vs3 ");
    emitHex (vendor3After, 8);
  }

  emitText ("\nc0 ");
  emitText (hostReady ? commandVerdict (status0, sent0) : "skip");
  emitText (" c1 ");
  emitText (verdict1);
  emitText (" t=");
  emitDec (tries);
  emitText (" to=");
  emitDec (timeouts);
  emitText (" rsp=");
  emitHex (rsp10, 8);

  if (probeMode == 0) {
    emitText ("\ncmd2 ");
    emitText (ready ? commandVerdict (status2, sent2) : "skip");
    if (ready) {
      emitText (" rsp=");
      emitHex (rsp20, 8);
    }

    emitText ("\ncmd3 ");
    emitText (cmd2Ok ? commandVerdict (status3, sent3) : "skip");
    if (cmd2Ok) {
      emitText (" rca=");
      emitDec ((rsp30 >> 16) & 0xffffu);
      emitText (" st=");
      emitDec ((rsp30 >> 9) & 0xfu);
      emitText (" rsp=");
      emitHex (rsp30, 8);
    }
  } else if (probeMode == 7) {
    // Every value below is a raw register, not a derived one: mode 6's `crc` is one bit of one
    // word, and this round exists to see the rest of that word set.
    emitText ("\ncmd2 a=");
    emitText (verdictA);
    emitText (" int=");
    emitHex (statusA, 8);
    if (sentA) {
      emitText ("\ncidA ");
      emitCid (rspA);
    } else {
      emitText ("\ncidA none");
    }
    // Printed whenever B was attempted, including when the re-arm itself stopped short: in that
    // case `c1=0` and the two numbers behind it say whether the card or the host gave up.
    if (resetA) {
      emitText ("\nreinit r=1 c0=");
      emitText (commandVerdict (rearm.status0, rearm.sent0));
      emitText (" c1=");
      emitDec (rearm.ready ? 1 : 0);
      emitText (" t=");
      emitDec (rearm.tries);
      emitText (" to=");
      emitDec (rearm.timeouts);
      if (!rearm.ready) {
        emitText (" int=");
        emitHex (rearm.status1, 8);
        emitText (" rsp=");
        emitHex (rearm.rsp1, 8);
      }
    } else {
      emitText ("\nreinit r=0");
    }
    if (sentB) {
      emitText ("\ncmd2b ");
      emitText (verdictB);
      emitText (" int=");
      emitHex (statusB, 8);
      emitText (" ps ");
      emitHex (presentAfterB, 8);
      emitText ("\ncidB ");
      emitCid (rspB);
    } else if (resetA) {
      emitText ("\ncmd2b skip");
    }
    emitText ("\ncmd3 ");
    emitText (sentC ? verdictC : "skip");
    if (sentC) {
      emitText (" rca=");
      emitDec ((rspC[0] >> 16) & 0xffffu);
      emitText (" st=");
      emitDec ((rspC[0] >> 9) & 0xfu);
      emitText (" int=");
      emitHex (statusC, 8);
    }
  } else if (probeMode == 8) {
    // Mode 8: X (check off, first slot), the re-arm, Y (check on, after it), then the commands
    // that only exist if the card is identified and addressed.
    emitText ("\nc2a ");
    emitText (verdictX);
    emitText (" int=");
    emitHex (statusX, 8);
    emitText ("\ncidA ");
    if (sentX) {
      emitCid (rspX);
    } else {
      emitText ("none");
    }
    emitText ("\nra ");
    if (rearmAttempted) {
      emitText ("c0=");
      emitText (commandVerdict (rearm8.status0, rearm8.sent0));
      emitText (" c1=");
      emitDec (rearm8.ready ? 1 : 0);
      emitText (" t=");
      emitDec (rearm8.tries);
      emitText (" to=");
      emitDec (rearm8.timeouts);
      if (!rearm8.ready) {
        emitText (" int=");
        emitHex (rearm8.status1, 8);
      }
    } else {
      emitText ("skip");
    }
    emitText ("\nc2b ");
    emitText (verdictY);
    emitText (" int=");
    emitHex (statusY, 8);
    emitText ("\ncidB ");
    if (sentY) {
      emitCid (rspY);
    } else {
      emitText ("none");
    }
    emitText ("\nc3 ");
    emitText (sentU ? verdictU : "skip");
    if (sentU) {
      emitText (" rca=");
      emitDec ((rspU[0] >> 16) & 0xffffu);
      emitText (" st=");
      emitDec ((rspU[0] >> 9) & 0xfu);
      emitText (" int=");
      emitHex (statusU, 8);
    }
    emitText ("\nc9 ");
    emitText (cmd3Ok ? verdictS : "skip");
    if (sentS) {
      emitText (" int=");
      emitHex (statusS, 8);
    }
    emitText ("\ncsd ");
    if (sentS) {
      emitCid (rspS);
    } else {
      emitText ("none");
    }
    emitText ("\nc13 ");
    emitText (cmd3Ok ? verdictD : "skip");
    if (sentD) {
      emitText (" st=");
      emitDec ((rspD0 >> 9) & 0xfu);
      emitText (" rsp=");
      emitHex (rspD0, 8);
    }
  } else if (probeMode == 9) {
    // Mode 9: the identification round94 proved (check off), the address it assigns, the first R1
    // at that address, the second 136-bit response with the check on, then the selection.
    emitText ("\nc2 ");
    emitText (verdictCid);
    emitText (" int=");
    emitHex (cidStatus, 8);
    emitText ("\ncid ");
    if (sentCid) {
      emitCid (cidRsp);
    } else {
      emitText ("none");
    }
    emitText ("\nc3 ");
    emitText (verdictAddr);
    if (sentAddr) {
      emitText (" st=");
      emitDec ((addrRsp0 >> 9) & 0xfu);
      emitText (" int=");
      emitHex (addrStatus, 8);
    }
    emitText ("\nc13 ");
    emitText (verdictState);
    if (sentState) {
      emitText (" st=");
      emitDec ((stateRsp0 >> 9) & 0xfu);
      emitText (" rsp=");
      emitHex (stateRsp0, 8);
    }
    emitText ("\nc9 ");
    emitText (verdictCsd);
    if (sentCsd) {
      emitText (" int=");
      emitHex (csdStatus, 8);
    }
    emitText ("\ncsd ");
    if (sentCsd) {
      emitCid (csdRsp);
    } else {
      emitText ("none");
    }
    emitText ("\nc7 ");
    emitText (verdictSel);
    if (sentSel) {
      emitText (" int=");
      emitHex (selStatus, 8);
    }
    emitText ("\nc13b ");
    emitText (verdictState2);
    if (sentState2) {
      emitText (" st=");
      emitDec ((state2Rsp0 >> 9) & 0xfu);
      emitText (" rsp=");
      emitHex (state2Rsp0, 8);
    }
  } else if (probeMode == 10) {
    // Mode 10: the same chain without the CID or CSD lines, plus the data command and the first
    // two words of what it returned. `efi 1` means those bytes are "EFI PART", i.e. LBA 1 really
    // is the GPT header and the data path works; `efi 0` with `c17 ok` means the transfer
    // completed but the bytes are not a GPT header, and the hdr line is what decoding starts on.
    emitText ("\nc2 ");
    emitText (verdictId10);
    emitText (" int=");
    emitHex (idStatus10, 8);
    emitText ("\nc3 ");
    emitText (verdictAddr10);
    if (sentAddr10) {
      emitText (" st=");
      emitDec ((addrRsp10 >> 9) & 0xfu);
      emitText (" int=");
      emitHex (addrStatus10, 8);
    }
    emitText ("\nc7 ");
    emitText (verdictSel10);
    if (sentSel10) {
      emitText (" int=");
      emitHex (selStatus10, 8);
    }
    emitText ("\nc13 ");
    emitText (verdictState10);
    if (sentState10) {
      emitText (" st=");
      emitDec ((stateRsp10 >> 9) & 0xfu);
      emitText (" rsp=");
      emitHex (stateRsp10, 8);
    }
    emitText ("\nc16 ");
    emitText (verdictBlk);
    if (sentBlk) {
      emitText (" int=");
      emitHex (blkStatus, 8);
    }
    emitText ("\nc17 ");
    emitText (verdictRead);
    if (sentRead) {
      emitText (" int=");
      emitHex (readStatus, 8);
      emitText (" ps=");
      emitHex (read32 (SDHCI_PRESENT_STATE), 8);
      emitText (" bs=");
      emitHex (read16 (SDHCI_BLOCK_SIZE), 4);
      // The two words are printed exactly as the FIFO returned them; the efi flag, not the word
      // order, is what says whether they are the signature. Without a transfer there is no hdr
      // line, so zeros can never be mistaken for the first block's contents.
      emitText ("\nhdr ");
      emitHex (hdr0, 8);
      emitText (" ");
      emitHex (hdr1, 8);
      // "EFI PART" as the FIFO's little-endian words: 'E','F','I',' ' then 'P','A','R','T'.
      bool efi = drained && hdr0 == 0x20494645u && hdr1 == 0x54524150u;
      emitText ("\nefi ");
      emitDec (efi ? 1 : 0);
    }
  } else if (probeMode >= 11) {
    // Mode 11: mode 10's chain, then the same read with BLK_CNT_EN set and the card's own
    // addressing. `nw` is how many FIFO words actually arrived and `sm` their 32-bit sum, so a
    // block of zeros and a block that was never sent cannot be read alike. `ev` carries the
    // command-completion spins/status and the whole data-phase poll count.
    // Mode 12 keeps the chain and replaces the burst with a paced drain: `dt f=` is when the
    // first word arrived, `dt l=` when the last did (both in us), and `nd` counts the words that
    // differed from the one before, which separates a real block from one word read 128 times.
    // Mode 13 is mode 12 after clearing CORE_FIFO_ALT_EN; `v3` replaces `hc`, which has not moved
    // since round92, because the FIFO selector is what this round changed.
    emitText ("\nc2 ");
    emitText (verdictId11);
    emitText (" int=");
    emitHex (idStatus11, 8);
    emitText ("\nc3 ");
    emitText (verdictAddr11);
    if (sentAddr11) {
      emitText (" st=");
      emitDec ((addrRsp11 >> 9) & 0xfu);
    }
    emitText ("\nc7 ");
    emitText (verdictSel11);
    if (sentSel11) {
      emitText (" int=");
      emitHex (selStatus11, 8);
    }
    emitText ("\nc13 ");
    emitText (verdictState11);
    if (sentState11) {
      emitText (" st=");
      emitDec ((stateRsp11 >> 9) & 0xfu);
    }
    emitText ("\nc16 ");
    emitText (verdictBlk11);
    if (sentBlk11) {
      emitText (" int=");
      emitHex (blkStatus11, 8);
      emitText (" r=");
      emitHex (blkRsp11, 8);
    }
    emitText ("\nc17 ");
    emitText (verdictRead11);
    if (sentRead11) {
      emitText (" int=");
      emitHex (readStatus11, 8);
      emitText (" ar=");
      emitDec (readArg11);
      emitText (" tm=");
      emitHex (transferMode11, 2);
      if (probeMode >= 12) {
        // The FIFO selector (13) or the bus width and DMA selector (12), then the timing, the
        // final clock control, the header words and where that header says the entries are. The
        // low 16 bits of VENDOR_SPEC3 carry CORE_FIFO_ALT_EN (bit 10) and nothing the RCG path
        // touches, so they are enough to read a write that landed.
        if (probeMode >= 13) {
          emitText ("\nv3 ");
          emitHex (vendor3Before13 & 0xffffu, 4);
          emitText (" ");
          emitHex (vendor3After13 & 0xffffu, 4);
        } else {
          emitText ("\nhc ");
          emitHex (read8 (SDHCI_HOST_CONTROL), 2);
          emitText (" ");
          emitHex (read16 (SDHCI_HOST_CONTROL2), 4);
        }
        if (probeMode >= 14) {
          // Mode 14 replaces the timing/header lines with the scan's own readings. `sc` carries
          // the three header fields the walk used and how many entry blocks were read, `lg` the
          // matching index (0 = no entry is named logfs) and its LBA extent, `ty` the type GUID
          // as the first 32-bit field plus fields 2-3. `efi` stays as the header regression.
          // Mode 16 already knows the extent, so it spends that space on the write/read-back:
          // `wr` the command's verdict and words handed over, `st` the card's own R1 after
          // programming, `old` the block's content before the write, `rd`/`w0` the read-back's
          // equality count and the words that identify the payload.
          if (probeMode < 16) {
            emitText ("\nsc el=");
            emitDec (entryEl14);
            emitText (" en=");
            emitDec (entryEn14);
            emitText (" es=");
            emitDec (entryEs14);
            emitText (" b=");
            emitDec (entryBlocks14);
            emitText (" ok=");
            emitDec (entryOk14 ? 1 : 0);
            if (entryBad14) {
              emitText (" hd=1");
            }
            if (!entryOk14) {
              emitText (" st=");
              emitHex (entryFail14, 8);
              emitText (" w=");
              emitDec (entryGot14);
            }
          }
          emitText ("\nlg i=");
          emitDec (entryFound14);
          if (entryFound14) {
            emitText (" a=");
            emitDec (entryFirst14);
            emitText (" b=");
            emitDec (entryLast14);
            if (probeMode < 16) {
              emitText ("\nty ");
              emitHex (entryType14, 8);
              emitText (" ");
              emitHex (entryTypeB14, 8);
            }
          }
          if (probeMode == 16 && entryFound14) {
            if (writeGated16) {
              emitText ("\nwr g=1");
            } else {
              emitText ("\nwr a=");
              emitDec (entryFirst14);
              emitText (" n=");
              emitDec (writeWords16);
              emitText (" ok=");
              emitDec (writeOk16 ? 1 : 0);
              emitText (" int=");
              emitHex (writeStatus16, 8);
              emitText ("\nst ");
              emitHex (stateRsp16, 8);
              emitText (" s=");
              emitDec ((stateRsp16 >> 9) & 0xfu);
              emitText (" p=");
              emitDec (statePolls16);
              emitText ("\nold ");
              emitHex (oldSum16, 8);
              emitText (" ");
              emitHex (oldWord16, 8);
              emitText ("\nrd ok=");
              emitDec (readOk16 ? 1 : 0);
              emitText (" cmp=");
              emitDec (equalWords16);
              emitText (" sum=");
              emitHex (newSum16, 8);
              emitText ("\nw0 ");
              emitHex (readOk16 ? gReadBlock[0] : 0, 8);
              emitText (" ");
              emitHex (readOk16 ? gReadBlock[1] : 0, 8);
              emitText (" ");
              emitHex (readOk16 ? gReadBlock[3] : 0, 8);
            }
          }
          if (probeMode >= 15 && entryFound14 == 0 && entryFirstIdx15) {
            // Calibration: raw words 14..16 and the start LBA of the first used entry. On this
            // board that is xbl at LBA 131072, so 00620078 0000006c 00000000 confirms both the
            // UTF-16LE packing and the place the entry array was read from.
            emitText ("\ne0 i=");
            emitDec (entryFirstIdx15);
            emitText (" w=");
            emitHex (entryFirstW15[0], 8);
            emitText (" ");
            emitHex (entryFirstW15[1], 8);
            emitText (" ");
            emitHex (entryFirstW15[2], 8);
            emitText (" a=");
            emitDec (entryFirstA15);
          }
          emitText ("\nps e=");
          emitHex (psEnd11, 8);
          emitText (" clk=");
          emitHex (read16 (SDHCI_CLOCK_CONTROL), 4);
          emitText ("\nefi ");
          emitDec (drained11 && hdr11a == 0x20494645u && hdr11b == 0x54524150u ? 1 : 0);

          if (probeMode == 17) {
            // The body is complete, so its length and byte sum are the record's. The header block
            // makes the record findable without this screen; the text block is what the screen
            // showed. Both are read back, so the line below reports what the card kept.
            if (entryFound14 == 0) {
              logGated17 = true;
            } else {
              logGated17 = !(entryFirst14 == 12320768u && entryLast14 == 12337151u);
            }
            if (!logGated17) {
              logLen17 = kLogPrefixLength + gReportLength + 1;
              logChecksum17 = 0;
              for (uint32_t i = 0; i < kLogPrefixLength; i++) {
                logChecksum17 += static_cast<uint8_t> (kLogPrefix[i]);
              }
              for (uint32_t i = 0; i < gReportLength; i++) {
                logChecksum17 += static_cast<uint8_t> (clover_emmc_report[i]);
              }
              logChecksum17 += static_cast<uint8_t> ('\n');
              for (uint32_t i = 0; i < kBlockWords; i++) {
                gWriteBlock[i] = 0;
              }
              gWriteBlock[0] = 0x554e5855u;   // "UXNU" in memory order
              gWriteBlock[1] = 0x31474f4cu;   // "LOG1"
              gWriteBlock[2] = entryFirst14;
              gWriteBlock[3] = logLen17;
              gWriteBlock[4] = logChecksum17;
              gWriteBlock[5] = 17;
              gWriteBlock[6] = gReportLength;
              logHeaderOk17 = writePioBlock (entryFirst14, gWriteBlock, logHeaderWords17,
                                             logHeaderStatus17);
              waitCardReady (logReadyRspA17, logReadyStatusA17, logReadyPollsA17);
              logHeaderReadOk17 = readPioBlock (entryFirst14, gReadBlock, logHeaderReadWords17,
                                                logHeaderReadStatus17);
              if (logHeaderReadOk17) {
                logRb0_17 = gReadBlock[0];
                logRb1_17 = gReadBlock[1];
                logRb3_17 = gReadBlock[3];
                logRb4_17 = gReadBlock[4];
                for (uint32_t i = 0; i < kBlockWords; i++) {
                  if (gReadBlock[i] == gWriteBlock[i]) {
                    logHeaderEqual17++;
                  }
                }
              }
              uint8_t *text = reinterpret_cast<uint8_t *> (gWriteBlock);
              for (uint32_t i = 0; i < 512; i++) {
                text[i] = 0;
              }
              for (uint32_t i = 0; i < kLogPrefixLength; i++) {
                text[i] = static_cast<uint8_t> (kLogPrefix[i]);
              }
              for (uint32_t i = 0; i < gReportLength; i++) {
                text[kLogPrefixLength + i] = static_cast<uint8_t> (clover_emmc_report[i]);
              }
              text[kLogPrefixLength + gReportLength] = '\n';
              logTextOk17 = writePioBlock (entryFirst14 + 1, gWriteBlock, logTextWords17,
                                           logTextStatus17);
              waitCardReady (logReadyRspB17, logReadyStatusB17, logReadyPollsB17);
              logTextReadOk17 = readPioBlock (entryFirst14 + 1, gReadBlock, logTextReadWords17,
                                              logTextReadStatus17);
              if (logTextReadOk17) {
                const uint8_t *readBack = reinterpret_cast<const uint8_t *> (gReadBlock);
                for (uint32_t i = 0; i < logLen17; i++) {
                  if (readBack[i] == text[i]) {
                    logTextEqual17++;
                  }
                }
              }
            }

            if (entryFound14 == 0) {
              emitText ("\ntx i=0");
            } else if (logGated17) {
              emitText ("\ntx g=1");
            } else {
              emitText ("\ntx a=");
              emitDec (entryFirst14);
              emitText (" n=");
              emitDec (logLen17);
              emitText (" h=");
              emitDec (logHeaderOk17 ? 1 : 0);
              emitText (" t=");
              emitDec (logTextOk17 ? 1 : 0);
              emitText (" hc=");
              emitDec (logHeaderEqual17);
              emitText (" tc=");
              emitDec (logTextEqual17);
              if (!logHeaderOk17) {
                emitText (" hs=");
                emitHex (logHeaderStatus17, 8);
              }
              if (!logTextOk17) {
                emitText (" ts=");
                emitHex (logTextStatus17, 8);
              }
              emitText ("\ntxh ");
              emitHex (logRb0_17, 8);
              emitText (" ");
              emitHex (logRb1_17, 8);
              emitText (" ");
              emitHex (logRb3_17, 8);
              emitText (" ");
              emitHex (logRb4_17, 8);
            }
          }

          if (probeMode == 18 || probeMode == 19) {
            // Read the record a previous boot left behind. Nothing here writes, so a clean `c=1`
            // is evidence that logfs kept the text across the reboot, not that this run rebuilt
            // it. The header's length is clamped before use; a stale or foreign block shows up as
            // `c=0` with its own magic, which is a reading rather than a fault.
            if (entryFound14 == 0) {
              emitText ("\nbk i=0");
            } else if (!(entryFirst14 == 12320768u && entryLast14 == 12337151u)) {
              emitText ("\nbk g=1");
            } else {
              logHeaderOk18 = readPioBlock (entryFirst14, gReadBlock, logHeaderWords18,
                                            logHeaderStatus18);
              if (logHeaderOk18) {
                logMagic0_18 = gReadBlock[0];
                logMagic1_18 = gReadBlock[1];
                logLen18 = gReadBlock[3];
                logSumStored18 = gReadBlock[4];
              }
              if (logMagic0_18 == 0x554e5855u && logMagic1_18 == 0x31474f4cu && logLen18 <= 512) {
                logTextOk18 = readPioBlock (entryFirst14 + 1, gReadBlock, logTextWords18,
                                            logTextStatus18);
              }
              const uint8_t *stored = reinterpret_cast<const uint8_t *> (gReadBlock);
              if (logTextOk18) {
                for (uint32_t i = 0; i < logLen18; i++) {
                  logSum18 += stored[i];
                }
                logCheck18 = logSum18 == logSumStored18;
              }
              emitText ("\nbk m=");
              emitHex (logMagic0_18, 8);
              emitText (" ");
              emitHex (logMagic1_18, 8);
              emitText (" n=");
              emitDec (logLen18);
              emitText (" s=");
              emitHex (logSumStored18, 8);
              emitText (" c=");
              emitDec (logCheck18 ? 1 : 0);
              if (!logHeaderOk18) {
                emitText (" hs=");
                emitHex (logHeaderStatus18, 8);
              }
              if (logTextOk18) {
                emitText ("\nbt ");
                for (uint32_t i = 0; i < logLen18 && i < 48; i++) {
                  uint8_t c = stored[i];
                  emitByte (c == '\n' ? '|' : (c >= 0x20 && c < 0x7f) ? static_cast<char> (c) : '.');
                }
              }
            }
          }

          if (probeMode == 19) {
            // Append rather than overwrite. Slot k is two blocks at logfs start + 2k, and the
            // first header that does not carry this driver's magic is free. The loop is bounded
            // at kLogSlots: a full window is printed as `ap f=`, never wrapped onto slot 0, so a
            // later boot cannot eat the oldest record by accident.
            if (entryFound14 == 0) {
              emitText ("\nap i=0");
            } else if (!(entryFirst14 == 12320768u && entryLast14 == 12337151u)) {
              emitText ("\nap g=1");
            } else {
              logFindOk19 = true;
              for (logSlot19 = 0; logSlot19 < kLogSlots; logSlot19++) {
                logFindOk19 = readPioBlock (entryFirst14 + 2 * logSlot19, gReadBlock,
                                            logFindWords19, logFindStatus19);
                if (!logFindOk19 ||
                    (gReadBlock[0] != kLogMagic0 || gReadBlock[1] != kLogMagic1)) {
                  break;
                }
              }
              logScan19 = (logSlot19 < kLogSlots) ? logSlot19 + 1 : kLogSlots;
              if (!logFindOk19) {
                emitText ("\nap r=");
                emitDec (logSlot19);
                emitText (" st=");
                emitHex (logFindStatus19, 8);
              } else if (logSlot19 >= kLogSlots) {
                emitText ("\nap f=");
                emitDec (logSlot19);
                emitText (" ss=");
                emitDec (logScan19);
              } else {
                logSlotLba19 = entryFirst14 + 2 * logSlot19;
                logLen19 = kLogPrefixLength + gReportLength + 1;
                logChecksum19 = 0;
                for (uint32_t i = 0; i < kLogPrefixLength; i++) {
                  logChecksum19 += static_cast<uint8_t> (kLogPrefix[i]);
                }
                for (uint32_t i = 0; i < gReportLength; i++) {
                  logChecksum19 += static_cast<uint8_t> (clover_emmc_report[i]);
                }
                logChecksum19 += static_cast<uint8_t> ('\n');
                for (uint32_t i = 0; i < kBlockWords; i++) {
                  gWriteBlock[i] = 0;
                }
                gWriteBlock[0] = kLogMagic0;
                gWriteBlock[1] = kLogMagic1;
                gWriteBlock[2] = logSlotLba19;
                gWriteBlock[3] = logLen19;
                gWriteBlock[4] = logChecksum19;
                gWriteBlock[5] = 19;
                gWriteBlock[6] = gReportLength;
                gWriteBlock[7] = logSlot19;
                logHeaderOk19 = writePioBlock (logSlotLba19, gWriteBlock, logHeaderWords19,
                                               logHeaderStatus19);
                waitCardReady (logReadyRsp19, logReadyStatus19, logReadyPolls19);
                logHeaderReadOk19 = readPioBlock (logSlotLba19, gReadBlock, logHeaderReadWords19,
                                                  logHeaderReadStatus19);
                if (logHeaderReadOk19) {
                  for (uint32_t i = 0; i < kBlockWords; i++) {
                    if (gReadBlock[i] == gWriteBlock[i]) {
                      logHeaderEqual19++;
                    }
                  }
                }
                uint8_t *text = reinterpret_cast<uint8_t *> (gWriteBlock);
                for (uint32_t i = 0; i < 512; i++) {
                  text[i] = 0;
                }
                for (uint32_t i = 0; i < kLogPrefixLength; i++) {
                  text[i] = static_cast<uint8_t> (kLogPrefix[i]);
                }
                for (uint32_t i = 0; i < gReportLength; i++) {
                  text[kLogPrefixLength + i] = static_cast<uint8_t> (clover_emmc_report[i]);
                }
                text[kLogPrefixLength + gReportLength] = '\n';
                logTextOk19 = writePioBlock (logSlotLba19 + 1, gWriteBlock, logTextWords19,
                                             logTextStatus19);
                waitCardReady (logReadyRsp19, logReadyStatus19, logReadyPolls19);
                logTextReadOk19 = readPioBlock (logSlotLba19 + 1, gReadBlock, logTextReadWords19,
                                                logTextReadStatus19);
                if (logTextReadOk19) {
                  const uint8_t *readBack = reinterpret_cast<const uint8_t *> (gReadBlock);
                  for (uint32_t i = 0; i < logLen19; i++) {
                    if (readBack[i] == text[i]) {
                      logTextEqual19++;
                    }
                  }
                }
                emitText ("\nap s=");
                emitDec (logSlot19);
                emitText (" ss=");
                emitDec (logScan19);
                emitText (" a=");
                emitDec (logSlotLba19);
                emitText (" n=");
                emitDec (logLen19);
                emitText (" h=");
                emitDec (logHeaderOk19 ? 1 : 0);
                emitText (" t=");
                emitDec (logTextOk19 ? 1 : 0);
                emitText (" hc=");
                emitDec (logHeaderEqual19);
                emitText (" tc=");
                emitDec (logTextEqual19);
                if (!logHeaderOk19) {
                  emitText (" hs=");
                  emitHex (logHeaderStatus19, 8);
                }
                if (!logTextOk19) {
                  emitText (" ts=");
                  emitHex (logTextStatus19, 8);
                }
                if (logTextReadOk19) {
                  emitText ("\nbta ");
                  const uint8_t *readBack = reinterpret_cast<const uint8_t *> (gReadBlock);
                  for (uint32_t i = 0; i < logLen19 && i < 48; i++) {
                    uint8_t c = readBack[i];
                    emitByte (c == '\n' ? '|' : (c >= 0x20 && c < 0x7f) ? static_cast<char> (c) : '.');
                  }
                }
              }
            }
          }

          if (probeMode == 20) {
            // The newest record, whichever boot wrote it. Reading it is also the arming step for
            // this boot's panic hook: by the time the panic comes, the card is selected and the
            // logfs coordinates are known, so the hook can append without repeating any of this
            // chain. `m=` says which writer left the record (19 report, 20 panic text).
            if (entryFound14 == 0) {
              emitText ("\nrd i=0");
            } else if (!(entryFirst14 == 12320768u && entryLast14 == 12337151u)) {
              emitText ("\nrd g=1");
            } else {
              logFindOk20 = true;
              for (logNewest20 = 0; logNewest20 < kLogSlots; logNewest20++) {
                logFindOk20 = readPioBlock (entryFirst14 + 2 * logNewest20, gReadBlock,
                                            logFindWords20, logFindStatus20);
                if (!logFindOk20 ||
                    (gReadBlock[0] != kLogMagic0 || gReadBlock[1] != kLogMagic1)) {
                  break;
                }
                logNewestFound20 = logNewest20 + 1;
                logNewestLba20 = entryFirst14 + 2 * logNewest20;
                logNewestLen20 = gReadBlock[3];
                logNewestKind20 = gReadBlock[5];
                logNewestSum20 = gReadBlock[4];
              }
              if (!logFindOk20) {
                emitText ("\nrd r=");
                emitDec (logNewest20);
                emitText (" st=");
                emitHex (logFindStatus20, 8);
              } else if (logNewestFound20 == 0) {
                emitText ("\nrd e=0 ss=");
                emitDec (logNewest20);
              } else {
                if (logNewestLen20 <= 512) {
                  logNewestTextOk20 = readPioBlock (logNewestLba20 + 1, gReadBlock,
                                                    logNewestWords20, logNewestStatus20);
                }
                const uint8_t *stored = reinterpret_cast<const uint8_t *> (gReadBlock);
                if (logNewestTextOk20) {
                  for (uint32_t i = 0; i < logNewestLen20; i++) {
                    logNewestCalc20 += stored[i];
                  }
                  logNewestCheck20 = logNewestCalc20 == logNewestSum20;
                }
                emitText ("\nrd s=");
                emitDec (logNewestFound20 - 1);
                emitText (" n=");
                emitDec (logNewestLen20);
                emitText (" m=");
                emitDec (logNewestKind20);
                emitText (" s=");
                emitHex (logNewestSum20, 8);
                emitText (" c=");
                emitDec (logNewestCheck20 ? 1 : 0);
                emitText (" ss=");
                emitDec (logNewest20);
                if (!logNewestTextOk20) {
                  emitText (" ts=");
                  emitHex (logNewestStatus20, 8);
                }
                if (logNewestTextOk20) {
                  emitText ("\nrt ");
                  for (uint32_t i = 0; i < logNewestLen20 && i < 32; i++) {
                    uint8_t c = stored[i];
                    emitByte (c == '\n' ? '|' : (c >= 0x20 && c < 0x7f) ? static_cast<char> (c) : '.');
                  }
                  emitText ("\nre ");
                  uint32_t from = logNewestLen20 > 32 ? logNewestLen20 - 32 : 0;
                  for (uint32_t i = from; i < logNewestLen20; i++) {
                    uint8_t c = stored[i];
                    emitByte (c == '\n' ? '|' : (c >= 0x20 && c < 0x7f) ? static_cast<char> (c) : '.');
                  }
                }
                // Round185: nothing arms here any more. The panic hook, the verdict ring and the
                // tee are armed by the GPT walk above, on this card's own entry for logdump, so a
                // mode-21 boot arms them and a mode-20 boot reads this window for the panel without
                // deciding anything about it.
              }
            }
          }
        } else {
          emitText ("\ndt f=");
          emitDec (firstUs12);
          emitText (" l=");
          emitDec (lastUs12);
          emitText (" nw=");
          emitDec (drainWords11);
          emitText (" nd=");
          emitDec (distinct12);
          emitText ("\nps a=");
          emitHex (psCmd11, 8);
          emitText (" e=");
          emitHex (psEnd11, 8);
          emitText (" clk=");
          emitHex (read16 (SDHCI_CLOCK_CONTROL), 4);
          emitText ("\nhdr ");
          emitHex (hdr11a, 8);
          emitText (" ");
          emitHex (hdr11b, 8);
          emitText (" sm=");
          emitHex (drainSum11, 8);
          emitText ("\ngpt lba=");
          emitDec (word6_12);
          emitText (" el=");
          emitDec (word18_12);
          emitText (" en=");
          emitDec (word20_12);
          emitText (" es=");
          emitDec (word21_12);
          emitText ("\nefi ");
          emitDec (drained11 && hdr11a == 0x20494645u && hdr11b == 0x54524150u ? 1 : 0);
        }
      } else {
        emitText ("\nev nc=");
        emitDec (spinsCmd11);
        emitText (" ia=");
        emitHex (statusCmd11, 8);
        emitText (" ns=");
        emitDec (spinsBrr11);
        emitText (" nw=");
        emitDec (drainWords11);
        emitText ("\nps a=");
        emitHex (psCmd11, 8);
        emitText (" e=");
        emitHex (psEnd11, 8);
        emitText ("\nhdr ");
        emitHex (hdr11a, 8);
        emitText (" ");
        emitHex (hdr11b, 8);
        emitText (" sm=");
        emitHex (drainSum11, 8);
        emitText ("\nefi ");
        emitDec (drained11 && hdr11a == 0x20494645u && hdr11b == 0x54524150u ? 1 : 0);
      }
    }
    if (probeMode >= 21) {
      // EXT_CSD's reading, printed even when the CMD17 phase did not end: `ok` is the block's own
      // verdict, `n` how many words arrived (128 means the whole structure), `sc` the sector count
      // the block device will publish, `st` the INT_STATUS group its data phase ended on.
      emitText ("\nex ");
      emitDec (extOk21 ? 1 : 0);
      emitText (" n=");
      emitDec (extWords21);
      emitText (" sc=");
      emitDec (extSectors21);
      emitText (" st=");
      emitHex (extStatus21, 8);
    }
    if (attemptedStop11) {
      emitText ("\nc12 ");
      emitText (verdictStop11);
      if (sentStop11) {
        emitText (" st=");
        emitDec ((stopRsp11 >> 9) & 0xfu);
      }
      emitText (" int=");
      emitHex (stopStatus11, 8);
      emitText ("\nc13b ");
      emitText (verdictAfter11);
      if (sentAfter11) {
        emitText (" st=");
        emitDec ((afterRsp11 >> 9) & 0xfu);
        emitText (" r=");
        emitHex (afterRsp11, 8);
      }
    }
  }
  emitByte ('\n');
  clover_emmc_stage = 8;
}

static void reidentify (void)
{
  clover_emmc_stage = 4;
  uint32_t capabilities = read32 (SDHCI_CAPABILITIES);
  uint16_t clockBefore = read16 (SDHCI_CLOCK_CONTROL);
  uint8_t powerBefore = read8 (SDHCI_POWER_CONTROL);
  uint8_t hostControl = read8 (SDHCI_HOST_CONTROL);
  uint16_t hostControl2 = read16 (SDHCI_HOST_CONTROL2);
  uint32_t presentBefore = read32 (SDHCI_PRESENT_STATE);

  // A command that timed out can leave the state machine busy. Reset the command and data parts,
  // not all of it: RESET_ALL also clears power and clock, and those are the two inputs this round
  // depends on. The reset bits self-clear, and a reset that never finishes is a reading too.
  write8 (SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
  uint32_t resetGuard = 0;
  while (read8 (SDHCI_SOFTWARE_RESET) & (SDHCI_RESET_CMD | SDHCI_RESET_DATA)) {
    if (++resetGuard >= kCommandSpinLimit) {
      break;
    }
  }
  bool resetDone = (read8 (SDHCI_SOFTWARE_RESET) & (SDHCI_RESET_CMD | SDHCI_RESET_DATA)) == 0;

  bool sentA = false, sentB = false, sent1 = false, sent2 = false, sent3 = false;
  uint32_t rsp0 = 0, rsp1 = 0, spins = 0;
  uint32_t statusA = 0, statusB = 0, status1 = 0, status2 = 0, status3 = 0;
  uint32_t rsp10 = 0, rsp11 = 0, rsp20 = 0, rsp21 = 0, rsp30 = 0, rsp31 = 0;

  // Probe 1: the firmware's clock, exactly as round87 and round88 found it.
  if (resetDone) {
    statusA = runCommand (SDHCI_MAKE_CMD (0, SDHCI_CMD_RESP_NONE), 0, sentA, rsp0, rsp1, spins);
  }

  // Probe 2: identification clock. Drop the card clock before touching the divider.
  write16 (SDHCI_CLOCK_CONTROL, static_cast<uint16_t> (clockBefore & ~SDHCI_CLOCK_CARD_EN));
  uint16_t clockWanted = static_cast<uint16_t> (
      (clockBefore & SDHCI_CLOCK_PLL_EN) | SDHCI_CLOCK_INT_EN |
      ((kIdentifyDivider & 0xffu) << SDHCI_DIVIDER_SHIFT) |
      (((kIdentifyDivider >> 8) & 0x3u) << 6));
  write16 (SDHCI_CLOCK_CONTROL, clockWanted);
  uint32_t stableGuard = 0;
  bool clockStable = false;
  while (stableGuard++ < kCommandSpinLimit) {
    if (read16 (SDHCI_CLOCK_CONTROL) & SDHCI_CLOCK_INT_STABLE) {
      clockStable = true;
      break;
    }
  }
  if (clockStable) {
    write16 (SDHCI_CLOCK_CONTROL, static_cast<uint16_t> (clockWanted | SDHCI_CLOCK_CARD_EN));
  }
  uint16_t clockAfter = read16 (SDHCI_CLOCK_CONTROL);
  if (resetDone) {
    statusB = runCommand (SDHCI_MAKE_CMD (0, SDHCI_CMD_RESP_NONE), 0, sentB, rsp0, rsp1, spins);
  }

  // CMD1: only an idle card answers, and only bit 31 of R3 says it finished powering up.
  uint32_t tries = 0, spins1 = 0;
  bool cmd1Ready = false;
  const char *verdict1 = "skip";
  if (sentB && !(statusB & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                           SDHCI_INT_INDEX)) && (statusB & SDHCI_INT_RESPONSE)) {
    clover_emmc_stage = 5;
    for (tries = 1; tries <= kCmd1Retries; tries++) {
      status1 = runCommand (SDHCI_MAKE_CMD (1, SDHCI_CMD_RESP_SHORT), kEmmcOcr,
                            sent1, rsp10, rsp11, spins1);
      if (!sent1) {
        break;
      }
      if (!(status1 & SDHCI_INT_RESPONSE)) {
        break;
      }
      if (status1 & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX)) {
        break;
      }
      if (rsp10 & 0x80000000u) {
        cmd1Ready = true;
        break;
      }
    }
    verdict1 = commandVerdict (status1, sent1);
    if (cmd1Ready) {
      verdict1 = "ok";
    } else if (sent1 && (status1 & SDHCI_INT_RESPONSE) &&
               !(status1 & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                            SDHCI_INT_INDEX))) {
      verdict1 = "no pwr";
    }
  }

  // CMD2 asks for the CID (R2); CMD3 assigns the address this port will use from now on.
  if (cmd1Ready) {
    status2 = runCommand (SDHCI_MAKE_CMD (2, SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC), 0,
                          sent2, rsp20, rsp21, spins);
  }
  bool cmd2Ok = sent2 && (status2 & SDHCI_INT_RESPONSE) &&
                !(status2 & (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT |
                             SDHCI_INT_INDEX));
  if (cmd2Ok) {
    status3 = runCommand (SDHCI_MAKE_CMD (3, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                             SDHCI_CMD_INDEX),
                          kProbeRca << 16, sent3, rsp30, rsp31, spins);
  }

  unsigned baseClock = 0, divider = 0, sdClockHz = 0;
  clockFacts (capabilities, clockAfter, baseClock, divider, sdClockHz);

  clover_emmc_observation[0] = clockBefore;
  clover_emmc_observation[1] = clockAfter;
  clover_emmc_observation[2] = powerBefore;
  clover_emmc_observation[3] = hostControl;
  clover_emmc_observation[4] = hostControl2;
  clover_emmc_observation[5] = presentBefore;
  clover_emmc_observation[6] = statusA;
  clover_emmc_observation[7] = statusB;
  clover_emmc_observation[8] = status1;
  clover_emmc_observation[9] = rsp10;
  clover_emmc_observation[10] = status2;
  clover_emmc_observation[11] = rsp20;
  clover_emmc_observation[12] = status3;
  clover_emmc_observation[13] = rsp30;
  clover_emmc_observation[14] = tries;

  gReportLength = 0;
  // Line 1 is the state the firmware handed over. clk writes back what round87 saw: 0007 is
  // internal clock on and stable, card clock on, divider 0.
  emitText ("pre clk=");
  emitHex (clockBefore, 4);
  emitText (" pw=");
  emitHex (powerBefore, 2);
  emitText (" hc=");
  emitHex (hostControl, 2);
  emitText (" hn=");
  emitHex (hostControl2, 4);
  emitText (" pst=");
  emitHex (presentBefore, 8);

  emitText ("\ncmd0a ");
  emitText (resetDone ? commandVerdict (statusA, sentA) : "skip");
  emitText (" rst=");
  emitDec (resetDone ? 1 : 0);
  emitText (" int=");
  emitHex (statusA, 8);

  emitText ("\nclk2=");
  emitHex (clockAfter, 4);
  emitText (" st=");
  emitDec (clockStable ? 1 : 0);
  emitText (" en=");
  emitDec ((clockAfter & SDHCI_CLOCK_CARD_EN) ? 1 : 0);
  emitText (" div=");
  emitDec (divider);
  emitText (" ");
  emitDec (sdClockHz / 1000U);
  emitText ("kHz");

  emitText ("\ncmd0b ");
  emitText (resetDone ? commandVerdict (statusB, sentB) : "skip");
  emitText (" int=");
  emitHex (statusB, 8);

  emitText ("\ncmd1 ");
  emitText (verdict1);
  emitText (" tries=");
  emitDec (tries);
  emitText (" rsp=");
  emitHex (rsp10, 8);

  emitText ("\ncmd2 ");
  emitText (cmd1Ready ? commandVerdict (status2, sent2) : "skip");
  if (cmd1Ready) {
    emitText (" rsp=");
    emitHex (rsp20, 8);
  }

  emitText ("\ncmd3 ");
  emitText (cmd2Ok ? commandVerdict (status3, sent3) : "skip");
  if (cmd2Ok) {
    uint32_t statusWord = rsp30;
    emitText (" rca=");
    emitDec ((statusWord >> 16) & 0xffff);
    emitText (" state=");
    emitDec ((statusWord >> 9) & 0xf);
    emitText (" rsp=");
    emitHex (statusWord, 8);
  }
  emitByte ('\n');
  clover_emmc_stage = 6;
}

// Round214: one decimal number into a byte buffer, for the BCB's second block. The panic path has no
// formatter it can call without a va_list, and this is the whole of what it needs.
static uint32_t bcbDecimal (uint8_t *out, uint32_t value)
{
  uint8_t digits[10];
  uint32_t count = 0;
  do {
    digits[count++] = static_cast<uint8_t> ('0' + value % 10u);
    value /= 10u;
  } while (value);
  for (uint32_t i = 0; i < count; i++) {
    out[i] = digits[count - 1 - i];
  }
  return count;
}

// Round214: the BCB's second block is untouched by every reader that boots this board, so the
// watchdog's two counters go there as text. The host reads them out of TWRP without writing `misc`,
// which is what keeps `boot-recovery` in block 0 the only thing that lands the board. Both marks sit
// at file scope and are forced out as data: inside the copy loops the compiler folds them into
// immediates, and then the payload gate has nothing to look for.
static const char kStuck[] __attribute__((used)) = "clover-stuck=";
static const char kRecov[] __attribute__((used)) = " recov=";

// Round196: leave a boot-recovery BCB in `misc` before the panic screen resets. Round195 proved the
// IMEM word is written but not consulted by this XBL, while the same BCB the host writes makes the
// board come up in TWRP in 14 s. Only the first two blocks are touched: command[32], status[32],
// recovery[768] and stage[32] all live in the first 1 KiB, and the rest of the partition stays as it
// was. Gated exactly like the log append -- a panic that fires before this boot reached logfs must
// not drive an unselected card -- plus the unattended argument, so a normal boot's panic keeps the
// reset behavior it had. Two PIO writes and the same paced wait; no read-back, because the next
// boot's landing is the read-back.
static unsigned int panicMarkRecovery (void)
{
  if (!gPanicLogReady || !gHost || !gUnattendedArmed || gBcbWritten) {
    return 1;
  }
  static const char kCommand[] = "boot-recovery";
  uint32_t words = 0, status = 0, rsp = 0, rstatus = 0, polls = 0;
  for (uint32_t block = 0; block < kBcbBlocks; block++) {
    uint8_t *bytes = reinterpret_cast<uint8_t *> (gWriteBlock);
    for (uint32_t i = 0; i < 512; i++) {
      bytes[i] = 0;
    }
    if (block == 0) {
      for (uint32_t i = 0; i < sizeof (kCommand) - 1; i++) {
        bytes[i] = static_cast<uint8_t> (kCommand[i]);
      }
    }
    // Round214: the watchdog's two counters go to the block's second half as text; the marks
    // themselves are the file-scope data above.
    if (block == 1) {
      uint32_t at = 0;
      for (uint32_t i = 0; i < sizeof (kStuck) - 1; i++) {
        bytes[at++] = static_cast<uint8_t> (kStuck[i]);
      }
      at += bcbDecimal (bytes + at, __atomic_load_n (&gFlightStuck, __ATOMIC_RELAXED));
      for (uint32_t i = 0; i < sizeof (kRecov) - 1; i++) {
        bytes[at++] = static_cast<uint8_t> (kRecov[i]);
      }
      at += bcbDecimal (bytes + at, __atomic_load_n (&gFlightRecovered, __ATOMIC_RELAXED));
      bytes[at++] = static_cast<uint8_t> ('\n');
    }
    if (!writePioBlock (kMiscFirstLba + block, gWriteBlock, words, status)) {
      return 2;
    }
    waitCardReady (rsp, rstatus, polls);
  }
  gBcbWritten = 1;
  return 100;
}

// Append text that this boot did not assemble -- the panic screen formats the panic message and
// its readings into a stack buffer and calls this by address (patch-xnu-entry-probe.py resolves
// the symbol and passes it as PANIC_SINK). No bring-up, no allocation, no locks: the card is
// still selected and the host is exactly as the last logfs pass left it, so the same paced PIO
// path is reused. A panic that fires before this boot reached logfs finds the ready flag clear
// and gets a status instead of a reset. There is deliberately no read-back here: the next boot's
// mode 20 is the reader, and a failure now must not hang the only screen this board has.
//
// Body is capped at 480 bytes so prefix + body + newline fits one block. Returns 100 + slot after
// both blocks were written, or 1, 2, 4 or 5 naming the stage that refused. (3 used to mean "window
// full"; round212 rotates instead, so it is never returned.)
static unsigned int panicAppendText (const char *text, unsigned int length)
{
  if (!gPanicLogReady || !gHost || !gPanicLogFirst || !text) {
    return 1;
  }
  uint32_t slot = 0, words = 0, status = 0;
  bool ok = true;
  uint32_t maxSeq = 0;
  uint32_t minSeq = 0xffffffffu, minSeqSlot = 0;
  for (slot = 0; slot < kLogSlots; slot++) {
    ok = readPioBlock (gPanicLogFirst + 2 * slot, gReadBlock, words, status);
    if (!ok || (gReadBlock[0] != kLogMagic0 || gReadBlock[1] != kLogMagic1)) {
      break;
    }
    // Round212: the window rotates instead of refusing. Word 8 is the record's turn, so a window
    // full of earlier panics gives its oldest record to this one rather than returning 3 and
    // leaving the reading off the card. Records older than round212 all carry 0 here, which makes
    // slot 0 the first to go, the same slot the old scan would have refused.
    if (gReadBlock[8] < minSeq) {
      minSeq = gReadBlock[8];
      minSeqSlot = slot;
    }
    if (gReadBlock[8] > maxSeq) {
      maxSeq = gReadBlock[8];
    }
  }
  if (!ok) {
    return 2;
  }
  if (slot >= kLogSlots) {
    slot = minSeqSlot;
  }
  if (length > 480) {
    length = 480;
  }
  const uint32_t body = length;
  const uint32_t total = kLogPrefixLength + body + 1;
  uint32_t sum = 0;
  for (uint32_t i = 0; i < kLogPrefixLength; i++) {
    sum += static_cast<uint8_t> (kLogPrefix[i]);
  }
  for (uint32_t i = 0; i < body; i++) {
    sum += static_cast<uint8_t> (text[i]);
  }
  sum += static_cast<uint8_t> ('\n');

  const uint32_t lba = gPanicLogFirst + 2 * slot;
  for (uint32_t i = 0; i < kBlockWords; i++) {
    gWriteBlock[i] = 0;
  }
  gWriteBlock[0] = kLogMagic0;
  gWriteBlock[1] = kLogMagic1;
  gWriteBlock[2] = lba;
  gWriteBlock[3] = total;
  gWriteBlock[4] = sum;
  gWriteBlock[5] = 20;   // written by the panic screen, not by a report pass
  gWriteBlock[6] = body;
  gWriteBlock[7] = slot;
  gWriteBlock[8] = maxSeq + 1;   // this record's turn in the rotating window
  gWriteBlock[9] = gBootId;

  uint32_t rsp = 0, rstatus = 0, polls = 0;
  gPioAttempt = 20;    // round305: the panic screen's own sentence, kind 20
  bool headerOk = writePioBlock (lba, gWriteBlock, words, status);
  waitCardReady (rsp, rstatus, polls);
  if (!headerOk) {
    return 4;
  }

  uint8_t *block = reinterpret_cast<uint8_t *> (gWriteBlock);
  for (uint32_t i = 0; i < 512; i++) {
    block[i] = 0;
  }
  for (uint32_t i = 0; i < kLogPrefixLength; i++) {
    block[i] = static_cast<uint8_t> (kLogPrefix[i]);
  }
  for (uint32_t i = 0; i < body; i++) {
    uint8_t c = static_cast<uint8_t> (text[i]);
    block[kLogPrefixLength + i] =
        (c == '\n') ? static_cast<uint8_t> ('\n')
                    : (c >= 0x20 && c < 0x7f) ? c : static_cast<uint8_t> ('.');
  }
  block[kLogPrefixLength + body] = '\n';
  bool textOk = writePioBlock (lba + 1, gWriteBlock, words, status);
  waitCardReady (rsp, rstatus, polls);
  if (!textOk) {
    return 5;
  }
  return 100 + slot;
}

// The symbol the entry-probe patcher resolves and hands to the panic screen as PANIC_SINK, so the
// name and the signature are part of the contract and stay exactly as they were. Round184 only adds
// what happens after the panic's own sentence is on the card: the two rings the boot was still
// holding get written out too, because the reason a boot ended and the sentences that led to it are
// two halves of one reading. It runs whatever the append returned -- a full log slot area (3) or a
// card that refused (2, 4, 5) is still a boot whose last words are in RAM, and the drain decides for
// itself whether the rings are armed.
// Round236: what the sink found when it was entered. panicAppendText returns 1 for a closed gate,
// 2/4/5 for a card that refused, so the return code alone cannot say which gate was closed -- and a
// frame with an empty report window is exactly the one that needs to know. The marker is written
// before the append and again with its return code, and neither write depends on the gates it
// reports. At most two records per boot, in the sector after the deadline record.
// Round302: the sentence, one block, written blind. This is the write path that needs no later
// opportunity: nothing is read first (the slot is boot id modulo the region's twelve sectors), it does
// not wait for the periodic flush that r301's ring copy depends on -- boot 91 is a boot where that
// flush never came -- and it carries its own magic, so a reader can tell it apart from whatever else
// happened to land in sector 44 + boot % 12. It reuses the copy r301 already made in RAM, so the two
// rounds are one path and not two owners of the sink: gPanicStash is filled before anything can refuse,
// and this is the third thing done with it.
//   The header's blocks= word carries the sentence's real length, because the block's own room is 352
// of the 480 characters the stash can hold. chars= < total= on the card says the sentence was longer
// than the blind record could take, which is a reading about the record, not about the panic.
static void flightPanicText (unsigned int appendRc)
{
  gPanicBlindChars = 0;
  gPanicBlindOk = 0;
  if (!gVerdictFirst || !gHost) {
    return;
  }
  // The same rule the ring copy follows: a return code of 100 or more means the kind-20 report record
  // already holds the sentence, and a second copy of it would let the host read one panic as two.
  // Here it is not a courtesy but the point -- the blind record exists for the boots whose report
  // window refused, which is every boot this board has lost a sentence on.
  if (appendRc >= 100 || gPanicStashLength == 0) {
    return;
  }
  const uint32_t chars = gPanicStashLength > kPanicStashBlindBytes
      ? kPanicStashBlindBytes : gPanicStashLength;
  uint32_t extra[3 + kPanicStashBlindBytes / 4];
  extra[0] = chars;
  extra[1] = appendRc;
  extra[2] = gBootId;
  for (uint32_t word = 0; word < kPanicStashBlindBytes / 4; word++) {
    uint32_t packed = 0;
    for (uint32_t i = 0; i < 4; i++) {
      const uint32_t at = word * 4 + i;
      // Zero, not a space, past the end: the reader trims at the first NUL and an all-space tail
      // would print as a sentence that grew into its room.
      const uint32_t byte = at < chars ? static_cast<unsigned char> (gPanicStash[at]) : 0u;
      packed |= byte << (8 * i);
    }
    extra[3 + word] = packed;
  }
  gPanicBlindChars = chars;
  gPanicBlindOk = flightWrite (kFlightEventText, 3, 0, gPanicStashLength, extra,
                               3 + kPanicStashBlindBytes / 4) ? 1u : 0u;
}

static void flightPanicMark (uint32_t stage, unsigned int rc, bool haveText)
{
  if (!gVerdictFirst || !gHost) {
    return;
  }
  uint32_t extra[18];
  extra[0] = stage;
  extra[1] = __atomic_load_n (&gPanicLogReady, __ATOMIC_ACQUIRE);
  // The host word says whether the sink found the controller owned, which is the one gate the
  // return code cannot express: a refused append with the host free is a card that said no.
  extra[2] = __atomic_load_n (&gHostOwner, __ATOMIC_ACQUIRE);
  extra[3] = gPanicLogFirst;
  extra[4] = rc;
  extra[5] = __atomic_load_n (&gPanicDrained, __ATOMIC_RELAXED);
  extra[6] = gBootId;
  extra[7] = (__atomic_load_n (&gUnattendedArmed, __ATOMIC_RELAXED) ? 1u : 0u) |
             (gBcbWritten ? 2u : 0u) | (haveText ? 4u : 0u);
  // Round305: the two things boot 94 could not say. [8] is the kernel's own second taken in this same
  // instruction stream, so the marker is placed on the log's clock instead of only bounded on it --
  // v0.7.14 withdrew the conversion this record's absence forced, and the withdrawal is what this word
  // pays for. [9..12] are the last refused block write: the sink's own silence splits into "the code
  // never ran" and "it ran and the card said no" only if the refusal has a voice, and boot 94's five
  // drain rows all had none.
  extra[8] = kernelSeconds ();
  extra[9] = gPioFailCode;
  extra[10] = gPioFailStatus;
  extra[11] = gPioFailWords;
  extra[12] = gPioFailAttempt;
  // Round310. Five more words, and all five are about the sink's own reach rather than the panic.
  //   [13]/[14] are the relay's two cursors -- how many rows the kernel sealed into it and how many a
  //     flush has taken to the card. Boot 102 proved the pair is the only thing that can settle a
  //     bracket row: its smp-pre reached the card while smp-post did not, and because the tail pass
  //     moves the kernel ring before it runs probeFlush, that pair cannot say whether the walk
  //     returned from the printf or the second seal simply arrived after the last flush. Sealed
  //     greater than consumed here is that question answered on the spot, in a record that lands.
  //   [15]/[16]/[17] are what the sink found the controller doing at its own door, before it asked
  //     the controller for anything: the outcome of the quiesce, the stage word, and the ticket that
  //     was held. The refusal counts above could only ever name the last block write this boot
  //     refused; these name the state the sink chose to leave behind.
  extra[13] = static_cast<uint32_t> (__atomic_load_n (&gProbeSeq, __ATOMIC_RELAXED));
  extra[14] = static_cast<uint32_t> (__atomic_load_n (&gProbeNext, __ATOMIC_RELAXED));
  extra[15] = __atomic_load_n (&gPanicQuiesce, __ATOMIC_RELAXED);
  extra[16] = __atomic_load_n (&gPanicQuiesceStage, __ATOMIC_RELAXED);
  extra[17] = __atomic_load_n (&gPanicQuiesceOwner, __ATOMIC_RELAXED);
  (void)flightWrite (kFlightEventPanic, 3, 0, 0, extra, 18);
}

/*
 * Round309: the drain's first breath, before it reads a gate.
 *
 * Boot 99 is the boot that asked for this. Its panic entry marker is on the card and every row the drain says is
 * not -- neither the r212 entry row, nor the r308 exit row, nor either report line -- and the only mechanism left
 * standing is that all of them reach the sector through a path that wants the host ticket, while this record's
 * path (the forced flightWrite) does not. Saying that from the marker's host=0x0 word is a candidate. This record
 * turns it into a reading: it exists on every boot that enters panicDrainRings(), and its boot stamp plus r293's
 * generation gate say whether the boot that owns this window is the one that wrote it.
 *
 * What it names is everything the gate below reads, plus the ticket itself, taken in the same instruction stream.
 * The refusal leg is not here: a drain-in sector that is absent or carries another boot's stamp means either that
 * the sink stopped before the drain or that this write was refused, and `CloverSdhci r309: drain legs in=` beside
 * it (on the ring, so it only speaks when the ring does) splits those two when the ring survives.
 */
static bool flightPanicDrainIn (void)
{
  uint32_t extra[8];
  extra[0] = gBootId;
  extra[1] = __atomic_load_n (&gHostOwner, __ATOMIC_ACQUIRE);
  extra[2] = (gHost != 0) ? 1u : 0u;
  extra[3] = gVerdictFirst ? 1u : 0u;
  extra[4] = __atomic_load_n (&gPanicDrained, __ATOMIC_RELAXED);
  extra[5] = gPanicAppendRc;
  extra[6] = kernelSeconds ();
  extra[7] = static_cast<uint32_t> (__atomic_load_n (&gProbeSeq, __ATOMIC_RELAXED));
  return flightWrite (kFlightEventDrainIn, 3, 0, 0, extra, 8);
}

/*
 * Round310: put the controller down before the sink asks it for anything.
 *
 * Boot 102 is the boot that asked for this. Its first marker reached the card and nothing after it did --
 * not the sentence, not the blind copy of it, not the marker's own exit half, not the drain-in sector --
 * and all four of those ride the same forced raw-PIO write that had just succeeded in the same
 * millisecond, so together they name one call that did not return rather than four refusals. What that
 * call was waiting for is also on the card: the marker's host= word is a live ticket, the stage had
 * already gone back to idle, and the last request record is a read stamped in the same millisecond --
 * a holder the panic has just stopped, with the beat path that would have taken the ticket from it
 * silent for 81 s. sheng never puts this call on its sink path at all (RAM copy, screen, park); the
 * closest shape that keeps our drain is to end the orphaned request first.
 *
 * The cap and the cooldown are waived here and only here: both exist to stop one dead card from turning
 * every request into a power cycle, and a boot that is ending has no later request to protect. The steal
 * is the beat's own compare-and-swap so a holder that is somehow still running keeps its card, and the
 * ladder is shared with the watchdog rather than duplicated. Where the door shows a machine still
 * running but no ticket held, this context takes one the normal way first: cardRecoverHeld resets
 * registers, and doing that under a request another core has just started is the one thing neither the
 * beat nor the sink is allowed. On the way out, while this context still owns the host, the rings are
 * flushed once: if the card answers now, the rows the census sealed seconds ago leave with them, and the
 * marker that follows carries the relay's own cursors either way.
 */
static void panicQuiesceCard (void)
{
  if (!gVerdictFirst || !gHost || !__atomic_load_n (&gHostArmed, __ATOMIC_ACQUIRE)) {
    __atomic_store_n (&gPanicQuiesce, 7u, __ATOMIC_RELAXED);
    return;
  }
  const uint32_t stage = __atomic_load_n (&gFlightStage, __ATOMIC_ACQUIRE);
  const uint32_t owner = __atomic_load_n (&gHostOwner, __ATOMIC_ACQUIRE);
  const uint32_t inhibit = read32 (SDHCI_PRESENT_STATE) & (SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT);
  __atomic_store_n (&gPanicQuiesceStage, stage, __ATOMIC_RELAXED);
  __atomic_store_n (&gPanicQuiesceOwner, owner, __ATOMIC_RELAXED);
  if (stage == kStageIdle && !inhibit && !owner) {
    return;
  }
  uint32_t ticket = 0;
  const bool stealing = owner != 0;
  if (stealing) {
    ticket = __atomic_add_fetch (&gHostTickets, 1u, __ATOMIC_RELAXED);
    if (!ticket) {
      ticket = __atomic_add_fetch (&gHostTickets, 1u, __ATOMIC_RELAXED);
    }
    uint32_t expected = owner;
    if (!__atomic_compare_exchange_n (&gHostOwner, &expected, ticket, false,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      __atomic_store_n (&gPanicQuiesce, 5u, __ATOMIC_RELAXED);
      return;
    }
    __atomic_add_fetch (&gFlightSteals, 1u, __ATOMIC_RELAXED);
  } else if (!tryTakeHost (ticket)) {
    __atomic_store_n (&gPanicQuiesce, 6u, __ATOMIC_RELAXED);
    return;
  }
  const bool ok = cardRecoverHeld ("panic", stage, true);
  teeSay ("CloverSdhci r310: quiesce %s stage=%u inhibit=%x owner=%x ok=%u probe=%u/%u\n",
          stealing ? "steal" : "idle", stage, inhibit, owner, ok ? 1u : 0u,
          static_cast<unsigned int> (__atomic_load_n (&gProbeSeq, __ATOMIC_RELAXED)),
          static_cast<unsigned int> (__atomic_load_n (&gProbeNext, __ATOMIC_RELAXED)));
  (void) flushVerdictRing ();
  dropHostTicket (ticket);
  __atomic_store_n (&gPanicQuiesce, ok ? (stealing ? 1u : 3u) : (stealing ? 2u : 4u),
                    __ATOMIC_RELAXED);
}

extern "C" unsigned int clover_emmc_panic_append (const char *text, unsigned int length)
{
  // Round210: the timer keeps running through a panic, and a beat that cut in here would be a
  // second owner of the card in the middle of the panic's own writes. The first panic owns it.
  __atomic_store_n (&gPanicActive, 1u, __ATOMIC_RELEASE);
  // Round301: the copy comes first, before anything that can refuse, because every step below this
  // one is a card write and the two boots that proved the sentence is worth keeping are the two
  // whose card write was refused. Printable only: the ring's reader splits rows on newlines, so
  // the sentence's own line breaks become spaces here rather than turning one panic into three
  // unverifiable rows.
  gPanicStashSaid = 0;
  gPanicStashLength = 0;
  if (text != nullptr && length > 0) {
    const uint32_t copied = length > kPanicStashBytes ? kPanicStashBytes : length;
    for (uint32_t i = 0; i < copied; i++) {
      const unsigned char c = static_cast<unsigned char> (text[i]);
      gPanicStash[i] = (c >= 0x20 && c < 0x7f) ? static_cast<char> (c) : ' ';
    }
    gPanicStashLength = copied;
  }
  // Round310: the card first, then the writes. Boot 102's marker says the sink got as far as its own
  // first record and no further, and the call it stopped in is the one that puts blocks on the card --
  // so whatever that request left running is now waiting for an answer no other core will give it.
  panicQuiesceCard ();
  flightPanicMark (0, 0, text != nullptr);
  const unsigned int rc = panicAppendText (text, length);
  gPanicAppendRc = rc;
  // Round302: before the exit marker, because the exit marker is a reading about this sink and the
  // sentence is the thing the whole sink exists to keep. If this write is refused too, the record
  // simply is not there and the magic rule says so -- which is the honest shape of 'the card said no
  // twice', as opposed to boot 91's sector that read like a verdict and belonged to a block request.
  flightPanicText (rc);
  flightPanicMark (1, rc, text != nullptr);
  panicDrainRings ();
  // Last, because everything above is a reading and this changes what the reset does. The screen
  // wrote its IMEM word before calling here; on this board that word does not survive into XBL's
  // decision, so the BCB is what carries the intent across the reset.
  (void)panicMarkRecovery ();
  return rc;
}

// Round283. Called from the kernel's reset funnels in scheduler context and from the panic screen,
// so: no locks, no allocation, no formatting. cntvct_el0 is the same 19.2 MHz counter the rest of
// the board's timing uses; ISB makes the read ordered after the stores above it.
static inline uint64_t imemCounter (void)
{
  uint64_t value;
  __asm__ volatile ("isb\n\tmrs %0, cntvct_el0" : "=r" (value));
  return value;
}

extern "C" void clover_imem_note (unsigned int kind, unsigned long pid, unsigned long aux,
                                  unsigned long pc, const char *text)
{
  volatile uint64_t *record = clover_imem_record;
  if (record == nullptr || record[0] == kResetRecordMagic) {
    // Nothing mapped, or somebody already named this reset. First writer wins so the panic screen
    // keeps the message that explains a panic even if a later restart hook follows it.
    return;
  }
  record[1] = kind;
  record[2] = pid;
  record[3] = aux;
  record[4] = pc;
  record[5] = imemCounter ();
  volatile unsigned char *bytes = reinterpret_cast<volatile unsigned char *> (record + 6);
  unsigned int i = 0;
  if (text != nullptr) {
    for (; i < 24; ++i) {
      const unsigned char c = static_cast<unsigned char> (text[i]);
      bytes[i] = c;
      if (c == 0) {
        break;
      }
    }
  }
  for (; i < 24; ++i) {
    bytes[i] = 0;
  }
  __asm__ volatile ("dsb sy" ::: "memory");
  record[0] = kResetRecordMagic;
  __asm__ volatile ("dsb sy" ::: "memory");
}

// One timer tick, no card: the last CNTVCT here is the last moment the board executed the clock's
// interrupt path. Words 9..11 only, so the reset writer's magic and text are never disturbed.
extern "C" void clover_imem_heartbeat (void)
{
  volatile uint64_t *record = clover_imem_record;
  if (record == nullptr) {
    return;
  }
  record[9] = kResetBeatMagic;
  record[10] = record[10] + 1;
  record[11] = imemCounter ();
  __asm__ volatile ("dsb st" ::: "memory");
}

// Round147: arm the unattended contract. Called from CloverPMGR right after it publishes its nub,
// which is the first point in this boot with working IOKit mapping -- before launchd and before
// anything that can panic for a reason worth reading. This half only makes resets land in fastboot;
// the deadline itself lives in ArmCpu, which measures clover-unattended=<seconds> on CNTVCT from
// the first timer tick and panics when it expires. Both parse the same argument, so one boot arg
// turns on the pair. Silent when the argument is absent; a mapping or read-back failure prints one
// line and leaves the pointer null, which makes the panic screen's reset an ordinary one.
extern "C" void clover_unattended_begin (void)
{
  unsigned seconds = 0;
  if (!PE_parse_boot_argn ("clover-unattended", &seconds, sizeof (seconds)) || !seconds) {
    // Not asked for. ArmCpu arms no deadline for the same argument, so nothing is pending.
    return;
  }
  if (gImemMap) {
    return;
  }
  const uintptr_t base = mapRange (gImemMap, kImemPhys, kImemSize);
  if (!base) {
    IOLog ("clover r148: unattended imem map failed %llx\n",
           static_cast<unsigned long long> (kImemPhys));
    return;
  }
  // Round283: one line per boot, before anything else uses the slot. The record belongs to the
  // previous boot; the heartbeat belongs to the same one and says how far its timer got even when
  // no reset hook fired. "empty" is itself a reading: it means the board came back through a path
  // that never reached this kernel's funnels, or it reset before this function armed the slot.
  //
  // Round284: the line goes through sdhciSay, not IOLog. Boot 70 proved why: the one-shot row sat
  // in msgbuf, the kernel-log tee copied it into the 512-record ring, and a boot that ran fourteen
  // minutes rotated it out before the card window was read -- a round whose whole answer was absent
  // read exactly like a boot that never touched the slot. sdhciSay pushes the same string into the
  // 64-record driver verdict ring, which the block path drains; that copy is outside the tee. The
  // push happens before the ring is armed (gVerdictFirst is still zero), so the line stays pending
  // in RAM and is the first record the drain writes once the window is known.
  volatile uint64_t *record = reinterpret_cast<volatile uint64_t *> (base + kResetRecordOffset);
  const uint64_t recordMagic = record[0];
  const uint64_t beatMagic = record[9];
  const uint64_t beats = record[10];
  const uint64_t beatCounter = record[11];
  if (recordMagic == kResetRecordMagic) {
    char text[25];
    const volatile unsigned char *bytes = reinterpret_cast<const volatile unsigned char *> (record + 6);
    unsigned int i = 0;
    for (; i < 24; ++i) {
      text[i] = bytes[i] ? static_cast<char> (bytes[i]) : 0;
      if (text[i] == 0) {
        break;
      }
    }
    text[i] = 0;
    sdhciSay ("clover r283: last reset kind=%llu pid=%llu aux=%llx pc=%llx cnt=%llu "
           "beat=%llu beatcnt=%llu beatmagic=%llx text=%s\n",
           static_cast<unsigned long long> (record[1]), static_cast<unsigned long long> (record[2]),
           static_cast<unsigned long long> (record[3]), static_cast<unsigned long long> (record[4]),
           static_cast<unsigned long long> (record[5]), static_cast<unsigned long long> (beats),
           static_cast<unsigned long long> (beatCounter),
           static_cast<unsigned long long> (beatMagic), text);
  } else {
    // Round293 puts the heartbeat's own magic next to the two numbers it maintains. Boots 77 and 79
    // both read this branch, and the words they found are shaped like small counters rather than
    // like anything this driver writes; with the magic in the row, an unnamed sighting splits into
    // "the slot never held our heartbeat" (the channel is not ours) and "our heartbeat, but nobody
    // named the reset" (the channel is ours and the hook did not fire). Without it the two read alike.
    sdhciSay ("clover r283: last reset unnamed magic=%llx beat=%llu beatcnt=%llu beatmagic=%llx\n",
           static_cast<unsigned long long> (recordMagic), static_cast<unsigned long long> (beats),
           static_cast<unsigned long long> (beatCounter),
           static_cast<unsigned long long> (beatMagic));
  }
  for (unsigned int i = 0; i < 20; ++i) {
    record[i] = 0;
  }
  clover_imem_record = record;
  __asm__ volatile ("dsb sy" ::: "memory");
  volatile uint32_t *cell = reinterpret_cast<volatile uint32_t *> (base + kRestartReasonOffset);
  cell[0] = kRestartFastboot;
  __asm__ volatile ("dsb sy" ::: "memory");
  const uint32_t readback = cell[0];
  if (readback != kRestartFastboot) {
    // A cell that does not hold the word is not a cell the panic screen should write either.
    IOLog ("clover r148: unattended mark %x read back %x\n", kRestartFastboot, readback);
    return;
  }
  clover_fastboot_reason = cell;
  gUnattendedArmed = 1;
  IOLog ("clover r148: unattended mark %x -> %x deadline=%us\n",
         kRestartFastboot, readback, seconds);
}

extern "C" void clover_emmc_run (void)
{
  unsigned mode = 0;
  if (!PE_parse_boot_argn ("clover-emmc", &mode, sizeof (mode)) || !mode) {
    // No argument means the board was not asked for eMMC; the RAMDisk root still boots. This
    // used to panic so a mis-built payload was visible, but that is instrumentation: the boot
    // now either uses the card or does not.
    return;
  }
  clover_emmc_stage = 1;
  // One question at a time: an unimplemented mode has to say so rather than boot quietly without
  // the storage instrument it was asked for.
  if (mode != 1 && mode != 2 && mode != 3 && mode != 4 && mode != 5 && mode != 6 && mode != 7 &&
      mode != 8 && mode != 9 && mode != 10 && mode != 11 && mode != 12 && mode != 13 &&
      mode != 14 && mode != 15 && mode != 16 && mode != 17 && mode != 18 && mode != 19 &&
      mode != 20 && mode != 21) {
    emmcFail (2, mode);
  }
  clover_emmc_stage = 2;
  gHost = mapRange (gHostMap, kSdhciHostPhys, kSdhciHostSize);
  if (!gHost) {
    emmcFail (3, kSdhciHostPhys);
  }
  if (mode == 1) {
    inspectHost ();
  } else if (mode == 2) {
    cmd13Probe ();
  } else if (mode == 3) {
    cmd13Sweep ();
  } else if (mode == 4) {
    reidentify ();
  } else {
    gGcc = mapRange (gGccMap, kGccPhys, kGccSize);
    if (!gGcc) {
      emmcFail (4, kGccPhys);
    }
    coldBringUp (mode == 5, (mode >= 7 && mode <= 21) ? mode : 0);
  }
  // Mode 21 is the boot path now, not a report: it asks CloverBlock for the block stack and the
  // boot continues. The stack itself runs later, when IOKitBSDInit() publishes "IOBSD" -- after
  // vfsinit(), which is where devfs and bdevvp become usable. Every other mode still ends on the
  // panic screen with the driver's report, because those modes exist only to answer a question.
  if (mode == 21) {
    clover_block_request ();
    return;
  }
  panic ("CloverSdhci:\n%s", clover_emmc_report);
}
