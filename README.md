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
| payload / boot.img | 已构建：56 MiB Fat32 payload 卷，以及 kernel 段就是固件的 boot 镜像；payload **不**走 ramdisk（见下文）。`v0.2.13` 起 payload 里的 `kernel.macho` 是一份打了探针的副本：入口色带、panic 屏幕，以及 `arm_init` 某个调用返回后触发的定时复位；`v0.2.16` 起在开 MMU 前调用一个 mapper 洞穴，把 framebuffer 恒等映射进 TTBR0（与窗口同槽就复用 L2 表，否则用窗口最后一页新建一张），并预先填好 `_vinfo`；mapper 失败会涂一条白色诊断带；`v0.2.17` 改成用自身 PC 定位引导页表（`boot_args.physBase` 落在 `__PRELINK_TEXT` 上、本身没有被映射），并在 MMU 打开后立刻涂第 4 条青色带；`v0.2.18` 再加一条独立的橙色带（第 12 条），在 `arm_init` 的第一条调用处直接从 `boot_args` 涂，不依赖栈、AT 和 `_vinfo`（见"菜单之后看什么"） |
| 固件 | 由 `uefi-clover` 构建（3 MiB 固件卷，内含 UniXNU 与 DarwinBoot）；`v0.3.10` 起 DXE 尾部完整派发，`CloverGopDxe` 发布 GOP |
| 设备事实 | 2026-10-22 采集过一次，位于 `facts/81ffcaaa-20261022-210928/` |
| 真机启动 | **菜单已到**（Stage 1）：`v0.3.10` 固件在屏幕上渲染 UniXNU 菜单并读到了 payload；菜单 3 秒后默认进 XNU。`v0.2.9` 实测屏幕出现**绿 + 蓝 + 品红**三条色带，即 DarwinBoot 已交接、XNU 入口已执行、引导页表已建好；`v0.2.10` 实测**品红出现约 6 秒后设备自己重启**，即 `sctlr_el1` 打开 MMU 后没有立刻死掉；`v0.2.11` 的 `_arm_vm_init` 复位点没有触发（**品红一直停着**），说明还没走到它；`v0.2.12` 实测**只有绿 + 蓝 + 品红、约 6 秒后自复位**，即 `_PE_init_platform` 返回了、开 MMU 后写屏确实不可用；`v0.2.13` 的 `_thread_bootstrap` 复位点实测是**品红后十几秒才复位、没有白字**，符合"panic 屏幕在 `vinfo` 仍为 0 时走了 12 秒兜底"；`v0.2.14` 扩映射并预填 `_vinfo`、不再主动复位，实测**仍是绿 + 蓝 + 品红，无黄带无白字**，由此定位到窗口在 framebuffer 之上、向上扩 `memSize` 永远够不到；`v0.2.15` 改为直接补页表项，实测**仍是绿 + 蓝 + 品红、无黄无白**，原因是 1 GiB 窗口只能落在高 DRAM bank，与 framebuffer 不在同一个 1 GB 槽，旧代码查错了槽位就退出；`v0.2.16` 换成独立 mapper 洞穴（同槽复用、异槽新建 L2 表），失败时用白色带自报错误码，实测**绿 + 蓝 + 品红 + 第 9 条白带**，即 mapper 走到自检第 4 步：窗口的第一个 L2 项不是 2 MB 块；由此查明 `boot_args.physBase` 锚在 `__PRELINK_TEXT` 上、本身不在引导映射内；`v0.2.17` 改为从 mapper 自己的 PC 反推引导映射，修掉复用 L2 表时未掩掉低位的 bug，并在 MMU 打开后立刻涂第 4 条青色带，实测**绿 + 蓝 + 品红 + 青、无黄**，即 MMU 已开、framebuffer 已经可写，卡点在 `_cpu_data_init` 被调用之前；`v0.2.18` 增加第 12 条橙色带（`arm_init` 入口后的第一条调用，不依赖 vinfo/栈/AT），用来区分"没进 `arm_init`"和"进了但 vinfo 画不出来"，待上机 |

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
6. **黄带**：`arm_init` 第 1 次调用 `_cpu_data_init` 返回后，按 `_vinfo` 涂第 3 条
   带。涂之前用 `AT S1E1W` 查一次翻译（失败就跳过），涂完用 `dc civac` + `dsb sy`
   把 Normal-WB 的像素推到显示引擎可见。

`v0.2.17` 上机的结果是**绿 + 蓝 + 品红 + 青，无黄**：MMU 已经打开、framebuffer 在
V=P 下可写，但黄带没有出现。黄带本身依赖三样东西——`_vinfo` 种子、`AT` 查询、
`arm_init` 里的调用点——任何一样出问题都画不出来，所以这一条无法区分"没走到"
和"走到了但画笔失效"。

`v0.2.18` 因此加一条**独立的橙色带（第 12 条）**，绕开全部三个依赖：

7. **橙带**：在 `_arm_init` 的**第一个调用**（`bl _arm_set_kernel_tbi`）处插入
   洞穴，直接从 `x0`（`boot_args` 指针，此时仍在 KVA 引导映射内）读
   `Video.base` / `rowBytes` / `height`，按 `(rowBytes*height)/16` 算第 12 条带
   并涂橙色（`0xFFFF8000`，BGRA），`dsb sy` + `dc civac` 之后 `bl` 原来的目标、
   跳回原地址继续。不用栈、不查 `AT`、不读 `_vinfo`，也不改 `x0`。
   只要 `arm_init` 入口的 `mov x19, x0` 之后、`sub sp` 之前的现场是好的，它就会
   出现在屏上。它把"`start.s` 的 `ret` 落到 `arm_init`"和"`arm_init` 走到
   `_cpu_data_init`"这两件事分开。

这一轮的色带把"没画出来"变成可读：白带说明 mapper 的哪一步拒绝，青带说明 MMU
打开后 framebuffer 真的可写，橙带说明 `arm_init` 至少执行到了第一条调用，黄带说明
`_cpu_data_init` 的调用点已经被走到（注意黄带在 `bl` 之前涂，不证明调用返回）。

| 现象 | 含义 |
| --- | --- |
| 绿 + 蓝 + 品红 + **青带 + 橙带 + 黄带**，随后白字 | mapper、种子、`AT`、`arm_init` 全通，内核之后踩到 panic；直接拍白字 |
| 绿 + 蓝 + 品红 + **青带 + 橙带 + 黄带**，然后停住 | `_cpu_data_init` 已进过，之后是自旋而不是 panic；下一轮把复位点放回 `_thread_bootstrap` 做时间二分 |
| 绿 + 蓝 + 品红 + **青带 + 橙带**，无黄带 | `arm_init` 已进入，卡点在第一条调用到 `_cpu_data_init` 调用点之间（rebase、`memmove` 或 `arm_set_kernel_tbi`）；下一轮在 `memmove` 前后各放一条带 |
| 绿 + 蓝 + 品红 + **青带**，无橙带 | `start.s` 的 `ret` 没有落到 `arm_init`，或落在入口就 fault（`sp` / 代码页问题）；下一轮在 `ret` 前再放一条带做对照 |
| 绿 + 蓝 + 品红，**没有青带也没有白带** | mapper 返回成功但 `bl` 没能回到品红洞穴，或 `msr` 之前就 fault；下一轮回到定时信号继续二分 |
| 绿 + 蓝 + 品红 + **白带在第 6–11 条** | mapper 失败；带号减 5 就是上表的错误码 |
| 品红后十几秒自复位、无白字 | panic 发生在种子之前，`vinfo` 仍为 0，走 12 秒兜底 |

```sh
fastboot flash cache ~/clover-out/round48-payload.img   # 只换这一格，boot 不变
fastboot reboot
```

`boot` 镜像没有变化：固件卷、BootShim 和 DTB 都没动，只有 `cache` 上的 payload
卷换了。两条涂色探针（黄带 stage 3、橙带 stage 4）现在是设备仓脚本里的默认值，
基础仓不需要任何本地改动；`patch-xnu-entry-probe.py --no-paint` / `--no-paint2`
可以分别关掉它们。把 `entry_probe_reset_delay` 加回来（并可配
`entry_probe_reset_after`）就恢复定时复位探针。

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
