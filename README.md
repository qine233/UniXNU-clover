# UniXNU-clover

**小米平板 4 8 英寸 LTE 版（2018）**的设备仓库——代号 `clover`，LTE 版本，
4 GB 内存，高通 **SDM660**（骁龙 660，Kryo 260：4× Cortex-A53 @1.8 GHz
+ 4× Kryo 385 Gold @2.2 GHz）。

它向 [UniXNU](https://github.com/UniXNU/UniXNU) 构建驱动描述这块板子：
目标 IPSW、内核配置、prelink 根、固件检出、ESP payload 和 Apple 格式设备树。
配套的固件仓库是 `uefi-clover`，同一块板子的 Mu-Silicium 移植。

`clover` 既是主线给这个家族的名字（`xiaomi,clover-lte`），也是小米给这个平台
的名字（`BoardPlatform: clover`），所以板级标识沿用它。

目标系统标识是原封不动的 **iPad7,1 / iPadOS 17.7.11 / 21H461** 镜像
（`j120ap`，`ApChipID 0x8011`，`ApBoardID 12`）。Kryo 260 是 ARMv8.0，
没有指针认证，所以只有这一档 4 KiB 页的镜像能跑，内核按
`DEVELOPMENT_ARM64_BCM2837` 构建。

逐轮的开发过程、每一张照片读到什么、由它推出什么，全部搬到了
[`notes/development-log.md`](notes/development-log.md)。这份 README 只留关键
事实和进度。

## 开发进度

现在的形态：整条内核引导链、根挂载、EL0 与多进程、eMMC 读写都已经有真机读数
并且通了；launchd 已经在拉守护进程（`zproxy`、`accessoryupdater`、
`identityservicesd` 都被 exec 起来过），卡住它的是驱动自己——一次 102 秒的
checkpoint flush 握着块守卫，把另一颗卷上的每一次读判成 EIO，pid 1 最终在
映射页时 SIGBUS。round171 修那一次 flush，round172 修「每个字节都过 CPU」这件
事本身：读通路现在有一条 ADMA2，靠逐字比较挣到信任。

### 已完成（都有真机读数）

| 事项 | 证据 |
| --- | --- |
| 固件与菜单 | `uefi-clover` v0.3.10 渲染 UniXNU 菜单、读到 payload，3 秒后默认进 XNU（Stage 1） |
| XNU 引导链 | 入口色带 + panic 屏证明内核经 `arm_init`、`machine_startup`、`vm_mem_bootstrap` 走到 `_bsd_init` |
| 时钟与抢占 | INTID 19 真的来 tick（round82 `ticks=0x14e5`）；`select` 期限、`wait4` 唤醒、抢占第一次有依据 |
| AMFI | `amfi-only-platform-code` + `CloverPMGR` 发布的 `ApplePMGRNub` 消掉 `No PMGR?` panic，`PMGRAON` 走完读写（round83） |
| EL0 与 syscall | `rd=md0` mockfs 根上 pid 1 进用户态；`getpid`、`open("/dev/console")`、`write`、`fork`、`wait4`、`execve` 都有读数 |
| APFS 根 | round137 挂上 OS 卷；round150 owner 审计接受未封印的根，三模块信任缓存放行 |
| SystemOS | round143/145 在 exec 时把 SystemOS 卷 union 到 `/`；round153 dyld 打开缓存目录、主缓存、`.01` 三处 err=0，两个头都是 `dyld_v1   arm64` |
| eMMC 高速读 | round159 帧：16 KiB 读 7 ms、224 KiB 读 189 ms（约 1–2 MB/s，比 round156 前的 25 KB/s 快 40–80 倍）；CMD6 高速链路、8 线、CMD18 批量成立 |
| eMMC 读不再级联失败 | round169 帧：卡停在数据态那一类级联消失——`buf_bread … error = 5`、`dev_get err 5`、APFS jhash 校验错在同一次冷刷里全部消失，唯一驱动读数 `block guard waited 31 ms`。**但 round170 帧暴露出另一类级联**：`read guard … st=00000000`，读不是被介质拒绝，是没排到块守卫；由 round171 修 |
| eMMC 写 | 写完成需 CMD13 确认、批量读须三份样本一致才信任（round167）；`disk5: general error` 自 round168 帧起未再出现。代价被 round170 帧量出来：`CACHE_CTRL` 清零之后 `tx_flush` 从 22.7 秒涨到 **102.0 / 120.0 秒**，握死块守卫；round171 改为开缓存 + 显式提交 |
| launchd 与守护进程是死是活可读 | round170 帧：`clover r170: exec pid=38 ppid=1 t=66 comm=accessoryupdater`、`exit pid=54 ppid=1 t=29 rv=0x4000 ns=-1 code=0x0 comm=zproxy`——comm 在换、pid 在铺、`t=` 在推进，`Early boot complete` 出现两次；结论是**在往前走而不是在循环**，直到 pid 1 自己 `initproc exited -- exit reason namespace 2 subcode 0xa`（SIGBUS） |
| Data 卷 | round159 帧：`seed data … bytes=33554432`、`bsd=disk5s1`、APFS `mount-complete volume DawnData`，UUID `FC1A580E-2D67-48FC-B8CF-87422A319340` 与钉住的种子一致，owner 的 `/private/var` 挂载失败行没有出现 |
| dyld 缓存预检 | round159 帧：目录 / 主缓存 / `.01` 全 err=0，`.01` 头 `len≈1.07 GiB sig=11491332+229376`，随后每颗子缓存的 `F_ADDFILESIGS` 都 `err=0 resid=0` |
| 诊断通道 | panic 屏直写 framebuffer（不经 tty）、无人值守的 IMEM `0x77665501` + PSCI 复位、`read-clover-log.py` 取 logfs、`decode-clover-address.py` 把地址翻成函数 |
| 仓库契约 | `porting.toml`（`schema_version = 1`，上游 13 area 校验通过）、`AGENTS.md`、`.github/workflows/porting-record.yml`；CI Device documentation 在 main 与 tag 双绿 |

### 进行中（代码已在镜像里，等实机读数）

| 轮次 | 内容 | 状态 |
| --- | --- | --- |
| round154（v0.5.7） | syscall 536 入口/返回、setup 阶梯与 `mapfile` 探针 | 探针仍在镜像里，至今没有 `map2` 读数 |
| round166（v0.5.20） | exec 侧打印前八个签名 blob 的 `mo/off/size` 与头四字 | `clover r166: sig` 尚未上屏 |
| round171（v0.5.27） | 批量读按「交付字数 + 内容错误位」判定（`st=0x21 w=1024/1024` 那种全量交付不再被丢回单块）；卡的写缓存打开并显式提交（CMD6 关→提交→开）；块守卫与批量缓冲从有界自旋改成 `IOLock`；批量 32 → 128 块，可信性闸门改为按请求取样 | 改动全部在 round172 镜像里，与下一行同一帧读 |
| round172（v0.5.28） | 读通路加一条 ADMA2：描述符形状逐字节抄自本板 4.19 CAF 树（12 字节项、`cmd` 在前 `len` 在后、末项带 END 不加 NOP），数据落在本驱动自有的 64 KiB 连续 bounce（照 sheng `NativeUfs` 的四条物理地址检查与 `dc civac`/`dc ivac` 节奏）；同一个请求先走已信任的批量 PIO、再走 ADMA，**逐字相同才准用**，失败八次整轮退役 | **已构建**，待冷刷 |

round171 + round172 一帧问六件事：`read guard` 是否整批消失（守卫只等、不拒），
`tx_flush … took … us to flush` 是否从 102/120 秒掉回毫秒量级，
`multi read fail … st=0x21 w=1024/1024` 是否不再出现且 `readcheck … trusted=1`
上屏；再加上 `CloverSdhci r172: dma up data=…` 有没有出现、
`dmacheck … trusted=` 与 `kbs=…/…` 那两个速率数、`first dma read … ms=` 有没有
落地。前五件成立，launchd 的页才继续映射得进来；第六件给出 536 与 `/data` 之前
是否要先重排吞吐这一格的答案。

### 排期（未开始 · 已知限制）

1. **共享区映射（syscall 536）** — 头字段与签名注册两段已被 round159 帧关掉，
   映射这一格还空着；排在守护进程起来之后。
2. **完整 `/data`** — 现在只是 32 MiB 的 APFS scratch 种子，追加在 userdata 的
   两个 GPT 副本之间、不进 GPT；原 54.4 GB userdata 内容没有迁移。
3. **显示与 GPU** — 没有 MDP5/DSI 驱动，屏幕只服务于启动诊断；Adreno 512 无驱动，
   没有 iOS 显示栈。
4. **输入** — 触摸（`edt-ft5406` / focaltech `fts`）没有驱动。
5. **网络、音频、USB** — WCN3990 WiFi/蓝牙、WCD 音频、USB device 模式都没有。
6. **电源管理** — 除 AMFI 需要的那颗 nub 外，充电、电池、PMIC 都没有真驱动。
7. **eMMC 写通路仍是 PIO** — 读的那条 ADMA2 已在 round172 镜像里（描述符抄自
   本板 CAF 树，数据走自有连续 bounce，且必须与批量 PIO 逐字相同才准用），
   `cap=742dc8b2` / `hcl=38` 证明这本来就是固件自己在用的通路。写不跟着上 DMA：
   一次写错不会以 EIO 回来，而是改卡。下一格是「往暂存区写模式、用 PIO 读回来
   比」的写/读回闸门，过了才谈 DMA 写。`write settle took … ms` 至今没被读到
   过，HS400/CQE 等这一行真的上屏再定。
8. **内核构建不可逐字节复现** — 每次 prelink UUID 不同；`verify-clover-payload.py`
   只比较两个可执行段。
9. **存储栈形态** — 没有 UFS、virtio-block 或 native verified-root；sheng 的存储栈
   是 SM8550 专有的，不能照搬。

## 关键硬件事实

大部分常量来自公开的设备树，在 `feeds.toml` 里钉住版本，`unixnu fetch` 会保留
一份副本；平板自己能回答的那些来自 `facts/<serial>-<stamp>/` 下的 dump
（现有的一份是 2026-10-22 采集的 `facts/81ffcaaa-20261022-210928/`）。

| 项目 | 值 | 来源 |
| --- | --- | --- |
| SoC / msm-id | SDM660，`qcom,msm-id = <317 0x0>`（`0x13d`） | 下游 `sdm660.dtsi` |
| 下游 board-id | MTP 基座上的 `qcom,board-id = <11 0>`；merged dtb 报 `<0x0001000b 0x0>` | `sdm660-mtp-clover.dts`、dump |
| 核心 | Silver `0x0-0x3`（A53，1804800 kHz），Gold `0x100-0x103`（2208000 kHz） | `sdm660.dtsi` → `sdm630.dtsi` |
| 时基 / tick | 19.2 MHz；虚拟定时器走 **INTID 19**（timer 节点四个 PPI 1、2、3、0 ⇒ 17/18/19/16） | 两套树一致、`mu-clover/Resources/DTBs/clover.dts:2233` |
| DRAM | base `0x80000000` + `0x80000000`，第二个 bank `0x100000000` + `0x7e4c0000` | `dt/memory.raw.txt`、`proc/meminfo.txt`（MemTotal 3825460 kB） |
| GIC | distributor `0x17a00000`/`0x10000`，redistributor `0x17b00000`/`0x100000`，stride `0x20000` | 下游 `sdm660.dtsi` |
| Splash / framebuffer | `0x9d400000` + `0x02400000`，即固件内存图里的 "Display Reserved" 行；overlay 的 `0x9d500000` 不作数 | `fw/xbl-uefiplat.txt` |
| 屏幕 | 1200×1920，`boe,tv080wum-nx2` / `novatek,nt51021`，107×172 mm，即 8 英寸 | 主线 + 下游 |
| 触摸 / 背光 | `edt,edt-ft5406` @ `blsp_i2c3` 0x38（下游用 focaltech `fts`）；背光 `pm660l_wled`，是 LED 驱动而非 PWM | `sdm660-xiaomi-clover-regular-common.dtsi` |
| 存储 | eMMC `sdhc_1: sdhci@c0c4000`（hc `0xc0c4000`/`0x1000`，cqhci `0xc0c5000`/`0x1000`，ICE `0xc0c8000`/`0x8000`），CQE + HS400ES，8-bit | 下游 `sdm660.dtsi` |
| userdata | mmcblk0p64，`first_lba 0xf10520` / `last_lba 0x747bfde`，即 54,449,765,888 字节 | dump |
| 保留区 | adsp `0x92a00000`、mba `0x94900000`、cdsp `0x94b00000`、venus `0x9cc00000`、ramoops `0x9fe00000`、gpu zap `0xf0b00000` | 下游设备树 |
| 串口 / USB | `blsp1_uart2` 115200 是 `stdout-path`；`dr_mode = "peripheral"`，只有 device 模式 | 两套树一致 |
| boot 镜像参数 | header version `0`、page size `0x1000`、kernel offset `0x8000`、ramdisk `0x1000000`、tags `0x100`、DTB 追加；无 AVB 尾，boot 镜像自身不带签名 | dump 的 stock `boot`/`recovery` 两个镜像互相印证 |
| 固件参照 | Mu-Silicium 的 `lavender`（Redmi Note 7）同颗 SoC：DDR `0x80000000`、FD `0x9FC00000`/`0x300000`、`append_dtb`、psci `smc` | `Resources/Configs/lavender.toml` |

## 目录

```
board.toml                    板级描述
configs/clover_defconfig      内核配置
feeds.toml                    每个常量所依据的两棵设备树
drivers/                      板级 XNU 驱动（ArmCpu、CloverPMGR、CloverSdhci、CloverBlock …）
patches/xnu-clover/           板级 XNU 补丁（探针与板级行为）
scripts/                      ADT 生成器、预检与校验、事实采集、归档构建、md0/mockfs 根镜像
notes/                        调研笔记 + development-log.md（逐轮流水）
facts/                        按机器存放的 dump，Git 忽略
```

## 构建

内核这一步只能在 macOS 上做；固件在 Linux 上构建。宿主机先补两件事：
`sudo xcodebuild -license accept`，以及给 `./unixnu` 准备一个不带版本号的
`python3`（它的 shebang 是 `env python3`，macOS 自带的是 3.9、没有 `tomllib`）：

```sh
mkdir -p ~/UniXNU-clover/build/shim
ln -sf /opt/homebrew/bin/python3.13 ~/UniXNU-clover/build/shim/python3
```

下面所有 `./unixnu` 都以 `PATH=$HOME/UniXNU-clover/build/shim:$PATH` 开头。
`scripts/check-clover.py` 没有这个要求，系统 Python 3.9 就能跑。

`--uefi` 指向的不是 `uefi-clover`，而是**应用了它 overlay 的那份 Mu-Silicium
检出**（本机 `../mu-clover`）：payload 的 panic 屏要复用固件的 8×16 点阵，
`patch-xnu-entry-probe.py` 因此去那份检出的 `FrameBufferSerialPortLib/Font.h`
取字模。

```sh
git clone --recurse-submodules https://github.com/UniXNU/UniXNU
git clone <this repo> UniXNU-clover

cd UniXNU
./unixnu defconfig clover_defconfig -D ../UniXNU-clover
./unixnu fetch
./unixnu tree
python3 ../UniXNU-clover/scripts/check-clover.py      # 预检，任何时候都可以跑
./unixnu build firmware --uefi ../mu-clover
SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" ./unixnu build kernel
./unixnu build package
cd ../UniXNU-clover && python3 scripts/make-clover-root.py   # md0/mockfs 根 + trust cache
cd ../UniXNU
./unixnu build payload --unique-chip-id 0x<device seed> --uefi ../mu-clover \
  --kernel ~/clover-out/roundNNN-kernel-amfi.macho \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
./unixnu bootimg --uefi ../mu-clover --target RELEASE \
  --dtb ../mu-clover/Resources/DTBs/clover.dtb --ramdisk none
```

四条必须照办，否则构建会静默产出错的镜像：

1. **先 kernel，再 payload。** payload 里那两半都从内核产物取。
2. **payload 必须带 `--kernel …-kernel-amfi.macho`。** 每次重建内核，AMFI 的两处
   补丁都会被 prelink 恢复成 stock；那份 copy 是 `scripts/patch-clover-amfi.py`
   重打出来的，才是实机上真正在跑的镜像。
3. **`--trust-cache` 用 `build/clover-trust/trustcache.bundle`（三模块）。**
   根镜像旁边那颗 56 字节单模块 bundle 会被 `board.toml` 的 `trust_cache_sha256`
   直接拒绝。
4. **`bootimg --ramdisk none` 是必须，不是省略。** 否则它把 payload 卷当 ramdisk
   塞进 boot 镜像，而这块板子的固件根本看不见 ramdisk 里的卷。产物是
   `out/clover/boot/boot.img`，它是 boot 分区唯一需要的命令（`unixnu` 没有
   `build boot`）。

补丁层的硬规则：`build-xnu-4k-baseline.sh` 按文件名排序逐个
`git apply --reverse --check`（已在树上就跳过），否则 `--check` + `apply`，
`set -e`。所以**新探针绝不能插进排序在前的补丁自己新增的块**——那会让逆撤失败、
正打也失败、构建中止；要改前面的补丁就重生成它自身
（`scripts/save-xnu-patch.py`，先逆撤同文件排序在后的补丁）。另外内核构建前必须在
`out/clover/tree/.cache/xnu-work` 逆撤
`zzy-clover-dyld-cache-skip.patch`（它盖住上游的
`native-4k-signature-hash-cache.patch`）。

`tree/patches/xnu` 是指向基仓 `~/UniXNU/patches/xnu` 的软链，构建只读那一份；
改完 `~/UniXNU-clover/patches/xnu-clover/` 必须同步过去，否则 forward apply 失败。

Xcode 的 clang 在重建归档时不带 `-isysroot` 会拿 macOS SDK 编 iOS 目标并把
`-Wincompatible-sysroot` 当错误，上面显式指定 iPhoneOS SDK 就是为了绕开它；
只重打 payload 不需要这一步。

## 校验

```sh
python3 scripts/check-clover.py            # 板级预检（= [firmware] preflight）
python3 scripts/verify-clover-payload.py   # 归档对镜像：21 项形状与探针断言
python3 scripts/verify-userdata-window.py  # 刷 userdata 前的宿主闸门（展开 sparse、SHNGROOT、APFS 只读挂载）
```

`check-clover.py` 生成一份临时 ADT 再把 Apple 的编码解回来，断言 `/chosen`
（64 字节 `random-seed`、`nvram-proxy-data` 与 `nvram-bank-size`、`dram-base`、
`dram-size`、`crypto-hash-method`、
`root-matching`、`display-base`、4 字节的 `amfi-only-platform-code`、4 字节为 1 的
`debug-enabled`）、`/chosen/memory-map` 里恰好只有 `RAMDisk` 与 `TrustCache`
两个占位、带八个 MPIDR 的 `/cpus`、`/psci`、带 `ranges` 与 GICv3 的 `/arm-io`、
`/defaults`、`/options`、`/product`；board 一段钉住根契约：根只由
`/chosen/root-matching` 对 OS 卷 UUID 的 `IOMedia` 属性匹配声明一次，`kernel_args`
里**不许出现任何 `rd=`**（round150 起，`rd=` 会在 owner 审计之前造出第二个根声明），
`cs_enforcement_disable=1` 与 `debug-enabled = <1>` 必须在（AMFI 只在
`PE_i_can_has_debugger()` 为真时接受那个参数），`clover-apfs = 3`，
`packed_data_ramdisk` 为假，`trust_cache_sha256` 非空并与 bundle 文件一致；
最后断言当前轮次的 `clover rNNN:` 探针形状与旧轮次复读形态的缺席。

## 上机

payload 卷不是 boot 镜像里的 ramdisk。固件卷有 `Fat`/`PartitionDxe`/`DiskIoDxe`
但没有 ramdisk 文件系统驱动，UniXNU 只能靠扫描 `SimpleFileSystem` 找
`\EFI\UniXNU\unixnu.toml`；只存在于 boot 镜像 ramdisk 字段的卷它看不见。所以卷
写到 eMMC 分区上，就像上游 fajita 写到 `boot_a` 那样——用 `cache`
（`mmcblk0p49`，256 MiB），可以随便牺牲，也装得下 56 MiB 的卷。

```sh
fastboot flash cache ~/clover-out/roundNNN-payload.img
fastboot reboot
```

**永远冷刷**：设备先关机，再用「音量减 + 电源」进 fastboot，绝不用
`fastboot boot`。只写 `cache`，不碰 `userdata`；要写别的分区，动手前必须说清
哪个分区、为什么，并取得同意。每一刷要问的问题写在轮次日志末尾，排期攒够时
一刷可以同时回答几件。

## 读数通道

- **入口色带 0–15**：下半屏的带在 panic 屏清屏后仍在，用于确认引导阶梯走到哪。
- **panic 屏**：直写 framebuffer，不经过 `vcputc`，是这台板子上唯一不受被测控制台
  影响的通道；读数在前、寄存器证据在后。
- **`clover rNNN:` 内核 printf 探针**：每轮换号，`verify-clover-payload.py`
  断言新号在、旧号的复读形态不在。
- **logfs**：`scripts/read-clover-log.py` 把内核环形缓冲拉回宿主。
- **地址翻译**：`scripts/decode-clover-address.py --suspects` 出本镜像的
  tty/console 区间表。
- **无人值守**：`clover-unattended=N` 的墙钟死线，把 panic 写进 IMEM
  `0x77665501` 再 PSCI 复位（当前 shipped 的 boot-arg 不带它）。

## 相关文档

- [`notes/development-log.md`](notes/development-log.md) — v0.1 起逐轮的改动、
  照片读数与推理，完整历史；新的轮次追加在文件末尾。
- [`notes/clover-device-tree-sources.md`](notes/clover-device-tree-sources.md) —
  每个常量的来源清单，含 splash 地址的推翻过程。
- [`porting.toml`](porting.toml) — 上游要求的 13 个 area 覆盖与证据。
- [`AGENTS.md`](AGENTS.md) — 仓库边界与当前活跃的 area。

## 许可证

BSD-2-Clause-Patent，与 UniXNU 一致。`scripts/build-arm-driver.py` 移植自
UniXNU-gta4l（NoNameBuilder），采用同一许可证。
