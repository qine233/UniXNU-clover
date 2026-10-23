# UniXNU-clover

**小米平板 4 8 英寸 LTE 版（2018）**的设备仓库——代号 `clover`，LTE 版本，
4 GB 内存，高通 **SDM660**（骁龙 660，Kryo 260：4× Cortex-A53 @1.8 GHz
+ 4× Kryo 385 Gold @2.2 GHz）。

它向 [UniXNU](https://github.com/UniXNU/UniXNU) 构建驱动描述这块板子：
目标 IPSW、内核配置、prelink 根、固件检出、ESP payload 和 Apple 格式设备树。
配套的固件仓库是 `uefi-clover`，同一块板子的 Mu-Silicium 移植。

`clover` 既是主线给这个家族的名字（`xiaomi,clover-lte`），也是小米给这个平台
的名字（`BoardPlatform: clover`），所以板级标识沿用它。

## 状态

| 层 | 状态 |
| --- | --- |
| 板级描述 | `board.toml` 可解析；`scripts/check-clover.py` 把它与 defconfig 和 payload 契约交叉核对 |
| ADT | `scripts/make-clover-adt.py` 为 8 个 Kryo 260 核生成 13,880 字节；检查器会把编码读回来，并断言 `ArmDTPlatform` 和 DarwinBoot 需要的节点 |
| 内核 | 已构建：`DEVELOPMENT_ARM64_BCM2837` 的 51.8 MB `kernel.macho` |
| payload / boot.img | 已构建：56 MiB Fat32 payload 卷，以及 kernel 段就是固件的 boot 镜像；payload **不**走 ramdisk（见下文）。`v0.2.11` 起 payload 里的 `kernel.macho` 是一份打了探针的副本：入口色带、panic 屏幕，以及开 MMU 后某个 `arm_init` 调用返回时触发的定时复位（见"菜单之后看什么"） |
| 固件 | 由 `uefi-clover` 构建（3 MiB 固件卷，内含 UniXNU 与 DarwinBoot）；`v0.3.10` 起 DXE 尾部完整派发，`CloverGopDxe` 发布 GOP |
| 设备事实 | 2026-10-22 采集过一次，位于 `facts/81ffcaaa-20261022-210928/` |
| 真机启动 | **菜单已到**（Stage 1）：`v0.3.10` 固件在屏幕上渲染 UniXNU 菜单并读到了 payload；菜单 3 秒后默认进 XNU。`v0.2.9` 实测屏幕出现**绿 + 蓝 + 品红**三条色带，即 DarwinBoot 已交接、XNU 入口已执行、引导页表已建好；`v0.2.10` 实测**品红出现约 6 秒后设备自己重启**，即 `sctlr_el1` 打开 MMU 后没有立刻死掉；`v0.2.11` 把复位点挪到 `arm_init` 里的 `_arm_vm_init` 之后，待上机 |

上面这张表之外，不做任何额外声称。

## 这里用到的硬件事实

大部分常量来自公开的设备树，在 `feeds.toml` 里钉住版本，`unixnu fetch`
会保留一份副本；平板自己能回答的那些来自 `facts/<serial>-<stamp>/` 下的
dump，"来源"一列写明了哪个是哪个：

| 项目 | 值 | 来源 |
| --- | --- | --- |
| SoC / msm-id | SDM660，`qcom,msm-id = <317 0x0>`（`0x13d`） | 下游 `sdm660.dtsi` |
| 下游 board-id | MTP 基座上的 `qcom,board-id = <11 0>` | `sdm660-mtp-clover.dts` |
| 核心 | Silver `0x0-0x3`（A53，1804800 kHz），Gold `0x100-0x103`（2208000 kHz） | `sdm660.dtsi` → `sdm630.dtsi` |
| 时基 | 19.2 MHz | 两套树一致 |
| DRAM | base `0x80000000` + `0x80000000`，第二个 bank `0x100000000` + `0x7e4c0000` | `dt/memory.raw.txt`、`proc/meminfo.txt`（MemTotal 3825460 kB） |
| GIC | distributor `0x17a00000`/`0x10000`，redistributor `0x17b00000`/`0x100000`，stride `0x20000` | 下游 `sdm660.dtsi` |
| Splash / framebuffer | `0x9d400000` + `0x02400000`，即固件自己内存图里的 "Display Reserved" 行。`clover-overlay.dtsi` 把 `cont_splash_mem` 挪到 `0x9d500000`，那是给"内核自己重画 splash"准备的；真正说话的是这块板子启动的固件，基座设备树与它一致 | `fw/xbl-uefiplat.txt` |
| 屏幕 | 1200×1920，`boe,tv080wum-nx2` / `novatek,nt51021`，107×172 mm，即 8 英寸 | 主线 + 下游 |
| 触摸 | 主线用 `edt,edt-ft5406` @ `blsp_i2c3` 0x38；下游用 focaltech `fts` | `sdm660-xiaomi-clover-regular-common.dtsi` |
| 背光 | `pm660l_wled`，是 LED 驱动而不是 PWM | 同上 |
| 存储 | eMMC `sdhc_1: sdhci@c0c4000`（hc `0xc0c4000`/`0x1000`，cqhci `0xc0c5000`/`0x1000`，ICE `0xc0c8000`/`0x8000`），CQE + HS400ES，8-bit | 下游 `sdm660.dtsi` |
| 保留区 | adsp `0x92a00000`、mba `0x94900000`、cdsp `0x94b00000`、venus `0x9cc00000`、ramoops `0x9fe00000`、gpu zap `0xf0b00000` | 下游设备树 |
| 串口控制台 | `blsp1_uart2`，115200，声明为 `stdout-path` | 两套树一致 |
| USB | `dr_mode = "peripheral"`——只有 device 模式 | 两套树一致 |
| 固件参照 | Mu-Silicium 的 `lavender`（Redmi Note 7）是同一颗 SoC：DDR `0x80000000`、FD `0x9FC00000`/`0x300000`、header v1、`append_dtb`、psci `smc` | `Resources/Configs/lavender.toml` |

目标系统标识是原封不动的 **iPad7,1 / iPadOS 17.7.11 / 21H461** 镜像
（`j120ap`，`ApChipID 0x8011`，`ApBoardID 12`）。Kryo 260 是 ARMv8.0，
没有指针认证，所以只有这一档 4 KiB 页的镜像能跑，内核按
`DEVELOPMENT_ARM64_BCM2837` 构建。

## 目录

```
board.toml                    板级描述
configs/clover_defconfig      内核配置
feeds.toml                    每个常量所依据的两棵设备树
drivers/                      （空）板级 XNU 驱动，等有了再放
kits/                         （空）板级内核库
patches/                      （空）板级补丁
package/                      （空）本板的 UniXNU kit 内容
scripts/                      ADT 生成器、预检、事实采集、归档构建
notes/                        带来源清单的调研笔记
facts/                        按机器存放的 dump，Git 忽略
```

## 构建

内核这一步只能在 macOS 上做；固件这一步在 Linux 上构建。动手之前宿主机还差
两件事：`sudo xcodebuild -license accept`（这台机器的 Xcode 27 已安装但未接受
许可，未接受前 `git` 和 Homebrew 也会跟着坏）以及给 `./unixnu` 本身准备的
Python 3.11+。`scripts/check-clover.py` 没有这个要求：它用 `tomllib`、`tomli`
或自带的子集解析器读 board.toml，所以在系统 Python 3.9 上四组检查都能跑。

```sh
git clone --recurse-submodules https://github.com/UniXNU/UniXNU
git clone <this repo> UniXNU-clover

cd UniXNU
./unixnu defconfig clover_defconfig -D ../UniXNU-clover
./unixnu fetch
./unixnu tree
python3 ../UniXNU-clover/scripts/check-clover.py      # 预检，任何时候都可以跑
./unixnu build firmware --uefi ../uefi-clover          # 需要固件仓库
./unixnu build kernel                                  # macOS + Xcode
./unixnu build package
./unixnu build payload --unique-chip-id 0x<device seed> --uefi ../uefi-clover
./unixnu build boot
```

`scripts/check-clover.py` 就是 `[firmware] preflight`。它把 ADT 生成到临时文件，
再把 Apple 的节点编码解回来，断言 `/chosen`（64 字节 `random-seed`、CHRP
NVRAM proxy、`dram-base`、`crypto-hash-method`、`root-matching`、`display-base`）、
`/chosen/memory-map` 里的占位 `RAMDisk`、`PackedRAMDisk` 和 `TrustCache`、
带八个 MPIDR 值的 `/cpus`、`/psci`、带 `ranges` 与 GICv3 节点的 `/arm-io`、
`/defaults`、`/options` 和 `/product`。

## dump 回答了什么

`scripts/dump-clover-facts.sh` 采集只有这台机器才能回答的东西，写到
`facts/<serial>-<stamp>/`：

```sh
adb shell cp /tmp/dump-clover-facts.sh /data/local/tmp/ && adb shell sh /data/local/tmp/dump-clover-facts.sh
```

dump 已跑过一次（2026-10-22），`board.toml` 里有三个占位现在由设备本身回答：

- `[userdata] first_lba` / `last_lba` —— `0xf10520` / `0x747bfde`，mmcblk0p64，
  按 512 字节扇区。没有它们，任何镜像都写不到 manifest 之后。
- DRAM 分 bank —— `0x80000000+0x80000000` 和 `0x100000000+0x7e4c0000`；
  `dram_size` 只声明低 bank，因为 `/chosen` 只带一组 base/size。
- `qcom,board-id` —— merged dtb 声明的是 `<0x0001000b 0x0>`，
  `qcom,msm-id` 确实是 `<317 0>`。

固件自己的 `uefiplat.cfg`（同一份 dump，`fw/xbl-uefiplat.txt`）随后在 splash
地址上推翻了设备树：是 `0x9d400000`，不是 overlay 的 `0x9d500000`；
`notes/clover-device-tree-sources.md` 第 5 节保留了推理过程。

boot 镜像参数也不再是猜的。dump 把整个 `boot` 和 `recovery` 分区都拉了下来
（`img/unixnu-boot.img`、`img/unixnu-recovery.img`——`unixnu-` 前缀是脚本的
命名，字节是 stock 小米的），它们的头在 `[bootimg.image]` 需要的每个字段上
都互相一致：

| 字段 | 值 | 如何得知，而不只是读到 |
| --- | --- | --- |
| header version | `0` | 两个镜像在 0x28 槽位都是零 |
| page size | `0x1000` | 由算术独立证明：ramdisk 恰好从 `0x1000 + align(kernel_size, 0x1000)` 开始 |
| kernel offset | `0x8000` | 相对 base；`0x80000000` 不进头部，这就是 `base = 0` 的原因 |
| ramdisk offset | `0x1000000` | 同上 |
| tags offset | `0x100` | 同上 |
| dtb | 追加 | kernel 段是 `Image.gz` 加紧跟 gzip 流之后的串联 DTB：`boot` 里 111 个，`recovery` 里 1 个 |

两个镜像都没有 AVB 尾，`id` 全零，所以这个变体的 boot 镜像自身不带签名。
`[bootimg.image]` 里原来那些 gta4l 占位值（`ramdisk_offset 0x20000000`、
`tags_offset 0x01e00000`、`header_version 1`）对 clover 是错的，固件仓库的
检查器现在钉住真实数值。

## payload 放在哪

payload 卷不是 boot 镜像里的 ramdisk。这块板子构建出的固件卷包含 `Fat`、
`PartitionDxe` 和 `DiskIoDxe`，但没有任何 ramdisk 文件系统驱动，所以可能出现
的 `SimpleFileSystem` 实例只能挂在 `SdccDxe` 的 eMMC 上。UniXNU 靠扫描每个
`SimpleFileSystem` 找 `\EFI\UniXNU\unixnu.toml`；只存在于 boot 镜像 ramdisk
字段里的卷不在其中。无论镜像多大，它都看不见。

所以这个卷写到 eMMC 分区上，就像上游 fajita 写到 `boot_a` 那样。
`cache`（`mmcblk0p49`，256 MiB，524288 扇区）可以随便牺牲，也装得下 56 MiB
的卷。`fastboot flash cache` 写入的是裸 Fat32 文件系统，也正是固件挂载的东西：
分区类型对 PartitionDxe 无关紧要，`CLOVERESP` 标签只是装饰。boot 镜像完全
不带 ramdisk，这也让它远低于 ABL 开始拒绝返回的大小。

```sh
fastboot flash boot  ../UniXNU/out/clover/boot/unixnu-diag-nopayload.img   # 只有固件
fastboot flash cache ../UniXNU/out/clover/payload/payload.img
fastboot reboot
```

## 菜单之后看什么

`blsp1_uart2` 在这块板子上没有引出，所以菜单之后只能让屏幕说话。`v0.2.8` 的
payload 里放的是一份**打了探针的 kernel 副本**（`scripts/patch-xnu-entry-probe.py`
改副本，原始 `out/clover/kernel/kernel.macho` 不动）。探针代码写在入口之后、
原本全零的填充区里（`0xFFFFFFF0073EC48C` 起 0xB74 字节），每个站点在改写前都
会先核对原始指令，内核一变就拒绝打补丁。

屏幕从上往下是 1/16 屏高的色带，最上面一条绿带由固件里的 DarwinBoot 自己画
（`v0.3.10` 就有，不需要改内核）：

| 色带 | 位置 | 含义 |
| --- | --- | --- |
| 绿 | 第 0 条，DarwinBoot 画 | `DarwinEnter` 跑到了跳转内核入口的那条 `br` |
| 蓝 | 第 1 条，内核入口 | `_start` 的第一条 `b` 之前：CPU 已经进 XNU，MMU 还是关的 |
| 品红 | 第 2 条，开 MMU 之前 | `msr sctlr_el1, x0` 之前：引导页表已经建好，正准备开 MMU |

三条都在，说明内核跑过了入口和页表建立；只有绿带说明 DarwinBoot 交接了但内核
第一条指令就没回来；只有绿和蓝说明卡在 `_start_first_cpu` 的引导阶段。色带
以下如果出现白字，那是 `panic_screen` 把 panic 的格式串、`TCR`、`SCTLR`、
页计数画在了内核自己的 framebuffer 映射上——正好可以拍照。没有 panic 时
屏幕就停在最后画完的色带。

MMU 一开，framebuffer 就不可读了，所以之后的问题只能由**时间**回答：探针在
指定位置等一会儿，再发 PSCI `SYSTEM_RESET`（`smc`，功能号 `0x84000009`）。
reboot 本身就是"执行到了这里"的证据；停住不动就是"没到"。

`v0.2.10` 的复位点在 `msr sctlr_el1, x0; isb` 之后，回答"MMU 本身开没开成"：
实测品红出现约 6 秒后设备自己重启，说明 `sctlr_el1` 生效、`cpacr_el1`、
`tpidr_el1` 和执行流都没问题。

`v0.2.11` 把复位点换成**调用返回后**（`board.toml` 的
`entry_probe_reset_after = "_arm_vm_init"`、`entry_probe_reset_in = "_arm_init"`、
`entry_probe_reset_occurrence = 1`）：探针在 `arm_init` 第 1 次调用
`_arm_vm_init` 的地方跳进 cave，先真的执行这次调用，等它返回，再等 6 秒复位。
所以这一问是"**在平台专家、CPU 拓扑、Apple 寄存器配置都跑完之后，
`arm_vm_init` 有没有返回**"：

| 现象 | 含义 |
| --- | --- |
| 品红出现后约 6 秒设备自己重启 | `arm_init` 跑到 `_arm_vm_init` 并返回，内核已经建好自己的映射；问题在 `serial_init` / `kernel_startup_initialize_upto` / `machine_startup` 及更后面 |
| 品红一直停着，永不重启 | 卡在 `arm_init` 前段或 `_arm_vm_init` 内部（含它前面那条 `msr PAN, #0x1`）；下一轮把复位点前移，继续二分 |
| 色带下方出现白字 | 走的是 panic 路径，白字就是 panic 的格式串和寄存器快照，直接拍照 |

这次复位是探针主动发的冷复位，不写任何分区，重启后仍从 `boot` 上的固件进菜单。

```sh
fastboot flash cache ../UniXNU/out/clover/payload/payload.img   # 只换这一格，boot 不变
fastboot reboot
```

`boot` 镜像没有变化：固件卷、BootShim 和 DTB 都没动，只有 `cache` 上的 payload
卷换了。`entry_probe_reset_delay` 控制等待秒数；`entry_probe_reset_after` 换成
别的符号（并配 `entry_probe_reset_in` / `_occurrence`）就把复位点挪到别的调用
之后，删掉它则退回 `v0.2.10` 的"开 MMU 后复位"。只想要色带和 panic 屏幕、
不主动重启的话，把 `entry_probe_reset_delay` 删掉重打 payload 即可
（`payload.py` 会改传 `--no-reset`）。

## 有意省略的部分

- **没有块设备驱动。** XNU 有 UFS 的 `NativeUfs`，但没有 eMMC/SDHCI 的，所以
  sheng 那条"直接从 userdata 读 APFS 卷"的路走不通。clover 走 gta4l 的形状：
  控制台优先的内核，带 `CONFIG_XNU_SOURCE_MOCKFS`，没有
  `CONFIG_XNU_NATIVE_UFS`，`[userdata]` 留在板级文件里，等哪天有 SDHCI 驱动。
- **没有 GPU。** `NativeAdreno` 面向 Adreno 740；这块板子是 Adreno 512。
  所以显示栈缺席，`scripts/build-clover-display-stub.py` 只提供
  DEVELOPMENT-only XNU 补丁无条件调用的那一个空符号。首次启动的目标是 UEFI
  菜单和 XNU 控制台，不是 framebuffer。
- **没有触摸。** `edt-ft5406` 没有 UniXNU 驱动，USB 又只有 device 模式，
  所以在有人补上之前，菜单输入只能走调试串口（`blsp1_uart2`，115200）。
- **没有 Data seed。** `[payload.adt]` 声明 `packed_data_ramdisk`，因为 ADT
  必须保留 `PackedRAMDisk` 占位，但 `[payload]` 还没有
  `trust_cache_sha256`，也没有 owner bundle。

## 许可证

BSD-2-Clause-Patent，与 UniXNU 一致。`scripts/build-arm-driver.py` 移植自
UniXNU-gta4l（NoNameBuilder），采用同一许可证。
