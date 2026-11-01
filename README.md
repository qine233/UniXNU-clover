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

## 当前卡点，用大白话说

**SpringBoard 每次都在等 backboardd 回信，信一直不来。**

* SpringBoard 是画桌面的进程。它启动后向 backboardd 发一连串请求，然后等回信；
  回信不来，它就停在半路，最后被看门狗杀掉（boot 26 里 448 s 那次 SIGKILL 就是这么来的）。
* 回信为什么不来——真机读数已经钉死到一条用户态死等：backboardd 里负责按钮/触感
  初始化的一条工作线程，在等一个「控制器就绪」的通知（`_controllerReadyGroup`，
  一个 `OS_dispatch_group`）。这个组在 `-[BKButtonHapticsController init]` 里 enter 了一次，
  leave 被打包成一个 block 交给 `BRButtonResolverController`，**而那个 ready 通知
  从来没有发生**——组的计数回不了 0，gen 从未前进，也没人 wake，
  线程从 t=464 s 起睡到 1800 s 死线。
* 这是**用户态在等一个永远不会来的条件，不是内核阻塞**：backboardd 其余线程
  （包括四个在 port set 上收消息的）都活着。r239（boot 28）已把这条链在设备上逐字段读出：
  组的账本 `cnt=1 g=0 wait=1`（一次 enter、零次 leave），block→self→组 的指针链也命中。
* 怎么去掉病根——**照搬参考移植 sheng 的做法**：它跑同一套镜像、已到桌面，靠的是
  ADT 把设备描述成一台**没有实体 home 键的 iPad**。backboardd 查到「没有 home 键」
  就打一行日志、**跳过**那段触感初始化，死等根本不发生。Mi Pad 4 也没有 Apple 的
  触感 home 键，如实这样描述硬件、去掉病根，比在内核里伪造一次 leave 干净。
* 走过三次弯路：r240（boot 29）只加了一个 `buttons`/`home-button-type=2` 节点，
  r241（boot 30）又照 sheng 把整个 `product`（产品身份）节点改成无 home 键的
  iPad Pro，r242（boot 31）再把 sheng 独有的剩下两样——根节点 `target-type` 和
  `/chosen` 的安全状态元组——也补齐。三次真机读数，死等**一字未变**。
* 于是不再猜节点，直接反汇编系统缓存里的取值代码，来源钉死了：`BSPlatform` 的
  「有没有 home 键」= MobileGestalt 的一个整数问题（混淆代号 `JwLB44/…`），而
  MobileGestalt 对这个问题的答案就是去设备树平面查 **`IODeviceTree:/buttons` 节点的
  `home-button-type` 属性**——r240 的方向从一开始就对。节点、属性、类型、值都已在
  ADT 里且过了校验，可死等不动，那么唯一从没被观测过的事就是：**内核到底有没有把
  这个节点发布进那个平面**。
* r243 不再改任何配置，只加一个读数：内核在自己已经证明会跑的采样点上，一次性
  把设备树平面整个点名一遍——每个节点叫什么、`/buttons` 按路径查得到查不到、
  `home-button-type` 的值是多少，并用 r205 已证可达用户态的 `/chosen` 做阳性对照。
  boot 32 证明它确实跑了（同一拍的死等读数在旁边），但也证明**它响在了没人听得见的
  时刻**：卡上带回的只有最后 ~770 s 的环形缓冲，而点名发生在开机 ~500 s。
  r244 只挪发射时刻——超过 1500 s（死线前最后 300 s）才点名——仪器与读数行原样。
* boot 33 的点名把这一条彻底结掉：平面里 9 个节点全在，`/buttons` 按 MG 同款路径
  查得到，`home-button-type` 读回来就是 4 字节的 2；同一拍的死等读数一字未变。
  也就是说**内核这一侧发布、命名、取值全对，病根不在这个节点上**。宿主这边随即
  把 r242 那两处「引用点」查了个明白：那是 dyld chunk 基址算错造出来的假象，真正的
  两个桩用 `dyld_info -fixups` 逐个解开了选择器和类引用——`sharedInstance`、
  `homeButtonType`、`BaseBoard/_OBJC_CLASS_$_BSPlatform`，取值链本身站得住。
* 于是不再猜节点，也不再猜反汇编，把最后两种可能各自变成一条读数。r245 先把这一支的
  语义在宿主上钉死（这次钉反过一次，随即纠正）：`applyDefinitions:` 里
  `cmp x22,#2 / b.ne`，**`==2` 那一侧才是记一条「不配 SSHB」的日志然后直接返回**，
  投出那条永远不 leave 的 block 的是 `!=2` 那一侧。也就是说要的是「用户态拿到 2」，
  不是「设备树里写 2」。
* r245 把 census 升级成判决器，加两行读数：平面根的 `model`（MobileGestalt 回答任何身份
  问题前会先按它的前缀分派），以及 IOService 平面里按键/HID/触感/Resolver 类的点名。
  boot 34 带回 `model=iPad7,1`（前提成立），但 `svc total=1`——这一行是仪器自己的错：
  `iterateOver(plane, 0)` 只走平面根的一层孩子，而 IOService 平面的根在最上面只有一个孩子
  （platform expert），其它服务全在它下面，所以这一列在任何板子上都报 1。
* r246 于是改两处，都不猜：服务平面换成整平面递归遍历（并把上限和「有没有撞到上限」一起报
  出来，短计数不可能再被读成空平面）；再加一行**用户态那一问**——`getPlane("IODeviceTree")`
  按名字查平面，再用这个平面查 `/buttons` 读 `home-button-type`。依据是内核代码本身：调用方
  不带平面对象时 `fromPath()` 从路径里截出名字交给 `getPlane()`，用户客户端的注册表调用也走
  同一条；而 `getPlane()` 返回 NULL 就是查不到，MobileGestalt 会把「查不到」写成默认整数。
  这条正好对上 boot 33/34 的矛盾：**内核读到 2，backboardd 的行为却是「不是 2」**。
* r246/r247 那两次刷写**一条读数都没留下**，上一轮把这记在 census 的 `svc->release()` 头上，
  是错的。宿主上的三条证据：卡上 boot 计数器还停在 34（`bootIdClaim()` 是块驱动上膛那一刻的
  本启动第一笔卡写，它没跑）、flight 区 0 条 entry/0 条 beat、三轮镜像反汇编到
  `_clover_iodt_census` 都是 `cmp x0, #0x5dc` 同一条 1500 s 闸门。真正的位置在
  `~/UniXNU/scripts/unixnu/payload.py`：它把 board.toml 的标题**裸插**进清单的
  `title = "…"`，而这两轮标题里写了 `getPlane("IODeviceTree")`，第一个内层引号把 TOML 字符串
  截断，固件（`UniXNULib/Config.c`）读不到 xnu 条目，菜单里没有操作系统——`tomllib` 逐份解析
  存档镜像实测：r245 通过，r246/r247 都在第 9 行报错。
* r248 因此只修通道，不动 census：生成器改 `json.dumps` 转义（与同文件 install 条目一致，
  固件的 `Toml.c` 支持 `\"`），并在写完后 `tomllib` 解析一遍、要求 xnu 条目带回本轮标题与
  内核路径——解析不了的清单再也刷不到板子上。`verify-clover-payload.py` 同步加一条卡内清单
  闸门（拿旧镜像验过它真会叫）。census 的第一手读数是 boot 36。
* boot 36（r248 实机）终于把 census 跑起来了，却只送到卡上一半：那一趟先被 9 行 bb 采样占了位子，
  census 的 `census t=`、9 行节点、`path buttons=` 填到每趟 **16 次推送**的上限（`kTeePushPerPass`）
  就断在这里，`hbt len=`、三行 `byname`、`svc total=/stopped=/button-family=` 和 `census done`
  被 `gTeeThrottled` 就地丢弃——扫描索引照样往前走，所以**没有下一趟补发**。boot 34 之所以读得完整，
  只因为那一版的服务点名只走一层、整批不超过 16 行。r249 因此只动读数通道：census 类与整集合行
  只因为那一版的服务点名只走一层、整批不超过 16 行。r249 因此只动读数通道：census 类与整集合行
  共用同一条例外（两者的「缺席」都不能与「什么都没查到」区分），每趟上限照旧保护那块还在服务
  文件系统请求的卡。boot 37 的判据是一行：**`census done` 在不在卡上**——在，就有完整的 byname 与
  服务点名可对号；不在而前面的 census 行在，那就是遍历本身走不完，是下一条要追的 bug。
* boot 37（r249 实机）把那判据答成了「在」：22 行 census 一行不少，`plane byname=ok`、
  `buttons byname=present`、`hbt byname len=4 u32=2`、`svc total=123 stopped=0 button-family=2`
  （点名叫到 `svc buttons` 与 `svc IOHIDResource`）、`model=iPad7,1`、`census done nodes=9`。
  于是 r246 预写的两条判决分支一起被否：平面名字没错，板上也不缺发 ready 的服务。**内核的发布侧
  没有毛病**——用户态用同一条按名路由能拿到同一个 `2`，backboardd 却还在等。剩下的唯一分岔不是
  「值不对」而是「有没有人问过」，而这台板子从来没有量过：census 答的是内核发布了什么，没有人
  答过注册表被问过什么。r250 就钉这一问：`is_io_registry_entry_from_path`（含 `_ool`）与
  `IOCopyPropertyCompatible` 是用户态每一次注册表查询的必经出口，两处都记「谁、问了什么、答了
  多少字节、那个数是多少」，只留名字含 `utton` 的；8 行环当场出声，census 在 `census done` 前
  把整个环再发一遍（走的正是 r249 那条豁免）。判决也写下：有 `row … comm=…` 就在「答案被读错」
  那条路上；只有 `no user client ever named a button path or key` 就说明 MG 的 home 键身份根本不
  来自注册表，ADT、平面、census 这条线整个作废，改走 sheng 的 HID home 提供者。
* boot 38（r250 实机）第一次答了「问没问」：`asks path=9 key=6 notes=15`，SpringBoard 与 PreBoard
  都把 `IODeviceTree:/buttons` 解析成功、把 `home-button-type` 读回 `len=4 val=2`。于是「ADT 缺节点」
  「平面名字取错」「用户态压根没问」三条线一起结掉——值是对的，也被真的读走了。可这台仪器又犯了
  r249 那一类自身的错：8 格环从第 9 条起回头覆盖，而打印按槽序不按时间序，`notes=15` 里最早那 7 条
  （最可能是 pid 更小的 backboardd 问出来的）正好被吃掉；`comm=` 还被 `pid <n>, ` 前缀截成了
  `SpringBo`。r251 因此不加新仪器，只把这一台修到能答完它自己提的问题：24 行、时间序、取名字尾部、
  每进程一行计数。真正剩下来的一句话是：**用户态拿到 2，backboardd 却还在 `!=2` 那一支死等**——
  要么 MG 的值不来自这块注册表，要么 r242 那条分支语义又钉反了，先在共享缓存里查清再决定动谁。


每一轮的设计、构建与真机读数（含本节的证据出处）都在
[`notes/development-log.md`](notes/development-log.md) 里逐轮可查；
r236–r239 是把这条链读出来的四轮。

## 距离 iOS 桌面还差什么

| # | 缺口 | 现状（真机读数） | 下一步 |
| --- | --- | --- | --- |
| 1 | **backboardd 不回 SpringBoard 的信**（上节详述） | 死等点已钉到 `_controllerReadyGroup`（t=1399 仍 `base=100c7c920 b=fffffffd g=0 cnt=1 wait=1`）；boot 38（r250）第一次把「问没问」读出来：`asks path=9 key=6 notes=15`，SpringBoard（pid 27，重启后 269）与 PreBoard（167）都`IODeviceTree:/buttons` 解析成功并读回 `home-button-type len=4 val=2`。**ADT 缺节点、平面名字取错、`/buttons` 不可达、用户态没问过这四条线全部结掉**——值发布了、可达、且被真实进程读对了。剩下唯一没答的是 backboardd 自己问没问，而这 7 条最早笔记被记录器自己的 8 格环挤掉了 （`notes=15`、槽位循环覆盖、按槽序打印） | r251 不加新仪器，只把这台仪器修到能答完它自己提出的问题：环扩到 24 行、按时间序打印、`comm` 跳掉 `pid <n>, ` 前缀取名字尾部、每个被记到的进程给一行计数。同时**在宿主**先把 `-[BSPlatform homeButtonType]` 与 libMobileGestalt 的取数点钉死：boot 38 说用户态拿到的是 `2`，而 r242 的反汇编说 `==2` 那支只是「记日志、跳过 SSHB」、投死等 block 的是 `!=2` 一侧——两者要同时成立，要么 MG 的值不来自这块注册表（另有来源或早于查询就缓存了），要么这条分支语义**又**钉反了一次（r245 已经在同一个函数上翻过一次车）。查清之前不许再开机赌方向 |
| 2 | 触摸没有任何应答 | QUP v2 I2C-3 电气是通的（两个候选地址都 NACK：`done xfers=4 nack=4 timeout=0`），`rails unproven` | 先把面板轨真的上电（依赖 3），再问地址 |
| 3 | PMIC 轨读不出来 | SPMI 仲裁器读通路是好的（`obs00=00000100`），但核心版本字不在 v5 带内，`apid=-1` 一个都没认，`ldo-a11`/`ldo-b3` 未读 | 先弄清那个版本字，再认 APID、读轨、开轨 |
| 4 | ADSP 没起 | SMEM 共享堆读通（`$TOC`/`$PRT` 全部自校验通过，`part(3) hosts=0:1` 是候选边），判决行还没上屏 | 有边就上 GLINK/APR；没镜像则先压 `vadsp` 电源 |
| 5 | watchdogd 每次起就崩 | 自身可执行文件里 `brk #1`（`IOWatchdog` 服务在板上不存在）。r193 的 `panic_on_crash_disabled=1` 已把它从「致命」摘掉，所以它只是噪声 | 写一个最小 `IOWatchdog` 服务，或让 plist 不再要求它 |

1 挡住桌面；2–4 是之后要补的板级驱动，5 只是噪声。

> 备注（时基）：r207 已核，环上的 `t=` 与 `mach_absolute_time` 一致，没有偏慢；
> 早期「慢 50 倍」是基数除错的读法，已废弃。另：每靴 1800 s 的死线是本项目
> 自设的无人值守收场开关，不是机器死机。

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
| 无人值守 | `run-clover-unattended.sh`：冷刷 cache → 1800 s 死线 → 预置 BCB 落 TWRP → 读回日志，退出码 0（本帧读数即由此而来） |
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

- [`notes/development-log.md`](notes/development-log.md) — round1–238 每一轮的设计、构建与真机读数，完整历史。
- [`notes/clover-device-tree-sources.md`](notes/clover-device-tree-sources.md) — 每个常量的来源，含 splash 地址的推翻过程。
- [`porting.toml`](porting.toml) — 上游要求的 13 个 area 与 findings。
- [`AGENTS.md`](AGENTS.md) — 仓库边界与闸门。

## 许可证

BSD-2-Clause-Patent，与 UniXNU 一致。`scripts/build-arm-driver.py` 移植自
UniXNU-gta4l（NoNameBuilder），采用同一许可证。
