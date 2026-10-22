# UniXNU-clover

Device repository for the **Xiaomi Mi Pad 4 8" LTE (2018)** — codename `clover`,
LTE variant, 4 GB RAM, Qualcomm **SDM660** (Snapdragon 660, Kryo 260: 4× Cortex-A53
at 1.8 GHz + 4× Kryo 385 Gold at 2.2 GHz).

It describes the board to the [UniXNU](https://github.com/UniXNU/UniXNU) build driver:
the target IPSW, the kernel configuration, the prelink roots, the firmware checkout,
the ESP payload and the Apple-format device tree. The companion firmware repository is
`uefi-clover`, a Mu-Silicium port for the same board.

`clover` is the name mainline uses for this family (`xiaomi,clover-lte`) and the name
Xiaomi uses for the platform (`BoardPlatform: clover`), so the board keeps it.

## Status

| Layer | State |
| --- | --- |
| Board description | `board.toml` parses; `scripts/check-clover.py` cross-checks it against the defconfig and the payload contract |
| ADT | `scripts/make-clover-adt.py` emits 13,880 bytes for the 8 Kryo 260 cores; the check reads the encoding back and asserts the nodes `ArmDTPlatform` and DarwinBoot require |
| Kernel | **not built** — the UniXNU kernel step needs macOS with an accepted Xcode license |
| Payload / boot.img | **not built** — they depend on the kernel artifacts |
| Firmware | **not built** — needs the `uefi-clover` checkout and this unit's proprietary firmware inputs |
| Device facts | `scripts/dump-clover-facts.sh` is ready; nothing has been collected from the tablet yet |
| Boot on hardware | **not demonstrated** |

Nothing above claims more than that table.

## Hardware facts used here

Every constant comes from a published device tree, not from guesswork. Both trees agree,
and `feeds.toml` pins them so `unixnu fetch` keeps a copy:

| Item | Value | Source |
| --- | --- | --- |
| SoC / msm-id | SDM660, `qcom,msm-id = <317 0x0>` (`0x13d`) | downstream `sdm660.dtsi` |
| Downstream board-id | `qcom,board-id = <11 0>` on the MTP base | `sdm660-mtp-clover.dts` |
| Cores | Silver `0x0-0x3` (A53, 1804800 kHz), Gold `0x100-0x103` (2208000 kHz) | `sdm660.dtsi` → `sdm630.dtsi` |
| Timebase | 19.2 MHz | both trees |
| DRAM | base `0x80000000`; the 4 GB unit adds a second bank at `0x1800000000` | both trees, TODO-DEVICE |
| GIC | distributor `0x17a00000`/`0x10000`, redistributor `0x17b00000`/`0x100000`, stride `0x20000` | downstream `sdm660.dtsi` |
| Splash / framebuffer | `cont_splash_mem` = `0x9d500000` + `0x02400000` (the base board declares `0x9d400000` + `0x23ff000`; the clover overlay moves it) | `clover-overlay.dtsi` |
| Panel | 1200×1920, `boe,tv080wum-nx2` / `novatek,nt51021`, 107×172 mm, so 8 inch | mainline + downstream |
| Touch | `edt,edt-ft5406` at `blsp_i2c3` 0x38 (mainline); downstream uses focaltech `fts` | `sdm660-xiaomi-clover-regular-common.dtsi` |
| Backlight | `pm660l_wled`, an LED driver rather than a PWM | same |
| Storage | eMMC `sdhc_1: sdhci@c0c4000` (hc `0xc0c4000`/`0x1000`, cqhci `0xc0c5000`/`0x1000`, ICE `0xc0c8000`/`0x8000`), CQE + HS400ES, 8-bit | downstream `sdm660.dtsi` |
| Reserved regions | adsp `0x92a00000`, mba `0x94900000`, cdsp `0x94b00000`, venus `0x9cc00000`, ramoops `0x9fe00000`, gpu zap `0xf0b00000` | downstream trees |
| Serial console | `blsp1_uart2`, 115200, declared as `stdout-path` | both trees |
| USB | `dr_mode = "peripheral"` — device only | both trees |
| Firmware reference | Mu-Silicium `lavender` (Redmi Note 7) is the same SoC: DDR `0x80000000`, FD `0x9FC00000`/`0x300000`, header v1, `append_dtb`, psci `smc` | `Resources/Configs/lavender.toml` |

The target OS identity is the unchanged **iPad7,1 / iPadOS 17.7.11 / 21H461** image
(`j120ap`, `ApChipID 0x8011`, `ApBoardID 12`). Kryo 260 is ARMv8.0 without pointer
authentication, so this 4 KiB-page class of image is the only one that can run here, and
the kernel is built as `DEVELOPMENT_ARM64_BCM2837`.

## Layout

```
board.toml                     board description
configs/clover_defconfig       kernel configuration
feeds.toml                     the two device trees every constant comes from
drivers/                       (empty) device XNU drivers, once there is one to put here
kits/                          (empty) device kernel libraries
patches/                       (empty) device patches
package/                       (empty) UniXNU kit contents for this board
scripts/                       ADT generator, preflight, fact dump, archive builders
notes/                         research notes with the source list
facts/                         per-unit dumps, ignored by Git
```

## Build

The kernel step is macOS-only; the firmware step builds on Linux. The host still needs
two things before any of this runs: `sudo xcodebuild -license accept` (Xcode 27 on this
machine is installed but unlicensed, which also breaks `git` and Homebrew until it is
accepted) and a Python 3.11+ interpreter for `./unixnu` and the board cross-check — the
system Python 3.9 has no `tomllib`, so `scripts/check-clover.py` runs its ADT group and
skips the board group there.

```sh
git clone --recurse-submodules https://github.com/UniXNU/UniXNU
git clone <this repo> UniXNU-clover

cd UniXNU
./unixnu defconfig clover_defconfig -D ../UniXNU-clover
./unixnu fetch
./unixnu tree
python3 ../UniXNU-clover/scripts/check-clover.py      # preflight, safe to run any time
./unixnu build firmware --uefi ../uefi-clover          # needs the firmware repository
./unixnu build kernel                                  # macOS + Xcode
./unixnu build package
./unixnu build payload --unique-chip-id 0x<device seed>
./unixnu build boot
```

`scripts/check-clover.py` is the `[firmware] preflight`. It generates the ADT into a
temporary file, decodes the Apple node encoding again, and asserts `/chosen` (64-byte
`random-seed`, CHRP NVRAM proxy, `dram-base`, `crypto-hash-method`, `root-matching`,
`display-base`), the `/chosen/memory-map` placeholders `RAMDisk`, `PackedRAMDisk` and
`TrustCache`, `/cpus` with the eight MPIDR values, `/psci`, `/arm-io` with `ranges` and
the GICv3 node, `/defaults`, `/options` and `/product`.

## Before the first install

`scripts/dump-clover-facts.sh` collects what only this unit can answer and writes it to
`facts/<serial>-<stamp>/`:

```sh
adb shell cp /tmp/dump-clover-facts.sh /data/local/tmp/ && adb shell sh /data/local/tmp/dump-clover-facts.sh
```

Three values in `board.toml` are placeholders until then, and each is marked `TODO-DEVICE`:

- `[userdata] first_lba` / `last_lba` — the GPT entry named `userdata` on this tablet.
  Without them no image can be written behind the manifest.
- The DRAM bank split — `dram_size` currently declares the low 2 GiB bank only.
- `qcom,board-id` as the merged dtb/dtbo of this unit actually declares it, and the ABL
  boot-image parameters.

## What is intentionally left out

- **No block driver.** XNU has `NativeUfs` for UFS and nothing for eMMC/SDHCI, so the
  sheng route — APFS volumes read straight out of userdata — is closed. clover follows
  the gta4l shape instead: a console-first kernel with `CONFIG_XNU_SOURCE_MOCKFS`, no
  `CONFIG_XNU_NATIVE_UFS`, and `[userdata]` kept in the board file for the day an SDHCI
  driver exists.
- **No GPU.** `NativeAdreno` targets Adreno 740; this board has Adreno 512. The display
  stack is therefore absent, and `scripts/build-clover-display-stub.py` provides only the
  one empty symbol the DEVELOPMENT-only XNU patch calls unconditionally. The goal of the
  first boot is the UEFI menu and the XNU console, not a framebuffer.
- **No touch.** `edt-ft5406` has no UniXNU driver, and USB is peripheral-only, so menu
  input has to come over the debug UART (`blsp1_uart2`, 115200) until someone adds one.
- **No Data seed.** `[payload.adt]` declares `packed_data_ramdisk` because the ADT has to
  reserve the `PackedRAMDisk` placeholder, but `[payload]` carries no `trust_cache_sha256`
  and no owner bundle yet.

## License

BSD-2-Clause-Patent, same as UniXNU. `scripts/build-arm-driver.py` is ported from
UniXNU-gta4l (NoNameBuilder), which carries the same license.
