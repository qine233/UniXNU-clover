// SPDX-License-Identifier: BSD-2-Clause-Patent
// Qualcomm SMEM heap, SMP2P items, APCS doorbell and ADSP/WCN bring-up constants for
// drivers/CloverDsp.
//
// WHY THESE NUMBERS AND NOT OTHER ONES
//
// Schedule item 5 is network and audio, and both of this board's answers to those two words live on
// the far side of one processor: the audio codec has no register window on the AP at all (every
// `asoc-cpu` name in this board's tree is `msm-dai-q6-*`, the WCD9335 sound card is `status =
// "disabled"`, and the only child of `qcom,msm-audio-apr` is a dummy -- clover.dts:13988-14035), and
// the Wi-Fi is an ICNSS block whose modem-side half is the WCN firmware running *on* the ADSP
// (`qcom,icnss@18800000` at clover.dts:5833, whose rails and MSA region are managed by that same
// ADSP's SSR). So before either block can be discussed, the port has to answer one question: can the
// AP see the ADSP at all, and is the ADSP up?
//
// That question has a hardware answer available in shared memory, which is why this header is about
// SMEM rather than about audio. The AP and the DSP address the same 2 MiB window, the boot allocates
// a table of contents in it, and every inter-processor channel in the Qualcomm design is either an
// item of that table or a bit of one 32-bit register at 0x17911008. Whether `adsp.mdt` was ever
// authenticated and released *in this boot chain* -- the thing no amount of reading the vendor tree
// can establish, because our kernel is not the tree that ran before it -- shows up as: SMEM's
// `initialized` word, the SMP2P items' magic and entry names, the doorbell register's live bits, and
// non-zero pages in the ADSP's carve-out. All four are readable without writing anything.
//
// Four sources, and every constant below names the one it came from:
//
//   DTB   the device's own flattened tree, decompiled into ~/uefi-clover/Resources/DTBs/clover.dts.
//         This is the authority for what the shipping product was actually configured with, and for
//         this subsystem it is unusually direct: the shipped tree names the doorbell register *in the
//         node address* -- `qcom,smp2p-adsp@17911008` with `reg = <0x17911008 0x04>` and
//         `qcom,irq-bitmask = <0x400>` (line 360-364) -- and the GLINK transports carry it as
//         `reg-names = "smem\0irq-reg-base"` (line 4818-4821). It is also the only source for this
//         board's SMEM geometry: `qcom,smem@86000000` at line 4687.
//   VEND  drivers/soc/qcom/smem.c, include/soc/qcom/smem.h, drivers/soc/qcom/smp2p.c and
//         arch/arm64/boot/dts/vendor/qcom/sdm660.dtsi + xiaomi/mi/clover/clover-overlay.dtsi of
//         https://github.com/E7G/clover-sdm660-ksb @ 5c81a3faf37e332be5008512c9b2dc2444dbcc01
//         (Xiaomi's own 4.19 CAF tree for this exact tablet). The authority for the on-the-wire
//         layout of SMEM and SMP2P: the struct definitions whose byte offsets this header turns into
//         constants, and the driver that runs on the shipping product.
//   MAIN  drivers/soc/qcom/smem.c and drivers/soc/qcom/smp2p.c of
//         https://github.com/sdm660-mainline/linux @ dd1ed52c8ceef4f16715118ed0252fe75adbe71d
//         (branch qcom-sdm660-7.2.y). Used as the second opinion on a layout the vendor tree states
//         once, and for the binding shape the shipped tree uses.
//   SHENG ~/UniXNU-sheng/package/QcomSmem.c, QcomSmp2p.{h,c} and AdspProbe.cpp -- the same three
//         blocks on SM8550, and the reason this round is confident about the *form* of the walk:
//         qcom_smem_partition()/qcom_smem_find() have been run against a DSP that answered. Its
//         numbers are a different SoC and are quoted, not copied: sheng's SMEM is 0x81d00000 (not
//         0x86000000), its doorbell is an IPCC block at 0x408000 (not one register at 0x17911008), and
//         its audio protocol is GPR/AudioReach, which does not apply to a platform whose GLINK child
//         is named `apr_audio_svc` (VEND sdm660.dtsi:1526-1529).
//
// THE ONE THING THE TWO DEVICE TREES DISAGREE ABOUT
//
// The CAF tree wires the doorbell through a mailbox *index* -- `mboxes = <&apcs_glb 10>` for the ADSP
// SMP2P edge (VEND sdm660.dtsi:1748), where `apcs_glb` is `mailbox@17911000` with `#mbox-cells = <1>`
// (:1445-1450) -- while the shipped tree gives the register and the mask outright and carries no
// mailbox phandle at all. Those are two bindings for one hardware fact, and they agree: 10 becomes
// BIT(10) = 0x400, which is exactly `qcom,irq-bitmask` in the shipped node. Six independent edges
// confirm the correspondence -- adsp glink 9 = 0x200, adsp smp2p 10 = 0x400, modem smp2p 14 = 0x4000,
// modem glink 15 = 0x8000, cdsp glink 29 = 0x20000000, cdsp smp2p 30 = 0x40000000 (VEND :1475, :1511,
// :1548, :1710, :1748, :1790 against DTB :349, :357, :365, :4815, :4825, :4845) -- and the shipped
// tree adds a seventh the CAF sdm660.dtsi does not place at that register: the RPM edge, mask 0x01
// (DTB :4846). So the register's meaning is settled by two bindings and seven samples rather than by
// one inferred rule.
//
// The shipped tree *also* drops the property the CAF tree needs in order to find the SMP2P items:
// `qcom,smem = <443>, <429>` (VEND :1746) is absent from DTB's `qcom,smp2p-adsp`, as are
// `qcom,local-pid` and the entry child nodes. A driver reading only the shipped node cannot know
// which SMEM item holds which edge. This header does not paper over that: kSmemSmp2pAppsBase is
// offered as the arithmetic the CAF numbers imply (427 + remote_pid gives 429/428/432 for pids
// 2/1/5, which are the three CAF outbound numbers), and the driver treats it as a *candidate*, not
// as a fact -- it reads the item and requires the layout's own self-validation (the "$SMP" magic,
// version 1, total_entries 16, and the local/remote pid fields) before it believes it found an edge.
// An item that has to be identified by its magic rather than by its number is a weaker claim than the
// vendor driver's, and it is the honest one for a tree that does not say.
//
// This header claims nothing about whether the ADSP is up. It names where to look, how to decode what
// is found, and which source each number came from.
#ifndef CLOVER_DSP_REGS_H
#define CLOVER_DSP_REGS_H

// ------------------------------------------------------------------------ the windows
//
// DTB `qcom,smem@86000000`, line 4687-4692:
//   reg = <0x86000000 0x200000 0x17911008 0x04 0x778000 0x7000 0x1fd4000 0x08>;
//   reg-names = "smem\0irq-reg-base\0aux-mem1\0smem_targ_info_reg";
//   qcom,mpu-enabled;
// The CAF tree declares the same first pair under the same name (VEND sdm660.dtsi:336-341,
// `smem_mem: smem-mem@86000000` with `reg = <0x86000000 0x200000>`), and the ADSP and modem GLINK
// transports point at it again as their `smem` reg (DTB :4811, :4820). Two sources, same span: this is
// a transcription. The region is inside DRAM bank 0 (`0x80000000` + `0x80000000`), which is why this
// driver maps it the way CloverSdhci maps a buffer and not the way CloverPower maps a peripheral.
static const uint64_t kSmemPhys = 0x86000000ULL;
static const uint32_t kSmemSize = 0x200000;          // 2 MiB; the whole span is mapped
// The partition table is not at a fixed offset: both trees put it one page from the *end* of the
// region -- VEND smem.c:1096 `devm_ioremap_wc(dev, r.start + resource_size(&r) - SZ_4K, SZ_4K)`, and
// SHENG QcomSmem.c:24 `const uint32_t toc = smem->size - 0x1000;`. Two independent implementations
// agreeing on "last page" is why kSmemPtableOffset is an expression rather than a constant.
static const uint32_t kSmemPtableOffset = kSmemSize - 0x1000;

// The APCS global block's inter-processor register: one 32-bit word, bit N ringing subsystem N's
// doorbell. DTB names it for seven edges (`reg = <0x17911008 0x04>` at :354, :362, :370, :4811,
// :4820, :4841, :4850 -- and again for the SMEM node itself at :4689). The CAF tree reaches the same
// word through the mailbox node `apcs_glb: mailbox@17911000` with `reg = <0x17911000 0x1000>` and
// `#mbox-cells = <1>` (VEND sdm660.dtsi:1445-1450), so the base is the same and the 0x8 is the
// channel-selection register within it. Written by SHENG's equivalent
// (AdspProbe.cpp:190 in the vendor driver it mirrors: `regmap_write(ipc_regmap, ipc_offset,
// BIT(ipc_bit))`), read here.
static const uint64_t kApcsPhys = 0x17911000ULL;     // the block, as the CAF tree names it
static const uint32_t kApcsSize = 0x1000;
static const uint32_t kApcsIpcOffset = 0x08;         // DTB's unit address, and the offset within apcs_glb
// The word itself, as an address rather than as a sum, because that is the form this unit's own tree
// states it (`/proc/device-tree/soc/qcom,smp2p-adsp@17911008/reg` cell 0) and the form the report
// prints. scripts/check-clover.py compares this against the dump, so a base transcribed one digit short
// -- round181's kQupPhys -- cannot reach the card wearing this name.
static const uint64_t kApcsIpcPhys = kApcsPhys + kApcsIpcOffset;

// ------------------------------------------------------------------------ SMEM's header
//
// VEND smem.c:138-146, and the offsets below are that struct's layout with `SMEM_ITEM_COUNT 512`
// (:78) substituted:
//   struct smem_header {
//           struct smem_proc_comm proc_comm[4];   // 4 x 16 = 0x40
//           __le32 version[32];                   // 0x40 .. 0xC0
//           __le32 initialized;                   // 0xC0
//           __le32 free_offset;                   // 0xC4
//           __le32 available;                     // 0xC8
//           __le32 reserved;                      // 0xCC
//           struct smem_global_entry toc[512];    // 0xD0, 16 B each -> 0xD0 + 0x2000
//   };
// The header is therefore 0x20D0 bytes, which is *more* than the one page VEND smem.c:1094 maps for it
// ("map starting 4K for smem header"). That is a real inconsistency in the vendor driver -- a lookup of
// global item 429 reads `header->toc[429]` at 0x1B50, past the end of its own mapping -- and the
// resolution here is not to copy it: this driver maps the whole 2 MiB region, so every TOC entry and
// every item it points at is inside the same span. Recorded because a reader comparing this file with
// the vendor source will otherwise look for a mapping rule that was deliberately not followed.
static const uint32_t kSmemProcComm = 0x00;          // 4 x 16 bytes of the legacy proc_comm interface
static const uint32_t kSmemVersionTable = 0x40;      // 32 words, one per subsystem
static const uint32_t kSmemVersionStride = 4;
static const uint32_t kSmemSblVersionIndex = 7;      // VEND smem.h's MASTER_SBL slot, read by
                                                     // smem.c:821-843 qcom_smem_get_sbl_version()
static const uint32_t kSmemInitialized = 0xC0;
static const uint32_t kSmemFreeOffset = 0xC4;
static const uint32_t kSmemAvailable = 0xC8;
static const uint32_t kSmemReserved = 0xCC;
static const uint32_t kSmemToc = 0xD0;
static const uint32_t kSmemTocEntrySize = 16;        // VEND smem.c:120-125
static const uint32_t kSmemItemCount = 512;          // VEND smem.c:78
#define kSmemAuxBaseMask 0xfffffffcU                 // VEND smem.c:126; bits 1:0 are reserved

// One TOC entry, in order: allocated, offset (from the region base), size (8-byte aligned), aux_base.
// VEND smem.c:120-125. The offset is relative to kSmemPhys, which is what makes the 2 MiB span enough
// to reach an item's data: qcom_smem_get_global() (:519-556) returns `area->virt_base + entry_offset`.
enum
{
  kSmemTocAllocated = 0,
  kSmemTocOffset = 4,
  kSmemTocSize = 8,
  kSmemTocAuxBase = 12
};

// ------------------------------------------------------------------------ SMEM's partition table
//
// VEND smem.c:176-186 (struct smem_ptable + SMEM_PTABLE_MAGIC) and :158-170 (:smem_ptable_entry),
// confirmed word-for-word by the proven SHENG walker (QcomSmem.c:8-11: TOC_MAGIC 0x434f5424,
// TOC_HEADER 32, TOC_ENTRY 48). 4 + 4 + 4 + 5*4 = 32 bytes of header, then 48-byte entries, then the
// optional smem_info.
// The two magics are written as the words a little-endian load produces, not as the four bytes the
// vendor spells: VEND declares `u8 magic[4]` and compares it with memcmp (:838, :915), so its source
// shows the byte order {0x24,0x54,0x4f,0x43} while an implementation that reads one aligned word has
// to compare 0x434f5424 -- which is exactly what SHENG's walker (QcomSmem.c:4-5) does, and the two
// spellings differ by a byte swap that turns a live heap into "no table here". The SMP2P and SIII
// constants below were already in word form (VEND smp2p.c:57 states its magic as a u32), and the host
// model writes the ASCII bytes so this form is pinned rather than remembered.
static const uint32_t kSmemPtableMagic = 0x434f5424ULL;   // "$TOC" read as one little-endian word
static const uint32_t kSmemPtableSize = 32;               // header before the first entry
static const uint32_t kSmemPtableEntrySize = 48;
enum
{
  kSmemPtableVersion = 4,           // VEND smem.c:178; SHENG QcomSmem.c:25 requires 1
  kSmemPtableNumEntries = 8,
  kSmemPtEntryOffset = 0,           // within the entry
  kSmemPtEntrySize = 4,
  kSmemPtEntryFlags = 8,
  kSmemPtEntryHosts = 12,           // host0 in bits 15:0, host1 in 31:16
  kSmemPtEntryCacheline = 16
};
// smem_info follows the last entry (VEND smem.c:855 `&ptable->entry[num_entries]`), magic "SIII"
// (:250-256) and a 16-bit num_items -- the number that bounds the TOC walk, which is why the driver
// reads it instead of assuming 512.
static const uint32_t kSmemInfoMagic = 0x49494953ULL;     // {0x53,0x49,0x49,0x49} = "SIII", word form
static const uint32_t kSmemInfoSize = 0x04;
static const uint32_t kSmemInfoBaseAddr = 0x08;
static const uint32_t kSmemInfoNumItems = 0x10;           // 16 bits

// One private partition's header: VEND smem.c:198-206, 4 + 2 + 2 + 4 + 4 + 4 + 12 = 32 bytes,
// and again exactly what SHENG uses (QcomSmem.c:12 PART_HEADER 32, reading magic at +0, hosts at +4,
// size at +8, the two free marks at +12 and +16).
static const uint32_t kSmemPartMagic = 0x54525024ULL;     // "$PRT" read as one little-endian word
static const uint32_t kSmemPartHeaderSize = 32;
enum
{
  kSmemPartHosts = 4,               // host0 15:0, host1 31:16
  kSmemPartSize = 8,
  kSmemPartFreeUncached = 12,       // where the uncached list ends (it grows up)
  kSmemPartFreeCached = 16          // where the cached area begins (it grows down)
};

static const uint32_t kSmemHostApps = 0;                 // VEND smem.c:81; DTB qcom,ee-style: AP is host 0
static const uint32_t kSmemHostGlobal = 0xfffe;          // VEND smem.c:84; SHENG QcomSmem.h:14
static const uint32_t kSmemHostCount = 8;                // VEND smem.c's SMEM_HOST_COUNT bound

// An item inside a partition: VEND smem.c:232-239 (u16 canary, u16 item, u32 size, u16 padding_data,
// u16 padding_hdr, u32 reserved) = 16 bytes, canary 0xa5a5 (:240). SHENG QcomSmem.c:13-20 walks the
// same 16-byte entry by word: word 0 = canary | item << 16, word 1 = size, word 2 = padding_data |
// padding_hdr << 16, word 3 = reserved -- which is the form the port uses, because it is the form a
// real DSP has already been read with.
static const uint32_t kSmemEntrySize = 16;
static const uint16_t kSmemEntryCanary = 0xa5a5;         // VEND smem.c:240, SHENG QcomSmem.c:9

// ------------------------------------------------------------------------ the items this board names
//
// The numbering is VEND smem.h:155-177, whose own comments carry the resolved values:
//   SMEM_SMP2P_APPS_BASE   = 427      SMEM_IMAGE_VERSION_TABLE  /* 469 */
//   SMEM_SMP2P_MODEM_BASE  = 435      SMEM_GLINK_NATIVE_XPRT_DESCRIPTOR /* 478 */
//   SMEM_SMP2P_AUDIO_BASE  = 443      SMEM_GLINK_NATIVE_XPRT_FIFO_0     /* 479 */
//   SMEM_SMP2P_WIRLESS_BASE= 451      SMEM_GLINK_NATIVE_XPRT_FIFO_1     /* 480 */
//   SMEM_SMP2P_POWER_BASE  = 459      SMEM_NUM_ITEMS            /* 498 */
// The base+8 pattern between the five SMP2P groups is what makes "427 + remote_pid" (Law 2) agree
// with three separate CAF edges, and the fact that the ADSP's GLINK FIFO items are 478/479/480 is not
// a coincidence between two SoCs -- SHENG uses those same three numbers (AdspProbe.cpp:56), so the
// item *slots* for a native-transport GLINK are stable across the family even though the SMEM base is
// not. That is worth one line and no more: this driver still reads the numbers it prints.
static const uint32_t kSmemSmp2pAppsBase = 427;          // VEND smem.h:155
static const uint32_t kSmemSmp2pItemStride = 8;          // the gap between the groups (:156-159)
enum
{
  kSmemItemAdspSmp2pOut = 429,      // 427 + 2, and VEND sdm660.dtsi:1746's second value
  kSmemItemModemSmp2pOut = 428,     // 427 + 1, and VEND :1707's second value
  kSmemItemCdspSmp2pOut = 432,      // 427 + 5, and VEND :1787's second value
  kSmemItemAdspSmp2pIn = 443,       // SMEM_SMP2P_AUDIO_BASE, VEND :1746's first value
  kSmemItemModemSmp2pIn = 435,      // SMEM_SMP2P_MODEM_BASE, VEND :1707's first value
  kSmemItemCdspSmp2pIn = 94,        // VEND :1787's first value -- outside every base+8 rule, and the
                                    // reason the outbound arithmetic is a candidate and not a law
  kSmemItemAdspSsr = 423,           // DTB:5868 `qcom,smem-id = <0x1a7>`; the same slot SHENG calls
                                    // kItemCrash (AdspProbe.cpp:55), i.e. the ADSP's SSR/crash state
  kSmemItemImageVersion = 469,      // VEND smem.h:162
  kSmemItemGlinkDesc = 478,         // VEND smem.h:171, SHENG AdspProbe.cpp:56
  kSmemItemGlinkFifo0 = 479,
  kSmemItemGlinkFifo1 = 480,
  kSmemItemNumItems = 498           // VEND smem.h:177 SMEM_NUM_ITEMS
};

// ------------------------------------------------------------------------ the SMP2P item
//
// VEND smp2p.c:75-89, `__packed`: u32 magic; u8 version; unsigned features:24; u16 local_pid;
// u16 remote_pid; u16 total_entries; u16 valid_entries; u32 flags; then 16 x {u8 name[16]; u32
// value}. That is 20 bytes of header and 20 per entry -- 340 total, and SHENG says the same thing as
// word indices (QcomSmp2p.h:19-20: SMP2P_ITEM_BYTES = 20 + 20*16; :22 of the .c: W_ENTRY(i) = 5+5*i).
static const uint32_t kSmp2pMagic = 0x504d5324ULL;       // "$SMP", VEND smp2p.c:57
static const uint32_t kSmp2pVersion = 1;                 // VEND smp2p.c:58
static const uint32_t kSmp2pFeatureSsrAck = 1;           // VEND smp2p.c:49
static const uint32_t kSmp2pFlagRestartDone = 1;         // VEND smp2p.c:47
static const uint32_t kSmp2pFlagRestartAck = 2;          // VEND smp2p.c:48
static const uint32_t kSmp2pMaxEntries = 16;             // VEND smp2p.c:50
static const uint32_t kSmp2pMaxEntryName = 16;           // VEND smp2p.c:51
static const uint32_t kSmp2pItemBytes = 20 + 20 * kSmp2pMaxEntries;
enum
{
  kSmp2pHeaderMagic = 0,
  kSmp2pHeaderVersion = 4,          // byte 4 version, bytes 5..7 features
  kSmp2pHeaderLocalPid = 8,         // 15:0
  kSmp2pHeaderRemotePid = 10,       // 15:0
  kSmp2pHeaderTotal = 12,
  kSmp2pHeaderValid = 14,
  kSmp2pHeaderFlags = 16,
  kSmp2pEntryStride = 20,
  kSmp2pEntryName = 0,              // 16 bytes, inside the entry
  kSmp2pEntryValue = 16             // u32, inside the entry
};

// ------------------------------------------------------------------------ the edges
//
// One row per AP-to-remote edge this board's trees describe. Three columns of facts, two of them from
// the shipped tree and one from the CAF tree, and the third is what the port is checking: the mask is
// read from DTB (`qcom,irq-bitmask` / `qcom,irq-mask`), the inbound interrupt is the shipped node's
// `interrupts` (type 0 = SPI, so INTID = SPI + 32, the same arithmetic round181 used for gpio67), and
// the CAF mailbox index is quoted to show the two bindings describe one register.
struct DspEdge
{
  const char *label;         // the shipped node's own `label`, when it has one
  uint32_t remotePid;        // DTB qcom,remote-pid, or VEND's for the GLINK edges
  uint32_t irqMask;          // DTB qcom,irq-bitmask / qcom,irq-mask at 0x17911008
  uint32_t cafMboxIndex;     // VEND `mboxes = <&apcs_glb N>`, 0xffffffff when the CAF tree has none
  uint32_t inboundSpi;       // the edge's inbound interrupt as an SPI number
  uint32_t smemInboundItem;  // VEND qcom,smem's first value, 0xffffffff when the trees do not say
  uint32_t smemOutboundItem; // ... and its second (also the 427 + pid prediction)
};

static const struct DspEdge kDspEdges[] = {
  // DTB:4818-4827 -- the transport every audio and compute channel on this board rides.
  { "lpass", 2, 0x200, 9, 157, kSmemItemAdspSmp2pIn, kSmemItemAdspSmp2pOut },
  // DTB:360-365, VEND:1744-1749. The SSRL handshake; the ADSP's force-stop state is driven through
  // its outbound "master-kernel" entry (VEND sdm660.dtsi:1935 `qcom,smem-states = <&adsp_smp2p_out 0>`).
  { "adsp",    2, 0x400,       10, 158, kSmemItemAdspSmp2pIn,  kSmemItemAdspSmp2pOut },
  // DTB:4809-4816, VEND:1466-1476.
  { "mpss",    1, 0x8000,      15, 452, kSmemItemModemSmp2pIn, kSmemItemModemSmp2pOut },
  // DTB:352-357, VEND:1706-1712.
  { "modem",   1, 0x4000,      14, 451, kSmemItemModemSmp2pIn, kSmemItemModemSmp2pOut },
  // DTB:4839-4845, VEND:1543-1550.
  { "cdsp",    5, 0x20000000,  29, 513, kSmemItemCdspSmp2pIn,  kSmemItemCdspSmp2pOut },
  // DTB:368-373, VEND:1786-1792.
  { "cdsp-p",  5, 0x40000000,  30, 514, kSmemItemCdspSmp2pIn,  kSmemItemCdspSmp2pOut },
  // DTB:4848-4855. Present only in the shipped tree, and the one row whose CAF column is empty: the
  // RPM transport is `qcom,glink-rpm-native-xprt` over `msgram` at 0x778000, not over SMEM, so it has
  // no `qcom,remote-pid` and no SMEM item pair at all -- the unknowns are spelled as 0xffffffff rather
  // than guessed, and the row still earns its place because its doorbell bit is in the same register.
  { "rpm", 0xffffffff, 0x01, 0xffffffff, 168, 0xffffffff, 0xffffffff },
};
static const uint32_t kDspEdgeCount = sizeof (kDspEdges) / sizeof (kDspEdges[0]);

// ADSP edge index, spelled rather than counted: the two modes that care about "the DSP" mean this one.
#define kDspEdgeAdspGlink 0U
#define kDspEdgeAdspSmp2p 1U

// ------------------------------------------------------------------------ the firmware regions
//
// The carve-outs the ADSP and the Wi-Fi firmware are loaded into, from the shipped tree's own
// reserved-memory nodes -- `adsp_fw_region@92a00000` with `reg = <0x92a00000 0x1f00000>` and the
// phandle (0xfc) that `qcom,lpass@15700000`'s `memory-region` points at (DTB:16459-16464 and :5872),
// and `wlan_msa_mem@85700000` + 1 MiB with the phandle (0xfa) that `qcom,icnss` names as
// `qcom,wlan-msa-fixed-region` (DTB:16437-16443 and :5850-5851). Both are `compatible =
// "removed-dma-pool"` with `no-map`, i.e. memory the shipping kernel deliberately took out of the
// allocator so nothing else would use it. If a PIL load happened in this boot chain, these pages are
// not zero; if it did not, they are, and that distinction is the reading mode 3 exists to produce.
//
// The two trees give different sizes for one of them, and the difference is this board's own: the
// SoC-level `adsp_fw_region@92a00000` is 0x1e00000 in VEND sdm660.dtsi:360-363, and Xiaomi's clover
// overlay rewrites it to 0x1f00000 (`reg = <0x0 0x92a00000 0x0 0x1f00000>`,
// xiaomi/mi/clover/clover-overlay.dtsi:8) -- which is the value the shipped DTB carries. 31 MiB, not
// 30. That is the shape of every geometry question on this board, so it is spelled out once here
// rather than at each constant: the SoC tree is the default, the board overlay is the truth, and the
// merged DTB on the device is what both are checked against.
static const uint64_t kAdspFwPhys = 0x92a00000ULL;
static const uint32_t kAdspFwSize = 0x1f00000;
static const uint64_t kWlanMsaPhys = 0x85700000ULL;
static const uint32_t kWlanMsaSize = 0x100000;
// The guard region the shipped tree places immediately below the MSA (DTB:16431-16435). Read for the
// same question and reported separately, because a non-zero guard would mean something wrote past the
// Wi-Fi carve-out and that is worth knowing before anything is loaded there.
static const uint64_t kWlanGuardPhys = 0x85600000ULL;
static const uint32_t kWlanGuardSize = 0x100000;

// Mode 3 does not map a whole 31 MiB carve-out to look at it: it maps one page at a time, up to a
// bounded number of pages, and asks two questions of what it sees -- is the page non-zero at all, and
// does any word of the scanned window read as an ELF header (`0x7f 'E' 'L' 'F'`). An ELF magic is the
// shape of an .mdt file (the ADSP's own `adsp.mdt`, per VEND qcom,ssctl and SHENG's QcomMdt loader),
// and a PIL loader that copied segments to their p_paddr does not necessarily leave one at offset 0,
// which is why the scan reports where it found one rather than asserting it should be at the start.
static const uint32_t kImageScanPages = 16;             // 64 KiB per region
static const uint32_t kImagePageSize = 0x1000;
static const uint32_t kImageElfMagic = 0x464c457fULL;   // 0x7f 'E' 'L' 'F' little-endian

// ------------------------------------------------------------------------ the PIL and loader blocks
//
// `qcom,lpass@15700000`, DTB:5854-5879: `compatible = "qcom,pil-tz-generic"`, `reg = <0x15700000
// 0x100>` named base_reg, `qcom,pas-id = <1>`, `qcom,smem-id = <0x1a7>`, `qcom,sysmon-id = <1>`,
// `qcom,ssctl-instance-id = <0x14>`, `qcom,firmware-name = "adsp"`, and its proxy rail
// `vdd_cx` = pm660l L9 with `qcom,vdd_cx-uV-uA = <0x180 0x186a0>` (RPM_SMD_REGULATOR_LEVEL_TURBO and
// 100 mA). None of this is written by this driver. It is here because it is the reason mode 3 is the
// last mode: bringing the ADSP up means an SCM call with PAS id 1 *and* a rail vote through the RPM
// for a LDO whose owner r182 has now measured -- so the load path is gated on the same ownership
// answer the first rail write is, and this round has to be able to say whether that gate is even
// necessary (i.e. whether the ADSP is already up).
static const uint64_t kAdspBaseRegPhys = 0x15700000ULL;
static const uint32_t kAdspBaseRegSize = 0x100;
static const uint32_t kAdspPasId = 1;
static const uint32_t kAdspSmemId = 423;
static const uint32_t kAdspSsctlInstanceId = 0x14;

// `qcom,icnss@18800000`, DTB:5833-5836: `membase` 0x18800000 + 0x800000. Not mapped by this round --
// reading a Wi-Fi block's registers without its rails and its clock is how a bus hang is produced,
// and a bus hang here costs every later line of the boot. The address is recorded because it is the
// next round's window, and because the MSA region below it *is* read here: DTB's `qcom,wlan-msa-memory
// = <0x100000>` and the fixed region handle say which 1 MiB the WCN firmware's memory is, and that is
// a memory question rather than a register one.
static const uint64_t kIcnssPhys = 0x18800000ULL;
static const uint32_t kIcnssSize = 0x800000;

// ------------------------------------------------------------------------ how much to read
//
// The report is one line per thing, into a 512-slot log ring that CloverSdhci tees to the card, and a
// run that prints 512 TOC entries is a run whose useful lines got pushed out of the ring. These are
// the caps, each one named where it is used.
static const uint32_t kTocDumpLimit = 24;               // allocated items listed in mode 1
static const uint32_t kPtableDumpLimit = 16;            // partition entries listed
static const uint32_t kSmp2pEntryDumpLimit = 8;         // entry names printed per item
static const uint32_t kBytesDumpMax = 8;                // printBytes() width, as in r182

// A data abort from an unmapped or MPU-refused address is the one way this driver could cost the boot
// rather than report, so nothing reads a SMEM offset it has not bounded first. kSmemMapWords is that
// bound expressed once, in words, for the TOC; the partition walk bounds itself against the partition
// header's own size field, as SHENG's walker does (QcomSmem.c:37-42).
static const uint32_t kSmemTocWords = (kSmemToc + kSmemItemCount * kSmemTocEntrySize) / 4;

#endif // CLOVER_DSP_REGS_H
