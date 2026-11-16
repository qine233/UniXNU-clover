// clover r434: the accelerator shell Metal's discovery looks for.
//
// Metal finds a device by IOServiceMatching("IOAcceleratorES") and then reads
// MetalPluginName from that service to load /System/Library/Extensions/<name>.bundle.
// This board has no Apple GPU, so the service is ours: it answers the discovery,
// names the metal2vulkan plugin, and hands every user-client call to a stub whose
// class is deliberately named AGXDeviceUserClient (the name the stock sandbox
// profiles allow the render server to open).
//
// IOGPUFamily is removed from the prelink roots for this round: it owns the
// IOAcceleratorES name and its base has no operational overrides, so the class
// below is the same IOService-shaped object Metal expects (mondrian measured
// that shape with its empty accelerator). The board probe publishes it once.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOKitKeys.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSBoolean.h>
#include <pexpert/pexpert.h>
#include <libkern/c++/OSMetaClass.h>
#include <mach/kmod.h>
#include <string.h>
#include <libkern/libkern.h>

extern "C" void proc_selfname(char *, int);
extern kmod_info_t g_kernel_kmod_info;
// CloverBlock's shape is not optional here: the OSMetaClass constructor refuses a class whose
// mod load handle has not been created, even when the superclass is kernel-resident. Boot 226
// printed "preModLoad() wasn't called for class IOAcceleratorES" and then took a data abort at
// the class name, which is why the shell never published and the display beat stopped.
extern void clover_accel_static_init(void) __asm__("__GLOBAL__sub_I_CloverAccel.cpp");

static volatile uint32_t gAccelEntries;
static volatile uint32_t gAccelInited;
static volatile uint32_t gAccelDisabled;
static volatile uint32_t gAccelPublished;
static volatile uint32_t gAccelFail;
static volatile uint32_t gAccelCalls;
static volatile uint32_t gAccelOpens;
static volatile uint32_t gAccelReads;
static uint32_t gAccelSelectors[3];
static char gAccelReader[17];
static char gAccelClient[17];
// Round437: the first opener name alone could not tell the render server from the media server. Every
// open is counted under its process name; the table is tiny because the interesting boot has < 4.
struct CloverAccelOpenRow { char name[17]; uint32_t opens; };
static CloverAccelOpenRow gAccelOpenBy[6];
static uint32_t gAccelOpenNames;
static uint32_t gAccelOpenOther;
static uint32_t gAccelSelCalls[16];
// Round439: the plugin's progress markers are IOServiceOpen calls of type 0x4147<value>, not
// external methods (NativeAdrenoUserTest.h Gpu::mark), so r437's selector counters could not see
// them and every mark landed in the per-name open count. Classify the type instead: the last mark
// names the step the published device reached, and backboardd's first four marks are its path
// (0x6800 entered, 0x6801 Turnip found, 0x6810+i cache dir served, 0x6802/0x6803 device made).
static uint32_t gAccelMarks;
static uint32_t gAccelMarkLast;
static char gAccelMarkName[17];
static uint32_t gAccelBBMarkCount;
static uint32_t gAccelBBMarks[4];

class AGXDeviceUserClient : public IOUserClient {
	OSDeclareDefaultStructors(AGXDeviceUserClient)
public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type,
	    OSDictionary *properties) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(AGXDeviceUserClient, IOUserClient)

bool AGXDeviceUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type,
    OSDictionary *properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	char name[17] = "?";
	proc_selfname(name, sizeof(name));
	if (__atomic_add_fetch(&gAccelOpens, 1, __ATOMIC_RELAXED) == 1) {
		strlcpy(gAccelClient, name, sizeof(gAccelClient));
	}
	{
		uint32_t slot = gAccelOpenNames;
		for (uint32_t i = 0; i < gAccelOpenNames; i++) {
			if (!strcmp(gAccelOpenBy[i].name, name)) {
				slot = i;
				break;
			}
		}
		if (slot < gAccelOpenNames) {
			__atomic_add_fetch(&gAccelOpenBy[slot].opens, 1, __ATOMIC_RELAXED);
		} else if (slot < (uint32_t)(sizeof gAccelOpenBy / sizeof gAccelOpenBy[0])) {
			strlcpy(gAccelOpenBy[slot].name, name, sizeof gAccelOpenBy[slot].name);
			__atomic_store_n(&gAccelOpenBy[slot].opens, 1, __ATOMIC_RELAXED);
			__atomic_store_n(&gAccelOpenNames, slot + 1, __ATOMIC_RELEASE);
		} else {
			__atomic_add_fetch(&gAccelOpenOther, 1, __ATOMIC_RELAXED);
		}
	}
	if ((type & 0xffff0000u) == 0x41470000u) {
		uint32_t value = type & 0xffffu;
		__atomic_add_fetch(&gAccelMarks, 1, __ATOMIC_RELAXED);
		__atomic_store_n(&gAccelMarkLast, value, __ATOMIC_RELAXED);
		strlcpy(gAccelMarkName, name, sizeof(gAccelMarkName));
		if (!strcmp(name, "backboardd")) {
			uint32_t count = gAccelBBMarkCount;
			if (count < (sizeof gAccelBBMarks / sizeof gAccelBBMarks[0])) {
				gAccelBBMarks[count] = value;
				__atomic_store_n(&gAccelBBMarkCount, count + 1, __ATOMIC_RELEASE);
			} else {
				gAccelBBMarks[0] = gAccelBBMarks[1];
				gAccelBBMarks[1] = gAccelBBMarks[2];
				gAccelBBMarks[2] = gAccelBBMarks[3];
				gAccelBBMarks[3] = value;
			}
		}
	}
	IOLog("CloverAccel r434: user client opened by %s type %u\n", name, type);
	return true;
}

IOReturn AGXDeviceUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

IOReturn AGXDeviceUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *dispatch __unused, OSObject *target __unused, void *reference __unused)
{
	for (uint32_t i = 0; i < args->scalarOutputCount && i < 16; i++) {
		args->scalarOutput[i] = 0;
	}
	if (args->structureOutput != NULL && args->structureOutputSize > 0) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	uint32_t calls = __atomic_add_fetch(&gAccelCalls, 1, __ATOMIC_RELAXED);
	if (calls <= 3) {
		gAccelSelectors[calls - 1] = selector;
	}
	if (selector < (sizeof gAccelSelCalls / sizeof gAccelSelCalls[0])) {
		__atomic_add_fetch(&gAccelSelCalls[selector], 1, __ATOMIC_RELAXED);
	}
	if (calls <= 32) {
		char name[17] = "?";
		proc_selfname(name, sizeof(name));
		IOLog("CloverAccel r434: %s selector %u answered (%u in, %u out)\n",
		    name, selector, args->scalarInputCount, args->scalarOutputCount);
	}
	return kIOReturnSuccess;
}

class IOAcceleratorES : public IOService {
	OSDeclareDefaultStructors(IOAcceleratorES)
public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
	    OSDictionary *properties, IOUserClient **handler) APPLE_KEXT_OVERRIDE;
	OSObject *copyProperty(const OSSymbol *key) const APPLE_KEXT_OVERRIDE;
	OSObject *copyProperty(const OSString *key) const APPLE_KEXT_OVERRIDE;
	OSObject *copyProperty(const char *key) const APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(IOAcceleratorES, IOService)

bool IOAcceleratorES::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "AGXDeviceUserClient");
	setProperty("MetalPluginName", "NativeAdrenoMetal");
	setProperty("IOAcceleratorStub", kOSBooleanTrue);
	registerService();
	IOLog("CloverAccel r434: IOAcceleratorES published, MetalPluginName NativeAdrenoMetal\n");
	return true;
}

IOReturn IOAcceleratorES::newUserClient(task_t owningTask, void *securityID, UInt32 type,
    OSDictionary *properties, IOUserClient **handler)
{
	AGXDeviceUserClient *client = OSTypeAlloc(AGXDeviceUserClient);
	if (!client) {
		return kIOReturnNoMemory;
	}
	if (!client->initWithTask(owningTask, securityID, type, properties)) {
		client->release();
		return kIOReturnError;
	}
	if (!client->attach(this)) {
		client->release();
		return kIOReturnError;
	}
	if (!client->start(this)) {
		client->detach(this);
		client->release();
		return kIOReturnError;
	}
	*handler = client;
	return kIOReturnSuccess;
}

static void note_read(const char *key)
{
	if (key && !strcmp(key, "MetalPluginName")) {
		uint32_t reads = __atomic_add_fetch(&gAccelReads, 1, __ATOMIC_RELAXED);
		char name[17] = "?";
		proc_selfname(name, sizeof(name));
		if (reads == 1) {
			strlcpy(gAccelReader, name, sizeof(gAccelReader));
		}
		if (reads <= 16) {
			IOLog("CloverAccel r434: MetalPluginName read by %s (%u)\n", name, reads);
		}
	}
}

OSObject *IOAcceleratorES::copyProperty(const OSSymbol *key) const
{
	note_read(key ? key->getCStringNoCopy() : nullptr);
	return IORegistryEntry::copyProperty(key);
}
OSObject *IOAcceleratorES::copyProperty(const OSString *key) const
{
	note_read(key ? key->getCStringNoCopy() : nullptr);
	return IORegistryEntry::copyProperty(key);
}
OSObject *IOAcceleratorES::copyProperty(const char *key) const
{
	note_read(key);
	return IORegistryEntry::copyProperty(key);
}

extern "C" void clover_accel_run(void)
{
	// Round434b: a plain latch made "never called", "disabled" and "published" read the same
	// through the beat, which is the one thing the shell exists to tell apart. Each stage keeps
	// its own counter instead; the beat prints all four.
	__atomic_add_fetch(&gAccelEntries, 1, __ATOMIC_RELAXED);
	uint32_t enabled = 1;
	PE_parse_boot_argn("clover-accel", &enabled, sizeof(enabled));
	if (!enabled) {
		__atomic_add_fetch(&gAccelDisabled, 1, __ATOMIC_RELAXED);
		IOLog("CloverAccel r434: disabled by clover-accel=0\n");
		return;
	}
	if (__atomic_exchange_n(&gAccelInited, 1, __ATOMIC_ACQ_REL) == 0) {
		void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
		if (!handle) {
			__atomic_add_fetch(&gAccelFail, 1, __ATOMIC_RELAXED);
			IOLog("CloverAccel r434: preModLoad refused the class\n");
			return;
		}
		clover_accel_static_init();
		if (OSMetaClass::postModLoad(handle)) {
			__atomic_add_fetch(&gAccelFail, 1, __ATOMIC_RELAXED);
			IOLog("CloverAccel r434: postModLoad refused the class\n");
			return;
		}
	}
	if (__atomic_load_n(&gAccelPublished, __ATOMIC_ACQUIRE) ||
	    __atomic_load_n(&gAccelFail, __ATOMIC_ACQUIRE)) {
		return;
	}
	IOPlatformExpert *platform = IOService::getPlatform();
	IOAcceleratorES *accel = OSTypeAlloc(IOAcceleratorES);
	if (!platform || !accel || !accel->init(nullptr) ||
	    !accel->attach(platform) || !accel->start(platform)) {
		__atomic_add_fetch(&gAccelFail, 1, __ATOMIC_RELAXED);
		IOLog("CloverAccel r434: publish failed (platform %d accel %d)\n",
		    platform != nullptr, accel != nullptr);
		if (accel) {
			accel->release();
		}
		return;
	}
	__atomic_store_n(&gAccelPublished, 1, __ATOMIC_RELEASE);
}

// Read by the display state beat (package/CloverLegacyDisplayProbe.cpp) so the shell's own
// counters survive the 512-line kernel ring: the publish happens tens of seconds before the
// first user process, and its IOLog lines are gone long before anyone reads the card.
// Round434e: the display beat's copy of this row never reached the card (the kernel ring is a
// class-budget ring and the old slots survived), so the publish point writes its own line into the
// driver's probe ring instead. That ring is drained by the driver every beat, and a 17-second line
// is exactly what it is for. This is the reading that separates "never called" from "disabled"
// from "published".
extern "C" void clover_accel_state(uint32_t *published, uint32_t *fail, uint32_t *reads,
    uint32_t *opens, uint32_t *calls, char *reader, uint32_t readerLength, char *client,
    uint32_t clientLength, uint32_t *selectors, uint32_t *entries, uint32_t *inited,
    uint32_t *disabled);

extern "C" unsigned int clover_emmc_probe_say(const char *text, unsigned int length);

extern "C" void clover_accel_clients(char *out, uint32_t length)
{
	if (!out || !length) {
		return;
	}
	size_t used = 0;
	out[0] = 0;
	uint32_t names = __atomic_load_n(&gAccelOpenNames, __ATOMIC_ACQUIRE);
	for (uint32_t i = 0; i < names && used + 24 < length; i++) {
		uint32_t count = __atomic_load_n(&gAccelOpenBy[i].opens, __ATOMIC_RELAXED);
		int n = snprintf(out + used, length - used, "%s%s:%u", used ? "," : "",
		    gAccelOpenBy[i].name, (unsigned)count);
		if (n <= 0) {
			break;
		}
		used += (size_t)n;
	}
	uint32_t other = __atomic_load_n(&gAccelOpenOther, __ATOMIC_RELAXED);
	if (other && used + 20 < length) {
		int n = snprintf(out + used, length - used, "%sother:%u", used ? "," : "", (unsigned)other);
		if (n > 0) {
			used += (size_t)n;
		}
	}
	if (!used) {
		strlcpy(out, "-", length);
	}
}

extern "C" uint32_t clover_accel_selector_calls(uint32_t selector)
{
	return selector < (sizeof gAccelSelCalls / sizeof gAccelSelCalls[0])
	    ? __atomic_load_n(&gAccelSelCalls[selector], __ATOMIC_RELAXED) : 0;
}

extern "C" void clover_accel_marks(char *out, uint32_t length)
{
	if (!out || !length) {
		return;
	}
	char name[17] = "-";
	strlcpy(name, gAccelMarkName[0] ? gAccelMarkName : "-", sizeof(name));
	snprintf(out, length, "mks=%u lmk=%04x mkn=%s bbc=%u bbh=%04x,%04x,%04x,%04x",
	    (unsigned)__atomic_load_n(&gAccelMarks, __ATOMIC_RELAXED),
	    (unsigned)__atomic_load_n(&gAccelMarkLast, __ATOMIC_RELAXED), name,
	    (unsigned)__atomic_load_n(&gAccelBBMarkCount, __ATOMIC_ACQUIRE),
	    (unsigned)gAccelBBMarks[0], (unsigned)gAccelBBMarks[1],
	    (unsigned)gAccelBBMarks[2], (unsigned)gAccelBBMarks[3]);
}

extern "C" void clover_accel_report(void)
{
	uint32_t pub = 0, fail = 0, reads = 0, opens = 0, calls = 0, sel[3] = { 0, 0, 0 };
	uint32_t entries = 0, inited = 0, disabled = 0;
	char reader[17] = "-", client[17] = "-", by[96] = "-";
	char line[256];

	clover_accel_state(&pub, &fail, &reads, &opens, &calls, reader, sizeof(reader),
	    client, sizeof(client), sel, &entries, &inited, &disabled);
	clover_accel_clients(by, sizeof(by));
	int n = snprintf(line, sizeof(line),
	    "clover r434: accel pub=%u fail=%u ent=%u init=%u dis=%u reads=%u rdr=%s opens=%u cli=%s calls=%u sel=%u,%u,%u by=%s selc=%u,%u,%u\n",
	    (unsigned)pub, (unsigned)fail, (unsigned)entries, (unsigned)inited, (unsigned)disabled,
	    (unsigned)reads, reader, (unsigned)opens, client, (unsigned)calls,
	    (unsigned)sel[0], (unsigned)sel[1], (unsigned)sel[2], by,
	    (unsigned)clover_accel_selector_calls(0), (unsigned)clover_accel_selector_calls(1),
	    (unsigned)clover_accel_selector_calls(2));
	if (n > 0) {
		if ((unsigned)n >= sizeof(line)) {
			n = (int)sizeof(line) - 1;
		}
		clover_emmc_probe_say(line, (unsigned)n);
	}
	char marks[128];
	clover_accel_marks(marks, sizeof(marks));
	n = snprintf(line, sizeof(line), "clover r439: accelmk %s\n", marks);
	if (n > 0) {
		if ((unsigned)n >= sizeof(line)) {
			n = (int)sizeof(line) - 1;
		}
		clover_emmc_probe_say(line, (unsigned)n);
	}
}

extern "C" void clover_accel_state(uint32_t *published, uint32_t *fail, uint32_t *reads,
    uint32_t *opens, uint32_t *calls, char *reader, uint32_t readerLength, char *client,
    uint32_t clientLength, uint32_t *selectors, uint32_t *entries, uint32_t *inited,
    uint32_t *disabled)
{
	*published = __atomic_load_n(&gAccelPublished, __ATOMIC_ACQUIRE);
	*fail = __atomic_load_n(&gAccelFail, __ATOMIC_ACQUIRE);
	*reads = __atomic_load_n(&gAccelReads, __ATOMIC_ACQUIRE);
	*opens = __atomic_load_n(&gAccelOpens, __ATOMIC_ACQUIRE);
	*calls = __atomic_load_n(&gAccelCalls, __ATOMIC_ACQUIRE);
	if (reader && readerLength) {
		strlcpy(reader, gAccelReader[0] ? gAccelReader : "-", readerLength);
	}
	if (client && clientLength) {
		strlcpy(client, gAccelClient[0] ? gAccelClient : "-", clientLength);
	}
	for (uint32_t i = 0; i < 3; i++) {
		selectors[i] = gAccelSelectors[i];
	}
	*entries = __atomic_load_n(&gAccelEntries, __ATOMIC_ACQUIRE);
	*inited = __atomic_load_n(&gAccelInited, __ATOMIC_ACQUIRE);
	*disabled = __atomic_load_n(&gAccelDisabled, __ATOMIC_ACQUIRE);
}
