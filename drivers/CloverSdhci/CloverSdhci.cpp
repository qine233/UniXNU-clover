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
// iokit/KernelConfigTables.cpp by a patch under patches/xnu (docs/drivers/xnu-drivers.md:14,
// :149). A device repository cannot add one: feeds.py resolves modules by (kind, name) and takes
// the first path that exists, so a device-side `patches/xnu` would replace the base repository's
// directory -- hundreds of upstream patches -- rather than extend it (round86 hit this same wall
// from the other side, for osfmk/console). The alternative the base tree does offer is
// kits/NativePrelink/patches/*.patch, but reaching it means the device repo taking over the whole
// NativePrelink kit to smuggle one patch, which is the wrong kind of clever.
//
// So the driver exports `clover_emmc_run()` and CloverPMGR calls it at the end of
// `sheng_pmgr_run()`, the bsd_init.c hook this board has already been running through since
// round83 (CONFIG_XNU_SHENG_PMGR, after bsd_bufferinit, with IOKit up and the panic screen
// armed). That is a scheduling decision, not a design one: the day this port has its own patch
// slot it gets its own personality.
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
// Round175: the kernel log buffer. printf() and IOLog() both end in log_putc() ->
// log_putc_locked(msgbufp, c) (bsd/kern/subr_prf.c:388, bsd/kern/subr_log.c:554), so everything this
// boot prints is already sitting in one circular buffer that the driver can read. The header is
// exported to drivers (EXPORT_HDRS/bsd/sys/msgbuf.h) and msgbufp is declared under
// XNU_KERNEL_PRIVATE, which the kernel's own compile record for this driver already defines -- which
// is why this round needs no XNU patch at all.
#include <sys/msgbuf.h>
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
static IOLock *gBlockLock = nullptr;
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
};
static const uint32_t kTeeAllowlistCount =
    sizeof (kTeeAllowlist) / sizeof (kTeeAllowlist[0]);

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
static uint32_t gPanicDrained = 0;              // one drain per boot; the sink can be called again
static uint32_t gPanicDrainWritten = 0;         // records this drain put on the card
static uint32_t gPanicDrainDropped = 0;         // oldest records it gave up to keep the newest

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

// One command engine and one FIFO: the guard has to be held for the whole request. Round171 changes
// how a second caller waits. It used to be refused (round126 and earlier), then it was a bounded
// spin (round127 through round170) -- and the spin was the wrong instrument: a caller holding this
// core's only running thread does not help its holder finish, and a request that loses a spin race
// silently degrades to single-block I/O. It now blocks on an IOLock, the same way sheng's NativeUfs
// block device serializes its window reads.
//
// The bound is kept as a reading rather than as a refusal: the wait is measured, and the first long
// one is reported once per boot, because "a read was waiting on a write" is exactly the fact the
// r167 and r168 frames needed. A lock has no timeout, so the holder's own request deadline is what
// bounds the wait -- every path that takes the lock ends by releasing it, including the ones that
// fail. On success the caller gets the tick to measure its own deadline from, after the wait.
static bool acquireBlockGuard (uint64_t &frequency, uint64_t &start)
{
  frequency = counterFrequency ();
  if (frequency < 1000000ULL || frequency > 1000000000ULL) {
    frequency = 19200000ULL;
  }
  if (!gBlockLock) {
    return false;
  }
  const uint64_t waitStart = readCounter ();
  IOLockLock (gBlockLock);
  const uint64_t waited = readCounter () - waitStart;
  // Round178: every wait is counted, and the worst one kept, so the hand-off report at the tail of a
  // request can say what the lock actually did rather than what it was expected to do. This is a
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

static void releaseBlockGuard (void)
{
  if (gBlockLock) {
    IOLockUnlock (gBlockLock);
  }
}

// Round178: hand the host to whoever is waiting, then take it back for the rest of this request.
// Called between chunks, so a request that needs eight 64 KiB runs is eight owners rather than one
// owner for the whole transfer -- which is what a page fault at the other volume needs. `owned`
// accumulates guarded time so the request keeps round169's deadline (a bound on the transfer, not on
// how long the rest of the boot took around it) while no single hold outlives one chunk. Losing the
// re-acquire is not a hazard: acquireBlockGuard fails only when gBlockLock is null, and
// releaseBlockGuard does nothing in exactly the same case, so the tail of a request can never unlock
// a lock nobody holds.
static bool handOffBlockGuard (uint64_t &frequency, uint64_t &start, uint64_t &owned)
{
  owned += readCounter () - start;
  releaseBlockGuard ();
  if (!acquireBlockGuard (frequency, start)) {
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
  while (read32 (SDHCI_PRESENT_STATE) & SDHCI_DATA_INHIBIT) {
    const uint64_t waited = readCounter () - start;
    if (!reported && waited > frequency / 4ULL) {
      reported = true;
      sdhciSay ("CloverSdhci r169: card busy %llu ms\n",
             static_cast<unsigned long long> (waited * 1000ULL / frequency));
    }
    if (waited > bound) {
      return false;
    }
  }
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
    if (waited > bound) {
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
static void reportDmaFailure (const char *stage, uint32_t lba, uint32_t blocks, uint32_t status)
{
  static uint32_t reported = 0;
  if (reported >= 8) {
    return;
  }
  reported++;
  sdhciSay ("CloverSdhci r172: dma %s lba=%u blocks=%u st=%08x err8=%02x hctl=%02x\n",
         stage, lba, blocks, status, read8 (SDHCI_ADMA_ERROR), read8 (SDHCI_HOST_CONTROL));
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
      break;
    }
  }
  ticks = readCounter () - start;
  status = read32 (SDHCI_INT_STATUS);
  const bool stopped = sendStopCommand ();
  if (!ended || (status & (kContentErrorMask | SDHCI_INT_ADMA_ERROR)) || !stopped) {
    abortReadTransfer ();
    reportDmaFailure (ended ? (stopped ? "error" : "stop") : "deadline", lba, blocks, status);
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
  if (!acquireBlockGuard (frequency, start)) {
    reportReadFailure ("guard", lba, blocks, 0, 0);
    return 0;
  }
  // Round169: the card can still be programming after the write that owned the guard was counted
  // as stored; a read that starts then is refused. Wait for the card, then start the deadline.
  if (!waitCardIdle (frequency)) {
    // The card never said ready. Stop it explicitly, or every later read inherits the same state.
    abortReadTransfer ();
    reportReadFailure ("idle", lba, blocks, 0, 0);
    releaseBlockGuard ();
    return 0;
  }
  // Round165: the first request long enough to be a batch proves batch and single-block reads agree
  // before any caller relies on the batch. Round171: "long enough" is the same four blocks the batch
  // path itself demands, and the sample is the request's own length, so a boot whose biggest read is
  // 16 KiB still gets its one check instead of waiting forever for a 64 KiB one.
  if (!gReadCheckDone && blocks >= 4) {
    checkReadPath (lba, blocks > kReadChunkBlocks ? kReadChunkBlocks : blocks);
  }
  // Round172: the second gate, one run later. It only opens once the first has proved the multi-block
  // command itself, because ADMA2 rides on exactly that command with one bit added -- a boot whose
  // batch reads are untrusted has no business comparing a DMA read against anything.
  if (!gDmaCheckDone && gReadBatchTrusted && blocks >= 4 && dmaBringUp ()) {
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
    if (owned + readCounter () - start > deadline) {
      reportReadFailure ("deadline", lba, blocks, done, 0);
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
        blockRead = readPioBlock (lba + done, target, got, status);
        lastStatus = status;
      }
      if (!blockRead) {
        reportReadFailure ("gave up", lba + done, blocks, done, lastStatus);
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
      if (!handOffBlockGuard (frequency, start, owned)) {
        reportReadFailure ("guard", lba + done, blocks, done, 0);
        held = false;
        break;
      }
      cutIn = true;
    }
  }
  if (cutIn) {
    reportGuardHandoff ("read", lba, blocks, done);
  }
  // Round174: this is one of the places the guard is held and the card is idle, so it is also where
  // pending verdict lines get written out. Four records per pass; the rest wait for the next request.
  // Round175: the kernel log tee runs first, so a line it accepts here can be flushed in this pass.
  if (held) {
    teeKernelLog ();
    flushVerdictRing ();
    releaseBlockGuard ();
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
static bool writePioBlock (uint32_t lba, const uint32_t *words, uint32_t &wordCount,
                           uint32_t &status)
{
  wordCount = 0;
  status = 0;
  if (!waitCommandSlot ()) {
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

// One LOG1 record, same shape as mode 17 and the panic writer: header block then text block, byte
// sum over prefix + body + newline. Word 8 carries the sequence number, which is what makes a
// wrapped ring orderable: slot index alone stops being time once a ring has more records than slots.
// Word 5 carries the ring's own kind -- 21 for the driver's verdicts, 22 for the kernel lines the tee
// lifted -- because the host reads two rings out of one dd and a record has to say which one it came
// from before anything else about it is trusted.
static bool ringWriteRecord (uint32_t which, const char *text, uint32_t length)
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
  gRingBlock[5] = (which == kRingKernel) ? 22 : 21;
  gRingBlock[6] = body;
  gRingBlock[7] = gRingCursor[which] % kRingCardSlots[which];
  gRingBlock[8] = gRingSeq[which];
  uint32_t rsp = 0, rstatus = 0, polls = 0;
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
}

// ------------------------------------------------------------------ round175: the kernel log tee
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
static bool teeLineMatches (const char *line, uint32_t length)
{
  for (uint32_t entry = 0; entry < kTeeAllowlistCount; entry++) {
    const uint32_t width = teeStringLength (kTeeAllowlist[entry]);
    if (!width || width > length) {
      continue;
    }
    for (uint32_t at = 0; at + width <= length; at++) {
      uint32_t i = 0;
      while (i < width && line[at + i] == kTeeAllowlist[entry][i]) {
        i++;
      }
      if (i == width) {
        return true;
      }
    }
  }
  return false;
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
  if (!teeLineMatches (gTeeLine + start, length - start)) {
    return;
  }
  if (pushed >= kTeePushPerPass) {
    // Eight lines per guarded pass is sixteen block writes on a card that is also trying to serve the
    // request this tail belongs to; a chatty boot gets its surplus counted rather than queued forever.
    gTeeThrottled++;
    return;
  }
  ringPush (kRingKernel, gTeeLine + start, length - start);
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

static void panicDrainRings (void)
{
  if (gPanicDrained || !gVerdictFirst || !gHost) {
    return;
  }
  gPanicDrained = 1;
  for (uint32_t pass = 0; pass < kPanicTeePasses; pass++) {
    teeKernelLog ();
  }
  for (uint32_t which = 0; which < kRingCount; which++) {
    if (which == kRingKernel ? !gTeeReady : !gVerdictReady) {
      continue;
    }
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
      gPanicDrainWritten++;
    }
    gRingBusy[which] = 0;
  }
  // The drain has to be readable as a drain: a host that finds a quiet ring cannot otherwise tell "it
  // was empty" from "it ran and the card said no". teeSay only queues, and the ring it queues into was
  // drained above, so the one record is written here rather than left for a flush that will not come.
  teeSay ("CloverSdhci r184: panic drain wrote=%u dropped=%u tee_passes=%u\n",
          gPanicDrainWritten, gPanicDrainDropped, kPanicTeePasses);
  while (gRingPending[kRingDriver]) {
    const uint32_t slot = (gRingWrite[kRingDriver] - gRingPending[kRingDriver]) % kRingRamSlots;
    if (!ringWriteRecord (kRingDriver, gRingText[kRingDriver][slot],
                          gRingLength[kRingDriver][slot])) {
      break;
    }
    gRingPending[kRingDriver]--;
    gPanicDrainWritten++;
  }
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
static void reportWriteDmaFailure (const char *stage, uint32_t lba, uint32_t blocks, uint32_t status)
{
  static uint32_t reported = 0;
  if (reported >= 8) {
    return;
  }
  reported++;
  sdhciSay ("CloverSdhci r173: dma write %s lba=%u blocks=%u st=%08x err8=%02x hctl=%02x\n",
         stage, lba, blocks, status, read8 (SDHCI_ADMA_ERROR), read8 (SDHCI_HOST_CONTROL));
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
      break;
    }
  }
  ticks = readCounter () - start;
  status = read32 (SDHCI_INT_STATUS);
  const bool stopped = sendStopCommand ();
  if (!ended || (status & (kContentErrorMask | SDHCI_INT_ADMA_ERROR)) || !stopped) {
    resetReadPath ();
    reportWriteDmaFailure (ended ? (stopped ? "error" : "stop") : "deadline", lba, blocks, status);
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
  if (!acquireBlockGuard (frequency, start)) {
    return 0;
  }
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
  while (done < blocks) {
    if (owned + readCounter () - start > deadline) {
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
      if (writeAdmaRun (lba + done, target, chunk, dmaStatus, dmaTicks)) {
        uint32_t rsp = 0, settleStatus = 0, polls = 0;
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
        if (!writeMultiBlock (lba + done, target, chunk, handed, status)) {
          continue;
        }
        // Round167: DATA_END is not durability. The card says READY_FOR_DATA in TRAN only after
        // the block reached its media, and a batch that never gets there is not counted as done.
        uint32_t rsp = 0, settleStatus = 0, polls = 0;
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
        if (!writePioBlock (lba + done, target, handed, status)) {
          continue;
        }
        uint32_t rsp = 0, settleStatus = 0, polls = 0;
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
      if (!handOffBlockGuard (frequency, start, owned)) {
        held = false;
        break;
      }
      cutIn = true;
    }
  }
  if (cutIn) {
    reportGuardHandoff ("write", lba, blocks, done);
  }
  // Round175: same as the read tail -- collect from the kernel log, then spend the guard on the card.
  if (held) {
    teeKernelLog ();
    flushVerdictRing ();   // Round174: same reason as the read tail
    releaseBlockGuard ();
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
// was never caching.
static volatile uint32_t gCacheSize = 0;
static volatile uint32_t gCacheOn = 0;
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
    return 1;
  }
  uint64_t frequency = 0;
  uint64_t start = 0;
  if (!acquireBlockGuard (frequency, start)) {
    return 0;
  }
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
  const bool switchOff = switchExtCsd (kExtCsdCacheCtrl, 0, response, status, &spins);
  teeSay ("CloverSdhci r191: flush-c1 ok=%d st=%08x rsp=%08x spins=%u ms=%llu\n",
          switchOff ? 1 : 0, status, response, spins,
          static_cast<unsigned long long> ((readCounter () - commitStart) * 1000ULL / frequency));
  flushVerdictRing ();
  FlushSettle waitOff = FlushSettle ();
  if (switchOff) {
    waitOff = flushSettle (frequency, boundMs);
  }
  teeSay ("CloverSdhci r191: flush-w1 ready=%d to=%d busy=%llu ms polls=%u rsp=%08x st=%08x "
          "smp=%u mk=%u ps=%08x ms=%llu\n",
          waitOff.ready ? 1 : 0, waitOff.timedOut ? 1 : 0,
          static_cast<unsigned long long> (waitOff.busyMs), waitOff.polls, waitOff.response,
          waitOff.status, waitOff.samples, waitOff.marks, waitOff.present,
          static_cast<unsigned long long> ((readCounter () - commitStart) * 1000ULL / frequency));
  flushVerdictRing ();
  const bool flushed = switchOff && waitOff.ready;
  uint32_t backResponse = 0;
  uint32_t backStatus = 0;
  uint32_t backSpins = 0;
  const uint64_t backStart = readCounter ();
  const bool switchOn = switchExtCsd (kExtCsdCacheCtrl, 1, backResponse, backStatus, &backSpins);
  teeSay ("CloverSdhci r191: flush-c2 ok=%d st=%08x rsp=%08x spins=%u ms=%llu\n",
          switchOn ? 1 : 0, backStatus, backResponse, backSpins,
          static_cast<unsigned long long> ((readCounter () - backStart) * 1000ULL / frequency));
  flushVerdictRing ();
  FlushSettle waitOn = FlushSettle ();
  if (switchOn) {
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
  teeKernelLog ();
  flushVerdictRing ();   // Round174: same reason as the read tail
  releaseBlockGuard ();
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
            const uint32_t needed = kRingOffset[kRingKernel] +
                                    2 * kRingCardSlots[kRingKernel];
            if (logSectors >= needed) {
              gPanicLogFirst = entryLdFirst;
              gPanicLogReady = 1;
              gVerdictFirst = entryLdFirst;
              gVerdictReady = 1;
              gTeeReady = 1;
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
            // nobody owned. Round171 pays for the flush instead of giving up the cache, because the
            // reading that justified r167's caution turned out to be the cost of it -- the checkpoint
            // flush that measured 22.7 s in the r167 frame measured 102 s and 122 s in r170's, each
            // 512-byte block being programmed before its own write command is answered.
            //
            // The rule is unchanged in the one place that matters: a write is only counted as stored
            // after writeSettled() sees READY_FOR_DATA in TRAN, and the cache is now committed at
            // every CloverBlock::synchronize() and requestIdle(). A card that reports no
            // CACHE_SIZE has nothing to switch and is left alone; one that already reports the
            // cache on does not get switched a second time. Either way the answer stays on the
            // frame.
            const uint32_t cacheSize = (gExtCsd[62] >> 8) | (gExtCsd[63] << 24);
            const uint32_t cacheCtrlBefore = (gExtCsd[8] >> 8) & 0xffu;
            // Round191: byte 246 is CACHE_FLUSH_TIMEOUT, the /10 ms the card may take to commit its
            // cache. The flush's wait is set from this number instead of from a guess, so the r171
            // line now carries it.
            const uint32_t cacheFlushUnits = (gExtCsd[61] >> 16) & 0xffu;
            uint32_t cacheRsp = 0, cacheStatus = 0, cachePolls = 0;
            bool cacheOn = cacheSize != 0 && cacheCtrlBefore != 0;
            if (cacheSize && cacheCtrlBefore == 0) {
              cacheOn = switchExtCsd (kExtCsdCacheCtrl, 1, cacheRsp, cacheStatus) &&
                        cardInTransfer (cacheRsp, cacheStatus, cachePolls);
            }
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
            // Round171: the host is shared, and the sharing is now a lock rather than a spin flag.
            // Allocate it before the device is published: a block device that cannot serialize its
            // callers is worse than no block device, so the publication is what is gated here.
            if (!gBlockLock) {
              gBlockLock = IOLockAlloc ();
            }
            if (gBlockLock) {
              __atomic_store_n (&gBlockReady, 1, __ATOMIC_RELEASE);
            } else {
              sdhciSay ("CloverSdhci r171: host lock allocation failed\n");
            }
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

// Append text that this boot did not assemble -- the panic screen formats the panic message and
// its readings into a stack buffer and calls this by address (patch-xnu-entry-probe.py resolves
// the symbol and passes it as PANIC_SINK). No bring-up, no allocation, no locks: the card is
// still selected and the host is exactly as the last logfs pass left it, so the same paced PIO
// path is reused. A panic that fires before this boot reached logfs finds the ready flag clear
// and gets a status instead of a reset. There is deliberately no read-back here: the next boot's
// mode 20 is the reader, and a failure now must not hang the only screen this board has.
//
// Body is capped at 480 bytes so prefix + body + newline fits one block. Returns 100 + slot after
// both blocks were written, or 1..5 naming the stage that refused.
static unsigned int panicAppendText (const char *text, unsigned int length)
{
  if (!gPanicLogReady || !gHost || !gPanicLogFirst || !text) {
    return 1;
  }
  uint32_t slot = 0, words = 0, status = 0;
  bool ok = true;
  for (slot = 0; slot < kLogSlots; slot++) {
    ok = readPioBlock (gPanicLogFirst + 2 * slot, gReadBlock, words, status);
    if (!ok || (gReadBlock[0] != kLogMagic0 || gReadBlock[1] != kLogMagic1)) {
      break;
    }
  }
  if (!ok) {
    return 2;
  }
  if (slot >= kLogSlots) {
    return 3;
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

  uint32_t rsp = 0, rstatus = 0, polls = 0;
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
extern "C" unsigned int clover_emmc_panic_append (const char *text, unsigned int length)
{
  const unsigned int rc = panicAppendText (text, length);
  panicDrainRings ();
  return rc;
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
