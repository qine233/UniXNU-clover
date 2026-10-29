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
// Rounds 129-131 aimed the mount at the first volume the registry walk reached and each got the same errno
// 13 back for a different guess about the caller's state. Round132 aims it at a volume it can name: the
// walk now reads each AppleAPFSVolume's own FullName/UUID/Sealed/Status and the container's UUID, counts
// how many volumes were published and how many of them are unsealed, and prefers an unsealed volume over a
// sealed one -- the same preference the base repository's qualified run expresses as a root-matching UUID.
// Nothing else about the mount moves, so the frame either shows the identity the host already read out of
// the flashed bytes and still says 13, or it says something the host did not predict.
//
// A failure panics with one short CloverBlock r137 line naming the step; success prints one line
// and lets the boot continue, except under a window (clover-apfs=1 or =2), where the APFS depth and
// what the vfs made of it are the report and the run always panics with them. A refire of the IOBSD
// notification prints one counted line and returns, so the photograph shows both that the run
// completed and how many times it was offered.
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
#include <libkern/crypto/sha2.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSObject.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
#include <sys/types.h>

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
// Round128: userdata's own window (GPT entry 64), zero until this boot's card reported that entry as
// the extent board.toml recorded. The root image is flashed at its byte zero, and the first 4 KiB of
// the partition is the SHNGROOT manifest that says where the disk images inside it are.
void clover_emmc_block_userdata_window (uint32_t *first, uint32_t *sectors);
uint32_t clover_emmc_block_read (uint32_t lba, uint32_t *words, uint32_t blocks);
uint32_t clover_emmc_block_write (uint32_t lba, const uint32_t *words, uint32_t blocks);
// Round171: the card's write cache is on, so a filesystem flush has to be a real commit. The host
// driver owns EXT_CSD and the command engine, so it owns the flush too: writing zero to CACHE_CTRL
// is what makes the card put its cached blocks on media, and writing one back is what lets the next
// batch be cached. One is success, zero a refusal; the cache statistics say whether the switch ever
// took on this card.
uint32_t clover_emmc_flush_cache (void);
void clover_emmc_cache_stats (uint32_t *size, uint32_t *on, uint32_t *flushes);
// The kernel's own block-device vnode constructor (bsd/vfs/vfs_subr.c). dev_t is the composite
// major<<24|minor this file passes; the vnode is never touched, and it stays for boot lifetime
// exactly as it would when it becomes the root vnode.
int bdevvp (int32_t dev, void **vpp);
// Round129's mount ladder, hand-declared for the same reason bdevvp is: these live in
// bsd/sys/mount_internal.h and bsd/sys/vnode.h, which are not among the headers this archive is
// compiled against. Each signature is copied from the source the boot links against
// (out/feeds/xnu-0b4b910d): vfs_rootmountalloc is vfs_subr.c:1176, VFS_MOUNT is kpi_vfs.c:193,
// VFS_ROOT is kpi_vfs.c:260, VFS_START is kpi_vfs.c:215, vfs_unbusy is vfs_subr.c:1088,
// mount_list_add is vfs_subr.c:7551, vnode_lookupat is vfs_subr.c:7641, vn_rdwr is vfs_vnops.c:915 and
// vnode_setmountedon is kpi_vfs.c:2257. The opaque handle types (mount_t,
// vnode_t, vfs_context_t, kauth_cred_t, proc_t) are all pointers, so void*; VFS_MOUNT's `data` is a
// user_addr_t, which is 64-bit; the two enum parameters are passed as ints.
int vfs_rootmountalloc (const char *fsname, const char *devname, void **mpp);
int VFS_MOUNT (void *mp, void *devvp, uint64_t data, void *ctx);
int VFS_ROOT (void *mp, void **vpp, void *ctx);
int VFS_START (void *mp, int flags, void *ctx);
void vfs_unbusy (void *mp);
int mount_list_add (void *mp);
int vnode_lookupat (const char *path, int flags, void **vpp, void *ctx, void *start_dvp);
int vnode_put (void *vp);
void vnode_setmountedon (void *vp);
int vn_rdwr (int rw, void *vp, void *base, int len, int64_t offset, int seg, int ioflg, void *cred,
             int *resid, void *p);
void *vfs_context_kernel (void);
void *vfs_context_ucred (void *ctx);
// apfs's own boot-arg reader, the stub at 0xfffffff005767e1c which resolves to _PE_parse_boot_argn
// (0xfffffff00794209c). It returns nonzero when the argument was present, and this file asks for the same
// key, the same buffer size (0x20) and the same call apfs makes at 0xfffffff0056eb580.
int PE_parse_boot_argn (const char *arg_string, void *arg_ptr, int arg_len);
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
// Round156's staging buffer: one doAsyncReadWrite call moves a run of blocks through it, so the
// card sees one CMD18 instead of one per 512 bytes. It is static because this kernel's stack
// cannot hold it.
// Round171: the run is 128 blocks (64 KiB), matching CloverSdhci's kReadChunkBlocks so a caller's
// request reaches the card as one command instead of four, and the busy flag is replaced by an
// IOLock. The r156 CAS only ever tried 1024 spins and then dropped the loser to the per-block path,
// which on a boot where APFS reads through several threads is how a 64 KiB request became 128 CMD17s
// at 1-2 MB/s. drivers/NativeUfs/NativeUfs.cpp is the reference here: `windowLock` with
// IOLockLock/IOLockUnlock around the window copy, allocated up front, and the completion callback
// run outside the guard -- every caller waits for the buffer instead of abandoning it, so every
// caller gets the batch path.
static const uint32_t kBatchBlocks = 128;
static const uint32_t kBatchWords = kBatchBlocks * kBlockWords;
static uint32_t gBatchWords[kBatchWords];
static IOLock *gBatchLock = nullptr;
// Writes this device has handed the card since the last committed flush. Round171 switches the
// card's write cache on, so a sync only has to spend a CMD6/CMD13 round trip when there is actually
// something to commit; without this counter every filesystem sync (and every requestIdle before a
// power transition) paid for a cache that might already be empty.
static volatile uint64_t gWritesSinceFlush = 0;
// Round128's manifest: the first 4 KiB of userdata, as `./unixnu build userdata` writes it and as
// drivers/NativeUfs/RootLayout.h reads it. 4096 bytes is eight of this device's 512-byte blocks,
// and it is read through the PIO path in one request rather than eight. Entry i starts at byte 24 +
// 128 i: name[32], u64 offset, u64 size, u32 block size, u32 flags, sha256[32]. The digest at byte
// 4064 is deliberately not checked -- this file links no hash, and what a torn image would change is
// the offsets, which the bounds below refuse on their own.
static const uint32_t kRootManifestBlocks = 8;
static const uint32_t kRootManifestBytes = kRootManifestBlocks * kBlockBytes;
static const uint8_t kRootMagic[8] = { 'S', 'H', 'N', 'G', 'R', 'O', 'O', 'T' };
static const char kRootWindowName[3] = { 'O', 'S', 0 };
// Round143's second window, entry 1 of the same manifest: the SystemOS cryptex volume. Round142's
// launchd died inside dyld (`namespace 6 subcode 0xd`, `/usr/lib/libSystem.B.dylib ... no such file,
// no dylib cache`), and the host says why: the OS volume carries neither that dylib nor
// System/Library/Caches/com.apple.dyld, and this volume carries the shared cache. sheng boots the
// same image by union-mounting this volume on "/" before launchd's exec, so the window is published
// and the two UUIDs below are the selection the owner mount reads.
static const char kSystemWindowName[] = "SystemOS";
static const char kRootVolumeUuid[] = "04769D26-2CA0-4825-B443-E2E5F88300F2";
static const char kSystemOsVolumeUuid[] = "8846FE78-2910-4297-AF97-D59BB6F8FC16";
static volatile uint32_t gSystemWindowFirst = 0;
static volatile uint32_t gSystemWindowSectors = 0;

// Round159: the mutable Data window. sheng seeds its Data volume from a RAM disk and lets the
// owner mount at launchd's exec attach it read-write; clover has no RAM block device, so the seed
// is a third SHNGROOT window on the card -- the 32 MiB APFS container scripts/append-data-window.py
// appends after SystemOS. The access path is the same one the owner mount asks sheng for: the
// kernel patch's native_ram_block_volume_seed() contract is answered here by a window provider
// instead of a memory disk, so kern_exec.c itself is not touched.
static const char kDataWindowName[] = "Data";
static const char kDataVolumeUuid[] = "FC1A580E-2D67-48FC-B8CF-87422A319340";
static volatile uint32_t gDataWindowFirst = 0;
static volatile uint32_t gDataWindowSectors = 0;
// Each entry's own identity, kept so native_verified_block_manifest_sha384() can name the exact
// bytes the manifest declared rather than rehashing a multi-gigabyte window at every exec.
struct ManifestEntry {
  uint8_t sha256[32];
  uint64_t offset;
  uint64_t size;
};
static ManifestEntry gOsEntry;
static ManifestEntry gSystemEntry;
static ManifestEntry gDataEntry;
// The OS window is a whole GPT disk, not a bare container: `hdiutil imageinfo` on the IPSW's own
// 044-76040-038.dmg reports MBR, Primary GPT Header, Primary GPT Table, one Apple_APFS partition and
// the backup, and the host-side copy of the flashed bytes says the same. So byte zero of the media
// this round describes is an MBR, and the reading that matters is the partition header one block in.
static const uint8_t kEfiPartMagic[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
static const uint32_t kGptHeaderOffset = kBlockBytes;
static uint32_t gRootManifest[kRootManifestBytes / 4];
static const char kApfsKext[] = "com.apple.filesystems.apfs";
// The three classes whose appearance under this media is the depth, in the order a successful
// attach produces them. Names are compared by metaCast, not by a matching dictionary: the walk only
// ever descends from this media, so anything found is attached to this media by construction.
static const char kApfsSchemeClass[] = "AppleAPFSContainerScheme";
static const char kApfsContainerClass[] = "AppleAPFSContainer";
static const char kApfsVolumeClass[] = "AppleAPFSVolume";
// The four properties that say *which* volume a node is. Rounds 128-131 counted volume nodes and
// mounted the first one the walk reached, and not one word of what that node calls itself ever came
// back on the screen -- so "we have been mounting the sealed volume" was an interpretation nobody could
// check, only assume. These are the names apfs publishes on its own AppleAPFSVolume objects, and they are
// the same four the base repository's own original-OS checkpoint reads out of IOKit's registry string log
// (scripts/check-original-apfs-root-checkpoints.py:50-56: UUID, Status, Sealed, FullName for the volume it
// mounts with return 0). Reading them as OSString is therefore not a guess about the type either.
static const char kFullNameKey[] = "FullName";
static const char kVolumeUuidKey[] = "UUID";
static const char kSealedKey[] = "Sealed";
static const char kStatusKey[] = "Status";
// 'N','o' little-endian -- what the unsealed volume the base repository mounts publishes. The same field
// on a sealed volume reads "Yes", and on a broken seal the head of "Broken"; anything else is its own
// reading, which is why the word itself is drawn rather than a yes/no bit.
static const uint32_t kSealedNo = 0x6f4e;
// A property that was not there, or was not a string. The mount ladder already prints 0xffff for "this
// rung never ran", so an absence here does not masquerade as a zero value.
static const uint32_t kNoProperty = 0xffff;
// The name vfs_rootmountalloc() looks a vfstable up by: bsd/vfs/vfs_subr.c:1181 compares it against
// `vfc_name`, which is what apfs.kext registered when round128's OSKextLoadKextWithIdentifier ran.
static const char kApfsFsName[] = "apfs";
// The three values the hand-declared prototypes' enum arguments need, spelled out here because the
// headers that name them are not in this archive's include path: <sys/uio.h>'s `enum uio_rw` lists
// UIO_READ first, its `enum uio_seg` gives UIO_SYSSPACE the explicit value 2, and <sys/vnode.h>
// defines IO_UNIT as 0x0001 ("do I/O as an atomic unit"), which keeps vn_rdwr() from clustering the
// read into requests this device has never seen.
static const int kUIORead = 0;
static const int kUIOSysSpace = 2;
static const int kIOUnit = 0x0001;
// The file whose bytes are this round's report, walked one component per lookup: on a board with no
// console, a four-level path has to say which level it stopped at. SystemVersion.plist is the file
// every iPadOS system volume carries for the software-update machinery, it is a binary plist (so the
// first eight bytes have a magic worth printing), and it is small enough to read in one request.
static const char *const kVolumePath[4] = { "System", "Library", "CoreServices", "SystemVersion.plist" };
static const uint32_t kFileBytes = 1024;
// struct mount and struct vfsstatfs as *this* build lays them out. The numbers are measured, not
// guessed: the dSYM beside kernel.macho carries 63 copies of struct mount (one per translation unit),
// all of them agreeing on mnt_vfsstat at 0x84 and mnt_devvp at 0x980 in a 0xa08-byte struct, and the
// same 63 copies of vfsstatfs agree on f_fstypename at +0x50 and f_mntfromname at +0x460. The probe
// still refuses to trust the measurement: vfs_rootmountalloc_internal() strlcpy()s vfc_name into
// f_fstypename (bsd/vfs/vfs_subr.c:1161) and copystr()s the devname it was given into f_mntfromname
// (:1164), so the struct it just allocated has to spell "apfs" at mp+0xd4 before this file writes a
// pointer at mp+0x980. Both are little-endian, and MFSTYPENAMELEN is 16, so a correct read is
// 0x73667061 ('a','p','f','s') followed by three zero words.
static const uint32_t kMntDevvp = 0x980;
static const uint32_t kFsTypeName = 0x84 + 0x50;
static const uint32_t kMntOnName = 0x84 + 0x60;
static const uint32_t kMntFromName = 0x84 + 0x460;
static const uint32_t kApfsNameWord = 0x73667061;
static const uint32_t kMountPointSlash = 0x2f;
// Round131's three, from the same dSYM pass and the same 63 agreeing copies: mnt_flag at 0x70,
// mnt_kern_flag at 0x74 and mnt_op at 0x28. mnt_op is what vfs_rootmountalloc_internal() fills from
// vfsp->vfc_vfsops (bsd/vfs/vfs_subr.c:1151), struct vfsops is 0xa0 bytes, and its first member is
// vfs_mount -- so *mnt_op is the address of the function that has been answering 13. The flag pair is
// the other half of this round: :1153 writes MNT_RDONLY | MNT_ROOTFS into mnt_flag, which means every
// mount this probe has ever asked for declared itself a boot root. MNT_RDONLY is bsd/sys/mount.h:296
// and MNT_ROOTFS is :326.
static const uint32_t kMntFlag = 0x70;
static const uint32_t kMntKernFlag = 0x74;
static const uint32_t kMntOp = 0x28;
static const uint32_t kMntRdonly = 0x1;
static const uint32_t kMntRootfs = 0x4000;
static const uint32_t kRootMountFlags = kMntRdonly | kMntRootfs;
// Round134 reads the block-special vnode itself after a failed mount. Four KiB is the same block-0
// request size nx_mount makes, and offset zero is aligned for spec_read's VBLK path. This is a new
// measurement after VFS_MOUNT has returned, not a pre-read that could change the mount result.
static const uint32_t kDeviceReadBytes = 4096;
// Round135's second half of the same question. vn_rdwr_64() (bsd/vfs/vfs_vnops.c:995-998) runs
// mac_vnode_check_read() before VNOP_READ unless the caller sets IO_NOAUTH, and IO_NOAUTH is 0x8000
// (bsd/sys/vnode.h:174) while IO_UNIT, the flag rounds 129-134 passed, is 0x0001 (:154). So round134's
// dr=13 has two producers in front of the media, and only one of them is the device: an authorization
// denial in the MAC framework and a refusal from underneath VNOP_READ look the same number. Repeating
// the identical read with IO_NOAUTH added separates them without touching any state -- na=0 with dr=13
// names the authorization layer, and na=13 says the refusal is below it.
static const int kIOFlagNoAuth = 0x8000;

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
  // Round171: one place decides whether the card's write cache has anything to commit. A flush is
  // two CMD6 switches and two CMD13 waits, so a filesystem that syncs a clean volume gets the answer
  // for free, and the first flush after a write is the one that costs the card its programming time.
  IOReturn commitCache(void);
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
  // Round171: the batch staging buffer's lock, allocated here rather than on first request the way
  // drivers/NativeUfs allocates `windowLock` before the device serves anything. init runs once,
  // before the device is published, so there is no race on the pointer; if it fails the batch path
  // is simply never taken (see doAsyncReadWrite) and every request goes one block at a time.
  if (!gBatchLock) {
    gBatchLock = IOLockAlloc();
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

IOReturn CloverBlockDevice::commitCache(void)
{
  // One counter for the whole driver, and one cache for the whole card: the logfs window and the two
  // boot windows are partitions of the same device, so whichever instance commits first empties the
  // queue for all of them. gWritesSinceFlush counts blocks rather than requests because a partial
  // batch that failed mid-chunk still left `written` blocks on the card.
  // Round190: the frame has to tell three states apart -- no barrier reached this driver at all, a
  // barrier arrived with nothing to commit, and a barrier arrived with blocks and is being committed.
  // The first needs no code here; the other two do, because "pending=0" returns before the driver is
  // ever called and would otherwise look exactly like a barrier that never came.
  const uint64_t pending = __atomic_load_n(&gWritesSinceFlush, __ATOMIC_RELAXED);
  if (!pending) {
    static bool emptyReported = false;
    if (!emptyReported) {
      emptyReported = true;
      IOLog("CloverBlock r190: sync pending=0 (once)\n");
    }
    return kIOReturnSuccess;   // nothing has been written since the last commit
  }
  IOLog("CloverBlock r190: sync-in pending=%llu\n", static_cast<unsigned long long>(pending));
  if (!clover_emmc_flush_cache()) {
    IOLog("CloverBlock r190: sync-out ok=0 pending=%llu\n", static_cast<unsigned long long>(pending));
    return kIOReturnError;
  }
  IOLog("CloverBlock r190: sync-out ok=1 pending=%llu\n", static_cast<unsigned long long>(pending));
  // The first commit of the boot is the line that says the write-back path is real: it names how
  // many blocks the card had been holding, and the driver-side counter says how many times the
  // cache has been switched off and back on. A frame with writes and no line here is a frame where
  // nothing ever synced, which is a different problem from a flush that failed.
  static bool committed = false;
  if (!committed) {
    committed = true;
    uint32_t size = 0;
    uint32_t on = 0;
    uint32_t flushes = 0;
    clover_emmc_cache_stats(&size, &on, &flushes);
    IOLog("CloverBlock r171: first commit blocks=%llu cache=%u on=%u flushes=%u\n",
          static_cast<unsigned long long>(__atomic_load_n(&gWritesSinceFlush, __ATOMIC_RELAXED)),
          size, on, flushes);
  }
  __atomic_store_n(&gWritesSinceFlush, 0, __ATOMIC_RELAXED);
  return kIOReturnSuccess;
}

IOReturn CloverBlockDevice::doSynchronizeCache(void)
{
  // Round171: the card's write cache is switched on by the host driver, so this is no longer a
  // no-op. A flush that was refused is reported as such -- the filesystem's own sync error is the
  // reading that says the commit did not happen, and pretending otherwise is how a torn checkpoint
  // reaches a frame two boots later.
  return commitCache();
}

// Round171: the one writer that has no filesystem above it to issue a sync. logfs records go straight
// to the card through IOMedia, and the boot that wrote the last one is the boot that is ending -- in
// a panic and a hard reset, with no unmount and therefore no synchronize call-out. A cache holding
// that record loses the frame this port reads its results from, so a write whose blocks fall inside
// the logfs window is committed here instead of waiting for a sync that is never coming. Writes
// anywhere else -- the Data volume's APFS -- keep the cache and let the filesystem sync it, which is
// the whole point of r171: r167's write-through made every block wait for its own programming, and
// that is what grew the checkpoint flush from 22.7 s to 102 s.
static bool overlapsLogfsWindow(uint64_t first, uint64_t count)
{
  uint32_t windowFirst = 0;
  uint32_t windowSectors = 0;
  clover_emmc_block_window(&windowFirst, &windowSectors);
  if (!windowSectors || !count) {
    return false;
  }
  return first < static_cast<uint64_t>(windowFirst) + windowSectors &&
         first + count > windowFirst;
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

// Each published media is one extent of the card, and round143 publishes two of them. The class
// layout is pinned to IOBlockStorageDevice's 0x90 bytes (the static_assert above), so the extent
// lives beside the object in this table rather than inside it. The lookup runs on every request and
// at media-build time, so it never allocates; a slot is claimed once, before attach, and the device
// pointer is the key.
struct DeviceWindow {
  const void *device;
  uint32_t first;
  uint32_t sectors;
  bool writeProtected;
};

static const uint32_t kDeviceWindowSlots = 4;
static DeviceWindow gDeviceWindows[kDeviceWindowSlots];

static void registerWindow(const void *device, uint32_t first, uint32_t sectors,
                           bool writeProtected)
{
  for (uint32_t slot = 0; slot < kDeviceWindowSlots; slot++) {
    if (!gDeviceWindows[slot].device || gDeviceWindows[slot].device == device) {
      gDeviceWindows[slot] = DeviceWindow{ device, first, sectors, writeProtected };
      return;
    }
  }
  panic("CloverBlock r143: window-slot %u", kDeviceWindowSlots);
}

static const DeviceWindow *windowFor(const void *device)
{
  for (uint32_t slot = 0; slot < kDeviceWindowSlots; slot++) {
    if (gDeviceWindows[slot].device == device) {
      return &gDeviceWindows[slot];
    }
  }
  return nullptr;
}

// The capacity this device claims: its window, the whole card otherwise. Both are what the card
// reported to drivers/CloverSdhci, so a description here can still only be as good as a measurement
// made on this boot.
static uint32_t mediaSectors (const CloverBlockDevice *device)
{
  const DeviceWindow *window = windowFor (device);
  uint32_t sectors = window ? window->sectors : 0;
  return sectors ? sectors : clover_emmc_block_sectors ();
}

IOReturn CloverBlockDevice::reportMaxValidBlock(UInt64 *maxBlock)
{
  if (!maxBlock) {
    return kIOReturnBadArgument;
  }
  uint32_t sectors = mediaSectors(this);
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
  // Round109 only read, and Stage 14 writes one logfs record through IOMedia, so the logfs device
  // has to be genuinely writable. Round143's two boot windows are not: they are the flashed images
  // the kernel mounts, and sheng's own block devices answer read-only here too. IOBlockStorageDriver
  // builds its media from this answer.
  const DeviceWindow *window = windowFor(this);
  *isWriteProtected = window ? window->writeProtected : false;
  return kIOReturnSuccess;
}

IOReturn CloverBlockDevice::getWriteCacheState(bool *enabled)
{
  if (!enabled) {
    return kIOReturnBadArgument;
  }
  // Round171: the answer is the card's, not a constant. r167 switched EXT_CSD CACHE_CTRL off and
  // paid for it with a 102 s tx_flush -- every write had to program to the media before the command
  // completed -- so the host driver now leaves the cache on and commits it on the filesystem's own
  // sync. Reporting that truth is what lets IOBlockStorageDriver keep its write-back behaviour
  // instead of adding a synchronous flush to every request.
  uint32_t size = 0;
  uint32_t on = 0;
  uint32_t flushes = 0;
  clover_emmc_cache_stats(&size, &on, &flushes);
  *enabled = on != 0;
  return kIOReturnSuccess;
}

IOReturn CloverBlockDevice::setWriteCacheState(bool enabled)
{
  // Same rule: the request is granted only if that is what the card is actually doing. Asking for
  // the cache off is refused rather than quietly ignored, because a caller that believes it turned
  // the cache off stops issuing syncs, and that is how uncommitted data survives a hard reset.
  uint32_t size = 0;
  uint32_t on = 0;
  uint32_t flushes = 0;
  clover_emmc_cache_stats(&size, &on, &flushes);
  if (enabled == (on != 0)) {
    return kIOReturnSuccess;
  }
  return kIOReturnUnsupported;
}

IOReturn CloverBlockDevice::doAsyncReadWrite(IOMemoryDescriptor *buffer,
                                             UInt64 block, UInt64 nblks,
                                             IOStorageAttributes *attributes,
                                             IOStorageCompletion *completion)
{
  IOReturn status = kIOReturnSuccess;
  UInt64 done = 0;
  // The caller's block numbers are relative to what this device claims, and the card's are
  // absolute; this device's window first LBA is the only difference between the two.
  const DeviceWindow *window = windowFor(this);
  uint32_t sectors = window && window->sectors ? window->sectors : clover_emmc_block_sectors();
  uint32_t windowFirst = window ? window->first : 0;
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
    // Round156: hand the card a run of blocks per call instead of one. The single-block
    // FIFO path costs a command, a response wait and a deadline check per 512 bytes;
    // dyld's shared cache is 3.7 GiB, so that overhead is most of what the read spends
    // its time on. The staging buffer is static because a 64 KiB array does not belong
    // on this kernel's stack, and Round171's IOLock is what keeps callers from sharing it.
    // A caller that cannot allocate the lock keeps the old per-block path, so this can only
    // make the device faster, never less able to complete a request.
    bool batched = gBatchLock != nullptr;
    if (batched) {
      IOLockLock(gBatchLock);
      while (done < nblks) {
        uint32_t chunk = static_cast<uint32_t>(nblks - done);
        if (chunk > kBatchBlocks) {
          chunk = kBatchBlocks;
        }
        uint32_t cardBlock = windowFirst + static_cast<uint32_t>(block + done);
        const uint32_t bytes = chunk * kBlockBytes;
        if (direction == kIODirectionOut) {
          if (buffer->readBytes(done * kBlockBytes, gBatchWords, bytes) != bytes) {
            status = kIOReturnNoSpace;
            break;
          }
          uint32_t written = clover_emmc_block_write(cardBlock, gBatchWords, chunk);
          __atomic_add_fetch(&gBlocksWritten, written, __ATOMIC_RELAXED);
          if (written != chunk) {
            // The card took `written` whole blocks before the command failed;
            // those did reach the media, so they are the only part of this chunk
            // that is done. The rest is left for the caller to retry.
            done += written;
            __atomic_add_fetch(&gWritesSinceFlush, written, __ATOMIC_RELAXED);
            status = kIOReturnError;
            break;
          }
          done += chunk;
          __atomic_add_fetch(&gWritesSinceFlush, chunk, __ATOMIC_RELAXED);
        } else {
          uint32_t read = clover_emmc_block_read(cardBlock, gBatchWords, chunk);
          __atomic_add_fetch(&gBlocksRead, read, __ATOMIC_RELAXED);
          if (read != chunk) {
            // Round157: none of these words reached the caller's buffer -- the
            // read failed before writeBytes ran -- so r156's `done += read` told
            // the completion that data had been delivered when it had not.
            status = kIOReturnError;
            break;
          }
          if (buffer->writeBytes(done * kBlockBytes, gBatchWords, bytes) != bytes) {
            // Same rule: a failed copy delivers no block of this chunk.
            status = kIOReturnNoSpace;
            break;
          }
          done += chunk;
        }
      }
      IOLockUnlock(gBatchLock);
    } else {
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
          __atomic_add_fetch(&gWritesSinceFlush, 1u, __ATOMIC_RELAXED);
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
  }

  // Round171: a write that landed in logfs's own extent is committed before the caller hears about
  // it. Only successful blocks are in the card, so the range checked is the range actually written.
  if (direction == kIODirectionOut && done &&
      overlapsLogfsWindow(static_cast<uint64_t>(windowFirst) + block, done)) {
    commitCache();
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
  // The driver's idle call-out is the other place a write-back cache has to be committed: it is
  // what runs before a power state change, and an uncommitted cache across a hard reset is the
  // exact failure r167 was afraid of. commitCache() answers the same way a sync does -- quiet card,
  // nothing pending, success -- and only pays for the CMD6 round trip when writes are outstanding.
  return commitCache();
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
  // Round171: IOBlockStorageDriver::synchronize() ends here, and the base class documents a barrier
  // synchronize as a full flush, so this is the path a filesystem's sync_fsk takes on its way to the
  // card. It is the same commit doSynchronizeCache() does -- which range is being synced makes no
  // difference to an eMMC write cache, whose flush is whole-device by definition (EXT_CSD
  // CACHE_CTRL=0 programs every block the card is holding).
  (void) block;
  (void) nblks;
  (void) options;
  return commitCache();
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
  panic("CloverBlock r137: %s %llu", step, static_cast<unsigned long long>(value));
}

static uint32_t le32At(const uint8_t *at)
{
  return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) |
         (static_cast<uint32_t>(at[2]) << 16) | (static_cast<uint32_t>(at[3]) << 24);
}

static uint64_t le64At(const uint8_t *at)
{
  return static_cast<uint64_t>(le32At(at)) | (static_cast<uint64_t>(le32At(at + 4)) << 32);
}

// A registry string, or nothing. Same wrapper every other property read in this file uses -- the
// non-deprecated form wants an OSString key and an option mask -- and the object getProperty() retains is
// left where it is on purpose: this runs once, at boot, on objects the registry keeps for the life of the
// machine, which is the trade round129 already made when it retained the volume's media.
static const char *stringProperty(IORegistryEntry *entry, const char *key)
{
  OSString *text = OSDynamicCast(OSString, property(entry, key));
  return text ? text->getCStringNoCopy() : nullptr;
}

// The first four bytes of a string, packed little-endian, so 'N','o' is 0x6f4e and 'O','n','l','i' is
// 0x696c6e4f. This is the shape the mount ladder already prints for "apfs" (0x73667061), so the frame has
// one way of showing a short word, not two.
static uint32_t packedHead(const char *text)
{
  uint32_t word = 0;
  for (uint32_t index = 0; index < 4 && text[index]; index++) {
    word |= static_cast<uint32_t>(static_cast<unsigned char>(text[index])) << (8 * index);
  }
  return word;
}

// FNV-1a over an ASCII string. The two strings this hashes are 36 and 16+ characters, far too long for a
// frame that already carries 44 numbers, and the point of drawing them is not to read them but to say "the
// registry on this card holds the same value the host reads out of the same bytes". A 32-bit hash proves
// that as exactly as the text does, and the host can compute the expected number before the photo is taken.
static uint32_t fnv1a(const char *text)
{
  uint32_t hash = 0x811c9dc5u;
  for (const char *at = text; *at; at++) {
    hash = (hash ^ static_cast<uint32_t>(static_cast<unsigned char>(*at))) * 0x010000193u;
  }
  return hash;
}

// One entry's declared identity. The bytes stay in gRootManifest for the life of the boot, so this is
// a copy of the three fields the manifest digest has to name exactly, not a pointer into the window.
static void rememberEntry(ManifestEntry *slot, const uint8_t *entry)
{
  memcpy(slot->sha256, entry + 56, sizeof(slot->sha256));
  slot->offset = le64At(entry + 32);
  slot->size = le64At(entry + 40);
}

// Latch the media to the windows inside the flashed root image. Everything the manifest holds is
// bytes and names, and each refusal is its own panic step, so a photograph can tell "nothing was
// flashed" (magic) from "a different release's manifest" (version) from "this partition is not the
// one the image was sized for" (bounds). The windows are published relative to the card: the offset
// in the manifest is relative to the start of userdata, and drivers/CloverSdhci's own read path only
// takes card LBAs. Round143 reads both entries in one pass: the OS window is what root-matching
// hands the kernel, and the SystemOS window is the cryptex volume the union mount on "/" needs.
// Round159 adds the third: Data, the only window whose entry must not claim write protection.
static void windowsFromManifest(uint32_t partitionFirst, uint32_t partitionSectors,
                                bool requireSystem, bool requireData)
{
  const uint32_t got = clover_emmc_block_read(partitionFirst, gRootManifest, kRootManifestBlocks);
  if (got != kRootManifestBlocks) {
    fail("root-read", got);
  }
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(gRootManifest);
  if (memcmp(bytes, kRootMagic, sizeof(kRootMagic)) != 0) {
    fail("root-magic", le32At(bytes));
  }
  const uint32_t version = le32At(bytes + 8);
  const uint32_t count = le32At(bytes + 12);
  if (version != 1) {
    fail("root-version", version);
  }
  if (!count || count > 4) {
    fail("root-count", count);
  }
  const uint64_t partitionBytes = static_cast<uint64_t>(partitionSectors) * kBlockBytes;
  bool rootFound = false;
  bool systemFound = false;
  bool dataFound = false;
  for (uint32_t index = 0; index < count; index++) {
    const uint8_t *entry = bytes + 24 + 128 * index;
    const bool root = memcmp(entry, kRootWindowName, sizeof(kRootWindowName)) == 0;
    const bool system = memcmp(entry, kSystemWindowName, sizeof(kSystemWindowName)) == 0;
    const bool data = memcmp(entry, kDataWindowName, sizeof(kDataWindowName)) == 0;
    if (!root && !system && !data) {
      continue;
    }
    // Both sealed windows are read-only images and say so in their entries. Data is the first window
    // the owner mount attaches for writing: a read-only entry there would mount, then fail at the
    // first write, so the manifest is refused here instead -- with its own tag.
    const uint32_t expectFlags = data ? 0u : 1u;
    const uint64_t offset = le64At(entry + 32);
    const uint64_t size = le64At(entry + 40);
    if (le32At(entry + 48) != kBlockBytes || le32At(entry + 52) != expectFlags) {
      fail(root ? "root-attr" : system ? "system-attr" : "data-attr", index);
    }
    if (offset < kRootManifestBytes || (offset % kBlockBytes) || size < kBlockBytes ||
        (size % kBlockBytes) || offset > partitionBytes || size > partitionBytes - offset) {
      fail(root ? "root-bounds" : system ? "system-bounds" : "data-bounds", offset);
    }
    const uint64_t first = partitionFirst + offset / kBlockBytes;
    const uint64_t sectors = size / kBlockBytes;
    if (sectors > UINT32_MAX || first > UINT32_MAX || first + sectors >
        static_cast<uint64_t>(clover_emmc_block_sectors())) {
      fail(root ? "root-geometry" : system ? "system-geometry" : "data-geometry", sectors);
    }
    // The entry copy happens-before the release stores of its window below, so a reader that
    // acquires the window's sector count also sees the declared offset, size and digest.
    rememberEntry(root ? &gOsEntry : system ? &gSystemEntry : &gDataEntry, entry);
    if (root) {
      __atomic_store_n(&gWindowFirst, static_cast<uint32_t>(first), __ATOMIC_RELEASE);
      __atomic_store_n(&gWindowSectors, static_cast<uint32_t>(sectors), __ATOMIC_RELEASE);
      rootFound = true;
    } else if (system) {
      __atomic_store_n(&gSystemWindowFirst, static_cast<uint32_t>(first), __ATOMIC_RELEASE);
      __atomic_store_n(&gSystemWindowSectors, static_cast<uint32_t>(sectors), __ATOMIC_RELEASE);
      systemFound = true;
    } else {
      __atomic_store_n(&gDataWindowFirst, static_cast<uint32_t>(first), __ATOMIC_RELEASE);
      __atomic_store_n(&gDataWindowSectors, static_cast<uint32_t>(sectors), __ATOMIC_RELEASE);
      dataFound = true;
    }
    // Keep looking for the other entries; the manifest has one of each and any order is valid.
  }
  if (!rootFound) {
    fail("root-name", count);
  }
  if (requireSystem && !systemFound) {
    fail("systemwin", count);
  }
  if (requireData && !dataFound) {
    fail("datawin", count);
  }
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

// Round135's direct read of the volume's own media gets its own completion slot rather than reusing
// gMediaOp, so the number drawn can never be the residue of the whole-disk GPT read above it.
static MediaOp gVolumeOp;

// Round136's read of the media the mount's dev_t actually names gets a third slot for the same reason,
// one level stronger: it runs after the volume read in the same function, and reusing gVolumeOp there
// would let a deferred completion overwrite ir= after it had been copied into the report.
static MediaOp gMinorOp;

// Round136: the dev_t VFS_MOUNT is handed names a (major, minor) pair, not an object. IOMediaBSDClient
// stamps both numbers on the media it registers, so a walk of the same subtree the depth pass walks can
// name the object that pair resolves to -- and comparing its address with the media this file mounted is
// the only way to tell "the volume we measured" from "whatever disk0s3 happens to be". Not retained, for
// the same reason findNamedMedia does not retain: the registry owns these for boot lifetime.
static IOMedia *findMediaByMinor(IORegistryEntry *root, uint32_t major, uint32_t minor,
                                 uint32_t depth)
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
      OSNumber *mediaMajor = OSDynamicCast(OSNumber, property(media, kBSDMajorKey));
      OSNumber *mediaMinor = OSDynamicCast(OSNumber, property(media, kBSDMinorKey));
      if (mediaMajor && mediaMinor && mediaMajor->unsigned32BitValue() == major &&
          mediaMinor->unsigned32BitValue() == minor) {
        found = media;
        break;
      }
    }
    if (depth < 8) {
      found = findMediaByMinor(entry, major, minor, depth + 1);
    }
  }
  return found;
}

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

// What one volume calls itself, in the four words the frame can hold. `name` is the head of FullName and
// is drawn as text rather than hashed because it is the field whose spelling has to be pointable at: the
// host copy of the bytes this window carries says `DawnUpdate21H461.J120J121J207J208OS`, and the first 16
// bytes of that are four words. `uuid` and `sealed` are what the base repository's root selection matches
// on and what the disassembled mount reads as its seal, respectively.
struct VolumeIdentity {
  uint32_t name[4];
  uint32_t uuid;
  uint32_t sealed;
  uint32_t status;
};

// Read the identity off the volume node itself. Every field is a registry string, and a missing one is
// drawn as kNoProperty so "apfs published nothing here" cannot be read as "it published a zero".
static void readVolumeIdentity(IORegistryEntry *volume, VolumeIdentity *identity)
{
  for (uint32_t index = 0; index < 4; index++) {
    identity->name[index] = 0;
  }
  const char *name = stringProperty(volume, kFullNameKey);
  if (name) {
    char head[16] = { 0 };
    for (uint32_t index = 0; index + 1 < sizeof(head) && name[index]; index++) {
      head[index] = name[index];
    }
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(head);
    for (uint32_t index = 0; index < 4; index++) {
      identity->name[index] = le32At(bytes + 4 * index);
    }
  }
  const char *uuid = stringProperty(volume, kVolumeUuidKey);
  identity->uuid = uuid ? fnv1a(uuid) : 0;
  const char *sealed = stringProperty(volume, kSealedKey);
  identity->sealed = sealed ? packedHead(sealed) : kNoProperty;
  const char *status = stringProperty(volume, kStatusKey);
  identity->status = status ? packedHead(status) : kNoProperty;
}

// Every service that descends from this media in the IOService plane attached to it, so a class name
// found here is attached to *this* disk, not to somebody else's reader: no matching dictionary (and
// so none of round116's double-release), no provider comparison, no second tree. The three flags are
// the depths a real attach produces in the order it produces them -- the scheme reads the container,
// the container object parses it, a volume appears only for a filesystem it can name -- and `seen`
// says how many services hung off the media at all, which tells "apfs never came" from "something
// came and stopped".
//
// Round128's tree is two levels deeper than round127's, and deliberately so: the partition scheme and
// the slice media are IOKit's own work between this media and the APFS scheme, so the walk has to
// reach past them. The cap on `seen` grew with it, because a partitioned disk carries a BSD client
// per partition as well.
struct ApfsDepth {
  uint32_t seen;
  uint32_t scheme;
  uint32_t container;
  uint32_t volume;
  // Round129 adds the second half of the answer: the flags say the tree grew, and the mount needs the
  // object the flags were only counting. This is the media the volume is reachable through -- the
  // AppleAPFSVolume itself when the class is its own IOMedia (which is how a scheme names a per-volume
  // device), otherwise its parent entry -- retained here because the walk's iterator is the only thing
  // keeping it alive otherwise.
  IOMedia *volumeMedia;
  // Round132's additions, and they are the answer to the question the mount ladder has been unable to
  // ask: `volumeCount` is how many volumes this container published at all, `unsealedCount` how many of
  // them say they are not sealed, `containerUuid` identifies the container they were published under, and
  // `identity` is the identity of whichever volume `volumeMedia` points at. Together they turn "apfs said
  // EACCES" into "apfs said EACCES about this volume, whose name is this".
  uint32_t volumeCount;
  uint32_t unsealedCount;
  uint32_t containerUuid;
  VolumeIdentity identity;
};

static void measureApfsDepth(IORegistryEntry *entry, ApfsDepth *depth, uint32_t level)
{
  OSSharedPtr<OSIterator> children = entry->getChildIterator(gIOServicePlane);
  if (!children.get()) {
    return;
  }
  while (depth->seen < 256) {
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
      if (!depth->containerUuid) {
        const char *uuid = stringProperty(child, kVolumeUuidKey);
        if (uuid) {
          depth->containerUuid = fnv1a(uuid);
        }
      }
    }
    if (child->metaCast(kApfsVolumeClass)) {
      depth->volume = 1;
      if (depth->volumeCount < 255) {
        depth->volumeCount++;
      }
      VolumeIdentity found{};
      readVolumeIdentity(child, &found);
      const bool unsealed = found.sealed == kSealedNo;
      if (unsealed) {
        depth->unsealedCount++;
      }
      // Round132's selection rule, and the reason this file reads an identity at all. Through round131 the
      // walk kept the *first* volume it reached -- `if (!depth->volumeMedia)` -- whatever that volume was,
      // so the mount was aimed at whichever node IOKit happened to attach first rather than at a volume
      // this file can name. The rule now is the one the base repository's qualified run uses (its root
      // matching is an IOPropertyMatch on a UUID that names the unsealed OS volume, and the sealed
      // SystemCryptex beside it is kept as the *negative* control -- that mount is expected to be refused
      // with the seal sentence), transcribed into the property that distinguishes the two here: an
      // unsealed volume wins, and a sealed one is mounted only when the container published nothing else.
      // With one volume on the container this changes nothing -- which is exactly what vt= and un= are
      // drawn to establish, because the frame will say whether there was ever a second volume to choose
      // between.
      IOMedia *media = static_cast<IOMedia *>(child->metaCast("IOMedia"));
      if (!media) {
        IORegistryEntry *parent = child->getParentEntry(gIOServicePlane);
        media = parent ? static_cast<IOMedia *>(parent->metaCast("IOMedia")) : nullptr;
      }
      if (media) {
        media->retain();
        const bool holdsSealed = depth->volumeMedia && depth->identity.sealed != kSealedNo;
        if (!depth->volumeMedia || (unsealed && holdsSealed)) {
          if (depth->volumeMedia) {
            depth->volumeMedia->release();
          }
          depth->volumeMedia = media;
          depth->identity = found;
        } else {
          media->release();
        }
      }
    }
    if (level < 7) {
      measureApfsDepth(child, depth, level + 1);
    }
  }
}

// ------------------------------------------------- round129-131's mount ladder

// One rung per field, and a field that was never reached says so. The reason the errnos are collected
// instead of panicking at the first non-zero one is that this board gets exactly one photo per flash:
// a mount that stops at the third path component still has to show the two that came before it, and a
// mount that runs to the end still has to show the bytes. 0xffff means "not attempted", 0 means the
// rung ran and passed.
struct MountReport {
  uint32_t major;
  uint32_t minor;
  uint32_t bdev;   // bdevvp() on the volume's own dev_t
  uint32_t alloc;  // vfs_rootmountalloc("apfs", <the volume's BSD name>)
  uint32_t name[4];  // the 16 bytes at mp+kFsTypeName -- "apfs" is 0x73667061, 0, 0, 0
  uint32_t on;     // the word at mp+kMntOnName: the kernel puts a single '/' there (vfs_subr.c:1162)
  uint32_t from;   // the first 4 bytes at mp+kMntFromName, the volume's own BSD name
  uint64_t devvp;  // what mp+kMntDevvp reads back, 0 if the layout proof refused the write
  uint32_t flag;   // mp+kMntFlag as the kernel left it; 0x4001|vfc_flags is what :1153 should write
  uint32_t cleared;  // mp+kMntFlag after MNT_ROOTFS came off, 0 when the flag proof refused the edit
  uint32_t kernFlag;  // mp+kMntKernFlag, drawn but never written
  uint64_t ops;    // mp+kMntOp: the filesystem's struct vfsops
  uint64_t mountFn;  // *ops, which is vfs_mount -- the function that has been returning 13
  uint32_t mount;  // VFS_MOUNT(mp, devvp, 0) -- the boot root's argument shape
  uint32_t start;  // VFS_START(mp, 0)
  uint32_t root;   // VFS_ROOT(mp)
  uint32_t lookup[4];
  uint32_t read;   // vn_rdwr of kFileBytes at offset 0
  uint32_t bytes;  // what that read actually filled
  uint32_t word0;
  uint32_t word1;
  // Round134's one new question, asked only after VFS_MOUNT has failed: does this exact devvp also
  // return EACCES for an independent 4 KiB read at offset zero? dr= is vn_rdwr's errno, db= the byte
  // count, and dx=/dy= the first eight bytes. A successful mount leaves dr= at kRungNotReached.
  uint32_t deviceRead;
  uint32_t deviceBytes;
  uint32_t deviceWord0;
  uint32_t deviceWord1;
  // Round135 asks which layer refuses, because dr=13 alone cannot say: the read that produced it went
  // through vn_rdwr_64(), and that function runs an authorization check before VNOP_READ unless the
  // caller sets IO_NOAUTH. na= is the same read with IO_NOAUTH added -- the only difference between dr=
  // and na= is that gate. ir= is the raw IOReturn from IOMedia::read() on the volume's own media object,
  // with ia= the bytes it says it moved, bypassing the vnode, the buffer cache and BSD entirely;
  // dkreadwrite()'s own early exits map to ENXIO/EINVAL/ENOMEM/EFAULT and can never be 13, so if a
  // number has to explain this, ir= is the field that carries it. mo=/mc=/mw= are the media's three
  // public state answers: isOpen(NULL), isOpen(this driver) and isWritable(), and IOMedia::read() gates
  // on its own open level -- kIOReturnNotOpen (0xe00002cd) is the one value in ir= that EACCES maps from
  // which the media itself can produce without a provider. All six stay at kRungNotReached if the mount
  // succeeds or the buffer allocation fails.
  uint32_t noAuthRead;
  uint32_t mediaRead;
  uint32_t mediaActual;
  uint32_t openAny;
  uint32_t openUs;
  uint32_t writable;
  // Round136 starts from what round135's BadArgument necessarily means if the shipped kext matches its
  // feed: with a non-null, In-direction buffer at byte zero, the only IOMedia::read() arm that can return
  // BadArgument is `_mediaSize - byteStart < buffer->getLength()` -- that is, the 512-byte probe was
  // longer than the media. ms= is that _mediaSize read back through the public getter, bs= its preferred
  // block size and mb= its base, all sampled before the mount so a later failure cannot hide them. ml=
  // is the length this round actually asks for, min(ms, 512), and md= that buffer's direction; asking for
  // no more than the media claims is what lets the request pass the same geometry test the mount's
  // (clipped) request passes and reach the media's provider, which the round135 probe never did.
  uint64_t mediaSize;
  uint32_t preferredBlock;
  uint64_t mediaBase;
  uint32_t bufferLength;
  uint32_t bufferDirection;
  // Round136's second half: the dev_t handed to VFS_MOUNT names a minor. xm= says a media carrying
  // (dv's major, dv's minor) was found in this disk's subtree, xe= that it is the very object sampled
  // above, xs=/xo=/xw= that media's size, isOpen(NULL) and isWritable(), and xr=/xa= a byte-zero read of
  // it with the same min(size, 512) rule, drawn only when xe=0 (when xe=1 the read above is that read).
  // wo= is the whole-disk media's isOpen(NULL), the fallback reading if no media carries the minor.
  uint32_t minorFound;
  uint32_t minorSame;
  uint64_t minorSize;
  uint32_t minorOpen;
  uint32_t minorWritable;
  uint32_t minorRead;
  uint32_t minorActual;
  uint32_t wholeOpen;
};

static const uint32_t kRungNotReached = 0xffff;

// The three numbers the volume is reached through. IOMediaBSDClient writes them when it attaches to
// the per-volume media, which is the same asynchronous configuration pass that named disk0s39 for
// step 5 of the partition path; the wait has the same shape, and a shorter budget, because this one
// is a leaf of a tree that already exists rather than the pass that builds the tree.
static bool volumeBsd(IOMedia *media, const char **name, uint32_t *major, uint32_t *minor)
{
  OSString *bsdName = OSDynamicCast(OSString, property(media, kBSDNameKey));
  OSNumber *bsdMajor = OSDynamicCast(OSNumber, property(media, kBSDMajorKey));
  OSNumber *bsdMinor = OSDynamicCast(OSNumber, property(media, kBSDMinorKey));
  if (!bsdName || !bsdMajor || !bsdMinor) {
    return false;
  }
  *name = bsdName->getCStringNoCopy();
  *major = bsdMajor->unsigned32BitValue();
  *minor = bsdMinor->unsigned32BitValue();
  return true;
}

// The volume whose UUID names it, reached through the same walk and with the same parent fallback
// measureApfsDepth uses. The returned media is retained. Selection by UUID rather than by "the first
// unsealed volume" is what makes round143's two windows addressable: root-matching hands the kernel
// the volume with kRootVolumeUuid, and the owner mount asks for the one with kSystemOsVolumeUuid, so
// both sides must be looking at the same node this file latched.
static IOMedia *findVolumeByUuid(IORegistryEntry *root, const char *wanted, uint32_t level)
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
    IORegistryEntry *child = OSDynamicCast(IORegistryEntry, object);
    if (!child) {
      continue;
    }
    if (child->metaCast(kApfsVolumeClass)) {
      const char *uuid = stringProperty(child, kVolumeUuidKey);
      if (uuid && strcmp(uuid, wanted) == 0) {
        IOMedia *media = static_cast<IOMedia *>(child->metaCast("IOMedia"));
        if (!media) {
          IORegistryEntry *parent = child->getParentEntry(gIOServicePlane);
          media = parent ? static_cast<IOMedia *>(parent->metaCast("IOMedia")) : nullptr;
        }
        if (media) {
          media->retain();
          found = media;
        }
      }
    }
    if (!found && level < 7) {
      found = findVolumeByUuid(child, wanted, level + 1);
    }
  }
  return found;
}

// The dev_t IOMediaBSDClient stamped on a volume's media: the number bdevvp() opens for the root and
// the one vnode_specrdev() reports for a mounted volume. Same packing as the root ladder's dv=.
static dev_t volumeDev(uint32_t major, uint32_t minor)
{
  return static_cast<dev_t>((static_cast<int32_t>(major) << 24) | static_cast<int32_t>(minor));
}

static const dev_t kNoDevice = static_cast<dev_t>(-1);

// The volumes the two windows produced, latched for boot lifetime. The owner mount at exec reads
// them long after this run returns, so the SystemOS media is retained; the OS volume is only needed
// as a number because root-matching already holds its media.
static volatile dev_t gOsVolumeDev = kNoDevice;
static volatile dev_t gSystemVolumeDev = kNoDevice;
static IOMedia *gSystemVolumeMedia = nullptr;
// Round159's Data volume. Unlike SystemOS this one is attached read-write, so its media has to stay
// alive for the life of the mount; the number alone is not enough to keep the tree that owns it up.
static volatile dev_t gDataVolumeDev = kNoDevice;
static IOMedia *gDataVolumeMedia = nullptr;

// vfs_mountroot(), transcribed: bsd/vfs/vfs_subr.c:1240 (bdevvp), :1258 (vfs_rootmountalloc), :1259
// (mnt_devvp), :1265 (VFS_MOUNT with no data argument), :1282 (SI_MOUNTEDON), :1284 and :1286
// (vfs_unbusy, mount_list_add) -- then the first lookup and the first read a mounted filesystem ever
// gets. Round129 could not copy the two struct-field rungs because this archive compiles no
// mount_internal.h, and its frame came back `bd=0 al=0 mt=13`: apfs knew the name, took the device
// vnode, and refused the mount without reading one new block off the card (r=464, round128's count to
// the byte). The only rung still missing after this round is vfs_init_io_attributes (:1294), which
// tunes the root's paging I/O sizes and has nothing to do with a mount that has not happened.
// Round130 copied the mnt_devvp rung and its frame came back with the layout proven and mt=13
// unchanged, so that rung is now disproved rather than missing; what round131 adds is not another
// transcription but the one thing the ladder had no answer for -- what the mount struct declares
// itself to be, and which function has been saying no.
//
// The field writes are gated on a proof rather than on the measurement. vfs_rootmountalloc_internal
// strlcpy()s the vfstable's name into f_fstypename, puts a single '/' at the head of f_mntonname
// (:1162) and copystr()s the devname into f_mntfromname (:1164) -- three marks the kernel writes
// itself, in three places, and none of them is the place this file is about to write into. So the
// gate is: mp+0xd4 must spell "apfs" (0x73667061 and three zero words) and mp+0xe4 must be 0x2f. Only
// then does this file put the device vnode at mp+0x980, and the frame shows what it wrote read back
// out of the struct -- which lets one photograph tell "the layout was wrong" from "the layout was
// right and mnt_devvp was not the reason". mp+0x4e4 is drawn as well, so dv=major:minor can be read
// as a statement about this volume's BSD name rather than about whichever media the BSD client
// happened to reach first.
//
// Every rung returns rather than panicking, so the caller can draw the rungs that ran.
static void volumeMountLadder(IOMedia *whole, IOMedia *volume, IOService *client,
                              MountReport *report)
{
  // Round136's geometry, read first and through public getters only. It is a property of the object, not
  // of the ladder, so it is drawn even on a run that stops before bdevvp or vfs_rootmountalloc.
  report->mediaSize = volume->getSize();
  report->preferredBlock = volume->getPreferredBlockSize();
  report->mediaBase = volume->getBase();

  // The device number and name still come from the AppleAPFSVolume's own IOMedia. Round133 proved that
  // parsing rd=md0 says nothing about the loader when devvp is non-null, so this round leaves the boot
  // argument alone and measures the device vnode that VFS_MOUNT actually received.
  const char *devname = nullptr;
  for (uint32_t attempt = 0; attempt < 1000 && report->major == kRungNotReached; attempt++) {
    if (!volumeBsd(volume, &devname, &report->major, &report->minor)) {
      IOSleep(10);
    }
  }
  if (report->major == kRungNotReached) {
    return;
  }

  void *ctx = vfs_context_kernel();
  void *devvp = nullptr;
  int32_t dev = (static_cast<int32_t>(report->major) << 24) | static_cast<int32_t>(report->minor);
  report->bdev = static_cast<uint32_t>(bdevvp(dev, &devvp));
  if (report->bdev || !devvp) {
    return;
  }

  void *mp = nullptr;
  report->alloc = static_cast<uint32_t>(vfs_rootmountalloc(kApfsFsName, devname, &mp));
  if (report->alloc || !mp) {
    return;
  }

  // mp+0xd4 is the name the kernel just strlcpy()ed in, and mp+0x4e4 the devname it just copystr()ed
  // in. Drawing both is the self-proof: the first has to be "apfs" for the struct to be the struct this
  // file thinks it is, and the second is what makes dv=major:minor a statement about this volume rather
  // than about whichever media the BSD client got to first.
  const uint8_t *mountBytes = reinterpret_cast<const uint8_t *>(mp);
  for (uint32_t index = 0; index < 4; index++) {
    report->name[index] = le32At(mountBytes + kFsTypeName + 4 * index);
  }
  report->on = le32At(mountBytes + kMntOnName);
  report->from = le32At(mountBytes + kMntFromName);
  if (report->name[0] == kApfsNameWord && !report->name[1] && !report->name[2] && !report->name[3] &&
      report->on == kMountPointSlash) {
    void **slot = reinterpret_cast<void **>(const_cast<uint8_t *>(mountBytes) + kMntDevvp);
    *slot = devvp;
    report->devvp = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(*slot));
  }

  // Round131's rung, and it is a question rather than a transcription: vfs_rootmountalloc_internal()
  // writes MNT_RDONLY | MNT_ROOTFS into mnt_flag (bsd/vfs/vfs_subr.c:1153) and only then ORs in the
  // vfstable's visible flags, so a mount struct that came out of that allocator has to carry both
  // bits -- which makes the expected value this field's own layout proof, in the same shape as the
  // three name markers above. Only when both are where :1153 puts them does this file drop
  // MNT_ROOTFS, and fc= is the field read back after the edit, so one frame still separates "the flag
  // was not there to clear" from "it was cleared and apfs refused anyway". Why ask it at all: the
  // base repository's own APFS fixture mounts a volume successfully with a mount_apfs whose mode is
  // 1 (an ordinary data mount) and records that mode 0 -- a request to serve as the boot root -- is
  // refused, and every mount this port has asked for has declared itself a boot root by construction.
  //
  // ops and mountFn are pure observation. mnt_op is the vfstable's vfsops table, struct vfsops is
  // 0xa0 bytes with vfs_mount as its first member, and this build stores those pointers unsigned, so
  // mountFn is the address of the very function that answered 13 in rounds 129, 130 and this one.
  // Drawing it costs nothing and turns the next refusal into a disassembly instead of a guess.
  uint32_t *flagSlot = reinterpret_cast<uint32_t *>(const_cast<uint8_t *>(mountBytes) + kMntFlag);
  report->flag = *flagSlot;
  report->kernFlag = le32At(mountBytes + kMntKernFlag);
  if (report->name[0] == kApfsNameWord && !report->name[1] && !report->name[2] && !report->name[3] &&
      report->on == kMountPointSlash && (report->flag & kRootMountFlags) == kRootMountFlags) {
    *flagSlot = report->flag & ~kMntRootfs;
    report->cleared = *flagSlot;
  }
  report->ops = le64At(mountBytes + kMntOp);
  if (report->ops) {
    report->mountFn = le64At(reinterpret_cast<const uint8_t *>(report->ops));
  }

  // Keep the mount itself byte-identical to rounds 129-133. Round133's path-open instrument is withdrawn:
  // container_load_with_callback skips its path vnode_open when the devvp argument is non-null, which is
  // exactly the case here, so opening /dev/md0 could never have measured the producer of mt=13. Nor is that
  // path the errno slot's only writer: nx_mount can propagate an error from its block-0 device read.
  //
  // The candidate chain for that read is concrete. spec_strategy has no permission gate; dkstrategy reaches
  // IOMedia::read, whose early kIOReturnNotOpen/NotReadable-style failures are converted by
  // IOService::errnoFromReturn to EACCES, stored in the buffer by dkreadwritecompletion, returned by
  // buf_biowait, and propagated through _vnode_dev_read and nx_mount into the loader's errno out parameter.
  // That chain is a candidate, not a proven runtime cause, and the host read the reason this round:
  // bdevvp() has no alias short-circuit in front of its open -- vnode_create(), VNOP_FSYNC(),
  // buf_invalidateblks() and VNOP_OPEN(nvp, FREAD) run in that order on every call (vfs_subr.c:2106-2170),
  // and a failed open panics. So bd=0 with no panic means spec_open() ran dkopen(dev, FREAD, S_IFBLK) and
  // dkopen's only EACCES is the FWRITE-and-!isWritable() arm, which a FREAD open cannot reach. Round135
  // therefore measures both ends of that chain instead of assuming either: the same read with the vnode's
  // one authorization gate removed, and one read of the media object itself.
  report->mount = static_cast<uint32_t>(VFS_MOUNT(mp, devvp, 0, ctx));
  if (report->mount) {
    // One question, after the mount can no longer be perturbed by it: can the same block-special vnode be
    // read independently? spec_read's VBLK arm uses buf_getblk/VNOP_STRATEGY, the same dkstrategy/buffer
    // error channel as nx_mount. dr=13 supports a persistent device-side refusal; dr=0 says that explanation
    // does not hold after the mount and sends the host pass back to APFS's internal/container-reuse paths.
    // r= is deliberately sampled only in the panic call, after this read, so it includes any new media I/O.
    // Round134 read this as "the refusal happens above CloverBlockDevice", which is only true if the 13 came
    // from underneath VNOP_READ -- the two rounds below are what separates that from the alternative.
    OSSharedPtr<IOBufferMemoryDescriptor> device =
        IOBufferMemoryDescriptor::withCapacity(kDeviceReadBytes, kIODirectionIn, true);
    if (!device.get()) {
      fail("device-read-buffer", 0);
    }
    int deviceResid = 0;
    report->deviceRead = static_cast<uint32_t>(
        vn_rdwr(kUIORead, devvp, device->getBytesNoCopy(), static_cast<int>(kDeviceReadBytes), 0,
                kUIOSysSpace, kIOUnit, vfs_context_ucred(ctx), &deviceResid, nullptr));
    if (deviceResid < 0) {
      deviceResid = 0;
    } else if (deviceResid > static_cast<int>(kDeviceReadBytes)) {
      deviceResid = static_cast<int>(kDeviceReadBytes);
    }
    report->deviceBytes = kDeviceReadBytes - static_cast<uint32_t>(deviceResid);
    const uint8_t *deviceHead = static_cast<const uint8_t *>(device->getBytesNoCopy());
    if (deviceHead) {
      report->deviceWord0 = le32At(deviceHead);
      report->deviceWord1 = le32At(deviceHead + 4);
    }

    // Round135, part one: the identical 4 KiB read again, with IO_NOAUTH added to ioflg. That flag skips
    // exactly one thing -- the mac_vnode_check_read() at the head of vn_rdwr_64() -- and VNOP_READ and
    // everything under it then run byte-for-byte as before. na=13 beside dr=13 puts the refusal below the
    // authorization layer; na=0 beside dr=13 means round134 measured its own gate rather than the media,
    // and the device-read theory goes back on the shelf unexamined.
    OSSharedPtr<IOBufferMemoryDescriptor> noAuth =
        IOBufferMemoryDescriptor::withCapacity(kDeviceReadBytes, kIODirectionIn, true);
    if (!noAuth.get()) {
      fail("noauth-read-buffer", 0);
    }
    int noAuthResid = 0;
    report->noAuthRead = static_cast<uint32_t>(
        vn_rdwr(kUIORead, devvp, noAuth->getBytesNoCopy(), static_cast<int>(kDeviceReadBytes), 0,
                kUIOSysSpace, kIOUnit | kIOFlagNoAuth, vfs_context_ucred(ctx), &noAuthResid, nullptr));

    // Round135, part two, read through round136's correction: ask the media object this volume actually
    // is, but ask it for no more bytes than the media claims to have. Round135 asked for a fixed 512 and
    // the shipped stack answered kIOReturnBadArgument with nothing moved. In the feed's IOMedia::read()
    // the inactive/open/zero-size arms come first and the four BadArgument arms last; with a non-null
    // In-direction buffer at byte start zero, the only one left is `_mediaSize - byteStart <
    // buffer->getLength()`. So that frame is itself the measurement that _mediaSize < 512 (the host's
    // copy of this container says a ~5.3 GiB quota, so this is a surprise worth drawing rather than
    // deducing), and the 512-byte probe died in geometry without ever reaching the media's provider.
    // dkreadwrite() clips every request to mediaSize before calling this same read, which means the
    // mount's own request would NOT have died on that arm -- it would have reached the provider and taken
    // whatever that layer says. min(ms, 512) makes this probe take exactly that path. ms= itself is drawn
    // before the mount; ml=/md= prove the request this field describes.
    uint32_t requestBytes = 0;
    if (report->mediaSize) {
      requestBytes = report->mediaSize < kBlockBytes
          ? static_cast<uint32_t>(report->mediaSize) : kBlockBytes;
    }
    if (requestBytes) {
      OSSharedPtr<IOBufferMemoryDescriptor> raw =
          IOBufferMemoryDescriptor::withCapacity(requestBytes, kIODirectionIn, true);
      if (!raw.get()) {
        fail("media-read-buffer", requestBytes);
      }
      // The two inputs the BadArgument arms test, read back off the descriptor rather than assumed:
      // bl= is its length (it must equal requestBytes) and md= its direction (kIODirectionIn = 1).
      report->bufferLength = static_cast<uint32_t>(raw->getLength());
      report->bufferDirection = static_cast<uint32_t>(raw->getDirection());
      gVolumeOp.done = 0;
      gVolumeOp.status = static_cast<uint32_t>(kIOReturnNotReady);
      gVolumeOp.actual = 0;
      IOStorageCompletion completion;
      completion.target = &gVolumeOp;
      completion.action = mediaOpDone;
      completion.parameter = nullptr;
      volume->read(client, 0, raw.get(), nullptr, &completion);
      report->mediaRead =
          gVolumeOp.done ? gVolumeOp.status : static_cast<uint32_t>(kIOReturnTimeout);
      report->mediaActual = static_cast<uint32_t>(gVolumeOp.actual);
    }
    // ms=0 is the one size that cannot be read at all; it is drawn by ms= and by ir= staying at
    // kRungNotReached. The feed would have answered UnformattedMedia before any geometry check, so a
    // zero here is a statement about the media rather than about this request.
    // The three public state answers, read after the request so they describe the state the read saw.
    // IOMedia overrides handleIsOpen() to consult its own client dictionary (IOMedia.cpp:486-505), so
    // mo= is "someone has this media open" and mc= is "this driver does" -- and since dkopen() calls
    // media->open() the moment VNOP_OPEN(FREAD) reaches it, mo=0 would mean the FREAD open that bdevvp()
    // performs unconditionally either never reached this minor or was rolled back.
    report->openAny = volume->isOpen(nullptr) ? 1u : 0u;
    report->openUs = volume->isOpen(client) ? 1u : 0u;
    report->writable = volume->isWritable() ? 1u : 0u;

    // Round136, part three: follow the dev_t instead of the object. dv= came from this volume's registry
    // properties, but a BSD name is a claim the client makes, and the read that actually refuses the
    // mount is dkreadwrite() on the minor that number resolves to. Walk the same disk subtree the APFS
    // scan walks, take the media whose BSD Major/Minor equal dv=, and record whether it is this pointer.
    // If it is not, xs=/xo=/xw= describe the object the mount really reads, and xr= is a byte-zero read
    // of that object with the same min(size, 512) rule. If it is the same object, ir=/ia= above already
    // is that read and xr= stays at kRungNotReached rather than issuing a duplicate.
    IOMedia *minorMedia = findMediaByMinor(whole, report->major, report->minor, 0);
    if (minorMedia) {
      report->minorFound = 1;
      report->minorSame = minorMedia == volume ? 1u : 0u;
      report->minorSize = minorMedia->getSize();
      report->minorOpen = minorMedia->isOpen(nullptr) ? 1u : 0u;
      report->minorWritable = minorMedia->isWritable() ? 1u : 0u;
      if (!report->minorSame && report->minorSize) {
        uint32_t length = report->minorSize < kBlockBytes
            ? static_cast<uint32_t>(report->minorSize) : kBlockBytes;
        OSSharedPtr<IOBufferMemoryDescriptor> other =
            IOBufferMemoryDescriptor::withCapacity(length, kIODirectionIn, true);
        if (!other.get()) {
          fail("minor-read-buffer", length);
        }
        gMinorOp.done = 0;
        gMinorOp.status = static_cast<uint32_t>(kIOReturnNotReady);
        gMinorOp.actual = 0;
        IOStorageCompletion minorCompletion;
        minorCompletion.target = &gMinorOp;
        minorCompletion.action = mediaOpDone;
        minorCompletion.parameter = nullptr;
        minorMedia->read(client, 0, other.get(), nullptr, &minorCompletion);
        report->minorRead =
            gMinorOp.done ? gMinorOp.status : static_cast<uint32_t>(kIOReturnTimeout);
        report->minorActual = static_cast<uint32_t>(gMinorOp.actual);
      }
    }
    // The whole media is what the GPT and the APFS scan were read through, so its open state is the
    // fallback reading if no media carries the minor: a whole disk that is still open cannot be the
    // source of a NotOpen, one that is not open can.
    report->wholeOpen = whole->isOpen(nullptr) ? 1u : 0u;
    return;
  }
  // vfs_subr.c:1282, through the exported helper rather than a third measured offset:
  // v_specflags is v_specinfo->si_flags (bsd/miscfs/specfs/specdev.h:100), which is two layouts deep.
  vnode_setmountedon(devvp);
  // The two bookkeeping calls the boot root makes the moment its mount returns 0. Both matter for
  // what happens after this line: vfs_rootmountalloc_internal() busy()s the mount (vfs_subr.c:1146)
  // and only vfs_unbusy() drops that shared hold, and a mount that is not on the mountlist is
  // invisible to vfs_work_thread(), which is what works a filesystem's mnt_workerqueue -- the queue
  // apfs puts its deferred volume teardown on. mount_list_add() reads mp->mnt_list, but that offset
  // lives inside the kernel, so the opaque struct costs nothing here. Its -1 (system shutting down)
  // is ignored exactly as vfs_mountroot() ignores it.
  vfs_unbusy(mp);
  mount_list_add(mp);
  report->start = static_cast<uint32_t>(VFS_START(mp, 0, ctx));

  void *dir = nullptr;
  report->root = static_cast<uint32_t>(VFS_ROOT(mp, &dir, ctx));
  if (report->root || !dir) {
    return;
  }

  // One component per call, relative, from the level above: on a board whose only report is a
  // photograph, a four-level path has to name the level it stopped at. vnode_lookupat() hands back
  // the found vnode with an iocount and leaves the caller's start_dvp alone -- lookup() takes its own
  // reference with vnode_getwithref (bsd/vfs/vfs_lookup.c:1125) precisely so the caller keeps
  // ownership -- so this vnode_put() is the only thing that drops the level we came from, and the
  // last reference dropped is the file's.
  for (uint32_t index = 0; index < 4; index++) {
    void *next = nullptr;
    report->lookup[index] = static_cast<uint32_t>(
        vnode_lookupat(kVolumePath[index], 0, &next, ctx, dir));
    vnode_put(dir);
    if (report->lookup[index] || !next) {
      return;
    }
    dir = next;
  }

  OSSharedPtr<IOBufferMemoryDescriptor> file =
      IOBufferMemoryDescriptor::withCapacity(kFileBytes, kIODirectionIn, true);
  if (!file.get()) {
    fail("file-buffer", 0);
  }
  int resid = 0;
  report->read = static_cast<uint32_t>(vn_rdwr(kUIORead, dir, file->getBytesNoCopy(),
                                               static_cast<int>(kFileBytes), 0, kUIOSysSpace,
                                               kIOUnit, vfs_context_ucred(ctx), &resid, nullptr));
  if (resid < 0) {
    resid = 0;
  }
  report->bytes = kFileBytes - static_cast<uint32_t>(resid);
  const uint8_t *head = static_cast<const uint8_t *>(file->getBytesNoCopy());
  if (head) {
    report->word0 = le32At(head);
    report->word1 = le32At(head + 4);
  }
  vnode_put(dir);
}

// Round 137 stops asking and starts booting. Whatever produced the 13 that this file's own VFS_MOUNT
// kept getting, vfs_mountroot() does not call that rung for apfs: it calls the filesystem's
// vfc_mountroot when the vfstable registers one (bsd/vfs/vfs_subr.c:1247-1255), and the root it hands
// over is the IOMedia whose UUID /chosen's root-matching names. So this function mounts nothing. It
// loads apfs.kext, waits for the volume with that UUID, waits until IOMediaBSDClient has named it,
// latches its dev_t, prints which volume it is, and returns; IOFindBSDRoot's wait and vfs_mountroot()
// run immediately after this handler returns, still inside the same bsd_autoconf.
//
// Round143 selects by UUID instead of taking whichever volume the depth walk preferred: the owner
// mount at exec re-checks the root device against native_verified_block_root_authorized(), so the
// number latched here has to be the number the root search will mount, not merely a volume in the
// same container.
static void prepareApfsBoot(IOMedia *whole)
{
  kern_return_t loaded = OSKextLoadKextWithIdentifier(kApfsKext);
  if (loaded) {
    fail("apfsload", static_cast<uint32_t>(loaded));
  }
  IOMedia *volume = nullptr;
  uint32_t passes = 0;
  for (; passes < 12000 && !volume; passes++) {
    volume = findVolumeByUuid(whole, kRootVolumeUuid, 0);
    if (!volume) {
      IOSleep(10);
    }
  }
  if (!volume) {
    fail("apfsvol", passes);
  }
  // The UUID is what root-matching matches, and the BSD name/major/minor are what IOFindBSDRoot turns
  // into rootdev. They are published by different clients after the media exists, so returning as soon
  // as the node appears would race the naming; the wait is for the pair.
  const char *devname = nullptr;
  uint32_t major = 0xffffffff;
  uint32_t minor = 0xffffffff;
  bool named = false;
  for (uint32_t attempt = 0; attempt < 1000; attempt++) {
    if (volumeBsd(volume, &devname, &major, &minor)) {
      named = true;
      break;
    }
    IOSleep(10);
  }
  if (named) {
    gOsVolumeDev = volumeDev(major, minor);
  }
  IOLog("CloverBlock r145: os vu=%s bsd=%s %u:%u p=%u\n", kRootVolumeUuid,
        devname ? devname : "?", major, minor, passes);
  volume->release();
  if (!named) {
    fail("apfsbsd", passes);
  }
}

// The second window's half, at sheng's moment. SystemOS is a sealed cryptex volume, so nothing here
// mounts it: the owner mount in bsd/kern/kern_exec.c does that at launchd's exec with
// MNT_RDONLY|MNT_UNION on "/", after asking native_verified_block_volume_device() for the device
// named by this UUID. On sheng that lookup is answered by walking IOMedia of the one disk published
// at boot, so no board code runs before the root is mounted. clover's image keeps SystemOS in a
// second window of userdata rather than in a partition of the boot disk, so the window is still this
// file's to publish -- but at the same moment sheng looks the volume up (exec, after bsd_init has
// mounted the root), not from clover_block_run(). Round143 published it at boot and both boots after
// that left bsd_init() with an empty mountlist; rounds 137-142, which published only the OS window,
// mounted the same root. The media is retained because the owner mount reads it long after this run.
static IOMedia *publishWindow(uint32_t first, uint32_t sectors, bool writeProtected,
                              CloverBlockDevice **clientOut);

static IOMedia *publishSystemOs(void)
{
  if (gSystemVolumeMedia) {
    return gSystemVolumeMedia;
  }
  const uint32_t first = __atomic_load_n(&gSystemWindowFirst, __ATOMIC_ACQUIRE);
  const uint32_t sectors = __atomic_load_n(&gSystemWindowSectors, __ATOMIC_ACQUIRE);
  if (!first || !sectors) {
    return nullptr;
  }
  CloverBlockDevice *device = nullptr;
  IOMedia *whole = publishWindow(first, sectors, true, &device);
  IOMedia *volume = nullptr;
  uint32_t passes = 0;
  for (; passes < 12000 && !volume; passes++) {
    volume = findVolumeByUuid(whole, kSystemOsVolumeUuid, 0);
    if (!volume) {
      IOSleep(10);
    }
  }
  if (!volume) {
    fail("systemvol", passes);
  }
  const char *devname = nullptr;
  uint32_t major = 0xffffffff;
  uint32_t minor = 0xffffffff;
  bool named = false;
  for (uint32_t attempt = 0; attempt < 1000; attempt++) {
    if (volumeBsd(volume, &devname, &major, &minor)) {
      named = true;
      break;
    }
    IOSleep(10);
  }
  IOLog("CloverBlock r145: system vu=%s bsd=%s %u:%u p=%u\n", kSystemOsVolumeUuid,
        devname ? devname : "?", major, minor, passes);
  if (!named) {
    volume->release();
    fail("systembsd", passes);
  }
  gSystemVolumeDev = volumeDev(major, minor);
  gSystemVolumeMedia = volume;
  return volume;
}

// Round159's third window, and the only writable one the boot publishes. The owner mount's Data step
// asks native_ram_block_volume_seed() at the same exec as the SystemOS lookup; on sheng that answer
// is a RAM disk, here it is a window on the card, so this is where the window is attached and the
// container's own volume is named. The media is retained because the read-write mount outlives this
// call, and because the Data window's blocks are the only ones the running system may write.
static IOMedia *publishDataVolume(void)
{
  if (gDataVolumeMedia) {
    return gDataVolumeMedia;
  }
  const uint32_t first = __atomic_load_n(&gDataWindowFirst, __ATOMIC_ACQUIRE);
  const uint32_t sectors = __atomic_load_n(&gDataWindowSectors, __ATOMIC_ACQUIRE);
  if (!first || !sectors) {
    return nullptr;
  }
  CloverBlockDevice *device = nullptr;
  IOMedia *whole = publishWindow(first, sectors, false, &device);
  IOMedia *volume = nullptr;
  uint32_t passes = 0;
  for (; passes < 12000 && !volume; passes++) {
    volume = findVolumeByUuid(whole, kDataVolumeUuid, 0);
    if (!volume) {
      IOSleep(10);
    }
  }
  if (!volume) {
    fail("datavol", passes);
  }
  const char *devname = nullptr;
  uint32_t major = 0xffffffff;
  uint32_t minor = 0xffffffff;
  bool named = false;
  for (uint32_t attempt = 0; attempt < 1000; attempt++) {
    if (volumeBsd(volume, &devname, &major, &minor)) {
      named = true;
      break;
    }
    IOSleep(10);
  }
  IOLog("CloverBlock r159: data vu=%s bsd=%s %u:%u p=%u\n", kDataVolumeUuid,
        devname ? devname : "?", major, minor, passes);
  if (!named) {
    volume->release();
    fail("databsd", passes);
  }
  gDataVolumeDev = volumeDev(major, minor);
  gDataVolumeMedia = volume;
  return volume;
}

// The whole of rounds 127 and 128, and it never returns: the console is the thing this board cannot
// trust this late, so the depth is drawn on the panic screen like every other reading since round121.
__attribute__((noreturn)) static void apfsProbe(IOMedia *whole, IOService *client)
{
  // 1. Whether the bytes the window claims are the bytes that are there, seen through IOMedia rather
  // than through this port's own PIO. Under round127 that question was NXSB 32 bytes into block zero,
  // because the window was a bare container. Under round128 the window is a real GPT disk, so the
  // question is the partition header one block in: `m=1` means the flashed image landed where the
  // manifest says and read back with the magic a GPT has, and nothing below this line is about bytes.
  OSSharedPtr<IOBufferMemoryDescriptor> first =
      IOBufferMemoryDescriptor::withCapacity(kBlockBytes, kIODirectionIn, true);
  if (!first.get()) {
    fail("buffer", 0);
  }
  if (!whole->open(client, 0, kIOStorageAccessReader)) {
    fail("open", 0);
  }
  IOReturn status = readMediaBytes(whole, client, first.get(), kGptHeaderOffset, &gMediaOp);
  whole->close(client);
  if (status != kIOReturnSuccess) {
    fail("first-read", static_cast<uint32_t>(status));
  }
  const uint8_t *bytes = static_cast<const uint8_t *>(first->getBytesNoCopy());
  uint32_t magic = bytes && memcmp(bytes, kEfiPartMagic, sizeof(kEfiPartMagic)) == 0 ? 1u : 0u;

  // 2. The media has to be free before anyone above it can claim it: the container scheme opens its
  // provider, and a probe that still held the read open would be measuring its own lease. The close
  // above is the last thing this file does to the media; everything below is registry traffic.
  //
  // apfs.kext is in the boot kext collection, not the linked kernel, exactly like IOStorageFamily --
  // and nothing in the boot asks for a filesystem on a disk that only just appeared. Loading it is
  // also what schedules matching for its personalities, which is why this happens before the wait.
  //
  // What is *not* done here is slicing the disk. IOBlockStorageDriver registers the whole media with
  // kIOServiceAsynchronous, and round116 read the consequence: IOKit's own configuration threads
  // matched IOGUIDPartitionScheme and IOMediaBSDClient onto it and named every partition before a
  // hand-built tree could. A window that carries a partition table lets that pass do the work, so the
  // APFS slice this round measures is Apple's scan of Apple's bytes, not this file's.
  kern_return_t loaded = OSKextLoadKextWithIdentifier(kApfsKext);

  // 3. Poll, do not block on a semaphore: the attach this is waiting for is performed by the same
  // configuration machinery that is running this handler, so the wait has to keep handing control
  // back. Step 5 of the partition path has been doing exactly this since round116. The budget is four
  // times round127's because a real container is read, not just recognised: at the pace the PIO path
  // moves (round127's 56 blocks in the 295 ticks this board's own counter calls about one second) a
  // few thousand blocks is tens of seconds, and the panic that follows has to be the probe's answer
  // rather than the watchdog's. board.toml raises armcpu-clock-panic to match. The counters start now,
  // so what they report is the storage stack's traffic and not the one read taken above.
  __atomic_store_n(&gBlocksRead, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&gBlocksWritten, 0, __ATOMIC_RELEASE);
  ApfsDepth best{};
  for (uint32_t attempt = 0; attempt < 12000 && !best.volumeMedia; attempt++) {
    ApfsDepth now{};
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
    // The counts are maxima rather than sums: a pass that ran before the container finished publishing
    // sees fewer volumes than the one after it, and the frame wants the fullest picture the tree ever
    // gave, not the number of times this file walked it.
    if (now.volumeCount > best.volumeCount) {
      best.volumeCount = now.volumeCount;
    }
    if (now.unsealedCount > best.unsealedCount) {
      best.unsealedCount = now.unsealedCount;
    }
    if (!best.containerUuid) {
      best.containerUuid = now.containerUuid;
    }
    // The walk retained whatever volume it chose, and the loop ends on the first pass that comes back
    // with a media, so the transfer below has no second owner; the release is for the one case where a
    // later pass finds a volume media and an earlier pass already had one -- which the loop condition
    // makes impossible, and an unbalanced retain on a boot-lifetime object would be.
    if (now.volumeMedia) {
      if (best.volumeMedia) {
        now.volumeMedia->release();
      } else {
        best.volumeMedia = now.volumeMedia;
        best.identity = now.identity;
      }
    }
    IOSleep(10);
  }
  // Nothing was ever chosen: the identity fields stay 0, but the two string fields that would read as a
  // zero byte say "absent" instead, so an empty frame cannot be misread as a volume whose Sealed is "".
  if (!best.volumeMedia) {
    best.identity.sealed = kNoProperty;
    best.identity.status = kNoProperty;
  }

  // 4. Round129's question, asked of the object the walk above was only counting: can the kernel mount
  // the volume Apple's own stack published on top of this PIO-backed disk, and can it read a file out
  // of it. Every field of the answer is on the same line as the depth that made it possible, so one
  // photograph says both whether the tree grew and what the vfs made of it.
  //
  // Round132 asks the half of that this file has never been able to answer: *which* volume the mount was
  // aimed at. Rounds 129-131 each changed one field of the mount struct and each got the same 13 back with
  // the same 464 blocks read -- three guesses about the caller's state, and not one word about the object
  // being mounted. The host has meanwhile read the flashed bytes (the window is the OS slice of the same
  // image the base repository mounts with return 0) and found exactly one volume in it, the unsealed one,
  // so the guess that survived round131 -- "we have been mounting the sealed volume" -- is already wrong
  // before this boot. What is still unproven is the device side of that claim: that the volume *this*
  // board's IOKit published, the one whose BSD name went into vfs_rootmountalloc() and whose devvp went
  // into the mount, carries the same name and the same seal. vt=/un= say whether there was ever more than
  // one to choose from, vs=/vo=/vu=/vc=/vn= name the one that was chosen, and the mount ladder below runs
  // unchanged -- same flags, same devvp, same data=0 -- so a 13 next to an expected identity is a 13 about
  // the right volume, and not an aim that never left the sealed one.
  MountReport report = {
    kRungNotReached, kRungNotReached, kRungNotReached, kRungNotReached,  // major minor bdev alloc
    { 0, 0, 0, 0 },                                                     // name
    0, 0, 0,                                                            // on from devvp
    0, 0, 0, 0, 0,                                                      // flag cleared kernFlag ops mountFn
    kRungNotReached, kRungNotReached, kRungNotReached,                  // mount start root
    { kRungNotReached, kRungNotReached, kRungNotReached, kRungNotReached },  // lookup
    kRungNotReached, 0, 0, 0,                                           // read bytes word0 word1
    kRungNotReached, 0, 0, 0,                                           // deviceRead .. word1
    kRungNotReached, kRungNotReached, 0,                                // noAuthRead mediaRead mediaActual
    kRungNotReached, kRungNotReached, kRungNotReached,                  // openAny openUs writable
    0, 0, 0,                                                            // mediaSize preferredBlock mediaBase
    0, 0,                                                               // bufferLength bufferDirection
    kRungNotReached, kRungNotReached, 0,                                // minorFound minorSame minorSize
    kRungNotReached, kRungNotReached, kRungNotReached, kRungNotReached, kRungNotReached,
  };                                                                    // minorOpen minorWritable minorRead minorActual wholeOpen
  if (best.volumeMedia) {
    volumeMountLadder(whole, best.volumeMedia, client, &report);
  }

  panic("CloverBlock r137: apfs f=%u n=%u m=%u l=%u d=%u s=%u c=%u v=%u "
        "vt=%u un=%u vs=0x%x vo=0x%x vu=0x%x vc=0x%x vn=0x%x/0x%x/0x%x/0x%x dv=%u:%u bd=%u al=%u "
        "t=0x%x/0x%x/0x%x/0x%x on=0x%x fm=0x%x dp=0x%x:0x%x fl=0x%x fc=0x%x fk=0x%x "
        "op=0x%x:0x%x mm=0x%x:0x%x mt=%u st=%u rt=%u k=%u/%u/%u/%u "
        "rd=%u g=%u x=0x%x y=0x%x dr=%u db=%u dx=0x%x dy=0x%x "
        "na=%u ir=0x%x ia=%u mo=%u mc=%u mw=%u "
        "ms=0x%x:0x%x bs=%u mb=0x%x:0x%x ml=%u md=0x%x "
        "xm=%u xe=%u xs=0x%x:0x%x xo=%u xw=%u xr=0x%x xa=%u wo=%u r=%u w=%u",
        __atomic_load_n(&gWindowFirst, __ATOMIC_ACQUIRE),
        __atomic_load_n(&gWindowSectors, __ATOMIC_ACQUIRE), magic,
        static_cast<uint32_t>(loaded), best.seen, best.scheme, best.container, best.volume,
        best.volumeCount, best.unsealedCount, best.identity.sealed, best.identity.status,
        best.identity.uuid, best.containerUuid,
        best.identity.name[0], best.identity.name[1], best.identity.name[2], best.identity.name[3],
        report.major, report.minor, report.bdev, report.alloc,
        report.name[0], report.name[1], report.name[2], report.name[3], report.on, report.from,
        static_cast<uint32_t>(report.devvp >> 32), static_cast<uint32_t>(report.devvp),
        report.flag, report.cleared, report.kernFlag,
        static_cast<uint32_t>(report.ops >> 32), static_cast<uint32_t>(report.ops),
        static_cast<uint32_t>(report.mountFn >> 32), static_cast<uint32_t>(report.mountFn),
        report.mount, report.start,
        report.root, report.lookup[0], report.lookup[1], report.lookup[2], report.lookup[3],
        report.read, report.bytes, report.word0, report.word1,
        report.deviceRead, report.deviceBytes, report.deviceWord0, report.deviceWord1,
        report.noAuthRead, report.mediaRead, report.mediaActual,
        report.openAny, report.openUs, report.writable,
        static_cast<uint32_t>(report.mediaSize >> 32), static_cast<uint32_t>(report.mediaSize),
        report.preferredBlock,
        static_cast<uint32_t>(report.mediaBase >> 32), static_cast<uint32_t>(report.mediaBase),
        report.bufferLength, report.bufferDirection,
        report.minorFound, report.minorSame,
        static_cast<uint32_t>(report.minorSize >> 32), static_cast<uint32_t>(report.minorSize),
        report.minorOpen, report.minorWritable, report.minorRead, report.minorActual,
        report.wholeOpen,
        __atomic_load_n(&gBlocksRead, __ATOMIC_ACQUIRE),
        __atomic_load_n(&gBlocksWritten, __ATOMIC_ACQUIRE));
}

// Publish one extent of the card as an IOBlockStorageDevice and start the one IOBlockStorageDriver
// it is for. The window is registered before attach because start() is where the stack asks for the
// media's geometry; the size check is the same proof round127 wrote -- the media the driver built has
// to describe exactly the sectors this window claims.
static IOMedia *publishWindow(uint32_t first, uint32_t sectors, bool writeProtected,
                              CloverBlockDevice **clientOut)
{
  CloverBlockDevice *device = new CloverBlockDevice;
  IOPlatformExpert *platform = IOService::getPlatform();
  if (!device || !device->init(nullptr)) {
    fail("device", 0);
  }
  registerWindow(device, first, sectors, writeProtected);
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
  const uint32_t expected = sectors ? sectors : clover_emmc_block_sectors();
  if (!whole || whole->getSize() / kBlockBytes != expected) {
    fail("media", 0);
  }
  *clientOut = device;
  return whole;
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
    IOLog("CloverBlock r137: refire n=%u (already ran)\n", fires);
    return;
  }

  // 1. The PIO path has to be proven before anything claims a device over it.
  if (!clover_emmc_block_ready() || clover_emmc_block_sectors() == 0) {
    fail("ready", 0);
  }

  // 1b. Which geometry to describe. `clover-apfs=1` asks for logfs's own window, and the window only
  // exists if this boot's card reported GPT entry 39 as exactly the extent drivers/CloverSdhci reads.
  // `clover-apfs=2` asks for the root image instead: entry 64's window is read for its SHNGROOT
  // manifest, and the media becomes the OS disk the manifest points at. A media that claimed a
  // partition the partition table does not describe would be a second card, and every filesystem above
  // it would be reading the wrong bytes. Latched here, before the device is published, because
  // reportMaxValidBlock() is what IOBlockStorageDriver builds the media from and the size has to be
  // right the first time anything reads it.
  uint32_t apfs = 0;
  if (PE_parse_boot_argn("clover-apfs", &apfs, sizeof(apfs)) && apfs) {
    if (apfs == 2 || apfs == 3) {
      uint32_t first = 0;
      uint32_t sectors = 0;
      clover_emmc_block_userdata_window(&first, &sectors);
      if (!first || !sectors) {
        fail("udwindow", sectors);
      }
      // Only the boot mode needs the other windows; the probe modes read the OS entry alone.
      windowsFromManifest(first, sectors, apfs == 3, apfs == 3);
    } else {
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

  // 4. Publish the device(s) this mode asks for. `clover-apfs=3` is the boot and publishes two
  // windows, strictly in order: the OS window, then -- only after its volume exists and
  // IOMediaBSDClient has named it -- the SystemOS window. drivers/CloverSdhci serves one caller at a
  // time through a two-second hand-over, so two APFS attaches running at once would make the loser
  // fail a read. Both boot windows are read-only; the logfs and probe modes keep the writable device
  // Stage 14 writes through.
  const uint32_t osFirst = __atomic_load_n(&gWindowFirst, __ATOMIC_ACQUIRE);
  const uint32_t osSectors = __atomic_load_n(&gWindowSectors, __ATOMIC_ACQUIRE);
  CloverBlockDevice *device = nullptr;
  IOMedia *whole = nullptr;
  if (apfs == 3) {
    const uint32_t systemFirst = __atomic_load_n(&gSystemWindowFirst, __ATOMIC_ACQUIRE);
    const uint32_t systemSectors = __atomic_load_n(&gSystemWindowSectors, __ATOMIC_ACQUIRE);
    const uint32_t dataFirst = __atomic_load_n(&gDataWindowFirst, __ATOMIC_ACQUIRE);
    const uint32_t dataSectors = __atomic_load_n(&gDataWindowSectors, __ATOMIC_ACQUIRE);
    if (!osSectors || !systemSectors || !dataSectors) {
      fail(!osSectors ? "rootwin" : !systemSectors ? "systemwin" : "datawin", 0);
    }
    // Round145: sheng's boot order. One disk is published and only the root volume is named before
    // bsd_init() mounts it; the SystemOS volume is left to native_verified_block_volume_device(),
    // which the owner mount calls at launchd's exec -- that is when sheng's registry walk answers,
    // and it is when this board publishes the second window. Round143 moved that publication here,
    // ahead of the root mount, and r143/r144 both ended in bsd_init() with an empty mountlist;
    // r137-r142 published this one window and mounted the same root. The window's geometry is still
    // read and bounds-checked here, so a manifest without SystemOS fails before the root mount.
    //
    // Round159's Data window is not published here either: the owner mount asks for its seed at the
    // same exec, through native_ram_block_volume_seed(), and that call is what attaches it. Only its
    // geometry is proven above, because a manifest without Data has to stop before the root mount
    // rather than mount a root whose /private/var can never be seeded.
    whole = publishWindow(osFirst, osSectors, true, &device);
    prepareApfsBoot(whole);
    IOLog("CloverBlock r159: boot os=%u+%u system=%u+%u data=%u+%u fires=%u deferred=1\n",
          osFirst, osSectors, systemFirst, systemSectors, dataFirst, dataSectors, fires);
    return;
  }
  whole = publishWindow(osFirst, osSectors, false, &device);

  // 4b. Under a window this is where the run ends, and deliberately so: steps 5-9 below are the
  // partition-named root's measurements, and what a window is asking instead is whether Apple's own
  // filesystem lives on top of this tree. Round127's window was a bare container, so it had no
  // partition scheme, no disk0s39 and nothing for those steps to measure; round128's does carry a GPT,
  // and the automatic pass will slice it, but the question stays the same one level up.
  //
  // Round137 split the two jobs the window was doing. `clover-apfs=2` keeps the probe (and its
  // never-returning panic); `clover-apfs=3` is the boot, and it returned above so the kernel's own
  // root search and mount run with the standard apfs mountroot instead of this file's.
  if (osSectors) {
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

  IOLog("CloverBlock r137: auto p=%u %s>%s %u:%u bd=1 map=1 fires=%u\n", partitions, foundName,
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


// ---------------------------------------------------------------- the owner mount's contract

// bsd/kern/kern_exec.c's offline owner mount -- sheng's SystemOS step, enabled on this board by
// patches/xnu-clover/zzy-clover-systemos-mount.patch -- validates the block stack through these
// symbols. drivers/NativeVirtioBlock supplies them on sheng by walking the IOMedia the boot disk
// published; this board supplies the same answers from the SHNGROOT manifest it read off userdata.
// Round145 keeps sheng's timing: the OS volume is the one root-matching handed the kernel, and the
// SystemOS volume is published only when this lookup runs, at launchd's exec. Round159 adds the two
// symbols the authenticated boot manifest and the scratch Data mount ask for, and both answer from
// the same third window: Data is published read-write when its seed is requested, never before.
extern "C" int native_verified_block_root_authorized(dev_t device)
{
  if (device == kNoDevice) {
    return 0;
  }
  // Data is deliberately absent: it is mounted read-write at /private/var and is never a root,
  // never a shared-cache cache, and never the SystemOS volume the owner mount unions onto "/".
  return device == gOsVolumeDev || device == gSystemVolumeDev;
}

static void le64Store(uint8_t *at, uint64_t value)
{
  for (unsigned shift = 0; shift < 64; shift += 8) {
    *at++ = static_cast<uint8_t>(value >> shift);
  }
}

// The digest a boot manifest records for one window. sheng hashes the authenticated envelope of its
// own block format; clover has no per-block envelope, so the identity hashed here is exactly the
// claim the flashed SHNGROOT manifest made about this window: its role, the UUID the kernel matched,
// the byte extent the driver read, and the seed digest the image was built with. Everything comes
// from this boot's own manifest read, so the number changes when any of those change.
extern "C" int native_verified_block_manifest_sha384(dev_t device, const char *uuid,
                                                     uint8_t digest[48])
{
  if (!digest) {
    return 0;
  }
  memset(digest, 0, 48);
  if (!uuid || strlen(uuid) != 36) {
    return 0;
  }
  const bool os = device != kNoDevice && device == gOsVolumeDev &&
                  strcmp(uuid, kRootVolumeUuid) == 0;
  const bool system = device != kNoDevice && device == gSystemVolumeDev &&
                      strcmp(uuid, kSystemOsVolumeUuid) == 0;
  if (!os && !system) {
    return 0;
  }
  // Pair with the release store in windowsFromManifest before reading the entry it latched.
  (void)__atomic_load_n(os ? &gWindowSectors : &gSystemWindowSectors, __ATOMIC_ACQUIRE);
  const ManifestEntry *entry = os ? &gOsEntry : &gSystemEntry;
  static const char domain[] = "VerifiedRoot.CloverBlockWindow.v1";
  uint8_t role = os ? 1 : 2;
  uint8_t extent[16];
  le64Store(extent, entry->offset);
  le64Store(extent + 8, entry->size);
  SHA384_CTX context;
  SHA384_Init(&context);
  SHA384_Update(&context, domain, sizeof(domain));
  SHA384_Update(&context, &role, sizeof(role));
  SHA384_Update(&context, uuid, 36);
  SHA384_Update(&context, extent, sizeof(extent));
  SHA384_Update(&context, entry->sha256, sizeof(entry->sha256));
  SHA384_Final(digest, &context);
  IOLog("CloverBlock r159: manifest %s vu=%s off=%llu size=%llu\n", os ? "os" : "system", uuid,
        static_cast<unsigned long long>(entry->offset),
        static_cast<unsigned long long>(entry->size));
  return 1;
}

// kern_exec.c compiles its GPU-addon step under the same switch that turns the Data mount on, and
// that step references native_ram_block_gpu_addon() as a weak import. A static kernel link resolves
// every undefined reference, so the weak attribute does not let this symbol stay absent -- the link
// asks CloverBlock for it exactly as it asks for the seed below. The step itself returns at once
// unless the boot carries `sheng-gpu-addon=1`, which this board never passes, and even if it ran the
// answer here is "no addon": the caller returns before touching the Data image, and /private/var
// stays exactly the seed's bytes.
extern "C" int native_ram_block_gpu_addon(const uint8_t **addon, uint64_t *bytes)
{
  if (addon) {
    *addon = nullptr;
  }
  if (bytes) {
    *bytes = 0;
  }
  return 0;
}

// The seed of the read-write Data volume. On sheng this answer comes from NativeRamBlock's memory
// disk; the owner Data step in bsd/kern/kern_exec.c does not care which backend answered, only that
// the device, path, byte count and seed digest name the same volume it is about to mount. Here the
// backend is the third SHNGROOT window, so publishing it is what answers the question, and the
// digest is the manifest's own record of the 32 MiB image that was appended to userdata.
extern "C" int native_ram_block_volume_seed(const char *uuid, dev_t *device, char *path,
                                            size_t size, uint64_t *base, uint64_t *bytes,
                                            uint8_t *digest)
{
  if (!uuid || !device || !path || size < 6 || !base || !bytes || !digest ||
      strcmp(uuid, kDataVolumeUuid) != 0) {
    return 0;
  }
  *device = kNoDevice;
  path[0] = 0;
  IOMedia *media = publishDataVolume();
  if (!media) {
    return 0;
  }
  const char *name = nullptr;
  uint32_t major = 0xffffffff;
  uint32_t minor = 0xffffffff;
  if (!volumeBsd(media, &name, &major, &minor)) {
    return 0;
  }
  const dev_t candidate = volumeDev(major, minor);
  if (candidate == kNoDevice || candidate != gDataVolumeDev) {
    return 0;
  }
  const uint32_t sectors = __atomic_load_n(&gDataWindowSectors, __ATOMIC_ACQUIRE);
  if (!sectors) {
    return 0;
  }
  // A window's seed is addressed by the block device, not by a kernel virtual range, so the logical
  // base is zero and the length is the window's own byte count. The Data mount checks both are
  // page-aligned and refuses a zero length.
  *base = 0;
  *bytes = static_cast<uint64_t>(sectors) * kBlockBytes;
  memcpy(digest, gDataEntry.sha256, sizeof(gDataEntry.sha256));
  snprintf(path, size, "/dev/%s", name);
  *device = candidate;
  IOLog("CloverBlock r159: seed data %s dev=%d bytes=%llu off=%llu\n", name,
        static_cast<int>(candidate), static_cast<unsigned long long>(*bytes),
        static_cast<unsigned long long>(gDataEntry.offset));
  return 1;
}

// The owner mount asks for the SystemOS volume by UUID, then opens the returned /dev path. Both have
// to name the same volume this boot found: the dev_t is the media's own BSD major/minor, and the path
// is the devfs node IOMediaBSDClient published for it -- `/dev/<bsd name>`, not a guess. On sheng the
// volume is already attached and this is a registry lookup; here the same lookup first has to publish
// the window the volume lives in, which is why it happens now and not in clover_block_run().
extern "C" int native_verified_block_volume_device(const char *uuid, dev_t *device, char *path,
                                                   size_t size)
{
  if (!uuid || !device || !path || !size || strcmp(uuid, kSystemOsVolumeUuid) != 0) {
    return 0;
  }
  IOMedia *media = publishSystemOs();
  if (!media) {
    return 0;
  }
  const char *name = nullptr;
  uint32_t major = 0xffffffff;
  uint32_t minor = 0xffffffff;
  if (!volumeBsd(media, &name, &major, &minor)) {
    return 0;
  }
  const dev_t candidate = volumeDev(major, minor);
  if (candidate != gSystemVolumeDev || !native_verified_block_root_authorized(candidate)) {
    return 0;
  }
  IOLog("CloverBlock r145: owner system %s dev=%d\n", name, static_cast<int>(candidate));
  snprintf(path, size, "/dev/%s", name);
  *device = candidate;
  return 1;
}
