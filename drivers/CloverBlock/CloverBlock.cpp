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
// Round 115's mask (0x5abc7) answered that. devfs was ready, the whole media already carried a BSD
// name before the manual client ran, and both "disk0" and "disk0s39" already existed while our
// logfs media had no properties. The boot log says where they came from: a column of
// "disk0sN: name slot allocation failed (Errno=17)" from the duplicate tree this file built by
// hand. Starting the driver's media calls registerService(); IOKit's config thread matches
// IOGUIDPartitionScheme and IOMediaBSDClient for it, and those clients name the whole disk and
// every partition before this run reaches its own scheme. The hand-built second scheme then loses
// every name to EEXIST, and its partition is the media this file was reading. Round 116 stops
// building a second tree: it waits for the automatic one to finish, finds the partition by the
// "BSD Name" the kernel itself wrote, and continues with that media.
//
// Round 116 got through that find, the property casts, IOBSDNameMatching and waitForService, then
// data-aborted at 0xfffffff00799238c. That instruction reads the vtable of the matching dictionary
// to release it; IOService::waitForService() had already released it, and IOFindBSDRoot() retains
// the dictionary once per wait for exactly this reason. Round 117 retains it the same way and
// releases it once after the wait, so the run can reach the root-match check, bdevvp and the
// offset comparison.
//
// Round 117's photograph was a bare data abort at pc 0xfffffff007a231a4, lr 0xfffffff00782ef6c --
// not one of the fail() lines below, so the first run had already cleared postload and registered
// CloverBlockDevice. The pc is inside OSMetaClass::postModLoad.cold.1, the duplicate-class panic
// path, reading orig->reserved->kext (NULL) to name the kext that already owns the class; the lr is
// postModLoad's call to that path. In other words clover_block_run() executed a second time: the
// re-run rebuilt gMetaClass (resetting reserved->kext to NULL), and its postModLoad found the first
// run's CloverBlockDevice already in sAllClassesDict. The boot log had reached APFS and launchd, so
// the first run finished and the crash came later. The cause is the notification itself.
// resourceMatching("IOBSD") matches the persistent gIOResources service, and IOService::doServiceMatch
// re-collects and re-invokes every gIOPublishNotification whose matching still matches on each pass,
// ignoring the handler's return value -- so returning false does NOT retire it here. Every later
// publishResource() (the property loop and boot-uuid in IOKitBSDInit) re-matches gIOResources and
// re-fires this handler. Round 118 keeps the notifier installed but latches the run: an atomic
// compare-and-swap admits the first invocation and turns every refire into a counted no-op, so the
// metaclass is registered exactly once and the boot continues to answer the real question.
// Round 118 measured that on the device: `auto p=39 disk0s39 1:39 bd=1 map=1 fires=1` printed, then
// one `refire n=2 (already ran)`, and the log ran on through APFS to the EL0 init. The block stack
// is also timed right -- both lines sit between apfs_syscall_register() and md0's mdevadd(), so the
// media exists before IOFindBSDRoot looks for a root. Round 119 therefore points `rd=` at this
// partition instead of md0. Nothing here changes: the same six steps, the same latch. What is new is
// what happens after this driver returns -- bdevvp(1<<24|39)'s vnode is read by mockfs through the
// buffer cache, which is the first read this board performs that does not go through a call this file
// makes itself.
// Round 119's photograph read `Failed to mount root device @IOKitBSDInit.cpp:984` at ticks=0xb0c, and the
// cause was not this driver: /chosen's `root-matching` property is deserialized into IOFindBSDRoot's
// `matching` dictionary at :790-797 and `continue`s, while the branch that turns `rd=` into
// IOBSDNameMatching is guarded by `if ((!matching) && rdBootVar[0])` at :888 -- so the argument was parsed
// at :781, ignored, and ten waits ran against a stock volume UUID no service here carries. Round 120 takes
// that property out of the device tree, which leaves step 6's dictionary as the only root contract and is
// the same wait round 118 already showed answering on this board.
// Round 120's photograph read the best case: `rootvp not authenticated after mounting @bsd_init.c:1098`,
// and the root it authenticated was this partition's -- the initrd's ramdisk sits at major 3, this media
// at major 1 (memdev.c:665 takes md's major from bdevsw_add(-1, &mdevbdevsw), a different slot), so
// mockfs's buf_meta_bread(rvp, 0, 4096) came back from a real PIO read. The mount works. Only the seal
// check refuses it, and a mockfs image has no seal to check. Round 121 therefore adds step 9: rename this
// media's "BSD Name" to "md0s39" once every measurement above has been taken under the name the storage
// family wrote. bsd_rooted_ramdisk() (bsd_init.c:450) tests only the RAMDEV prefix "md0" (bsd_init.c:447),
// so that name makes bsd_init skip the FSIOC_KERNEL_ROOTAUTH block entirely -- the other reader of that
// test in the whole tree is exclaves_storage.c:219. IOFindBSDRoot's own memory-device shortcut still
// refuses the name, because it requires rdBootVar[3] == 0 (:849), so the root search keeps taking the
// BSD-name branch at :888 and keeps matching this media by the property written here, with this media's
// own numbers. The bytes still come from eMMC; what changes is that the kernel is told to believe an
// unsealed image on a real partition is a ramdisk. That is a bypass, not a pass, and step 9's whole point
// is to ask what lies past it: the exec of /sbin/launchd should now fail on the partition's contents
// (kern_exec.c:8104, `Process 1 exec of /sbin/launchd failed, errno N`) instead of on authentication.
//
// Round 127 asks a different question with the same device, and changes what the device is made to
// describe. Apple's own apfs.kext is in this port's boot kext collection, and the only way to find
// out whether it can attach to a device this port published, let alone mount a volume, is to hand it
// something to attach to. It matches a whole disk
// whose first block is a container superblock -- on a host, ioreg shows the personality
// `IOPropertyMatch {"Whole"=Yes}` firing for an image with no partition map at all, and the
// synthesized volume of a real container carries `Content Hint` = 41504653-... An eMMC partition
// cannot be advertised that way after the fact: the match runs while the media registers, and
// rewriting the card's GPT to change one entry's type GUID would move every filesystem this port
// has been reading. So the media itself is re-scoped. drivers/CloverSdhci exports logfs's own
// window -- first LBA and length, taken from the GPT entry this boot read, published only when that
// entry is the extent it has always been -- and under the `clover-apfs=1` boot arg this file
// describes exactly that window: `reportMaxValidBlock` stops at the partition, `doAsyncReadWrite`
// adds the window's first LBA to every block number it is given. Media byte zero then is logfs
// block zero, which is where the container image built by
// scripts/make-clover-apfs-container.py puts its `NXSB` superblock, and Whole=Yes is what an
// un-partitioned disk already carries. Nothing on the card's partition table changed.
//
// The reading is a depth, and it is panic-drawn because the console cannot be trusted this late:
// did apfs load, did the container scheme attach to *this* media (the walk only ever descends from
// it, so a match is by construction ours), did a container parse, did a volume get published, and
// how many blocks did the device serve on the way. Steps 5-9 -- the disk0s39 wait, the root
// dictionary, bdevvp, the offset map, the md0s39 rename -- are round118-121's measurements of a
// partition-named root and cannot run against a window (there is no GPT at its block zero), so the
// apfs branch stops after step 4's size check and reports.
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
// A failure panics with one short CloverBlock r127 line naming the step; success prints one line
// and lets the boot continue, except under clover-apfs=1, where the depth itself is the report and
// the run always panics with it. A refire of the IOBSD notification prints one counted line and
// returns, so the photograph shows both that the run completed and how many times it was offered.
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
#include <pexpert/pexpert.h>
#include <mach/kmod.h>

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
kern_return_t OSKextLoadKextWithIdentifier (const char *);
void clover_block_static_init (void) __asm__ ("__GLOBAL__sub_I_CloverBlock.cpp");
// Provided by drivers/CloverSdhci. `ready` is false until mode 21 has read EXT_CSD and gated the
// sector count against the logfs extent, so the block device can never claim a geometry the
// partition table does not describe. The window is logfs's own first LBA and length, published only
// when the GPT entry this boot read is that extent, and zero otherwise (round127).
uint32_t clover_emmc_block_ready (void);
uint32_t clover_emmc_block_sectors (void);
void clover_emmc_block_window (uint32_t *first, uint32_t *sectors);
uint32_t clover_emmc_block_read (uint32_t lba, uint32_t *words, uint32_t blocks);
uint32_t clover_emmc_block_write (uint32_t lba, const uint32_t *words, uint32_t blocks);
// The kernel's own block-device vnode constructor (bsd/vfs/vfs_subr.c). dev_t is the composite
// major<<24|minor this file passes; the vnode is never touched, and it stays for boot lifetime
// exactly as it would when it becomes the root vnode.
int bdevvp (int32_t dev, void **vpp);
}

static const uint32_t kBlockBytes = 512;
static const uint32_t kBlockWords = kBlockBytes / 4;
static const char kBSDNameKey[] = "BSD Name";
static const char kBSDMajorKey[] = "BSD Major";
static const char kBSDMinorKey[] = "BSD Minor";
// Step 9's rename target. The prefix is what matters: bsd_init.c's bsd_rooted_ramdisk() compares only
// the first strlen(RAMDEV) == 3 characters of rd= against "md0", while IOFindBSDRoot's memory-device
// shortcut demands the name end after its third character (rdBootVar[3] == 0), so this string clears
// the first test and fails the second -- rootauth gets skipped, the root still comes from the BSD-name
// match, and the trailing s39 keeps naming the same slice the storage family named disk0s39.
static const char kRootAlias[] = "md0s39";
// Round127's geometry, latched from drivers/CloverSdhci's window before the device is published and
// never touched again: zero sectors means the old whole-card description, nonzero means this device
// *is* logfs's partition, and every block number the storage stack hands down has the window's
// first LBA added to it before it reaches the card.
static volatile uint32_t gWindowFirst = 0;
static volatile uint32_t gWindowSectors = 0;
// Blocks this device actually served. Reset after this file's own first read so what the panic line
// reports is traffic the storage stack above it asked for, not the probe's.
static volatile uint32_t gBlocksRead = 0;
static volatile uint32_t gBlocksWritten = 0;
// The APFS container superblock: nxs_magic is the fourth field of nx_super_block, 32 bytes of
// reserved space into the 4096-byte block, and its bytes spell NXSB. Byte zero of the window is
// byte zero of the container, which is the whole reason the window is what this round describes.
static const uint32_t kNxsbOffset = 0x20;
static const uint8_t kNxsbMagic[4] = { 'N', 'X', 'S', 'B' };
static const char kApfsKext[] = "com.apple.filesystems.apfs";
// The three classes whose appearance under this media is the depth, in the order a successful
// attach produces them. Names are compared by metaCast, not by a matching dictionary: the walk only
// ever descends from this media, so anything found is attached to this media by construction.
static const char kApfsSchemeClass[] = "AppleAPFSContainerScheme";
static const char kApfsContainerClass[] = "AppleAPFSContainer";
static const char kApfsVolumeClass[] = "AppleAPFSVolume";

// The IOBSD publish notification is delivered on the persistent gIOResources service, whose
// doServiceMatch re-invokes every still-matching gIOPublishNotification on each pass and ignores the
// handler's return, so returning false does not retire it: every later publishResource() re-fires
// the handler. Round117 ran the whole body twice and the second postModLoad hit the duplicate-class
// path (CloverBlockDevice was already in sAllClassesDict) and dereferenced a NULL reserved->kext.
// gCloverBlockRan is a latch, not a lock: the first invocation flips 0->1 and runs, every refire
// sees 1 and returns after counting itself in gCloverBlockFires. Neither is ever cleared.
static volatile uint32_t gCloverBlockRan = 0;
static volatile uint32_t gCloverBlockFires = 0;

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

// The capacity this device claims: the window under round127, the whole card otherwise. Both are
// what the card reported to drivers/CloverSdhci, so a description here can still only be as good as
// a measurement made on this boot.
static uint32_t mediaSectors (void)
{
  uint32_t window = __atomic_load_n (&gWindowSectors, __ATOMIC_ACQUIRE);
  return window ? window : clover_emmc_block_sectors ();
}

IOReturn CloverBlockDevice::reportMaxValidBlock(UInt64 *maxBlock)
{
  if (!maxBlock) {
    return kIOReturnBadArgument;
  }
  uint32_t sectors = mediaSectors();
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
  uint32_t sectors = mediaSectors();
  uint32_t windowFirst = __atomic_load_n (&gWindowFirst, __ATOMIC_ACQUIRE);
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
      // The caller's block numbers are relative to what this device claims, and the card's are
      // absolute; the window's first LBA is the only difference between the two.
      uint32_t cardBlock = windowFirst + static_cast<uint32_t>(block + done);
      if (direction == kIODirectionOut) {
        if (buffer->readBytes(done * kBlockBytes, words, kBlockBytes) != kBlockBytes) {
          status = kIOReturnNoSpace;
          break;
        }
        if (clover_emmc_block_write(cardBlock, words, 1) != 1) {
          status = kIOReturnError;
          break;
        }
        __atomic_add_fetch(&gBlocksWritten, 1u, __ATOMIC_RELAXED);
      } else {
        if (clover_emmc_block_read(cardBlock, words, 1) != 1) {
          status = kIOReturnError;
          break;
        }
        if (buffer->writeBytes(done * kBlockBytes, words, kBlockBytes) != kBlockBytes) {
          status = kIOReturnNoSpace;
          break;
        }
        __atomic_add_fetch(&gBlocksRead, 1u, __ATOMIC_RELAXED);
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
  panic("CloverBlock r127: %s %llu", step, static_cast<unsigned long long>(value));
}

// The mask's job is done: round115 showed that the names this file wants are taken by the tree
// IOKit builds on its own. This walk finds the media the kernel itself named, and counts the named
// partitions under the whole disk, so a timeout says whether matching never ran or only got
// partway. gIOServicePlane children of the whole media are the partition scheme, whose children
// are the partition media; the depth limit only guards against a malformed tree.
static IOMedia *findNamedMedia(IORegistryEntry *root, const char *name,
                               uint32_t *partitions, uint32_t depth)
{
  OSSharedPtr<OSIterator> children = root->getChildIterator(gIOServicePlane);
  if (!children.get()) {
    return nullptr;
  }
  IOMedia *found = nullptr;
  while (!found) {
    OSObject *object = children->getNextObject();
    if (!object) {
      break;
    }
    IORegistryEntry *entry = OSDynamicCast(IORegistryEntry, object);
    if (!entry) {
      continue;
    }
    IOMedia *media = static_cast<IOMedia *>(entry->metaCast("IOMedia"));
    if (media) {
      OSString *bsdName = OSDynamicCast(OSString, property(media, kBSDNameKey));
      if (bsdName) {
        (*partitions)++;
        if (strcmp(bsdName->getCStringNoCopy(), name) == 0) {
          found = media;
          break;
        }
      }
    }
    if (depth < 3) {
      found = findNamedMedia(entry, name, partitions, depth + 1);
    }
  }
  return found;
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

// ---------------------------------------------------------------- round127's apfs depth

// Every service that descends from this media in the IOService plane attached to it, so a class name
// found here is attached to *this* disk, not to somebody else's reader: no matching dictionary (and
// so none of round116's double-release), no provider comparison, no second tree. The three flags are
// the depths a real attach produces in the order it produces them -- the scheme reads the container,
// the container object parses it, a volume appears only for a filesystem it can name -- and `seen`
// says how many services hung off the media at all, which tells "apfs never came" from "something
// came and stopped".
struct ApfsDepth {
  uint32_t seen;
  uint32_t scheme;
  uint32_t container;
  uint32_t volume;
};

static void measureApfsDepth(IORegistryEntry *entry, ApfsDepth *depth, uint32_t level)
{
  OSSharedPtr<OSIterator> children = entry->getChildIterator(gIOServicePlane);
  if (!children.get()) {
    return;
  }
  while (depth->seen < 64) {
    OSObject *object = children->getNextObject();
    if (!object) {
      break;
    }
    IORegistryEntry *child = OSDynamicCast(IORegistryEntry, object);
    if (!child) {
      continue;
    }
    depth->seen++;
    if (child->metaCast(kApfsSchemeClass)) {
      depth->scheme = 1;
    }
    if (child->metaCast(kApfsContainerClass)) {
      depth->container = 1;
    }
    if (child->metaCast(kApfsVolumeClass)) {
      depth->volume = 1;
    }
    if (level < 4) {
      measureApfsDepth(child, depth, level + 1);
    }
  }
}

// The whole of round127, and it never returns: the console is the thing this board cannot trust this
// late, so the depth is drawn on the panic screen like every other reading since round121.
__attribute__((noreturn)) static void apfsProbe(IOMedia *whole, IOService *client)
{
  // 1. Whether the container is where the window says it is, seen through IOMedia rather than
  // through this port's own PIO: byte zero of the media is block zero of logfs, and NXSB sits 32
  // bytes into that block.
  OSSharedPtr<IOBufferMemoryDescriptor> first =
      IOBufferMemoryDescriptor::withCapacity(kBlockBytes, kIODirectionIn, true);
  if (!first.get()) {
    fail("buffer", 0);
  }
  if (!whole->open(client, 0, kIOStorageAccessReader)) {
    fail("open", 0);
  }
  IOReturn status = readMediaBytes(whole, client, first.get(), 0, &gMediaOp);
  whole->close(client);
  if (status != kIOReturnSuccess) {
    fail("first-read", static_cast<uint32_t>(status));
  }
  const uint8_t *bytes = static_cast<const uint8_t *>(first->getBytesNoCopy());
  uint32_t magic = bytes && memcmp(bytes + kNxsbOffset, kNxsbMagic, sizeof(kNxsbMagic)) == 0 ? 1u : 0u;

  // 2. The media has to be free before anyone above it can claim it: the container scheme opens its
  // provider, and a probe that still held the read open would be measuring its own lease. The close
  // above is the last thing this file does to the media; everything below is registry traffic.
  //
  // apfs.kext is in the boot kext collection, not the linked kernel, exactly like IOStorageFamily --
  // and nothing in the boot asks for a filesystem on a disk that only just appeared. Loading it is
  // also what schedules matching for its personalities, which is why this happens before the wait.
  kern_return_t loaded = OSKextLoadKextWithIdentifier(kApfsKext);

  // 3. Poll, do not block on a semaphore: the attach this is waiting for is performed by the same
  // configuration machinery that is running this handler, so the wait has to keep handing control
  // back. Step 5 of the partition path has been doing exactly this since round116, and the budget
  // here is the same shape -- 30 s at 10 ms a turn, which fits long before the tick watchdog's
  // 60000 ticks. The counters start now, so what they report is the storage stack's traffic and not
  // the one read taken above.
  __atomic_store_n(&gBlocksRead, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&gBlocksWritten, 0, __ATOMIC_RELEASE);
  ApfsDepth best = { 0, 0, 0, 0 };
  for (uint32_t attempt = 0; attempt < 3000 && !best.volume; attempt++) {
    ApfsDepth now = { 0, 0, 0, 0 };
    measureApfsDepth(whole, &now, 0);
    if (now.seen > best.seen) {
      best.seen = now.seen;
    }
    if (now.scheme) {
      best.scheme = 1;
    }
    if (now.container) {
      best.container = 1;
    }
    if (now.volume) {
      best.volume = 1;
    }
    IOSleep(10);
  }

  panic("CloverBlock r127: apfs f=%u n=%u m=%u l=%u d=%u s=%u c=%u v=%u r=%u w=%u",
        __atomic_load_n(&gWindowFirst, __ATOMIC_ACQUIRE),
        __atomic_load_n(&gWindowSectors, __ATOMIC_ACQUIRE), magic,
        static_cast<uint32_t>(loaded), best.seen, best.scheme, best.container, best.volume,
        __atomic_load_n(&gBlocksRead, __ATOMIC_ACQUIRE),
        __atomic_load_n(&gBlocksWritten, __ATOMIC_ACQUIRE));
}

extern "C" void clover_block_run(void)
{
  // 0. Latch. The IOBSD publish notification is re-delivered on the persistent gIOResources service
  // every time a later publishResource() re-matches it, and returning false does not retire it, so
  // this body is offered more than once per boot. Only the first offer may run: preModLoad +
  // clover_block_static_init + postModLoad register CloverBlockDevice, and a second run would find
  // it already in sAllClassesDict and take the duplicate-class panic path (round117's data abort on
  // a NULL reserved->kext). Count every offer, admit exactly one, and report each refire so the
  // photograph shows the notification really did fire again.
  uint32_t fires = __atomic_add_fetch(&gCloverBlockFires, 1u, __ATOMIC_RELAXED);
  uint32_t expected = 0;
  if (!__atomic_compare_exchange_n(&gCloverBlockRan, &expected, 1u, false,
                                   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    IOLog("CloverBlock r127: refire n=%u (already ran)\n", fires);
    return;
  }

  // 1. The PIO path has to be proven before anything claims a device over it.
  if (!clover_emmc_block_ready() || clover_emmc_block_sectors() == 0) {
    fail("ready", 0);
  }

  // 1b. Which geometry to describe. `clover-apfs=1` asks for logfs's own window, and the window only
  // exists if this boot's card reported GPT entry 39 as exactly the extent drivers/CloverSdhci reads:
  // a media that claimed a partition the partition table does not describe would be a second card,
  // and every filesystem above it would be reading the wrong bytes. Latched here, before the device
  // is published, because reportMaxValidBlock() is what IOBlockStorageDriver builds the media from
  // and the size has to be right the first time anything reads it.
  uint32_t apfs = 0;
  if (PE_parse_boot_argn("clover-apfs", &apfs, sizeof(apfs)) && apfs) {
    uint32_t first = 0;
    uint32_t sectors = 0;
    clover_emmc_block_window(&first, &sectors);
    if (!first || !sectors) {
      // The printed value is the sector count. Zero means the entry never matched; a matching entry with
      // first LBA zero is not a thing a GPT can hold, so the two cases do not need separating.
      fail("window", sectors);
    }
    __atomic_store_n(&gWindowFirst, first, __ATOMIC_RELEASE);
    __atomic_store_n(&gWindowSectors, sectors, __ATOMIC_RELEASE);
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
  if (!whole || whole->getSize() / kBlockBytes != mediaSectors()) {
    fail("media", 0);
  }

  // 4b. Under the window this is where the run ends, and deliberately so: steps 5-9 below are the
  // partition-named root's measurements, and a window has no GPT at its block zero, hence no
  // IOGUIDPartitionScheme, no disk0s39, no root dictionary and no bdevvp. What round127 is asking
  // instead is whether Apple's own filesystem lives on top of this tree.
  if (__atomic_load_n(&gWindowSectors, __ATOMIC_ACQUIRE)) {
    apfsProbe(whole, device);
  }

  // 5. The tree is not built here. The driver's own registerService() on this media is what makes
  // IOKit start IOGUIDPartitionScheme and IOMediaBSDClient for it, and those clients name the
  // whole disk and every partition. Round115's hand-built scheme was a second tree and lost every
  // name to EEXIST. Wait for the automatic pass, then take the logfs partition from it by the name
  // the kernel wrote.
  uint32_t partitions = 0;
  IOMedia *target = nullptr;
  bool wholeNamed = false;
  for (uint32_t attempt = 0; attempt < 100 && !target; attempt++) {
    partitions = 0;
    target = findNamedMedia(whole, "disk0s39", &partitions, 0);
    wholeNamed = property(whole, kBSDNameKey) != nullptr;
    if (!target) {
      IOSleep(10);
    }
  }
  // The high half says whether the whole disk itself was named when the wait ended; the low half
  // is the number of partition media under it that carry a BSD name.
  if (!target || !wholeNamed) {
    fail("auto", (wholeNamed ? 0x10000u : 0) | (partitions & 0xffff));
  }

  OSString *bsdName = OSDynamicCast(OSString, property(target, kBSDNameKey));
  OSNumber *bsdMajor = OSDynamicCast(OSNumber, property(target, kBSDMajorKey));
  OSNumber *bsdMinor = OSDynamicCast(OSNumber, property(target, kBSDMinorKey));
  if (!bsdName || !bsdMajor || !bsdMinor) {
    fail("bsd-type", 0);
  }

  // 6. The exact discovery setconf() performs for rd=<bsd name>. waitForService() releases the
  // matching dictionary itself before it returns -- IOFindBSDRoot() retains once per wait for the
  // same reason. Round116 released it a second time on return and faulted reading the freed
  // dictionary's vtable at 0xfffffff00799238c.
  OSDictionary *matching = IOBSDNameMatching(bsdName->getCStringNoCopy());
  mach_timespec_t timeout = { 1, 0 };
  IOService *matched = nullptr;
  if (matching) {
    matching->retain();
    matched = IOService::waitForService(matching, &timeout);
    OSSafeReleaseNULL(matching);
  }
  if (matched != target) {
    fail("root-match", reinterpret_cast<uint64_t>(matched));
  }

  // 7. bdevvp() is what vfs_mountroot() calls on that dev_t. It opens the minor through the
  // bdevsw IOMediaBSDClient installed, so this both proves the number and leaves the vnode the
  // root mount would use.
  int32_t rootdev = (static_cast<int32_t>(bsdMajor->unsigned32BitValue()) << 24) |
                    static_cast<int32_t>(bsdMinor->unsigned32BitValue());
  void *rootvp = nullptr;
  int error = bdevvp(rootdev, &rootvp);
  if (error || !rootvp) {
    fail("bdevvp", static_cast<uint32_t>(error));
  }

  // 8. The partition's offset mapping: the same block read through the whole media at the
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

  // 9. Hand the root a name the kernel mounts without a seal. Everything above was measured under the
  // name the storage family wrote; this is the last thing the run does, and it is what round121 asks.
  // bsd_rooted_ramdisk() (bsd_init.c:450) is the only gate between a mounted root and the exec of
  // /sbin/launchd here: it is true when rd= begins with RAMDEV, "md0" (bsd_init.c:447), and for such a
  // root bsd_init skips the FSIOC_KERNEL_ROOTAUTH block that round120 died in. The number the root
  // gets is unaffected -- IOFindBSDRoot still takes its BSD-name branch (:888, the memory shortcut
  // wants rdBootVar[3] == 0 at :849), still matches this media by the property written here, and still
  // reads this media's own major and minor. The bytes stay eMMC's; only the authentication is bypassed.
  // Snapshot the old name first: setProperty() replaces the registry value, and bsdName is a borrowed
  // pointer into it.
  char foundName[32];
  strlcpy(foundName, bsdName->getCStringNoCopy(), sizeof(foundName));
  if (!target->setProperty(kBSDNameKey, kRootAlias)) {
    fail("alias", 0);
  }
  OSString *renamed = OSDynamicCast(OSString, property(target, kBSDNameKey));
  if (!renamed || strcmp(renamed->getCStringNoCopy(), kRootAlias) != 0) {
    fail("alias-read", 0);
  }
  // And that the name the root search will look for is the one a fresh dictionary matches, the same way
  // step 6 checked the name it was given: waitForService releases the dictionary, so retain for it.
  OSDictionary *aliasMatch = IOBSDNameMatching(kRootAlias);
  IOService *aliasService = nullptr;
  if (aliasMatch) {
    mach_timespec_t oneSecond = { 1, 0 };
    aliasMatch->retain();
    aliasService = IOService::waitForService(aliasMatch, &oneSecond);
    OSSafeReleaseNULL(aliasMatch);
  }
  if (aliasService != target) {
    fail("alias-match", reinterpret_cast<uint64_t>(aliasService));
  }

  IOLog("CloverBlock r127: auto p=%u %s>%s %u:%u bd=1 map=1 fires=%u\n", partitions, foundName,
        kRootAlias, bsdMajor->unsigned32BitValue(),
        bsdMinor->unsigned32BitValue(), fires);
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
// Round114's bsd-props showed this hook is not sufficient by itself, and round115's mask found out
// why: the hook is fine and devfs is ready, but the names are taken before this run asks for them.
// The handler runs on a config thread, and by the time it reaches its own scheme, other config
// threads have already started the automatic partition scheme and BSD clients for the same media.
// Round116 therefore stops hand-building the tree and waits for the automatic one to name the
// partition, then uses that media.
//
// The boot does not wait for any of this: rd= is still md0 and round116 only asks whether the
// device side completes. When a later round points rd= at disk0s39, setconf()'s IOFindBSDRoot()
// supplies the wait.
static bool cloverBlockIOBSD(void *, void *, IOService *, IONotifier *)
{
  clover_block_run();
  // Returning false does NOT retire this notifier: it matches the persistent gIOResources service,
  // and doServiceMatch re-invokes every still-matching publish notification on each pass regardless
  // of the return value, so every later publishResource() re-fires the handler. The latch at the top
  // of clover_block_run() is what makes the body run once; this only stops the notifier from being
  // re-added by a waiter. The refire count it prints is the proof that this really fires again.
  return false;
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

