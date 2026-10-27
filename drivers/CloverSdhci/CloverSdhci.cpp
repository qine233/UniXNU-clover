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
static volatile uint32_t gBlockReady = 0;
static volatile uint32_t gBlockSectors = 0;
static volatile uint32_t gBlockGuard = 0;
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
// Round156: how many blocks one CMD18 asks for. 32 blocks is 16 KiB -- small enough
// that the FIFO drain stays inside a single deadline and the caller's buffer needs no
// staging, large enough that a shared-cache read is 32 commands instead of 32 blocks'
// worth of per-command overhead. SDHCI's block-count register is 16 bit, so 32 fits.
static const uint32_t kReadChunkBlocks = 32;
// Round157: DATA_END can be set in the same INT_STATUS word as a CRC, end-bit or
// timeout error. r156's return expressions only asked for DATA_END, so a transfer
// that ended in error could be reported as a clean one and the corrupt words went
// on to the caller. Any error bit overrides the end bit.
static const uint32_t kDataErrorMask = SDHCI_INT_DATA_TIMEOUT | SDHCI_INT_DATA_CRC |
                                       SDHCI_INT_DATA_END_BIT | SDHCI_INT_ADMA_ERROR;

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
  return (status & SDHCI_INT_DATA_END) && !(status & kDataErrorMask) &&
         wordCount == kBlockWords;
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
    IOLog ("CloverSdhci r159: multi %s fail lba=%u blocks=%u st=0x%x w=%u/%u (once)\n",
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
    return multiFailure (0, lba, blocks, status, 0, totalWords);
  }
  uint64_t frequency = counterFrequency ();
  if (frequency < 1000000ULL || frequency > 1000000000ULL) {
    frequency = 19200000ULL;
  }
  // 200 ms plus 10 ms per block: the data phase of one block at the slowest
  // identification clock is 10.4 ms, and this bound only has to outlast the card.
  const uint64_t deadline = frequency / 5ULL +
                            static_cast<uint64_t>(blocks) * (frequency / 100ULL);
  const uint64_t start = readCounter ();
  uint32_t guard = 0;
  bool clean = false;
  for (;;) {
    status = read32 (SDHCI_INT_STATUS);
    if (status & (SDHCI_INT_DATA_END | SDHCI_INT_DATA_TIMEOUT | SDHCI_INT_DATA_CRC |
                  SDHCI_INT_DATA_END_BIT | SDHCI_INT_ADMA_ERROR)) {
      clean = (status & SDHCI_INT_DATA_END) && !(status & kDataErrorMask);
      break;
    }
    if (readCounter () - start > deadline) {
      break;
    }
    if (wordCount < totalWords &&
        (read32 (SDHCI_PRESENT_STATE) & SDHCI_DATA_AVAILABLE)) {
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
  sendStopCommand ();
  if (clean && wordCount == totalWords) {
    return true;
  }
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

// One command engine and one FIFO: the guard has to be held for the whole request. What changed for
// round127 is the second caller's fate. It used to be refused, and a refused read is not a retry
// for a filesystem -- it is a mount failure -- so it waits now. The wait is bounded by the same
// two-second counter deadline the request itself gets, because a host stuck on someone else's
// transfer has to stay a timeout rather than become a hang, and no timer runs here to wake anyone.
// On success the caller gets the tick to measure its own deadline from, after the wait.
static bool acquireBlockGuard (uint64_t &frequency, uint64_t &start)
{
  frequency = counterFrequency ();
  if (frequency < 1000000ULL || frequency > 1000000000ULL) {
    frequency = 19200000ULL;
  }
  const uint64_t handover = frequency * 2ULL;
  const uint64_t waitStart = readCounter ();
  for (;;) {
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n (&gBlockGuard, &expected, 1u, false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      start = readCounter ();
      return true;
    }
    if (readCounter () - waitStart > handover) {
      return false;
    }
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
    return 0;
  }
  const uint64_t deadline = frequency * 2ULL;   // 2 s for the whole request
  uint32_t done = 0;
  while (done < blocks) {
    if (readCounter () - start > deadline) {
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
    // become less readable than it was before this change.
    if (chunk >= 4) {
      uint32_t got = 0;
      uint32_t status = 0;
      if (readMultiBlock (lba + done, target, chunk, got, status)) {
        done += chunk;
        continue;
      }
    }
    uint32_t got = 0;
    uint32_t status = 0;
    if (!readPioBlock (lba + done, target, got, status)) {
      break;
    }
    done++;
  }
  __atomic_store_n (&gBlockGuard, 0, __ATOMIC_RELEASE);
  return done;
}

// One whole 512-byte block, written the mirror image of readPioBlock: CMD24 with the block count
// enabled and the direction bit clear, the same 160 us counter gap between FIFO words, the same
// 200 ms deadline, and a final level-gated sweep so a controller that still had room takes the
// rest. Writing slower than the card consumes is safe -- SDHCI stops the card clock while the
// buffer is empty -- so the read path's pace is reused rather than guessed at. `wordCount` is how
// many words were handed to the data port; the return value is true only for a clean command
// response, DATA_END and all 128 words handed over.
static bool writePioBlock (uint32_t lba, const uint32_t *words, uint32_t &wordCount,
                           uint32_t &status)
{
  wordCount = 0;
  status = 0;
  if (!waitCommandSlot ()) {
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
    return false;
  }
  uint64_t frequency = counterFrequency ();
  if (frequency < 1000000ULL || frequency > 1000000000ULL) {
    frequency = 19200000ULL;
  }
  const uint64_t gap = frequency / 6250ULL;        // 160 us; writes are rare, keep the proven pace
  const uint64_t deadline = frequency / 5ULL;      // 200 ms
  const uint64_t start = readCounter ();
  uint64_t lastWrite = start;
  bool haveWritten = false;
  uint32_t guard = 0;
  for (;;) {
    status = read32 (SDHCI_INT_STATUS);
    if (status & (SDHCI_INT_DATA_END | SDHCI_INT_DATA_TIMEOUT | SDHCI_INT_DATA_CRC |
                  SDHCI_INT_DATA_END_BIT | SDHCI_INT_ADMA_ERROR)) {
      break;
    }
    uint64_t now = readCounter ();
    if (now - start > deadline) {
      break;
    }
    if (wordCount < kBlockWords && (!haveWritten || now - lastWrite >= gap) &&
        (read32 (SDHCI_PRESENT_STATE) & SDHCI_SPACE_AVAILABLE)) {
      write32 (SDHCI_BUFFER, words[wordCount++]);
      haveWritten = true;
      lastWrite = now;
    }
    if (++guard >= kCommandSpinLimit) {
      break;
    }
  }
  while (wordCount < kBlockWords &&
         (read32 (SDHCI_PRESENT_STATE) & SDHCI_SPACE_AVAILABLE)) {
    write32 (SDHCI_BUFFER, words[wordCount++]);
  }
  return (status & SDHCI_INT_DATA_END) && !(status & kDataErrorMask) &&
         wordCount == kBlockWords;
}

// Round159: the write mirror of readMultiBlock. One CMD25 for several consecutive blocks, then
// CMD12 with the busy spelling. The FIFO is level-gated rather than paced: below 26 MHz the card
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
  const uint64_t deadline = frequency / 5ULL +
                            static_cast<uint64_t>(blocks) * (frequency / 100ULL);
  const uint64_t start = readCounter ();
  uint32_t guard = 0;
  bool clean = false;
  for (;;) {
    status = read32 (SDHCI_INT_STATUS);
    if (status & (SDHCI_INT_DATA_END | SDHCI_INT_DATA_TIMEOUT | SDHCI_INT_DATA_CRC |
                  SDHCI_INT_DATA_END_BIT | SDHCI_INT_ADMA_ERROR)) {
      clean = (status & SDHCI_INT_DATA_END) && !(status & kDataErrorMask);
      break;
    }
    if (readCounter () - start > deadline) {
      break;
    }
    if (wordCount < totalWords &&
        (read32 (SDHCI_PRESENT_STATE) & SDHCI_SPACE_AVAILABLE)) {
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
  sendStopCommand ();
  if (clean && wordCount == totalWords) {
    return true;
  }
  return multiFailure (1, lba, blocks, status, wordCount, totalWords);
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
  const uint64_t deadline = frequency * 2ULL;
  uint32_t done = 0;
  while (done < blocks) {
    if (readCounter () - start > deadline) {
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
    if (chunk >= 4) {
      uint32_t handed = 0;
      uint32_t status = 0;
      if (writeMultiBlock (lba + done, target, chunk, handed, status)) {
        done += chunk;
        continue;
      }
    }
    uint32_t handed = 0;
    uint32_t status = 0;
    if (!writePioBlock (lba + done, target, handed, status)) {
      break;
    }
    done++;
  }
  __atomic_store_n (&gBlockGuard, 0, __ATOMIC_RELEASE);
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
static bool switchExtCsd (uint32_t index, uint32_t value, uint32_t &response, uint32_t &status)
{
  bool sent = false;
  uint32_t response1 = 0;
  uint32_t spins = 0;
  const uint32_t argument = kSwitchWriteByte | ((index & 0xffu) << 16) | ((value & 0xffu) << 8);
  status = runCommandBusy (SDHCI_MAKE_CMD (MMC_SWITCH, SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC |
                                           SDHCI_CMD_INDEX),
                           argument, sent, response, response1, spins);
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
              }
              if (entryFound14 && entryUdFound) {
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
            IOLog ("CloverSdhci r159: link hs=%d/%x bw=%d/%x clk=%x hc=%x hdr=%x/%x ok=%d\n",
                   link21.highSpeed ? 1 : 0, link21.hsStatus,
                   link21.width8 ? 1 : 0, link21.bwStatus,
                   link21.clockAfter, link21.hostControl,
                   link21.header0, link21.header1, fastOk21 ? 1 : 0);
            __atomic_store_n (&gBlockReady, 1, __ATOMIC_RELEASE);
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
                // Only a boot that found and read a record arms the panic hook. Its first job is
                // to append the text the panic screen formats, which is where a crash log first
                // stops being the driver's own report.
                gPanicLogFirst = entryFirst14;
                gPanicLogReady = 1;
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
extern "C" unsigned int clover_emmc_panic_append (const char *text, unsigned int length)
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
