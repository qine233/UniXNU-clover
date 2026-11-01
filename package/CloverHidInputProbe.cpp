// SPDX-License-Identifier: BSD-2-Clause-Patent
// Round255. An owned offline HID device, attached under the original IOHIDResource.
//
// Why this exists. backboardd never answers SpringBoard because a dispatch group it waits on
// forever is only left by a ButtonResolver completion block, and that block is only built for a
// process whose HID service carries the keys the resolver filters on. Sixteen rounds of ADT work
// could not move that wait because the wait was never about a registry number: home-button-type is
// published, reachable and read back as 2 by backboardd itself (boots 39/40/42, `path=3 key=3`,
// `home-button-type len=4 val=2`). Round253 then showed the other wrong door: Apple's own
// AppleM68Buttons does match /J120AP/buttons, starts, and panics inside handleStart at 12.7 s
// because it builds function-button_<name> for every name on its own hardcoded list and a
// synthesized ADT node supplies none of them. UniXNU-sheng publishes the identical buttons node,
// reaches the desktop, and never runs that driver -- what it runs instead is this shape, and the
// whole point of it is that nothing about the original HID implementation is replaced:
//
//   load IOHIDFamily and IOHIDEventDriver by identifier at runtime, read the five HID class sizes
//   back out of the metaclass table, wait for the original IOHIDResource, register this board's
//   own OSMetaClass against the kernel's own kmod, then init/attach/start an IOHIDDevice subclass
//   on that resource -- and once the original IOHIDEventDriver has matched below it, publish on
//   the event service (and on its parent interface) exactly the properties backboardd filters on.
//
// scripts/build-clover-hid-input.py is what makes the subclass legal to compile at all: it reads
// IOHIDDevice's real 207-slot table out of this board's linked kernel and refuses the build unless
// every slot, thunk and byte the driver depends on is the original one.
//
// Gates: offline-hid-input=1 arms the probe, offline-hid-home=1 asks for the Home-capable device.
// Both are sheng's names; they describe an offline input source, not a board, so there is no
// reason to invent a second spelling. CloverInput (round181) is the touch instrument and has no
// HID producer yet, so the six symbols NativeHidInput links against for touch are defined here
// empty: count stays 0, the seqlock never moves, and the driver's tick sends no finger report.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSKext.h>
#include <pexpert/pexpert.h>
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" void native_hid_static_init(void) __asm__("__GLOBAL__sub_I_NativeHidInput.cpp");
extern "C" IOService *native_hid_input_allocate(void);

// Defined by CloverLegacyDisplayProbe.cpp: the count of swaps the original validator accepted,
// which is what "the screen is actually being driven" means on this board.
extern "C" volatile uint32_t native_legacy_transfer_calls;

extern "C" {
// Touch inputs of NativeHidInput, with nothing behind them on this board yet.
volatile uint32_t native_touch_seq, native_touch_count, native_touch_hid_reports;
volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
// What happened, in the order the steps run: 1 armed, 2 families loaded, 3 resource found,
// 4 metaclass registered, 5 device started (or the negative step that refused). Signed so a
// refusal reads as the step it refused at rather than as 4294967292.
volatile int32_t clover_hid_stage;
volatile uint32_t clover_hid_resource_found, clover_hid_class_sizes[5];
volatile int clover_hid_load_status = -1, clover_hid_matching_load_status = -1, clover_hid_meta_status = -1;
volatile uint64_t clover_hid_device;
volatile uint32_t clover_hid_events[8];
volatile int clover_hid_results[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
volatile uint32_t clover_hid_published, clover_hid_property_seen, clover_hid_presses;
volatile uint32_t clover_hid_sink_touch, clover_hid_sink_buttons;
}

// The driver asks this before it sends anything, so a device that exists before the panel does
// cannot put events in front of a display. A Home press is exactly what backboardd is waiting for,
// but the driver's tick would otherwise press one every 1.1 s for the rest of the boot: this grants
// a bounded number, three by default and offline-hid-auto-home-count=<n<=8> to change it, which is
// sheng's spelling. Only the device's own work loop calls this, so a plain counter needs no lock.
extern "C" bool native_hid_input_requested(void) {
    static uint32_t budget;
    static bool configured;
    if (native_legacy_transfer_calls < 2) { return false; }
    if (!configured) {
        configured = true;
        budget = 3;
        unsigned requested = 0;
        if (PE_parse_boot_argn("offline-hid-auto-home-count", &requested, sizeof requested) && requested <= 8) {
            budget = requested;
        }
    }
    uint32_t taken = __atomic_fetch_add(&clover_hid_presses, 1, __ATOMIC_RELAXED);
    return taken < budget;
}

extern "C" void native_hid_input_observed(unsigned event, int result) {
    if (event < 8) { clover_hid_events[event] = clover_hid_events[event] + 1; clover_hid_results[event] = result; }
}

// The driver hands its work-loop actions to two producers: the touch instrument, which publishes a
// panel frame, and the button instrument, which publishes a key press. Neither exists on this board
// yet -- round181's I2C probe has no proven rails and round255's own device is the only producer it
// has -- and a weak import is not "optional" to a static kernel link: ld refuses the image over it
// either way, which is what the first r255 build measured. So the board defines both, keeps what the
// driver offers, and calls nothing. The counters are the honest half of that: they say the driver
// reached for a producer and this board had none, which is a reading, not a silent gap.
// answered with an undefined weak symbol, so the board defines both and calls nothing. The counters
// are defined in the block above, the same place every other reading of this instrument lives.
extern "C" void native_touch_set_sink(void (*sink)(void *), void *context) {
    (void)sink; (void)context;
    clover_hid_sink_touch = clover_hid_sink_touch + 1;
}
extern "C" void native_buttons_set_sink(void (*sink)(void *, uint32_t), void *context) {
    (void)sink; (void)context;
    clover_hid_sink_buttons = clover_hid_sink_buttons + 1;
}

// The classification the digitizer and button services are judged by, published once the original
// driver has matched -- both halves of that sentence are load-bearing, and both are sheng's
// readings of the target rather than guesses:
//   * set it on the IOHIDEventDriver the device grew below, and on that driver's parent interface,
//     because backboardd reads the service it opened, not the HID device two levels under it;
//   * set it after matching, because a property on the device changes which driver matches, and
//     matching depends on the report-descriptor layout this device publishes;
//   * set it exactly once, because every setProperty is a registry property-change notification and
//     repeating it keeps backboardd and hidd busy re-evaluating the same service.
__attribute__((noinline)) static void describeDriver(IORegistryEntry *driver, size_t &used, char *into,
                                                     size_t capacity) {
    static const char *keys[] = {"Built-In", "PrimaryUsagePage", "PrimaryUsage"};
    for (const char *key : keys) {
        OSObject *value = driver->copyProperty(key);
        used += (size_t)snprintf(into + used, used < capacity ? capacity - used : 0, " %s=%d", key,
                                 value != nullptr);
        if (value) { value->release(); }
    }
}

static IORegistryEntry *findEventDriver(IORegistryEntry *parent, unsigned depth) {
    if (!depth) { return nullptr; }
    OSIterator *iterator = parent->getChildIterator(gIOServicePlane);
    if (!iterator) { return nullptr; }
    IORegistryEntry *found = nullptr;
    while (OSObject *child = iterator->getNextObject()) {
        IORegistryEntry *entry = OSDynamicCast(IORegistryEntry, child);
        if (!entry) { continue; }
        if (entry->metaCast("IOHIDEventDriver")) { found = entry; break; }
        if ((found = findEventDriver(entry, depth - 1))) { break; }
    }
    iterator->release();
    return found;
}

// Called by the driver once per second with the device, from its own timer. The card has a ring of
// its own and a per-class budget on top of it, so this prints while the picture is still changing
// and stops: eight lines cover absent device, unmatched driver, first publish and the readback of
// the properties backboardd is about to be judged on, and the counters say the rest.
extern "C" __attribute__((noinline)) void native_hid_live_checkpoint(IOService *device) {
    static unsigned printed;
    char line[160];
    size_t used = (size_t)snprintf(line, sizeof line, "clover r255: device=%llx", (unsigned long long)clover_hid_device);
    if (device == nullptr) {
        if (printed++ < 8) { printf("%s absent\n", line); }
        return;
    }
    IORegistryEntry *driver = findEventDriver(device, 4);
    if (driver == nullptr) {
        describeDriver(device, used, line, sizeof line);
        if (printed++ < 8) { printf("%s driver=none\n", line); }
        return;
    }
    if (device->getProperty("Built-In") == nullptr) {
        // No keyboard/pointing collection was ever published with Built-In set, so there is
        // nothing to propagate; say so rather than publish a false identity.
        if (printed++ < 8) { printf("%s device-has-no-built-in\n", line); }
        return;
    }
    static IORegistryEntry *publishedFor;
    if (publishedFor != driver) {
        publishedFor = driver;
        driver->setProperty("Built-In", kOSBooleanTrue);
        driver->setProperty("DisplayIntegrated", kOSBooleanTrue);
        // -[BKHIDDirectTouchEventProcessor processEvent:sender:dispatcher:] drops a digitizer event
        // unless the sending service is usage page 0x0D with usage 1, 4 or 0x20; this device lists
        // its keyboard collection first, so the services would otherwise publish (1,6).
        driver->setProperty("PrimaryUsagePage", (unsigned long long)0x0d, 32);
        driver->setProperty("PrimaryUsage", (unsigned long long)4, 32);
        if (IORegistryEntry *interface = driver->getParentEntry(gIOServicePlane)) {
            interface->setProperty("Built-In", kOSBooleanTrue);
            interface->setProperty("DisplayIntegrated", kOSBooleanTrue);
            interface->setProperty("PrimaryUsagePage", (unsigned long long)0x0d, 32);
            interface->setProperty("PrimaryUsage", (unsigned long long)4, 32);
        }
        clover_hid_published = clover_hid_published + 1;
    }
    used = 0;
    describeDriver(driver, used, line, sizeof line);
    clover_hid_property_seen = 1;
    if (printed++ < 8) { printf("clover r255: service=%llx%s\n", (unsigned long long)(uintptr_t)driver, line); }
}

extern "C" void clover_hid_input_probe(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("offline-hid-input", &enabled, sizeof enabled) || enabled != 1) { return; }
    if (clover_hid_stage) { return; }
    clover_hid_stage = 1;
    clover_hid_load_status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily");
    if (!clover_hid_load_status) {
        clover_hid_matching_load_status = OSKext::loadKextWithIdentifier("com.apple.iokit.IOHIDEventDriver",
                                                                        true, false, kOSKextExcludeNone,
                                                                        kOSKextExcludeNone, nullptr);
    }
    clover_hid_stage = 2;
    static const char *names[] = {"IOHIDUserDevice", "IOHIDEventDriver", "IOHIDEventService",
                                  "IOHIDResourceDeviceUserClient", "IOHIDDevice"};
    for (unsigned i = 0; i < 5; ++i) {
        const OSSymbol *name = OSSymbol::withCString(names[i]);
        const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
        if (meta) { clover_hid_class_sizes[i] = meta->getClassSize(); }
        OSSafeReleaseNULL(name);
    }
    if (clover_hid_load_status) {
        printf("clover r255: IOHIDFamily load=%d, no device\n", clover_hid_load_status);
        return;
    }
    // The subclass is only legal against the image scripts/build-clover-hid-input.py qualified:
    // instance size 208 is the static_assert the header carries, so a family that grew would have
    // failed the build, and this is the same claim read from the running kernel instead.
    if (clover_hid_class_sizes[4] != 208) {
        printf("clover r255: IOHIDDevice size=%u not 208, refusing\n", clover_hid_class_sizes[4]);
        clover_hid_stage = -4;
        return;
    }
    OSDictionary *matching = IOService::serviceMatching("IOHIDResource");
    IOService *resource = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    clover_hid_resource_found = resource != nullptr;
    clover_hid_stage = 3;
    unsigned home = 0;
    PE_parse_boot_argn("offline-hid-home", &home, sizeof home);
    if (resource && home == 1) {
        void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
        if (handle) {
            native_hid_static_init();
            clover_hid_meta_status = OSMetaClass::postModLoad(handle);
            if (!clover_hid_meta_status) {
                IOService *device = native_hid_input_allocate();
                clover_hid_device = (uintptr_t)device;
                bool valid = device && device->metaCast("IOHIDDevice") && device->init();
                native_hid_input_observed(0, valid ? 0 : kIOReturnError);
                bool attached = valid && device->attach(resource);
                native_hid_input_observed(1, attached ? 0 : kIOReturnError);
                bool started = attached && device->start(resource);
                if (started) {
                    clover_hid_stage = 5;
                } else {
                    native_hid_input_observed(7, kIOReturnError);
                    if (attached) { device->detach(resource); }
                    OSSafeReleaseNULL(device);
                    clover_hid_device = 0;
                    clover_hid_stage = -5;
                }
            } else {
                clover_hid_stage = -4;
            }
        } else {
            clover_hid_stage = -3;
        }
    } else if (!resource) {
        clover_hid_stage = -2;
    }
    OSSafeReleaseNULL(resource);
    OSSafeReleaseNULL(matching);
    printf("clover r255: stage=%d load=%d matching=%d meta=%d resource=%u sizes=%u,%u,%u,%u,%u "
           "device=%llx transfer=%u presses=%u sinks=%u,%u\n", clover_hid_stage, clover_hid_load_status,
           clover_hid_matching_load_status, clover_hid_meta_status, clover_hid_resource_found,
           clover_hid_class_sizes[0], clover_hid_class_sizes[1], clover_hid_class_sizes[2],
           clover_hid_class_sizes[3], clover_hid_class_sizes[4],
           (unsigned long long)clover_hid_device, native_legacy_transfer_calls, clover_hid_presses,
           clover_hid_sink_touch, clover_hid_sink_buttons);
}
