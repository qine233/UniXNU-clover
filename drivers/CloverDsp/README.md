# CloverDsp — reading the shared heap to ask whether the ADSP is already up

排期 5 is "network, audio, USB", and all three of those answers run through one processor: the ADSP.
The audio codec path is APR over GLINK over SMEM, the WCN3990's control path is GLINK, and the board's
own tree routes both through the same two blocks — a 2 MiB shared heap at `0x86000000` and one
doorbell word at `0x17911008`. So the first question of 排期 5 is not "how do we speak APR" but
"what is already there": if the ADSP is running, its SMP2P edges are allocated in SMEM and its
firmware pages are non-zero, and the round after this one is a protocol round. If it never came up,
the same round would be spending its time on a transport with nothing on the other end, and the real
next step is 排期 6's — a PIL load (`qcom,pas-id = <1>`) over a `vdd_cx` rail that round182 has
already measured as off.

This driver reads exactly those two blocks and prints enough to make that fork. It writes nothing, and
on this hardware the distinction is not a nicety: on the APCS IPC register, a write *is* the interrupt.

## The windows, from this board's own device tree

`/proc/device-tree/soc/qcom,smem@86000000/reg` and the three `qcom,smp2p-*@17911008` nodes; the
transcription lives in `DspRegs.h` and each address is bound in `check-clover.py`'s DT contract:

| region | physical | size | what lives there |
| --- | --- | --- | --- |
| SMEM | `0x86000000` | `0x200000` | header at `+0x00`, 512-entry global TOC at `+0xd0`, `$TOC` partition table one page from the end |
| APCS IPC | `0x17911008` | 4 | the doorbell word — seven edges' bits in one register |
| adsp_fw | `0x92a00000` | `0x1f00000` | the ADSP's `removed-dma-pool` carve-out, `no-map`; referenced by `qcom,lpass@15700000` |
| wlan_msa | `0x85700000` | `0x100000` | the WCN firmware's MSA region, named by `qcom,icnss@18800000` |
| wlan_guard | `0x85600000` | `0x100000` | the guard page below it, read because a non-zero guard means something wrote past the carve-out |

The `qcom,icnss` register block itself is *not* mapped. Reading a Wi-Fi block's registers without its
rails or clocks is how a bus hang is produced, and a hang here costs every later line of the boot.

## The three questions, one per boot argument (`clover-dsp=N`)

Mode 1 reads the heap and nothing else: the header fields (`initialized`, `free_offset`, `available`,
`reserved`, the SBL version slot, the legacy proc_comm pair), a bounded walk of the 512-entry global
TOC with a census of allocated items, then the `$TOC` partition table with each partition row checked
against its own header — the two host ids, the declared size, and the partition's own magic.

Mode 2 does mode 1, reads the doorbell word once and decodes it against the seven masks this board's
tree declares, then looks up the six ADSP/modem/CDSP SMP2P items — outbound and inbound for each pair
— validating every candidate as `$SMP` version 1 with sixteen entries and both pids agreeing before it
prints anything about it.

Mode 3 does mode 2, dumps the raw bytes of the ADSP's SSR item (423), the three GLINK native-transport
items (478/479/480) and the image-version table (469), then scans the first 64 KiB of the ADSP
firmware region and the WLAN MSA region: pages seen, pages non-zero, first non-zero page, and the
offset of any word that reads as an ELF magic.

How to read the report:

| what appears | what it means |
| --- | --- |
| `mode 3 smem=86000000+200000 apcs=17911000+1000` | both windows mapped before the first byte was read — that order is the MPU protection, not a style |
| `smem initialized=0; continuing, the table may still be there` | the boot left the heap uninitialised for this EE; partition lookups are disabled and only global TOC reads are attempted |
| `toc allocated=… listed=… beyond_declared=…` | how many of the 512 slots are taken, and whether the walk stayed inside the declared item count |
| `ptable at … magic=$TOC version=1 entries=… num_items=…` | the heap is the layout this code implements; a `magic=… rc=-4` line instead means the bytes are a byte-order mismatch, not an empty heap |
| `part[n] hosts=0:x … ok=1 hosts_ok=1 rc=0` | a partition this AP actually shares with a remote — the only place its SMP2P items can live |
| `doorbell 17911008+04 = 00000000` | no edge has signalled this boot; the register answered, which is itself the reading |
| `edge[1] adsp pid=2 mask=00000400 … set=0 out=429 pred=429 law=1` | the row's declared item matches the `427 + remote_pid` rule, so the item number used downstream is derived, not copied |
| `adsp-out item=429 rejected rc=-11 (pid)` | something allocated at that number, but its pid fields name a different pair — an item is not an edge until both agree |
| `adsp-out item=429 absent rc=-9 (item)` | neither the private partition nor the global TOC holds it; on a heap whose rest decoded cleanly this means the ADSP never registered |
| `image adsp-fw 92a00000+10000 pages=16 nonzero=0 …` | the firmware carve-out is empty in this boot: nothing was ever PIL-loaded there |
| `image adsp-fw … nonzero=11 first=0 elf_at= none` | pages with content, and no ELF header at the start — a loader copied segments and moved on, or something else owns those bytes |
| `verdict adsp_edge=yes adsp_loaded=yes` | 排期 5 is a protocol round: talk to what is already up |
| `verdict adsp_edge=no adsp_loaded=no` | 排期 5's first step is 排期 6's: `pas-id 1` over a rail round182 measured off, and the order of those two is fixed by the L9 enable byte |

## Why the decoder is not in `CloverDsp.cpp`

Because the one bug class that would make this whole round lie is a byte-order bug, and it is invisible
on a board. SMEM's magics are read as *words* — `$TOC` is `0x434f5424`, `$PRT` is `0x54525024`, `SIII`
is `0x49494953`, `$SMP` is `0x504d5324` — while the vendor sources declare them as `u8 magic[4]` and
`memcmp` them. Transcribe the four bytes in the order they appear in the source and you get a live
2 MiB heap that decodes as "no partition table", on a board where every address is correct. That is
indistinguishable from an empty heap in the field and costs a full flash cycle to find out.

So `DspCore.h` holds the decoder as inline functions over plain spans — header walk, TOC census,
partition rows, the two item lists (uncached growing up, cached growing down from `size - align_up(16,
cacheline)`), canary checks, SMP2P validation, the seven doorbell masks, the page/ELF scan — and
`drivers/CloverDsp/tests/clover_dsp_host.cpp` drives those exact functions against a synthesized
128 KiB heap on a Mac, with 189 assertions. `scripts/test-clover-dsp.py` compiles and runs it, and
`scripts/check-clover.py` treats a nonzero return as a failed round, so the magic form, every bound
and the pid pairing are proven before the driver reaches hardware.

## Why it is read-only, in the hard sense

Nothing in this driver has a write path. The doorbell register is read once per run because writing
`BIT(n)` there raises the interrupt for that edge — this load is the only way to ask the question
without becoming part of the answer. SMEM is walked through `volatile uint8_t *` spans with subtractive
bounds checks against the mapped size, and the image scan maps a bounded 64 KiB window per region,
releases it, and moves on.

That guarantee is machine-checked: `check-clover.py` fails if `windowWrite`, `ds_write`, `kSmp2pSet` or
`IOMemoryDescriptor::withAddress` appears anywhere in the driver, and requires each bound it relies on
(`smem.size < kSmemToc`, `entry.data + entry.size > smem.size`, `item.total_entries != kSmp2pMaxEntries`,
`if (entry.aux_base)`, `DS_E_RANGE`) to still be in `DspCore.h`.

## Why nothing here panics

The ADSP's own `clover.dts:4692` carries `qcom,mpu-enabled`, and a data abort from an unmapped or
MPU-refused address is the one way this round could cost the boot rather than report. Three rules keep
that impossible: both windows are mapped *before* the first read of either, every offset the decoder
follows is bounded against the span it came from, and a refusal is a return code with a name
(`DS_E_SPAN` … `DS_E_PID`), never a fault. It runs last in the PMGR tail on purpose — after
`clover_power_run()` — because "was the ADSP ever loaded" only becomes a plan next to "is `vdd_cx`
on", and an unloaded ADSP plus an off L9 is a two-step job with the rail first, not a PIL attempt that
hangs on an unpowered CX domain. `clover-dsp` absent means not a byte is read.

## Link wiring

| piece | file | note |
| --- | --- | --- |
| compile | `scripts/build-arm-driver.py CloverDsp` | the recorded `IOPlatformExpert.cpo.json` flags, plus the ARM64/iOS 17 object audit |
| archive | `build/clover-dsp/libCloverDsp.a` | plain archive path, no `force_load`; `_clover_dsp_run` is what pulls the member in |
| link | `configs/clover_defconfig` → `CONFIG_XNU_EXTRA_LD_ARCHIVES` | fifth entry, after CloverSdhci / CloverBlock / CloverInput / CloverPower |
| schedule | `drivers/CloverPMGR/CloverPMGR.cpp` | `clover_dsp_run()` at the very tail of `sheng_pmgr_run()`, after `clover_power_run()` |
| switch | `board.toml` → `kernel_args` | `clover-dsp=3` |
| host gate | `scripts/test-clover-dsp.py` | run by `scripts/check-clover.py`; a nonzero return is a failed round |
| verify | `scripts/verify-clover-payload.py` | `kernel.macho: dsp instrument linked`, nine strings |

## What this is not

No PIL load, no SCM call, no peripheral-advantage-state transition — `kAdspPasId` is recorded here
because it is the next round's argument, not this round's. No APCS write, so no interrupt is raised and
no edge is cleared. No GLINK, APR or QRTR packet, and no claim that a channel exists because an item
is allocated. No clocks or rails voted: the `vdd_cx` question belongs to the SPMI/RPM path that
round182 measured and round184 may ask to switch. And no register read of `qcom,icnss@18800000`, the
Wi-Fi block itself — only its MSA memory, which is a memory question rather than a register one.
