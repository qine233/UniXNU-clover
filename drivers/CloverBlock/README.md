# CloverBlock — handing the card to XNU's own storage stack, one contract at a time

Round107 started this file with a rule that still holds: the port's private PIO report is not a storage
stack, so the question "can this board be read by iOS" has to be answered through IOKit's own classes.
The source's opening comment records the shape of the answer — publish an `IOBlockStorageDevice`, let
`IOStorageFamily` and `IOGUIDPartitionScheme` do what the kernel does on a real device, and let the
kernel name the partition the way `setconf()`/`vfs_mountroot()` would. The boot itself still runs from
whatever `rd=` names; "this round only proves the device side" is that comment's own closing sentence.

## Where it starts, and why not from the PMGR hook

`clover_block_request()` installs a `resourceMatching("IOBSD")` publish notification and
`clover_block_run()` runs from its handler, `cloverBlockIOBSD()`, latched by the `gCloverBlockRan`
compare-and-swap in step 0 of `clover_block_run()` so a re-fired notification cannot build a second
tree — the refire count it prints (`CloverBlock r137: refire`) is the proof the notification really
does fire again. The driver is asked for by `drivers/CloverSdhci` in the `mode == 21` branch of
`clover_emmc_run()`, whose comment says mode 21 is the boot path now, not a report.

That position is measured, not chosen for taste. Round113 ran the whole thing from `sheng_pmgr_run()`
and stopped at `bsd-props`: that hook is inside `bsd_init()` *before* `vfsinit()`, so `devfs_ready` is
still 0, `devfs_make_node()` returns NULL, `MinorTable::insert()` fails, and
`IOMediaBSDClient::start()` returns true anyway because it ignores `createNodes()`. The IOBSD resource
is published by `IOKitBSDInit()` at the end of `bsd_autoconf()` — after `vfsinit()`, before `setconf()` —
and that is the only post-`vfsinit` hook this repository owns; the whole argument is the
`the post-vfsinit hand-off` block above `cloverBlockIOBSD()`.

## The class, and the one documented rule this file cannot follow

`class CloverBlockDevice : public IOBlockStorageDevice` is the closest any clover driver comes
to the class shape of `docs/drivers/xnu-drivers.md:19-64`, and it still cannot use the standard macros.
The reason is a real constraint, written out in the source's own header paragraph:
`com.apple.iokit.IOStorageFamily` is a prelinked component kept from the original kernelcache
(`board.toml:105`), not a set of symbols the native kernel exports — `kernel.macho` exports no
`IOBlockStorageDevice`, `IOMedia` or `IOBlockStorageDriver` symbol — so `OSDefineMetaClass*` would name
a superclass metaclass address at link time. The metaclass is therefore hand-written
(`CloverBlockDevice::MetaClass`, with `superClass`, `gMetaClass` and its `alloc()`) with the superclass
pointer filled in after `OSKextLoadKextWithIdentifier`, every class-owned virtual slot implemented here,
and a `static_assert` pinning this device's own storage to `IOBlockStorageDevice`'s `0x90` bytes. The
same rule is why the driver finds `IOBlockStorageDriver` by name through `lookupMetaClass()` and drives
it as an object — the call inside `publishWindow()` — rather than subclassing it.

`registerService()` is never called in this file (the header comment and step 5 of `clover_block_run()`
say who does: the media's own registration, after which IOKit matches `IOGUIDPartitionScheme` and
`IOMediaBSDClient` for it); what this file does instead is *wait* for the automatic tree and then look up
`disk0s39` by the BSD name the kernel itself wrote (the `kBSDNameKey`/`kRootAlias` rename and the
`IOBSDNameMatching` guard in `clover_block_run()`). Round115's mask is the evidence for that design: a
hand-built second scheme lost every name to `EEXIST` — "name slot allocation failed (Errno=17)" — while
the automatic one already had the whole disk named.

## Which geometry, one boot argument

`clover-apfs` is the only boot argument — read by `PE_parse_boot_argn` in step 1b of
`clover_block_run()`, whose comment carries the semantics and explains why it is latched before
publication (`reportMaxValidBlock()` is what `IOBlockStorageDriver` builds the media from):

| value | what it describes |
| --- | --- |
| absent / 0 | the whole card, then discovery steps 5–9 |
| 1 | the logfs window — GPT entry 39, and only if this boot's card reported exactly the extent `clover_emmc_block_window()` hands back, else `fail("window", …)` (step 1b's `else` branch) |
| 2 | the root image: `clover_emmc_block_userdata_window()`'s entry-64 window is read for its `SHNGROOT` manifest and the media becomes the OS disk the manifest points at (step 1b, step 4b's `clover-apfs=2 keeps the probe`) |
| 3 | the boot path: every window in the manifest is read and bounds-checked (`fail("rootwin"/"systemwin"/"datawin")`), only the OS window is published — read-only — and the run returns after `prepareApfsBoot()`, leaving SystemOS and Data to the owner mount at launchd's exec (step 4 plus round145's and round159's deferral comments) |

The constants are all in `BlockCore.h` except the absolute LBAs, which live with the card reader:
`kBlkBytes = 512` (aliased here as `kBlockBytes`), `kRootAlias = "md0s39"` (and the `"disk0s39"` string
it is renamed to), `kBlkSpanBlocks = 8` for the manifest read, the row walk `kBlkRowsAt +
kBlkRowStride * index` with fields at `kBlkRowNameAt`/`kBlkRowOffsetAt`/`kBlkRowSizeAt`/
`kBlkRowFlagsAt`/`kBlkRowDigestAt` inside `blk_parse_manifest()`, `kDeviceWindowSlots = 4` and
`kBatchBlocks = 128`. The `SHNGROOT` signature test is `blk_has_root_magic()`, not a string in this
file any more — and, measured in round316, not a string in the kernel image either: the optimizer folds
eight consecutive ASCII bytes into immediates, so `scripts/verify-clover-payload.py` looks for the
walk's refusal tags (`root-magic`, `root-count`, `system-bounds`, `data-geometry`, `datawin`,
`root-code`), which are CStrings because `fail()` prints them. GPT entry 39 (logfs) and 64 (userdata)
are named in the comment above `clover_emmc_block_userdata_window()`'s declaration here and in
`CloverSdhci`'s own `kLogfsFirstLba`/`kLogfsLastLba`/`kUserDataFirstLba`/`kUserDataLastLba` — logfs
`12320768..12337151`, userdata `15795488..122142686`. A media that claimed a partition the GPT does not
describe would be a second card, and every filesystem above it would be reading the wrong bytes; that is
the reason the window is checked against the card's own report before anything is published.

## The pure layer, and what reads it

`BlockCore.h` is this driver's arithmetic with the bus removed: the manifest walk, the request bound,
the 128-block batching, the delivery accounting, the logfs overlap test and `reportMaxValidBlock()`'s
answer. It includes `<stdint.h>` and nothing else, so `drivers/CloverBlock/tests/clover_block_host.cpp`
compiles the same functions the kernel does and `scripts/test-clover-block.py` runs it on the host. Fifteen
groups cover the walk in any row order, every refusal with the number it prints, the digest copy, the
half-open overlap, and that `blk_last_block()` and `blk_admit_request()` cannot disagree about the last
serviceable block.

Writing the model found three things, and only one of them was a model bug:

| found while asserting | what it turned out to mean |
| --- | --- |
| the fake card's partition ran past the end of the card | the model was asserting an impossible geometry, not the core: `kCardSectors` had to grow before the bounds cases meant anything |
| `blk_le32 (bytes)` of the signature is `0x474e4853` | the transcribed word was wrong; the bytes are 'S','H','N','G' little-endian, which is what `blk_has_root_magic()` compares against |
| a null buffer and a zero-length buffer fell into the same refusal | keeping that identical was the whole job — `blk_admit_request()` takes `has_buffer` explicitly so the legacy zero-block hole stays a hole, and the model asserts it rather than quietly blessing a new refusal |

## What it will and will not write

`reportWriteProtection()` answers per window, from the `writeProtected` field of the `DeviceWindow`
that `registerWindow()` recorded: the boot's own OS window goes out through
`publishWindow(osFirst, osSectors, true, …)` in step 4 of `clover_block_run()`, `publishSystemOs()` and
`publishDataVolume()` — the two windows round145 and round159 deliberately *deferred* to launchd's exec,
each published on demand by the owner mount — pass `true` and `false` respectively, and the logfs/probe
branch ends on `publishWindow(..., false, ...)`. `blk_parse_manifest()` refuses to carry a read-only
flag on Data at all (`const uint32_t expect_flags = which == BLK_W_DATA ? kBlkFlagWritable : kBlkFlagReadOnly`
and its own refusal tag). Only writes inside the logfs window are force-committed — the `blk_overlaps()`
test at the tail of `doAsyncReadWrite()`.

What that flag is worth was measured in round316 against the prelinked family's own source
(`/Users/zhouzhou/UniXNU/out/feeds/IOStorageFamily-ad2e13e0/`), because the claim "this window is
read-only" is only real if some code enforces it: `IOBlockStorageDriver.cpp:1616` calls
`getProvider()->reportWriteProtection(&_writeProtected)` from `recordMediaParameters()`, `:1490` hands
`!_writeProtected` to the media as its `isWritable`, and `IOMediaBSDClient.cpp:1002-1006` turns a
`FWRITE` open of a non-writable media into `EACCES`. `isMediaWritable()` (`:1578`) is not what guards
anything — it is uncalled inside the family — and `IOMediaBSDClient.cpp:985-992` is a one-shot root
exception that grants writer access to a writable *non-APFS* root media, which is why the rd= md0 media
and the OS window are not the same question.

The non-claims are stated where they could be mistaken: an authentication that reads the owner's own
policy is "a bypass, not a pass" (the header's ramdisk paragraph); `prepareApfsBoot()`'s own comment
says "this function mounts nothing"; "what is *not* done here is slicing the disk" is step 2 of
`apfsProbe()`; and of the owner-root work, "the bytes stay eMMC's; only the authentication is bypassed"
is step 5's comment on the `kBSDNameKey` rename in `clover_block_run()`. Report lines carry
`CloverBlock rNNN:` — capital C, unlike every other driver's `clover rNNN:` — and the newest token in the
file is `CloverBlock r190`, the `sync-in`/`sync-out` lines in `doSynchronizeCache()`; `fail()` panics
through `"CloverBlock r137: %s %llu"` because a stopped step has to be loud.

## How this driver meets the UniXNU driver documentation

Layer and wiring are the documented ones (`docs/drivers/overview.md:11-16`; archive from
`[kernel] archives`, `docs/drivers/xnu-drivers.md:118-124`). The object ABI of
`docs/drivers/xnu-drivers.md:106-116` is checked by this module's own builder — `build-clover-block.py`
requires filetype `1` and build version `(2, 0x110000)`, and the magic/CPU/subtype triple is refused
before that by `kits/NativePrelink/native_prelink.py`. Its provider
relationship is the one `docs/drivers/device-tree-matching.md:20-28` describes for `IOResources`
clients — it waits for the `IOBSD` resource instead of matching a device tree node, which is also why
there is no `Info.plist`: `docs/drivers/xnu-drivers.md:14` treats the plist as the record of a
personality, and this driver has none to record. The chain-of-entry-points divergence and the three
prerequisites for replacing it are `notes/unixnu-doc-conformance.md` §5.

Against `docs/drivers/xnu-drivers.md:230-239`: compile and object ABI are gated, hardware is the on-board
loop, kernel boot is covered by the same loop, and host logic has a model — `BlockCore.h` plus
`drivers/CloverBlock/tests/clover_block_host.cpp`, run by `scripts/test-clover-block.py` in the same shape
as `drivers/CloverDsp/tests`. That closes the open item round314 recorded here for this driver
(`notes/unixnu-doc-conformance.md` §7). What still diverges is the class shape above, and it is a
link-time constraint, not a preference.

`check-clover.py` pins the source strings the contract depends on; the pins are named by the round comment
they sit under, because that file is edited nearly every round and a line number in it would rot within
one round of being written:

| pin group | what it refuses |
| --- | --- |
| `# round107, the block device` | the build/link/`clover-emmc=21` triangle, where an unlinked archive stops the storage stack from existing without failing the build |
| `# Round145.` | round145's on-demand SystemOS publication policy |
| `# Round156.` | the 128-block batch with its per-block fallback |
| `# Round159.` | the Data-window verdict tags |
| `# Round171.` | commit-cache and the batch's accounting |
| `# Round190:` | the `sync-in`/`sync-out` barrier lines |
| `# Round316.` | that the walk and the request arithmetic live in `BlockCore.h`, that the driver calls them, that no second copy survives in the `.cpp`, that the model covers every `BLK_E_*` refusal — and then it runs the model |
