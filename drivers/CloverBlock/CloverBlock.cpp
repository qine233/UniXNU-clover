// SPDX-License-Identifier: MIT
// clover (Mi Pad 4, SDM660): publish the eMMC as an IOBlockStorageDevice so XNU's own storage
// stack, not this port's private PIO report, is what reads the card.
//
// Round 106 left the card selected by drivers/CloverSdhci with a working paced-PIO read, and
// round 104/105 proved logfs survives a reboot when that path reads it. This file is the other
// half: it registers a block device above the same path, lets IOStorageFamily attach its
// IOBlockStorageDriver and create the IOMedia, then reads the GPT and the newest logfs slot
// *through IOMedia* and prints what came back. A working line here means the kernel and the card
// agree through the storage stack, not merely through a driver-private function.
//
// The class has to be hand-written. IOStorageFamily is loaded at runtime from the boot kext
// collection (kernel.macho exports no IOBlockStorageDevice, IOMedia or IOBlockStorageDriver
// symbol), so the standard OSDefineMetaClass* macros cannot be used: they would take the
// superclass metaclass' address at link time. Instead the superclass pointer is filled in after
// OSKextLoadKextWithIdentifier, and every class-owned virtual slot is implemented here, because
// an inherited slot would name a symbol the kernel does not export.
//
// The two base lifecycle symbols that the compiler emits for this class's constructor and
// destructor (IOBlockStorageDevice's are not exported either) are aliased by name to IOService's
// in scripts/build-clover-block.py, where that alias is audited together with every import.
//
// Nothing here panics. CloverSdhci already panicked for its own failures; this report is the
// second %s on that same panic screen, so a block stack that stopped early still names its stage
// instead of leaving the panel identical to a driver-only run.
#define IOKIT_ENABLE_SHARED_PTR 1
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOMedia.h>
#include <libkern/libkern.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSObject.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
kern_return_t OSKextLoadKextWithIdentifier (const char *);
void clover_block_static_init (void) __asm__ ("__GLOBAL__sub_I_CloverBlock.cpp");
// Provided by drivers/CloverSdhci. `ready` is false until mode 21 has read EXT_CSD and gated the
// sector count against the logfs extent, so the block device can never claim a geometry the
// partition table does not describe.
uint32_t clover_emmc_block_ready (void);
uint32_t clover_emmc_block_sectors (void);
uint32_t clover_emmc_block_read (uint32_t lba, uint32_t *words, uint32_t blocks);
// stage,kext-load,class-size,postModLoad,instances,media-sectors,blockSize<<32|writable,
// open,gpt-status,entrySize<<32|entries,logfsLast<<32|index,kind<<32|len,stored-sum,
// computed-sum,header-status,text-status.
volatile uint64_t clover_block_observation[16];
// One %s argument; a failure leaves the later lines at zero and names its stage.
char clover_block_report[512];
}

static const uint32_t kBlockBytes = 512;
static const uint32_t kBlockWords = kBlockBytes / 4;
static const uint32_t kLogSlots = 16;
static const uint32_t kLogMagic0 = 0x554e5855u;   // "UXNU" in memory order
static const uint32_t kLogMagic1 = 0x31474f4cu;   // "LOG1"
static const uint32_t kLogPrefixLength = 23;      // "unixnu clover emmc log\n"

// GPT entry name "logfs" as the bytes an on-disk entry carries: UTF-16LE, then a NUL wchar.
static const uint8_t kLogfsName[12] = {
  0x6c, 0x00, 0x6f, 0x00, 0x67, 0x00, 0x66, 0x00, 0x73, 0x00, 0x00, 0x00
};

// ---------------------------------------------------------------- block device

class CloverBlockDevice : public IOBlockStorageDevice {
public:
  // The base's MetaClass constructor is the only place that can name a runtime superclass.
  class MetaClass : public OSMetaClass {
  public:
    MetaClass();
    virtual OSObject *alloc() const APPLE_KEXT_OVERRIDE;
  };

  static const OSMetaClass *superClass;   // set after IOStorageFamily is loaded
  static MetaClass gMetaClass;

  virtual const OSMetaClass *getMetaClass() const APPLE_KEXT_OVERRIDE { return &gMetaClass; }

  CloverBlockDevice();
  virtual ~CloverBlockDevice() APPLE_KEXT_OVERRIDE;

  static void *operator new(size_t size) { return OSObject::operator new(size); }
  static void operator delete(void *memory, size_t size) { OSObject::operator delete(memory, size); }

  virtual bool init(OSDictionary *properties) APPLE_KEXT_OVERRIDE;
  virtual OSObject *getProperty(const OSSymbol *key) const APPLE_KEXT_OVERRIDE;
  virtual IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;

  virtual IOReturn doEjectMedia(void) APPLE_KEXT_OVERRIDE;
  virtual IOReturn doFormatMedia(UInt64 byteCapacity) APPLE_KEXT_OVERRIDE;
  virtual UInt32 doGetFormatCapacities(UInt64 *capacities,
                                       UInt32 capacitiesMaxCount) const APPLE_KEXT_OVERRIDE;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  virtual IOReturn doSynchronizeCache(void) APPLE_KEXT_OVERRIDE;
#pragma clang diagnostic pop

  virtual char *getVendorString(void) APPLE_KEXT_OVERRIDE;
  virtual char *getProductString(void) APPLE_KEXT_OVERRIDE;
  virtual char *getRevisionString(void) APPLE_KEXT_OVERRIDE;
  virtual char *getAdditionalDeviceInfoString(void) APPLE_KEXT_OVERRIDE;

  virtual IOReturn reportBlockSize(UInt64 *blockSize) APPLE_KEXT_OVERRIDE;
  virtual IOReturn reportEjectability(bool *isEjectable) APPLE_KEXT_OVERRIDE;
  virtual IOReturn reportMaxValidBlock(UInt64 *maxBlock) APPLE_KEXT_OVERRIDE;
  virtual IOReturn reportMediaState(bool *mediaPresent,
                                    bool *changedState = 0) APPLE_KEXT_OVERRIDE;
  virtual IOReturn reportRemovability(bool *isRemovable) APPLE_KEXT_OVERRIDE;
  virtual IOReturn reportWriteProtection(bool *isWriteProtected) APPLE_KEXT_OVERRIDE;
  virtual IOReturn getWriteCacheState(bool *enabled) APPLE_KEXT_OVERRIDE;
  virtual IOReturn setWriteCacheState(bool enabled) APPLE_KEXT_OVERRIDE;

  virtual IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer,
                                    UInt64 block, UInt64 nblks,
                                    IOStorageAttributes *attributes,
                                    IOStorageCompletion *completion) APPLE_KEXT_OVERRIDE;

  virtual IOReturn requestIdle(void) APPLE_KEXT_OVERRIDE;
  virtual IOReturn doUnmap(IOBlockStorageDeviceExtent *extents,
                           UInt32 extentsCount,
                           IOStorageUnmapOptions options = 0) APPLE_KEXT_OVERRIDE;
  virtual IOReturn doSetPriority(IOBlockStorageDeviceExtent *extents,
                                 UInt32 extentsCount,
                                 IOStoragePriority priority) APPLE_KEXT_OVERRIDE;
  virtual IOReturn doSynchronize(UInt64 block, UInt64 nblks,
                                 IOStorageSynchronizeOptions options = 0) APPLE_KEXT_OVERRIDE;
  virtual IOReturn doGetProvisionStatus(UInt64 block, UInt64 nblks,
                                        UInt32 *extentsCount,
                                        IOBlockStorageProvisionDeviceExtent *extents,
                                        IOStorageGetProvisionStatusOptions options = 0)
                                        APPLE_KEXT_OVERRIDE;
};

const OSMetaClass *CloverBlockDevice::superClass = nullptr;
CloverBlockDevice::MetaClass CloverBlockDevice::gMetaClass;

CloverBlockDevice::MetaClass::MetaClass()
  : OSMetaClass("CloverBlockDevice", CloverBlockDevice::superClass, sizeof(CloverBlockDevice))
{
}

OSObject *CloverBlockDevice::MetaClass::alloc() const
{
  return new CloverBlockDevice;
}

CloverBlockDevice::CloverBlockDevice()
  : IOBlockStorageDevice(&gMetaClass)
{
  // The original constructor, whose storage this alias would otherwise skip, is the only
  // place that owns this pointer; every base method that could read it is overridden here.
  _expansionData = nullptr;
  gMetaClass.instanceConstructed();
}

CloverBlockDevice::~CloverBlockDevice()
{
}

static_assert(sizeof(CloverBlockDevice) == 0x90, "Exact IOBlockStorageDevice-derived storage");

bool CloverBlockDevice::init(OSDictionary *properties)
{
  // The base init only adds the Generic device type, which is what IOBlockStorageDriver's
  // personality matches on. It does not touch _expansionData, so calling IOService::init
  // directly (the base's own ctor/dtor are aliased, see the build script) is the same thing.
  if (!IOService::init(properties)) {
    return false;
  }
  return setProperty(kIOBlockStorageDeviceTypeKey, kIOBlockStorageDeviceTypeGeneric);
}

OSObject *CloverBlockDevice::getProperty(const OSSymbol *key) const
{
  // The shared-pointer API deprecates this overload but IOBlockStorageDevice still owns the
  // slot, so the override has to forward to the exact function it replaces.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  return IORegistryEntry::getProperty(key);
#pragma clang diagnostic pop
}

IOReturn CloverBlockDevice::setProperties(OSObject *properties)
{
  return kIOReturnUnsupported;
}

IOReturn CloverBlockDevice::doEjectMedia(void)
{
  return kIOReturnUnsupported;   // fixed, non-removable media
}

IOReturn CloverBlockDevice::doFormatMedia(UInt64 byteCapacity)
{
  return kIOReturnUnsupported;
}

UInt32 CloverBlockDevice::doGetFormatCapacities(UInt64 *capacities,
                                                UInt32 capacitiesMaxCount) const
{
  return 0;   // no format capacity is offered
}

IOReturn CloverBlockDevice::doSynchronizeCache(void)
{
  return kIOReturnSuccess;   // nothing is cached: every read goes straight to the card
}

char *CloverBlockDevice::getVendorString(void)
{
  return const_cast<char *>("Clover");
}

char *CloverBlockDevice::getProductString(void)
{
  return const_cast<char *>("eMMC");
}

char *CloverBlockDevice::getRevisionString(void)
{
  return const_cast<char *>("1");
}

char *CloverBlockDevice::getAdditionalDeviceInfoString(void)
{
  return const_cast<char *>("logfs");
}

IOReturn CloverBlockDevice::reportBlockSize(UInt64 *blockSize)
{
  if (!blockSize) {
    return kIOReturnBadArgument;
  }
  *blockSize = kBlockBytes;
  return kIOReturnSuccess;
}

IOReturn CloverBlockDevice::reportEjectability(bool *isEjectable)
{
  if (!isEjectable) {
    return kIOReturnBadArgument;
  }
  *isEjectable = false;
  return kIOReturnSuccess;
}

IOReturn CloverBlockDevice::reportMaxValidBlock(UInt64 *maxBlock)
{
  if (!maxBlock) {
    return kIOReturnBadArgument;
  }
  uint32_t sectors = clover_emmc_block_sectors();
  if (!sectors) {
    return kIOReturnNotReady;
  }
  *maxBlock = static_cast<UInt64>(sectors - 1);
  return kIOReturnSuccess;
}

IOReturn CloverBlockDevice::reportMediaState(bool *mediaPresent, bool *changedState)
{
  if (!mediaPresent) {
    return kIOReturnBadArgument;
  }
  *mediaPresent = true;
  if (changedState) {
    *changedState = false;
  }
  return kIOReturnSuccess;
}

IOReturn CloverBlockDevice::reportRemovability(bool *isRemovable)
{
  if (!isRemovable) {
    return kIOReturnBadArgument;
  }
  *isRemovable = false;
  return kIOReturnSuccess;
}

IOReturn CloverBlockDevice::reportWriteProtection(bool *isWriteProtected)
{
  if (!isWriteProtected) {
    return kIOReturnBadArgument;
  }
  *isWriteProtected = true;   // this round publishes a reader only
  return kIOReturnSuccess;
}

IOReturn CloverBlockDevice::getWriteCacheState(bool *enabled)
{
  if (!enabled) {
    return kIOReturnBadArgument;
  }
  *enabled = false;
  return kIOReturnSuccess;
}

IOReturn CloverBlockDevice::setWriteCacheState(bool enabled)
{
  return enabled ? kIOReturnUnsupported : kIOReturnSuccess;
}

IOReturn CloverBlockDevice::doAsyncReadWrite(IOMemoryDescriptor *buffer,
                                             UInt64 block, UInt64 nblks,
                                             IOStorageAttributes *attributes,
                                             IOStorageCompletion *completion)
{
  IOReturn status = kIOReturnSuccess;
  UInt64 done = 0;
  uint32_t sectors = clover_emmc_block_sectors();

  if (!buffer || buffer->getDirection() != kIODirectionIn) {
    status = kIOReturnNotWritable;
  } else if (block >= sectors || nblks > sectors - block) {
    status = kIOReturnBadArgument;
  } else {
    UInt64 fitting = buffer->getLength() / kBlockBytes;
    if (nblks > fitting) {
      nblks = fitting;
    }
    uint32_t words[kBlockWords];
    for (; done < nblks; done++) {
      if (clover_emmc_block_read(static_cast<uint32_t>(block + done), words, 1) != 1) {
        status = kIOReturnError;
        break;
      }
      if (buffer->writeBytes(done * kBlockBytes, words, kBlockBytes) != kBlockBytes) {
        status = kIOReturnNoSpace;
        break;
      }
    }
  }

  // IOBlockStorageDriver::executeRequest completes the request itself whenever this returns
  // anything but success, so a request handled here always completes exactly once, inline, and
  // the buffer and `this` are not touched afterwards.
  if (completion && completion->action) {
    IOStorageCompletion copy = *completion;
    copy.action(copy.target, copy.parameter, status, done * kBlockBytes);
  }
  return kIOReturnSuccess;
}

IOReturn CloverBlockDevice::requestIdle(void)
{
  return kIOReturnSuccess;
}

IOReturn CloverBlockDevice::doUnmap(IOBlockStorageDeviceExtent *extents,
                                    UInt32 extentsCount,
                                    IOStorageUnmapOptions options)
{
  return kIOReturnUnsupported;
}

IOReturn CloverBlockDevice::doSetPriority(IOBlockStorageDeviceExtent *extents,
                                          UInt32 extentsCount,
                                          IOStoragePriority priority)
{
  return kIOReturnSuccess;
}

IOReturn CloverBlockDevice::doSynchronize(UInt64 block, UInt64 nblks,
                                          IOStorageSynchronizeOptions options)
{
  return kIOReturnSuccess;
}

IOReturn CloverBlockDevice::doGetProvisionStatus(UInt64 block, UInt64 nblks,
                                                 UInt32 *extentsCount,
                                                 IOBlockStorageProvisionDeviceExtent *extents,
                                                 IOStorageGetProvisionStatusOptions options)
{
  if (extentsCount) {
    *extentsCount = 0;
  }
  return kIOReturnUnsupported;
}

// ---------------------------------------------------------------- the run

static const OSMetaClass *lookupMetaClass(const char *text)
{
  OSSharedPtr<const OSSymbol> name = OSSymbol::withCString(text);
  return name ? OSMetaClass::getMetaClassWithName(name.get()) : nullptr;
}

static uint32_t readLe32(const uint8_t *p)
{
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

static uint64_t readLe64(const uint8_t *p)
{
  return static_cast<uint64_t>(readLe32(p)) | (static_cast<uint64_t>(readLe32(p + 4)) << 32);
}

struct BlockResult {
  uint32_t stage;
  uint32_t load;
  uint32_t classSize;
  uint32_t post;
  uint32_t instances;
  uint32_t driverMeta;
  uint32_t driverStarted;
  uint32_t driverFound;
  uint32_t mediaFound;
  uint64_t mediaSectors;
  uint32_t blockSize;
  uint32_t open;
  uint32_t writable;
  uint32_t gptOk;
  uint32_t gptStatus;
  uint32_t entryCount;
  uint32_t entrySize;
  uint32_t logfsIndex;
  uint64_t logfsFirst;
  uint64_t logfsLast;
  uint32_t slot;
  uint32_t slotKind;
  uint32_t slotLen;
  uint32_t slotSum;
  uint32_t slotCalc;
  uint32_t slotCheck;
  uint32_t prefixOk;
  uint32_t headerStatus;
  uint32_t textStatus;
  uint32_t previewLength;
  char preview[25];
};

static BlockResult gResult;
static unsigned gReportLength;

static void putChar(char c)
{
  if (gReportLength < sizeof(clover_block_report) - 1) {
    clover_block_report[gReportLength++] = c;
  }
}

static void emitText(const char *text)
{
  while (*text) {
    putChar(*text++);
  }
}

static void emitDec(uint64_t value)
{
  char digits[20];
  unsigned count = 0;
  do {
    digits[count++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value);
  while (count) {
    putChar(digits[--count]);
  }
}

static void emitHex(uint64_t value, unsigned digits)
{
  static const char kHex[] = "0123456789abcdef";
  for (unsigned i = digits; i; i--) {
    putChar(kHex[(value >> ((i - 1) * 4)) & 0xf]);
  }
}

// One 512-byte block through IOMedia's asynchronous interface. The completion is expected inline:
// IOBlockStorageDriver runs the request through its command gate on this thread, and the device
// above completes inline. If the stack defers instead, `done` stays zero and the caller records
// kIOReturnTimeout -- a reading, not a hang, which matters because a sleep at this point in boot
// has no timer to wake it. `out` is the caller's copy of the descriptor's memory, so the GPT walk
// never reads the descriptor twice and every block has the same lifetime.
struct MediaRead {
  volatile uint32_t done;
  volatile uint32_t status;
  volatile uint64_t actual;
};

static MediaRead gMediaRead;

static void mediaReadDone(void *target, void *, IOReturn status, UInt64 actualByteCount)
{
  MediaRead *read = static_cast<MediaRead *>(target);
  read->status = static_cast<uint32_t>(status);
  read->actual = actualByteCount;
  read->done = 1;
}

static IOReturn readMediaBlock(IOMedia *media, IOService *client,
                               IOMemoryDescriptor *buffer, uint64_t lba,
                               MediaRead *read, uint8_t *out)
{
  read->done = 0;
  read->status = static_cast<uint32_t>(kIOReturnNotReady);
  read->actual = 0;
  IOStorageCompletion completion;
  completion.target = read;
  completion.action = mediaReadDone;
  completion.parameter = nullptr;
  media->read(client, lba * kBlockBytes, buffer, nullptr, &completion);
  if (!read->done) {
    return kIOReturnTimeout;
  }
  if (read->status != static_cast<uint32_t>(kIOReturnSuccess)) {
    return static_cast<IOReturn>(read->status);
  }
  if (read->actual != kBlockBytes || buffer->readBytes(0, out, kBlockBytes) != kBlockBytes) {
    return kIOReturnError;
  }
  return kIOReturnSuccess;
}

// A hang between two stages used to leave the panel at the last subsystem that spoke, which says
// nothing about this one. The marker is printed before the call that can hang, so the last line on
// the screen names the step instead of the next boot having to guess.
static void blockStage(const char *step)
{
  IOLog("CloverBlock r109: %s\n", step);
}

extern "C" void clover_block_run(void)
{
  BlockResult &r = gResult;
  gReportLength = 0;
  clover_block_report[0] = 0;
  for (unsigned i = 0; i < 16; i++) {
    clover_block_observation[i] = 0;
  }

  do {
    // Stage 1: the card path must already be proven. Mode 21 sets the ready flag only after a
    // clean CMD17 and a gated EXT_CSD read, so a block device can never appear over an unproven
    // data path.
    r.stage = 1;
    if (!clover_emmc_block_ready() || clover_emmc_block_sectors() == 0) {
      break;
    }

    // Stage 2: bring in IOStorageFamily and require the *exact* original class size. A layout
    // change would make every override below sit on the wrong slot, so this stops first.
    r.stage = 2;
    blockStage("2 storage family");
    kern_return_t load = OSKextLoadKextWithIdentifier("com.apple.iokit.IOStorageFamily");
    r.load = static_cast<uint32_t>(load);
    const OSMetaClass *base = lookupMetaClass("IOBlockStorageDevice");
    if (!base) {
      break;
    }
    r.classSize = base->getClassSize();
    if (r.classSize != sizeof(CloverBlockDevice)) {
      break;
    }

    // Stage 3: register our metaclass with the freshly loaded superclass.
    r.stage = 3;
    blockStage("3 metaclass");
    CloverBlockDevice::superClass = base;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
      break;
    }
    clover_block_static_init();
    OSReturn post = OSMetaClass::postModLoad(handle);
    r.post = static_cast<uint32_t>(post);
    if (post) {
      break;
    }
    r.instances = CloverBlockDevice::gMetaClass.getInstanceCount();

    r.stage = 4;
    blockStage("4 allocate");
    CloverBlockDevice *device = new CloverBlockDevice;
    if (!device || !device->init(nullptr)) {
      break;
    }

    // Stage 5: attach to the platform, then start the one driver this device is for.
    //
    // Round108 ended on "5 register" and never returned. That call was
    // registerService(kIOServiceSynchronous), whose inline match runs the catalogue before
    // IOKitInitializeTime: the photograph shows APFS and AMFI kext starts and an
    // IOCPUInterruptController registerService nested under it, then silence. PMGR's two
    // synchronous registrations return because no kext matches them; ours matches
    // IOStorageFamily, so the inline match drags in the early kext cascade and stops there.
    //
    // Round109 no longer asks the catalogue. It allocates IOBlockStorageDriver by name and
    // does what startCandidate() would do -- attach, then start -- with a marker before each
    // call. The driver's own media registration stays asynchronous (our device never entered
    // kIOServiceSynchronousState), so the same cascade is not entered from here.
    r.stage = 5;
    blockStage("5 attach");
    IOPlatformExpert *platform = IOService::getPlatform();
    if (!platform || !device->attach(platform) || !device->start(platform)) {
      break;
    }

    blockStage("5 driver meta");
    const OSMetaClass *driverMeta = lookupMetaClass("IOBlockStorageDriver");
    if (!driverMeta) {
      break;
    }
    r.driverMeta = 1;

    blockStage("5 driver alloc");
    IOService *handDriver = static_cast<IOService *>(driverMeta->alloc());
    if (!handDriver || !handDriver->init(nullptr)) {
      break;
    }

    blockStage("5 driver attach");
    if (!handDriver->attach(device)) {
      break;
    }

    blockStage("5 driver start");
    if (!handDriver->start(device)) {
      break;
    }
    r.driverStarted = 1;

    // Stage 6: the driver and its media are children in the service plane.
    r.stage = 6;
    blockStage("6 driver");
    IORegistryEntry *driverEntry = device->getChildEntry(gIOServicePlane);
    IOService *driver = driverEntry
        ? static_cast<IOService *>(driverEntry->metaCast("IOBlockStorageDriver")) : nullptr;
    if (!driver || driver->getProvider() != device || driver != handDriver) {
      break;
    }
    r.driverFound = 1;

    r.stage = 7;
    blockStage("7 media");
    IORegistryEntry *mediaEntry = driver->getChildEntry(gIOServicePlane);
    IOMedia *media = mediaEntry
        ? static_cast<IOMedia *>(mediaEntry->metaCast("IOMedia")) : nullptr;
    if (!media) {
      break;
    }
    r.mediaFound = 1;
    r.mediaSectors = media->getSize() / kBlockBytes;
    r.blockSize = static_cast<uint32_t>(media->getPreferredBlockSize());
    r.writable = media->isWritable() ? 1 : 0;
    if (r.mediaSectors != clover_emmc_block_sectors() || r.blockSize != kBlockBytes) {
      break;
    }

    // Stage 9: open as a reader. The media's own checks (size, block size) have now been read
    // through the storage stack rather than from this driver's globals.
    r.stage = 9;
    blockStage("9 open");
    if (!media->open(device, 0, kIOStorageAccessReader)) {
      break;
    }
    r.open = 1;

    OSSharedPtr<IOBufferMemoryDescriptor> buffer =
        IOBufferMemoryDescriptor::withCapacity(kBlockBytes, kIODirectionIn, true);
    if (!buffer.get()) {
      break;
    }
    uint8_t block[kBlockBytes];

    // Stage 10: the GPT header through IOMedia. `gp` is the stack's own status: kIOReturnTimeout
    // here means the request was not completed inline, which is a reading about the storage stack
    // rather than a failure of the card.
    r.stage = 10;
    blockStage("10 gpt");
    IOReturn status = readMediaBlock(media, device, buffer.get(), 1, &gMediaRead, block);
    r.gptStatus = static_cast<uint32_t>(status);
    if (status != kIOReturnSuccess || memcmp(block, "EFI PART", 8) != 0) {
      break;
    }
    uint64_t entryLba = readLe64(block + 72);
    uint32_t entryCount = readLe32(block + 80);
    uint32_t entrySize = readLe32(block + 84);
    r.entryCount = entryCount;
    r.entrySize = entrySize;
    if (entryCount == 0 || entrySize < 128 || entrySize > kBlockBytes ||
        (kBlockBytes % entrySize) != 0) {
      break;
    }
    r.gptOk = 1;

    // Stage 11: walk the entry array for the entry named exactly "logfs". The name is UTF-16LE
    // at byte 56, so the match includes the terminator and cannot accept "logfsx".
    r.stage = 11;
    blockStage("11 entries");
    const uint32_t perBlock = kBlockBytes / entrySize;
    uint32_t blocks = (entryCount + perBlock - 1) / perBlock;
    if (blocks > 128) {
      blocks = 128;
    }
    for (uint32_t b = 0; b < blocks && !r.logfsIndex; b++) {
      status = readMediaBlock(media, device, buffer.get(), entryLba + b, &gMediaRead, block);
      if (status != kIOReturnSuccess) {
        r.gptOk = 0;
        r.gptStatus = static_cast<uint32_t>(status);
        break;
      }
      for (uint32_t e = 0; e < perBlock && (b * perBlock + e) < entryCount; e++) {
        const uint8_t *entry = block + e * entrySize;
        if (memcmp(entry + 56, kLogfsName, sizeof(kLogfsName)) == 0) {
          r.logfsIndex = b * perBlock + e + 1;   // 1-based, the way the kernel names it
          r.logfsFirst = readLe64(entry + 32);
          r.logfsLast = readLe64(entry + 40);
          break;
        }
      }
    }
    if (!r.logfsIndex) {
      break;
    }

    // Stage 12: scan the fixed two-block logfs slots and keep the last valid header. A read
    // failure and an unwritten slot are different readings: the former is recorded, the latter
    // leaves s=0.
    r.stage = 12;
    blockStage("12 slots");
    for (uint32_t slot = 0; slot < kLogSlots; slot++) {
      status = readMediaBlock(media, device, buffer.get(), r.logfsFirst + 2 * slot, &gMediaRead, block);
      if (status != kIOReturnSuccess) {
        r.headerStatus = static_cast<uint32_t>(status);
        break;
      }
      const uint32_t *words = reinterpret_cast<const uint32_t *>(block);
      if (words[0] != kLogMagic0 || words[1] != kLogMagic1) {
        break;
      }
      r.slot = slot;
      r.slotLen = words[3];
      r.slotSum = words[4];
      r.slotKind = words[5];
      if (slot == kLogSlots - 1) {
        break;
      }
    }
    // `slotSum` is zero only when no slot header carried the magic; a real empty slot 0 still
    // has this boot's own header because mode 21 does not write, so the previous boot's record
    // is what the scan finds.
    if (r.headerStatus) {
      break;
    }
    if (r.slotLen == 0 && r.slotSum == 0 && r.slotKind == 0) {
      break;
    }

    // Stage 13: the text block that header names. Length is clamped before it drives a loop,
    // and the byte sum is recomputed rather than trusted.
    r.stage = 13;
    blockStage("13 text");
    status = readMediaBlock(media, device, buffer.get(), r.logfsFirst + 2 * r.slot + 1, &gMediaRead, block);
    r.textStatus = static_cast<uint32_t>(status);
    if (status != kIOReturnSuccess) {
      break;
    }
    uint32_t length = r.slotLen;
    if (length > kBlockBytes) {
      length = kBlockBytes;
    }
    uint32_t sum = 0;
    for (uint32_t i = 0; i < length; i++) {
      sum += block[i];
    }
    r.slotCalc = sum;
    r.slotCheck = (sum == r.slotSum) ? 1 : 0;
    r.prefixOk = (length >= kLogPrefixLength &&
                  memcmp(block, "unixnu clover emmc log\n", kLogPrefixLength) == 0) ? 1 : 0;
    for (uint32_t i = 0; i < sizeof(r.preview) - 1 && kLogPrefixLength + i < length; i++) {
      char c = static_cast<char>(block[kLogPrefixLength + i]);
      r.preview[i] = (c >= 0x20 && c < 0x7f) ? c : (c == '\n' ? '|' : '.');
      r.previewLength = i + 1;
    }
    r.stage = 15;
    blockStage("15 done");
  } while (false);

  clover_block_observation[0] = r.stage;
  clover_block_observation[1] = r.load;
  clover_block_observation[2] = r.classSize;
  clover_block_observation[3] = r.post;
  clover_block_observation[4] = r.instances;
  clover_block_observation[5] = r.mediaSectors;
  clover_block_observation[6] = (static_cast<uint64_t>(r.blockSize) << 32) | r.writable;
  clover_block_observation[7] = r.open;
  clover_block_observation[8] = r.gptStatus;
  clover_block_observation[9] = (static_cast<uint64_t>(r.entrySize) << 32) | r.entryCount;
  clover_block_observation[10] = (static_cast<uint64_t>(r.logfsLast & 0xffffffffu) << 32) |
                                 r.logfsIndex;
  clover_block_observation[11] = (static_cast<uint64_t>(r.slotKind) << 32) | r.slotLen;
  clover_block_observation[12] = r.slotSum;
  clover_block_observation[13] = r.slotCalc;
  clover_block_observation[14] = r.headerStatus;
  clover_block_observation[15] = r.textStatus;

  emitText("b st=");
  emitDec(r.stage);
  emitText(" ld=");
  emitDec(r.load);
  emitText(" cs=");
  emitHex(r.classSize, 4);
  emitText(" pm=");
  emitDec(r.post);
  emitText(" dm=");
  emitDec(r.driverMeta);
  emitText(" ds=");
  emitDec(r.driverStarted);
  emitText("\nb dv=");
  emitDec(r.driverFound);
  emitText(" md=");
  emitDec(r.mediaFound);
  emitText(" sz=");
  emitDec(r.mediaSectors);
  emitText(" bl=");
  emitDec(r.blockSize);
  emitText("\nb op=");
  emitDec(r.open);
  emitText(" wp=");
  emitDec(r.writable);
  emitText(" gp=");
  emitDec(r.gptOk);
  emitText(" ix=");
  emitDec(r.logfsIndex);
  emitText("\nb lf s=");
  emitDec(r.slot);
  emitText(" a=");
  emitDec(r.logfsFirst);
  emitText(" b=");
  emitDec(r.logfsLast);
  emitText(" m=");
  emitDec(r.slotKind);
  emitText(" n=");
  emitDec(r.slotLen);
  emitText(" c=");
  emitDec(r.slotCheck);
  emitText(" p=");
  emitDec(r.prefixOk);
  if (!r.gptOk && r.gptStatus) {
    emitText(" hs=");
    emitHex(r.gptStatus, 8);
  }
  if (r.headerStatus) {
    emitText(" hs=");
    emitHex(r.headerStatus, 8);
  }
  if (r.textStatus) {
    emitText(" ts=");
    emitHex(r.textStatus, 8);
  }
  emitText("\nb tx ");
  for (uint32_t i = 0; i < r.previewLength; i++) {
    putChar(r.preview[i]);
  }
  clover_block_report[gReportLength] = 0;
}
