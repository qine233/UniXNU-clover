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
* **r299（v0.7.3，按用户指定「把多核修了」）：副核不缺代码，缺的是一枚用别人板子
  命名的开关**（boot 88 已读回，见下一条）。整套路径一直在 `drivers/ArmCpu/ArmCpu.cpp` 里——`probe()` 认第二颗 nub（`:226-241`）、
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
* **boot 88（r299 / v0.7.3）把副核放上来了，八个核都在收 IPI，而桌面那一格因此有了反证**：
  `clover r271: cpu t=835 real=8 max=8 reg=8 start=7 run=8 s=01111111 r=11111111 tm=59695,43260,35219,
  25964,18180,11705,4748,5073`——八颗全注册、七颗被请求、八颗 running，而且**八颗都数到自己的
  architectural timer**（最小的也走了 5073 拍），不是「上线躺着」。新那行逐字段按预登记落地：
  `req=1`（`armcpu-smp=1` 真到了驱动，不再靠 args 表推断）、`con=1`（`/psci method=smc` 读到，
  与 panic 屏每年用的 `SYSTEM_RESET` 同一 conduit）、`rc=0`×7 ⇒ **固件七次都答应，
  「引导加载器会不会 DENIED」这条结掉**、`st=07777777`（最左那个 `0` 是 boot 核那一格，它不走这条路径）、
  `fl=00000000`、`entry=0,a3203000×7`（内核物理复位向量；`fl≠9` 且 `rc≠-1002` ⇒ `ml_static_vtop`
  没递 0，本轮自己那枚短路没触发）。`want=` 对 `seen=` 的差**不是不一致**：驱动自己的比较只取低 16 位
  （`ArmCpu.cpp:590`），`seen=0x80000001` 低 16 位是 `0x0001`、对 `want=1`，多出来的 `0x80000000`
  是 MPIDR 的 Aff3-valid 位，既不在 ADT 的 `reg=` 也不在那句掩码里 ⇒ **七颗就是那七颗**；
  `seen[0]=0`/`entry[0]=0` 同属「没人写过」而不是「读到 0」。同拍另一枚仪器答了「用没被用上」：
  `clover r270: ipi t=835 sent=344711 recv=344557 sgi=72687,70129,60763,52767,31488,25582,17658,13580`
  （boot 87 是 `sent=0 recv=0 sgi=0,…`）⇒ 八颗各收过 1.3 万～7.3 万笔 IPI，**「核上线」与「调度器用它们」
  是两个独立读数，这一靴两个都亮**，`armcpu-self-ipi=1` 作为使能项被读数接住。**但桌面那一格没动**：
  同拍 `r229: bb … hops=3 wait=6`、`r229: sbport … cnt=0 ql=6 sr=1` ⇒ 发送方向依旧没人取走任何东西、
  backboardd 的等待跳数没变，**那笔没人回音的信不是「一颗核忙不过来」**，多核不进修复清单。
  （普查头一行 `wait=17 run=0` **不承担**这条反证：`run=` 是瞬时状态数，单核像上它本来取过 2…7
  ——boot 89 把逐靴序列列在 `notes/development-log.md` 的 §boot 89 读数 ③。）**这一枚仪器留了一条新债**：环里最新一枚槽就是 r299 那行，boot 87 同一拍的下一格
  本该是 `clover r262: th`，本靴 `r262: th`/`r296:`/`r298:` 各 0 行——但这两族的嫌疑**不一样**：
  `r262` 不在每拍推送上限的豁免列里（它自己的类预算 384 行没打满），被闸掉是它的合法解释；
  `r296`/`r298` 在豁免列里、类预算 128 也没打满，所以它们的缺席**不能**读成被闸掉。
  而卡上没有任何 `capped at` 通知点名它们 ⇒ 「被闸掉」那一支要一次**没有通知的**丢弃（r249/r251 已修掉的形状），
  所以按「写流在这里停了」记；**但一次 fire 没有「打完」的标记，这一枚仪器分不清「被打断」与「少打一半」**
  ——下一格补 fire 尾部自证。**结局是第九起静默无 panic 复位，也是这一族目前最早的一靴**：boot 88 名下
  kind-20 记录 0 条、没有 boot 88 的死线记录（卡上那枚仍署 boot 87、印 STALE）、没有 boot 88 的 sink 标记；
  下界由内核环最后一行给（**835.009 s**）、上界由宿主落地读回给（18:58 冷刷、19:14:16 拉卡 ⇒ ≈9xx s）。
  带九条 718/1184/1248/1335/997/707/1681/1712–1800/**本靴 835–9xx**；**八颗核上线之后结局前移了一大截**，
  这是一靴的观察不是结论——负载（r294b 已证）、镜像（boot 85 已证）之外只剩核数这一个刚变的变量。
  WDT 两拍 `en=0 armed=0`（**连续第十二靴**排除 APSS）、keystore `opens 9→25 calls 29→79 refused=0`、
  I2C `verdict=102` 两拍同值、GPU 两拍照常、`dma deadline` 本窗 0 笔、落地快照第七次
  （`charger_temp=327 temp=250 die_health=Cool`）且 `rawdump` 头第七次全 0。
* **boot 89（round299 同一枚镜像的控制轮）两件事同时落定：八核这条路可复现，而那条尾巴的缺席也可
  复现**：`clover r271: cpu t=601 … reg=8 start=7 run=8` 八颗都数到自己的 timer，
  `clover r299: smp t=601 req=1 con=1 run=7 st=07777777 fl=00000000 rc=0×7 entry=0,a3203000×7`
  与 boot 88 **一字不差**，`r270: ipi … sent=296411 recv=296290` 也是非零收发 ⇒ 「开关→注册→上线→互发
  IPI」不是这一靴走运。而 `clover r262: th`/`r296:`/`r298:` 本窗又是 **0 行**（round298 那枚镜像的
  boot 87 同一段环里是 33 + 8 + 8，round297 那枚是 41 + 8）——当时判成「跟着 r299 这枚镜像来的性质」，
  **boot 90 换了镜像（r300）尾巴仍然整段没有，所以这条归因已降级：「跟镜像」与「跟核数」两条假设都还活着**
  （单核像 86/87 都送出、八核像 88/89/90 全 0）。这正是 r300 那两行要切开的东西：`done th=` 非零而卡上 0 行 ⇒ 仪器（每拍推送上限），
  `done th=0` ⇒ 内核这一拍没进循环。**结局当时记成「第十起静默无 panic 复位」，这个词已被 boot 90 撤回**
  （见下面 boot 90 那条：`panicDrainRings()` 只有一个调用者，而本靴名下有 `drain enter boot=89 …
  append_rc=2`，所以 boot 89 进了 panic sink，只是文案被卡拒了）：内核环最后
  一行 661.003 s（`clover r225: sbport t=661`）、驱动 flight 里本靴最后一笔块请求 650.759 s
  （`pid=252 comm=mapssync`、`xpcproxy`，`stuck=1 recov=1`）、最新心跳 536.228 s；kind-20 记录 0 条、
  卡上死线记录仍署 boot 87、panic 标记两枚一枚是 boot 87 的 STALE 一枚是没写过的槽（`boot=0 gates=0x0()`）、
  `misc` block 0/1 全空。带十条：718/1184/1248/1335/997/707/1681/1712–1800/835–9xx/**661–7xx**。
  预登记的两支（「与 boot 88 同带」「回到 1681–1800 那一族」）**都没落**，落的是第三支：同一枚镜像两靴都早、
  且早得不同——八核像的开始时间被压到 660–940 s 这一段。两靴一像只能说「分布挪了位」，说「核数挪的」还差一票；
  r300 只加两行 printf 与两个计数器、不动任何硬件路径，所以 **boot 90 同时也是仪器轮的控制轮**。
* **boot 90（r300 / v0.7.4 的读回）：仪器不自证，而这一靴其实喊过一声、嗓子被占了**。八核第三次上线，
  这次**跨了镜像**：`r271: cpu t=698 real=8 max=8 reg=8 start=7 run=8`、`r299: smp … req=1 con=1 run=7
  st=07777777 fl=0 rc=0×7 entry=0,a3203000×7` 与前两靴逐字段相同、`r270: ipi sent=250500 recv=250415`
  ⇒ 多核不是某一枚镜像的运气。桌面那一格照旧没动（`threads=24 shown=20 hidden=4 wait=22`、
  `sbport bb nm=2d03 cnt=0`、债主仍 `own=58:backboardd`）。**r300 的读数落在预登记五支之外**：卡上
  `clover r300: fire` 与 `done` 各 **0 行**，而这不是地平线问题——kernel 环 512 行的 `seq` 我逐条核过
  **1194…1705 连续无洞**，`r299: smp` 是 seq 1657（698.750532），下一行 seq 1658 是 698.977911 的别人的行，
  `fire` 本该坐在两者之间 ⇒ 每一支都要求 `fire` 先到，**五支全落空，只能记「没量到」**。
  错在我自己：那两行 printf 与它们要量的行**走同一条管道**（printf → msgbuf → tee → 卡上环），
  缺席必然与被量物同形——「判决行要有一条不经该管道的副本」这条规矩，我这次把副本印进了 burst 里。
  **本靴真正的新事实是它的结局不是静默**：flight `panic seq 0 slot 0 t=686630ms stage=0 log_ready=1
  host=0x20fdf rc=0 boot=90 gates=0x7(armed,bcb,text)`（入口标记落了、出口标记没落），verdict 环里
  `r212: drain enter boot=90 … append_rc=2` / `panic tail … lap=29` / `r184: panic drain wrote=0` /
  `drain exit boot=90`——`panicDrainRings()` 在整份源码里只由 `clover_emmc_panic_append` 调用 ⇒ **sink 跑过**，
  而 `append_rc=2` 是 `panicAppendText()` 在写之前**读**报告槽那笔 `readPioBlock` 失败（`:8824`），
  配上 `host=0x20fdf≠0`（那一刻控制器被占）⇒ 一次性尝试、不重试 ⇒ **卡上没有 boot-90 的 kind-20 记录，
  panic 那句话丢了**。时钟对齐（boot 87：sink 1803.044 s vs 环尾 1814 s）说明驱动 ms 钟慢约 9.8 s，
  所以 sink 入口 ≈ 内核 696 s，而普查那一拍在 698.75 s——**尾巴丢的那一刻正处在 sink 已开跑、guard 被占的窗口里**
  （同窗还有「最新心跳 587.938 s vs 最新请求 736.607 s ⇒ 148.669 s 没有一条心跳找到过空闲 guard」、
  `card busy 5285 ms`）；**这条不作因果结论**（boot 88 没进 sink 尾巴也整段没有），只是把两个缺席放进同一个待测嫌疑。
  结局时间带 **746–7xx s**（下界环尾 746.396 s，本靴最后两行是 `r286/r290: gpu`；上界由 20:56:05 读回扣
  recovery+读回，墙钟只有 ±30 s 把握）⇒ 三条八核靴三个时间（661–7xx / 746–7xx / 835–9xx），**分布而非时刻**，
  「核数」与「镜像」谁挪的仍差一整轮控制。三笔仪器债登记在 `notes/development-log.md` §boot 90 ⑧。
* **r300（v0.7.4，仪器债）：一次普查 fire 现在自己说「打到哪儿」**。`task.c` 里那一拍加编号与收尾：
  `clover r300: fire t=%llu seq=%llu pid=%d th=%u hidden=%u`（`th=` 打算打几行线程、`hidden=` 被
  `CLOVER_BB_CENSUS` 挡在卡外的线程数）打在 r299 行之后、线程循环之前；
  `clover r300: done t=%llu seq=%llu th=%u chain=%u`（`th=` 真打出去几行、`chain=` `clover_bb_chain()`
  真走完并打出行对的跳数）打在链返回之后。**被数的三行一行没改**（`r262`/`r296`/`r298` 与 boot 87
  逐字节同形），所以 boots 50–89 仍可比，这一轮只在它们外面加了一个括号。判读态五支已在
  `notes/development-log.md` §Round300 ② 预登记，其中两支直接对上 boot 88/89 的形状：有 `fire` 无 `done`
  ⇒ 这一靴被自己在中途截停（复位时机第一次有秒级下界）；`done th=19` 而卡上 0 行 ⇒ 每拍推送上限吃掉了
  整条尾巴，那么「线程行没说话」这件事从此与内核无关，桌面排程不该再引用它。驱动侧新类
  `{ "clover r300: ", "r300-fire", 64 }`（每 fire 两行、二十次 fire＝四十行，与 r270/r271/r299 同一份余量）
  并把 `r300` 加进每拍上限的豁免列——**这一行是给别的行的缺席定价的，它自己被节流掉就等于把假空造回来**。
  板级补丁 `z`×59 `-clover-r300.patch`（`task.c`，+33 行）两份逐字节同装，`LATE_PATCH` 头名（摘除序
  r300→r299→r298，因为计数器归零坐在 r299 那行与线程循环之间、跳计数器坐在 r298 注释封口处）；
  `check-clover.py` 钉两行完整格式串与三个计数器并禁止重印被数的行，`verify-clover-payload.py` 加
  `bb-census-fire`/`bb-census-done` 两枚整串针，守卫做过正对照（摘类行 / 摘豁免各 EXIT=1 并指名）。
  **boot 90 已读回：落在五支之外——`fire`/`done` 自己也没到卡上，所以这债没结清，副本必须挪出那条管道（r301）**。
* **r301（v0.7.6，仪器债：把 panic 的那句话做成两份，第二份不走被测管道）**。boot 89/90 都走进了
  `clover_emmc_panic_append`（各自带 `drain enter boot=NN append_rc=2`），但**卡上没有 kind-20 记录**：
  `panicAppendText()` 写之前要先**读**报告槽找最老的一格，那次预读被拒（boot 90 的标记同时给 `host=0x20fdf`），
  而它是一次性、不重试的 ⇒ 这条板上价值最高的那句「这靴为什么结束」丢在最后一步。**副本交给 RAM 环而不是
  再交给一次块写**，依据是同一帧里的实测：boot 90 那五行排干行 `said by driver`、kind 21、slot 40–45 都在卡上，
  而它们自己说 `wrote=0`——环里的行会被**之后每一次 flush 重试**（panic 屏之后还有 15 s 复位延迟），
  块写路径没有这第二手。做法：`clover_emmc_panic_append` 在置起 `gPanicActive` 之后、`flightPanicMark` 之前
  把入参抄进 `gPanicStash`（480 B，与报告正文同宽；非可打印字符折成空格）；`panicDrainRings()` 在排干前
  按 240 字符分行说 `CloverSdhci r301: panic text rc=%u chars=%u part=%u %s`，**且只在
  `gPanicAppendRc < 100` 时说**（`rc>=100` 表示报告记录已落，重复它会让宿主把一次 panic 读成两次）；
  状态行 `CloverSdhci r301: panic copy rc=%u chars=%u said=%u` 无条件说。负断言钉死**副本不得骑 tee**
  （`{ "clover r301` 不许出现在 tee 类表里），否则这一轮白做；顺带把 `check-clover.py` 三处固定 600/900
  字符窗的 sink 断言改成按 `\n}` 收口取函数体（我这轮的注释把调用推出了窗外，三条假 FAIL）。
  驱动侧单独一轮：不动内核补丁、不动 boot args、不动 tee、不改 `panicAppendText` 的 `rc` 编码（那是 patcher 的契约）。
  **boot 91 的五支判读态（含「没有 `drain enter`＝这靴压根没喊」这一支）预登记在
  `notes/development-log.md` §Round301 ⑤**；债 (2)（tee 自报行只有 12 条）与「尾巴属镜像还是属核数」那轮控制仍未动。
* **boot 91（r301 / v0.7.6 的读回）：这一靴落在预登记五支之外，而它把一个更基本的坑翻了出来——判决记录读起来像读数，其实是两次写的混血**。
  八核第四次上线、跨第三枚镜像（`r271 real=8 reg=8 start=7 run=8`、`r299 req=1 con=1 run=7 st=07777777 fl=0 rc=0×7 entry=0,a3203000×7`、
  `r270 sent=271685 recv=271562`），桌面那一格照旧没动。**r301 无法在这一靴判读**：卡上既无 `drain enter`、也无
  `r301: panic text`/`panic copy` 任何一行，而飞区 panic 槽里那一格署名 boot 91、`event=5 dir=3`、
  `t=631323 ms`，**八个 extras 却只有 word 35 有值、其余全 0，同时 header 带着 `blocks=8` 和活的 `stage_lba/stage_pid/comm=osanalyt`**。
  逐字反推 + 枚举源码后确定：**没有一次 `flightWrite` 能同时产出这两套字段**——`event/dir=5/3` 只出自
  `flightPanicMark`，而 `blocks=` 与活的 stage 只出自 `flightEnter`/beat，且 boot 戳只在 `extraWords == 0` 那条路上打；
  `gFlightBlock` 是**唯一一块共享记录缓冲**，sink 与并发块请求在同一次 FIFO 传输里把它重建 ⇒ 卡片收到的是混血。
  **boot 88 那一格与此逐字同形** ⇒ 不是 boot 91 偶然、也不是 r301 引进（它一行 `flightWrite` 都没碰）。
  于是这一靴**两支都活着、我不选边**：(a) sink 进了、marker 头几个字落了、后续被拒且复位前排干一次没写成；
  (b) sink 没进、`5/3` 另有来源。分不开的原因是**marker 记录没有自己的 magic**，读者把 r293 的 boot 戳当成 `stage=91` 印了出来。
  结局时间带 **642.7–7xx s**（下界内核环尾 `642.683981`，上界读回墙钟；marker 的 `631323 ms` + 已知钟偏 ≈ 641.1 s
  ⇒ **驱动钟与内核钟第一次互相验证**）。八核像四靴四个不同时间（642.7–7xx / 661–7xx / 746–7xx / 835–9xx）
  ⇒「分布」第四次落地，但**「核数 vs 镜像」那一票仍未投**；普查尾巴 `r262: th` 与 `clover r300` 这一靴仍各 **0 行**
  ⇒ **连续四靴、跨三枚镜像**。**本轮已改的是读者（纯宿主）**：`read-clover-log.py` 对 kind-23 panic 格加拒读规则
  （stamp/gates/log_ready/log_first 全 0 ⇒ 印「这里没有 marker」并保留 `blocks`/`pid`/`comm` 作为现场证据），
  `--selftest` 用手工拼的混血做正对照，变异测试 `if False` ⇒ `AssertionError`，boot 89/90 两窗不误伤。
  两笔设备侧债（marker 独占缓冲 + 自带 magic；句子要有一条**不预读、不靠后续 flush** 的盲写路径）记为 **r302**，
  形状按用户规矩先查 sheng 的对照做法再动。详见 `notes/development-log.md` §boot 91 ①–⑧。
* **r302（v0.7.8，仪器债：判决记录不再与流量共用缓冲，句子不再等第二次机会）**。形状全部由 boot 91 那一格混血点名，
  四条照 sheng 抄（`~/UniXNU-sheng`：panic sink 根本不碰存储栈、writer 线程独占它的缓冲、一次性记录带 magic 且认证头最后写、
  槽位由 `slots_written % slot_count` 盲写不预读——见 `notes/development-log.md` §Round302 ②）。本板落地：
  新增 `gFlightMarker` 独占缓冲，`forced` 事件（deadline / panic marker / 句子）一律建在它上面，
  流量那一半留在 `gFlightBlock`；`kFlightMarkerMagic = 'CLVM'` 写在**自己 extras 之后的那一格**且**在 extras 之后落笔**
  （deadline word 46、marker word 43、句子 word 126），读者按事件表查偏移，缺签名就印「absent」而不是印假字段；
  句子记录住进那 12 格无主区（`kFlightTextOffset = 44`），**槽位 = boot id % 12、一个字节都不先读**
  ——`panicAppendText()` 那笔「读满 16 格找最老」正是 boot 90 丢句子的地方（`append_rc=2`、一次性、不重试）；
  它复用 r301 已抄好的 `gPanicStash`，**只在 `append_rc < 100` 时写**，就在 sink 里、退出 marker 之前，
  所以句子从此有一条**不依赖后来 flush** 的出路（boot 91 证明「后来的 flush」可以整靴不存在）。
  一块 512 B 装 352 个字符，全文长度放在 header 的 `blocks=` 位 ⇒ `chars < total` 读作「记录截断了」而不是「panic 变短了」。
  状态行 `CloverSdhci r302: blind ok=%u chars=%u total=%u slot=%u` 无条件说，作用是把「这笔写被拒」与「排干根本没跑」切开；
  `r302` 负断言不进 tee 类表。**改前先想清的一个坑**：句子写在每靴固定一格上，若照其它事件走 `(*seqp)++` 会把
  entry 环的光标推走、下一靴所有请求记录错位旋转 ⇒ `seq` 取 `gBootId` 且不推进，checker 钉住。
  内核 `1237fa95` → AMFI 两刀仍在 `0x1ad70e4`/`0x1ad716c` → 归一化 `f2c048bc`；
  `round302-v0.7.8-payload.img` 58,720,256 B，sha256 `f26c10376eb0d9f9017d0897b1fd8d27cf55f17305824c999227aa807dd08284`；
  两道闸门 EXIT=0（verify 50 项 OK，新增 `panic-blind-row` 针），变异正对照六支各 EXIT=1 并指名，改完与备份逐字节相同。
  **boot 92 的七态判读（marker 侧三支 + 句子侧四支）预登记在 `notes/development-log.md` §Round302 ⑤**；
  其中「槽被拒读而 `drain enter boot=92` 存在」那一支是本轮最重要的一支——若落地，说明混血不是共用缓冲造的，
  诊断要从「谁重建缓冲」挪到「谁重放传输」。普查尾巴仍四靴 0 行、`r300: fire/done` 那份非管道副本仍欠，
  桌面主线（#577）与「镜像 vs 核数」那轮控制都没动。
* **boot 92（r302 / v0.7.8 的读回）：marker 的签名第一次落地，句子仍写不进卡——这一靴把「卡在 sink 里拒收」钉成了形状**。
  判读落在预登记的两支：**marker 侧 (a)**——`panic seq 0 slot 0 t=564828ms stage=0 boot=92 magic=yes`，
  独占缓冲 + 签名成立，boot 91 那格混血（`event=5 dir=3 blocks=8`）不再出现；
  **句子侧 (f)**——`CloverSdhci r302: blind ok=0 chars=352 total=480 slot=8` 且 flight 段 `no sentence record` ⇒
  这笔盲写**有正文可写却被卡拒收**。三笔同病：`append_rc=2`（append 的 pre-write read 被拒，与 boot 90 同款）、
  `blind ok=0`、且 boot 92 的退出 marker（slot 1）缺席——sink 里**只有进门那一笔写成了**。
  结构上更硬的证据在 verdict 环时间线里：seq 39/40 = `r172: dma cmd lba=27830960` → `r159: multi read fail lba=27830960`
  发生在 drain enter 之前，seq 51/52 = `r169: read gave up` → `r215: card recover why=gaveup` 在 drain exit 之后，
  而中间 drain 的 verdict 环行**照写不误** ⇒ 不是「卡全死」，是「卡在 sink 那段对 flight 写拒收」，
  预登记 (b) 支的方向（诊断从「谁重建缓冲」挪到「谁重放传输」）这一靴提前兑现。report 环最后一笔是 boot 87
  （16 格 seq 34–49，boot 88/89/90/91/92 五连靴全 `append_rc=2` 没留一张 kind-20）。
  结局 ≈645–7xx s（卡上最后一笔 645.4 s、无 deadline 记录）⇒ 八核像五靴五个时间，分布第五次落地；
  「核数 vs 镜像」那轮控制仍未跑。**债务重估**：r302 结清「判决记录独占缓冲 + 自带 magic」，
  未结清的是「句子到不了卡」——写法有了（盲写不预读），剩下的是 sink 里那笔写为什么 false（**这条驱动侧
  `writePioBlock` 失败自证仍未付**，往后排）。详见 `notes/development-log.md` §Round302 读数。
* **r303（v0.7.10，桌面主线 #577：付的是 boot 87 §⑩ 预登记的主线债 (4)+(5)，不是上一段那句驱动侧自证）**。
  `patches/xnu-clover/zzz(60)-clover-r303.patch`，六处改动全在 `osfmk/kern/task.c`：
  **(a)** 新行 `clover r303: hop=%u t=%llu rpid=%d rnm=%x rentr=%d rvbits=%x rwait=%u rwtid=%llx rbusy=%d`，
  与 r298 行按 `hop=`/`t=` 配对，**去普查收方任务自己的 ipc entry**——发送方写进去的那枚名字（`rnm=tname`）
  在这个任务里是什么、有没有线程真等在它上面（`rwait/rwtid`，在 `lck_mtx_try_lock` 下数——**try 不是 lock**，
  普查要在卡死的用户态旁边站着说话）；**(b)** `clover_sb_port_obj()` 加 `uint32_t *vbits`，r298 行末尾多印
  `vb_r=%x vb_v=%x`——把 `ipc_right_lookup_read()` **已经成功交出的** entry 位掩码印出来，将
  `verr=17` 那枚译不出的名字分成 port set / send-once / 死名，**不需要对象**；**(c)** `clover_sb_port_mail()`
  加 `int *err`，r296 行的 `qmc=/qsr=` 从此带 `qerr=%d`，让「0 只在同排返回码为 0 时算读数」这条规矩也管上
  `cap=send` 行。**动手前先验的前提**：`ipc_right_lookup_read()` 会 SMR 解引用 `&space->is_table`，
  所以普查先 `next->itk_space != IPC_SPACE_NULL`、NULL 时报 `rentr=-2` 不硬调。r303 行有独立 tee 类 `r303-hop`
  预算 128 + 每拍推送豁免；r296/r298 只加尾列不动既有列，boots 50–88 仍与 89+ 对齐。内核 `d19361ee`（AMFI 两刀
  仍在文件 `0x1ad70e4`/`0x1ad716c`、VA 不变）、归一化 `3da8fdb5`；`round303-v0.7.10-payload.img` 58,720,256 B，
  sha256 `3c2e7be48a683e611eb85e5470923d0b8eb654621e4308aa1e568a72f4df894b`。`check-clover.py` 与
  `verify-clover-payload.py` 双 EXIT=0（新增整行 `bb-receiver-census` 针）。**boot 93 判读预登记在
  `notes/development-log.md` §Round303 ⑤**：`rentr=0`+`rvbits=20000`+`rwait>0` ⇒ 任务级债链闭合；
  `rvbits=80000`(PORT_SET) ⇒ boot 87 猜中那支、本仪器第一次分得开集合与一次性；`40000`/`100000` ⇒ 链形重画；
  非零非 -2 的 `rentr` ⇒ 上跳对象与本名不是同一 entry；`rbusy=1` 与 `rwait=0` 是两种不同的「没看到」。
* **boot 93（r303 / v0.7.10 的读回）：这一拍落在环的最新一行、尾巴没冲上卡——r303 没有任何读数，且落进预登记之外。**
  冷刷 `round303-v0.7.10-payload.img`（`3c2e7be4…`），boot 92→93，落回 TWRP。身份对（SMP 八核逐字段复现
  `r271 real=8 reg=8 start=7 run=8`、`r299 req=1 con=1 run=7 rc=0×7`、`r270 sent=261965 recv=261866`），
  但**普查那一拍（`clover r262: census t=555 threads=18 rx=6`）就是 kind-22 环的最新几行之一**（seq 1571，
  环跨度 255.4–555.4 s），它和 `r270`/`r271`/`r299` 一起上卡了，而**逐跳尾巴——`r262 th`/`r296`/`r298`/`r300`/
  `r303`——各 0 行**：本轮的 `rentr/rvbits/rwait/vb_v/qerr` 全未取到，§⑤ 五支一支都没有输入。
  机制是 r212 那条、不是 r303 的 bug：这一族尾巴骑 `printf→msgbuf→tee→环`，**tee 只在块 I/O 尾部泵**，而这张卡
  的 flight 记录到 543.65 s 就没了块请求，普查在 555 s 印进 msgbuf 后**没有下一次块尾部来泵它**；这靴又**没进
  panic sink**（两枚 marker 全 STALE＝boot 92 entry / boot 87 exit，解码器直说「boot 93 never entered the sink」，
  死线记录还是 boot 87 的）⇒ **没有第二次 flush 来重试环**。所以 boot 93 不能把「尾巴被节流／尾巴在环地平线外／
  没有后续 flush」三种缺席分开。这正是 §⑤ 早标「不作本轮结论」的普查尾巴债，**现在坐实到 r303＝连续第六靴、跨五枚镜像**。
  八核像：boot 93 是这一族**第一靴连 sink 都没进**、停在 ~555 s（比前五靴 642.7–835 更早），仍是一张同族票、
  「核数 vs 镜像」控制未跑。**下一手（r304，主线前置）**：照 r212 给内核探针做的那样，把这一族逐跳行改抄进
  **驱动 RAM 环 + 周期/排干 flush**（不经卡、不经块尾部节奏），这样一靴哪怕 555 s 就停、没进 sink，尾巴也能靠那次
  flush 到卡；在那之前桌面主线那一格读不出、**既不记失败也不记 (a)/(b)**。详见 `notes/development-log.md` §Round303 读数。
* **r304（v0.7.12，主线前置：给普查尾巴一份不骑被测管道的副本）**。两半。**内核**
  `patches/xnu-clover/zzz(61)-clover-r304.patch`（只改 `osfmk/kern/task.c`）：新增
  `clover_census_row(line)`——`printf` 之后再 `clover_emmc_probe_say(line, strlen(line))`（r212 的中转、一字未改），
  六处打印点（`r262 th`/`r296`/`r298`/`r303`/`r300 fire`/`r300 done`）由裸 `printf` 改成 `snprintf(crow[256])`+helper，
  **格式串逐字节不变**（boots 50–93 仍与 94 对齐）。**驱动** `CloverSdhci.cpp:4799` 新函数 `probeFlush (why, cap)`：
  r212 的排干原本每次重读整窗——只有它一个消费者时成立，加了消费者就会重复送行，故改成**单游标** `gProbeNext`
  由 tail/beat/drain 共用、只有卡收下才推进；未密封最多等 4 拍（`kProbeSealRetries`）再跳过并计数，卡拒收**不**推进，
  每拍上限 16 条。两个新泵：块尾（`flushVerdictRing()` 末，`why=tail`）与 **CPU0 的 4 s 节拍**
  （`clover_emmc_flight_beat()`，`why=beat`），panic 排干保留（`why=drain`、`cap=0`）。自证行
  `CloverSdhci r304: probe why=… wrote=… seen=… next=… pend=… flushed=… skipped=… stalled=… refused=… dropped=…`——
  `pend=seen-next` 就是 boot 93 只能推断的那个「已在 BSS、未上卡」的数字；报告自身限额 64 条、积压时每 64 拍一次
  （别再犯 r251）。**动手前量过的前提**：`kProbeTextBytes=256` 而历史最宽一行是 r296 的 **228 B**，两份副本同一字节
  截断、卡与串口不会就同一字段说两种话；`probe_say` 只写 BSS（原子＋拷贝＋release 密封，无锁无睡眠），在普查那条
  try_lock 路径上合法；r304 在 `LATE_PATCH` 必须**先于 r303 反打**（它改的正是 r262/r296/r298/r300/r303 自己写的行）。
  **只搬预登记的五族**，`r267`/`r265` 那几行没被挪进中转；三条负断言（中转点必须 6 处、不许恢复整窗重读、普查路径
  不许任何睡眠/取锁调用）写进 `check-clover.py`。内核 `12a76a24`（AMFI 两刀仍在 `0x1ad70e4`/`0x1ad716c`）、归一化
  `3acfb517`；`round304-v0.7.12-payload.img` 58,720,256 B，sha256
  `431a70d3a4dbbadedbfd2e871acef98b58beb9fb63db9d6686949cbaf8e2621e`；双 checker EXIT=0（新增 `census-relay`、
  `census-relay-onecopy`、`probe-beat-flush` 三针）。**boot 94 判读预登记在 §Round304 ⑤**：(甲) 尾巴齐 ⇒ §Round303 ⑤
  五支第一次有输入；(乙) 有 `fire` 无 `done` 且 `pend>0` ⇒ 消费没跑完，再看 `dropped>0` 分清「中转环自己成了新地平线」；
  (丙) 零 kind-25 且零 `probe why=` ⇒ 驱动消费者根本没跑，**不能读成内核没抄**；(丁) `seen=0 wrote=0` ⇒ 双投递没进镜像，
  记在镜像身份而非普查；(戊) 只齐某一族 ⇒ 那一族自己没产生，是内容不是管道。未付：`writePioBlock` 自证、「核数 vs 镜像」控制轮。
* **boot 94（r304 / v0.7.12 的读回）：中转这台仪器被证明是好的，而普查尾巴一行都没进它——并且这一靴第一次拍到
  「panic sink 的入口与普查那一拍在同一靴里同场」（v0.7.14 更正：两者的先后与间隔读不出，见本节末与 §Round304 读数更正）。**
  冷刷 `round304-v0.7.12-payload.img`（`431a70d3…`），boot 93→94，落回 TWRP；八核身份逐字段复现、显示通路还活着
  （`r180 frame#5 / copy#4 135 ms / retire#4 done=4`）。仪器侧：五条 `CloverSdhci r304: probe why=tail` 的 `seen`
  13→14→31→48→55、每次 `next==seen pend=0`、`skipped/stalled/refused/dropped` 全 0，55 条 kind-25 逐条 `ok`，而且
  **同一个中转把 r207/r208/r211/r218/r226 五族 kill 现场行原样送上了卡**（正对照）、单游标没重复送行。但**本轮那六族
  （`r300 fire`/`done`、`r262 th`、`r296`、`r298`、`r303`）在 kind-22 与 kind-25 里都 0 行**：那一拍只打到自己的头四行
  （`r262 census t=741 threads=19` → `r270` → `r271` → `r299 [741.380886]`）就断了。这**不是 §⑤ 五支里的任何一支**，
  是新的一格：**管道活着、行没进管道**。
  **同时收回一句**：boot 93 那句「卡到 543.6 s 就没块请求、尾巴没有下一次块尾部来泵」是跨钟推断——flight 记录的 `t=`
  是**驱动上膛以来的毫秒**（`flightCounterMs(gFlightStart)`，CloverSdhci.cpp:3642/3715/7603），不是内核 uptime。
  （v0.7.14 再收一层：本节原来写「boot 93/94 差 11.83/11.65 s、反推 boot 87 的死线记录也自洽」——**那三个数各自都是
  拿一对「是否同时」正待判的事件相减得来的，不是测量**；能站住的只有上面那句结构事实。）
  新落的第一格：这一靴 **panic sink 的 entry marker 落地了**（`t=729750ms stage=0 rc=0 boot=94 magic=yes
  gates=0x7(armed,bcb,text)`）。它落在内核哪一秒**读不出**：符号有界（上膛在 XNU 起来之后 ⇒ `flight ms ≤ kernel secs`），
  量值没测过 ⇒ **原句「折算 ≈ 内核 741.4 s、正落在 r299 与 `fire=` 之间那三个自增上」作废**。站着的是：marker 与普查
  头四行在同一靴里都上了卡，而连续七靴尾巴 0 行第一次有了**同靴的 panic 现场**；至于是「普查打到那儿出事」还是
  「sink 在普查之前就已经动了、普查的头行是一个进行中的 panic 的旁证」，两支都活着。最后一枚有逐跳尾巴的靴（87）也正是八核打开前那枚——那是相关，不是控制。
  第二格：r302 的 marker 第一次在真 panic 上成立（独占缓冲 + 自带 magic 结清），但句子那圈 kind-20 **仍冻结在 boot 87
  seq 49**、零条 `why=drain`、boot 94 的 stage=1 出口记录不在卡上（slot 1 仍是 boot 87 那枚）⇒ 「sink 进去了、marker
  写得上、之后的排干与句子没上卡」（**「没跑」与「跑了、那一笔卡写被拒」在读分不开**；`drained=` 这一列已从证据里划掉：
  排干跑在两枚 marker 之后，CloverSdhci.cpp:9220→:9221，任何 marker 上它结构上都是 0——boot 87 那枚句子确实上了卡的
  stage=1 marker 也印 `drained=0`）；那句
  `writePioBlock` 失败自证从卫生**升级成主线的挡路石**。第三格：r304 自己写漏的前提——**五条报告全是 `why=tail`、
  零条 `why=beat`**，因为 `probeFlush("beat")` 挂在 `flightWrite(kFlightEventBeat)` 之后，而 `clover_emmc_flight_beat()`
  在 host 被人拿时提前 return（:3911-3941），这靴 beat 停在 602,343 ms（gap 127 s）⇒ 第二个泵整靴没跑，这是仪器缺陷。
  结局带：642.7/~645/661/746/835/555/**742**（第八票；「与普查同位」只就内核环最新行 `[742.030664]` 与 r299
  `[741.380886]` 这同一枚钟成立，不就 marker 成立）；`charger_temp=365` 家族新高。
  **下一手 r305（三小条）**：① `clover_census_row` 改成**先中转再 printf**——把「走到这一行」与「进得了 msgbuf」分开，
  boot 95 若 kind-25 有 `fire` 而 kind-22 没有就是停在控制台侧，两边都没有就是根本没走到；② **原写「节拍消费者挪到
  owner/guard 之前」不成立**（`probeFlush`→`ringWriteRecord`→`writePioBlock` 写卡要 host ticket），改成「饿掉的节拍
  自己计数（`held=`/`young=`/`panic=`，随下一次跑成的报告印出来）＋一枚两把钟同记录的交接记录（内核秒 ↔ 上膛毫秒）」；
  ③ `writePioBlock` 自证，让句子能上卡、给「谁在普查那一拍附近动手」一个名字。
  详见 `notes/development-log.md` §Round304 读数 与文末 §Round304 读数更正（v0.7.14）。
* **r305（v0.7.15，把上一段预登记的三小条一次付清：三件仪器，都只问自己那一格）**。**内核**
  `patches/xnu-clover/zzz(62)-clover-r305.patch`（只改 `osfmk/kern/task.c`）：`clover_census_row` 里把
  `clover_emmc_probe_say` 挪到 `printf` **之前**，并**删掉**旧位置那枚 `printf("%s", line)`——kind-25 有行而 kind-22
  没有 ⇒ 停在控制台侧，两边都没有 ⇒ 根本没走到那一行；r305 在 `LATE_PATCH` 必须先于 r304 反打。**驱动**三件：
  ① **节拍的饥饿自己计数**（`gBeatStarved[4]`，CloverSdhci.cpp:592，`held`/`young`/`panic`/`blind` 四支早退
  :3995/3997/4006/4020/4026 各 tally 一处），**累加不清零**、由下一次跑成的报告与每枚 beat 记录同时带出——
  被饿死的那个消费者不能是它自己唯一的报告者，这条是本轮动手前自己抓出来的仪器缺陷；② **两把钟同一枚记录**
  （`kernelSeconds()` :3729 用 `absolutetime_to_nanoseconds(mach_absolute_time())`，beat 的 `extra[0]`、marker 的
  `extra[8]` 各带一份，配 `gBootId` 在 `extra[12]`/`extra[12]`，:4046 与 :9326），用来把「驱动上膛毫秒」与「内核秒」
  在**同一点**对上，v0.7.14 收回的那把跨钟尺子由此换成实测的 `arming_offset`；③ **`writePioBlock` 的三条早退各自
  出声**（`pioFail()` :3237，`gPioAttempt` :3849 按调用点取 `100+event`/`21`/`22`/`20`），报告行尾巴
  `starve=%u/%u/%u/%u pio=%u/%x/%u@%u ch=%u`（:4980）。读者侧 `read-clover-log.py` 按新词表重排
  （`FLIGHT_BOOT_WORD={1:35,2:35,3:47,4:45,5:41,6:37}`、`FLIGHT_MAGIC_WORD={4:46,5:48}`）。三条负断言（不许清零
  饥饿计数、中转点必须仍是那些、普查路径不许任何睡眠/取锁调用）与解码器自测的正/负对照写进 `check-clover.py`。
* **boot 95（r305 / v0.7.15 的读回）：内核这把钟最后一行永远是普查的头一行，而两把钟第一次在同一枚记录里对上，
  于是「节拍被饿了一百二十秒」这句话第一次不需要跨钟推断。** 冷刷 `round305-v0.7.15-payload.img`（`c52f1f00…`，
  58,720,256 B；内核 `68ce7dc9`→AMFI `924b7243`→归一化 `82d8992b`→卡内 `ad5ba323`；ADT `e37eca9a` 14,384 B、
  trustcache `7c369ce0`、ramdisk `614b8356`），boot 94→95，落回 TWRP。① **那条「先中转再 printf」落在 §Round304 ⑤
  的（丙）支**：这一靴 kind-25 有 **0 行**（窗口里 23 行全是 boot 94 的残留），kind-22 里 `r300 fire`/`done`、`r262 th`、
  `r296`、`r298`、`r303` 也全 **0 行**；内核环最新行是 `[616.939083] r262 census t=616 pid=58 threads=24 shown=20
  hidden=4 wait=23 run=1`，后面只剩 `[616.939108] r270`、`[616.939148] r271 real=8 reg=8 start=7 run=8`、
  `[616.939185] r299 req=1 con=1 run=7 st=07777777` 三行就断了 ⇒ 「走到了头一行」有独立见证，停点被夹在
  **task.c:13595 与 task.c:13714 那 29 行之间**（三次自增 + 一发 `snprintf(fire)`）。**（丙）不当结论用**：这靴没有
  SIGKILL 现场（`killsrc`/`killustk`/`skst` 三族 0 行），中转唯一的调用者就是普查，所以「`probe_say` 对本靴一切都拒绝」
  这条替代解释没被排除，而尾巴报告行的印条件是 `wrote>0 || pend>0`——它自己的沉默由它自己的条件解释，不构成反证。
  ② **两钟同记录验证成功并立刻兑了一次**：`arming_offset` 四拍 `11861/11855/11841/11835 ms`（12 s 内 0.2 % 漂移），
  最后一枚跑成的 beat 在 **内核 497 s**，普查头行在 **616.9 s** ⇒ **同一枚钟上读出 120 s 的饥饿**（v0.7.14 说读不出的
  正是这一类数）；beat marker 与普查的**先后**仍**没断言**（落在外推误差之内）。③ **第三件仪器出声**：`pio=0/0x00000000/0w@0ms
  ch=0 total=0`（497 s 前没有任何一次块写被拒）＋ `starve=0/0/29267/31`——`held=31` 就是饥饿本身、`young=29267` 是 4 s
  门槛的正常拒绝、`miss=31` 是同帧正对照。④ sink/死线没动：两枚 marker 仍是 boot 94 与 boot 87 的、`magic=absent`、
  死线记录还是 boot 87，零条句子；`r283: last reset unnamed` ⇒ **第九起无名静默复位**，家族带 642.7/~645/661/746/835/
  555/742/**617**（第九票），`charger_temp=342`。⑤ **本轮自己造出来的假读数已被读者挡住**：boot 94 的陈旧 marker 走
  r305 的新词表解出过 `k=1129076301s arming_offset=1129075571250ms`，`read-clover-log.py` 因此加了 `r305_own()` 生成
  门槛（新字段必须同时说出哪一代镜像带着它），selftest 用 boot 95 真实形状加了正/负对照。
  **下一手 r306（两条，都不新开问题）**：① 让中转自己的沉默出声——`clover_emmc_probe_say` 每条早退各自计数，报告行的
  印条件加一支 `calls>0 而 wrote=0`；② 给「走到了」加一条**不经中转**的副本——`:13705` 三次自增之后、`snprintf(fire)`
  之前放一条裸 `printf` 的 step 行（要测的正是中转，副本不能骑在被测管道上）；另有 `drained=` 归属债与仍欠的
  smp-off 同镜像控制轮。详见 `notes/development-log.md` §Round305 读数。
* **r306（v0.7.16，把上一段预登记的两条一次付清：一次沉默的三种形状各拿一枚数）**。**内核**
  `patches/xnu-clover/zzz(63)-clover-r306.patch`（只改 `osfmk/kern/task.c`）：`clover_bb_chain_hops = 0;` 之后、
  `snprintf(fire)` 之前放一条**裸 printf** 的 step 行（`task.c:13718`：`clover r306: step t= seq= pid= th= hidden=`），
  `th=` 是真打出去的线程行数、`hidden=` 是 `call - cn`（打算打而没打成），所以「走没走到」与「走到后打了几个」被这一行
  自己分开。**两处刻意的不做**：不走 `clover_census_row`（本轮要测的正是中转，副本不能骑在被测管道上），不进 tee 豁免
  （它靠 `"clover r"` 通配过筛，走 printf→msgbuf→tee→内核环 kind-22，与尾巴那两族不同源，因此是真正的第二条腿）。
  **驱动**三件：`probeFlush` 那条合并门槛 `if (!gVerdictFirst || !gHost || gRingBusy[kRingDriver]) return 0;` 拆成
  命名分支 `gProbeBlocked[3]` = `unarmed`/`host`/`busy`（`:525`，**累计不清零**——两个读者，清零会决定另一个看到什么），
  并在**退出点本身**用 `teeSay` 出声（`:4980`，上限 `kProbeSilenceSayMax=4`：`teeSay` 只写驱动自己的 RAM 环，无锁、
  不睡、不碰卡，这正是它被允许站在一条「按定义做不了块写」的路径上的原因）；生产者侧两条早退也计数——空串/零长
  `gProbeEmptySay`（`:3598`）、超长截断 `gProbeCutSay`（`:3604`）；报告行的印条件加第三支 `nothing = calls>0 &&
  flushed==0`（`:5049`，上限 4），并把这五个数复制进块尾报告行末尾的 `blocked=%u/%u/%u empty=%u cut=%u`（`:5066`）。
  残留限制写进注释而不假装解掉：`unarmed` 那一支的沉默行自己也上不了卡，但计数不清零，下一靴（窗口上膛后）照样读得到。
  契约：tee 类 `{ "clover r306: ", "r306-step", 64 }`（`:1115`）+ 豁免 `teeHasPrefix (…, "r306")`（`:4656`），
  `check-clover.py` 钉住两条负断言（合并门槛必须已不在、`CloverSdhci r304: probe` 这个 token 必须已不在）并把 step 行的
  「不许走中转」检查**只看代码行**（注释里提一下函数名就会误报），`verify-clover-payload.py` 的 `probe-beat-flush`
  换成整条新格式串并新增 `probe-silent`/`census-step`。r306 在 `LATE_PATCH` 必须先于 r305 与 r304 反打。
  详见 `notes/development-log.md` §Round306。
* **boot 96（r306 / v0.7.16 的读回）：中转第一次被证明「这一靴一次都没被人叫过」，而那句沉默从 29 行夹到
  21 行、并且不再是地平线问题。** 冷刷 `round306-v0.7.16-payload.img`（`9a5e9bbf…`，58,720,256 B；内核
  `05762cb2`→AMFI `5d4e3afd`→归一化 `f1586403`→卡内 `32287fdc`），boot 95→96，落回 TWRP。① **新支 `nothing` 一靴说了 4 次**：
  `CloverSdhci r306: probe why=tail wrote=0 seen=0 pend=0 flushed=0 … starve=0/0/50/1 pio=0/0/0@0 ch=0
  blocked=0/0/0 empty=0 cut=0` ⇒ 三个门槛一次没走、生产者一次没被喂空串或超长、中转**一行都没进过**；boot 95 的
  三种形状里 (乙)「中转拒收」与 (丙)「消费者卡在门槛前」被这两枚数**同时排除**，只剩「没人往里放」。
  第二条腿独立成立：sink 自己读的 `drain enter boot=96 … tee=1 append_rc=4 probe_seen=0` 也是 0（一枚累计不清零的数、
  两个时刻、两个读者）。② **step 行 0 条，但这条缺席站得住**：卡上最新一行是 `[576.330721] r299`，而 step 在
  `task.c:13718`、`clover_census_row` 在 `:13728` ⇒ 停点夹在 **`:13703` 的 `}` 与 `:13728` 那次中转调用**之间（里面只有三次
  自增＋两条 printf）。它不是「印了没搬走」：卡上已有 `r299` ⇒ tee 游标已在它之后；sink 又跑了 `kPanicTeePasses=8` 趟
  `teeKernelLog()`（32 KiB 余量 vs 那一拍只写了约 1 KiB）；step 一族在 tee 豁免列里不受每趟 16 条上限；且 sink 之后
  的卡写被证明接受（`drain enter` 自己上了卡）。③ **sink 与普查仍在同一枚钟的同一秒**：marker `t=564223ms k=576s
  arming_offset=11777ms magic=yes rc=0` vs 普查头行 `[576.330590]`；`kernelSeconds()` 只有 1 s 分辨率（四拍 offset
  11991/11989/11987/11974 与 marker 的 11777 之差就是这枚截断），**先后仍不断言**。句子仍没落地（`append_rc=4`、
  零条 boot-96 的 kind-20）。④ **本轮仪器自己被抓到一处失明**：`nothing` 的上限是全局 4 次，被 `why=tail` 花光，
  于是**在现场那一趟跑的 `why=drain` 一条都没能说出口**——限量出声的报告，预算必须按「谁在说」分。⑤ 八核**第九靴、
  跨八枚镜像**复现（`real=8 reg=8 start=7 run=8`、`req=1 con=1 run=7 st=07777777`、`sent=278476 recv=278361`、头行
  `threads=16 shown=16 hidden=0 wait=16 run=0`）；死线记录仍是 boot 87 那枚 ⇒ 本靴没跑到 1800 s；`r283: last reset
  unnamed`＝**第十起无名静默复位**，带 642.7/~645/661/746/835/555/742/617/**576**，`charger_temp=337`。桌面主线**仍无读数**。
  **下一手 r307（三条）**：① `nothing` 预算按 `why` 分（`tail`/`beat`/`drain` 各一份）并带上 `calls=`/`probe_seen=`；
  ② 在 `:13707` 三次自增之后再放一条**走 kind-25 的 `clover r307: pre` 副本**——中转已被证明是没被人叫过的旁观者，
  现在被测的管道是内核日志/tee，所以这条副本合法，两支一比就能把「没印」与「印了没搬走」彻底切开；③ **控制轮正式跑**
  （同一枚 r307 镜像两次，第二次 `armcpu-smp=0`）。详见 `notes/development-log.md` §Round306 读数。
* **r307（v0.7.17，三笔账：一份报告预算买断了另一个说话的人 / 「走到了」的第二条腿 / 欠了十靴的控制轮）**。
  **驱动**：`gProbeSilenceSay`/`gProbeNothingSay`/`gProbeReports` 由单枚计数改成按 `why` 落位的数组
  （`tail`/`beat`/`drain` 各一份，`probeWhySlot()`），因为 boot 96 的四次 `nothing` 名额被 `why=tail` 全花完，
  而**在现场那一趟跑的 `why=drain` 一条都没能说出口**——限量出声的报告，预算归谁就报了谁的队首，不是板子；
  报告行与守卫早退行各带上 `calls=`（跑成的冲数），与本来就有的 `seen=`（累计封行数）配成一对，
  两条行的 token 一起升到 `CloverSdhci r307: probe`（第九层：新列必须说出哪代镜像带着它，否则旧镜像那行的
  缺列会被读成 0）。**内核**：`task.c:13706`（`clover_bb_chain_hops = 0;`）之后、r306 的 step `printf`（`:13738`）
  之前，把 `clover r307: pre t=/seq=/pid=/th=/hidden=` 直接交给 `clover_emmc_probe_say()`（`:13724`），**不发 printf**：
  r306 已把中转证明成「这一靴没被人叫过」的旁观者，被测的管道换成了内核日志/tee，所以这一条副本现在才合法。
  `pre`（kind-25）/`step`（kind-22）/`fire`（两渠道）三行字段一字不差，四支判读预登记在 §Round307 ⑥。
  **控制轮**：同一枚 r307 内核、只把 `armcpu-smp` 翻成 0 再打一次 payload（args 烤在镜像里，判据是 `payload.json`
  的 `kernel_args`），第一次在**同镜像**上切「尾巴属镜像 vs 属核数」——boot 87 是「单核＋别的镜像」，两变量混着。
  闸门：`check-clover ok`（含反向钉单枚计数与 `CloverSdhci r306: probe` 已不在盘上）、`verify-clover-payload` EXIT=0
  （新键 `census-pre`/`census-pre-relay-only`/`probe-report-generation`）、`read-clover-log --selftest` ok、
  `task.c` 语法 `exit=0 hard=0`、`CloverSdhci` `-Werror`＋对象审计过。产物链
  `b3037960`→AMFI `8c4a0cb8`→归一化 `cbf8f75a`（973/973 戳）→卡内 `a71a18fc`；镜像 `0aa8e893…` 58,720,256 B。
* **boot 97（v0.7.17 的第一靴）落在预登记的 (乙) 支，而且第一次不是「两种读法挤在一起」**：内核环覆盖
  **258.616 → 490.544 s**（seq 1090→1601），这 232 秒里普查只响一次（`[448.907920] r262: census t=448`），
  同拍紧跟 `r270`/`r271`/`r299` 三行，**然后环里下一行已经是 `[452.616469]`**；
  `clover r307: pre`（kind-25）与 `clover r306: step`（kind-22）**两条腿都 0 行**，`fire`/`done`/`r262: th` 同样
  0 行 ⇒ 走路没到 `task.c:13704` 那三次自增。**这一句现在有独立的腿**：`drain enter boot=97 … probe_seen=0`
  配上 drain 自己那趟的 `calls=139065 wrote=0 seen=0 blocked=0/0/0 empty=0 cut=0`，而生产者的代码里**没有**
  「未上膛就丢行」这一支（`clover_emmc_probe_say()` 只有空文本与超长截断两条早退，两条都记 0）⇒ `seen=0`
  只能读成**这一靴没人叫过它**，不是「叫了被拒」：**(甲)「tee 丢了 step 行」在这一靴被排除**，不再只是没读到。
  ⇒ 停点从 `:13595…:13738` 收窄到 **`task.c:13679 … :13707`**，而**这一段里没有一条能阻塞的语句**（`r299` 的
  printf 之后只剩一枚 `}`，三次自增不占时间，再往下是纯内存的交接）⇒ 能停在这里的不是锁、不是睡眠、
  不是卡片拒绝，而是**拿着这条走路的线程本身不往下走了**；本靴杀进程现场三族 0 行，所以「杀在普查那一拍上」
  既没证实也没排除——这是下一轮的题。r307 第①条当轮兑了：报告行 9 条 = `tail` 4／`beat` 4／**`drain` 1**
  （boot 96 是 4 全 tail、drain 0），**在现场那一趟跑的消费者第一次有了自己的声音**。sink 进了、一句话都没到卡上
  （`r302: blind ok=0`、首笔被拒块写 `pio=1/…@445924ms`、`starve=0/895/38329/54`）；第十一**起无名静默复位**，
  时机家族带 642.7/~645/661/746/835/555/742/617/576/**≥490**；八核跨九枚镜像逐字段复现（`real=8 reg=8 start=7
  run=8`、`sent=222806 recv=222710`）；**桌面主线那一格仍无读数**。
  **本轮自己抓到的一处假读数**：`drain exit` 那行的 `probe_pend=11` 打的是 `gRingPending[kRingDriver]`
  （整条驱动环），不是中转的 `seen-next`，同靴的 `pend=0` 把它否掉了——名字说一件事、字段带另一件事＝一枚会
  自己造读数——记为 r308 第①条：改名 `ring_pend=` 或补真值，行 token 一并升到本代。详见
  `notes/development-log.md` §Round307 读数。
* **boot 98（控制轮，同一枚归一化内核 `cbf8f75a`＋`armcpu-smp=0`，镜像 `2c1827b9…`）没答尾巴，它把「控制轮凭什么算跑过」
  这格还给了仪器**：本靴窗口里 `r262`/`r229`/`r270`/`r271`/`r299`/`r296`/`r298`/`r300`/`step`/`pre` **整族 0 行**
  （全窗口搜 `r262` 命中 0）⇒ 预登记的「尾巴到了/仍 0 行」两支都没有题面。**「不在环里」不许读成「没跑」**：
  环覆盖 256.569→524.995 s，而 tee 本靴最后报 `seen=3284 kept=779 throttled=56 lap=2`——每趟 16 条的上限
  会永久丢掉非豁免行（`r262/r229/r271` 不在豁免列），`lap=2` 是 msgbuf 写手实测反超读游标两次；boot 97 那句
  「走路没到 `:13704`」靠的是中转那条不经 tee 的腿，本靴那条腿也报 `seen=0`，可 `pre` 被放在三次自增**之后**，
  于是它说不出 burst 进没进。能站住的只有一条机制性候选：`r234: beat t=299/419 pid=0 rn=0`＝采样目标到
  内核 419 s 还没上闩（beat 线程只在 `clover_bb_target_pid != 0` 才调 `clover_bb_sample`，`task.c:11217`；
  闩锁来自批量里出现 `<pid>:backboardd`，`:14390`），而下一枚心跳要 539 s 才印、环只到 524.995 s ⇒
  419 s 之后读不出，**这一格不许当整靴事实**。本靴真正的新形状是**结束没有任何卡上记录**：四枚节拍
  `k=503/507/511/519 s`、`panicked=0`、`pio` 全 0（boot 97 是 895 与首笔拒绝）、marker 两枚仍属 boot 96/87
  ⇒ **一次都没进 sink**；监视器 05:37:31 刷完、05:55 见 TWRP（预上膛 BCB 接住的，落点≠死因）⇒ 终点只剩
  区间 **[525, ≈1044] s**，按第六层不许换算成复位时机的下一票。顺带：`retire#1..#4 done=presented=frames=1..4`
  与 `frame#5 pixels=45460` 几乎全黑——交帧在单核靴上同样跑得起来，桌面那一格仍无判据。
  **r308 的第②条因此改写**：把「burst 进了」与「几颗核」一起搬到 kind-25 的腿上（在 `clover_bb_sample()`
  进函数、走线程表**之前**投一枚 `r308: enter t= pid= target=` 并带核数摘要），控制轮再以那枚不经 tee 的行
  为成立判据重跑；本靴的正对照＝中转消费者跑了 **7257 趟**（boot 97 只到 902）而 `seen=` 仍 0、
  `blocked=0/0/0`、`empty=0`、`cut=0`，分预算后的 `tail`/`beat` 两支这次都在说话。
* **r308（v0.7.18，一笔账：裁判不能坐在被审的那条腿上）**。**内核**（`osfmk/kern/task.c`）三枚新行，
  全部先交 `clover_emmc_probe_say()`（kind-25，不经内核日志/msgbuf/tee）：
  ① `clover r308: beat t= target= ent= fired= same= cap= gone= notask= busy= reg= start= run=`——心跳线程
  自己每 120 s 一枚（`clover_r308_beat_row()`，挂在 `clover_ulock_dump(secs);` 之后），**这条是正对照**：
  它与 latch、与 `bb_fired>=96` 的 cap、与批量签名都无关，所以「relay `seen=0`」第一次能分清
  「通路死了」与「通路活着、没人走到」；交完中转再 `printf` 同一串字节，两条腿的差就是 tee 的账。
  ② `clover r308: smp-pre` 与 ③ `clover r308: smp-post`——只走中转、不 printf，一枚在 `r299` 那行 printf
  **之前**、一枚在它**返回之后**，字段 `t=/seq=/pid=/th=/hidden=` 与 `reg=/start=/run=` 直接复用 `r271`
  那行自己的局部变量（第十一层：列名与填它的表达式同源）。**为什么是这两枚**：boot 97 卡上有
  `r262`/`r270`/`r271`/`r299` 四枚 printf 行、relay 整靴 `seen=0`、`r306: step` 与 `r307: pre` 都不在，
  于是停点被夹在 `r299` 与那三次自增之间——那 28 行里只有一枚 `}` 和三次数；而 bytes 进 msgbuf 只证明
  `log_message` 被叫过，不证明 printf 返回。`smp-pre`/`smp-post` 之间**恰好只有那一句 printf**。
  计数同时搬到文件级（`clover_bb_enters/fired/same/capped/gone/notask/busy`）：`bb_fired`/`bb_same` 原本
  是采样函数自己的 `static`，读它们的行只能由它们产的行走出来。**驱动**：`drain exit` 那行的
  `probe_pend=` 打的其实是 `gRingPending[kRingDriver]`（tee 环积压），改名 `ring_pend=`，另补真
  `probe_pend= = gProbeSeq - gProbeNext`，token 升 `CloverSdhci r308: drain exit`；新增 tee 类
  `r308-relay`（64）并把 `r308` 放进豁免列，让 `beat` 行的第二腿不被每趟 16 条的上限吃掉。
  闸门：`check-clover ok`（新钉：三枚行各只出现一次、两枚括号行不许有 printf 副本、`clover_census_row`
  不在场、旧局部计数已删、位置链 `ctm[7]);`→`smp-pre`→`Round299`→`smp-post`→`clover_census_seq++`、
  `beat` 行排在 `clover_ulock_dump` 之后、双仓补丁逐字节相同、LATE_PATCH 里 r308 先于 r307 逆放，
  并反向钉 `CloverSdhci r212: drain exit` 已不在盘上）、`verify-clover-payload` EXIT=0
  （新键 `relay-bracket`/`relay-bracket-onceliteral`/`beat-accounting`/`exit-generation`）、
  `read-clover-log --selftest` ok、`task.c` 语法 `exit=0 hard=0`、`CloverSdhci` `-Werror`＋对象审计过。
  产物链内核 `185955ea`→AMFI `ff5c1972`（两刀编码与 r305–r307 逐字节同）→归一化 `7e669d95`（973/973 戳、
  `_clover_fastboot_reason` 在符号表）；镜像 `~/clover-out/round308-v0.7.18-payload.img`
  sha256 `ae4baa8a…`，58,720,256 B。**boot 99 的判读四支与控制轮的成立判据，读数之前写死在
  `notes/development-log.md` §Round308 ⑥⑦**。
* **这两条仪器债已由 r303 结清**（原文曾记作「本格欠着」）：`vbits=` 现以 r298 行末的 `vb_r=`/`vb_v=` 落地
  （`ipc_right_lookup_read()` 已经交出的 entry 位掩码，用来分 port set / send-once / 死名，不需要对象、锁纪律与
  translate 同一条），`cap=send` 行的 `qmc=/qsr=` 也带上了自己的返回码 `qerr=`。
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
* **多核已坐实（boot 88 把这句话证掉了旧的那格）**：内核 `CpuDataEntries` 从前只注册一颗核，副核从未被 start。
  boot 87 给的是「修之前」那一格：`clover r271: cpu … real=1 max=8 reg=1 start=0 run=1 s=00000000 tm=237841,0,0,0,0,0,0,0`
  ——**拓扑数到 8 颗（`max=8`），注册只有 1 颗（`reg=1`），缺的不是设备树也不是 `MAX_CPUS`**，
  是「谁去放副核」那一步的开关（`drivers/ArmCpu` 里那条被 `sheng-smp` 关着的 PSCI `CPU_ON` 路径）。
  boot 88 同一格是 `real=8 max=8 reg=8 start=7 run=8 s=01111111 r=11111111`，八颗都有 timer 计数、
  七颗各收到过上万笔 IPI（见上面 boot 88 那条）。
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
**十一连「无 panic 文案」复位（718/1184/1248/1335/997/707/1681/1712–1800/835–9xx/661–7xx/746–7xx s）已经判定它是独立的一道坎，不是主线症状**：
boot 89/90 两靴把「静默」这个词从它们自己身上摘掉了——两靴都进了 panic sink（`drain enter boot=89/90 …
append_rc=2`，而 `panicDrainRings()` 只由 sink 调用），只是那句 panic 文案在被写之前要读一次报告槽，
那笔读被「控制器被人占着」拒了（`host=0x20fdf`），于是**名字有、文案没送出**；所以从现在起这条坎的准确说法是
**「读不到为什么结束」**，而 r301 就是把那一份文案改成 RAM 副本、走已被证明能落地的 verdict 环。
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
| 八核 SMP | r299（v0.7.3）把 `ArmCpu` 里那条被 `sheng-smp` 关着的 PSCI `CPU_ON` 路径接进 `armcpu-*` 家族；boot 88 读回 `real=8 max=8 reg=8 start=7 run=8`、`rc=0`×7、`fl=0`，且 `clover r270: ipi` 从 `sent=0 recv=0` 变成 `sent=344711 recv=344557`（八颗各收过 1.3 万～7.3 万笔）。boot 89（同一枚镜像的控制轮）逐字段复现：`smp` 那行一字不差、`sent=296411 recv=296290`；boot 90（**换到 r300 那枚镜像**）再次复现 `real=8 … run=8`、`st=07777777 rc=0×7`、`sent=250500 recv=250415` ⇒ 可复现性跨镜像成立 |
| 显示栈首次出帧 | r202 attach + start，5 个进程各拿到一个 `IOMobileFramebufferUserClient`；r206 第一次 present：`swap#1/2/3 accepted`、`scanout#3 ok bytes=9216000`、`retire#3 done=3 presented=3`、`copy#3 93 ms` |
| 读数通道 | 两个环落在 **logdump**（p56）：kind-21 驱动判据 64 槽、kind-22 内核日志 512 槽，一次 `dd` 1216 扇区读回；panic 屏把 `fb=` 写进记录 |
| 无人值守 | `run-clover-unattended.sh`：冷刷 cache → 死线 → 预置 BCB 落 TWRP → 读回日志，退出码 0（本帧读数即由此而来） |
| 可复现内核 | 968 个 object 戳、`LC_UUID`、`_PrelinkKCID` 由文件内容派生，同树两次构建收敛到同一镜像 |

## 待完成

- 上表只有 **1** 挡住桌面；**6** 是 1 最可能的病根（尚未证，桌面仍 CPU-first）；**2–4** 挡住板级功能；
  **5** 是已定名的噪声。「读不到为什么结束」是**独立的一道坎**（见上表后的说明），不是主线症状——
  boot 89/90 之后它的措辞从「静默无 panic」改成「进了 panic sink、文案被卡拒」，因为 `drain enter
  boot=89/90 … append_rc=2` 只在 sink 里产生。
- **多核已到位并且可复现（boots 88–90，跨两枚镜像 / v0.7.3 与 v0.7.4）**：`clover r271: cpu … real=8 max=8 reg=8 start=7
  run=8 s=01111111`、`clover r299: smp … req=1 con=1 run=7 st=07777777 fl=00000000 rc=0×7
  entry=a3203000×7`、`clover r270: ipi … sent=344711 recv=344557`（boot 89：`sent=296411 recv=296290`，
  `smp` 那行一字不差；boot 90 换 r300 像：`sent=250500 recv=250415`，`smp` 那行仍一字不差）
  ——八颗核上线、固件每次都答应、而且都在收 IPI，三靴三次、两枚像。
  「一核跑 iPadOS 17 迟早要还」这一条不用还了。它**不挡**「进桌面」这句话：三靴同拍
  `r229: sbport … cnt=0` 与债主 `own=58:backboardd` 都不变，所以多核不进修复清单（这条反证靠的是队列与属主，
  不是 `run=`——单核像上 `run=` 本来取过 2…7）。**控制轮已经把它该答的答了，同时挪出一条新问题**：
  八核像的结局三靴分别落在 835–9xx、661–7xx 与 746–7xx s，而 v0.7.0–v0.7.2 那四靴要么跑到 1800 s 死线、
  要么在 1681 s 之后——预登记的「同带」/「回到晚带」两支都没落，落的是第三支（都早、且早得不同）。
  三靴两像只能说分布挪了位；**核数与镜像谁挪的仍没分开**（要一枚八核像关掉 `armcpu-smp`、
  或一枚没有 r299 行的像开八核，都得花一整轮，所以现在只登记）。
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
