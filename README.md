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
* **第二道坎（间歇）**：部分靴子在 726–1420 s 被一条致命 `Kernel data abort` 提前掐掉，boot 74 崩在
  `_memmove`、boot 75 崩在 `IOSKArena::unmap` 的 cold 分支（两靴 `lr` 同一个值；boot 48/67 曾活到死线没犯）。
  r286 的 GPU 写被排除（GPU 行先干净打完，异常在完全不同的代码里）。**「同一 caller」这句话在落盘时要收回一半**：
  那个 `lr` 在本板内核符号表里落在一段 12 MB 的空档（`_native_legacy_*` 两个绝对符号之间），宿主解不出名字，
  而卡上带回的 480 字节 panic 记录又在寄存器表中途截断，`esr`/`far`/`fp`/`sp` 一个没收 —— 所以 r289 改在
  `panic_with_thread_kernel_state()`（内核里唯一同时握着这些字段的地方）打一条现场 `abt` 行加最多十二个
  返回地址，走异常自己的 `fp` 链。**boot 76 没犯**：它是迄今最远的一靴（日志尾 1814 s，SpringBoard 在 t≈1626
  仍在读 IODeviceTree），panic 槽里是 r148 的死线而不是 data abort，全窗 `clover r289` 行数为 0 而镜像里确有
  `_clover_abort_trace` —— 沉默带对照，读作「这靴真没崩」。三例（boot 54/74/75）分属三张镜像，**一次开机抓不到
  它**，所以 r289 留在每张镜像里等复现；每轮读回固定查一次该行计数，非零才解帧。**boot 77 又是一支新的形状**：
  它 1248 s 就断了，panic 记录环里 boot 77 名下**一个 kind-20 槽都没有**（最新 seq 47 是 boot 76 的死线），
  死线（1800 s）也没到，最后一条内核记录之后的槽还是 `BAD (newline missing)` —— 一次不留 panic 记录的静默
  复位，正是 r283/r284 造出来要定名的那类现象的第一个卡上实例；`clover r289` 计数 0 与它自洽（没有 panic 就
  没有 `abt` 行）。
* **第三道坎（新的主线瓶颈）：五连「静默、无 panic」复位，第一条因果假说已被自己的控制轮杀掉。** boot 77 停在 1248.429585 s（最后一槽半行），
  boot 78 停在 718.84 s（最后一槽**完整**，`clover r229: sbport t=718 bb`，seq 2183 `ok`），
  boot 79 停在 1184.031391 s（内核环最后一行），boot 80 停在 1335.047367 s（同一位置，最后一行是 `clover r231: bbhop pid=58`），
  boot 81 停在 997.261788 s（`clover r223: sbport t=997`）
  ——五靴都没有 kind-20 panic 槽、都没到 1800 s 死线、
  `clover r289` 计数 0，也就是说复位落在两条 printf 之间，机器没留遗言。
  boot 79 把这坎的范围量出来了：卡上最后一笔请求在 1165.522 s 开始（cfprefsd 的 8 块写，`lba=34388184`）、
  驱动自己的心跳最后一条落在 1163.919 s、内核 tee 环最新一行标着 1184.031391 s——**盘与内核都在照常出声，然后一起没声**。
  boot 80 把这条量法重新量了一遍并署了名：最新请求 1323.814 s、最新一拍 1320.098 s、最后一行 1335.047 s，
  飞区 12 条 entry 与 4 条 beat 全部署 boot 80，而 deadline 槽与 panic 标记仍属 boot 76 并报 `STALE`——
  **r293 的 boot 戳第一轮就用上了**，否则这一帧会被读成「boot 80 也走到了死线并 panic」。
  两条通道都封顶不了它：**「落回 TWRP」不是读数**（r198 每靴预写 recovery BCB，任何复位都落 TWRP），
  **驱动 verdict 环也没有时间戳**（boot 78 一共 40 条，全是上膛期与 cap 通知；seq 是跨环全局计数器，
  所以「内核环还在写时驱动没有新行」只能读作「驱动没有要说的行」）。r283/r284 造的那条 IMEM 跨复位记录
  **被它自己新加的词作废**：boot 80 读到 `beatmagic=801a082d822c1192` ≠ `BEAT283`，`magic` 也不等于 `CLOV283`
  ⇒ 0x660 那 20 个词从来不是本驱动的心跳，这一格是别人的内存（上一轮只是「看不懂」，这一轮是「不是我们的」）。
  TWRP 侧的 PMIC PON 闩锁也用一记**负对照**结掉了：`adb reboot recovery` 主动造一次已知软复位，落回后的
  `power-on reason` / `power-off reason` 行与静默复位落回时逐字节相同 ⇒ 那枚闩锁没有区分力，内核侧 SPMI PON
  仪器不值得建（这排除的是仪器的判决力，不是 PON 本身）。
  曾经唯一的新信息是一条时间线：**四起静默复位都在带 r290 那两笔 GPUCC 分支使能写
  （`kGpuccRbcprEn`/`kGpuccRbbmtimerEn`）的负载上，而那两笔写第一次上机就是 boot 77**；本 port 活得最久的两靴
  （boot 73 无死线跑到 3145 s、boot 76 真走到自己的 1800 s 死线 panic）都在 r290 之前。r294（v0.6.98）就是为此
  把这两笔写删掉的**控制轮**——判据写在源码注释里：若 boot 81 仍静默复位，这条相关当场死。
  **boot 81 让它当场死了**：997 s 又一起静默复位，而被删的两票在卡上被读出来（r290 的分支行本轮
  `rbcpr=0/1 rbbm=0/1`、`rcg_rbcpr/rcg_rbbm` 回到 `80000000`——en 从 1 回到 0，正对照来自被回退的那台仪器本身）。
  ⇒ 「这两笔写」被排除；剩下的分支是「**负载是不是变量**」，于是 r294b 拿归档的
  `round289-v0.6.86-payload.img`（boot 76 那颗、最后一颗走到自己死线的镜像）**原样重刷**：旧镜像现在也静默复位
  ⇒ 变量不在负载，GPU 整条线从复位嫌疑里摘掉；旧镜像再次走到 1800 s ⇒ 差异在 r290 之后的镜像里，下一趟就把
  GPU 的**读**也关掉（`clover-gpu=0`，boot 参数级二分）——boot 76 与 boot 81 之间只差那三次新读
  （r290 分支读、r291 轨读、r294 普查读），而「读一个没上电的块要不要代价」在本板从没测过。
  电池这一支在 rec 里现测排除（100%、4.36 V、25 °C、Full、`current_now=0`）。
* **`watchdogd` 那条相关性已撤掉，它的死因宿主已定名。** `watchdogd+0xab0c` 的 `bl` 在宿主上解到
  `libSystem/__os_crash`，紧跟的 `brk #1` 是编译器给 noreturn 调用返回加的兜底，它带的字符串是
  `connect_to_kext() unexpectedly failed - unable to lookup watchdog KEXT service` —— 缺的是
  `IOWatchdog` 服务的**设备树匹配**（`AppleARMWatchdogTimer` 本体在 prelinked 镜像里，13 处引用，
  但 `wdt,s5l8960x` 我们的 ADT 从不发布）。反证在卡上：这枚 brk 在五靴都打过（boot 71 t=1671.275、
  boot 73 t=2866.356 与 t=4055.340 两次、boot 76 t=1336.199、boot 77、boot 78），而 **boot 76 一路跑到 1800 s
  死线、panic 槽里是 r148 的死线、一次静默复位都没有** —— 这条相关性是被一靴证伪的，不是被推理削弱的；
  trap 到死的间隔还不固定（43 s / 184 s / ∞）。sheng 也没有这个服务却能进桌面，所以它不是本板的挡路石，
  要补也得先量 `AppleARMIODevice` 在本板到底有没有平面，r292 不碰它。
* **新的现形（落盘当天就被自己收紧了一半）：两靴死前 11–12 s 起，再没有**新的读写请求**开始过。**
  r210 起卡上一直有一个飞区（12 条块请求 + 每 4 s 一拍自带 CNTVCT 的心跳），八十个轮次没人解码过。
  解码后 boot 78 最后心跳 t=707.437、最后一笔块请求 t=707.848（pid 224 的 8 块读，lba 30386448，
  `stage=entry` 即只记了「开始」），而内核自己的采样器一直印到 718.840606；boot 77 同序
  （1235.848 / 1236.492 / 1248.429585）。**被死线掐掉的 boot 76 是反过来的配对**：请求环到 1802.582
  还在进、心跳比它旧 126 s —— 那是流量把心跳饿住。
  **但「整条块 I/O 静了」这句今天写重了，当场收紧**：`flightEnter` 只挂在 read（`CloverSdhci.cpp:2787`）
  与 `clover_emmc_block_write`（`:5058`）两条路径上，`clover_emmc_flush_cache`（`:5801`）与两个环的落卡
  （`ringWriteRecord`→`writePioBlock`，`:3411`/`:3430`）都不记条目也不取 guard；所以 707.848 之后
  那 11 s 里 tee 行照常上了卡，块设备**并没有**沉默。飞区能说的是「没有新的读写请求开始」，
  不能说「盘没人用了」，更不能说「盘挂住」。本轮起 runner 每轮固定解 `--flight`，读法按这条收紧后的写。
* **r292 换的是仪器地平线，不是硬件形状。** 能把「硬件 bite」从这次静默里切出来的只有 APSS 看门狗，而 r281 的
  `wdt` 行每 120 s 才一拍：boot 78 只在 617 s 读到一次 `en=0 armed=0`，之后 101 s 到死再无第二拍 —— 假设需要的
  读数从没进过房间。r292 把定时器缩到 4 s、保持原有的行密度（状态行每 30 拍、表每第 5 个状态行），
  `EN/STS/BARK/BITE` 只在变化时打印，并给每行盖 `why=first|chg|ctrl`：`chg` 缺席要能读成「没动过」而不是
  「定时器死了」。该行有独立 tee 类 `r281-wdt`（排在 `r281-aks` 之前，first-match-wins，一次性 bark 不能吃
  keystore 的预算）。判据：死前出现 `armed=1` 的 `chg` ⇒ 复位有硬件出口，下一格查谁武装的；到最后一拍都是
  `ctrl armed=0` ⇒ 0x17817000 从头到尾没武装，往 AOP/PON 那一侧找。**`--flight` 那条判据按上一条收紧后重写**：
  分「驱动自己的定时路径停了」与「卡不接了」，要比的是 **beat（驱动直接落卡）** 与 **tee（靠一次受 guard 的
  操作顺带刷出去）** 两条写通路各自最后一条，而不是 entry 环——entry 只记 read/`block_write` 的开始。
* **boot 79 把 r292 的判据结掉了，走的是 (b) 那一支；boot 80/81 第四次、第五次按同一形状复现。** 活到落卡的两拍
  （boot 79 fire=151 t=860 s、fire=211 t=1101 s；boot 80 t=737.611779、t=1098.120726；boot 81 t=865、t=993）
  都是 `en=0 armed=0 sts=0`，bark/bite 还在复位值，`why=chg` 一次没有——`0x17817000` 这块在本靴允许仪器看见的
  区间里从头到尾没被武装，**静默复位不来自 APSS watchdog**；下一支在 AOP/PON 那一层（这颗寄存器管不到它）。
  地平线照旧要说清：内核环止于 1184 s / 1335 s / 997 s，那之后这格是盲的，所以这条只否证「看得见的那段」。
* **r293 是把四处读数改窄，不改硬件。** 这一轮差点把 boot 76 的结局当成 boot 79 的：飞区的 deadline 槽与 panic 槽
  都还是上一上一靴的（boot 79 从没写过 kind-20 记录，原始扫过 1217 个块），而解码器照样印出「deadline fired after
  1800 s」——**只有 panic 标记带 boot id（r236），entry/beat/deadline 三类不带**，于是「写手停了的环」会悄悄混两靴。
  r293 把 `gBootId` 盖进这三类（35 号词，deadline 走 `extra[10]` → 45 号词），解码器按靴次判陈旧并印
  `STALE: boot N never wrote one, so this group says nothing about this boot`；DMA 被拒的行不再一律说 `"deadline"`，
  切成 `spin|deadline|error|stop` 并加 `ms=/budget=/polls=`（boot 79 那 4 次读拒绝 + 1 次写拒绝分不出是卡压过 600 ms
  预算还是本驱动自旋预算先用完）；r283 的两条行加 `beatmagic=%llx`，让下一次 `unnamed` 能分成「那格从没是我们的心跳」
  与「是我们的、钩子没响」；runner 落回 TWRP 那句也改窄成「复位 + 预写 BCB 把它接进 recovery，死线响没响读卡」。
  另两处误引一起收回：`clover r277: enter secs=1617` 那条槽是 boot 76 的，不是 boot 79 活到 1617 s 的证据；
  「boot 79 拒了七笔传输」重数是 4 读 + 1 拒写、7 次整卡重初始化。
  **eMMC 抖动这条假设随之降级**：4–9 次卡恢复在归档的每一靴都出现（包括干净 panic 结束的），报告器又封顶 8 行——慢性噪声，不是杀手。
  **boot 81 让 r293 那枚阶段名第一次咬到人**：四笔被拒的**读**全部名为 `deadline`，
  `ms=1147/1213/6208/2387` 对 `budget=600`（`polls=62/29/17/96`），`spin` 一次没出现——等待确实越过了每请求
  600 ms 的预算才退出，不是本驱动先停手。（boot 80 那一笔只到命令相位，`ms=0 budget=0 polls=0` 是因为命令相位本来不量。）
* **r283 的跨复位记录第一次上卡**：boot 77 的 verdict 环 slot 0 带回
  `clover r283: last reset unnamed magic=ed88000200a909f8 beat=403443458069358 beatcnt=1371352791997`。
  `unnamed` 只说明**槽里没有我们的 magic**（写手 `CloverSdhci.cpp:8830` 最后才落 `CLOV283`，读者 `:8916` 只认
  它）：三个词都比应有的心跳形状大三四个数量级，读作「IMEM 的 0x660 槽没活过上次复位 → BCB → TWRP → fastboot
  → 重刷 → boot 77 这段路」，**读不出复位来源**。`named` 那一支至今没在卡上兑现过。**boot 80 把它作废**：
  r293 加的 `beatmagic=%llx` 读到 `801a082d822c1192` ≠ `BEAT283`，`magic` 也不等于 `CLOV283` ⇒ 那 20 个词
  从来不是本驱动的心跳，这一格是别人的内存；跨复位「谁按的」这条通道到此为止。

每一轮的设计、构建与真机读数都在 [`notes/development-log.md`](notes/development-log.md)
逐轮可查，本节只留当前结论。

## 距离 iOS 桌面还差什么

| # | 缺口 | 现状 | 下一步 |
| --- | --- | --- | --- |
| 1 | **backboardd 不回 SpringBoard 的信** | 卡在发给 `datamigrator` 的一笔没人回音的 XPC 往返（上节详述）；keystore 已发布、解锁等待消失，workloop 五条等待者照旧 | 追 datamigrator 那条 workloop 为什么不再排活；做 PreBoard 同形状普查 |
| 2 | 触摸没有任何应答 | QUP v2 I2C-3 电气通，两个候选地址全 NACK（`verdict=102`） | 先把面板轨真上电（依赖 3），再问地址 |
| 3 | PMIC 轨读不出来 | SPMI 仲裁器读通路好，但核心版本字不在 v5 带内，APID 一个没认 | 弄清版本字、认 APID、读轨开轨 |
| 4 | ADSP 没起 | SMEM 共享堆读通（`$TOC`/`$PRT` 自校验通过），判决行还没上屏 | 有边就上 GLINK/APR；没镜像先压 `vadsp` 电源 |
| 5 | watchdogd 每次起就崩 | 死因已定名：`bl` 到 `libSystem/__os_crash`，串是 `connect_to_kext() … unable to lookup watchdog KEXT service`。`AppleARMWatchdogTimer` 本体在镜像里，缺的是 `wdt,s5l8960x` 的设备树匹配（provider 是 `AppleARMIODevice`，本板平面未证）；致命已由 `panic_on_crash_disabled=1` 摘掉。**boot 76 带着这枚 brk 活到死线，sheng 零引用 `IOWatchdog` 也进桌面 ⇒ 不是本板的挡路石** | 先量 `AppleARMIODevice` 在本板有没有平面，再谈造 `IOWatchdog` 服务；不在当前排期 |
| 6 | **GPU/Metal 一个都没有**（Adreno 512 无驱动、无固件、无 renderer） | `gpu` 区 `planned`；boot 74 实机：**r286 上电写序列在本板第一次生效**——`cx_on=1`、`gfx3d_en=1`、`cxo_en=1`、AHB/BIMC 三票=1（eMMC 正对照成立），但 `gx_on=0` → `chipid skipped`。**boot 76/77 逐字段复现**，所以 `gx_on=0` 是稳定硬件状态，不是竞态也不是时机。**boot 77 的 r290 读数把「问了」和「在跑」分开了**：`gfx3d=1/1`、`rbbm=1/1`（BIT(0) 按下、BIT(31) 仍 HALT），而 `cxo=1/0`、`rbcpr=1/0` 两支 halt=0 —— 前者是本轮没动过的天然正对照，后者是本轮新按的一票真的把分支启动了。（`rcg=80000000` 曾被读成「父源没设」，已收回；halt 两支的 RCG 都是 `0x80000000`、在跑那支 `0x00000000`，**只记相关性**：BIT(31) 在 RCG 命令字里的名字要等一份归档的 `clk-rcg.c`。见 [`notes/gpu-bringup.md`](notes/gpu-bringup.md) §5） | 分支清白、债在电：`gx` 的 GDSC `gx_collapse=0` 而 `gx_on=0` 是**电没到**，不是使能没按。r291（v0.6.92 已发出）按 §5.6 的厂商表读 GFX LDO，但**不新增映射**：`gfx_ldo_vreg` 的 0x0506e000 正落在 DT 声明的 GPUCC `reg = <0x5065000 0x10000>` 里（偏移 0x9000），于是读 `0x9018` VREF（µV=val*5000+375000）、`0x901c` 开关控制（byp/pdn/cpr_byp）、`0x9028` 的 `LDO_READY=BIT(2)`/`LDO_VREF_SETTLED=BIT(4)`，连同当拍 `cx_on`/`gx_on` 一起打——那两票是这行的正对照：三个词全 `0xffffffff` 而 `cx_on` 照常，是这页没应答，整行作废。checker 把「本驱动只开三张窗口」钉成 `IODeviceMemory::withRange` 计数=3。判据写死：`LDO_READY=0` 或 µV=375000 ⇒ 角从没抬起（债在 CPR4/`vdd_gfx`）；µV 落在 DT 的 400000…925000 且 `ready=1` 而 `gx_on=0` ⇒ 轨清白，回查 GDSC 自己的等待字段与 `gpu_gx_domain_addr`。角数（厂商 `SDM660_MMSS_FUSE_CORNERS=6` vs DT 的 1..7）不印也不下结论。S3/S5 两颗是 `qcom,rpm-smd-regulator`，SPMI 读不到，不在这一格。**boot 78 把两条预登记判据一起点亮**（`beat` 副本顶上，`ldo post` 在环窗外）：`vref=0 val=0 uv=375000`、`sts=0`（`ready=0 settled=0`）、`ctrl=80000038`=`LDO_CLAMP_IO|LDO_PDN|LDO_UNDER_SW_CTRL`+一枚没名字的 BIT(3)，同拍 `cx_on=1` 作正对照 ⇒ **轨自这次上电起既没使能也没编程，「轨清白」这一支关闭**。下一手是对轨**编程**，缺的不是寄存器（三处都在 `msm_gfx_ldo.c` 点得名）而是使能全序与角→电压来源（CPR4 fuse 在 `0x00784000` 另一张窗口），没有 sha256 出处不写。**boot 79/80 逐拍复现同一读数**（`uv=375000 sts=0 pdn=1` 且同拍 `cx_on=1`），轨的状态不是时机问题。**r294 把这格的读数铺成全普查**：`GpuRegs.h` 按厂商 `msm_gfx_ldo.c` 把 0x00..0x30 全部定名（0x20 在那份文件里没有名字，普查行照印但不替它编名；ADC_CTL 0x34 落在 DT `reg = <0x0506e000 0x34>` 之外，不读），`clover_gpu_report_ldo_words()` 在 post/beat 两拍各印 13 个词 + 当拍 `cx_on/gx_on`，**一个词也不写**——它是下一轮真去编程轨时的基线，r291 那三个词保留自己那行以便和已归档的 boot 78 对比。**boot 81 的普查第一次上卡就当轮给结论**：两拍（t=859/t=980）13 个词里只有 `w1c=80000038`、`w24=0000000d` 非零，其余 11 个全 `00000000`；并排厂商 `sdm660_ldo_config[]` 的期望值（ATEST `0x00000080`、CFG0 `0x0100A600`、CFG1 `0x000000A0`、CFG2 `0x0000C3FE`、VREF_TEST_CFG `0x00401100`、REF_X1 `0x00000230`、REF_X2 `0x00000048`）⇒ **一位都不对**。所以不是「编程到一半」，是这颗 LDO 从复位到现在没人按那张表配过，引导加载器也没配。正对照照旧成立：`w1c` 读出非零、同拍 `cx_on=1` ⇒ 这页在应答，0 是读数不是空窗。`w24` 是 `LDO_STATUS1_REG`（厂商 msm_gfx_ldo.c:54 定义了却从没读过），归档里没有它的位名 ⇒ 印出来、不命名。**同一轮还做了一个控制**：删掉 r290 的 `kGpuccRbcprEn`/`kGpuccRbbmtimerEn` 两笔写（读照旧），因为四起静默复位只出现在带那两笔写的负载上；checker 现在反向拒绝这两行回到源码。**桌面仍 CPU-first** |

1 挡住桌面；6 是 1 最可能的病根（尚未证）；2–4 是之后要补的板级驱动，5 只是噪声（已定名且非瓶颈）。
五连「静默、无 panic」复位与它是同一条线上的症状还是独立的坎仍未判；r292 把 WDT 的地平线换细以后，
boot 79、80、81 三靴都排除了 APSS watchdog 这一支。**第一条因果假说（r290 的两笔 GPUCC 分支使能写）已被 r294
这个控制轮杀掉**——boot 81 带着「没按那两票」的读数仍在 997 s 静默复位。剩下的通道：AOP/PON 那一层这颗寄存器
管不到，而 TWRP 的 PON 闩锁经负对照证明没有区分力，所以那一层目前没有能用的仪器；于是先问「负载是不是变量」——
**r294b 用归档的 round289 镜像原样重刷**（boot 76 那颗、最后一颗走到自己死线的镜像），两支的判据写在
[`notes/development-log.md`](notes/development-log.md) 的 boot 81 一节里。

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
