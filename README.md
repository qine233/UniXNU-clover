# UniXNU-clover

小米平板 4 8 英寸 LTE（2018）的 XNU 移植，代号 `clover`：4 GB / SDM660（Kryo 260，
ARMv8.0，无指针认证），跑原封不动的 **iPad7,1 / iPadOS 17.7.11 / 21H461** 4 KiB 页镜像。
配套固件仓库 `uefi-clover`（Mu-Silicium 移植），驱动与硬件常量在 `porting.toml`。

现状一句话：**屏幕开始出真正的新帧，SpringBoard 第二代活满了整个观察窗**——XNU 起来、三卷全挂、
launchd 把守护进程拉起来；boot 17（r228，死线 1800 s）里 pid 213 的 SpringBoard 从 ~500 s 一直
活着推进到 1771 s：发给 lsd 的请求等到了回信，卡点移到 backboardd；r180 在 1468 s 接受
`swap#4`、随后两帧全屏 `box=0x0..1199x1919`——653 s 无新帧的旧停滞被翻篇。

r228 读数（boot 17）：**三件仪器全部兑现，桌面卡点收窄为「发给 backboardd 的 `nm=3403` 没人回」。**
0 代 pid 27 在 497 s 被 backboardd `SIGKILL`，这次的 dump 走驱动 probe 环落了卡：
`rcdump t=497 n=64 tot=660` + 3 条 `rce pid=27 gen=0`（最后一条距 kill 不到 1 s，它被杀时还在收，
全程 660 条）。1 代 pid 213 出生接棒：主线程 tid=0x6ec 从 901 s 卡在 `_mach_msg2_trap` 发给
`115:lsd` 的 `nm=6003`，到 1522 s 换成发给 `58:backboardd` 的 `nm=3403`（`sbport cnt=0 ql=6
seq=3 rn=2227`）——lsd 回了信，它发出了下一个请求，此后一直活着到 1771 s 最后一拍，没人再杀它。
屏上 1468.3 s `swap#4 accepted`、`frame#3 t=1006s pixels=41517`（backboardd 进度条局部）、
`copy#4 217 ms` → `scanout#4 ok` → `retire#4 presented=4`，**1472/1482 s `frame#4/#5` 两帧全屏
`pixels=2301667 box=0x0..1199x1919`**。之后 1482 s 到 1800 s 死线之间屏上还有没有新帧被噪声挤掉：
512 行内核环里 `fl=…fs=apfs` 裸行 141 条 + `r170: exit` 循环重启 97 条占去约一半。watchdogd
987 s 退出（`rv=0x5 ns=2`）；1800.16 s 死线 panic 照章收场（ticks=335739）。

r227 读数（boot 16）：**环与锁存都工作；SpringBoard 换了三代，C 代健康活到 906 s，是 900 s
死线把一次可能成功的启动杀掉。**
第一代 A 出生即卡：~614.7 s 被 `SIGKILL` 触发 dump（`rcvsum`/`rcv0..31`/A 的 `killsig`/`exit`
都落在 617.04 s 起的可读窗口之外，A 没留下任何收记录）。B=pid 27 接着正常收：`rcv32 t=614
tid=0x712` 到 `rcv63 t=636 tid=0x748`，随后 636 s 起停在一个等待上 15.5 s，651.53 s 被
backboardd `SIGKILL`（`exit pid=27 rv=0x9`）。C=pid 229：653.24 s 锁存
（`sbtask gen=1 pid=229`），906 s 仍活着且在收（`sbm2 t=906 rem=6c03`、`sbport nm=6c03
…id=40000000`——boot 15 同款消息这次被取走）。但 906 s 仍无新 UI 帧：r180 帧线停在 653.2 s
（`retire#4 done=4` 之后无新帧，屏上还是 backboardd 进度条 `client pid=58`），随后 912 s
自家 900 s 死线 panic 收场。两个仪器缺陷：dump 一次性 guard 被 A 代 kill 消耗，B 代被杀时
无 dump；dump 边打印边读活环（64 条从 ~614.7 s 打到 649.46 s，晚打印的槽已含 B 代新记录，
且 620→636 s 的 16 s 空档是 printf 本身慢——其间 r217 五线程采样、watchdogd 崩溃、
r224/r225 每 4 s 节拍一切照常）。

r226 读数（boot 15）：**kill 现场拿到了——backboardd 传的就是 SpringBoard 的 pid，调用点仍是
「Killing %@」helper；回信在 480 s 停，之后它一直挂在一个等待上。**
532.091901 s backboardd（spid 58）对 pid 27 发 `SIGKILL`。发信线程的完整寄存器在环里：
`x0=0x1b`（27，正是被杀的 pid）、`x1=0x9`（SIGKILL）、`x2=0x1`，`pc=0x1d2975100`
（`libsystem_kernel` 的 `___kill`+0x8，`svc`），`lr=0x100763144`，而 `x20=0x1006f1000`
（backboardd 镜像基址），`lr−x20=0x72144`——与 r218 由另一条 slide 得到的 `0x72144`
是同一个返回点，即「Killing %@」helper 里第二处 `bl _kill`。`sp` 下十六个字
（`kraw0/kraw1`）按原样留在环里，不做换算。另一头：SpringBoard 的回信端口 5203 在 472 s
是 `seq=3`、480 s 是 `seq=11`，此后到 532 s 被杀一直 `seq=11 cnt=0 ql=5 rn=5203 ms=0 sr=0`；
r221 发送面在 480/508/525 s 都看到主线程 `0x104` 以 `rem=5903 rn=1d03` 反复进 `mach_msg2`
（r223 的 5903 端口上压着一条 `bits=0x20111211 sz=0x86 id=0x40000000` 的消息，没人取）。
**回信在 480 s 停了，然后进程停在一个等待上**；532.120 s `exit pid=27 t=10s rv=0x9`，
536.135 s `sbgone t=536 pid=27` 把死亡本身变成一条读数。屏上到 532 s 仍是 backboardd 的
进度条（`frame#0..#3 pixels=41586 box=305x887..755x1029`、`client pid=58`，
532.91 s 还在 present）。收方向探针在本靴可读窗口里一条都没有（内核日志尾段从 469.32 s
才开始）：这既可能是 32 行全局配额在窗口之前就用完，也可能是 `clover_sb_note_current`
只锁存第一颗 SpringBoard task（重启后门控恒假）——r226 没能把两者分开。
r227 改成「内核环里记录、kill 时倒出」，不再从收路径实时打印，并让 task 锁存跟随重启。

更早的逐轮读数在 [`notes/development-log.md`](notes/development-log.md)。

## 距离 iOS 桌面还差什么

| # | 缺口 | 现状（真机读数） | 下一步 |
| --- | --- | --- | --- |
| 1 | **发给 backboardd 的 `nm=3403` 没人回——SpringBoard 卡在它上面** | r228 读数（boot 17）：0 代 pid 27 于 497 s 被杀但 dump 落卡（`rcdump t=497 tot=660`，被杀瞬间还在收）；1 代 pid 213 活到 1771 s：lsd 的 `nm=6003` 在 ~1400–1522 s 间**等到回信**，卡点移到 `58:backboardd` 的 `nm=3403`（`sbport cnt=0 ql=6 seq=3`），无人再杀它，1800 s 死线收场 | r229（已刷，boot 18）：驱动 tee 负过滤砍掉 `fl=…apfs`×141 与 exit 循环×97 这半数噪声，让 1482 s 之后的帧可见；内核按 r224 样法采 backboardd（pid 58）线程与其收队列，回答「3403 在不在 58 队里、58 主线程卡在哪」 |
| 2 | **屏上有帧了，但还不知道是不是桌面** | r228（boot 17）：1468 s `swap#4 accepted`，1472/1482 s `frame#4/#5` 首次全屏（`pixels=2301667 box=0x0..1199x1919`）；r227 之前帧计数停在 653 s。1482 s 后内容被环噪声挤掉，未见后续帧 | 等第 1 项砍掉噪声后读 1482–1800 s 的帧序列；桌面成形前不动几何/色带 |
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
