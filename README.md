# UniXNU-clover

小米平板 4 8 英寸 LTE（2018）的 XNU 移植，代号 `clover`：4 GB / SDM660（Kryo 260，
ARMv8.0，无指针认证），跑原封不动的 **iPad7,1 / iPadOS 17.7.11 / 21H461** 4 KiB 页镜像。
配套固件仓库 `uefi-clover`（Mu-Silicium 移植），驱动与硬件常量在 `porting.toml`。

现状一句话：**显示链路出帧，SpringBoard 被 backboardd 的看门狗杀掉**——XNU 起来、
OS/SystemOS/Data 三个卷全挂、launchd 把守护进程拉起来；r202 让
`IOMobileFramebufferLegacy` attach + start，r205 往 `/chosen` 写了 `display-scale=2`
解掉 `FBSDisplayMode` 的断言，r206/r207 里 compositor 连续 present（4 帧、每帧 9 216 000
字节扫上屏、`done/presented=4`）。帧仍是全黑的（2 304 000 像素里 2 262 414 个纯黑），
因为画 UI 的 SpringBoard 被 backboardd 用 `kill(2)` 杀掉（r207：`_kill+0x140`、
`ns=-1 code=0`、CS 位不含 `CS_KILLED`；r208/r210 的 `sender upc` 逐位相同）。

r210 把「是不是卡在块 I/O」这条结掉了：那一帧的 flight 区记到死线前最后一笔读
（`t=900.664 s`、120 块、`ps=0x01f800f0` 无 CMD/DAT inhibit），CPU0 心跳一拍不落地走到
`t=897.566 s`，panic sink 也写得进去（内核环到 `[911.924696]`）；杀点就在环里：
`[457.346085]` backboardd(pid 58) 向 SpringBoard(pid 27) 发 SIGKILL（`cs=0x22000301`、
`ra=fffffff0076ffd7c`），杀之前 SpringBoard 5 条线程全在 TH_WAIT、没有一条 tid 等于 pid。
桌面就差这一件事：**SpringBoard 在等什么**。r211 想在杀点把每条线程的内核栈打出来，
这一帧没拿到：flight 区最后一笔是 `t=341.015 s` 的 8 块读（`lba=28308144`，**无后继**），
最后一拍心跳 `t=340.978 s` 之后再没有一拍——守卫被一笔没回来的读拿住，内核环因此停在
`[352.186939]`（`sync-out ok=1 pending=152`）。机器活到 913 s，8 颗 CPU 各自在
`ArmCpu.cpp:644/647` 的死线上落了一条 kind-20 记录（`cnt1≈0x414c…`、`fb=1`），panic 文本和
4 条驱动记录写上了卡，但 `panicTailRead` **一条也没产出、连状态行都缺席**——它唯一的早退
路径是 `gTeeReady` 门。于是 352→913 s 的 msgbuf 连同 r211 的线程栈一起丢了。
r212 让内核 probe 直接把文本推进驱动的 32 槽 RAM 环（不经块路径、不经 msgbuf、不经 tee），
panic 时随驱动环落卡；每条记录带 boot id、drain 进出各说一行、尾读去掉 `gTeeReady` 门。

r212 的靴号当场关掉了 r211 的歧义：这一帧是 1 号靴，驱动环 36 条里 35 条 boot 1，唯一一条 boot 0
的 `r184: panic drain wrote=4` 是上一靴的尾巴，report 窗 12 条 kind-20 也全是 boot 0——**本靴的
panic 一条都没写**。但这一帧没走到杀点：块路径在 `194.18 s` 就停住了，flight 最后一笔是连续
8 块读里的最后一笔（`t=181885 ms`、`lba=31076656`，Data 卷内 6.08 GiB 处，**无后继**），
最后一拍心跳比它早 12.99 s（`miss=13`），内核环停在 `[194.181939]`。SpringBoard 152.2 s exec
之后再无输出，所以 kind-25 probe 行是 0 条——通道没被检验，不是失败。**从 194 s 起就没人能写卡**，
panic 只是又一个受害者。r213 因此不再加探针，而是把「谁来拿主机」换成有界的票据：
等待者 60 s 放弃并报 `owner/stage`，持有者每进一段写一个 stage，CPU0 心跳看到 stage 75 s
没动就软复位、CMD12、清中断并抢回票；被抢的一方在下一个 chunk 头自觉收手。
一笔坏请求只毁一个请求，不毁整次启动；flight 记录同时补上 stage 与调用线程的 pid/comm。

r213 上机后票确实上了膛（`r213: host ticket armed, wait=60000ms stuck=75000ms`、boot 2），
但停点只是往后挪到了 **418.465 s**：flight 最后一批入口是 `t=407.460 s` 起 chronod 的连续
8 块读（`lba=28103552…`、`pid=133`），内核 tee 最后一行是 `[418.465029]`，之后卡上再没有任何
成功写入——没有 `guard stuck`、没有 `guard wait gave up`、panic 连 `drain enter` 都没写上
（report 窗 12 条仍是 boot 0）。r213 的看门狗本该在停滞 75 s 后落一行，它没落，说明卡死之后
连软复位 + CMD12 也救不回来（或看门狗根本没触发）。r214 因此把看门狗升级成完整的卡重初始化
（关时钟、断电、重新识别、恢复高速），并用 panic 的复位落点做一条不依赖卡的 1 bit 通道，
回答「看门狗触发过没有」。

## 距离 iOS 桌面还差什么

| # | 缺口 | 现状（真机读数） | 下一步 |
| --- | --- | --- | --- |
| 1 | **SpringBoard 被 backboardd 的看门狗杀掉** | r207 读数：`killsig pid=27 comm=SpringBoard ppid=1 sig=9 spid=58 scomm=backboardd ns=-1 code=0x0 cs=0x22000301 ra=fffffff0076ffd7c`。锚点与 `_psignal_internal` 的链接地址逐位相同（slide=0），`ra` 解成 `_kill+0x140`；CS 位不含 `CS_KILLED`，不是签名校验；backboardd 里有 `_pingSystemShellForCollectiveWatchdog:`、`watchdogPing: system app claims it's no longer alive`、`Killing %{public}@.`。r208 没有复现（环停 `[167.198224]`）；r209 走到死线但 sink 没写进去；r210 块路径健康到死线（flight 到 `t=900.664 s`、心跳到 `t=897.566 s`），sink 写入成功，kill 在 `[457.346085]`，杀前 5 线程全在 TH_WAIT、pc 落在 `DiagnosticRequestService` / `libswift_StringProcessing`，无主线程。r211 在杀点加了内核栈回流，但这一帧的守卫被 `t=341.015 s` 一笔无后继的 8 块读拿住，环停在 `[352.186939]`，panic 时 `panicTailRead` 又被 `gTeeReady` 门挡住——杀点的栈已经打进 msgbuf 却没落卡。r212 给出靴号与 probe 环，但这一帧更早停：flight 最后一笔是 `t=181885 ms` 的 8 块读（`lba=31076656`，Data 卷 6.08 GiB 处，无后继），心跳早 12.99 s 断（`miss=13`），内核环停在 `[194.181939]`；SpringBoard 152.2 s exec 后无输出，杀点（r210 是 457 s）没到，所以 kind 25 是 0 条。r213 的票上了膛且拿到 37 条 boot-2 驱动行，但停点仍被卡写光：flight 最后一批入口是 `t=407.460 s` 起 chronod 的连续 8 块读，tee 最后一行 `[418.465029]`，之后没有 `guard stuck`、没有 drain，panic 没有一条上卡 | r214 把看门狗从「软复位 + CMD12」升级为完整卡重初始化（关时钟、断电、重新识别、恢复 50 MHz 高速），并用 panic 落点（fastboot vs TWRP）做 1 bit 通道，回答看门狗到底触发过没有 |
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
