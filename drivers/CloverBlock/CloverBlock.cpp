// SPDX-License-Identifier: MIT
// clover (Mi Pad 4, SDM660): publish the eMMC as an IOBlockStorageDevice so XNU's own storage
// stack, not this port's private PIO report, is what reads the card.
//
// Round 111 left the logfs write path proven through IOMedia. Round 112 starts collecting the
// instruments and moves up a layer: the partition table is no longer walked by hand. IOStorage
// Family's IOGUIDPartitionScheme parses the GPT and publishes each entry as an IOMedia, and
// IOMediaBSDClient gives the whole disk and the logfs partition BSD names. The run then performs
// the two discovery steps the kernel's own root code performs, IOBSDNameMatching and bdevvp, so
// a success line means setconf()/vfs_mountroot() could name this partition as the root device.
// The boot itself still runs from the RAMDisk (rd=md0); this round only proves the device side.
//
// Round 113 ran all of that from sheng_pmgr_run() and stopped at bsd-props. That hook is in
// bsd_init() before vfsinit(), so devfs is not initialized yet, devfs_make_node() returns NULL,
// and IOMediaBSDClient::start() still reports success because it ignores createNodes(). Round 114
// moved the start to the publication of IOKit's "IOBSD" resource, which IOKitBSDInit() performs
// at the end of bsd_autoconf(), after vfsinit(); clover_block_request() at the bottom explains why
// that is the only post-vfsinit hook this repository owns.
//
// Round 114 still stopped at bsd-props, so the ordering was not the only obstacle: either
// createNodes() fails somewhere else, or devfs refuses the node for another reason. start() hides
// which one. Round 115 keeps the same run and replaces the bare "bsd-props 0" with a bitmask read
// from the same media objects (README v0.2.87 lists the bits), plus a devfs_make_node/remove probe
// for a private name and for the four names the client would have used. The probe proves devfs is
// ready regardless of what the client did; the name probes show whether a node already existed.
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
// A hand-driven service still has to see the same attach/probe/start order IOService::probeCandidates
// uses: IOGUIDPartitionScheme::scan() starts with getProvider() and dereferences it, so probe() on
// an unattached candidate faults inside the kext. Attach happens before probe, and the object stays
// attached for start (startCandidate() attaches again after the matching loop's detach).
//
// A failure panics with one short CloverBlock r115 line naming the step; success prints one line
// and lets the boot continue, because there is nothing left to photograph.
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
uint32_t clover_emmc_block_write (uint32_t lba, const uint32_t *words, uint32_t blocks);
// The kernel's own block-device vnode constructor (bsd/vfs/vfs_subr.c). dev_t is the composite
// major<<24|minor this file passes; the vnode is never touched, and it stays for boot lifetime
// exactly as it would when it becomes the root vnode.
int bdevvp (int32_t dev, void **vpp);
// bsd/miscfs/devfs/devfs_tree.c. The probe only needs the entry to be accepted and removed, so
// dev_t is passed as the same 32-bit word the C prototype uses. DEVFS_BLOCK is 1.
void *devfs_make_node (uint32_t dev, int chrblk, uint32_t uid, uint32_t gid, int perms,
                       const char *fmt, ...);
void devfs_remove (void *node);
}

static const uint32_t kBlockBytes = 512;
static const uint32_t kBlockWords = kBlockBytes / 4;
static const uint32_t kLogfsPartitionId = 39;   // GPT index, from the round101 entry walk
static const char kPartitionIdKey[] = "Partition ID";
static const char kBSDNameKey[] = "BSD Name";
static const char kBSDMajorKey[] = "BSD Major";
static const char kBSDMinorKey[] = "BSD Minor";

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
  // Round109 only read. Stage 14 now writes one logfs record through IOMedia, so the media has
  // to be genuinely writable; IOBlockStorageDriver builds its media from this answer.
  *isWriteProtected = false;
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
  const IODirection direction = buffer ? buffer->getDirection() : kIODirectionNone;

  if (!buffer || (direction != kIODirectionIn && direction != kIODirectionOut)) {
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
      if (direction == kIODirectionOut) {
        if (buffer->readBytes(done * kBlockBytes, words, kBlockBytes) != kBlockBytes) {
          status = kIOReturnNoSpace;
          break;
        }
        if (clover_emmc_block_write(static_cast<uint32_t>(block + done), words, 1) != 1) {
          status = kIOReturnError;
          break;
        }
      } else {
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

static OSObject *property(IORegistryEntry *entry, const char *key)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  return entry->getProperty(key);
#pragma clang diagnostic pop
}

__attribute__((noreturn)) static void fail(const char *step, uint64_t value)
{
  panic("CloverBlock r115: %s %llu", step, static_cast<unsigned long long>(value));
}

// The bsd-props mask has 19 bits, so decimal is unreadable on a photographed panic screen.
__attribute__((noreturn)) static void failMask(const char *step, uint64_t mask)
{
  char value[24];
  snprintf(value, sizeof(value), "0x%llx", static_cast<unsigned long long>(mask));
  panic("CloverBlock r115: %s %s", step, value);
}

// Round115 instrument. createNodes() is the only place that writes the BSD properties, and
// IOMediaBSDClient::start() ignores its answer, so bsd-whole/bsd-root prove nothing about them.
// The client fails for one of two reasons: devfs_make_node() returns NULL (devfs not ready, or the
// name is already taken), or the anchor/minor table rejects the media before that. These bits have
// to be read before the client objects are released, and the name probes are exactly the names the
// client would have requested: "disk0"/"disk1" for a whole media whose anchor ID is 0 or 1, and
// "disk0s39"/"disk1s39" for the logfs partition off the same anchor.
static bool devfsProbe(const char *name)
{
  void *node = devfs_make_node(/* dev */ 0x7e00007eu, /* DEVFS_BLOCK */ 1,
                               /* uid */ 0, /* gid (GID_OPERATOR) */ 5, /* perms */ 0640,
                               /* name */ "%s", name);
  if (!node) {
    return false;
  }
  devfs_remove(node);
  return true;
}

enum BsdPropsBit {
  kWholeNameBit   = 1u << 0,   // whole media has "BSD Name"
  kWholeMajorBit  = 1u << 1,   // whole media has "BSD Major"
  kWholeMinorBit  = 1u << 2,   // whole media has "BSD Minor"
  kTargetNameBit  = 1u << 3,   // logfs partition has "BSD Name"
  kTargetMajorBit = 1u << 4,   // logfs partition has "BSD Major"
  kTargetMinorBit = 1u << 5,   // logfs partition has "BSD Minor"
  kLocationBit    = 1u << 6,   // partition "Location" is exactly "39"
  kTargetProvBit  = 1u << 7,   // partition has a provider
  kWholeProvBit   = 1u << 8,   // whole media has a provider
  kWholeFlagBit   = 1u << 9,   // whole media reports isWhole()
  kTargetWholeBit = 1u << 10,  // partition reports isWhole() (must stay clear)
  kProbeNameBit   = 1u << 11,  // devfs accepted a private name: devfs is ready and writable
  kProbeDisk0Bit  = 1u << 12,  // "disk0" was still free after the clients ran
  kProbeDisk1Bit  = 1u << 13,  // "disk1" was still free
  kProbeS39Bit    = 1u << 14,  // "disk0s39" was still free
  kProbeS39AltBit = 1u << 15,  // "disk1s39" was still free
  kWholeEarlyBit  = 1u << 16,  // whole media already had a BSD name before its client started
  kTargetEarlyBit = 1u << 17,  // partition already had a BSD name before its client started
  kTargetBaseBit  = 1u << 18,  // partition base is non-zero
};

static uint64_t bsdPropsMask(IOMedia *whole, IOMedia *target, bool wholeEarly, bool targetEarly)
{
  uint64_t mask = 0;
  const char *location = target->getLocation();

  if (property(whole, kBSDNameKey))   mask |= kWholeNameBit;
  if (property(whole, kBSDMajorKey))  mask |= kWholeMajorBit;
  if (property(whole, kBSDMinorKey))  mask |= kWholeMinorBit;
  if (property(target, kBSDNameKey))  mask |= kTargetNameBit;
  if (property(target, kBSDMajorKey)) mask |= kTargetMajorBit;
  if (property(target, kBSDMinorKey)) mask |= kTargetMinorBit;
  if (location && strcmp(location, "39") == 0) mask |= kLocationBit;
  if (target->getProvider())          mask |= kTargetProvBit;
  if (whole->getProvider())           mask |= kWholeProvBit;
  if (whole->isWhole())               mask |= kWholeFlagBit;
  if (target->isWhole())              mask |= kTargetWholeBit;
  if (wholeEarly)                     mask |= kWholeEarlyBit;
  if (targetEarly)                    mask |= kTargetEarlyBit;
  if (target->getBase() != 0)         mask |= kTargetBaseBit;

  // The probes come last: each successful one creates and removes a node, and the four client
  // names are probed after the clients so a taken name means the client (or an automatic one)
  // actually got there.
  if (devfsProbe("cloverbdprobe"))  mask |= kProbeNameBit;
  if (devfsProbe("disk0"))          mask |= kProbeDisk0Bit;
  if (devfsProbe("disk1"))          mask |= kProbeDisk1Bit;
  if (devfsProbe("disk0s39"))       mask |= kProbeS39Bit;
  if (devfsProbe("disk1s39"))       mask |= kProbeS39AltBit;

  return mask;
}

struct MediaOp {
  volatile uint32_t done;
  volatile uint32_t status;
  volatile uint64_t actual;
};

static MediaOp gMediaOp;

static void mediaOpDone(void *target, void *, IOReturn status, UInt64 actualByteCount)
{
  MediaOp *op = static_cast<MediaOp *>(target);
  op->status = static_cast<uint32_t>(status);
  op->actual = actualByteCount;
  op->done = 1;
}

// One block through IOMedia, with the same inline-completion contract the round111 reads used:
// IOBlockStorageDriver completes on this thread, so a deferred request is a reading
// (kIOReturnTimeout), not a hang.
static IOReturn readMediaBytes(IOMedia *media, IOService *client, IOMemoryDescriptor *buffer,
                               uint64_t byteStart, MediaOp *op)
{
  op->done = 0;
  op->status = static_cast<uint32_t>(kIOReturnNotReady);
  op->actual = 0;
  IOStorageCompletion completion;
  completion.target = op;
  completion.action = mediaOpDone;
  completion.parameter = nullptr;
  media->read(client, byteStart, buffer, nullptr, &completion);
  if (!op->done) {
    return kIOReturnTimeout;
  }
  if (op->status != static_cast<uint32_t>(kIOReturnSuccess) || op->actual != kBlockBytes) {
    return static_cast<IOReturn>(op->status);
  }
  return kIOReturnSuccess;
}

// Hand-start an IOStorageFamily service by name. Round109 showed the synchronous catalogue match
// never returns on this boot path, so each object is allocated from its metaclass and driven
// through attach/start directly. The objects are never released: the registry, the media tree
// and the BSD minor table all keep pointing at them for the rest of the boot.
static IOService *startService(const char *className, IOService *provider)
{
  const OSMetaClass *meta = lookupMetaClass(className);
  if (!meta) {
    return nullptr;
  }
  IOService *service = static_cast<IOService *>(meta->alloc());
  if (!service || !service->init(nullptr) || !service->attach(provider) ||
      !service->start(provider)) {
    return nullptr;
  }
  return service;
}

extern "C" void clover_block_run(void)
{
  // 1. The PIO path has to be proven before anything claims a device over it.
  if (!clover_emmc_block_ready() || clover_emmc_block_sectors() == 0) {
    fail("ready", 0);
  }

  // 2. IOStorageFamily plus the exact original class size: every override in CloverBlockDevice
  // sits on an IOBlockStorageDevice slot, so a layout change has to stop here.
  kern_return_t load = OSKextLoadKextWithIdentifier("com.apple.iokit.IOStorageFamily");
  if (load) {
    fail("storage", static_cast<uint32_t>(load));
  }
  const OSMetaClass *base = lookupMetaClass("IOBlockStorageDevice");
  if (!base || base->getClassSize() != sizeof(CloverBlockDevice)) {
    fail("abi", base ? base->getClassSize() : 0);
  }

  // 3. Register this class against the freshly loaded superclass.
  CloverBlockDevice::superClass = base;
  void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
  if (!handle) {
    fail("modload", 0);
  }
  clover_block_static_init();
  if (OSMetaClass::postModLoad(handle)) {
    fail("postload", 0);
  }

  // 4. Publish the block device and start the one driver it is for.
  CloverBlockDevice *device = new CloverBlockDevice;
  IOPlatformExpert *platform = IOService::getPlatform();
  if (!device || !device->init(nullptr)) {
    fail("device", 0);
  }
  if (!platform || !device->attach(platform) || !device->start(platform)) {
    fail("attach", 0);
  }
  const OSMetaClass *driverMeta = lookupMetaClass("IOBlockStorageDriver");
  IOService *driver = driverMeta ? static_cast<IOService *>(driverMeta->alloc()) : nullptr;
  if (!driver || !driver->init(nullptr) || !driver->attach(device) || !driver->start(device)) {
    fail("driver", 0);
  }
  IORegistryEntry *mediaEntry = driver->getChildEntry(gIOServicePlane);
  IOMedia *whole = mediaEntry
      ? static_cast<IOMedia *>(mediaEntry->metaCast("IOMedia")) : nullptr;
  if (!whole || whole->getSize() / kBlockBytes != clover_emmc_block_sectors()) {
    fail("media", 0);
  }

  // 5. The real partition table reader. probe() opens the media, reads LBA 0/1 and the entry
  // array and keeps one media object per entry; start() attaches and registers each of them.
  const OSMetaClass *schemeMeta = lookupMetaClass("IOGUIDPartitionScheme");
  IOService *scheme = schemeMeta ? static_cast<IOService *>(schemeMeta->alloc()) : nullptr;
  if (!scheme || !scheme->init(nullptr)) {
    fail("gpt", 0);
  }
  // probeCandidates() attaches a candidate before probe(), and scan() starts with
  // getProvider() without checking it, so an unattached probe faults on a null media.
  if (!scheme->attach(whole)) {
    fail("gpt-attach", 0);
  }
  SInt32 score = 0;
  if (!scheme->probe(whole, &score)) {
    fail("gpt-probe", static_cast<uint32_t>(score));
  }
  if (!scheme->start(whole)) {
    fail("gpt-start", 0);
  }

  // 6. The logfs entry the round101 walk named. The count is the scheme's own published total,
  // so a wrong number is visible without another hand-rolled GPT parse.
  uint32_t partitions = 0;
  IOMedia *target = nullptr;
  OSSharedPtr<OSIterator> children = scheme->getChildIterator(gIOServicePlane);
  if (!children.get()) {
    fail("gpt-children", 0);
  }
  // IOMedia's metaclass symbol belongs to IOStorageFamily, not the kernel, so the cast is by
  // name: the only class this scheme publishes children of is IOMedia.
  while (OSObject *object = children->getNextObject()) {
    IOMedia *partition = static_cast<IOMedia *>(object->metaCast("IOMedia"));
    if (!partition) {
      continue;
    }
    partitions++;
    OSNumber *id = OSDynamicCast(OSNumber, property(partition, kPartitionIdKey));
    if (id && id->unsigned32BitValue() == kLogfsPartitionId) {
      target = partition;
    }
  }
  if (!target) {
    fail("gpt-target", partitions);
  }

  // 7. BSD nodes. The whole disk is started first so the anchor is unit 0 and the partition is
  // named disk0s39 by the same code that names every disk in a normal boot. The two early reads
  // record whether IOKit's own asynchronous match already published a client before this run:
  // that is the difference between "nobody created a node" and "a node already existed".
  bool wholeEarly = property(whole, kBSDNameKey) != nullptr;
  bool targetEarly = property(target, kBSDNameKey) != nullptr;
  if (!startService("IOMediaBSDClient", whole)) {
    fail("bsd-whole", 0);
  }
  if (!startService("IOMediaBSDClient", target)) {
    fail("bsd-root", 0);
  }
  OSString *bsdName = OSDynamicCast(OSString, property(target, kBSDNameKey));
  OSNumber *bsdMajor = OSDynamicCast(OSNumber, property(target, kBSDMajorKey));
  OSNumber *bsdMinor = OSDynamicCast(OSNumber, property(target, kBSDMinorKey));
  if (!bsdName || !bsdMajor || !bsdMinor) {
    failMask("bsd-props", bsdPropsMask(whole, target, wholeEarly, targetEarly));
  }

  // 8. The exact discovery setconf() performs for rd=<bsd name>.
  OSDictionary *matching = IOBSDNameMatching(bsdName->getCStringNoCopy());
  mach_timespec_t timeout = { 1, 0 };
  IOService *matched = matching ? IOService::waitForService(matching, &timeout) : nullptr;
  OSSafeReleaseNULL(matching);
  if (matched != target) {
    fail("root-match", reinterpret_cast<uint64_t>(matched));
  }

  // 9. bdevvp() is what vfs_mountroot() calls on that dev_t. It opens the minor through the
  // bdevsw IOMediaBSDClient installed, so this both proves the number and leaves the vnode the
  // root mount would use.
  int32_t rootdev = (static_cast<int32_t>(bsdMajor->unsigned32BitValue()) << 24) |
                    static_cast<int32_t>(bsdMinor->unsigned32BitValue());
  void *rootvp = nullptr;
  int error = bdevvp(rootdev, &rootvp);
  if (error || !rootvp) {
    fail("bdevvp", static_cast<uint32_t>(error));
  }

  // 10. The partition's offset mapping: the same block read through the whole media at the
  // partition's base and through the partition at byte zero has to be identical.
  OSSharedPtr<IOBufferMemoryDescriptor> wholeBlock =
      IOBufferMemoryDescriptor::withCapacity(kBlockBytes, kIODirectionIn, true);
  OSSharedPtr<IOBufferMemoryDescriptor> partBlock =
      IOBufferMemoryDescriptor::withCapacity(kBlockBytes, kIODirectionIn, true);
  if (!wholeBlock.get() || !partBlock.get()) {
    fail("buffer", 0);
  }
  uint64_t partitionBase = target->getBase();
  if (!whole->open(device, 0, kIOStorageAccessReader)) {
    fail("open-whole", 0);
  }
  IOReturn status = readMediaBytes(whole, device, wholeBlock.get(), partitionBase, &gMediaOp);
  whole->close(device);
  if (status != kIOReturnSuccess) {
    fail("read-whole", static_cast<uint32_t>(status));
  }
  if (!target->open(device, 0, kIOStorageAccessReader)) {
    fail("open-part", 0);
  }
  status = readMediaBytes(target, device, partBlock.get(), 0, &gMediaOp);
  target->close(device);
  if (status != kIOReturnSuccess) {
    fail("read-part", static_cast<uint32_t>(status));
  }
  const void *wholeBytes = wholeBlock->getBytesNoCopy();
  const void *partBytes = partBlock->getBytesNoCopy();
  if (!wholeBytes || !partBytes || memcmp(wholeBytes, partBytes, kBlockBytes) != 0) {
    fail("map", 0);
  }

  IOLog("CloverBlock r115: post-vfsinit p=%u %s %u:%u bd=1 map=1\n", partitions,
        bsdName->getCStringNoCopy(), bsdMajor->unsigned32BitValue(),
        bsdMinor->unsigned32BitValue());
}

// ---------------------------------------------------------------- the post-vfsinit hand-off

// Where this runs matters more than what it does. sheng_pmgr_run() is called by bsd_init() before
// vfsinit(), and vfsinit() is what calls devfs_init() -> devfs_sinit(), the only place that sets
// devfs_ready. Before that, devfs_make_node() refuses with a NULL node, MinorTable::insert() fails,
// and IOMediaBSDClient::start() returns true anyway -- round113's bsd-props with no properties
// behind it. bdevvp() has the same prerequisite: it needs the vnode tables vntblinit()/nchinit()
// build inside vfsinit().
//
// IOKitBSDInit() publishes the "IOBSD" resource from the end of bsd_autoconf(), which is the first
// point in the boot past vfsinit() and still before setconf()/vfs_mountroot(). A publish
// notification for that resource is therefore a post-vfsinit hook that this repository owns: a
// device patches/xnu would shadow the base repository's whole patch set (CloverSdhci.cpp explains
// that wall at length), and bsd_init.c itself is out of reach.
//
// Round114's bsd-props showed this hook is not sufficient by itself: either the handler still runs
// before devfs is usable (an earlier "IOBSD" publication in the boot kext collection), or
// createNodes() fails on the media tree. Round115 does not change the hook; it makes bsd-props say
// which of the two it is, because a mask is cheaper to photograph than a reordering guess.
//
// The handler runs on IOKit's config thread, the same kind of thread probeCandidates() uses for a
// normal personality match, so the work is no longer on the boot thread. The boot does not wait for
// it: rd= is still md0 and this round only asks whether the device side completes. When a later
// round points rd= at disk0s39, setconf()'s IOFindBSDRoot() supplies the wait.
static bool cloverBlockIOBSD(void *, void *, IOService *, IONotifier *)
{
  clover_block_run();
  return false;   // one shot: the resource is published once per boot
}

extern "C" void clover_block_request(void)
{
  OSSharedPtr<OSDictionary> matching = IOService::resourceMatching("IOBSD");
  if (!matching) {
    fail("notify", 0);
  }
  IONotifier *notifier = IOService::addMatchingNotification(
      gIOPublishNotification, matching.get(), &cloverBlockIOBSD, nullptr, nullptr, 0);
  if (!notifier) {
    fail("notify", 1);
  }
}

