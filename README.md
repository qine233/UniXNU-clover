# UniXNU-clover

小米平板 4 8 英寸 LTE（2018）的 XNU 移植，代号 `clover`：4 GB / SDM660（Kryo 260，
ARMv8.0，无指针认证），跑原封不动的 **iPad7,1 / iPadOS 17.7.11 / 21H461** 4 KiB 页镜像。
配套固件仓库 `uefi-clover`（Mu-Silicium 移植），驱动与硬件常量在 `porting.toml`。

现状一句话：**显示链路出帧，SpringBoard 活不下来**——XNU 起来、OS/SystemOS/Data 三个卷全挂、
launchd 把守护进程拉起来；r202 让 `IOMobileFramebufferLegacy` attach + start，r205 写
`display-scale=2` 解掉 `FBSDisplayMode` 断言，r206 起 compositor 连续 present
（`done/presented=4`、每帧 9 216 000 字节），但整屏仍是黑的——SpringBoard 没有把界面画上去，
450–535 s 时被 backboardd 的系统 shell 看门狗用 `kill(2)` 杀掉。

r222 读数（boot 11）：**阻塞点这次有了一整条调用链**。SpringBoard 主线程（tid `0x104`）在
403/411/423/436 s 四次采样完全一致，六帧从下到上依次是
`_mach_msg_overwrite+0x1ec` → `_mach_msg+0x14` → libdispatch（`0x18f56dd28+0x20c`）→
libdispatch（`0x18f56e2a4+0x38`）→ **`libxpc._xpc_connection_send_message_with_reply_sync+0x100`** →
Foundation thunk（`0x186e80d04+0xc`）。宿主反汇编该 thunk 只有一条尾调 `bl 0x187548f50`，而它 `b`
进 libxpc 的同步发送，所以 x30 直接回到 thunk+0xc——调用者就是 Foundation 的一条同步 XPC 请求。
**它等的是什么也读出来了**：395.171 s 主线程往 launchd 端口 `0x3f07` 发了一条 24 字节空体的同步请求
（`msgh_id=0x5b9173`，reply 端口 `0xa03`，16 KiB）；403 s 起每次采样都是纯收
`op=40700420e b=0 sz=0 rn=a03 rs=4000`，一直到 446.42 s 被 backboardd `kill(9)`（`spid=58`）、
456.1 s 退出（`rv=0x9`）都没收到回信。launchd 在 450 s 之后到 912.9 s 一直没再拉起 SpringBoard，
同期 bluetoothd / fairplayd.A2 每 ~5 s 成对 spawn/exit——launchd 自己也不正常。其余活动线程分别是
`notify_post`（tid `0x168`）、XPC 发送（`0x40f`）、给 logd 发 36 字节 `h=13.0.1b03.0.0.2d52`
（`0x411`）、`__kernelrpc_mach_port_is_connection_for_service+0x80`（`0x41c`）。缺口收窄成一句话：
**SpringBoard 卡在一条发给 launchd 的同步 XPC 上，launchd 回没回、回了为什么没唤醒都不清楚；
r223 直接读两端端口的内核状态（消息队列、msgcount、waitq、srights），把这两种可能分开。**

更早的逐轮读数在 [`notes/development-log.md`](notes/development-log.md)。

## 距离 iOS 桌面还差什么

| # | 缺口 | 现状（真机读数） | 下一步 |
| --- | --- | --- | --- |
| 1 | **SpringBoard 活不下来：不渲染，约 450–535 s 后被 backboardd 的系统 shell 看门狗杀掉** | r222 读数（boot 11）：主线程（tid `0x104`）四次采样同一组六帧，栈顶是 **`libxpc._xpc_connection_send_message_with_reply_sync+0x100`**，上面还有 Foundation thunk 与 libdispatch 两层；395.171 s 它往 launchd 端口 `0x3f07` 发了 24 字节空体同步请求（`msgh_id=0x5b9173`，reply `0xa03`），403 s 起一直纯收 `rn=a03 rs=4000`，446.42 s 被 `kill(9)`、456.1 s 退出（`rv=0x9`）。launchd 在 450→912.9 s 没再拉起 SpringBoard，bluetoothd/fairplayd 每 ~5 s 成对 spawn/exit | r223：在采样 beat 上读 `0xa03` 与 `0x3f07` 的内核端口状态（ip_messages/msgcount/waitq/mscount/srights/receivers/kobject），队列非空就打出首条 kmsg 头，区分「回信没人唤醒」与「launchd 根本没回」 |
| 2 | **屏幕上是黑的** | compositor 已 present 四帧：`frame#0..3`、`copy#4 111 ms bytes=9216000`、`scanout#4 ok`、`retire#4 done=4 presented=4`；但 2 304 000 像素里 2 262 414 个 `ff000000`，只有 `305x887..755x1029` 那块变了 41 586 个像素 | 等第 1 项修好、SpringBoard 活下来画 UI；之后用色带校验几何与缩放 |
| 3 | 触摸没有任何应答 | QUP v2 I2C-3 电气是通的（两个候选地址都 NACK：`done xfers=4 nack=4 timeout=0`），`rails unproven` | 先把面板轨真的上电（依赖 4），再问地址 |
| 4 | PMIC 轨读不出来 | SPMI 仲裁器读通路是好的（`obs00=00000100`），但核心版本字不在 v5 带内，`apid=-1` 一个都没认，`ldo-a11`/`ldo-b3` 未读 | 先弄清那个版本字，再认 APID、读轨、开轨 |
| 5 | ADSP 没起 | SMEM 共享堆读通（`$TOC`/`$PRT` 全部自校验通过，`part(3) hosts=0:1` 是候选边），判决行还没上屏 | 有边就上 GLINK/APR；没镜像则先压 `vadsp` 电源 |
| 6 | watchdogd 每次起就崩 | 自身可执行文件里 `brk #1`（`IOWatchdog` 服务在板上不存在）。r193 的 `panic_on_crash_disabled=1` 已把它从「致命」摘掉，所以它只是噪声 | 写一个最小 `IOWatchdog` 服务，或让 plist 不再要求它 |

1 与 2 是一条链：SpringBoard 活下来才有 UI，有 UI 桌面才不是黑的；3–6 是之后要补的板级驱动。

> 时基已核（r207）：r170 的 `t=` 是 `mach_absolute_time()/1e9`，把 CNTVCT 的 tick 数当秒打印，
> 不是秒。用环里带 CNTVCT 的 r193 行反算，十处比值全是 1.0000，900 s 死线的
> `cnt1−cnt0` 也是 899.5 s——mach_absolute_time、mach_continuous_time 与环时间戳一致，
> 时基没有偏慢。上一轮的「慢 50 倍」是基数除错的读法。

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
| 无人值守 | `run-clover-unattended.sh`：冷刷 cache → 900 s 死线 → 预置 BCB 落 TWRP → 读回日志，退出码 0（本帧读数即由此而来） |
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

- [`notes/development-log.md`](notes/development-log.md) — round1–200 每一轮的设计、构建与读数，完整历史。
- [`notes/clover-device-tree-sources.md`](notes/clover-device-tree-sources.md) — 每个常量的来源，含 splash 地址的推翻过程。
- [`porting.toml`](porting.toml) — 上游要求的 13 个 area 与 findings。
- [`AGENTS.md`](AGENTS.md) — 仓库边界与闸门。

## 许可证

BSD-2-Clause-Patent，与 UniXNU 一致。`scripts/build-arm-driver.py` 移植自
UniXNU-gta4l（NoNameBuilder），采用同一许可证。
