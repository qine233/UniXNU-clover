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

static uintptr_t mapRange (uint64_t phys, uint64_t length)
{
  IODeviceMemory *memory = IODeviceMemory::withRange (phys, length);
  if (!memory) {
    return 0;
  }
  gHostMap = OSSharedPtr<IOMemoryMap> (memory->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  memory->release ();
  return gHostMap ? static_cast<uintptr_t> (gHostMap->getVirtualAddress ()) : 0;
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
  if (mode != 1 && mode != 2 && mode != 3 && mode != 4) {
    emmcFail (2, mode);
  }
  clover_emmc_stage = 2;
  gHost = mapRange (kSdhciHostPhys, kSdhciHostSize);
  if (!gHost) {
    emmcFail (3, kSdhciHostPhys);
  }
  if (mode == 1) {
    inspectHost ();
  } else if (mode == 2) {
    cmd13Probe ();
  } else if (mode == 3) {
    cmd13Sweep ();
  } else {
    reidentify ();
  }
  // The report is one pointer argument; if the panic screen cannot read it, it prints the
  // address in angle brackets instead of faulting, and the raw slots above still name the call.
  panic ("CloverSdhci: %s", clover_emmc_report);
}
