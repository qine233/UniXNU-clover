# UniXNU-clover

小米平板 4 8 英寸 LTE（2018）的 XNU 移植，代号 `clover`：4 GB / SDM660（Kryo 260，
ARMv8.0，无指针认证），跑原封不动的 **iPad7,1 / iPadOS 17.7.11 / 21H461** 4 KiB 页镜像。
配套固件仓库 `uefi-clover`（Mu-Silicium 移植），驱动与硬件常量在 `porting.toml`。

现状一句话：**显示链路出帧，SpringBoard 活不下来**——XNU 起来、OS/SystemOS/Data 三个卷全挂、
launchd 把守护进程拉起来；r202 让 `IOMobileFramebufferLegacy` attach + start，r205 写
`display-scale=2` 解掉 `FBSDisplayMode` 断言，r206 起 compositor 连续 present
（`done/presented=4`、每帧 9 216 000 字节），但整屏仍是黑的——SpringBoard 没有把界面画上去，
约 497 s 时被 backboardd 的系统 shell 看门狗用 `kill(2)` 杀掉。

r225 读数（boot 14）：**launchd 在发消息，回信也在到；死的是一块画不出桌面的 SpringBoard**。
606.098 与 730.375 s 两次心跳抓到 pid 1 的同一线程 `0x482` 停在 `_mach_msg2_trap` 上，
发的是 `id=0x20000000` 的 libxpc 消息：一条 64 B、`bits&0xff=0x12`（`MOVE_SEND_ONCE`）、
体里有 `CPX@`/`error`；一条 116 B、带复杂位与一个描述符、体里有 `rec_execcnt`/`req_pid`。
目标都是 send-once 权，所以 `sbport … kr=17` 只是 `MAKE_SEND` 对这种权利不适用，
**不是端口失效**。另一头 `sbport rp nm=a03 cnt=0 ql=5 seq=26→29→30 rn=a03 ms=1 sr=1`
（422–491 s，11 条）：SpringBoard 的回复端口 `seq` 两次上涨、`cnt` 恒为 0，
同段里它的主线程 `0x104` 正以 `rem=7c03/753b rn=a03` 做 MIG 式 send+recv——
**r223/r224 的「launchd 不回信」不成立于本靴**。但 SpringBoard 到 497 s 仍没画出桌面，
屏上一直是 backboardd 的进度条（`frame#0..#2 pixels=41586 box=305x887..755x1029`、
`colours ff000000/2262414…`、`client pid=58`），497.143 s `killsig pid=27 … spid=58
scomm=backboardd sig=9`，`upc` 落在 `___kill`+0x8 的 `svc` 上，497.336 s `exit t=9s rv=0x9`。
缺口再收窄：**不是「谁没回信」，而是 backboardd 为什么判 SpringBoard 不健康。
r226 把 kill 发信线程的完整寄存器/栈、SpringBoard 的收方向与 4 s 节拍上的 `gone` 收尾行一起读。**

更早的逐轮读数在 [`notes/development-log.md`](notes/development-log.md)。

## 距离 iOS 桌面还差什么

| # | 缺口 | 现状（真机读数） | 下一步 |
| --- | --- | --- | --- |
| 1 | **SpringBoard 活不下来：不渲染，约 497 s 被系统 shell 看门狗杀掉** | r225 读数（boot 14）：launchd（pid 1）线程 `0x482` 在 606.098 / 730.375 s 停在 `_mach_msg2_trap` 上发 `id=0x20000000` 的 libxpc 消息（64 B `MOVE_SEND_ONCE` 体含 `CPX@`/`error`；116 B 复杂消息体含 `rec_execcnt`/`req_pid`），目标是一次性回信权，`kr=17` 只是 `MAKE_SEND` 不适用。SpringBoard 的回复端口 `a03` 在 422–491 s 的 11 条读数里 `seq 26→29→30`、`cnt=0`，主线程 `0x104` 以 `rem=7c03/753b rn=a03` 做 MIG 式 send+recv，**回信是通的**。屏上始终是 backboardd 的进度条（`frame#0..#2 pixels=41586 box=305x887..755x1029`）；497.143 s backboardd（spid=58）`SIGKILL`，`upc` 落在 `___kill`+0x8，497.336 s `exit t=9s rv=0x9`，到 900 s 没再拉起 | r226：打 kill 发信线程的完整用户寄存器与原始栈字（`ulr=0x10086a144` 落在 backboardd `__TEXT` 之外，未符号化）、SpringBoard 收方向的队首消息、peer 节拍改 4 s 并补 `gone` 收尾行 |
| 2 | **屏幕上是黑的** | compositor 已连续 present：r225（boot 14）`frame#0..#2`、`copy#1..#3 bytes=9216000`、`scanout/retire done=presented=1..3`；但 2 304 000 像素里 2 262 414 个 `ff000000`，只有 `305x887..755x1029` 那块变了 41 586 个像素——那是 backboardd 的引导进度条，不是桌面 | 等第 1 项修好、SpringBoard 活下来画 UI；之后用色带校验几何与缩放 |
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
