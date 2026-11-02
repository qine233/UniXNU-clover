// SPDX-License-Identifier: BSD-2-Clause-Patent
// Round276. Publish the owned AppleKeyStore this board has been missing since the wait got a name.
//
// Why this exists. Boot 61 read datamigrator's own frames as function names: the XPC handler that
// took backboardd's request calls -_unlockKeyBagUseOfForceIsAuthorized, that calls
// -waitUntilPreboardDidFinishUnlock, and that parks a thread in
// dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER) -- a device-unlock wait, forever, inside the
// process that owes the desktop its reply (notes/development-log.md, r273/r274 readings). This
// board's own r252 ledger has always said why no unlock can happen: PreBoard asks for
// IODeviceTree:/arm-io/sep and does not have one, and audiomxd asks eighteen times for
// IOService:/IOResources/AppleKeyStore and does not find that either. UniXNU-sheng runs the same
// iPadOS image to the desktop with no SEP in sight: it ships this service itself.
//
// Why the guard is kept and not copied blindly. The upstream driver's gate is "publish only when the
// AppleKeyStore *metaclass* is absent" -- sheng boots with no prelink roots, so that is trivially
// true there, but this board links the stock kernelcache, whose image really does carry the class
// strings. Publishing a second service of a class that already exists would be a different problem
// and needs different code, so r275 measured the precondition first and boot 62 answered:
//
//   clover r275: aks class=0 userclient_class=0
//   clover r275: aks walked=127 stopped=0 keystore_services=0
//
// Both classes absent, the walk finished, nothing in the plane. That is the green light, and the
// same line is what will disprove this round: once a service of this class exists, the census has to
// read class=1, and if it still reads 0 the publish did not happen.
//
// Gates. offline-key-events=1 is sheng's name and stays: it describes an offline keystore, not a
// board, and round255 already set that rule for the HID pair. clover-aks=1 (sheng's sheng-aks) does
// not survive that test -- it names another device -- so it is spelled for this board here, and the
// reason is written down rather than left to whoever ports the next one. clover-aks-sls=<n> overrides
// the reported lock state the way sheng's does; the default is 0, unlocked since boot, which is what
// the driver's own header says libaks reads as "no passcode, nothing locked".
//
// Where the readings go. Nowhere near here. The probe runs at bsd_autoconf(), and boot 59 taught
// that a print that early is outside the ring the panic drain reads back -- the reading would look
// like an absence. So everything below is state, and the census (round243, latched to one fire past
// 1500 s) prints it, along with the keystore rows r275 added. CONFIG_XNU_BOOT_PROGRESS=n on this
// board, so sheng's boot_progress_ext rows are not available and could not be read anyway; the
// counters are exported instead, which is also what makes them readable by a second instrument.
#include <IOKit/IOService.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <string.h>

extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void native_keyevents_static_init(void) __asm__("__GLOBAL__sub_I_NativeKeyStoreEvents.cpp");
extern "C" IOService *native_keyevents_allocate(void);

extern "C" {
// What the census reads. stage: 0 never armed, 1 armed and refused, 2 classes registered, 3 service
// started. status is the IOReturn of whichever step last decided the outcome.
volatile uint32_t native_keyevents_stage, native_keyevents_refused_opens;
volatile int native_keyevents_status = -1;
volatile uint64_t native_keyevents_service;
volatile uint32_t native_aks_opens, native_aks_calls;

// The selectors worth naming in a reading are the four that answer something (see the driver's own
// header). The histogram is kept for all 96 so a daemon asking for a key operation shows up as
// ks_other= rather than as silence, and the census prints the named four individually.
#define CLOVER_AKS_SELECTORS 96
volatile uint32_t native_aks_selector_hits[CLOVER_AKS_SELECTORS];
volatile int32_t native_aks_selector_rc[CLOVER_AKS_SELECTORS];
volatile uint32_t native_aks_other_calls;
volatile uint32_t native_aks_last_selector;
volatile int32_t native_aks_last_rc;

void native_keyevents_open_refused(void) {
    native_keyevents_refused_opens = native_keyevents_refused_opens + 1;
}

// The driver's own gate: state mode only when this board asks for it.
bool native_aks_state_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        unsigned value = 0;
        cached = PE_parse_boot_argn("clover-aks", &value, sizeof(value)) && value == 1;
    }
    return cached == 1;
}

uint32_t native_aks_lock_state(void) {
    static int cached = -1;
    if (cached < 0) {
        unsigned value = 0;
        if (!PE_parse_boot_argn("clover-aks-sls", &value, sizeof(value)) || value > 7) {
            value = 0;
        }
        cached = (int) value;
    }
    return (uint32_t) cached;
}

void native_aks_opened(void) {
    native_aks_opens = native_aks_opens + 1;
}

void native_aks_call(uint32_t selector, int32_t result) {
    native_aks_calls = native_aks_calls + 1;
    native_aks_last_selector = selector;
    native_aks_last_rc = result;
    if (selector < CLOVER_AKS_SELECTORS) {
        native_aks_selector_hits[selector] = native_aks_selector_hits[selector] + 1;
        native_aks_selector_rc[selector] = result;
    } else {
        native_aks_other_calls = native_aks_other_calls + 1;
    }
}
}

// extern "C" is load-bearing, not decoration: CloverLegacyDisplayProbe.cpp declares this by C name and
// the archive's own contract check reads the symbol table, so a C++-mangled definition here links as
// "defined by no archive member" -- which is what the build says, minutes before the kernel link would
// have said the same thing as an undefined symbol.
extern "C" void clover_keystore_probe(void) {
    unsigned enabled = 0, original = 0;

    if (!PE_parse_boot_argn("offline-key-events", &enabled, sizeof(enabled)) || enabled != 1 ||
        native_keyevents_stage) {
        return;
    }
    native_keyevents_stage = 1;
    PE_parse_boot_argn("offline-keystore", &original, sizeof(original));
    native_keyevents_status = kIOReturnNotPermitted;

    // sheng's check, kept because it is the statement of the problem rather than board trivia: an
    // ephemeral fixture and a persistent store with no SEP both end the same way -- nothing can be
    // unlocked -- but an original keystore loaded offline (offline-keystore=1) wins, and this board
    // does not ask for that. The path is read without a second plane prefix because fromPath with an
    // explicit plane already implies one.
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    if (chosen == NULL) {
        native_keyevents_status = kIOReturnNoDevice;
        return;
    }
    OSData *data = OSDynamicCast(OSData, chosen->getProperty("ephemeral-storage"));
    uint32_t ephemeral = 0;
    if (data != NULL && data->getLength() == sizeof(ephemeral)) {
        memcpy(&ephemeral, data->getBytesNoCopy(), sizeof(ephemeral));
    }
    OSSafeReleaseNULL(chosen);
    if ((ephemeral != 1 && !native_aks_state_enabled()) || original) {
        return;
    }

    // The precondition r275 measured on boot 62, re-checked here rather than assumed from the log:
    // a metaclass registered under this name means an AppleKeyStore exists and two of them is not a
    // fix. This is also the reason the census keeps printing the same lookup -- it is the control.
    const OSSymbol *name = OSSymbol::withCString("AppleKeyStore");
    bool absent = name != NULL && OSMetaClass::getMetaClassWithName(name) == NULL;
    OSSafeReleaseNULL(name);
    if (!absent) {
        native_keyevents_status = kIOReturnBusy;
        return;
    }

    IOService *provider = IOService::getResourceService();
    if (provider == NULL) {
        native_keyevents_status = kIOReturnNoDevice;
        return;
    }

    // The archive's own initializers were stripped by the .late staging (that is what makes adding
    // objects to a linked kernel safe), so the metaclasses the driver declares have to be registered
    // by hand against the kernel's own kmod, exactly as the HID probe does with
    // native_hid_static_init(). If postModLoad fails the classes did not land and nothing is started.
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (handle == NULL) {
        native_keyevents_status = kIOReturnError;
        return;
    }
    native_keyevents_static_init();
    native_keyevents_status = OSMetaClass::postModLoad(handle);
    if (native_keyevents_status != 0) {
        return;
    }
    native_keyevents_stage = 2;

    IOService *service = native_keyevents_allocate();
    bool initialized = service != NULL && service->init();
    bool attached = initialized && service->attach(provider);
    bool started = attached && service->start(provider);
    if (!started) {
        if (attached) {
            service->detach(provider);
        }
        OSSafeReleaseNULL(service);
        native_keyevents_status = kIOReturnError;
        return;
    }
    native_keyevents_service = (uintptr_t) service;
    native_keyevents_status = kIOReturnSuccess;
    native_keyevents_stage = 3;
}
