# UniXNU-clover

小米平板 4 8 英寸 LTE（2018）的 XNU 移植，代号 `clover`：4 GB / SDM660（Kryo 260，
ARMv8.0，无指针认证），跑原封不动的 **iPad7,1 / iPadOS 17.7.11 / 21H461** 4 KiB 页镜像。
配套固件仓库 `uefi-clover`（Mu-Silicium 移植），驱动与硬件常量在 `porting.toml`。

> 这份 README **只放表格与当前状态**：逐轮的过程内容（取证、推翻、判读预登记）一律写在
> [`notes/development-log.md`](notes/development-log.md) 的 `§RoundNNN` 里，本文件只把结论收成一行。
> 表里 `✓` = 已做到并有产物，`✗` = 还没有，`—` = 这一格对本颗不适用。
> 历史行文里的「排期 3／4／5／6／7／8」自 r314 起以本表的**行名**为准（3 显示与 GPU、4 输入、
> 5 网络音频 USB、6 电源、7 块设备写吞吐、8 内核可复现）。

## 进度：从按下电源到桌面

| # | 步骤 | 状态 | 最新证据 | 出处 |
| --- | --- | --- | --- | --- |
| 1 | 固件 | 通 | `uefi-clover` 渲染菜单、读到 payload，3 秒后默认进 XNU | `porting.toml` firmware |
| 2 | 内核 | 通 | 入口走到 `_bsd_init`；时钟、抢占、系统调用都有读数 | dev-log §Round82、§Round187 |
| 3 | 平台服务 | 通 | AMFI 放行（`amfi-only-platform-code` + exec hook 两刀）、`ApplePMGRNub` 就位 | dev-log §Round83、§Round163–164 |
| 4 | 磁盘 | 通 | eMMC 高速读写 + ADMA2；APFS 的 OS 卷与 Data 卷都挂上 | dev-log §Round128–138、§Round159 |
| 5 | 用户态 | 通 | launchd 起一整套 iOS 守护进程，`Early boot complete` | dev-log §Round186 |
| 6 | 点亮屏幕 | 通 | 第一次 present：`swap#1/2/3 accepted`、`scanout#3 ok bytes=9216000`、`presented=3` | dev-log §Round206 |
| 7 | SpringBoard | **卡在这里** | boot 107 定名：主线程空等一笔没人应答的 PreBoard 数据迁移（`0x6003` `pr=0 to=0`、`migrationpluginwrapper` t=653 起）；r320 照 sheng 给 ADT 补 `/chosen/ephemeral-storage`，待 boot 108 读回 | dev-log §Round295–320 |
| 8 | 桌面 | 未到 | 画面那一格其实已通（r180 起 `presented=4`、copy/scanout/retire 全 ok）；欠的是 SpringBoard 离开 PreBoard——不是 GPU 也不是触摸 | dev-log §Round320 ① |

## 各驱动开发进度

十一颗 `drivers/*`，全部按 [`notes/unixnu-doc-conformance.md`](notes/unixnu-doc-conformance.md) §2
那八条文档条款逐颗判定过；「文档」那一列是 r314 两道闸门当场查的东西，不是自述。

| 驱动 | 这一格负责 | 接线（`board.toml` 行） | 文档现状：类形状 / personality / README+引用 / 宿主模型 / plist | 最新真机读数 | 下一步 | 阻塞于 |
| --- | --- | --- | --- | --- | --- | --- |
| **CloverSdhci** | eMMC 通路 + 卡上日志环（读数的唯一通道） | archives `:69`；`clover-emmc=21` `:1968` | ✗ / ✗（已说明）/ ✓ / ✗ / ✗（已说明） | 两个环落 logdump（kind-21 判据 64 槽、kind-22 内核日志 512 槽）；boot 102 最后一条 block 记录 `comm=chronod` 与 panic marker 同毫秒 | 把 completion 判据、环算术、tee 配额抽成纯函数配宿主模型 | 无（能用，缺的是测试不是硬件） |
| **CloverBlock** | 把真 GPT 盘交给 IOKit，挂 OS/Data 卷 | archives `:70`；`clover-apfs=3` `:1967` | ✗（用现成类）/ ✗（已说明）/ ✓ / **✓**（`BlockCore.h` + `test-clover-block.py` PASS）/ ✗（已说明） | OS 卷挂上且 owner 审计放行；Data 卷 `ENOSPC` 消失 | 用真机验这块纯层：boot 103 的 `boot os=/system=/data=` 三对 LBA 要和 r316 模型的走查同形 | 无 |
| **CloverPMGR** | 开机链的调度点（所有仪器从它后面起跑）+ 发布 `ApplePMGRNub` | archives `:68`；`clover-pmgr=2` `:1261` | ✓（`OSDefineMetaClassWithInit`，父类按名运行期解析）/ ✗ / ✓ / ✗ / ✓ | 每靴都在；`No PMGR?` 自 r83 起消失 | 让它做第一颗真走 personality 的驱动 | ADT 没有 provider 节点（conformance §5 前提 1） |
| **ArmCpu** | CPU、时钟、SMP、看门狗、无人值守死线 | archives `:62`；`armcpu-timer-intid=19` `:1242`、`armcpu-smp=1` `:1256`、`armcpu-self-ipi=1` `:1257`、`clover-unattended=1800` `:2597` | ✓ / ✓（基仓 patch）/ ✓ / — / ✓（覆写基仓模块的四件套齐全） | boot 88–102 十二靴逐字段复现八核上线：`real=8 max=8 reg=8 start=7 run=8`，IPI `sent/recv` 差约百；boot 100 是 `armcpu-smp=0` 控制轮 | 无（这一格已够；复位时机那一坎已证明不属核数） | 无 |
| **NativeMobileDisplay** | legacy display / GOP 出帧（屏幕那一步） | archives `:67`；`offline-legacy-*` `:2280-2287` | ✓ / ✗（已说明）/ ✓ / — / ✗（基仓也没有） | 原生 `IOMobileFramebufferLegacy` 已链接并构造到 phase 1；r206 第一次 present 成功 | 桌面欠的「画面本身」在这一格，仍 CPU-first | 帧内容不由这颗驱动产生，属主线 |
| **NativeHidInput** | 自有 `IOHIDDevice` 进门并发一帧报告 | archives `:67`（`build-clover-gop-legacy-display.py` 调 `build-clover-hid-input.py`）；`offline-hid-input=1` `:2325`、`offline-hid-home=1` `:2326`、`sheng-touch-hid=1` `:2337` | **✓（唯一整块持有条文类形状：标准宏 + 从原始 kernelcache 派生真实 ABI）** / ✗（匹配记录是 Apple 自己的 personality）/ ✓ / — / ✗（正确形状） | AppleM68Buttons 早崩已定因并回退（boot 41/42）；r259 起自有设备真发一帧数字化仪报告 | 与 CloverInput 同一格：等面板出数 | 触摸无应答（见下面两颗） |
| **NativeKeyStoreEvents** | keystore 事件（「无 keybag、开机即解锁」这一问的答案） | archives `:67` 内；`clover-aks=1` `:2379`、`offline-key-events=1` `:2372` | ✓ / ✗（已说明）/ ✓ / — / ✗（基仓也没有） | boot 82：`opens=22→27 calls=80→99 refused=0`；boot 85：`stage=3 opens=100 calls=442` | 已发布并测到，本颗无缺口 | 无 |
| **CloverInput** | 触摸 / QUP v2 I2C-3 电气与应答普查 | archives `:71`；`clover-input=3` `:2422` | ✗ / ✗（已说明）/ ✓ / **✓**（`InputCore.h` + `test-clover-input.py` PASS，263 检查）/ ✗（已说明） | boot 104（r317 那一支）`status=10000c8 xfers=4 timeout=0 nack=4 verdict=102` 两拍逐字同 ⇒ 仍是「已知不应答」；**带 r318 原字的首帧尚未落地**（cache 已换 `02b845b4…`） | 读 boot 105 的两行，按 dev-log §Round318 ⑩/⑬ 预登记判；再上电 `pm660l_l3` / `pm660_l11` 问地址 | 读回要人工进一次 TWRP：boot 104 panic 后没有自动落 recovery（panic 路径自己没写 BCB） |
| **CloverPower** | SPMI 仲裁器 + PMIC 轨读数 | archives `:72`；`clover-power=1` `:2489` | ✗ / ✗（已说明）/ ✓ / **✓**（`test-clover-power.py` PASS）/ ✗（已说明） | **未上机**：盘上最后一帧仍是 round183 的 `not a v5 arbiter (03000000)`；boot 104 的 tee 环只覆盖 557 s 之后，而 r313 的报告在开机早期一次印完 ⇒ **这一靴证不到它，不许读成拒读** | 按 [`notes/power-bringup.md`](notes/power-bringup.md) §8 七支定档，再升 mode 2（revid）、mode 3（读 `0x46` 开轨） | 板子不在家 |
| **CloverDsp** | ADSP / SMEM 共享堆通路 | archives `:73`；`clover-dsp=3` 在 `:2534` 是**注释状态** | ✗ / ✗（已说明）/ ✓ / **✓**（`test-clover-dsp.py`）/ ✗（已说明） | `$TOC` / `$PRT` 自校验通过，判决行没上屏 | 有边就上 GLINK/APR；没镜像先压 `vadsp` 电源 | 这颗会读堆，参数整颗摘掉（留着会毁掉当靴读数） |
| **CloverGpu** | Adreno 512 的电源/时钟普查（只读） | archives `:74`；`clover-gpu=1` `:2441` | ✗ / ✗（已说明）/ ✓ / **✓**（`GpuCore.h` + `test-clover-gpu.py` PASS）/ ✗（已说明） | **boot 104 两行都到**：r311 `mem_acc map=1 answered=1 raw=00000000 l1=0 corner=2 gpucc=1`（`0x01fcf004` 第一次应读，读回零）＋ r317 `shape=list-never-written named=13/13 comparable=8 agree=1 differ=7 dead=0 map_ok=1 control=1`（两拍同形，`cx_on=1 gx_on=0` 不变） | 按 [`notes/gpu-bringup.md`](notes/gpu-bringup.md) §5.13 五支读 `answered=1 raw=0` 那一支的下游（corner=2 而 l1=0），并等一次带正对照的写 | 供电那一头在 RPM 手里；桌面 CPU-first，本线只量不写 |

「personality」那一列全 ✗ 是同一件事：本板 ADT 的 `/arm-io` 只有 `intc` 一个孩子，spmi／sdhci／gpu／i2c／dsp
都没有节点，`IOPlatformDevice` + `IONameMatch` 谁都匹配不到；补齐它的三个前提与顺序在 conformance §5，
**要上机**，排在桌面这一格之后。`plist` 列的「✗（已说明）」意思是文档 `xnu-drivers.md:14` 把 `Info.plist`
当 personality 的记录——没有 personality 就没有该记录的东西，闸门要求 README 把这句写出来，写不出来就 FAIL。

## 距离 iOS 桌面还差什么

| # | 缺口 | 现状（一句话） | 下一步 | 出处 |
| --- | --- | --- | --- | --- |
| 1 | SpringBoard 等一笔没人应答的 PreBoard 数据迁移 | boot 107 把停点定名了：主线程空等迁移往返（`migrationpluginwrapper` t=653 起、端口 `0x6003` `pr=0 to=0` 挂 140 s、普查 `wait=19 run=1`），既非 GPU 也非触摸；与 sheng 的 ADT 只差 `/chosen/ephemeral-storage` 一条 | r320 照 sheng 把 ephemeral 解耦到 `writable_root`，`build payload` 已证它进 ADT（14,384→14,424、u32(1)，闸门 EXIT=0）；**等 boot 108 冷刷读回**，两支预登记见 §Round320 ⑤（进桌面 / 仍停迁移⇒假设排除） | dev-log §Round295–320 |
| 2 | 触摸没有任何应答 | QUP v2 I2C-3 电气通，两个候选地址全 NACK | 依赖 3：先把面板轨上电 | dev-log §Round181、§Round312 |
| 3 | PMIC 轨读不出来 | r313 起「不在 v5 带内」不再是拒读的理由（地板图改成按版本字选表） | 刷 `round313` / `round314` 镜像定档 | [`notes/power-bringup.md`](notes/power-bringup.md) §8 |
| 4 | ADSP 没起 | SMEM 读通，判决行没上屏 | 有边上 GLINK/APR；没镜像先压 `vadsp` | dev-log §Round183 |
| 5 | watchdogd 每次起就崩 | 已定名且非瓶颈：boot 76 带着这枚 brk 活到死线，sheng 零引用 `IOWatchdog` 也进桌面 | 先量 `AppleARMIODevice` 在本板有没有平面；不在当前排期 | dev-log §Round199–200 |
| 6 | GPU/Metal 一个都没有 | 分支清白、债在电：`gx_collapse=0` 而 `gx_on=0`，那颗 LDO 从复位起没人按厂商表配过 | 只读侦察到 MEM_ACC 那一格；桌面 CPU-first | [`notes/gpu-bringup.md`](notes/gpu-bringup.md) §5.11–5.13 |
| — | 「读不到为什么结束」（独立的一道坎，不是主线症状） | 「进了 panic sink、文案被卡拒」到 boot 104 又退一格：**sink 第一笔（marker）都没落地**，misc 因此没人写 ⇒ 不自动进 TWRP（misc 现读全零**不是**证据，进 recovery 会吃掉 BCB）；GPU 线已由 r294 / r294b 两个控制轮从复位嫌疑里摘出去 | 落地即测的物理量（传感器 + `rawdump` 头部）已做进宿主脚本 | dev-log §Round294、§Round301–310 |

## 已完成（都有真机读数）

| 事项 | 证据 |
| --- | --- |
| 固件与菜单 | `uefi-clover` 渲染菜单、读到 payload，3 秒后默认进 XNU |
| XNU 引导链 | 入口色带 + panic 屏走到 `_bsd_init`；时钟/抢占/`select` 期限都有读数 |
| AMFI / PMGR | `amfi-only-platform-code` + `ApplePMGRNub`，`No PMGR?` 消失 |
| EL0 与多进程 | mockfs 根上 pid 1 进用户态；`fork`/`wait4`/`execve` 全通 |
| APFS 根与 SystemOS | OS 卷挂上、owner 审计放行、exec 时 union 到 `/`；dyld 缓存目录/主缓存/`.01` 全 `err=0` |
| Data 卷 | 1 GiB 种子、UUID 钉住（r198），`ENOSPC` 消失 |
| eMMC | 高速 8 线 + CMD18 批量；ADMA2 读逐字比对、写只准回写自己读到的字节；r192 把 cache flush 交还卡 |
| launchd 与守护进程 | `exec`/`exit` 成对、`Early boot complete`、SpringBoard 被反复拉起且不再被杀 |
| 八核 SMP | boot 88–102 十二靴、跨十枚镜像逐字段复现 `real=8 reg=8 start=7 run=8` 且都在收 IPI |
| 显示栈首次出帧 | r206：`swap#1/2/3 accepted`、`scanout#3 ok bytes=9216000`、`retire#3 done=3 presented=3` |
| 读数通道 | 两个环落在 logdump（p56）：kind-21 判据 64 槽、kind-22 内核日志 512 槽，一次 `dd` 1216 扇区读回 |
| 无人值守 | `run-clover-unattended.sh`：冷刷 cache → 死线 → 预置 BCB 落 TWRP → 读回日志，退出码 0 |
| 可复现内核 | 同树两次构建归一化后同一枚镜像；r314 两趟 amfi 件不同（`97639a36` vs `4b687d6b`）而归一化都是 `3fd3032b`，`--prove` PASS |
| 驱动符合性闸门 | 十一颗驱动全部点名文档并带能落地的 `docs/**.md:行号`；12 枚归档、22 枚对象核过五字段、导出符号在链接好的内核里 |

## 待完成

| 事项 | 现状 | 出处 |
| --- | --- | --- |
| 第一颗真走 personality 的驱动 | 三个前提各欠一件：ADT 发布节点、顺序怎么表达、归档补 force_load | conformance §5、§7 未结 1 |
| 固件那一层的文档对照 | `docs/drivers/uefi-drivers.md:7-13` 在 `uefi-clover`，同一套办法搬过去即可 | conformance §7 未结 4 |
| payload 容器不逐字节可复现 | ADT 的 `/chosen/random-seed` 每次重抽（内核本身可复现） | dev-log §Round177 |
| 构建顺序反着 | `unixnu` 先跑 archives 再跑 XNU，身份戳会删掉 object 让 `nm` 失败；r186 起用一次性 object 预 pass 绕过，没有修 | dev-log §Round186 |
| 补丁回放名单手写 | 新增补丁若碰到排序在前那条的上下文，必须手动加进 `build-xnu-4k-baseline.sh` 的「先反向」名单 | `AGENTS.md` |

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

| 闸门 | 查什么 | 跑在哪 | r317 实跑 |
| --- | --- | --- | --- |
| `scripts/check-clover.py` | 板级预检 + ADT/DT 常量 + 补丁与探针形状 + 驱动宿主模型 | 仓库 | EXIT=0，`drivers documented: 11, base-repo drivers exempt: 0`，12 条 notice 全指向未构建的产物；`test-clover-block.py` 与本轮新增的 `test-clover-gpu.py` 都被这道闸门**跑**而不只是断言存在 |
| 同一脚本的产物那一半 | 12 枚归档逐枚走对象、核 `docs/drivers/xnu-drivers.md:106-116` 那五个字段、再到链接好的内核里验每颗驱动的导出符号 | **必须在 `out/clover/tree` 里跑** | EXIT=0，`12 archives, 22 objects ABI-checked, kernel read`；`exempt: 11` ——组装树把基仓的 11 颗 `drivers/*`  symlink 进来，r316 起文档闸门只判本仓 authored 的那些 |
| 同一脚本的文档那一半 | 有源码的 `drivers/*` 必须有 README；README 必须有一段点名文档并至少引一枚**能落地**的 `docs/**.md:行号`；没有 `Info.plist` 的必须说出理由；禁止「本仓 `.cpp`:数字」这种形状 | 仓库即可 | 负向测过：CPU type 改 `0x1000007`、filetype 改 2、换假符号名都报错，带路径的厂商引用不误报；r316 新暴露的一条：本仓 `.cpp` 引用被 `BlockCore.h` 取代，README 改用符号锚点 |
| `scripts/verify-clover-payload.py` | 归档对镜像：卷内形状 + 逐轮探针断言（含标题必须带双引号，那是 boots 35/36 的转义防线） | 仓库 | EXIT=0，48 行 OK、0 FAIL；`SHNGROOT` 那一枚针脚换成了走查的拒绝标签——优化器把八枚 ASCII 折进立即数，镜像里没有那串字了 |
| `scripts/verify-userdata-window.py` | 刷 userdata 前的宿主闸门 | 仓库 | 本轮未涉及 |

产物缺席只发 notice，**不会静默通过**——那一格要抓的正是无 `force_load` 链接下「成员安静消失」
（`configs/clover_defconfig:70` 是被 `build-xnu-4k-baseline.sh:1002` 原样追加的）。

## 相关文档

- [`notes/development-log.md`](notes/development-log.md) — 每一轮的设计、构建与真机读数，完整历史（过程只写在这里）。
- [`notes/unixnu-doc-conformance.md`](notes/unixnu-doc-conformance.md) — 十一颗驱动逐条对照 UniXNU 驱动文档的判定、
  仍未结的缺口（按能推进的顺序排），以及 r314 定下的引用规矩（冻结目标用行号、本仓可变 `.cpp` 用符号锚点）。
- [`notes/clover-device-tree-sources.md`](notes/clover-device-tree-sources.md) — 每个常量的来源，含 splash 地址的推翻过程。
- [`notes/power-bringup.md`](notes/power-bringup.md)、[`notes/gpu-bringup.md`](notes/gpu-bringup.md)、
  [`notes/wifi-bringup.md`](notes/wifi-bringup.md) — 各条线自己的判读预登记；显示那一格的底稿在
  [`drivers/NativeMobileDisplay/README.md`](drivers/NativeMobileDisplay/README.md) 与 dev-log §Round179–206。
- [`porting.toml`](porting.toml) — 上游要求的 13 个 area 与 findings。
- [`AGENTS.md`](AGENTS.md) — 仓库边界与闸门（含「README 只放状态、过程进 dev-log」这条）。

## 许可证

BSD-2-Clause-Patent，与 UniXNU 一致。`scripts/build-arm-driver.py` 移植自
UniXNU-gta4l（NoNameBuilder），采用同一许可证。
