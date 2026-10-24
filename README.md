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
| payload / boot.img | 已构建：56 MiB Fat32 payload 卷，以及 kernel 段就是固件的 boot 镜像；payload **不**走 ramdisk（见下文）。`v0.2.13` 起 payload 里的 `kernel.macho` 是一份打了探针的副本：入口色带、panic 屏幕，以及 `arm_init` 某个调用返回后触发的定时复位；`v0.2.16` 起在开 MMU 前调用一个 mapper 洞穴，把 framebuffer 恒等映射进 TTBR0（与窗口同槽就复用 L2 表，否则用窗口最后一页新建一张），并预先填好 `_vinfo`；mapper 失败会涂一条白色诊断带；`v0.2.17` 改成用自身 PC 定位引导页表（`boot_args.physBase` 落在 `__PRELINK_TEXT` 上、本身没有被映射），并在 MMU 打开后立刻涂第 4 条青色带；`v0.2.18` 再加一条独立的橙色带（第 12 条），在 `arm_init` 的第一条调用处直接从 `boot_args` 涂，不依赖栈、AT 和 `_vinfo`；`v0.2.19` 修掉种子守卫写反的 bug（`cbz x8` 在 mapper 成功时跳过种子，`_vinfo` 因此一直是 0，黄带从 `v0.2.15` 起从未画出来过），并在第一条调用返回后再涂一条红带（第 13 条）；`v0.2.20` 修掉 ADT 里 `cluster-type` 的尺寸契约（字符串改为 4 字节整数 `'E'` / `'P'`）；`v0.2.21` 又因为这份内核只支持单簇而移除该属性，让 8 个核落在一个 `CLUSTER_TYPE_SMP` 平坦簇里；`v0.2.22` 关掉 `CONFIG_XNU_NATIVE_PAN` 并重建内核（Kryo 260 是 ARMv8.0，没有 PAN），这是本轮第一次改内核构建配置，探针脚本不变；`v0.2.23` 换回定时信号：`arm_vm_init` 返回后 10 秒复位，panic 屏幕在 framebuffer 不再翻译时 3 秒复位；`v0.2.24` 把复位点移到第二次 `PE_init_platform(TRUE)` 返回后 15 秒，这一段跨过 `serial_init`、`kernel_startup_initialize_upto(KPRINTF)` 和 `PE_create_console`，后者会重新映射 framebuffer，成功的话 panic 屏幕又能画字（见"菜单之后看什么"）；`v0.2.25` 修掉 `PE_create_console` 开头内联的 panic-log 探测（这份 BCM2837 内核读树莓派地址 `0x3c0fc000`，在 clover 上是外部异常），复位点不变；`v0.2.26` 把 `arm_init` 后段五个调用各接一个涂色洞穴（第 6–10 条带），复位点移到 `_machine_startup` 调用处，复位洞穴先涂第 11 条红带再等 15 秒；`v0.2.27` 把刻度带移到第 10–14 条、控制带移到第 15 条（都在 panic 屏不清的下半屏），并让 panic 屏印出 RA/FP/LR、fmt/args 和 va_list 前后各槽，用于从照片定位断言调用点；`v0.2.28` 修掉刻度洞穴的 4 字节对齐缺陷（第 14 条洞穴整体左移、`bl` 落到 `_fiq_context_init - 4`，`v0.2.27` 的白字是探针自己的误报），并在打洞时校验每条 `_start` 与每个 `bl` 目标；`v0.2.29` 把检查点从 `_machine_startup` 调用前推进到交接之后：第 15 带表示进入 `_machine_startup`，随后 9/8/7/6 带依次覆盖 `_machine_conf` 返回、`_kernel_bootstrap` 入口、首条 `printf` 返回和 `vm_mem_bootstrap` 调用点，复位也移到最后一档 |
| 固件 | 由 `uefi-clover` 构建（3 MiB 固件卷，内含 UniXNU 与 DarwinBoot）；`v0.3.10` 起 DXE 尾部完整派发，`CloverGopDxe` 发布 GOP |
| 设备事实 | 2026-10-22 采集过一次，位于 `facts/81ffcaaa-20261022-210928/` |
| 真机启动 | **菜单已到**（Stage 1）：`v0.3.10` 固件在屏幕上渲染 UniXNU 菜单并读到了 payload；菜单 3 秒后默认进 XNU。`v0.2.9` 实测屏幕出现**绿 + 蓝 + 品红**三条色带，即 DarwinBoot 已交接、XNU 入口已执行、引导页表已建好；`v0.2.10` 实测**品红出现约 6 秒后设备自己重启**，即 `sctlr_el1` 打开 MMU 后没有立刻死掉；`v0.2.11` 的 `_arm_vm_init` 复位点没有触发（**品红一直停着**），说明还没走到它；`v0.2.12` 实测**只有绿 + 蓝 + 品红、约 6 秒后自复位**，即 `_PE_init_platform` 返回了、开 MMU 后写屏确实不可用；`v0.2.13` 的 `_thread_bootstrap` 复位点实测是**品红后十几秒才复位、没有白字**，符合"panic 屏幕在 `vinfo` 仍为 0 时走了 12 秒兜底"；`v0.2.14` 扩映射并预填 `_vinfo`、不再主动复位，实测**仍是绿 + 蓝 + 品红，无黄带无白字**，由此定位到窗口在 framebuffer 之上、向上扩 `memSize` 永远够不到；`v0.2.15` 改为直接补页表项，实测**仍是绿 + 蓝 + 品红、无黄无白**，原因是 1 GiB 窗口只能落在高 DRAM bank，与 framebuffer 不在同一个 1 GB 槽，旧代码查错了槽位就退出；`v0.2.16` 换成独立 mapper 洞穴（同槽复用、异槽新建 L2 表），失败时用白色带自报错误码，实测**绿 + 蓝 + 品红 + 第 9 条白带**，即 mapper 走到自检第 4 步：窗口的第一个 L2 项不是 2 MB 块；由此查明 `boot_args.physBase` 锚在 `__PRELINK_TEXT` 上、本身不在引导映射内；`v0.2.17` 改为从 mapper 自己的 PC 反推引导映射，修掉复用 L2 表时未掩掉低位的 bug，并在 MMU 打开后立刻涂第 4 条青色带，实测**绿 + 蓝 + 品红 + 青、无黄**，即 MMU 已开、framebuffer 已经可写，卡点在 `_cpu_data_init` 被调用之前；`v0.2.18` 增加第 12 条橙色带（`arm_init` 入口后的第一条调用，不依赖 vinfo/栈/AT），实测**绿 + 蓝 + 品红 + 青 + 橙、无黄**，确认 `arm_init` 已进入、第一条调用点已到达；由此回查代码发现种子守卫从 `v0.2.15` 起就是反的（`cbz x8` 在 mapper 成功时跳过种子），`_vinfo` 一直为 0，黄带从未有机会出现；`v0.2.19` 改成 `cbnz x8`，并让第一条调用返回后再涂第 13 条红带，实测读到**白字 panic 屏幕**：`CPU property 'cluster-type' has bad size 6 @machine_routines.c:936`，即种子/画屏/早段 `arm_init` 全通，内核已走到 `ml_parse_cpu_topology`；`v0.2.20` 把 `cluster-type` 从字符串改成 4 字节整数 `'E'` / `'P'`，实测接着触发 `machine_routines.c:1099` 的 `phys_cluster_id <= MAX_CPU_CLUSTER_PHY_ID` 断言——这份 BCM2837 内核只支持单簇，无法接受 E/P 双簇描述；`v0.2.21` 改为不发布 `cluster-type`，让 8 核按 `CLUSTER_TYPE_SMP` 建一个平坦簇，实测拓扑已通过，接着读到 `Undefined kernel instruction: pc=0xfffffff0079aff58 instr=d500419f`——`msr pan, #1`，Kryo 260 是 ARMv8.0，没有 PAN；`v0.2.22` 把 `CONFIG_XNU_NATIVE_PAN` 改成 `n` 并重建内核，实测七条带全部出现（绿 + 蓝 + 品红 + 黄 + 青 + 橙 + 红）、没有白字：内核进了 `arm_init`，两个早段探针都返回了，之后死在 `bl _arm_vm_init` 处或更晚，而且没有可见的 panic；`v0.2.23` 因此把信号换回定时复位，并让 panic 屏幕在 framebuffer 不可翻译时也能报出来，实测**色带出现后约 10 秒自复位**，即 `arm_vm_init` 返回了，卡点在它后面；`v0.2.24` 把复位点移到 `PE_create_console` 之后的第二次 `PE_init_platform(TRUE)`，实测**色带之后什么都没有**——既没有文字，也没有 15 秒复位，卡点在 `arm_vm_init` 返回之后、`PE_init_platform` 之前；`v0.2.25` 据此跳过 `PE_create_console` 开头读树莓派 panic log 地址的四条指令，实测**色带 + 约 15 秒复位、无文字**，即控制台与第二次 `PE_init_platform` 都已返回（无文字是 `gc_enabled` 仍为假所致，不是故障）；`v0.2.26` 给随后五个调用各涂一条带（6–10）并把复位点推到 `_machine_startup` 之前；实测**绿 + 蓝 + 白字**（`<1>:0 Assertion failed: <d>`、`TCR=0x…1a251a`、`pages=free=0`），即 `arm_init` 后段有断言 panic、发生在 VM 页初始化之前，但旧清屏把第 2 条以下的带全擦掉、参数又与已知冷段常量对不上；`v0.2.27` 因此改印 RA/FP/LR 与原始参数槽；其实机读数（绿 + 蓝 + 白字、`LR=0xfffffff0073f1dc4`、`a0=1 a1=0 a2=0xd`）指向的不是内核断言，而是第 14 条刻度洞穴自己：它被放在 `0x3ed784`（4 mod 8），`ld.lld` 把 `_start` 对齐到 `0x3ed788`，而 patcher 仍按 `0x3ed784` 写入，整段 `bl` 左移 4 字节落到 `_fiq_context_init - 4`（`_platform_syscall` 冷段里的 `bl _Assert`），`x0/x1/x2` 还留着 `arm_init` 传给 `_fiq_context_init` 的 `TRUE/0/0xd`；`v0.2.28` 修掉打洞对齐并逐条校验 `bl` 目标，第 10–13 条带则证明那四个调用确实都返回了；`v0.2.28` 上机读数是**10–14 全在 + 15 红 + 约 15 秒复位**，即 `_fiq_context_init` 也返回了、`arm_init` 已到 `_machine_startup` 调用点，交接之前没有任何卡死；`v0.2.29` 因此让检查点真正进入 `_machine_startup`，把刻度推进到 `kernel_bootstrap` 的 `vm_mem_bootstrap` 调用处（round59，待上机） |

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
SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" ./unixnu build kernel   # macOS + Xcode
./unixnu build package
./unixnu build payload --unique-chip-id 0x<device seed> --uefi ../uefi-clover
./unixnu build boot
```

Xcode 27 的 clang 在设备仓重建归档时不带 `-isysroot`，会拿 macOS SDK 去编译 iOS
目标，并把 `-Wincompatible-sysroot` 当成错误；上面显式指定 iPhoneOS SDK 就是为了
绕开它。只重打 payload 不需要这一步。

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

`v0.2.10` 的复位点在 `msr sctlr_el1, x0; isb` 之后，回答"MMU 本身开没开成"：
实测品红出现约 6 秒后设备自己重启，说明 `sctlr_el1` 生效、`cpacr_el1`、
`tpidr_el1` 和执行流都没问题。

`v0.2.11` 把复位点换成**调用返回后**（`entry_probe_reset_after = "_arm_vm_init"`）：
真机结果是**品红一直停着、不重启**，也就是 `_arm_vm_init` 没走到。`v0.2.12` 把
复位点前移到 `_PE_init_platform` 之后，并在 `_cpu_data_init` 调用点试画一条黄带。
真机读数是**只有绿 + 蓝 + 品红，约 6 秒后自复位**：复位照常发生，说明
`_PE_init_platform` 确实返回了；黄带没出现，说明开 MMU 之后这块屏暂时画不了。

原因在 `v0.2.14` 的读数之后查清了：DarwinBoot 的窗口分配在"能放下它的空闲段"里，
而这块板子 `0x9D400000` 以下是 "Kernel"/SMEM/148 MiB 的 DXE Heap、再往下就是
`0x9D400000 + 0x02400000` 的显示预留和固件自己那几段；唯一够大的空闲从
`0xA0000000` 起。所以窗口（`boot_args.physBase`）落在 framebuffer **上面**，而
`_start_first_cpu` 是从 physBase **向上**映射 memSize 字节——扩 memSize 只能往下
"长"不到 `0x9D400000`。旧 `GROW` 在 `fb_end <= physBase` 时整段跳过，种子却照写，
panic 屏幕再去读一个未映射的 baseaddr 就会 fault：既没有黄带，也没有白字，正好是
`v0.2.14` 的实测。`v0.2.13` 的 12 秒兜底同样说明 MMU 之后确实 panic 过一次，
只是当时画不出来。

`v0.2.15` 直接补页表，仍然**不主动复位**，但实测**还是绿 + 蓝 + 品红、无黄无白**。
原因读代码就能确定：它假设窗口和 framebuffer 在同一个 1 GB L1 槽里，于是拿
framebuffer 的槽号去查 L1，再看那个槽的 L2 表。可是 `0x10000000`（256 MiB）以上的
窗口才能放下 1 GiB 的 `darwinboot-memory-size`，这块板子唯一的候选在高 DRAM bank
（L1 槽 4），framebuffer 在槽 2；`l1[2]` 是无效项，mapper 在第一个自检就退出，
`x8` 保持 0，种子被跳过，`_vinfo` 仍为 0，`AT` 查不到映射就跳过黄带——既没有黄带
也没有白字，`--no-reset` 下就停住。

`v0.2.17` 修正 mapper 的定位方式，仍然**不主动复位**：

1. **蓝带**：入口，MMU 关闭，和以前一样。
2. **品红带**：涂完之后、`msr sctlr_el1, x0` 之前（MMU 仍关），`bl` 进
   `entry + 0x600` 的 mapper 洞穴（`lr` 存在 `x16`，不用栈——此时 `sp` 是高地址
   KVA）。
   - **不再读 `boot_args.physBase`**。这个字段等于 DarwinBoot 分配窗口的地址，
     而内核 Mach-O 里 `__PRELINK_TEXT` 的 vmaddr 比 `__TEXT` 低约 0x2D88000，
     所以 `Kernel.Base` 取的是 prelink 段的地址，`DarwinBoot` 把窗口起始交给它，
     `_mh_execute_header` 落在窗口起始 + 0x2D88000。`start.s` 的引导映射是从
     `_mh_execute_header` 向上建的，**physBase 本身没有被映射**——`v0.2.16` 从
     `[x20, #0x10]` 取窗口基址，读到的是块映射以下的空地，所以自检第 4 步失败。
   - `v0.2.17` 改用 `adr x14, .` 取 mapper 自己运行时的物理 PC（`adr` 是 PC 相对
     的，MMU 关着时就是 PA），从这里向下取 L1 表基址，再确认窗口 L1 项是表、
     其第一个 L2 项是 2 MB 块，把这个块当属性模板。这条路径不依赖任何 boot_args
     字段，而且一定落在已映射区间内。
   - framebuffer 所在槽若已有 L2 表（与窗口同槽）：先把 L1 项的有效位/类型位掩掉
     得到表基址（`v0.2.16` 漏了这一步，复用路径一走到就会非对齐访问），再把缺的
     恒等块补进这张表；
   - 若是 1 GB 块：整槽已映射，直接算成功；
   - 若是无效项（通常情况）：拿窗口最后一页做一张新 L2 表（DarwinBoot 保证窗口顶部
     至少留 2 MB，所以这页是空闲的），清零后写满 framebuffer 的恒等块，再挂到
     framebuffer 的 L1 槽上；
   - 写完 `dsb` + `tlbi vmalle1`，MMU 打开后这块屏就在 V=P 里了。
3. **白色诊断带**：mapper 用 `x8` 返回 0（成功）或失败码；失败时品红洞穴在 MMU 打开
   **之前**涂一条白带，位置是第 `5 + 码` 条 1/16 带。失败码：

   | 码 | 含义 |
   | --- | --- |
   | 1 | `boot_args.Video.base` 为空 |
   | 2 | `rowBytes * height` 为 0 |
   | 3 | 窗口的 L1 项不是表 |
   | 4 | 窗口的第一个 L2 项不是 2 MB 块（拿不到属性模板） |
   | 5 | 保留的 L1 描述符，或窗口最后一页不够空闲 |
   | 6 | 目标 L2 项已经被占用（不是我们写的恒等映射） |

4. **种子**：`x8 == 0` 才把 `boot_args.Video` 的几何写进内核自己的 `_vinfo`
   （MMU 未开，按物理地址写，不压栈），`v_type` 放 `0xC10C10C1`。映射失败时
   `_vinfo` 保持 0，panic 屏幕走 12 秒兜底而不是 fault。
5. **青带（新增，第 4 条）**：`msr sctlr_el1` + `isb` 之后立刻按同样的几何涂第 4 条
   1/16 带，颜色是青色（`0xFF00FFFF`，BGRA），涂完 `dc civac` + `dsb sy` 再跳去
   `target`。它和 mapper 成功标志、`Video` 的几何绑定：只有"MMU 已开、framebuffer
   V=P 可写"才会出现。这样青带把"开 MMU 这一刻"和之后的 `_cpu_data_init` 分开：
   有青无黄 = 卡在 `start.s` 到 `arm_init` 之间；再配合下一轮的橙带，可以把
   "没进 `arm_init`"和"进了但没走到调用点"区分开。
6. **黄带**：`arm_init` 第 1 次调用 `_cpu_data_init` **之前**，按 `_vinfo` 涂第 3 条
   带（洞穴先涂色，再执行被挪走的 `bl`）。涂之前用 `AT S1E1W` 查一次翻译
   （失败就跳过），涂完用 `dc civac` + `dsb sy` 把 Normal-WB 的像素推到显示引擎可见。

`v0.2.17` 上机的结果是**绿 + 蓝 + 品红 + 青，无黄**；`v0.2.18` 加橙带后再测，读数是
**青 + 橙，仍然无黄**。橙带出现说明 `arm_init` 确实进了、第一条调用点也到了；黄带
缺席则一直按"没走到 `_cpu_data_init`"来读。

`v0.2.19` 查出了真正的原因：**种子守卫写反了**。`v0.2.15` 给 `SEED` 加的守卫是
`cbz x8, 9f`——mapper 成功时 `x8 == 0`，这条指令恰好跳过整个种子，`_vinfo` 一直是
0。黄带画笔读 `_vinfo.v_baseaddr` 得到 0，直接跳过，所以从 `v0.2.15` 起黄带从未
有机会出现。它还有一个反向的隐患：mapper 失败时（`x8 != 0`）种子反而会运行，把
未映射的 framebuffer 写进 `_vinfo`，panic 屏幕照着画就会 fault，而不是走 12 秒兜底。
`v0.2.19` 把它改成 `cbnz x8, 9f`。

同时 stage 4 的洞穴变成两段涂色：调用前涂**橙带 12**（证明调用点到了），
`bl _arm_set_kernel_tbi` 返回后再从 `x19`（`arm_init` 在入口保存的 boot_args，
callee-saved）涂**红带 13**（`0xFF0000FF`，证明调用真的返回了）。这样一次上机就能把
"没进 `arm_init` / 没从第一条调用返回 / 到了调用点"三件事分开。

`v0.2.19` 上机后第一次读到了**白字 panic 屏幕**（panic 屏幕按设计从第 `h/8` 行往下
清屏，第 2 条以下的色带都被清掉，所以只剩绿 + 蓝是正常的）：

```text
CPU property 'cluster-type' has bad size 6 @machine_routines.c:936
```

这一屏同时证明了三件事：种子修好之后 `_vinfo` 可用、panic 屏幕能照着它画、内核已经
走到 `_arm_init` 里的 `ml_parse_cpu_topology`（`_cpu_data_init`、`_PE_init_platform`
都已经返回）。panic 的原因是 ADT 契约错误：生成器把 `cluster-type` 写成了字符串
`"ECore"` / `"PCore"`（6 字节，含结尾 0），而 `ml_readprop()` 只接受 1/2/4/8 字节
整数，拓扑代码再把值跟 `'E'` / `'P'` 比较。`v0.2.20` 把每个 `cpuN` 的
`cluster-type` 改成 4 字节整数——cpu0–3 是 `'E'`，cpu4–7 是 `'P'`——并在
`check-clover.py` 里加了断言，防止它再退化成字符串。内核补丁没有任何变化，只有
payload 里的 `kernel.macho.adt` 换了（`kernel.macho` 的哈希与 `v0.2.19` 相同）。

`v0.2.20` 上机后 panic 前进了一步，但仍然停在拓扑里：

```text
machine_routines.c:1099 Assertion failed: phys_cluster_id <= MAX_CPU_CLUSTER_PHY_ID
```

这次不是属性尺寸，而是**簇的数量超出了内核的模型**。这个内核是
`DEVELOPMENT_ARM64_BCM2837`：`board_config.h` 给它 `MAX_CPU_CLUSTERS 1`，也没有
`HAS_CLUSTER`。于是拓扑代码用 `(cluster_type == 'P')` 反推物理簇号，'P' 得到 1，
而允许的最大值是 0，断言在第一个性能核上失败。一个只支持单簇的内核无法接受
"4 个 E + 4 个 P"的描述。

`v0.2.21` 因此不在 ADT 里发布 `cluster-type`：`ml_readprop` 返回默认值 0，即
`CLUSTER_TYPE_SMP`，8 个核落在一个平坦的簇里——这正是这份 Pi 3 内核在没有 ADT 时
的默认状态，也是它 `MAX_CPU_CLUSTERS 1` 能表示的拓扑。要如实描述 Kryo 260 的双簇，
得把内核按 `MAX_CPU_CLUSTERS >= 2` 重新编译，那是后面独立的一轮；眼下先把拓扑搭
起来。`check-clover.py` 的断言也改成"要么没有该属性，要么是 4 字节 `'E'`，`'P'`
会被拒绝"，把这份内核的限制写进检查里。

`v0.2.21` 上机后又前进了一步，而且这次拓扑确实过了：

```text
Undefined kernel instruction: pc=0xfffffff0079aff58 instr=d500419f @ sleh.c:1045
```

`instr=d500419f` 就是 `msr pan, #1`。`ml_parse_cpu_topology` 返回后，`arm_init`
继续执行到 `arm_vm_init` 之前的 PAN 设置就同步异常了。Kryo 260（Cortex-A73/A53
派生）是 ARMv8.0，没有 `PSTATE.PAN`，这条指令在硬件上就是未定义的。根因是
`configs/clover_defconfig` 沿用了 sheng 的 `CONFIG_XNU_NATIVE_PAN=y`；那是给
ARMv8.1 的 SM8550 开的。

`v0.2.22` 把它改成 `n` 并重建内核：旧镜像里的 15 条 `msr pan` 全部消失，三份曾用
`-march=armv8.1-a` 编译的源文件（`copyio.c`、`pcb.c`、`platform_tests.c`）也回到
v8.0。这是本轮第一次改内核构建配置；探针脚本和 ADT 都没有变化，但 `kernel.macho`
的哈希换了，所以 payload 里的内核是重新编译的版本。重建内核时还要给 Xcode 27 的
clang 显式指定 iPhoneOS SDK（见上面的构建命令）。

`v0.2.22` 上机后七条带全中、没有白字：内核进了 `arm_init`，`_arm_set_kernel_tbi`
返回了，`_cpu_data_init` 调用点也到了，随后画面一直停在菜单加色带。紧挨 PAN 的
下一条指令就是 `bl _arm_vm_init`，所以执行已经进入 `arm_vm_init` 或更后面。到
这里色带帮不上忙了：`arm_vm_init` 中途会执行 `set_mmu_ttb(invalid_ttep)`（源码
注释："From here on out, we're off the bootstrap translation tables"），TTBR0 换成
内核自己的页表，framebuffer 的恒等映射随之消失。之后任何涂色都会被 `AT` 守卫
静默跳过；更糟的是 panic 屏幕会照着种子里的物理地址直接写，panic 若发生在切换
之后，第一笔就 fault，于是既没有白字也没有复位。

`v0.2.23` 因此把信号换回时间，并让 panic 也说得出口：panic 屏幕先用
`AT S1E1W` 查一次基址，查不通就 **3 秒后 PSCI 复位**；另外在 `_arm_init` 的
`bl _arm_vm_init` 处放一个复位洞穴，真正执行完这次调用后 **10 秒复位**。

上机读数是**七条带都出现、约 10 秒后设备自己复位**：10 秒正是那个复位洞穴，
所以 `arm_vm_init` 返回了，卡点在它后面。它之后依次是 `patch_low_glo`、
`PE_consistent_debug_inherit`、`serial_init`、`kernel_startup_initialize_upto(KPRINTF)`、
`PE_create_console`、`PE_init_printf`、`dump_kva_space`、`cpu_machine_idle_init`、
`PE_init_platform(TRUE, &BootCpuData)`。其中 `serial_init` 在 ADT 里找不到
`gpio` / `aux` 节点，注册不了串口便返回 0；**`PE_create_console` 是屏幕可能回来的
地方**：`initialize_screen` 此时看到 `kernel_map == VM_MAP_NULL`，会用
`ml_io_map_unmappable()` 把 `boot_args.Video` 重新映射，把新 KVA 写进
`vinfo.v_baseaddr`，之后 panic 屏幕又能画字。但它的第一件事 `check_for_panic_log`
在这份 BCM2837 内核里写死了 `PLATFORM_PANIC_LOG_PADDR = 0x3c0fc000`（树莓派的
地址）；在 clover 上读它就是这一轮要排除的嫌疑：外部异常发生时 framebuffer
还没重新映射，panic 屏幕也画不出来。

`v0.2.24` 因此把复位洞穴放到**第二次 `PE_init_platform(TRUE, &BootCpuData)`
返回后 15 秒**：它在 `PE_create_console` 之后，既能证明控制台建出来了，也给
`kprintf` 的屏幕输出留出拍照时间。

上机读数是**色带之后什么都没有**：既没有文字，也没有 15 秒复位。复位洞穴没响，
说明卡点在 `arm_vm_init` 返回之后、第二次 `PE_init_platform` 之前。把这一段逐个
过一遍，只剩一个具体的怀疑对象：

- `kernel_startup_bootstrap()` 在 `arm_vm_init` 之前就把所有 `STARTUP_SUB_LOCKS`
  及以下的入口排好并跑完了，所以后面的
  `kernel_startup_initialize_upto(STARTUP_SUB_KPRINTF)` 只会跑一个入口
  `PE_init_kprintf`，它也就是再调一次 `serial_init`——ADT 里没有 `gpio` / `aux`
  节点，立刻返回 0。
- 控制台之前的 `kprintf` 是屏幕空操作：`cons_ops_index` 是 `VC_CONS_OPS`，
  `vcputc_options` 在 `gc_initialized` 为假时直接返回；`log_lock` 和
  `kprintf_lock` 都有 LOCKS 阶段的初始化。
- 剩下的就是 `PE_create_console`。它开头的内联 `check_for_panic_log` 在这份
  BCM2837 构建里被编译成读**物理地址 `0x3c0fc000`**（`PLATFORM_PANIC_LOG_PADDR`，
  树莓派的地址）。clover 的 DRAM 从 0x80000000 起，这个地址在它下方约 1 GiB，
  第一条 load 就会外部异常；而此时视频控制台还不存在，panic 屏幕画不出任何东西，
  正好就是"色带之后什么都没有"。

`v0.2.25` 因此由探针脚本把 `PE_create_console` 开头那四条指令

```text
mov w19, #0x4000        ; panic log 大小
mov w0, #0x3c0000
movk w0, #0x3c0f, lsl #16   ; 0x3c0fc000
mov w1, #0x4000
```

改写成

```text
mov w20, #1
b   +0x94               ; 直接到 initialize_screen 之前的 gPESF 判断
nop
nop
```

跳过映射、读 magic 和 `bzero`。`w20 = 1` 是正常路径在映射之后才写的值，
`cinc w1, w20, eq` 会拿它当视频模式参数；`gPanicBase` 保持为 0，正是
`debug_log_init()` 已经能处理的状态。

上机读数是**色带 + 约 15 秒复位、无文字**：复位洞穴响了，说明跳过生效——
`PE_create_console` 返回了、视频控制台已经建立，第二次 `PE_init_platform`
也返回了。没有文字是预期结果，不是故障：`initialize_screen` 只把
`gc_initialized` 置真，`gc_enabled` 仍为假（只有 `vcattach` → `vc_enable`
才会置真），`vcputc_options` 要两者都为真才画字；而 `arm_init` 调的是
`PE_init_printf(FALSE)`，并不会 attach。所以这一段不能用 `kprintf` 当刻度，
复位点继续往后推。

`v0.2.26` 用从 `v0.2.19` 起一直可靠的 vinfo 画笔，给 `arm_init` 剩下的这段
做逐调用刻度。第二次 `PE_init_platform` 之后的五个调用各配一个独立洞穴：
执行调用、涂一条带、跳回原处。

| 色带 | 调用 |
| --- | --- |
| 6 品红 | `_ml_map_cpu_pio` |
| 7 青 | `_cpu_timebase_init` |
| 8 橙 | `_kptimer_curcpu_up` |
| 9 绿 | `_PE_init_cpu` |
| 10 白 | `_fiq_context_init` |

复位点移到 `_machine_startup` 调用处（`--reset-before`，不执行调用），它的
洞穴先涂第 11 条红带再等 15 秒复位。只有 11 出现时，缺失的 6–10 才能归咎于
对应调用。stage 4 被复位占用，所以橙带 12 和红带 13 这一轮不画；黄带仍在
`_cpu_data_init` 调用点。

| 现象 | 含义 |
| --- | --- |
| 6–10 全出现 + 11 红 + 约 15 秒复位 | 五个调用全部返回，卡点在 `machine_startup` 内部 |
| 6–10 里最后一条缺失、11 出现 | 那条调用没有返回 |
| 11 红也不出现 | 复位洞穴没到，或它也画不出来（vinfo / AT 失效） |
| 色带 + **白字** + 不复位 | 控制台之后发生 panic，屏幕已可画，直接拍照 |

```sh
fastboot flash cache ~/clover-out/round56-payload.img   # 只换这一格，boot 不变
fastboot reboot
```

`boot` 镜像没有变化：固件卷、BootShim 和 DTB 都没动，只有 `cache` 上的 payload
卷换了。黄带探针（stage 3，`_cpu_data_init` 调用点）和四条刻度带都是设备仓
脚本里的默认值，基础仓不需要任何本地改动；`patch-xnu-entry-probe.py --no-paint`
可以关掉黄带，`--no-trace` 关掉刻度带。`board.toml` 的
`entry_probe_reset_delay = 15`、`entry_probe_reset_after = "_machine_startup"`、
`entry_probe_reset_before = true` 和 `entry_probe_reset_occurrence = 1` 把
stage 4 洞穴变成"在调用 `machine_startup` 之前定时复位"；删掉这几项、payload
就会退回 `--no-reset` 的纯涂色模式。

### v0.2.27：让 panic 屏印出调用者

`v0.2.26` 实机照片是**绿 + 蓝 + 白字**：

```
<1>:0 Assertion failed: <d>
TCR=0x0000000022a51a251a
SCTLR=0x0000000034d4593d
pages=0x0000000000000000
free=0x0000000000000000
```

第 2 条以下的色带消失是 panic 屏自己清的（它原本从 `h/8` 一直清到 `h`）；
`pages=free=0` 说明 panic 发生在 VM 页初始化之前，也就是 `arm_init` 后段 /
`machine_startup` 这条线上。`<1>` 和 `<d>` 不是字面量，而是格式器对"读不了的
指针"的写法，真实参数是 `0x1` 和 `0xd`。矛盾在于：这份内核里
`"%s:%d Assertion failed: %s"` 只被 `_Assert` 引用，而 `_Assert(file, line,
expression)` 的冷段调用点传的都是常量（`kptimer.c` 575、
`machine_routines.c` 2404 等），与 `0x1`/`0xd` 对不上——单凭这张照片无法判断
是谁调的断言。

因此 `v0.2.27` 让 panic 屏自己交代调用链：

| 行 | 含义 |
| --- | --- |
| `RA=` | 进屏时的 LR（`_panic` 钩子路径下是 `_Assert` 内的返回地址） |
| `FP=` | 调用者的帧指针 |
| `LR=` | `[FP+8]` 的保存值，即调用 `_Assert` 的那个函数里的返回地址——断言现场 |
| `fmt=` / `args=` | 格式串地址和 va_list 指针 |
| `m2` `m1` `a0`–`a5` | va_list 前后各槽的原值；每槽先做 AT S1E1R 查询，读不了就印 `----` |

清屏范围同时缩到 `[h/8, h/2)`，刻度带挪到 10–14（白、橙、绿、青、品红），复位
控制带挪到 15（红）。它们都在下半屏，panic 之后仍能与白字出现在同一张照片里。

| 色带 | 调用 |
| --- | --- |
| 10 白 | `_ml_map_cpu_pio` |
| 11 橙 | `_cpu_timebase_init` |
| 12 绿 | `_kptimer_curcpu_up` |
| 13 青 | `_PE_init_cpu` |
| 14 品红 | `_fiq_context_init` |
| 15 红 | 复位控制（`_machine_startup` 调用前） |

| 现象 | 含义 |
| --- | --- |
| 10–15 全在 + 白字 | panic 在 `_fiq_context_init` 返回之后、`_machine_startup` 之前 |
| 10–15 里最后一条缺失 + 白字 | 那条带对应的调用内部 panic，`LR=` 就是断言现场 |
| 只有白字、没有 10–15 | panic 在第一个被跟踪调用之前 |
| 10–15 全在 + 约 15 秒复位 | 没有 panic，卡点在 `machine_startup` 内部 |

```sh
fastboot flash cache ~/clover-out/round57-payload.img
fastboot reboot
```

### v0.2.28：白字是探针自己撞出来的

`v0.2.27` 实机照片是**绿 + 蓝 + 白字**，panic 屏把调用链交代得很完整：

```
<1>:0 Assertion failed: <d>
RA=0xfffffff00797a3b0
FP=0xfffffff007baff20
LR=0xfffffff0073f1dc4
fmt=0xfffffff007047677
args=0xfffffff007baff00
m2=0xfffffff0071568d0  m1=0x0000000000000000
a0=0x0000000000000001  a1=0x0000000000000000
a2=0x000000000000000d  a3=0x0000000000000000
a4=0xfffffff007bafff0  a5=0xfffffff0073f1dc4
```

`RA` 是 `_Assert` 里的返回地址，`fmt` 是真实的断言格式串，说明断言路径本身
没错；矛盾在参数。`"%s:%d Assertion failed: %s"` 在这份内核里只有 `_Assert`
引用，而 `_Assert(file, line, expression)` 的调用点传的三个参数是
`(文件名, 行号, 表达式)`，全部是编译期常量；`0x1 / 0 / 0xd` 既不是文件名，
也不是行号。换句话说：**panic 是真的，但它不是内核代码主动调的 `_Assert`。**

于是这一轮先反汇编探针，而不是内核。结果如下：

- 第 14 条刻度洞穴（`_fiq_context_init`）被放在 `0xfffffff0073ed784`，地址
  4 mod 8；`ld.lld` 会把 `.text` 向上对齐，`_start` 实际落在 `0x3ed788`，
  而 patcher 仍按 `0x3ed784` 写入二进制。
- 于是整段 cave 里所有 PC 相对分支都低了 4 字节：`bl _fiq_context_init`
  落到 `_fiq_context_init - 4`，正好是 `_platform_syscall` 冷段里那条
  `bl _Assert`；返回指令 `b site+4` 也退回到调用点本身。
- `x0/x1/x2` 此时仍是 `arm_init` 传给 `_fiq_context_init` 的参数
  `TRUE / 0 / 0xd`，与照片里 `a0/a1/a2` 完全吻合；`LR=0x3f1dc4` 是
  `_Assert` 保存在 `[FP+8]` 的返回地址，也就是"调用者"被认成了
  `_fiq_context_init`。
- 第 10–13 条洞穴都是对齐的，`bl` 与返回目标都正确，所以
  `_ml_map_cpu_pio`、`_cpu_timebase_init`、`_kptimer_curcpu_up`、
  `_PE_init_cpu` **确实都返回了**；`v0.2.27` 那张照片对
  `_fiq_context_init` 和 `_machine_startup` 没有提供任何信息。

`v0.2.28` 在 patcher 里加了三道保险：`assemble()` 用 `llvm-nm` 要求
`_start` 正好等于请求的 VA；刻度洞穴的放置改为先对齐到 16 字节，且对齐后
仍要放得进同一段零填充；写入前解码 `bl`，目标必须等于符号表里的 callee。
五条洞穴现在都从 16 字节边界开始。刻度带、复位点、panic 屏内容都不变：

| 色带 | 调用 |
| --- | --- |
| 10 白 | `_ml_map_cpu_pio` |
| 11 橙 | `_cpu_timebase_init` |
| 12 绿 | `_kptimer_curcpu_up` |
| 13 青 | `_PE_init_cpu` |
| 14 品红 | `_fiq_context_init` |
| 15 红 | 复位控制（`_machine_startup` 调用前） |

| 现象 | 含义 |
| --- | --- |
| 10–14 全在 + 15 红 + 约 15 秒复位 | 五个调用全部返回，卡点在 `machine_startup` 内部 |
| 10–14 里最后一条缺失、15 出现 | 那条调用没有返回 |
| 10–13 + 白字 `machine_routines.c:2404` | `_fiq_context_init` 里中断状态不对，断言现场就是它 |
| 15 红也不出现 | 复位洞穴没到，或它也画不出来（vinfo / AT 失效） |

```sh
fastboot flash cache ~/clover-out/round58-payload.img
fastboot reboot
```

### v0.2.29：跨过交接点

`v0.2.28` 实机读数是**第 10–14 条带全在 + 第 15 条红带 + 约 15 秒复位**。
五条刻度带都是"调用返回后才涂"，所以 `_ml_map_cpu_pio`、`_cpu_timebase_init`、
`_kptimer_curcpu_up`、`_PE_init_cpu`、`_fiq_context_init` 全部返回了；红带出现
说明 `arm_init` 走到了 `_machine_startup` 调用点，15 秒后复位也正常。到这一刻
为止，交接点之前没有任何卡死——问题只能在 `_machine_startup` 里面。

`_machine_startup` 只有两个动作：`bl _machine_conf`，然后 `b _kernel_bootstrap`。
因此 `v0.2.29` 把检查点从"调用前复位"改成"真正调用"，并把复位挪到
`kernel_bootstrap` 里：

| 色带 | 含义 |
| --- | --- |
| 15 红 | `arm_init` 到达交接点，进入 `_machine_startup` |
| 9 绿 | `_machine_conf` 返回 |
| 8 橙 | `_kernel_bootstrap` 已进入（在它的第一条 `printf` 之前涂） |
| 7 青 | 那条 `printf` 返回 |
| 6 白 | 到达 `vm_mem_bootstrap` 调用点；涂完等 15 秒复位 |

每一档都只在上一次调用返回之后才涂，所以**最高的缺失色带就是没返回的那个调用**。
第 10–14 条刻度带保留，读数可与 `v0.2.28` 直接对照。

| 现象 | 含义 |
| --- | --- |
| 10–14 + 15 + 9 + 8 + 7 + 6 + 约 15 秒复位 | 一路走到 `vm_mem_bootstrap` 调用点；卡点在它内部 |
| 10–14 + 15 + 9 + 8 + 7，无 6、无复位 | 首条 `printf` 已返回，卡在它之后、`vm_mem_bootstrap` 之前（`_scale_setup`、`kernel_bootstrap_log`） |
| 10–14 + 15 + 9，无 8 | `_machine_conf` 返回后、进入 `kernel_bootstrap` 前卡住 |
| 10–14 + 15，无 9 | 卡在 `_machine_conf` 内部 |
| 10–14，无 15 | 连交接点都没到（与 `v0.2.28` 读数矛盾，说明探针自身出问题） |
| 出现白字 | panic 屏工作，直接拍照 |

```sh
fastboot flash cache ~/clover-out/round59-payload.img
fastboot reboot
```

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
