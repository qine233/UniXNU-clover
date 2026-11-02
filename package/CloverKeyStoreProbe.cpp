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
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
// IOMemoryMap is declared in IOMemoryDescriptor.h in this SDK; there is no IOMemoryMap.h, and the
// drivers/CloverInput mapping this file copies gets the class from the same header.
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <string.h>
#include <kern/task.h>
#include <sys/proc.h>

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

/*
 * Round281. Round280 built one late table so a row would outlive the ring's horizon, and boot 68 answered
 * the other half of that lesson: the table spoke at 1600 s and the board was gone at 1324.217 s, so the
 * shape that keeps a row for 355 s was still silent -- it had one speaking point, and the stop came
 * before it. These stops are intermittent (726, 1324 and 1499 s against 1813, 1814 and 1814 s on the
 * same image family), so the reading cannot be a moment. This round speaks on a schedule instead: one
 * state row every 120 s starting at 240 s, tables every fifth row. Wherever the board stops, its last
 * row is at most two minutes old, and the ring keeps ten times that.
 *
 * The same beat reads the APSS watchdog, because a stop with no panic record and no log tail is what a
 * hardware bite looks like. The block is 17817000.qcom,wdt -- TWRP's dmesg names it and /proc/iomem
 * marks 17817000-17817fff as msm-watchdog -- and the offsets are the downstream driver's
 * (drivers/soc/qcom/watchdog_v2.c, the one this chip's Linux boots with): RST 0x04, EN 0x08, STS 0x0c,
 * BARK_TIME 0x10, BITE_TIME 0x14, with EN bit 0, UNMASKED_INT_EN bit 1, and both time registers counted
 * in ticks of WDT_HZ=32765. Read only: nothing here writes RST, so this instrument cannot change the
 * thing it measures. If the map fails the row says nomap and the state row still arrives, which keeps
 * one broken reading from hiding the other.
 *
 * Why the counters stay in the census. opens=/calls=/sel7=/sel14=/sel17=/sel35= are printed from the
 * census at 1543 s on every boot that reaches it: if the cadence rows are missing and those counters are
 * not, the instrument failed and the events happened, which is the distinction a single-channel reading
 * cannot make on its own.
 */

// IOLib.cpp, declared extern "C" there. It hands back "pid <n>, <name>" as a +1 OSString, and round251
// already measured the cost of printing that from the front: the pid is a field of its own two
// columns later, so only the part after the comma-space is worth the width here.
extern "C" OSString *IOCopyLogNameForPID(int pid);

static const char *
clover_aks_short(const char *logname)
{
    const char *comma = logname != NULL ? strchr(logname, ',') : NULL;

    return comma != NULL && comma[1] == ' ' ? comma + 2
         : logname != NULL ? logname : "?";
}

// The caller of the current IOKit call, in the two fields that identify a daemon: pid and comm. Both
// are best-effort -- a NULL bsd task or a failed name copy leaves pid=-1 comm=? -- because a row that
// says who asked is worth having even when the name is missing, and nothing here may fail the call
// the client is making.
static void
clover_aks_who(int *pid_out, char *name_out, size_t name_cap)
{
    task_t   t = current_task();
    proc_t   p = t != NULL ? (proc_t) get_bsdtask_info(t) : NULL;
    int      pid = p != NULL ? proc_pid(p) : -1;
    OSString *logname = pid > 0 ? IOCopyLogNameForPID(pid) : NULL;

    *pid_out = pid;
    if (name_cap == 0) {
        return;
    }
    name_out[0] = '\0';
    if (logname != NULL) {
        const char *short_name = clover_aks_short(logname->getCStringNoCopy());
        size_t      k = 0;
        while (k + 1 < name_cap && short_name[k] != '\0') {
            name_out[k] = short_name[k];
            k++;
        }
        name_out[k] = '\0';
        logname->release();
    }
    // Written a byte at a time, not with strcpy: this kernel's <string.h> makes strcpy a threeargument
    // macro, and the error clang gives for that reads as a broken string literal two lines away.
    if (name_out[0] == '\0' && name_cap >= 2) {
        name_out[0] = '?';
        name_out[1] = '\0';
    }
}

/*
 * Round281's cadence. Round279 printed these events as they happened and boot 67 read nothing: the card's
 * kernel-log ring holds 512 records and that boot kept 758 kernel lines, so what came back covered 1458.7 s
 * to 1814.1 s, and every row printed before the horizon was rotated out. Round280 answered that with one
 * table at 1600 s and boot 68 ended at 1324 s, before the table. Both failures are the same mistake in
 * different clothes: a reading with a fixed speaking time only describes boots that outlive it. So the
 * events are still copied into small tables (pid, comm, selector, return code) and nothing prints on the
 * event path, but the timer now repeats: one state row every 120 s from 240 s, the tables every fifth row.
 */
#define CLOVER_AKS_ROWS        8
#define CLOVER_AKS_CALL_ROWS  12
#define CLOVER_AKS_FIRST_MS    240000u
#define CLOVER_AKS_STATE_MS    120000u
#define CLOVER_AKS_TABLE_EVERY 5

// The watchdog block the two TWRP facts name, and the downstream driver's offsets and tick rate. The
// register numbers are quoted rather than guessed so a row that disagrees with them is a reading of a
// different block, not a decoding question.
#define CLOVER_WDT_PHYS        0x17817000ull
#define CLOVER_WDT_BYTES       0x20
#define CLOVER_WDT_EN          0x08
#define CLOVER_WDT_STS         0x0c
#define CLOVER_WDT_BARK        0x10
#define CLOVER_WDT_BITE        0x14
#define CLOVER_WDT_HZ          32765u

struct clover_aks_row {
    int       pid;
    char      comm[20];
    uint32_t  selector;
    int32_t   rc;
};
static clover_aks_row clover_aks_open_rows[CLOVER_AKS_ROWS];
static clover_aks_row clover_aks_call_rows[CLOVER_AKS_CALL_ROWS];
static uint32_t clover_aks_open_rowc, clover_aks_call_rowc;
static uint32_t clover_aks_fires;
static IOTimerEventSource *clover_aks_timer;
static bool clover_aks_armed;
static IOMemoryMap *clover_wdt_map;
static volatile uint32_t *clover_wdt_regs;
static bool clover_wdt_tried;

// Measured, not derived from the fire count: a row that says t= must be able to disagree with the beat
// above it, or it is not a second reading of the same boot.
static unsigned
clover_uptime_seconds(void)
{
    uint64_t uptime = 0;
    uint64_t nsecs = 0;

    clock_get_uptime(&uptime);
    absolutetime_to_nanoseconds(uptime, &nsecs);
    return (unsigned) (nsecs / 1000000000ull);
}

// Mapped on the first fire, not at bsd_autoconf: this reading is wanted beside a state row, and the
// platform's memory picture is settled by then. The map is kept for the boot -- releasing it would only
// unmap the page the next row reads -- so a failed map is remembered rather than retried every two
// minutes.
static void
clover_wdt_open(void)
{
    IODeviceMemory *memory;

    if (clover_wdt_tried) {
        return;
    }
    clover_wdt_tried = true;
    memory = IODeviceMemory::withRange(CLOVER_WDT_PHYS, CLOVER_WDT_BYTES);
    if (memory == NULL) {
        return;
    }
    clover_wdt_map = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    if (clover_wdt_map != NULL) {
        clover_wdt_regs = (volatile uint32_t *) clover_wdt_map->getVirtualAddress();
    }
}

// Same barrier shape as drivers/CloverInput's windowRead: the loads are ordered before the row prints.
static uint32_t
clover_wdt_read(uint32_t offset)
{
    uint32_t value = clover_wdt_regs[offset / 4];

    __asm__ volatile ("dsb sy" ::: "memory");
    return value;
}

// Recorded, never printed: the caller's thread is the client's, and the only thing this path should cost it
// is a copy of thirty-odd bytes.
static void
clover_aks_record(clover_aks_row *rows, uint32_t *filled, uint32_t cap, uint32_t selector, int32_t rc)
{
    if (*filled >= cap) {
        return;
    }
    int  pid = 0;
    char name[20];

    clover_aks_who(&pid, name, sizeof(name));
    rows[*filled].pid = pid;
    rows[*filled].selector = selector;
    rows[*filled].rc = rc;
    size_t k = 0;
    while (k + 1 < sizeof(rows[*filled].comm) && name[k] != '\0') {
        rows[*filled].comm[k] = name[k];
        k++;
    }
    rows[*filled].comm[k] = '\0';
    *filled = *filled + 1;
}

// The scheduled reading. The IOKit timer hands back the owner it was given (the published service, or the
// resource service when the publish did not happen), so the row can name the object whose work loop is
// speaking. The tables are appended by client threads and read here without a lock: a row that is caught
// mid-copy can misname one comm, and that is cheaper than making every client wait on a diagnostic.
static void
clover_aks_say(OSObject *owner, IOTimerEventSource *sender)
{
    unsigned  t = clover_uptime_seconds();
    bool      tables;
    uint32_t  k;

    clover_aks_fires++;
    tables = (clover_aks_fires % CLOVER_AKS_TABLE_EVERY) == 0;

    printf("clover r281: aks t=%u fire=%u stage=%d status=%x service=%llx owner=%s opens=%u calls=%u "
        "refused=%u other=%u\n", t, (unsigned) clover_aks_fires,
        (int) native_keyevents_stage, (unsigned) native_keyevents_status,
        (unsigned long long) native_keyevents_service,
        // getName() belongs to IOObject, not OSObject, and the owner the timer hands back is typed OSObject*;
        // casting is what keeps the row honest about which service it is reporting on rather than printing a
        // pointer twice under two names.
        OSDynamicCast(IOService, owner) != NULL && OSDynamicCast(IOService, owner)->getName() != NULL
            ? OSDynamicCast(IOService, owner)->getName() : "?",
        (unsigned) native_aks_opens, (unsigned) native_aks_calls,
        (unsigned) native_keyevents_refused_opens, (unsigned) native_aks_other_calls);

    // The watchdog row, every fire, beside the state it belongs to: this is the hypothesis under test, and
    // a row that only appears once would be the same mistake round280 made with the tables.
    clover_wdt_open();
    if (clover_wdt_regs == NULL) {
        printf("clover r281: wdt t=%u nomap\n", t);
    } else {
        uint32_t en = clover_wdt_read(CLOVER_WDT_EN);
        uint32_t sts = clover_wdt_read(CLOVER_WDT_STS);
        uint32_t bark = clover_wdt_read(CLOVER_WDT_BARK);
        uint32_t bite = clover_wdt_read(CLOVER_WDT_BITE);

        printf("clover r281: wdt t=%u en=%x armed=%u intmask=%u sts=%x bark=%u bite=%u bark_ms=%u "
            "bite_ms=%u\n", t, en, en & 1u, (en >> 1) & 1u, sts, bark, bite,
            (unsigned) ((uint64_t) bark * 1000ull / CLOVER_WDT_HZ),
            (unsigned) ((uint64_t) bite * 1000ull / CLOVER_WDT_HZ));
    }

    if (!tables) {
        goto rearm;
    }
    for (k = 0; k < CLOVER_AKS_SELECTORS; k++) {
        if (k != 7 && k != 14 && k != 17 && k != 35) {
            continue;
        }
        printf("clover r281: aks sel%u n=%u rc=%x\n", k,
            (unsigned) native_aks_selector_hits[k], (unsigned) native_aks_selector_rc[k]);
    }
    for (k = 0; k < clover_aks_open_rowc; k++) {
        printf("clover r281: open %u pid=%d comm=%.16s\n", k, clover_aks_open_rows[k].pid,
            clover_aks_open_rows[k].comm);
    }
    for (k = 0; k < clover_aks_call_rowc; k++) {
        printf("clover r281: call %u pid=%d comm=%.16s sel=%u rc=%x\n", k,
            clover_aks_call_rows[k].pid, clover_aks_call_rows[k].comm,
            (unsigned) clover_aks_call_rows[k].selector, (unsigned) clover_aks_call_rows[k].rc);
    }
    // The honesty row: how much of what happened never reached a table. Without it a full table and a busy
    // system look the same, which is the mistake boot 67 just cost a round.
    printf("clover r281: rows open=%u/%u call=%u/%u dropped=%u\n", clover_aks_open_rowc,
        (unsigned) native_aks_opens, clover_aks_call_rowc, (unsigned) native_aks_calls,
        (unsigned) (native_aks_opens + native_aks_calls > clover_aks_open_rowc + clover_aks_call_rowc
            ? native_aks_opens + native_aks_calls - clover_aks_open_rowc - clover_aks_call_rowc : 0));

rearm:
    // Re-armed from inside the callback: wakeAtTime() takes a new deadline while the source is enabled,
    // and that is how a repeating IOKit timer is spelled. If the service is torn down the source goes with
    // its work loop, which is the correct end of the schedule.
    if (sender != NULL) {
        (void) sender->setTimeoutMS(CLOVER_AKS_STATE_MS);
    }
}

// Arm the schedule. It belongs to the instrument rather than to the published service: the watchdog row is
// not this service's reading, so an armed-but-withheld keystore must not take the hardware question down
// with it. Called with NULL first (the resource service's work loop), and again with the service after a
// successful publish only when that first attempt had nowhere to run.
static void
clover_aks_arm(IOService *service)
{
    IOService  *owner;
    IOWorkLoop *wl;

    if (clover_aks_armed) {
        return;
    }
    owner = service != NULL ? service : IOService::getResourceService();
    wl = owner != NULL ? owner->getWorkLoop() : NULL;
    if (wl == NULL) {
        return;
    }
    clover_aks_timer = IOTimerEventSource::timerEventSource(owner, clover_aks_say);
    if (clover_aks_timer == NULL || wl->addEventSource(clover_aks_timer) != kIOReturnSuccess) {
        OSSafeReleaseNULL(clover_aks_timer);
        return;
    }
    clover_aks_armed = true;
    // Kept on purpose: the work loop holds the only reference it needs, and freeing this would cancel the
    // one reading this round exists to take.
    (void) clover_aks_timer->setTimeoutMS(CLOVER_AKS_FIRST_MS);
}

void native_keyevents_open_refused(void) {
    native_keyevents_refused_opens = native_keyevents_refused_opens + 1;
    clover_aks_record(clover_aks_open_rows, &clover_aks_open_rowc, CLOVER_AKS_ROWS, 0xffffffffu, -1);
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
    clover_aks_record(clover_aks_open_rows, &clover_aks_open_rowc, CLOVER_AKS_ROWS, 0xfffffffeu, 0);
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
    // First twelve, then silence the tables can be honest about: the census row prints the totals, and the
    // late rows print how many events never got a table slot.
    clover_aks_record(clover_aks_call_rows, &clover_aks_call_rowc, CLOVER_AKS_CALL_ROWS, selector, result);
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
    // The schedule starts here, before any step below can refuse the publish: its watchdog row is not this
    // service's reading, and r279/r280 both lost rounds to a schedule that only existed after the thing it
    // measured succeeded. The first row is still 240 s away, well past bsd_autoconf.
    clover_aks_arm(NULL);
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

    // Round278's bisect knob is gone, as that round promised it would be: boot 66 read the withheld
    // shape (stage=2, service=0, ks_names=0 with the census' own class row at 1) and lived to the
    // deadline, while the two boots that published the service did not, so the service is the
    // variable and the question is now what a client does with it rather than whether to hand it out.

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
    /*
     * The publish itself is not printed here any more. Round279 printed it at bsd_autoconf, and boot 67
     * showed why that buys nothing: a line from the first second of the kernel's life is behind the ring's
     * 355 s horizon before anything can read it. The census prints the same stage and pointer at 1543 s --
     * inside the horizon -- and the scheduled row prints them every 120 s, so the publish has two readings
     * that both arrive after the ring learned to keep them.
     */
    // Retries only if the early arm found no work loop; the guard inside makes this a no-op otherwise.
    clover_aks_arm(service);
}
