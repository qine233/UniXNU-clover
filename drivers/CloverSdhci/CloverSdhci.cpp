// SPDX-License-Identifier: BSD-2-Clause-Patent
// clover (Mi Pad 4, SDM660): the eMMC host driver's first question, asked without writing a
// single register.
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
// That is the thing worth one photograph: **does the host still answer, and what state did the
// firmware leave it in?** A read of SDHCI_CAPABILITIES that comes back with a plausible base
// clock and a spec version between 1.0 and 4.2 says the window is mapped, the SDCC clocks are
// running, and the bus is not hung. All zeros says gated. All ones says a NoC abort. Anything
// else says look at the fields.
//
// This round therefore issues no commands, sets no clock, touches no reset line, enables no
// interrupt, and writes nothing anywhere. It maps one 4 KiB window, reads, formats, and panics.
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
// next rounds add those, in this order, one flash each: a command with no data (CMD13
// SEND_STATUS, which proves the command path and the response registers), then one block with
// FIFO data (CMD17 at LBA 1, whose first five bytes are the GPT magic "EFI PART" -- an answer
// that checks itself), then whatever the log-in-a-partition plan needs of it.
//
// Every path that is not the success path panics, because on this board silence is not a reading.
// The tick watchdog stays armed at 60000 ticks: if a plain MMIO read hangs the interconnect, the
// panic that follows is the watchdog's, and its `elr=` names the instruction we are stuck on.
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

  // Base clock in kHz, the way Linux computes it: CLOCK_BASE_MASK 15:8, and the unit multiplier
  // is bit 24 of CAPABILITIES_1 (x1 in the spec's own words: "SDHCI_CLOCK_MUL_MASK 23:16" carries
  // the ratio, 0 means the field is in Hz * 1000 as usual).
  unsigned baseClock = (capabilities & SDHCI_CLOCK_BASE_MASK) >> SDHCI_CLOCK_BASE_SHIFT;
  unsigned clockMultiplier = (capabilities1 & 0x00800000) ? 1 : 0;
  unsigned divider = ((clockControl >> SDHCI_DIVIDER_SHIFT) & 0xff) | ((clockControl & 0x300) >> 6);
  unsigned sdClockHz = divider ? (baseClock * 100000U) / (2U * divider) : 0;

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
  emitText ("MHz x");
  emitDec (clockMultiplier ? 1 : 0);
  emitText (" div");
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

extern "C" void clover_emmc_run (void)
{
  unsigned mode = 0;
  if (!PE_parse_boot_argn ("clover-emmc", &mode, sizeof (mode)) || !mode) {
    return;
  }
  clover_emmc_stage = 1;
  // One question at a time: mode 1 is the only thing this round implements, and an unimplemented
  // mode has to say so rather than boot quietly without the storage instrument it was asked for.
  if (mode != 1) {
    emmcFail (2, mode);
  }
  clover_emmc_stage = 2;
  gHost = mapRange (kSdhciHostPhys, kSdhciHostSize);
  if (!gHost) {
    emmcFail (3, kSdhciHostPhys);
  }
  inspectHost ();
  // The report is one pointer argument; if the panic screen cannot read it, it prints the
  // address in angle brackets instead of faulting, and the raw slots above still name the call.
  panic ("CloverSdhci: %s", clover_emmc_report);
}
