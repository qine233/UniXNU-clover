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
| 7 | SpringBoard | **卡在这里（backboardd）** | boot 109（r322）把卡点从 chronod 前移到 **backboardd（pid 58）**；**boot 110（r324 读回）再证一次：`clover r229: bb t=607 pid=58 hops=4`（boot 109 是 hops=2，债在加深）、`r262 census pid=58 threads=19 wait=18 run=1`，停点 ~612 s**；**boot 112（r326 读回）同形第三次：`r229: bb t=882 pid=58 f=1 hops=4`＋`r262 census threads=18 shown=18 wait=17 run=1`，封行 `clover r326: bbnow` 两通道都落地（中转 seq 66、内核环 885.179096）——位置那一半结掉，读数给的是 `hn=4 rem=0 res=0`，四列全 `-`**；**boot 111（r325 读回）同形再来一次：`r229: bb t=712 pid=58 f=1 hops=4`＋`r262 census threads=19 shown=19 wait=18 run=1`＋`r271 real=8 reg=8 start=7 run=8`，停点移到 712 s；那靴唯一一次 fire 的 burst 逐行打到第 7 行（`r299: smp`）就在同一秒进 panic sink，hop 属主那一行（`r325: bbnow`）因此零行＝预登记 B①「跑过但没算到那行」，不是「没有债」**；**boot 113（r327 读回）第四次同形：`clover r327: bbnow t=565 pid=58 fired=1 hn=3 state=hop_unresolved named=0 asked=1 o0=null o1=null o2=fail o3=- r0=0 r1=0 r2=7513 r3=0` 两通道都落地（中转 seq 59／内核环 565.436709，位置仍夹在 `r234: sbown` 与 `r262: census` 之间）⇒ boot 112 那四枚 `-` 分成三种成因：两枚「这一跳没有发送对象」（`r0=r1=0`）、一枚「问了没答」（`r2=7513`→`fail`）、一枚「这一跳没走」（`hn=3`⇒`o3=-`）；同靴同帧的 `r221: sbm2 … rem=2e03 … ro=58:backboardd` 是正对照 ⇒ 解析器与本空间都成立，缺席来自「封行只问发送列」而不是「backboardd 不欠谁」**；**待发：boot 114＝`round328-v0.7.39`（`clover r328: bbrcv` 第一次问收方向，判据 §Round328 ⑥ A–D），boot 115＝`round329-v0.7.40`（`dispstate` 周期行，§Round329 ⑧），r331（v0.7.42，往外数发送权持有者，§Round331 ⑦ S-A…S-E）最早 boot 116——板子不在，这三枚都还没上过机** | dev-log §Round323–325、§Round324 读数③、§Round325 读数①–③、§Round327 读数①–⑧、§Round328 ⑥、§Round329 ⑧、§Round331 ⑦ |
| 8 | 桌面 | 未到 | **画面这一格第一次真通了**：boot 108 `r180 client#` 0 行 → boot 109 五个 `IOMobileFramebufferUserClient` 进门（86/87/58/56/74）、`swap#1→#3 accepted` + copy 9.2 MB→scanout ok→`presented=2` 全跑通。但交出来的仍是 `box=305x887..755x1029` 的一个小黑盒（整屏 2.26 M 纯黑 + 41543 非黑），presented 停在 2 后静默——**是进度盒不是桌面**；欠的是 SpringBoard 只交出这一个 view + backboardd 那笔同步往返 | dev-log §Round323 ③④⑤ |

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
| 1 | SpringBoard 那笔同步往返没人回（boot 109 起＝backboardd） | boot 107 迁移（`0x6003`）→ r320 ephemeral 让 boot 108 挪到 chronod → **r322 revert ⇒ boot 109 读回：迁移真跑、chronod 那格过、显示路第一次整条转起来（5 帧缓冲客户端 + swap#1→#3 + presented=2）但主线程停 backboardd、只画 450×142 黑盒不是桌面** ⇒ **boot 110（r324 读回）再量一次：backboardd 仍阻塞、`hops` 从 2 涨到 4，债更深不是解开；那笔阻塞的属主名仍没拿到** ⇒ **boot 111（r325 读回）第一次把「没拿到」变成逐行读数：那靴唯一一次 fire（`r229: bb … f=1 hops=4`，t=712）从 `r229`→`r234 sbown`→`r262`→`r270`→`r271`→`r299` 一路打到第 7 行就停在同一秒进 panic sink，`clover r325: bbnow` 零行——缺席是预登记 B①（跑过、没算出那行＝位置问题），不是「没有债」；显示侧多走一帧（`frame#0…#5`、`presented=4`、`swap#4 accepted`）但仍是同一枚 `box=305x811..970x1108` 的黑盒** | **r324 手2（逐拍把债主梯子挪到 backboardd）没落地，而根因是本轮自己档案里那句诊断说反了**：闸其实开过——`latch t=603 pid=58`→`r229 hops=4` 都在卡上，缺的是「闩上之后没有下一拍」：那靴只有 t=181/301/421/547 四枚 `r308: beat`（每枚都 `target=0`，因为闩锁在 603 才开），下一枚排在 667 而板子 ~612 就没了——**120 s 的节拍慢过它要判的那 ~9 s 窗口**（第十三层）。**r325（已读回 boot 111）＝第 (乙) 条**：把 hop 属主在**算出它的那一刻**就中转出去——`clover r325: bbnow` 一次 fire 一行、带 o0..o3 全链、先中转后 printf，不再等任何时钟；它没落地，因为它挂在 hop 循环之后，而那靴那次 fire 从没走到 hop 循环（`bbhopo`/`bbhop`/`r300: done` 全零）。**r326＝把同一行挪到已被证明落地的位置**（已发 v0.7.37，待发 boot 112）：`clover r229: bb` 那行的紧后面（`hn`/`hrem[]` 在 `task.c:13592` 就填好了，`hops=` 已经在那一行里；同毫秒带里 `cen-in`、`smp-pre` 两枚中转都到了卡上）。**r326 已发（v0.7.37，`round326-v0.7.37-payload.img` `820b7782…`，双闸门 EXIT=0／verify 51 OK 0 FAIL，内核 `6822bee8…` 由 `payload.json` 自证是归一化那一枚）＝那一行现在封在 `clover r229: bb` 的紧后面、四个属主名由封行自己解析**；一处对预登记的偏离已记账：不再搬 r324 的闩锁，本地量、列名 `res=`（`set=` 已有 r324 一个写手）。**boot 112 已读回：行落地（两通道），`hn=4` 而 `rem=0`、四列全 `-` ⇒ 这一跳的远端端口是 `MACH_PORT_NULL`，没有发送对象可命名；同一串里 `r229: sbport nm=2f03 ms=0 sr=1`＋`r234: sbown own=58:backboardd` 把它说成「backboardd 在自己的收口上等一封没人发的消息」。四枚 `-` 的成因还分不开（远端 0／远端等于本地／问了没名字），且 `hloc=x[3]>>32` 那一列当时被写成「残值」——这句在 boot 113 被撤回（同一列在 r221 行里叫 `loc=`、且被同一次解析命名），未证不等于已证。**r327 已发（v0.7.38，`round327-v0.7.38-payload.img` `9f53fa5d…`，双闸门 EXIT=0／verify 51 OK 0 FAIL，内核 `5f07606d…` 由 `payload.json` 自证）＝那一枚封行自己说清缺席是哪一种：`o0..o3` 用成因字面量 `null`（远端端口为 0）／`fail`（问了、名字空间走穿、没属主）／其余为属主名，`res=` 换成 `named=`/`asked=` 两枚计数，`state=` 加 `hop_null` 一档，并带上 `r0..r3` 四枚原字。一处对预登记的偏离已记账：**不做 `self` 这一档**——判它要用 `hloc`（`x[3]>>32`；boot 112 当它是残值，boot 113 说那未被证明），所以自环改成照名字读（`58:backboardd` 出现在 `o0=` 里就是自环），本轮封行不再问 `hloc`。boot 113 已读回（第四次零人工落回 TWRP）＝预登记 C 支：行落地、`hn=3`、`asked=1 named=0`、`o0=null o1=null o2=fail`、`r0=r1=0 r2=7513`。这一支没有把「backboardd 不欠谁」读出来，反而抓到封行自己的一处问错列：`x[3]` 低半是**发送**方向，而同一帧里 `hrn=x[5]>>32` 才是**收**方向（同一靴的 `r221: sbm2` 用同一个解析器把 `rem=2e03` 命名成 `58:backboardd`，是解析器成立的正对照）；两枚 `null` 的形状＝纯收的调用没有发送目标。顺带撤一条旧论断：r326 ④-1 那句「`hloc=x[3]>>32` 是调用者残值」是从 boot 112 的四枚 `-` 推出来的，未证——同一列在 r221 行里叫 `loc=` 且被同一次解析命名。**r328 已发（v0.7.39，`round328-v0.7.39-payload.img` `3a9eee3e…`，双闸门 EXIT=0／verify 51 OK 0 FAIL，内核 `0f666cf8…` 由 `payload.json` 自证；boot-args 一字未动 ⇒ boot 114 与 113 并排可读）＝封行在同一毫秒段带里补问收方向**：新行 `clover r328: bbrcv` 对 `hn` 枚 hop 各问 `hrn[i]=x[5]>>32`——属主名（同一函数、同一 task）＋那枚口的 `imq_msgcount`/`srights`/错误码，列 `c=`（名字）`v=`（四枚原字）`q=`/`s=`/`e=`，缺席按成因分 `null`/`fail`/名字，`rstate=` 四档 `rcv_none`/`rcv_owner`/`rcv_unresolved`/`rcv_null`。对预登记记三处偏离：①**不改** r327 的 `state=` 判据（那行逐字留作 boot 112/113 的两通道对照；把收列折进它＝一个词同时指两列），改成 r328 自带 `rstate=`/`rnamed=`/`rasked=`；②原字列从 `rcv=` 改名 `v=`，和 `c=` 配成 name/value；③翻译腿走已有的 `clover_sb_port_mail()`（带 `int *err`）而不是 `clover_sb_port_state`，因为「helper 的 0 与真正的 0」必须靠返回码分开（r303）。本轮另一处自我修正记进闸门：契约针脚原先钉的是 `"rcv_unresolved" : "rcv_null"` 这枚**跨行**子串，源码为守 84 列把三元折成两行 ⇒ 本轮被自己的契约判红；改成两条字面量各自必在——**换行是排版不是语义**（第十八层）。待 boot 114 读回：四支 A–D（A＝任一 hop 的 `c=` 给出名字，再看那槽 `q>0`＝信躺口里没被取／`q=0 且 e=0`＝没人发；B＝`c` 全 `null` 且 `v` 全 `0` ⇒ `x16==-47` 那道闸筛错对象；C＝`rstate=rcv_unresolved` ⇒ 先对照 `v=` 原字判「另一个空间」而不是先判解析器坏；D＝`r327: bbnow`/`r319: last`/`r281 … armed=1` 应在、桌面格读 `frame#`/`presented=`/`box=`）预登记在 dev-log §Round328 ⑥。**r329 已发（v0.7.40，`round329-v0.7.40-payload.img` `e9c41a1f…`，内核 `d340ed02…` 由 `payload.json` 自证，双闸门 EXIT=0／verify 51 OK 0 FAIL，boot-args 与 r328 逐字节相同）＝纯宿主轮、未刷写**：boot 113 之后桌面那一格说不出「backboardd 不再问了」还是「卡片自己没预算印了」，因为本端口关于这颗客户端的每一行都只在首次目击时印一次，而 r180 那份 96 行预算已被 boot 113 吃掉 55 行。对照 sheng 的结论有四条落在盘上：那一靴的等待本身有答案（`-> 00000000`＝成功，swapId 到 4 没第 5 次）；selector 55 的 `e00002c7` 是 stock（出厂 vtable slot 202 那枚无条件返回的桩 ⇒ `sheng-vsync` 既非因也非药，`IOMFBDisplayRefresh` 无读者）；`r252: miss` 那三枚属性 sheng 也不发布（红鲱鱼）；sheng 多的其实是**调用者**——它的 `BootLog.c` 把 round180 vendored 的四枚显示报告 helper 全拉进一份报告，这四枚在本仓零调用者。本轮据此加一行周期读数 `clover r329: dispstate ms= fires= seen= client= calls= fails= regall= regs= grew= hot= s6= done= presented= frames= vs= log=/ logdrop=`：挂在显示驱动自己的工作循环上（30 s，不经 boot-arg，因为挂在参数上的节奏不能用来读那枚参数控制的东西），两枚触发点一个生产者（节拍＋首个客户端 interpose 那一枚，后者是「循环挂了 vs 没人调用」的判别器），缺席按成因各印 `-`，预算被拒自数，320 B 装不下时另发 `dispstatecut`。两处本轮自己抓住的错：局部 `presented` 与同行缓冲重名把 archive 编译判死（改名 `shown`）；board.toml 卡上标题原写「wires two of them」而那四枚 helper 至今无人调 ⇒ 已改成「另建同源报告、helper 的债留着」并重建负载（标题进 `unixnu.toml`）。顺带一枚宿主发现：ADT 的 `adt_sha256` 每建必变——差异全在 `/chosen` 的 `random-seed` 那 64 字节，清零后 r328/r329 逐字节同 `d7eaf753…` ⇒ §Round324 那句「ADT 与 r322 逐字节同」按 `adt_sha256` 判不出来。**下一刷仍是 `round328-v0.7.39`（boot 114 读那对债主行不被混淆）**，r329 的 `dispstate` 四支 A–D 预登记在 §Round329 ⑧。**r331 已发（v0.7.42，`round331-v0.7.42-payload.img` `e7b14b6a…`，`payload.json` 自证 `kernel_sha256 1e5871a2…`/`payload_kernel_sha256 0692a048…`，`kernel_args` 与 r328 逐字节相同，双闸门 EXIT=0／verify 51 OK 0 FAIL，纯宿主轮、未刷写）＝把「谁持有这枚端口的发送权」做成往外数的两条腿**：`clover r331: bbsend` 一行两形状（列名一致，只把 `obj=%llx` 换成 `obj=0`），腿 A 走内核反向表 `ipc_hash_lookup` × pid 1..400（`ipc_hash.c:173-175` 写明**只有纯发送权进表** ⇒ 它是下界，必须跟 `sr=` 并排读，`hits<sr` 本身是读数），腿 B 走等待者自己的 entry 表比 `ie_volatile_object`（`ipc_right.c:176` 那枚 volatile 字段）⇒ 能认出「那枚发送权是 backboardd 自己的」；`scan=` 一种「没得到读数」占一枚字面量（`no_cand`/`no_space`/`xlate`/`notport`/`no_srights`/`own_only`/`hash_only`/`holders`/`no_holder`），`cand=`/`debt=` 在所有 hop 上数（一行只挑 `debt` 第一枚），`probed=`/`walked=` 把两腿的地平线印在旁边，对象全程被 `ip_reference` 引用住；明细行 `bbsh`/`bbown` 各带 `ty=` 与 `raw=`（`IE_BITS_TYPE_MASK` 只有 16..20 位，EX_RECEIVE 是第 21 位，所以「读作 send」这件事写出来不藏）；三枚 tee 类 + `r331` 进每-pass 上限豁免链（**被上限吃掉的 `hits=0` 与没数过的 `hits=0` 必须是两种读数**）。对照 sheng 是阴性：它的 `patches/xnu/*.patch` 只碰 kern_exec／processor／sched_clutch／pexpert，`task.c` 整枚没动。列名全用新词（`scan=`/`ownnames=`/`walked=`…），因为 boot 100 那格教训同样适用于名字本身，闸门明文禁止复用 `state=`/`own=`/`ent=`/`set=`/`res=`。**r331b（同代、不另发版）＝把闸门自己的 flake 修成回归钉**：CloverPower 宿主模型的假 PMIC 用 `usleep (50)` 排节拍、而 `pa_wait()` 按轮询次数判超时——答案的延迟与读者的耐心是两把尺子，满载时那条断言红在这台机器的调度上；改成 `sched_yield` + `rearm()`/`serve()` 之间的 `channelLock` + 满载 sizing 的 `kPolls`，运行器从此每轮跑空载与 `loaded:<nproc>` 两趟，四条新针脚逐条负测过（证据 `~/clover-out/round331b-evidence/`）。r331 的首枚读数靴判读 S-A…S-E 预登记在 §Round331 ⑦ | dev-log §Round323–325、§Round324 读数②、§Round325 读数①–⑤、§Round326 读数、§Round327 ①–⑥、§Round327 读数①–⑧、§Round328 ①–⑥、§Round329 ①–⑧、§Round331 ①–⑧、§Round331b ①–⑤ |
| 2 | 触摸没有任何应答 | QUP v2 I2C-3 电气通，两个候选地址全 NACK | 依赖 3：先把面板轨上电 | dev-log §Round181、§Round312 |
| 3 | PMIC 轨读不出来 | r313 起「不在 v5 带内」不再是拒读的理由（地板图改成按版本字选表） | 刷 `round313` / `round314` 镜像定档 | [`notes/power-bringup.md`](notes/power-bringup.md) §8 |
| 4 | ADSP 没起 | SMEM 读通，判决行没上屏 | 有边上 GLINK/APR；没镜像先压 `vadsp` | dev-log §Round183 |
| 5 | watchdogd 每次起就崩 | 已定名且非瓶颈：boot 76 带着这枚 brk 活到死线，sheng 零引用 `IOWatchdog` 也进桌面 | 先量 `AppleARMIODevice` 在本板有没有平面；不在当前排期 | dev-log §Round199–200 |
| 6 | GPU/Metal 一个都没有 | 分支清白、债在电：`gx_collapse=0` 而 `gx_on=0`，那颗 LDO 从复位起没人按厂商表配过 | 只读侦察到 MEM_ACC 那一格；桌面 CPU-first | [`notes/gpu-bringup.md`](notes/gpu-bringup.md) §5.11–5.13 |
| — | 「读不到为什么结束」+「挂死不自动回 TWRP」 | **回 TWRP 这一半已解（boot 110、111、112、113 连续四靴零人工落回；110 与 112 同形＝panic marker 双 STALE 的裸复位，111 是进了 sink 那支，113 是第一靴**只有**看门狗解释得通：无 deadline 记录、无本靴 marker（卡上两枚是 boot 111/87 的 STALE）、`r281: wdt … en=1 armed=1 req=1 pet=60` 止于 t=546、内核环止于 565.4369、bark 29 s／bite 32 s ⇒ 复位在 ~578 s）；「为什么结束」这一半仍未测**：r324 上膛 APSS WDT 后 `clover r281: wdt ... en=1 armed=1 bite=1048575(≈32s) req=1 pet=…` 在场（boot 109 整靴 `en=0 armed=0`），两靴都自落 TWRP（`r198: recovery BCB pre-armed rc=100 written=1`、卡上死线记录仍是 boot 108 的）。**但两靴的自落不是同一种死法，且不许再写成「bite 复位」**：boot 110 的 panic marker 是 STALE（解码器明说 `boot 110 never entered the sink`）＝内核没进 sink 的裸复位；boot 111 的 marker 是本靴（`boot=111 magic=yes stage=0 rc=0 probe=8/8`）＝进了 sink 且其后一步没落、无 drain-in、无句子记录，与 boot 102 定名的 `panicAppendText()` 没返回同形。而 `r281: wdt` 是**变化闸控**行，它在 t=542 之后不出声只说明签名没变，不说明停喂；bark≈29 s/bite≈32 s 落在 ~574 s，与 boot 111 进 sink 的 k=712 s 对不上——**boot 110/111 的死因都没有读数**。控制对照仍成立：boot 109（en=0）挂住不自落、boot 110/111（en=1）自落 | 读回通道对「挂死」已成立，r325/r326/r327 沿用同一枚上膛（r326 与 r327 都未动 boot-args，boot 112/113 与 110/111 可比）；`clover r324: wdt-arm` 一次行被 512 行环地平线转掉，改由周期 r281 状态行这第二腿扛证据。剩下的债是「死因」与「落回时刻」两格：boot 111 已把范围收到「burst 打到第 7 行后同一秒进 sink」，boot 113 把另一支收到「停调度后无人喂、bite 复位」（无 marker 即无 sink 参与），要定它需要一条不经变化闸的喂狗心跳行＋sink 句子真正落卡。宿主侧新账：`run-clover-unattended.sh` 2220 s 内没抓住 boot 113 的落回（板子后来确在 `recovery`、卡上确是 boot 113），落回**发生了、时刻没测到**，本轮读数改由手工 `dd` 取——「谁带回 TWRP」有卡上证据，「什么时候」没有 | dev-log §Round323 ①、§Round324 读数①、§Round325 读数②④ |

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
| 时间线只有「写」的一侧被钉住 | r330 起了 `$(basename "$OUT").timeline.txt`，闸门钉的是四个窗口都写；**读**的一侧还没有工具，判一靴的静默仍要靠人 `grep` 那份文件 | dev-log §Round330 ⑧ |
| 四枚 vendored 显示报告 helper 仍无调用者 | r329 另建了同源的周期行，这四枚（`prepare_report`/`properties_now`/`state_words`/`clients_dump`）至今零调用 | dev-log §Round329 ②、§Round330 ⑧ |
| 持有者扫描的四格地平线 | r331 腿 A 只走到 pid 400、腿 B 只走等待者自己的表、明细行截在 4 枚且没有「丢了几枚」这一列、一行只挑 `debt` 的第一枚 hop | dev-log §Round331 ⑧ |
| 纯注释轮仍欠 | `NativeLegacyDisplay.cpp` 里 `int-209`/`int-210` 两枚不可核引文未撤回，探针注释「this round closes」未改准 | dev-log §Round330 ⑧、§Round331 ⑧ |

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
- **宿主等待窗口**：`scripts/run-clover-unattended.sh` 每次运行写一份 `$(basename "$OUT").timeline.txt`，
  一段一个 watch，每行是那一拍自己的 `adb`／`fastboot` 两个答案加 elapsed（变化或满 60 s 才写），
  开段与收段各带一张 `ioreg -p IOUSB -w0`。它回答的是「什么时候回来的」，卡上那些记录回答的是「谁把它放回来的」。

## 闸门

| 闸门 | 查什么 | 跑在哪 | 最近一次实跑 |
| --- | --- | --- | --- |
| `scripts/check-clover.py` | 板级预检 + ADT/DT 常量 + 补丁与探针形状 + 驱动宿主模型 | 仓库 | EXIT=0（r331），`drivers documented: 11, base-repo drivers exempt: 0`，12 条 notice 全指向未构建的产物；`test-clover-block.py`、`test-clover-gpu.py` 与 `test-clover-power.py` 都被这道闸门**跑**而不只是断言存在 |
| 仲裁器宿主模型那一半（r331b 新增） | `test-clover-power.py` 每轮跑**两趟**：空载一趟、`loaded:<nproc>`（每核一枚 busy 进程）一趟；闸门钉四件事——服务线程不许用计时睡眠、得是 `sched_yield`、被服务的预算得是满载 sizing 的那枚、`rearm()`/`serve()` 之间得有锁 | 仓库 | 5 次连跑全 EXIT=0（每次两腿）。四条针脚逐条负测过，句子都在 `~/clover-out/round331b-evidence/`：`kPolls` 退回 200000 ⇒ 满载腿在 line 419 断言失败（空载腿仍过），删 `sched_yield`／改名 `channelLock`／写回 `usleep (50)` 各判红一次 |
| 同一脚本的产物那一半 | 12 枚归档逐枚走对象、核 `docs/drivers/xnu-drivers.md:106-116` 那五个字段、再到链接好的内核里验每颗驱动的导出符号 | **必须在 `out/clover/tree` 里跑** | EXIT=0，`12 archives, 22 objects ABI-checked, kernel read`；`exempt: 11` ——组装树把基仓的 11 颗 `drivers/*`  symlink 进来，r316 起文档闸门只判本仓 authored 的那些 |
| 同一脚本的文档那一半 | 有源码的 `drivers/*` 必须有 README；README 必须有一段点名文档并至少引一枚**能落地**的 `docs/**.md:行号`；没有 `Info.plist` 的必须说出理由；禁止「本仓 `.cpp`:数字」这种形状 | 仓库即可 | 负向测过：CPU type 改 `0x1000007`、filetype 改 2、换假符号名都报错，带路径的厂商引用不误报；r316 新暴露的一条：本仓 `.cpp` 引用被 `BlockCore.h` 取代，README 改用符号锚点 |
| `scripts/verify-clover-payload.py` | 归档对镜像：卷内形状 + 逐轮探针断言（含标题必须带双引号，那是 boots 35/36 的转义防线） | 仓库 | EXIT=0，**51 行 OK、0 FAIL**（r331）；`SHNGROOT` 那一枚针脚换成了走查的拒绝标签——优化器把八枚 ASCII 折进立即数，镜像里没有那串字了 |
| `scripts/verify-userdata-window.py` | 刷 userdata 前的宿主闸门 | 仓库 | 本轮未涉及 |
| 读回通路那一半（r330 新增） | `run-clover-unattended.sh` 必须过 `bash -n`；四个等待窗口都要开时间线段、并且每一拍同时问 `adb` 与 `fastboot`；`read-clover-log.py --selftest` 里 r329 那两行由探针源码的格式串当场造出来 | 仓库 | 仓库侧与 tree 侧都 **EXIT=0**；三条新针脚逐条负测过（删一段 `timeline_open`、摘掉 `watch_landing()` 的 `fastboot_serial`、尾部加一句不闭合的 `if`），空跑一遍真的写出 1,970 B 时间线、poll 行落在 0/60/120 s |

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
