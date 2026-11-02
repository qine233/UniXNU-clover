# UniXNU-clover

小米平板 4 8 英寸 LTE（2018）的 XNU 移植，代号 `clover`：4 GB / SDM660（Kryo 260，
ARMv8.0，无指针认证），跑原封不动的 **iPad7,1 / iPadOS 17.7.11 / 21H461** 4 KiB 页镜像。
配套固件仓库 `uefi-clover`（Mu-Silicium 移植），驱动与硬件常量在 `porting.toml`。

## 启动到哪一步了

从按下电源到桌面，整条链分八步。前六步已经真机走通（每步都有卡上日志的读数背书），
现在停在第七步：

1. **固件** —— `uefi-clover` 渲染菜单、读到 payload，3 秒后默认进 XNU。
2. **内核** —— XNU 起来，时钟、抢占、系统调用都有读数。
3. **平台服务** —— AMFI 放行、`ApplePMGRNub` 就位，进入用户态。
4. **磁盘** —— eMMC 高速读写通了，APFS 的 OS 卷与 Data 卷都挂上。
5. **用户态** —— launchd 跑起来，把一整套 iOS 守护进程拉起（`Early boot complete`）。
6. **点亮屏幕** —— backboardd 拿到帧缓冲，第一次 present/scanout/retire 成功，
   屏幕上出现它自己的启动进度条。
7. **SpringBoard ← 卡在这里** —— 桌面进程被反复拉起，每次都停在半路被杀，
   屏幕一直是进度条，进不了桌面。
8. **桌面** —— 未到。

## 当前卡点（结论）

SpringBoard 每次都在等 backboardd 回信，信一直不来。逐轮取证把病根从「触感初始化欠一次
leave」一路收紧到一句可测的话：

* **卡住的不是渲染，是一次没人回音的 XPC 往返**：backboardd 全进程唯一一笔在外的请求
  发给 `com.apple.datamigrator`，请求被取走、回音永不到期（`mc=0 sr=1 dl=0`）。
* **债主 datamigrator 自己在等设备解锁**：它的 XPC 处理器在
  `dispatch_semaphore_wait(…, DISPATCH_TIME_FOREVER)` 上等
  `-waitUntilPreboardDidFinishUnlock`，本板没有 SEP。
* **keystore 已发布并测到**（`NativeKeyStoreEvents` + `CloverKeyStoreProbe`，回答
  「无 keybag、开机即解锁」）：解锁信号量等待随之消失，但 workloop 那五条等待者照旧，
  桌面仍未出。
* **单核已坐实**：内核 `CpuDataEntries` 只注册一颗核（`real=1`），副核从未被 start。

每一轮的设计、构建与真机读数都在 [`notes/development-log.md`](notes/development-log.md)
逐轮可查，本节只留当前结论。

## 距离 iOS 桌面还差什么

| # | 缺口 | 现状 | 下一步 |
| --- | --- | --- | --- |
| 1 | **backboardd 不回 SpringBoard 的信** | 卡在发给 `datamigrator` 的一笔没人回音的 XPC 往返（上节详述）；keystore 已发布、解锁等待消失，workloop 五条等待者照旧 | 追 datamigrator 那条 workloop 为什么不再排活；做 PreBoard 同形状普查 |
| 2 | 触摸没有任何应答 | QUP v2 I2C-3 电气通，两个候选地址全 NACK（`verdict=102`） | 先把面板轨真上电（依赖 3），再问地址 |
| 3 | PMIC 轨读不出来 | SPMI 仲裁器读通路好，但核心版本字不在 v5 带内，APID 一个没认 | 弄清版本字、认 APID、读轨开轨 |
| 4 | ADSP 没起 | SMEM 共享堆读通（`$TOC`/`$PRT` 自校验通过），判决行还没上屏 | 有边就上 GLINK/APR；没镜像先压 `vadsp` 电源 |
| 5 | watchdogd 每次起就崩 | 自身可执行文件里 `brk #1`（`IOWatchdog` 服务不存在），已摘除致命 | 写最小 `IOWatchdog` 服务，或让 plist 不再要求它 |
| 6 | **GPU/Metal 一个都没有**（Adreno 512 无驱动、无固件、无 renderer） | `gpu` 区 `planned`；boot 74 实机：**r286 上电写序列在本板第一次生效**——`cx_on=1`、`gfx3d_en=1`、AHB/BIMC 三票=1（eMMC 正对照成立），但 `gx_on=0`，因为 `rcg=80000000`（gfx3d_clk_src 的父源 CONFIG=0，正是 r286 没设的那一半）→ `chipid skipped` | 下一步由数据定：r288 先编 RCG 父源+频率再重走 gx，读 `chipid`（对 0x05010200）；**桌面仍 CPU-first** |

1 挡住桌面；6 是 1 最可能的病根（尚未证）；2–4 是之后要补的板级驱动，5 只是噪声。

> 备注（时基）：r207 已核，环上的 `t=` 与 `mach_absolute_time` 一致，没有偏慢；
> 早期「慢 50 倍」是基数除错的读法，已废弃。每靴 1800 s 的死线是本项目自设的无人值守
> 收场开关（不是机器死机）——它是自动落回 TWRP 读回的唯一通路，r285 试过摘掉它、
> boot 73 跑到 t=3145 s 也不再自己回来，r287 又装回去。

## 已完成（都有真机读数）

| 事项 | 证据 |
| --- | --- |
| 固件与菜单 | `uefi-clover` 渲染菜单、读到 payload，3 秒后默认进 XNU |
| XNU 引导链 | 入口色带 + panic 屏走到 `_bsd_init`；时钟/抢占/`select` 期限都有读数 |
| AMFI / PMGR | `amfi-only-platform-code` + `ApplePMGRNub`，`No PMGR?` 消失 |
| EL0 与多进程 | mockfs 根上 pid 1 进用户态；`fork`/`wait4`/`execve` 全通 |
| APFS 根与 SystemOS | OS 卷挂上、owner 审计放行、exec 时 union 到 `/`；dyld 缓存目录/主缓存/`.01` 全 `err=0` |
| Data 卷 | 1 GiB 种子、UUID 钉住（r198），`ENOSPC` 消失 |
| eMMC | 高速 8 线 + CMD18 批量；ADMA2 读（逐字比对）、写只准回写自己读到的字节；r192 把 cache flush 交还卡，barrier 不再挂死 |
| launchd 与守护进程 | `exec`/`exit` 成对、`Early boot complete`、SpringBoard 被反复拉起 |
| 显示栈首次出帧 | r202 attach + start，5 个进程各拿到一个 `IOMobileFramebufferUserClient`；r206 第一次 present：`swap#1/2/3 accepted`、`scanout#3 ok bytes=9216000`、`retire#3 done=3 presented=3`、`copy#3 93 ms` |
| 读数通道 | 两个环落在 **logdump**（p56）：kind-21 驱动判据 64 槽、kind-22 内核日志 512 槽，一次 `dd` 1216 扇区读回；panic 屏把 `fb=` 写进记录 |
| 无人值守 | `run-clover-unattended.sh`：冷刷 cache → 死线 → 预置 BCB 落 TWRP → 读回日志，退出码 0（本帧读数即由此而来） |
| 可复现内核 | 968 个 object 戳、`LC_UUID`、`_PrelinkKCID` 由文件内容派生，同树两次构建收敛到同一镜像 |

## 待完成

- 上表 1–2 挡住桌面；3–6 挡住板级功能。
- **payload 容器不逐字节可复现**：ADT 的 `/chosen/random-seed` 每次重抽（内核本身可复现）。
- **构建顺序反着**：`unixnu` 先跑 archives 再跑 XNU，身份戳会删掉 object，让 `nm` 失败；r186 起用一次性 object 预 pass 绕过，没有修。
- **补丁回放名单手写**：新增补丁若碰到排序在前那条的上下文，必须手动加进 `build-xnu-4k-baseline.sh` 的「先反向」名单。

## 构建

内核只能在 macOS 上做。先备一个不带版本号的 `python3`（`./unixnu` 的 shebang 是
`env python3`，macOS 自带 3.9 没有 `tomllib`），并用 `SDKROOT` 指定 iPhoneOS：

```sh
mkdir -p ~/UniXNU-clover/build/shim
ln -sf /opt/homebrew/bin/python3.13 ~/UniXNU-clover/build/shim/python3
export PATH=$HOME/UniXNU-clover/build/shim:$PATH

./unixnu defconfig clover_defconfig -D ../UniXNU-clover
./unixnu fetch && ./unixnu tree
python3 ../UniXNU-clover/scripts/check-clover.py
./unixnu build firmware --uefi ../mu-clover
SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" ./unixnu build kernel
./unixnu build package
```

然后依次：打 AMFI → 归一化 → 出 payload → 出 boot.img。

```sh
python3 scripts/patch-clover-amfi.py \
  ../UniXNU/out/clover/kernel/kernel.macho ~/clover-out/roundNNN-kernel-amfi.macho
python3 scripts/normalize-kernel-macho.py ~/clover-out/roundNNN-kernel-amfi.macho
cd ../UniXNU
./unixnu build payload --unique-chip-id 0x<seed> --uefi ../mu-clover \
  --kernel ~/clover-out/roundNNN-kernel-amfi.macho \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
./unixnu bootimg --uefi ../mu-clover --target RELEASE \
  --dtb ../mu-clover/Resources/DTBs/clover.dtb --ramdisk none
```

四条不能省：

1. **先 kernel 再 payload**，载荷里两半都从内核产物取。
2. **payload 必须显式给 `--kernel …-kernel-amfi.macho`**：AMFI 两刀会被 prelink 恢复成 stock。
3. **`bootimg --ramdisk none`**：这块板子的固件看不见 boot 镜像里的 ramdisk。
4. **改过 `clover_defconfig` 就重跑 `defconfig`**：`build kernel` 不会替你复制 `.config`。

补丁侧：`build-xnu-4k-baseline.sh` 按文件名排序逐条 `git apply --reverse --check`，
所以新补丁绝不能插进排序在前那条新增的块；`~/UniXNU-clover/patches/xnu-clover/`
改完必须同步到 `~/UniXNU/patches/xnu/`（`tree/patches/xnu` 是软链）。细节见
[`notes/development-log.md`](notes/development-log.md)。

## 上机

payload 卷写在 `cache`（`mmcblk0p49`，256 MiB，可以随便牺牲）；**永远冷刷**
（关机 → 音量减 + 电源进 fastboot），不用 `fastboot boot`，不碰 `userdata`。

```sh
scripts/run-clover-unattended.sh --out ~/clover-out/roundNNN-log-window.bin \
  --flash-cache ~/clover-out/roundNNN-payload.img --boot-after-flash
```

脚本等板子自己落进 TWRP，一次 `dd` 拉回 1216 扇区并解码。手动刷就是
`fastboot flash cache …` + `fastboot reboot`。

## 读数怎么看

- **panic 屏**：直写 framebuffer，不经过被测控制台；记录第二行是 `fb=`（1 = IMEM 标记写进并读回）。
- **logdump**：`scripts/read-clover-log.py [--verdict|--tee|--count] <window>`。
  驱动判据环 64 槽、内核日志环 512 槽，转圈的环按序号还原先后。
- **内核 `clover rNNN:` 探针**：每轮换号，`verify-clover-payload.py` 断言新号在、旧号复读形态缺席。
- **地址翻译**：`scripts/decode-clover-address.py`；`read-clover-log.py` 的 `--selftest` 在闸门里跑。

## 闸门

```sh
python3 scripts/check-clover.py            # 板级预检 + 补丁/探针形状 + 驱动宿主模型
python3 scripts/verify-clover-payload.py   # 归档对镜像：卷内形状 + 逐轮探针断言
python3 scripts/verify-userdata-window.py  # 刷 userdata 前的宿主闸门
```

## 相关文档

- [`notes/development-log.md`](notes/development-log.md) — 每一轮的设计、构建与真机读数，完整历史。
- [`notes/clover-device-tree-sources.md`](notes/clover-device-tree-sources.md) — 每个常量的来源，含 splash 地址的推翻过程。
- [`porting.toml`](porting.toml) — 上游要求的 13 个 area 与 findings。
- [`AGENTS.md`](AGENTS.md) — 仓库边界与闸门。

## 许可证

BSD-2-Clause-Patent，与 UniXNU 一致。`scripts/build-arm-driver.py` 移植自
UniXNU-gta4l（NoNameBuilder），采用同一许可证。
