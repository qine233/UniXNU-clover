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
7. **SpringBoard ← 卡在这里** —— 桌面进程被反复拉起，起来之后不再被杀（r206 那一格已成历史），
   而是和 backboardd 一起等在同一笔没人回音的信上；屏幕一直是进度条，进不了桌面。
8. **桌面** —— 未到。

## 当前卡点（结论）

SpringBoard 每次都在等 backboardd 回信，信一直不来。逐轮取证把病根从「触感初始化欠一次
leave」一路收紧到一句可测的话：

* **卡住的不是渲染，是一次没人回音的 XPC 往返**：backboardd 全进程唯一一笔在外的请求
  发给 `com.apple.datamigrator`，请求被取走、回音永不到期（`mc=0 sr=1 dl=0`）。
* **债链比这句话长一跳（boot 82 现场）**：datamigrator 自己（pid 147）7 个线程全在等、0 个在跑
  （`threads=7 wait=7 run=0 rx=1 tm=1 wl=3`——五个 workloop 等待者现在读成三个），而它有一个线程
  `tid=38e` 持着一笔没人回音的 send：`remote=7e0f sr=1 rmc=0 peer=187:com.apple.migra`
  （远端口 0 条待办 = 消息被取走、回音没来）。**pid 187 是谁现在读不出来**：`peer=`/`comm=` 被截到 15 字符
  （同一枚缺陷把 `com.apple.datam` 也截了），缓存里只剩 `com.apple.migration` 与
  `com.apple.migrationpluginwrapper` 两个候选——**选不出来是仪器问题，但不是能加宽的仪器问题**：
  `bsd/sys/param.h:95` 的 `MAXCOMLEN 16` 让 `p_comm` 只有 17 字节，缓冲区摆宽救不回字段里从没存在的字节。
  r295 因此换**第二枚名字源**——可执行文件 vnode 的完整名头，经 `vnode_getname()` 在
  `bsd/kern/proc_info.c`（本树里 `struct proc` 与 `struct vnode` 同时完整的少数文件之一）读出来，
  配一枚四态 `ex=`（`ok`/`noproc`/`notext`/`noname`）让空名字自己说为什么空；正对照是
  pid 58 那行必须完整印出 `backboardd`。普查同时从钉死 58/147 改成
  **顺着 send 传递式走 4 跳**，终止条件印成读数：`lock_busy`/`no_trap`/`no_send`/`no_owner`/
  `self_reference`/`cycle`/`depth_limit`。
* **boot 83 把这条链读全了（r295 已发出）**：`hop=0 exec=backboardd ex=ok`（正对照成立，两拍六行 `ex=` 全 `ok`）
  → `hop=1 exec=com.apple.datamigrator`（pid 148，8 线程 7 等 1 跑）→ 叶子在两拍里**不是同一个人**：
  t=657 是 `pid 166 exec=osanalyticshelper`，t=797 是 **`pid 202 exec=com.apple.migrationpluginwrapper`**
  （`comm=` 仍是截断的 `com.apple.migra`，靠 `exec=` 才分开——boot 82 那两个候选的答案是**插件包装进程**；
  pid 也从 187 挪到 202，旧 pid 别再引用）。叶子 `threads=4 wait=4 run=0 trap=1 end=no_send`。
  **`no_send` 不能读成「这人没在等」**：它在代码里的含义是「trap 里那根的 `x3`=0」，也就是**纯收**——
  捕获条件只挑 `x3!=0` 的线程，所以那根的 `rcv=`/`to=` 与收方向的 `rmc`/`rsr` 没读出来，
  「回音是否已经在队列里」这一问本靴答不了。r296 补了这一格，boot 84 读到了答案（见下）。
* **r296 已发出（v0.7.0）补的就是这一格**：链行加 `cap=send|recv|none` 说明那三列线程字段是谁的；`recv` 时
  把只收那根的 `rcv=`/`to=` 填回原列，并用**同一个** `clover_sb_port_mail()` 在收方向读那枚口的 `qmc=`/`qsr=`。
  前提照 r295 的规矩一起验：那函数只拿 object 自己的锁、读两个词、不分配不阻塞，调用点仍在
  `try_lock`→读线程→`unlock` **之后**；checker 这次多禁一枚 `thread_block(`——链是在已经卡住的用户空间旁边走的，
  采样线程自己睡着的代价是后面每一跳都没读数。`no_send` 这枚判决**故意不改名**：boot 83 两行就是这个值，
  本轮要读的正是「同一个判决下面，队列里到底有没有东西」。
* **boot 84 把这一格读满了，而且链比上一靴深一跳**：`hop=3 pid=126 exec=installcoordinationd`
  `cap=recv thid=2e5 rcv=1917 remote=0 to=ffffffff qmc=0 qsr=0 end=no_send`——正对照成立（`cap=recv` 且 `rcv` 非零），
  所以「根本没进 `MACH_RCV`」那一支不必启动。债链形状现在是
  **backboardd(58) → datamigrator(144) → migrationpluginwrapper(189) → installcoordinationd(126)，最后这个人在收**。
  四条判据落在 **(b)**：收的那枚口里**既没有消息（`qmc=0`）也没有发送权计数（`qsr=0`）**——「谁本该拿着向这枚口
  说话的权利」属镜像侧。（**boot 86 把这一条抽掉了**：`qmc=`/`qsr=` 与 `rvobj=` 走同一枚翻译，
  翻译失败时两者都停在初始化的 0，而 boot 86 证明叶子那枚 `rcv` 译不出对象 ⇒ 前两靴无法回溯判定，见下面 boot 86 那条。）(a)(c) 排除；(d) 字面成立但要打折：`to=0xffffffff` 是有限值，按 ms 折算约 **49.7 天**，
  本靴离死线只剩 110 s ⇒ **不能读成「它会自己醒」**，而且单位这条是推断（同一次发送方向的 `to=0` 加约定），
  卡上没有小超时的正对照。另记：`cap=recv` 那行的 `rmc=`/`rsr=` 是**没读**的初值，不是三个 0 之一。
* **boot 85 是同一枚 v0.7.0 镜像的控制轮，它把两件事分开答了**：仪器答「稳定」——八行两拍，
  链还是 **backboardd → datamigrator → migrationpluginwrapper → installcoordinationd**，叶子还是
  `cap=recv qmc=0 qsr=0`（判据 (b) 再现——其支持已被 boot 86 抽掉，见下条），每靴变的只有 pid（144→147、189→191、126→119）和端口名
  （`7f07`→`5707`、`1917`→`1e0b`）——这正是「端口名跨 task 不可比」的第二次证明。结局答「不稳定」——
  boot 84 跑满 1800 s 死线并按设计 panic，boot 85 卡上最后动静停在 **t=1680923 ms**、没有自己的死线记录、
  没有自己的 panic 标记（飞区那两枚仍署 boot 84，解码器印 STALE）⇒ **第七起静默无 panic 复位**，
  上界由死线记录给出（跑到了就该有），落在 **1681–1800 s**。带因此是七条：
  718 / 1184 / 1248 / 1335 / 997 / 707 / 本靴。**「什么时候结束」在 v0.7.0 上是分布，不是常数**——
  负载不是变量（r294b 已证），镜像也不是变量。WDT 两行 `t=` 在走、仍 `en=0 armed=0`；
  keystore `stage=3 opens=100 calls=442`（上一靴 88/387）；落地快照第三次 `charger_temp=327`、
  `rawdump` 头第四次全 0。
* **boot 86（r297 / v0.7.1）三态一次到齐，而它先抽掉了 boot 84 那格 `qmc=0` 的支持**：八行两拍（t=1131／1399），
  凡两边都译得出对象的地方全答 **`no`**（hop=1 两拍、hop=2 第二拍）——本任务里那根在 trap 内的线程等的
  **不是**上一跳写进去的那枚口；决定性的那一跳答 **`unknown`**（叶子 `rcv=d03` 在两拍里都译不成对象）。
  正对照成立：`hop=0` 两拍 `prev=0 link=-`，八条 r296 行配八条 r297 行（两类的 cap 豁免都响）⇒ 表完整、没被截。
  **代价是收回判据 (b)**：`qmc=/qsr=` 与 `rvobj=` 共用同一枚翻译，两条路径失败时都停在初始化的 0
  （`task.c:11364-11379`／`:11410-11423`），本靴叶子证明 `d03` 两次翻译都失败 ⇒ 「既没消息也没发送权」从此有
  两种读法，而 boot 84/85 的行不带返回码、无法回溯判定 ⇒ **(b) 失去支持（不是被反驳）**。同一支还读出第二枚
  仪器缺陷：`rvobj` 译的是 `frcv`（普查里第一根在 trap 内的线程，`:12772`），`cap=send` 行打印的 `rcv=` 是
  `nrcv`（第一根 `x3≠0` 的线程，`:12781`），两者只在 `cap=recv` 时相等 ⇒ `hop=0` 的 `rvobj=0` 有二义
  （名字没译出 / `frcv` 本来是 0）。(d) 兑现成新的自证：三跳 `robj` 两拍逐位相同
  （`…0ccc40`／`…e1b60`／`…d92a0`）而 `rvobj` 在两拍之间变（pid 266 的 `5003` 从译不出变成译得出）⇒ 发送侧对象
  整靴稳定、接收侧重建过。叶子换人：`58 backboardd → 147 datamigrator → 266 migrationpluginwrapper →
  **65 containermanagerd**`（前两靴是 installcoordinationd 126/119），`cap=recv rcv=d03 remote=0 to=ffffffff
  end=no_send` 的形状不变 ⇒ 第四跳是谁每靴重读。**结局是第八起静默无 panic 复位**：内核环最后一行 1711.714 s、
  飞区最新 entry `t=1701831 ms`（launchd 的一笔 8 块写，卡仍在正常应答），没有 boot 86 名下的死线记录与 sink
  标记（仍署 boot 84、印 STALE）⇒ 带八条 718/1184/1248/1335/997/707/1681/**本靴 1712–1800 s**。
  WDT 三行 `en=0 armed=0`（连续第十靴排除 APSS）、keystore 本靴内 `opens` 63→65、`calls` 248→260、
  GPU 三拍照常（`ctrl=0x80000038` 与 LDO 全 0 按 §5.12 读作「符合本板 DTB 配置」而非故障票）、6 笔
  `dma deadline` 无 `spin` 记慢性噪声、落地快照第四次（`charger_temp=327`、`rawdump` 头第五次全 0）、
  `clover r283:` 第二次带全套列上卡仍 `unnamed`（`named` 那一支至今没在卡上兑现）。
* **boot 87（r298 / v0.7.2）把那一问答了，而且答在一个不需要那枚名字译得出对象的比较上**：
  八行两拍、正对照三支全成立（`clover r297:` 0 行、8/8 配对、`hop=0` 是 `prev=0 link=- tlink=-`）。
  `tlink=` 三跳全 **`no`**，而**每一跳的 `tspace=` 恰好等于下一跳自己的 `space=`**——上一跳发进去的那枚口，
  它的 `ip_receiver` 就是下一跳那个任务，逐跳成立三次，且都在同一个地址空间里比 ⇒ **债链在任务这一级是真的**；
  不成立的是名字：`hop=N+1` 被采样那根线程等的是另一枚名字（`tname=3207/3507/2903` 对 `vname=4503/1003/d0b`）。
  按预登记七态落 (b) ⇒ 下一步是**普查那个收方任务自己的 entry 集**。另一半收获是 `verr=17` 有出处：
  `kern_return.h:151` 的 `KERN_INVALID_RIGHT` 只在 `ipc_right_lookup_read()` **成功**、而 entry 的类型
  既不含 RECEIVE 也不含 SEND 时才回 ⇒ 那三枚名字**不是不存在，是「存在但不是 port 权」**（port set 或 send-once）。
  这也把 boot 84/85 叶子 `qmc=0 qsr=0` 那格正式结掉：本靴同一枚 `d0b` 带着 `verr=17`，那两个 0 是「没读成」。
  **两枚遗留**：`cap=send` 行的 `qmc=/qsr=` 读的是 `nrcv` 而 `verr=` 报的是 `frcv`，那两列**还没有自己的返回码**；
  `helper` 只留两次尝试里最后一次的 kr，RECEIVE 那次的码没上卡。本靴**跑到自己的 1800 s 死线**
  （`deadline fired … boot=87 elapsed=1800.003s`、panic 标记两条都署 boot 87）⇒ 这一族四靴三种结局，
  「何时结束」仍是分布。WDT 两拍 `en=0 armed=0`（连续第十一靴排除 APSS）、keystore `opens 44→49 calls 165→186`
  四枚 selector 全 `rc=0`、GPU 三拍照常、`dma deadline` 降到 1 笔、`r283`/`tee seen` 本窗 0 行按「环窗外」读
  （环覆盖 `t=1444.5…1815.1`）、落地快照第六次（`charger_temp=322`）且 `rawdump` 头第六次全 0。
* **r299（v0.7.3，按用户指定「把多核修了」已发出，等 boot 88 读回）：副核不缺代码，缺的是一枚用别人板子
  命名的开关**。整套路径一直在 `drivers/ArmCpu/ArmCpu.cpp` 里——`probe()` 认第二颗 nub（`:226-241`）、
  `finishSecondary()` 调 `ml_processor_register()`（`:512`）、`startCPU()` 打 **PSCI 0.2 `CPU_ON`**
  （SMC64 `0xc4000003`，入口是内核物理复位向量、先 `cleanToMemory` 那一页，`:872-885`）、新核自己
  `wakeRedistributor` + `enableGic` + 装 IPI handler 并把 `armcpu_smp_step` 数到 5/6/7（`:573-598`）——
  挡着它的那枚 boot-arg 写作 `sheng-smp`（这枚驱动的来源板），本板 args 里没人会去找它，于是 `reg=1`
  活了十一靴。r299 把它改名进 **`armcpu-smp`** 家族并与 `armcpu-self-ipi=1` 一起传上：后者不是假设而是
  **使能项**（`osfmk/kern/machine.c` 只在一根核收到第一笔 IPI 时才清 `SIGPdisabled`，所以「IPI 从未被请求」
  那格有一半是它）。`armcpu-spi-ipi` 继续不传——硬编码的 `1008..1015` 确实超读本板（`typer=0x7b04f3`
  ⇒ ITLinesNumber=19 ⇒ SPI 只到 INTID 671），而 self-IPI 走 SGI 0，不吃这条限制。另一枚先前挡路的疑虑
  也排掉了：BCM2837 未定义 `HAS_CLUSTER`，拓扑按 `phys_cluster_id` 把 8 颗全归 cluster 0
  （`machine_routines.c:1094-1103`），`MAX_CPU_CLUSTERS=1` 的 `assert` 踩不到。前置条件本机都在：ADT 的
  `/cpus` 是 8 颗、`reg=0x0-0x3,0x100-0x103` 与归档 `sdm660.dtsi` 的 Kryo 260 MPIDR 逐位一致，`/psci`
  带 `method=smc`（正是 `psciConduit()` 读的那枚属性），而这板的 PSCI conduit **每年都在被 panic 屏用作
  `SYSTEM_RESET`**——那是现成的正对照。仪器侧新增 `clover r299: smp …` 行，打在**同一拍普查里、紧接
  r271 那行下面**（不是一次性打印，环地平线内每靴可回溯），每颗核六个能各自归名的字段
  `st=/fl=/rc=/want=/seen=/entry=`；`entry=` 专为 `rc=-4`（`INVALID_ADDRESS`）而加——固件说地址坏，
  与我们自己 `ml_static_vtop` 递出 0（后者现在短路成 `fl=9 rc=-1002`），两件事从此分得开。
  「修之前」那一格仍是 boot 87 的 `clover r271: cpu … real=1 max=8 reg=1 start=0 run=1`。
* **再后一支（本格欠的两条仪器债）**：`vbits=`（`ipc_right_lookup_read()` 已经交出的 entry 位掩码，
  用来分 port set / send-once，不需要对象、锁纪律与 translate 同一条）与 `qmc=/qsr=` 自带返回码。
* **那笔「等设备解锁」的死等已经不是现场，但它的机制现在读全了**：r274 命中的形状是
  `-[DMMigrator _unlockKeyBagUseOfForceIsAuthorized]` → `-[MigratorAlternateAppDelegate
  waitUntilPreboardDidFinishUnlock]` → `dispatch_semaphore_wait(sem, FOREVER)`（宿主复核
  `~/clover-out/datamigrator.macho`：`dyld_info -objc` 直接给出类名；**那个串只在主镜像里**，
  缓存 54 张切片搜不到，`BKDisplayLinkManager` 一类归名不成立、已收回）。逐条读 `__text` 出来的是两个与门：
  全类唯一的 `dispatch_semaphore_signal` 挂在 `alternateSystemApp:didExitWithStatus:` 里，且要求
  **status==0** 与 **`MKBDeviceUnlockedSinceBoot()` 非 0**；`alternateSystemAppDidLaunch:`、
  `didTerminateWithSignal:`、`didFailToLaunchWithError:` 三枚回调只打日志、不发信号。
  ⇒ 真实设备上这笔等待要的是「PreBoard 干净退出 + MobileKeyBag 说已解锁」；本板 PreBoard 确实在跑
  （round258：`pid=170 comm=PreBoard`，missing 的正是 `/arm-io/sep`），而 boot 81/82 里 `r268`/`hint=6`
  两靴都是 **0 行**——这笔死等随之从现场退场，别再当现阻塞引用。
* **keystore 已发布并测到**（`NativeKeyStoreEvents` + `CloverKeyStoreProbe`，回答「无 keybag、开机即解锁」）：
  boot 82 的 `clover r281: aks` 行 `opens=22→27 calls=80→99 refused=0`——**有人在开、有人在读、一个都没被拒**；
  桌面仍未出，欠的回音现在指向链路的下一跳。`MKBDeviceUnlockedSinceBoot()` 在本板到底答什么，宿主读不出来（空的）。
* **单核已坐实，r299 已经去改它（等 boot 88 读回）**：内核 `CpuDataEntries` 只注册一颗核，副核从未被 start。
  boot 87 给的是这一格的基线读数：`clover r271: cpu … real=1 max=8 reg=1 start=0 run=1 s=00000000 tm=237841,0,0,0,0,0,0,0`
  ——**拓扑数到 8 颗（`max=8`），注册只有 1 颗（`reg=1`），而 7 颗从未被请求（`start=0`）**，所以缺的不是
  设备树也不是 `MAX_CPUS`，是「谁去放副核」那一步的开关（`drivers/ArmCpu` 里那条被 `sheng-smp` 关着的
  PSCI `CPU_ON` 路径；r299 把它改名成 `armcpu-smp` 并传上，见上面那条）。
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
* **第三道坎（新的主线瓶颈）：六连「静默、无 panic」复位；第一条因果假说被自己的控制轮杀掉，第二刀（r294b）把「负载是变量」这件事也杀掉了。** boot 77 停在 1248.429585 s（最后一槽半行），
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
  `round289-v0.6.86-payload.img`（boot 76 那颗、最后一颗走到自己死线的镜像）**原样重刷**，判据写在刷之前：
  旧镜像现在也静默复位 ⇒ 变量不在负载；旧镜像再次走到 1800 s ⇒ 差异在 r290 之后的镜像里，下一趟把 GPU 的**读**
  也关掉（`clover-gpu=0`，boot 参数级二分）——boot 76 与 boot 81 之间还差着那三次新读
  （r290 分支读、r291 轨读、r294 普查读），而「读一个没上电的块要不要代价」在本板从没测过。
* **boot 82 落在第一支，而且落得干脆**：同一颗镜像、同一个文件，boot 76 走到 1800 s 并正常 panic，
  boot 82 在 **707.103272 s** 静默复位（boot 82 名下 kind-20 **0 条**；deadline 记录印 `un-stamped`——round289 早于
  r293 不会盖章；两条 panic 标记仍是 `boot=76` 并报 `STALE: boot 82 never entered the sink`；内核环覆盖
  475.035805→707.103272 s）。**镜像相同而结局不同 ⇒ 负载不是变量**，`clover-gpu=0` 那一支作废，
  **GPU 整条线从「整卡复位嫌疑」里摘出去**（它作为桌面那一格的债主照旧）。六起现在是
  718/1184/1248/1335/997/707 s——一条带，没有一靴重复另一靴的秒数。
* **平台自己的跨复位记录＝零（又一记负对照，纯宿主）。** 落回 TWRP 后逐颗盘探：唯一的 dump 分区
  `rawdump`（128 MiB）设备端 `md5sum` 与整颗拉回后逐字节一致（`fde9e0818281836e4fc0edfede2b8762`）、
  **非零字节 0** ⇒ 这块板从没 staged 过 ramdump，六起一起都没抓到；`apdp`/`msadp` 是小米 attestation 证书、
  `ddr` 是训练 blob、`ssd` 8 KiB 二进制、pstore 目录空。⇒「等它崩了拿现成 dump」这条路在本板不存在，
  复位来源只能靠**复位前一刻我们自己写下的读数**或**卡外的物理量**。
* **物理量这一支现在有了正确的测法**：boot 82 落地后读到 `temp=250`（25.0 ℃）、`charger_temp=294`、
  `die_health=Cool`、`status=Full capacity=100 voltage_now=4360340`（`~/clover-out/boot82-landing-twrp-sensors.txt`），
  但**这是复位后 9 分钟的读数，读不成「复位时不热」**——TWRP 空转 9 分钟足以吹凉机身，正是 r293「仪器要能看见它问的
  那一刻」的同一家族。⇒ 改宿主脚本（不动负载、不发版）：落回 TWRP 的第一件事抓 `power_supply`/`thermal` 与
  `rawdump` 头部，再抓卡上日志。之前那句「电池这一支在 rec 里现测排除」按这条收回，等落地即测的读数再说。
* **boot 83 把落地快照第一次自动跑出来，并且立刻证明那次九分钟的读数是低报的**：r295 的脚本改动在 TWRP 的 adb
  出现后约 **25 s** 抓到 `temp=250`（25.0 ℃）而 **`charger_temp=322`（32.2 ℃）、`status=Charging`、
  `current_now=18554`**——boot 82 晚九分钟读的是 `charger_temp=294` 与 `status=Full`，**同一个量差 2.8 ℃ 且方向是
  复位后更热**，可见晚读会系统性低报，那条修正不是仪式。tsens 最高 37.0 ℃、`die_health=Cool`、`system_temp_level=0`
  ⇒ 只能把「热到关机」判成不太可能，**判不出**「与电无关」。`rawdump` 头 512 字节仍全 0 ⇒ 第七起也没 staged ramdump。
  `emmc_therm=-40` 是没接的传感器，不进结论。本靴也是**第七起静默复位**（kind-20 名下 0 条、panic 标记仍 `boot=76`
  报 `STALE: boot 83 never entered the sink`、deadline 槽是那条未盖章旧记录），落点由宿主墙钟界定在开机后
  **约 884–1253 s**，仍在 707–1335 s 那一带；WDT 第八靴同判（`en=0 armed=0`、全程无 `chg`）⇒ APSS watchdog 继续排除。
  卡上最后的动静：内核环尾 `829.762 s`、飞区最新 `909897 ms`，之后到复位没有任何写入（解码器自己报 `gap 118246 ms`）
  ——与「UI 那几个进程全在等不回音的往返」同形，**只记相关性**。
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
| 1 | **backboardd 不回 SpringBoard 的信** | 卡在发给 `datamigrator` 的一笔没人回音的 XPC 往返（上节详述）；keystore 已发布（boot 82：`opens=22→27 calls=80→99 refused=0`）、当年那笔解锁死等已消失（`r268`/`hint=6` 两靴 0 行），workloop 等待者从五条读成三条；**boot 82 新读：债链多一跳**——datamigrator（pid 147）7 线程全等 0 跑，它自己有一笔没回音的 send 指向 `pid 187:com.apple.migra…`（`sr=1 rmc=0`），而 `peer=`/`comm=` 截到 15 字符，这个名字**现在读不满**（仪器缺陷，不是硬件事实） | **r295**：`peer=`/`comm=` 的截断**加宽解决不了**（`MAXCOMLEN+1`=17 字节，是字段的极限不是读者的极限），
所以换第二枚不截断的名字源——exec vnode 的完整名头经 `vnode_getname()` 读在 `bsd/kern/proc_info.c`
（`struct proc`/`struct vnode` 都完整的文件），调用点在任何 task lock 之外，配四态 `ex=`；正对照=pid 58
那行完整印出 `backboardd`。普查同时改成**顺着 send 传递式走 4 跳**（`seen[]` 防环、引用只在
`clover_bb_peer_task` 取与放），终止条件印成七种读数 `lock_busy`/`no_trap`/`no_send`/`no_owner`/
`self_reference`/`cycle`/`depth_limit`；宿主那侧已把当年那笔等待读全（唯一的 `signal` 要 PreBoard 状态 0 **且** `MKBDeviceUnlockedSinceBoot()` 非 0，另外三枚回调只打日志），并且把落地瞬间的 `power_supply`/`thermal`/`rawdump` 快照做进了 `run-clover-unattended.sh`。**boot 83 已按这条读回**：链 `58 backboardd → 148 com.apple.datamigrator → 叶子`，叶子在两拍里换了人（t=657 是 `pid 166 osanalyticshelper`，t=797 是 **`pid 202 exec=com.apple.migrationpluginwrapper`**——两个候选靠 `exec=` 分开，`comm=` 照旧截断；旧 pid 187 别再引用），每一行都带 `end=`，六行 `ex=` 全 `ok`。⇒ **r296（v0.7.0 已发出）**：`end=no_send` 只能说「trap 里那根的 `x3`=0」也就是**纯收**，而捕获规则把它的 `rcv=`/`to=` 留在了 0；r296 给这一行加 `cap=send|recv|none`、在 `recv` 时填回那根的 `rcv=`/`to=`，并用 `clover_sb_port_mail()` 读那枚口的收方向队列 `qmc=`/`qsr=`（前提同 r295：只拿 object 自己的锁、在 task lock 之外调用；checker 多禁 `thread_block(`）。boot 84 四支判据：`qmc>0` 仍在等 ⇒ 回音已入队、缺唤醒；`qmc=0` 且 `qsr=0` ⇒ 没人有权回，属镜像侧；`qmc=0` 且 `qsr>=1` ⇒ 有人能答而没答；连 `rcv=0` ⇒ 那根线程没进 `MACH_RCV`，改把 r236 的用户栈采样挂到叶子上 **boot 84 读回来了**：叶子那行 `cap=recv thid=2e5 rcv=1917 remote=0 to=ffffffff qmc=0 qsr=0`——正对照成立，链也深一跳：**backboardd(58)→datamigrator(144)→migrationpluginwrapper(189)→installcoordinationd(126)**，最后这个人在收，而那枚收口里**既没消息也没发送权计数**⇒ 落判据 (b)（谁本该拿着发送权属镜像侧），(a)(c) 排除；(d) 字面成立但打折：`to=0xffffffff` 按 ms 折算是 49.7 天，本靴离死线只剩 110 s，**不能读成「会自己醒」**，而且单位是推断不是读数。下一支（r297）补对象身份 `robj=`/`rvobj=`+`link=yes|no|unknown`：端口名跨 task 不可比，`hop=2` 的 `remote=7f07`（189 的空间）与 `hop=3` 的 `rcv=1917`（126 的空间）是不是同一枚端口，本格读不出——而 (b) 那句只在同一枚端口上才成立。**这靴还活到了自己的 1800 s 死线**（flight `deadline fired … boot=84`、boot 84 名下 kind-20 一条）⇒ 那串「707–1335 s 静默复位」这一靴没添条目，WDT 三行逐拍在动、仍 `en=0 armed=0`（APSS 继续排除）。**boot 85 用同一枚 v0.7.0 镜像作控制轮，两件事分开落地**：链八行两拍逐跳同形、叶子仍 `cap=recv qmc=0 qsr=0`（只有 pid 与端口名在换：126→119、`1917`→`1e0b`——跨 task 不可比这件事第二次自己走上门来）⇒ 仪器稳定；但结局不同——卡上最后动静 `t=1680923 ms`，没有 boot 85 名下的死线记录也没有 sink 标记（那两枚仍署 boot 84、解码器印 STALE）⇒ **第七起静默无 panic 复位**，上界由死线记录给（跑到了就该有），落在 **1681–1800 s**；带因此是七条 718/1184/1248/1335/997/707/本靴，**「何时结束」在同一枚镜像上是分布不是常数**。WDT 两行在动仍 `en=0 armed=0`，keystore `stage=3 opens=100 calls=442` 继续在涨，落地快照第三次 `charger_temp=327`、`rawdump` 头第四次全 0。**boot 86 把 r297 读回来了**：`link=` 三态一次到齐——凡两边都译得出对象处全 `no`（hop=1 两拍、hop=2 第二拍），决定性那一跳 `unknown`（叶子 `rcv=d03` 两拍都译不成对象），正对照 `hop=0 prev=0 link=-` 与 8/8 配对成立 ⇒ (a) 没出现、(b) 只在一半上成立、(c) 落在提问那一格。**代价是抽掉 (b) 的支持**：`qmc=/qsr=` 与 `rvobj=` 共用同一枚翻译、失败都停在初始化的 0，本靴证明叶子那枚名字译不出 ⇒ boot 84/85 的「队列既没消息也没发送权」从此有两种读法且无法回溯判定；同一支还查明 `rvobj` 译的是 `frcv`（第一根在 trap 内的线程）而不是 `cap=send` 行打印的 `nrcv`。(d) 兑现：三跳 `robj` 两拍逐位相同、`rvobj` 在两拍之间变 ⇒ 指针这把尺第一次自证；叶子换到 `65 containermanagerd`，链形不变。**这靴是第八起静默无 panic 复位**：环尾 1711.714 s、飞区最新 entry `t=1701831 ms`（launchd 的一笔 8 块写、卡仍在正常应答）、没有 boot 86 名下的死线记录与 sink 标记 ⇒ 上界由死线记录给，落在 **1712–1800 s**；带八条 718/1184/1248/1335/997/707/1681/1712–1800，**「何时结束」是分布**第二次落地（v0.7.0/v0.7.1 这一族三靴三种结局）。WDT 三行 `en=0 armed=0`（连续第十靴排除 APSS）、keystore 本靴内 `opens` 63→65、GPU 三拍照常（`ctrl=0x80000038` 与 LDO 词全 0 按 §5.12 读作「符合本板 DTB 配置」而非故障票）、6 笔 `dma deadline` 无 `spin` 记慢性噪声、落地快照第四次（`charger_temp=327`）、`rawdump` 头第五次全 0、`clover r283:` 第二次带全套列上卡仍 `unnamed`。**boot 87 已把 r298 读回来**：`tlink=` 三跳全 `no`，而每一跳的 `tspace=` 恰等于下一跳自己的 `space=` ⇒
**债链在任务这一级逐跳成立（三次，且都在同一地址空间内比）**，不成立的是名字——被采样的线程等的是另一枚；
落预登记判据 (b) ⇒ 下一步普查收方任务自己的 entry 集。`verr=17`＝`KERN_INVALID_RIGHT`（`kern_return.h:151`）
只在 entry 存在而类型不含 RECEIVE/SEND 时回 ⇒ 那三枚名字「存在但不是 port 权」（port set 或 send-once），
boot 86 的 `link=unknown` 由此升一级；同一枚 `d0b` 也正式结掉 boot 84/85 叶子 `qmc=0 qsr=0` 那格（是「没读成」）。
遗留两枚仪器债：`cap=send` 行的 `qmc=/qsr=` 仍没有自己的返回码；helper 只留两次尝试里最后一次的 kr。
本靴跑到自己的死线（`elapsed=1800.003s`、panic 标记署 boot 87），其余各格无退步。 |
| 2 | 触摸没有任何应答 | QUP v2 I2C-3 电气通，两个候选地址全 NACK（`verdict=102`） | 先把面板轨真上电（依赖 3），再问地址 |
| 3 | PMIC 轨读不出来 | SPMI 仲裁器读通路好，但核心版本字不在 v5 带内，APID 一个没认 | 弄清版本字、认 APID、读轨开轨 |
| 4 | ADSP 没起 | SMEM 共享堆读通（`$TOC`/`$PRT` 自校验通过），判决行还没上屏 | 有边就上 GLINK/APR；没镜像先压 `vadsp` 电源 |
| 5 | watchdogd 每次起就崩 | 死因已定名：`bl` 到 `libSystem/__os_crash`，串是 `connect_to_kext() … unable to lookup watchdog KEXT service`。`AppleARMWatchdogTimer` 本体在镜像里，缺的是 `wdt,s5l8960x` 的设备树匹配（provider 是 `AppleARMIODevice`，本板平面未证）；致命已由 `panic_on_crash_disabled=1` 摘掉。**boot 76 带着这枚 brk 活到死线，sheng 零引用 `IOWatchdog` 也进桌面 ⇒ 不是本板的挡路石** | 先量 `AppleARMIODevice` 在本板有没有平面，再谈造 `IOWatchdog` 服务；不在当前排期 |
| 6 | **GPU/Metal 一个都没有**（Adreno 512 无驱动、无固件、无 renderer） | `gpu` 区 `planned`；boot 74 实机：**r286 上电写序列在本板第一次生效**——`cx_on=1`、`gfx3d_en=1`、`cxo_en=1`、AHB/BIMC 三票=1（eMMC 正对照成立），但 `gx_on=0` → `chipid skipped`。**boot 76/77 逐字段复现**，所以 `gx_on=0` 是稳定硬件状态，不是竞态也不是时机。**boot 77 的 r290 读数把「问了」和「在跑」分开了**：`gfx3d=1/1`、`rbbm=1/1`（BIT(0) 按下、BIT(31) 仍 HALT），而 `cxo=1/0`、`rbcpr=1/0` 两支 halt=0 —— 前者是本轮没动过的天然正对照，后者是本轮新按的一票真的把分支启动了。（`rcg=80000000` 曾被读成「父源没设」，已收回；halt 两支的 RCG 都是 `0x80000000`、在跑那支 `0x00000000`，**只记相关性**：BIT(31) 在 RCG 命令字里的名字要等一份归档的 `clk-rcg.c`。见 [`notes/gpu-bringup.md`](notes/gpu-bringup.md) §5） | 分支清白、债在电：`gx` 的 GDSC `gx_collapse=0` 而 `gx_on=0` 是**电没到**，不是使能没按。r291（v0.6.92 已发出）按 §5.6 的厂商表读 GFX LDO，但**不新增映射**：`gfx_ldo_vreg` 的 0x0506e000 正落在 DT 声明的 GPUCC `reg = <0x5065000 0x10000>` 里（偏移 0x9000），于是读 `0x9018` VREF（µV=val*5000+375000）、`0x901c` 开关控制（byp/pdn/cpr_byp）、`0x9028` 的 `LDO_READY=BIT(2)`/`LDO_VREF_SETTLED=BIT(4)`，连同当拍 `cx_on`/`gx_on` 一起打——那两票是这行的正对照：三个词全 `0xffffffff` 而 `cx_on` 照常，是这页没应答，整行作废。checker 把「本驱动只开三张窗口」钉成 `IODeviceMemory::withRange` 计数=3。判据写死：`LDO_READY=0` 或 µV=375000 ⇒ 角从没抬起（债在 CPR4/`vdd_gfx`）；µV 落在 DT 的 400000…925000 且 `ready=1` 而 `gx_on=0` ⇒ 轨清白，回查 GDSC 自己的等待字段与 `gpu_gx_domain_addr`。角数（厂商 `SDM660_MMSS_FUSE_CORNERS=6` vs DT 的 1..7）不印也不下结论。S3/S5 两颗是 `qcom,rpm-smd-regulator`，SPMI 读不到，不在这一格。**boot 78 把两条预登记判据一起点亮**（`beat` 副本顶上，`ldo post` 在环窗外）：`vref=0 val=0 uv=375000`、`sts=0`（`ready=0 settled=0`）、`ctrl=80000038`=`LDO_CLAMP_IO|LDO_PDN|LDO_UNDER_SW_CTRL`+一枚没名字的 BIT(3)，同拍 `cx_on=1` 作正对照 ⇒ **轨自这次上电起既没使能也没编程，「轨清白」这一支关闭**。下一手是对轨**编程**，缺的不是寄存器（三处都在 `msm_gfx_ldo.c` 点得名）而是使能全序与角→电压来源（CPR4 fuse 在 `0x00784000` 另一张窗口），没有 sha256 出处不写。**boot 79/80 逐拍复现同一读数**（`uv=375000 sts=0 pdn=1` 且同拍 `cx_on=1`），轨的状态不是时机问题。**r294 把这格的读数铺成全普查**：`GpuRegs.h` 按厂商 `msm_gfx_ldo.c` 把 0x00..0x30 全部定名（0x20 没有 `#define`，但厂商的 dump 名字表 `register_str[8]`（`:249`）就叫它 `LDO_STATUS0`——普查行仍只印词、不判位，见 §5.11 第 6 条；ADC_CTL 0x34 落在 DT `reg = <0x0506e000 0x34>` 之外，不读），`clover_gpu_report_ldo_words()` 在 post/beat 两拍各印 13 个词 + 当拍 `cx_on/gx_on`，**一个词也不写**——它是下一轮真去编程轨时的基线，r291 那三个词保留自己那行以便和已归档的 boot 78 对比。**boot 81 的普查第一次上卡就当轮给结论**：两拍（t=859/t=980）13 个词里只有 `w1c=80000038`、`w24=0000000d` 非零，其余 11 个全 `00000000`；并排厂商 `sdm660_ldo_config[]` 的期望值（ATEST `0x00000080`、CFG0 `0x0100A600`、CFG1 `0x000000A0`、CFG2 `0x0000C3FE`、VREF_TEST_CFG `0x00401100`、REF_X1 `0x00000230`、REF_X2 `0x00000048`）⇒ **一位都不对**。所以不是「编程到一半」，是这颗 LDO 从复位到现在没人按那张表配过，引导加载器也没配。正对照照旧成立：`w1c` 读出非零、同拍 `cx_on=1` ⇒ 这页在应答，0 是读数不是空窗。`w24` 是 `LDO_STATUS1_REG`（厂商 msm_gfx_ldo.c:54 定义了却从没读过），归档里没有它的位名 ⇒ 印出来、不命名。**同一轮还做了一个控制**：删掉 r290 的 `kGpuccRbcprEn`/`kGpuccRbbmtimerEn` 两笔写（读照旧），因为四起静默复位只出现在带那两笔写的负载上；checker 现在反向拒绝这两行回到源码。**这个控制与紧随的 r294b 一起把 GPU 从「整卡复位嫌疑」里清出去了**（boot 81 删写仍 997 s 复位、boot 82 刷 boot 76 那颗旧镜像仍 707 s 复位）⇒ 本格的债仍在电（轨没人配过），但**它不再是复位线的嫌疑人**，**§5.11 把「下一手」的前提换了**：`msm_gfx_ldo.c` 的 SDM660 分支（`:1560`）不调 `corner_config_init`，所以那份文件里的 fuse 代码与 `ldo_fuse_enable` 在本板从不执行；mode 只由 CPR4 那句 `ldo_regulator_bypass = BHS_MODE`（`cpr4-mmss-ldo-regulator.c:513`）决定，而 `ctrl=0x80000038` 里 BIT(9) PWR_SRC_SEL=0 + BIT(2)/BIT(1)=0 说的是**两个供电级（LDO 与 BHS）都停在复位状态**——比 boot 81 那句「11 个词全 0」更硬，且不依赖那 11 个 0。真正的判据 fuse 在 CPR4 那份文件里点齐了名（row 73 bit 62..57 = 六角的 ldo_enable、row 71 bit 38 = 闭环允许、row 65 = 初压/偏移，全在 DT 声明的 `fuse_base = <0x00784000 0x1000>` 内）⇒ 下一手是**读这三枚 8 字节行**（要按 DT 第二段窗口主动把映射数从 3 抬到 4；行距已结：`cpr3_read_fuse_param` 的本体在**新归档的 `cpr3-util.c:195`**（sha256 `2f3c672d0142…`，`BYTES_PER_FUSE_ROW 8` :24、偏移 row*8 的 8 字节 `readq_relaxed`），与 gfx-ldo 那份逐字一致 ⇒ 三枚行 = `+0x208/+0x238/+0x248`）。**桌面仍 CPU-first** |

1 挡住桌面；6 是 1 最可能的病根（尚未证）；2–4 是之后要补的板级驱动，5 只是噪声（已定名且非瓶颈）。
**六连「静默、无 panic」复位（718/1184/1248/1335/997/707 s）已经判定它是独立的一道坎，不是主线症状**：
r292 把 WDT 的地平线换细以后 boot 79/80/81/82 都排除了 APSS watchdog；r294 这个控制轮杀掉了第一条因果假说
（r290 的两笔 GPUCC 分支使能写——boot 81 带着 `rbcpr=0/1 rbbm=0/1` 的读数仍在 997 s 复位）；**r294b 又杀掉了
「负载是变量」这一支**——boot 82 刷的还是 boot 76 那颗 `round289-v0.6.86` 归档镜像，同一颗文件这次 707 s 静默复位
（boot 82 名下 kind-20 0 条、panic 标记仍署 boot 76 并报 `STALE`）⇒ **GPU 整条线从复位嫌疑里摘出去**，
`clover-gpu=0` 那一支作废。仪器侧再补两记负对照：平台唯一的 dump 分区 `rawdump` 整颗 128 MiB **全零**
（md5 `fde9e081…`，设备端与宿主逐字节一致）——六起一起都没抓到 ramdump；TWRP 的 PON 闩锁没有区分力（已证）。
**所以这一坎现在没有可用仪器**：落地即测的物理量（温度/电量/电压）是唯一的卡外通道，而 boot 82 那份传感器读数
是复位后 9 分钟才抓的，按 r293 的规矩不能当「复位时不热」用——下一支把它做进 `run-clover-unattended.sh`
（落回 TWRP 先抓传感器与 `rawdump` 头部，再抓卡上日志），这属于宿主脚本，不动负载、不发版。

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

- 上表只有 **1** 挡住桌面；**6** 是 1 最可能的病根（尚未证，桌面仍 CPU-first）；**2–4** 挡住板级功能；
  **5** 是已定名的噪声。静默无 panic 复位是**独立的一道坎**（见上表后的说明），不是主线症状。
- **多核**：boot 88 之前，本板仍是单核——上一格读数是 boot 87 的 `clover r271: cpu … real=1 max=8 reg=1
  start=0 run=1`（拓扑数到 8 颗、只注册了 1 颗、副核从未被请求）。放核的整套代码在我们自己的
  `drivers/ArmCpu` 里，被一枚写作 `sheng-smp` 的 boot-arg 关着；r299 把它改名成 `armcpu-smp` 并传上，
  加 `armcpu-self-ipi=1`，另加每拍重播的 `clover r299: smp …` 行。**这份清单要等 boot 88 才能勾**：
  固件答 `DENIED`、或大核缺时钟/电源前置，都还在外面。它不挡「进桌面」这句话里的逻辑，
  但一核跑 iPadOS 17 迟早要还。
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
