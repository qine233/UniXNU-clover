// SPDX-License-Identifier: MIT
// clover (Mi Pad 4, SDM660): publish an original-shaped ApplePMGRNub so the unchanged target
// AMFI gets the provider it waits for. Shape taken from UniXNU-sheng's
// package/ShengPlatformPower.cpp, which solves the same structural problem (a board with no
// Apple PMGR hardware behind a prelinked AMFI that insists on one).
//
// What AMFI actually needs, from the base repository's scripts/audit-amfi-pmgr-contract.py,
// checked against the bytes of our own out/clover/kernel/kernel.macho: it builds
// IOService::serviceMatching("ApplePMGRNub") and calls IOService::waitForMatchingService with a
// nanosecond timeout, then reads the matched nub's provider (nub+0x88), calls virtual slot 320
// on it, ORs the 32-bit result with 0x4 and hands it to slot 321. On Apple silicon the provider
// is AppleT8011PMGR and those two slots statically amount to "read returns 0xffffffff, write
// does nothing". So the requirement is a *service*, not power management.
//
// This file therefore provides a provider that can do nothing except that pair: every other
// virtual slot of the original ApplePMGR ABI is overridden to panic, so a code path that
// genuinely wants power speaks up instead of silently getting a stub. No clocks, no rails, no
// secondary-CPU success is ever claimed.
//
// Opt-in twice over: the kernel links this only with CONFIG_XNU_SHENG_PMGR=y (the base
// repository names the hook sheng_pmgr_run, in bsd/kern/bsd_init.c after bsd_bufferinit), and
// the function returns without touching anything unless the board passes `clover-pmgr=2`.
//
// Unlike sheng, every failure here panics. A silent bail would look exactly like the round82
// photo -- AMFI's own "No PMGR?" -- and the two would be indistinguishable on the panel.
#include "OriginalPMGRABI.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
kern_return_t OSKextLoadKextWithIdentifier(const char *);
void clover_pmgr_static_init(void) __asm__("__GLOBAL__sub_I_CloverPMGR.cpp");
IOService *clover_original_pmgr_nub_create(IORegistryEntry *, ApplePMGR *);
uint32_t clover_original_pmgr_nub_read(IOService *);
void clover_original_pmgr_nub_write(IOService *, uint64_t);
// stage,status,parent-size,own-size,provider,nub,bound-provider,read-before,
// read-after,own-before,own-after,nub-before,nub-after,attached,registered,mode.
volatile uint64_t clover_pmgr_observation[16];
__attribute__((noinline)) void clover_pmgr_checkpoint(void) { __asm__ volatile("" ::: "memory"); }
// drivers/CloverSdhci, scheduled from here because a device repository has no patch slot for an
// IOKit personality (see that file's header comment). It is opt-in through its own boot argument
// and returns without touching anything when `clover-emmc` is absent.
void clover_emmc_run(void);
}

// The panic screen prints these lines, so the reading survives even when the console is the
// thing under suspicion.
#define CLOVER_PMGR_TIMEOUT_NS 2000000000ULL

__attribute__((noreturn)) static void unimplemented(unsigned slot)
{
  clover_pmgr_observation[0] = 0x100 + slot;
  clover_pmgr_observation[1] = static_cast<uint32_t>(kIOReturnUnsupported);
  clover_pmgr_checkpoint();
  panic("CloverPMGR: unimplemented power operation %u", slot);
}

__attribute__((noreturn)) static void fail(unsigned stage, uint64_t status)
{
  clover_pmgr_observation[0] = stage;
  clover_pmgr_observation[1] = status;
  clover_pmgr_checkpoint();
  panic("CloverPMGR: failed at stage %u status %llu (mode %llu parent %llu own %llu provider %llu nub %llu)",
        stage, static_cast<unsigned long long>(status),
        static_cast<unsigned long long>(clover_pmgr_observation[15]),
        static_cast<unsigned long long>(clover_pmgr_observation[2]),
        static_cast<unsigned long long>(clover_pmgr_observation[3]),
        static_cast<unsigned long long>(clover_pmgr_observation[4]),
        static_cast<unsigned long long>(clover_pmgr_observation[5]));
}

class CloverPlatformPower : public ApplePMGR {
  OSDeclareDefaultStructors(CloverPlatformPower)
public:
  void enableCPUCore(unsigned, uint64_t) override { unimplemented(168); }
  void enableCPUCore(unsigned) override { unimplemented(169); }
  void disableCPUCore(unsigned) override { unimplemented(170); }
  void enableCPUCluster(unsigned) override { unimplemented(171); }
  void disableCPUCluster(unsigned) override { unimplemented(172); }
  void initCPUIdle(ml_processor_info_t *) override { unimplemented(173); }
  void enterCPUIdle(UInt64 *) override { unimplemented(174); }
  void exitCPUIdle(UInt64 *) override { unimplemented(175); }
  void updateCPUIdle(UInt64 *) override { unimplemented(176); }
#include "GuardedPMGRSlots.inc"
};
OSDefineMetaClassWithInit(CloverPlatformPower, ApplePMGR, )
OSMetaClassConstructorInit(CloverPlatformPower, ApplePMGR, )
OSDefineBasicStructors(CloverPlatformPower, ApplePMGR)
// The original object's storage exceeds kalloc_type's 16 KiB size limit, so this uses XNU's
// zeroed, size-aware OSObject heap allocation pair.
void *CloverPlatformPower::operator new(size_t size) { return OSObject::operator new(size); }
void CloverPlatformPower::operator delete(void *memory, size_t size)
{
  OSObject::operator delete(memory, size);
}
static_assert(sizeof(CloverPlatformPower) == 0x4b8a8, "Exact original provider storage");

static const OSMetaClass *lookup(const char *text)
{
  const OSSymbol *name = OSSymbol::withCString(text);
  const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
  OSSafeReleaseNULL(name);
  return meta;
}

extern "C" void sheng_pmgr_run(void)
{
  // Round107's photograph was an ordinary boot to IOCPU: the nub was published, AMFI latched to
  // it, and no CloverSdhci report existed -- so the arguments this kernel actually received did
  // not contain clover-emmc. Every later boot names the kernel and echoes the command line before
  // any switch, so a photograph either contains this line (this payload ran) or does not (the
  // payload that booted is not the one that was flashed).
  IOLog("CloverPMGR r118: args [%s]\n", PE_boot_args());
  unsigned mode = 0;
  if (!PE_parse_boot_argn("clover-pmgr", &mode, sizeof(mode)) || !mode) return;
  clover_pmgr_observation[15] = mode;
  clover_pmgr_observation[1] = static_cast<uint32_t>(kIOReturnError);
  if (mode != 2) fail(1, mode);
  clover_pmgr_observation[0] = 1;

  // The kext is already linked into the preboot collection; this only runs its start, which is
  // what registers the ApplePMGR and ApplePMGRNub classes.
  kern_return_t result = OSKextLoadKextWithIdentifier("com.apple.driver.ApplePMGR");
  if (result) fail(2, static_cast<uint32_t>(result));
  const OSMetaClass *parent = lookup("ApplePMGR"), *nubMeta = lookup("ApplePMGRNub");
  if (!parent || parent != &ApplePMGR::gMetaClass || parent->getClassSize() != 0x4b8a8 ||
      !nubMeta || nubMeta->getClassSize() != 0x740) {
    fail(3, reinterpret_cast<uint64_t>(nubMeta));
  }
  clover_pmgr_observation[2] = parent->getClassSize();

  // Our metaclass has to be registered before ApplePMGR can be used as its superclass.
  void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
  if (!handle) fail(4, 0);
  clover_pmgr_static_init();
  result = OSMetaClass::postModLoad(handle);
  if (result) fail(5, static_cast<uint32_t>(result));
  clover_pmgr_observation[9] = CloverPlatformPower::gMetaClass.getInstanceCount();
  clover_pmgr_observation[11] = nubMeta->getInstanceCount();

  CloverPlatformPower *provider = new CloverPlatformPower;
  IORegistryEntry *description = new IORegistryEntry;
  IOService *nub = nullptr;
  if (!provider || !description) fail(6, reinterpret_cast<uint64_t>(provider));
  if (!provider->init() || !description->init()) fail(7, 0);
  clover_pmgr_observation[0] = 2;
  clover_pmgr_observation[3] = provider->getMetaClass()->getClassSize();
  clover_pmgr_observation[4] = reinterpret_cast<uintptr_t>(provider);
  // An empty resource description: no Apple register map, no device capabilities.
  description->setName("clover-platform-power");
  nub = clover_original_pmgr_nub_create(description, provider);
  clover_pmgr_observation[5] = reinterpret_cast<uintptr_t>(nub);
  if (!nub || nub->getMetaClass() != nubMeta || !provider->metaCast("ApplePMGR")) fail(8, 0);
  uintptr_t bound = 0;
  memcpy(&bound, reinterpret_cast<const char *>(nub) + 0x88, sizeof(bound));
  clover_pmgr_observation[6] = bound;
  if (bound != reinterpret_cast<uintptr_t>(provider)) fail(9, static_cast<uint64_t>(bound));

  // Exercise the exact pair AMFI performs, before anything is published.
  clover_pmgr_observation[0] = 3;
  uint32_t value = clover_original_pmgr_nub_read(nub);
  clover_pmgr_observation[7] = value;
  clover_original_pmgr_nub_write(nub, static_cast<uint64_t>(value | 4));
  clover_pmgr_observation[8] = clover_original_pmgr_nub_read(nub);
  clover_pmgr_observation[13] = nub->getProvider() != nullptr || provider->getProvider() != nullptr;
  clover_pmgr_observation[14] = (nub->getState() | provider->getState()) & kIOServiceRegisteredState;
  if (value != 0xffffffff || clover_pmgr_observation[8] != 0xffffffff ||
      clover_pmgr_observation[13] || clover_pmgr_observation[14]) {
    fail(10, value);
  }

  IOService *platform = IOService::getPlatform();
  if (!platform || !provider->attach(platform)) fail(11, reinterpret_cast<uint64_t>(platform));
  if (!provider->start(platform)) fail(12, 0);
  if (!nub->attach(provider)) fail(13, 0);
  if (!nub->start(provider)) fail(14, 0);
  provider->registerService(kIOServiceSynchronous);
  nub->registerService(kIOServiceSynchronous);
  clover_pmgr_observation[0] = 5;
  clover_pmgr_observation[13] = nub->getProvider() == provider && provider->getProvider() == platform;
  clover_pmgr_observation[14] = (nub->getState() & provider->getState()) & kIOServiceRegisteredState;

  // Prove AMFI's own discovery path finds this: same class name, same matcher, and it must
  // return now that the board has a timer for the deadline to fire against.
  auto matching = IOService::serviceMatching("ApplePMGRNub");
  IOService *matched = matching
      ? IOService::waitForMatchingService(matching, CLOVER_PMGR_TIMEOUT_NS) : nullptr;
  OSSafeReleaseNULL(matching);
  if (matched != nub) fail(15, reinterpret_cast<uint64_t>(matched));
  if (matched->getMetaClass() != nubMeta) fail(16, 0);
  if (!clover_pmgr_observation[13] || !clover_pmgr_observation[14]) fail(17, 0);
  clover_pmgr_observation[10] = CloverPlatformPower::gMetaClass.getInstanceCount();
  clover_pmgr_observation[12] = nubMeta->getInstanceCount();
  clover_pmgr_observation[1] = kIOReturnSuccess;
  // Both objects stay alive and registered; the registry and the nub refer to this provider.
  OSSafeReleaseNULL(matched);
  OSSafeReleaseNULL(description);
  clover_pmgr_checkpoint();
  // Only once the publication above held, so an eMMC reading and a failed PMGR step can never
  // share a photograph: CloverPMGR's own panic comes first and names its own stage.
  clover_emmc_run();
}
