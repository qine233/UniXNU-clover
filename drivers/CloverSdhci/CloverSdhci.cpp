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
// The formatted report, one %s argument, so the panic screen's argument list cannot shift.
char clover_emmc_report[512];
volatile uint32_t clover_emmc_stage = 0;
}

// ------------------------------------------------------------------ the window and its reads
static OSSharedPtr<IOMemoryMap> gHostMap;
static uintptr_t gHost = 0;
// Mode 5 also needs the SDCC1 clock source, which is in GCC, not in this 4 KiB window.
static OSSharedPtr<IOMemoryMap> gGccMap;
static uintptr_t gGcc = 0;

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

static bool waitCommandSlot (void)
{
  uint32_t guard = 0;
  while (read32 (SDHCI_PRESENT_STATE) & (SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT)) {
    if (++guard >= kCommandSpinLimit) {
      return false;
    }
  }
  return true;
}

// Returns the command group of INT_STATUS. `sent` is false when the register was never written,
// which is a host verdict, not a card one. The response words are read even after an error: a CRC
// error still leaves the bits that arrived in the register, and that is evidence.
static uint32_t runCommand (uint16_t command, uint32_t argument, bool &sent,
                            uint32_t &response0, uint32_t &response1, uint32_t &spins)
{
  sent = waitCommandSlot ();
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
// are M=1, N=~3=0xfc, D=~4=0xfb, with CFG mode bit12 set (clk-rcg2.c:290-333).
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
// Vendor clock mux (sdhci-msm.c:78-85): 10:8 select the MCLK, bit18 enables the HS400-only
// selector, 21:19 name it. Default is 2, the free-running MCLK.
static const unsigned kSdhciVendorSpec = 0x20c;
static const unsigned kSdhciVendorSpec3 = 0x250;
static const uint32_t kHcMclkSelMask = 3u << 8;
static const uint32_t kHcMclkSelDefault = 2u << 8;
static const uint32_t kHcSelectInEn = 1u << 18;
static const uint32_t kHcSelectInMask = 7u << 19;
static const uint32_t kPwrSaveDll = 1u << 3;

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
  emitText (" vs ");
  emitHex (vendorAfter, 8);
  emitText (" vs3 ");
  emitHex (vendor3After, 8);

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
  } else {
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

extern "C" void clover_emmc_run (void)
{
  unsigned mode = 0;
  if (!PE_parse_boot_argn ("clover-emmc", &mode, sizeof (mode)) || !mode) {
    return;
  }
  clover_emmc_stage = 1;
  // One question at a time: an unimplemented mode has to say so rather than boot quietly without
  // the storage instrument it was asked for.
  if (mode != 1 && mode != 2 && mode != 3 && mode != 4 && mode != 5 && mode != 6 && mode != 7 &&
      mode != 8 && mode != 9) {
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
    coldBringUp (mode == 5, (mode == 7 || mode == 8 || mode == 9) ? mode : 0);
  }
  // The report is one pointer argument; if the panic screen cannot read it, it prints the
  // address in angle brackets instead of faulting, and the raw slots above still name the call.
  // Round94's photograph showed the screen wraps at 66 characters (its 68-character `h` line put
  // `f0` on the next line), and the 13-character label would otherwise eat into the first line's
  // 66; the newline keeps every report line starting at column zero.
  panic ("CloverSdhci:\n%s", clover_emmc_report);
}
