# UniXNU-clover

**小米平板 4 8 英寸 LTE 版（2018）**的设备仓库——代号 `clover`，LTE 版本，
4 GB 内存，高通 **SDM660**（骁龙 660，Kryo 260：4× Cortex-A53 @1.8 GHz
+ 4× Kryo 385 Gold @2.2 GHz）。

它向 [UniXNU](https://github.com/UniXNU/UniXNU) 构建驱动描述这块板子：
目标 IPSW、内核配置、prelink 根、固件检出、ESP payload 和 Apple 格式设备树。
配套的固件仓库是 `uefi-clover`，同一块板子的 Mu-Silicium 移植。

`clover` 既是主线给这个家族的名字（`xiaomi,clover-lte`），也是小米给这个平台
的名字（`BoardPlatform: clover`），所以板级标识沿用它。

## 状态

| 层 | 状态 |
| --- | --- |
| 板级描述 | `board.toml` 可解析；`scripts/check-clover.py` 把它与 defconfig 和 payload 契约交叉核对 |
| ADT | `scripts/make-clover-adt.py` 为 8 个 Kryo 260 核生成 13,880 字节；检查器会把编码读回来，并断言 `ArmDTPlatform` 和 DarwinBoot 需要的节点 |
| 内核 | 已构建：`DEVELOPMENT_ARM64_BCM2837` 的 51.8 MB `kernel.macho` |
| payload / boot.img | 已构建：56 MiB Fat32 payload 卷，以及 kernel 段就是固件的 boot 镜像；payload **不**走 ramdisk（见下文）。`v0.2.13` 起 payload 里的 `kernel.macho` 是一份打了探针的副本：入口色带、panic 屏幕，以及 `arm_init` 某个调用返回后触发的定时复位；`v0.2.16` 起在开 MMU 前调用一个 mapper 洞穴，把 framebuffer 恒等映射进 TTBR0（与窗口同槽就复用 L2 表，否则用窗口最后一页新建一张），并预先填好 `_vinfo`；mapper 失败会涂一条白色诊断带；`v0.2.17` 改成用自身 PC 定位引导页表（`boot_args.physBase` 落在 `__PRELINK_TEXT` 上、本身没有被映射），并在 MMU 打开后立刻涂第 4 条青色带；`v0.2.18` 再加一条独立的橙色带（第 12 条），在 `arm_init` 的第一条调用处直接从 `boot_args` 涂，不依赖栈、AT 和 `_vinfo`；`v0.2.19` 修掉种子守卫写反的 bug（`cbz x8` 在 mapper 成功时跳过种子，`_vinfo` 因此一直是 0，黄带从 `v0.2.15` 起从未画出来过），并在第一条调用返回后再涂一条红带（第 13 条）；`v0.2.20` 修掉 ADT 里 `cluster-type` 的尺寸契约（字符串改为 4 字节整数 `'E'` / `'P'`）；`v0.2.21` 又因为这份内核只支持单簇而移除该属性，让 8 个核落在一个 `CLUSTER_TYPE_SMP` 平坦簇里；`v0.2.22` 关掉 `CONFIG_XNU_NATIVE_PAN` 并重建内核（Kryo 260 是 ARMv8.0，没有 PAN），这是本轮第一次改内核构建配置，探针脚本不变；`v0.2.23` 换回定时信号：`arm_vm_init` 返回后 10 秒复位，panic 屏幕在 framebuffer 不再翻译时 3 秒复位；`v0.2.24` 把复位点移到第二次 `PE_init_platform(TRUE)` 返回后 15 秒，这一段跨过 `serial_init`、`kernel_startup_initialize_upto(KPRINTF)` 和 `PE_create_console`，后者会重新映射 framebuffer，成功的话 panic 屏幕又能画字（见"菜单之后看什么"）；`v0.2.25` 修掉 `PE_create_console` 开头内联的 panic-log 探测（这份 BCM2837 内核读树莓派地址 `0x3c0fc000`，在 clover 上是外部异常），复位点不变；`v0.2.26` 把 `arm_init` 后段五个调用各接一个涂色洞穴（第 6–10 条带），复位点移到 `_machine_startup` 调用处，复位洞穴先涂第 11 条红带再等 15 秒；`v0.2.27` 把刻度带移到第 10–14 条、控制带移到第 15 条（都在 panic 屏不清的下半屏），并让 panic 屏印出 RA/FP/LR、fmt/args 和 va_list 前后各槽，用于从照片定位断言调用点；`v0.2.28` 修掉刻度洞穴的 4 字节对齐缺陷（第 14 条洞穴整体左移、`bl` 落到 `_fiq_context_init - 4`，`v0.2.27` 的白字是探针自己的误报），并在打洞时校验每条 `_start` 与每个 `bl` 目标；`v0.2.29` 把检查点从 `_machine_startup` 调用前推进到交接之后：第 15 带表示进入 `_machine_startup`，随后 9/8/7/6 带依次覆盖 `_machine_conf` 返回、`_kernel_bootstrap` 入口、首条 `printf` 返回和 `vm_mem_bootstrap` 调用点，复位也移到最后一档；`v0.2.30` 把第 10–14 条带从 `arm_init` 改标 `vm_mem_bootstrap` 里的五个调用（`vm_page_bootstrap`、`zone_bootstrap`、`vm_object_bootstrap`、`vm_map_init`、`kmem_init`），第 5 条红带表示 `kmem_init` 返回、随后 15 秒复位；`v0.2.31` 把已经证明返回的五个调用摘掉，第 5 条红带保留为前半段的收尾，第 10–14 条带改标后段（`kernel_startup_initialize_upto(KMEM)`、`vm_fault_init`、`kernel_startup_initialize_upto(ZALLOC)`、最后那条 `kprintf` 的进入与返回），返回后 15 秒复位；`v0.2.32` 在 `vm_mem_bootstrap` 返回后直接调用 `_vcattach`，第 15 条带从红改涂绿表示调用返回，控制台接上后屏幕交给内核文字，本轮不再定时复位；`v0.2.33` 在 ADT 的 `/chosen` 发布 `amfi-only-platform-code = <1>`（AMFI 启动的前置），并让控制台洞穴先调 `initialize_screen(NULL, kPETextMode)` 再调 `_vcattach`（单独 `vcattach` 在本机是空操作：`PE_create_console` 选了图形模式，`gc_graphics_boot` 为真）；`v0.2.34` 不再画带，改为在 `_bsd_init` 的九个无条件调用（`vfsinit`、自动配置前的初始化块、`kminit`、`IOKitBSDInit`、`loopattach`、`ether_family_init`、`net_init_run`、`vfs_mountroot`、`devfs_kernel_mount`）返回后各印一行 `[clover] … returned`，最后一行标记直接指出没返回的调用；`v0.2.35` 起 payload 多了两个文件，`Payloads/scratch-data.img`（签过名的静态 EL0 init，DarwinBoot 把它放进窗口并填 `/chosen/memory-map/RAMDisk`，内核当 `md0` 挂成 mockfs 根）和 `Payloads/kernel.macho.trustcache`（那颗 cdhash 的离线信任），`unixnu.toml` 里因此有 `initrd` 和 `rd=md0`，ADT 只有一个 `RAMDisk` 槽、不再有 `PackedRAMDisk`；`v0.2.36` 只换根镜像里的 init 程序（它自己 `open("/dev/console")`，退出码按「阶段 × 32 + errno」编码），`kernel.macho` 与探针逐字节不变 ；`v0.2.37` 把退出改成失败通道：四步全通就把 banner 连写 240 遍（两整屏）并停在 EL0，让活控制台当读数，只有失败才 `exit(阶段 × 32 + errno)` ；`v0.2.38` 花掉洪水、改问多进程：banner 一遍后 `fork`，子进程印自己的 pid/ppid 后 `exit(7)`，父进程 `wait4` 后印 reaped pid 与原始 status 再停住；`v0.2.39` 仍只换那颗根镜像：父进程 `fork` 一返回就印 `forked pid=`，子进程第一条指令先印 `alive`，把 round68「banner 之后什么都没有」的三义性切成三个独立读数；`v0.2.40` 仍只换那颗根镜像：按 Darwin arm64 的 fork ABI 用 `x1` 分流（孩子拿到 `x0=自己 pid, x1=1`），两边各印自己拿到的 `x0/x1`，`_fail` 从 `ret` 改成直接 `exit`，失败不再可能变成静默自旋；`v0.2.41` 仍只换这颗根镜像：只有 pid 1 才 fork，孩子印完自己的行就 `execve("/sbin/launchd", argv, envp)`（syscall 59，`syscalls.master` 抄的；`argv`/`envp` 运行时填进 `__DATA`，因为 `-static` PIE 的数据段放不了绝对地址），exec 成功就等于屏幕上出现第二份这个程序、且它的 `console fd=1`；`v0.2.42` 仍只换这颗根镜像：exec 出来的那份副本改发 mach trap（x16 = 表下标的相反数：`thread_self` -27、`task_self` -28、`host_self` -29、`mach_reply_port` -26、`mach_msg` -31；返回只写 x0、不碰 carry），三个 self 端口 + 一个 reply 端口 + 一条 24 字节消息自己发给自己再收回来，跑完 `exit(11)`；`v0.2.43` 仍只换这颗根镜像：mach 探针在 send 前后、receive 前后各加一次出声打印，并把消息缓冲区的地址也印出来，把 round72「`send=` 那行没出现」这一张图能做的两种解释切分开；`v0.2.44` 仍只换这颗根镜像：消息改走 `mach_msg2_trap`（x16 = -47，八个打包参数，发送腿带 `MACH64_SEND_MQ_CALL`），缓冲区指针从 x7 挪到 x20（x7 现在是 msg2 的 timeout 实参），末尾再用 legacy `mach_msg` 重放一次作为 boot-arg 对照；`v0.2.45` 仍只换这颗根镜像：孩子那一路一条指令不改，父亲把阻塞的 `wait4` 换成 `WNOHANG` 轮询、每轮烧几十毫秒计数并印一行，用来把「孩子挂在里面」「孩子已死但父亲被堵在悼词写」「tty 写路径自己死了」这三种共用一张照片的情况切开；`v0.2.46` 仍只换这颗根镜像，且孩子那一路一条指令不改：父亲把轮询期间的打印**全部拿掉**（一个字符都不写），200 轮之后用 `exit(384)` 报超时，因为 panic 屏是直写 framebuffer 的、不经过 `vcputc`，是这台板子上唯一不受被测控制台影响的通道 |
| 固件 | 由 `uefi-clover` 构建（3 MiB 固件卷，内含 UniXNU 与 DarwinBoot）；`v0.3.10` 起 DXE 尾部完整派发，`CloverGopDxe` 发布 GOP |
| 设备事实 | 2026-10-22 采集过一次，位于 `facts/81ffcaaa-20261022-210928/` |
| 真机启动 | **菜单已到**（Stage 1）：`v0.3.10` 固件在屏幕上渲染 UniXNU 菜单并读到了 payload；菜单 3 秒后默认进 XNU。`v0.2.9` 实测屏幕出现**绿 + 蓝 + 品红**三条色带，即 DarwinBoot 已交接、XNU 入口已执行、引导页表已建好；`v0.2.10` 实测**品红出现约 6 秒后设备自己重启**，即 `sctlr_el1` 打开 MMU 后没有立刻死掉；`v0.2.11` 的 `_arm_vm_init` 复位点没有触发（**品红一直停着**），说明还没走到它；`v0.2.12` 实测**只有绿 + 蓝 + 品红、约 6 秒后自复位**，即 `_PE_init_platform` 返回了、开 MMU 后写屏确实不可用；`v0.2.13` 的 `_thread_bootstrap` 复位点实测是**品红后十几秒才复位、没有白字**，符合"panic 屏幕在 `vinfo` 仍为 0 时走了 12 秒兜底"；`v0.2.14` 扩映射并预填 `_vinfo`、不再主动复位，实测**仍是绿 + 蓝 + 品红，无黄带无白字**，由此定位到窗口在 framebuffer 之上、向上扩 `memSize` 永远够不到；`v0.2.15` 改为直接补页表项，实测**仍是绿 + 蓝 + 品红、无黄无白**，原因是 1 GiB 窗口只能落在高 DRAM bank，与 framebuffer 不在同一个 1 GB 槽，旧代码查错了槽位就退出；`v0.2.16` 换成独立 mapper 洞穴（同槽复用、异槽新建 L2 表），失败时用白色带自报错误码，实测**绿 + 蓝 + 品红 + 第 9 条白带**，即 mapper 走到自检第 4 步：窗口的第一个 L2 项不是 2 MB 块；由此查明 `boot_args.physBase` 锚在 `__PRELINK_TEXT` 上、本身不在引导映射内；`v0.2.17` 改为从 mapper 自己的 PC 反推引导映射，修掉复用 L2 表时未掩掉低位的 bug，并在 MMU 打开后立刻涂第 4 条青色带，实测**绿 + 蓝 + 品红 + 青、无黄**，即 MMU 已开、framebuffer 已经可写，卡点在 `_cpu_data_init` 被调用之前；`v0.2.18` 增加第 12 条橙色带（`arm_init` 入口后的第一条调用，不依赖 vinfo/栈/AT），实测**绿 + 蓝 + 品红 + 青 + 橙、无黄**，确认 `arm_init` 已进入、第一条调用点已到达；由此回查代码发现种子守卫从 `v0.2.15` 起就是反的（`cbz x8` 在 mapper 成功时跳过种子），`_vinfo` 一直为 0，黄带从未有机会出现；`v0.2.19` 改成 `cbnz x8`，并让第一条调用返回后再涂第 13 条红带，实测读到**白字 panic 屏幕**：`CPU property 'cluster-type' has bad size 6 @machine_routines.c:936`，即种子/画屏/早段 `arm_init` 全通，内核已走到 `ml_parse_cpu_topology`；`v0.2.20` 把 `cluster-type` 从字符串改成 4 字节整数 `'E'` / `'P'`，实测接着触发 `machine_routines.c:1099` 的 `phys_cluster_id <= MAX_CPU_CLUSTER_PHY_ID` 断言——这份 BCM2837 内核只支持单簇，无法接受 E/P 双簇描述；`v0.2.21` 改为不发布 `cluster-type`，让 8 核按 `CLUSTER_TYPE_SMP` 建一个平坦簇，实测拓扑已通过，接着读到 `Undefined kernel instruction: pc=0xfffffff0079aff58 instr=d500419f`——`msr pan, #1`，Kryo 260 是 ARMv8.0，没有 PAN；`v0.2.22` 把 `CONFIG_XNU_NATIVE_PAN` 改成 `n` 并重建内核，实测七条带全部出现（绿 + 蓝 + 品红 + 黄 + 青 + 橙 + 红）、没有白字：内核进了 `arm_init`，两个早段探针都返回了，之后死在 `bl _arm_vm_init` 处或更晚，而且没有可见的 panic；`v0.2.23` 因此把信号换回定时复位，并让 panic 屏幕在 framebuffer 不可翻译时也能报出来，实测**色带出现后约 10 秒自复位**，即 `arm_vm_init` 返回了，卡点在它后面；`v0.2.24` 把复位点移到 `PE_create_console` 之后的第二次 `PE_init_platform(TRUE)`，实测**色带之后什么都没有**——既没有文字，也没有 15 秒复位，卡点在 `arm_vm_init` 返回之后、`PE_init_platform` 之前；`v0.2.25` 据此跳过 `PE_create_console` 开头读树莓派 panic log 地址的四条指令，实测**色带 + 约 15 秒复位、无文字**，即控制台与第二次 `PE_init_platform` 都已返回（无文字是 `gc_enabled` 仍为假所致，不是故障）；`v0.2.26` 给随后五个调用各涂一条带（6–10）并把复位点推到 `_machine_startup` 之前；实测**绿 + 蓝 + 白字**（`<1>:0 Assertion failed: <d>`、`TCR=0x…1a251a`、`pages=free=0`），即 `arm_init` 后段有断言 panic、发生在 VM 页初始化之前，但旧清屏把第 2 条以下的带全擦掉、参数又与已知冷段常量对不上；`v0.2.27` 因此改印 RA/FP/LR 与原始参数槽；其实机读数（绿 + 蓝 + 白字、`LR=0xfffffff0073f1dc4`、`a0=1 a1=0 a2=0xd`）指向的不是内核断言，而是第 14 条刻度洞穴自己：它被放在 `0x3ed784`（4 mod 8），`ld.lld` 把 `_start` 对齐到 `0x3ed788`，而 patcher 仍按 `0x3ed784` 写入，整段 `bl` 左移 4 字节落到 `_fiq_context_init - 4`（`_platform_syscall` 冷段里的 `bl _Assert`），`x0/x1/x2` 还留着 `arm_init` 传给 `_fiq_context_init` 的 `TRUE/0/0xd`；`v0.2.28` 修掉打洞对齐并逐条校验 `bl` 目标，第 10–13 条带则证明那四个调用确实都返回了；`v0.2.28` 上机读数是**10–14 全在 + 15 红 + 约 15 秒复位**，即 `_fiq_context_init` 也返回了、`arm_init` 已到 `_machine_startup` 调用点，交接之前没有任何卡死；`v0.2.29` 因此让检查点真正进入 `_machine_startup`，把刻度推进到 `kernel_bootstrap` 的 `vm_mem_bootstrap` 调用处；`v0.2.29` 上机读数是**整条梯子全亮 + 约 15 秒复位**（逐带取中位色：10 白、11 橙、12 绿、13 青、14 品红、15 红、6 白），即 `_machine_conf`、首条 `printf` 都返回了，`vm_mem_bootstrap` 已被调用，卡点在它内部；`v0.2.30` 因此把第 10–14 条带改成 `vm_mem_bootstrap` 内五个调用的探针、复位后移到 `kmem_init` 之后；`v0.2.30` 上机读数是**十六条带全在、含第 5 条红带、约 15 秒复位**，即 `vm_page_bootstrap` 到 `kmem_init` 五个调用全部返回、复位档也到达，卡点在 `vm_mem_bootstrap` 后段；`v0.2.31` 因此摘掉这五个已证调用，第 5 条红带保留为前半段收尾，第 10–14 条带改标后段四个调用；`v0.2.31` 上机读数是**13、14 都在 + 约 15 秒复位**，即最后那条 `kprintf` 返回、整个 `vm_mem_bootstrap` 返回，卡点在 `kernel_bootstrap` 更后面；`v0.2.32` 因此不再加刻度，而是在 `vm_mem_bootstrap` 返回后调用 `_vcattach` 接上视频控制台；`v0.2.32` 上机读数是一块**白字 panic**：`"AMFI: no amfi-only-platform-code device tree node!" @AppleMobileFileIntegrity.cpp:5025`，RA/LR 都落在预链接的 AMFI kext 里，说明内核根本没停在 `vm_mem_bootstrap` 之后，而是一路走到 AMFI；色带还在是因为 `vcattach` 被 `gc_graphics_boot == TRUE` 短路成了空操作；`v0.2.33` 因此补上 ADT 的 `amfi-only-platform-code` 属性，并先用 `kPETextMode` 清掉图形模式标志再 attach；`v0.2.33` 上机读数：**文字控制台已接上**，满屏内核日志（AMFI、Sandbox、apfs、AppleImage4 都走过），最后停在 NVRAM sync 失败那一行、**没有 panic**；但那一行可能来自后台 kext 线程，不能当作启动线程的卡点；`v0.2.34` 因此在 `_bsd_init` 的九个调用返回后各印一行 `[clover]` 标记；`v0.2.34` 上机读数是**九行标记全部印到 `[clover] net_init_run returned`，`vfs_mountroot returned` 没有**，最后一行是 `Waiting on <dict … IOProviderClass IOMedia … IOPropertyMatch … UUID …>`，即启动线程正常停在 `IOFindBSDRoot`，等的根 `IOMedia` 在这块没有驱动的 eMMC 板上永远不会被发布——不是卡死，是这条路不通；`v0.2.35` 因此把根换成 `rd=md0` + mockfs，并让 init 自己 `exit(0)`，用 `initproc exited` 这条 panic 证明 EL0 跑到了；`v0.2.35` 上机读数是 **`initproc failed to start -- no exit reason available -- (signal 0, exit status 0)`、没有 `CS_KILLED`**，也就是 pid 1 已经建好、已经跑进 EL0、四个 syscall 都返回、最后自己 `exit(0)`——Stage 6 达成（exec 真失败会报 `Process 1 exec of … failed, errno N`）。缺的那行 banner 不是 bug：这份 XNU 没有 `open_console()`，initproc 的 fd 0/1/2 是空的，`v0.2.36` 因此让 init 自己 `open("/dev/console")`；round66 实机读数 **`exit status 0`** = `getpid`/`sysctl`/`open`/`write` 四步全通，EL0 到控制台 tty 的 syscall 通道闭环，banner 没进照片是因为 panic 钩子清屏且 tty 输出不进 msgbuf；`v0.2.37` 因此成功不退出：banner 写 240 遍后停住，活控制台当读数；round67 实机读数**整屏 `[clover] el0 init: pid=1 console fd=0 pagesize=4096`、停住不复位** = tty 的字画上了面板，EL0→屏幕闭环（`fd=0` 证明描述符表空、`pagesize=4096` 证明用户栈可写）；`v0.2.38` 因此改问多进程：fork/子进程 exit(7)/父进程 wait4；round68 实机读数是**banner 之后什么都没有**——没有 `child:`、没有 `waited`、无 panic 无复位，而同屏的 `Added memory device md0`/`BSD root: md0` 与 `dli_init … scheduled at least once` 说明根挂上了、内核线程调度是好的，这一张图却有三种解释（fork 没返回 / 子进程没被调度 / 子进程第一次写没落地），`v0.2.39` 因此把 fork 点切成三个标记：父进程一返回就印 `forked pid=`、子进程第一条指令就印 `alive`；round69 实机读数是**两行一模一样的 `forked pid=2`**、没有 `alive`、没有 `waited`，也就是两个进程都跑了、都走了父分支——根因是 Darwin arm64 的 fork ABI：`thread_set_child`（`osfmk/arm64/status.c`）给孩子的是 `x0=自己的 pid, x1=1`，父亲是 `x0=同一个 pid, x1=0`（`kern_fork.c:576` 的 `retval[1] = 0`），把孩子的 x0 归零是 libSystem 的事，手写 `svc` 没有；而孩子随后 `wait4(-1)` 拿到 ECHILD、掉进 `_fail`，`_fail` 是 `b.cs` 进来的、结尾 `ret` 回到陈旧 x30，于是原地自旋一个字都不写——round68 的「只有 banner」是同一个错误的另一张照片。`v0.2.40` 因此按 `x1` 分流、两边都印出自己拿到的 `x0/x1`、并把 `_fail` 改成直接退出（失败必须出声）；round70 实机读数**五行齐**：`forked pid=2 x1=0`、`child: alive`、`child: pid=2 ppid=1 x0=2 x1=1`、`waited pid=2 status=1792`，即 fork 复制 pmap、次核调度孩子、孩子 CoW 栈可写、`exit(7)`、父亲 `wait4` 被唤醒收尸，整条多进程链在这块硅上闭合（`1792 = 7 << 8`）；`v0.2.41` 因此把孩子的 `exit(7)` 换成 `execve`，问下一面墙——一个进程能不能把自己换成另一个程序；round71 实机读到六行、末行 `[clover] el0 init: pid=2 console fd=1 pagesize=4096`，即 exec 成功、fd 0 跨 exec 保留（新副本 open 拿到 1）、父亲再无 `waited` 行——namei、mockfs 读、第二次 AMFI、换地址空间整条 exec 路径已通（v0.2.42 问 mach）；round72 实机读到 `[clover] el0 mach: thread=259 task=515 host=771`、`[clover] el0 mach: reply=1827`——x16 的负数约定是对的，四条 trap 都分发成功，而且这个 fork+exec 出来的 task **IPC space 是活的**（端口名全是内核真分配的），`mach_reply_port` 还证明了端口能分配并抄回用户页；但 `send=` 行的字面量都没出现，父亲读到 `status=9` = **SIGKILL**，死在消息那一趟，同屏多出 `Failed to send exception EXC_CORPSE_NOTIFY. error code: 5 for pid 2`（`task.c:2461`：这份内核没设 exception 端口，所以每个 EL0 进程死时都会印这行，它是死亡的另一条通道而非死因）。源码侧已排除三件事：`syscall_sw.c:137` 的 31 号确实是 `mach_msg_trap`（不是 `kern_invalid`，而 `goto bad` 的结尾是 panic，内核没 panic）、`arm_get_mach_syscall_args` 对 64 位 state 原样 `bcopy` 寄存器（`munge_wwwwwww` 只在 32 位分支用，指针没被截断）、这份副本写 `__DATA` 是成功的。剩下的分歧我读不出来，因为探针在 send 与 receive 之间一次都没打印——`v0.2.43` 因此把这两次 `svc` 各包上「调用前标记 + 调用后立即打印」，并顺手印出消息缓冲区的地址，一次刷机把「死在哪一次调用」切成独立读数；round73 实机读数是 `[clover] el0 mach: msg=4254904160`、`[clover] el0 mach: presend`，然后**还是没有 `send=`**，父亲又读到 `status=9` —— boot-arg 没能救活 legacy `mach_msg`。而这一次不能再拿「策略没编进去」当解释：`objdump out/clover/kernel/kernel.macho` 里 `_ipc_policy_allow_legacy_send_trap+0x20` 确实是先测 `MACH64_POLICY_HARDENED`（bit 55）、再 `ldrb` 读 `_allow_legacy_mach_msg`（`0xfffffff007158000+0x1a8`）、`tbnz` 为真就直接 `mov w0, #0` 返回放行，说明 DEVELOPMENT 分支是活代码；payload 里 `unixnu.toml` 的 args 末尾确实是 `allow_legacy_mach_msg=1`，而 `PE_boot_args()`（`pexpert/arm/pe_bootargs.c:11`）返回的就是 DarwinBoot 填的那块 `CommandLine`（`PE_init_platform` 直接取 booter 递进来的指针，ARM 上不看设备树的 `chosen/boot-args`），`rd=md0` 变成 `BSD root: md0` 已经证明这条通道；解析器对尾 token 也走得通（键名扫描停在 `=`、`getval` 吃掉 `=` 返回 NUM、`argnumcpy` 写 1 字节、`kernel_startup_tunable_init` 归一化成 bool）。**四条都成立，进程照旧被杀**，所以 round72/73 的死因目前只能记为未定，不能继续宣称是 legacy 闸；`v0.2.44` 因此不再和这一个调用纠缠，改问 Apple 真正推荐的 `mach_msg2_trap`（表位 47，八个打包参数），它的发送腿根本不进 `ipc_policy_allow_legacy_send_trap`（唯一调用点 `mach_msg.c:611` 只被 `mach_msg_overwrite_trap` 走到），并把 legacy 调用挪到整轮最后重放一次，让「boot-arg 是死是活」变成一个独立的、不会污染主问题的读数；round74 实机读数是**九个 el0 行到 `[clover] el0 mach: msg=` 就断在半行**——光标停在 `=` 后面、地址数字没出来、`m2: presend` 没有、corpse notify 没有、父亲的 `waited` 也没有，屏幕之后一直不变；两行「死亡通知」同时缺席意味着 pid 2 **根本没有退出**，它要么卡在携带数字的那次 `write` 里面、要么还在跑，而一张静态照片分不开这两者，所以 round74 没答上它自己提的问题（msg2 的 `svc` 一次都没执行到，三道策略闸一条都没测到）；objdump 已核对 v0.2.44 的机器码与源码逐条一致、同一套 `_print_dec` 在同一屏上刚印完 259/515/771/1827，因此不是探针写坏；`v0.2.45` 因此把问题换成「停在半行里的是进程还是 tty」，只改父亲：`wait4(child, WNOHANG)` 轮询 + 前 10 轮各印一行 `parent: poll <n>`（之后继续轮询但闭嘴），轮间用 `subs/b.ne` 计数循环烧时间——因为这份内核里 `__semwait_signal(0,0,1,1,…)` 不是延时（`port_name_to_semaphore` 对非法端口名直接返回 `KERN_INVALID_NAME`，`kern_sig.c:1090` 于是回 EINVAL），而 `select`/`mach_wait_until` 分别要结构体 copyin 和一个本表读不到的绝对期限。**round75 实机读数**：面板停在 `[clover] parent: poll ` 半行、光标在空格后、数字没出来，孩子的第一条 `[clover] el0 child: alive`（自带换行的单次 write）一个字都没有，且**没有 panic 屏**——write 若返回 errno 就会走 `_fail`→`exit()`→panic，所以父亲是没从 write 里回来。这与 round74 停在 `el0 mach: msg=` 是同一个形状（标签成功、紧随的那次 write 没回来），而两轮之间唯一的变化是父亲成了控制台的**第二个写者**：EL0 每个字符都落在 `vcputc_options`（`video_console.c:1231`）里那把 `lck_ticket_t`（`:153`）上，排队自旋、无超时，而同一函数在 `gc_enabled` 为假时还会静默丢字符——照片分不开这两种。⇒ **mach 的嫌疑被暂时搁置，round72-74 关于 mach 的读数全部标记为受污染**。round76 因此只做一个变量：只有一个写者 |

上面这张表之外，不做任何额外声称。

## 这里用到的硬件事实

大部分常量来自公开的设备树，在 `feeds.toml` 里钉住版本，`unixnu fetch`
会保留一份副本；平板自己能回答的那些来自 `facts/<serial>-<stamp>/` 下的
dump，"来源"一列写明了哪个是哪个：

| 项目 | 值 | 来源 |
| --- | --- | --- |
| SoC / msm-id | SDM660，`qcom,msm-id = <317 0x0>`（`0x13d`） | 下游 `sdm660.dtsi` |
| 下游 board-id | MTP 基座上的 `qcom,board-id = <11 0>` | `sdm660-mtp-clover.dts` |
| 核心 | Silver `0x0-0x3`（A53，1804800 kHz），Gold `0x100-0x103`（2208000 kHz） | `sdm660.dtsi` → `sdm630.dtsi` |
| 时基 | 19.2 MHz | 两套树一致 |
| DRAM | base `0x80000000` + `0x80000000`，第二个 bank `0x100000000` + `0x7e4c0000` | `dt/memory.raw.txt`、`proc/meminfo.txt`（MemTotal 3825460 kB） |
| GIC | distributor `0x17a00000`/`0x10000`，redistributor `0x17b00000`/`0x100000`，stride `0x20000` | 下游 `sdm660.dtsi` |
| Splash / framebuffer | `0x9d400000` + `0x02400000`，即固件自己内存图里的 "Display Reserved" 行。`clover-overlay.dtsi` 把 `cont_splash_mem` 挪到 `0x9d500000`，那是给"内核自己重画 splash"准备的；真正说话的是这块板子启动的固件，基座设备树与它一致 | `fw/xbl-uefiplat.txt` |
| 屏幕 | 1200×1920，`boe,tv080wum-nx2` / `novatek,nt51021`，107×172 mm，即 8 英寸 | 主线 + 下游 |
| 触摸 | 主线用 `edt,edt-ft5406` @ `blsp_i2c3` 0x38；下游用 focaltech `fts` | `sdm660-xiaomi-clover-regular-common.dtsi` |
| 背光 | `pm660l_wled`，是 LED 驱动而不是 PWM | 同上 |
| 存储 | eMMC `sdhc_1: sdhci@c0c4000`（hc `0xc0c4000`/`0x1000`，cqhci `0xc0c5000`/`0x1000`，ICE `0xc0c8000`/`0x8000`），CQE + HS400ES，8-bit | 下游 `sdm660.dtsi` |
| 保留区 | adsp `0x92a00000`、mba `0x94900000`、cdsp `0x94b00000`、venus `0x9cc00000`、ramoops `0x9fe00000`、gpu zap `0xf0b00000` | 下游设备树 |
| 串口控制台 | `blsp1_uart2`，115200，声明为 `stdout-path` | 两套树一致 |
| USB | `dr_mode = "peripheral"`——只有 device 模式 | 两套树一致 |
| 固件参照 | Mu-Silicium 的 `lavender`（Redmi Note 7）是同一颗 SoC：DDR `0x80000000`、FD `0x9FC00000`/`0x300000`、header v1、`append_dtb`、psci `smc` | `Resources/Configs/lavender.toml` |

目标系统标识是原封不动的 **iPad7,1 / iPadOS 17.7.11 / 21H461** 镜像
（`j120ap`，`ApChipID 0x8011`，`ApBoardID 12`）。Kryo 260 是 ARMv8.0，
没有指针认证，所以只有这一档 4 KiB 页的镜像能跑，内核按
`DEVELOPMENT_ARM64_BCM2837` 构建。

## 目录

```
board.toml                    板级描述
configs/clover_defconfig      内核配置
feeds.toml                    每个常量所依据的两棵设备树
drivers/                      （空）板级 XNU 驱动，等有了再放
kits/                         （空）板级内核库
patches/                      （空）板级补丁
package/                      （空）本板的 UniXNU kit 内容
scripts/                      ADT 生成器、预检、事实采集、归档构建、md0/mockfs 根镜像
notes/                        带来源清单的调研笔记
facts/                        按机器存放的 dump，Git 忽略
```

## 构建

内核这一步只能在 macOS 上做；固件这一步在 Linux 上构建。动手之前宿主机还差
两件事：`sudo xcodebuild -license accept`（这台机器的 Xcode 27 已安装但未接受
许可，未接受前 `git` 和 Homebrew 也会跟着坏）以及给 `./unixnu` 本身准备的
Python 3.11+。`./unixnu` 的 shebang 是 `env python3`，而 macOS 自带的
`/usr/bin/python3` 是 3.9、没有 `tomllib`，Homebrew 的 3.13 又叫
`python3.13`，所以要给 PATH 准备一个不带版本号的 `python3`：

```sh
mkdir -p ~/UniXNU-clover/build/shim
ln -sf /opt/homebrew/bin/python3.13 ~/UniXNU-clover/build/shim/python3
```

下面所有 `./unixnu` 都以 `PATH=$HOME/UniXNU-clover/build/shim:$PATH` 开头。
`scripts/check-clover.py` 没有这个要求：它用 `tomllib`、`tomli`
或自带的子集解析器读 board.toml，所以在系统 Python 3.9 上四组检查都能跑。

`--uefi` 指向的不是 `uefi-clover`，而是**应用了它的 overlay 的那份 Mu-Silicium
检出**（本机 `../mu-clover`）：payload 的 panic 屏幕要复用固件的 8×16 点阵，
`patch-xnu-entry-probe.py` 因此去 `<uefi>/Silicon/Silicium/SiliciumPkg/Library/
FrameBufferSerialPortLib/Font.h` 取字模，而 `uefi-clover` 只有板级的那几个
目录。

```sh
git clone --recurse-submodules https://github.com/UniXNU/UniXNU
git clone <this repo> UniXNU-clover

cd UniXNU
./unixnu defconfig clover_defconfig -D ../UniXNU-clover
./unixnu fetch
./unixnu tree
python3 ../UniXNU-clover/scripts/check-clover.py      # 预检，任何时候都可以跑
./unixnu build firmware --uefi ../mu-clover            # 应用了 overlay 的固件检出
SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" ./unixnu build kernel   # macOS + Xcode
./unixnu build package
cd ../UniXNU-clover && python3 ../UniXNU-clover/scripts/make-clover-root.py   # md0/mockfs 根 + trust cache
cd ../UniXNU
./unixnu build payload --unique-chip-id 0x<device seed> --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-root/trustcache.bundle
./unixnu build boot
```

Xcode 27 的 clang 在设备仓重建归档时不带 `-isysroot`，会拿 macOS SDK 去编译 iOS
目标，并把 `-Wincompatible-sysroot` 当成错误；上面显式指定 iPhoneOS SDK 就是为了
绕开它。只重打 payload 不需要这一步。

`scripts/check-clover.py` 就是 `[firmware] preflight`。它把 ADT 生成到临时文件，
再把 Apple 的节点编码解回来，断言 `/chosen`（64 字节 `random-seed`、CHRP
NVRAM proxy、`dram-base`、`crypto-hash-method`、`root-matching`、`display-base`、
4 字节的 `amfi-only-platform-code`）、`/chosen/memory-map` 里恰好只有 `RAMDisk`
和 `TrustCache` 两个占位（出现 `PackedRAMDisk` 就报错：DarwinBoot 不接受两个
ramdisk 槽）、
带八个 MPIDR 值的 `/cpus`、`/psci`、带 `ranges` 与 GICv3 节点的 `/arm-io`、
`/defaults`、`/options` 和 `/product`。board 一段还钉住 `v0.2.35` 的根契约：
`packed_data_ramdisk` 必须为假，`kernel_args` 必须有 `rd=md0`，
`trust_cache_sha256` 必须非空。

## dump 回答了什么

`scripts/dump-clover-facts.sh` 采集只有这台机器才能回答的东西，写到
`facts/<serial>-<stamp>/`：

```sh
adb shell cp /tmp/dump-clover-facts.sh /data/local/tmp/ && adb shell sh /data/local/tmp/dump-clover-facts.sh
```

dump 已跑过一次（2026-10-22），`board.toml` 里有三个占位现在由设备本身回答：

- `[userdata] first_lba` / `last_lba` —— `0xf10520` / `0x747bfde`，mmcblk0p64，
  按 512 字节扇区。没有它们，任何镜像都写不到 manifest 之后。
- DRAM 分 bank —— `0x80000000+0x80000000` 和 `0x100000000+0x7e4c0000`；
  `dram_size` 只声明低 bank，因为 `/chosen` 只带一组 base/size。
- `qcom,board-id` —— merged dtb 声明的是 `<0x0001000b 0x0>`，
  `qcom,msm-id` 确实是 `<317 0>`。

固件自己的 `uefiplat.cfg`（同一份 dump，`fw/xbl-uefiplat.txt`）随后在 splash
地址上推翻了设备树：是 `0x9d400000`，不是 overlay 的 `0x9d500000`；
`notes/clover-device-tree-sources.md` 第 5 节保留了推理过程。

boot 镜像参数也不再是猜的。dump 把整个 `boot` 和 `recovery` 分区都拉了下来
（`img/unixnu-boot.img`、`img/unixnu-recovery.img`——`unixnu-` 前缀是脚本的
命名，字节是 stock 小米的），它们的头在 `[bootimg.image]` 需要的每个字段上
都互相一致：

| 字段 | 值 | 如何得知，而不只是读到 |
| --- | --- | --- |
| header version | `0` | 两个镜像在 0x28 槽位都是零 |
| page size | `0x1000` | 由算术独立证明：ramdisk 恰好从 `0x1000 + align(kernel_size, 0x1000)` 开始 |
| kernel offset | `0x8000` | 相对 base；`0x80000000` 不进头部，这就是 `base = 0` 的原因 |
| ramdisk offset | `0x1000000` | 同上 |
| tags offset | `0x100` | 同上 |
| dtb | 追加 | kernel 段是 `Image.gz` 加紧跟 gzip 流之后的串联 DTB：`boot` 里 111 个，`recovery` 里 1 个 |

两个镜像都没有 AVB 尾，`id` 全零，所以这个变体的 boot 镜像自身不带签名。
`[bootimg.image]` 里原来那些 gta4l 占位值（`ramdisk_offset 0x20000000`、
`tags_offset 0x01e00000`、`header_version 1`）对 clover 是错的，固件仓库的
检查器现在钉住真实数值。

## payload 放在哪

payload 卷不是 boot 镜像里的 ramdisk。这块板子构建出的固件卷包含 `Fat`、
`PartitionDxe` 和 `DiskIoDxe`，但没有任何 ramdisk 文件系统驱动，所以可能出现
的 `SimpleFileSystem` 实例只能挂在 `SdccDxe` 的 eMMC 上。UniXNU 靠扫描每个
`SimpleFileSystem` 找 `\EFI\UniXNU\unixnu.toml`；只存在于 boot 镜像 ramdisk
字段里的卷不在其中。无论镜像多大，它都看不见。

所以这个卷写到 eMMC 分区上，就像上游 fajita 写到 `boot_a` 那样。
`cache`（`mmcblk0p49`，256 MiB，524288 扇区）可以随便牺牲，也装得下 56 MiB
的卷。`fastboot flash cache` 写入的是裸 Fat32 文件系统，也正是固件挂载的东西：
分区类型对 PartitionDxe 无关紧要，`CLOVERESP` 标签只是装饰。boot 镜像完全
不带 ramdisk，这也让它远低于 ABL 开始拒绝返回的大小。

```sh
fastboot flash boot  ../UniXNU/out/clover/boot/unixnu-diag-nopayload.img   # 只有固件
fastboot flash cache ../UniXNU/out/clover/payload/payload.img
fastboot reboot
```

## 菜单之后看什么

`blsp1_uart2` 在这块板子上没有引出，所以菜单之后只能让屏幕说话。`v0.2.8` 的
payload 里放的是一份**打了探针的 kernel 副本**（`scripts/patch-xnu-entry-probe.py`
改副本，原始 `out/clover/kernel/kernel.macho` 不动）。探针代码写在入口之后、
原本全零的填充区里（`0xFFFFFFF0073EC48C` 起 0xB74 字节），每个站点在改写前都
会先核对原始指令，内核一变就拒绝打补丁。

屏幕从上往下是 1/16 屏高的色带，最上面一条绿带由固件里的 DarwinBoot 自己画
（`v0.3.10` 就有，不需要改内核）：

| 色带 | 位置 | 含义 |
| --- | --- | --- |
| 绿 | 第 0 条，DarwinBoot 画 | `DarwinEnter` 跑到了跳转内核入口的那条 `br` |
| 蓝 | 第 1 条，内核入口 | `_start` 的第一条 `b` 之前：CPU 已经进 XNU，MMU 还是关的 |
| 品红 | 第 2 条，开 MMU 之前 | `msr sctlr_el1, x0` 之前：引导页表已经建好，正准备开 MMU |

三条都在，说明内核跑过了入口和页表建立；只有绿带说明 DarwinBoot 交接了但内核
第一条指令就没回来；只有绿和蓝说明卡在 `_start_first_cpu` 的引导阶段。色带
以下如果出现白字，那是 `panic_screen` 把 panic 的格式串、`TCR`、`SCTLR`、
页计数画在了内核自己的 framebuffer 映射上——正好可以拍照。没有 panic 时
屏幕就停在最后画完的色带。

`v0.2.10` 的复位点在 `msr sctlr_el1, x0; isb` 之后，回答"MMU 本身开没开成"：
实测品红出现约 6 秒后设备自己重启，说明 `sctlr_el1` 生效、`cpacr_el1`、
`tpidr_el1` 和执行流都没问题。

`v0.2.11` 把复位点换成**调用返回后**（`entry_probe_reset_after = "_arm_vm_init"`）：
真机结果是**品红一直停着、不重启**，也就是 `_arm_vm_init` 没走到。`v0.2.12` 把
复位点前移到 `_PE_init_platform` 之后，并在 `_cpu_data_init` 调用点试画一条黄带。
真机读数是**只有绿 + 蓝 + 品红，约 6 秒后自复位**：复位照常发生，说明
`_PE_init_platform` 确实返回了；黄带没出现，说明开 MMU 之后这块屏暂时画不了。

原因在 `v0.2.14` 的读数之后查清了：DarwinBoot 的窗口分配在"能放下它的空闲段"里，
而这块板子 `0x9D400000` 以下是 "Kernel"/SMEM/148 MiB 的 DXE Heap、再往下就是
`0x9D400000 + 0x02400000` 的显示预留和固件自己那几段；唯一够大的空闲从
`0xA0000000` 起。所以窗口（`boot_args.physBase`）落在 framebuffer **上面**，而
`_start_first_cpu` 是从 physBase **向上**映射 memSize 字节——扩 memSize 只能往下
"长"不到 `0x9D400000`。旧 `GROW` 在 `fb_end <= physBase` 时整段跳过，种子却照写，
panic 屏幕再去读一个未映射的 baseaddr 就会 fault：既没有黄带，也没有白字，正好是
`v0.2.14` 的实测。`v0.2.13` 的 12 秒兜底同样说明 MMU 之后确实 panic 过一次，
只是当时画不出来。

`v0.2.15` 直接补页表，仍然**不主动复位**，但实测**还是绿 + 蓝 + 品红、无黄无白**。
原因读代码就能确定：它假设窗口和 framebuffer 在同一个 1 GB L1 槽里，于是拿
framebuffer 的槽号去查 L1，再看那个槽的 L2 表。可是 `0x10000000`（256 MiB）以上的
窗口才能放下 1 GiB 的 `darwinboot-memory-size`，这块板子唯一的候选在高 DRAM bank
（L1 槽 4），framebuffer 在槽 2；`l1[2]` 是无效项，mapper 在第一个自检就退出，
`x8` 保持 0，种子被跳过，`_vinfo` 仍为 0，`AT` 查不到映射就跳过黄带——既没有黄带
也没有白字，`--no-reset` 下就停住。

`v0.2.17` 修正 mapper 的定位方式，仍然**不主动复位**：

1. **蓝带**：入口，MMU 关闭，和以前一样。
2. **品红带**：涂完之后、`msr sctlr_el1, x0` 之前（MMU 仍关），`bl` 进
   `entry + 0x600` 的 mapper 洞穴（`lr` 存在 `x16`，不用栈——此时 `sp` 是高地址
   KVA）。
   - **不再读 `boot_args.physBase`**。这个字段等于 DarwinBoot 分配窗口的地址，
     而内核 Mach-O 里 `__PRELINK_TEXT` 的 vmaddr 比 `__TEXT` 低约 0x2D88000，
     所以 `Kernel.Base` 取的是 prelink 段的地址，`DarwinBoot` 把窗口起始交给它，
     `_mh_execute_header` 落在窗口起始 + 0x2D88000。`start.s` 的引导映射是从
     `_mh_execute_header` 向上建的，**physBase 本身没有被映射**——`v0.2.16` 从
     `[x20, #0x10]` 取窗口基址，读到的是块映射以下的空地，所以自检第 4 步失败。
   - `v0.2.17` 改用 `adr x14, .` 取 mapper 自己运行时的物理 PC（`adr` 是 PC 相对
     的，MMU 关着时就是 PA），从这里向下取 L1 表基址，再确认窗口 L1 项是表、
     其第一个 L2 项是 2 MB 块，把这个块当属性模板。这条路径不依赖任何 boot_args
     字段，而且一定落在已映射区间内。
   - framebuffer 所在槽若已有 L2 表（与窗口同槽）：先把 L1 项的有效位/类型位掩掉
     得到表基址（`v0.2.16` 漏了这一步，复用路径一走到就会非对齐访问），再把缺的
     恒等块补进这张表；
   - 若是 1 GB 块：整槽已映射，直接算成功；
   - 若是无效项（通常情况）：拿窗口最后一页做一张新 L2 表（DarwinBoot 保证窗口顶部
     至少留 2 MB，所以这页是空闲的），清零后写满 framebuffer 的恒等块，再挂到
     framebuffer 的 L1 槽上；
   - 写完 `dsb` + `tlbi vmalle1`，MMU 打开后这块屏就在 V=P 里了。
3. **白色诊断带**：mapper 用 `x8` 返回 0（成功）或失败码；失败时品红洞穴在 MMU 打开
   **之前**涂一条白带，位置是第 `5 + 码` 条 1/16 带。失败码：

   | 码 | 含义 |
   | --- | --- |
   | 1 | `boot_args.Video.base` 为空 |
   | 2 | `rowBytes * height` 为 0 |
   | 3 | 窗口的 L1 项不是表 |
   | 4 | 窗口的第一个 L2 项不是 2 MB 块（拿不到属性模板） |
   | 5 | 保留的 L1 描述符，或窗口最后一页不够空闲 |
   | 6 | 目标 L2 项已经被占用（不是我们写的恒等映射） |

4. **种子**：`x8 == 0` 才把 `boot_args.Video` 的几何写进内核自己的 `_vinfo`
   （MMU 未开，按物理地址写，不压栈），`v_type` 放 `0xC10C10C1`。映射失败时
   `_vinfo` 保持 0，panic 屏幕走 12 秒兜底而不是 fault。
5. **青带（新增，第 4 条）**：`msr sctlr_el1` + `isb` 之后立刻按同样的几何涂第 4 条
   1/16 带，颜色是青色（`0xFF00FFFF`，BGRA），涂完 `dc civac` + `dsb sy` 再跳去
   `target`。它和 mapper 成功标志、`Video` 的几何绑定：只有"MMU 已开、framebuffer
   V=P 可写"才会出现。这样青带把"开 MMU 这一刻"和之后的 `_cpu_data_init` 分开：
   有青无黄 = 卡在 `start.s` 到 `arm_init` 之间；再配合下一轮的橙带，可以把
   "没进 `arm_init`"和"进了但没走到调用点"区分开。
6. **黄带**：`arm_init` 第 1 次调用 `_cpu_data_init` **之前**，按 `_vinfo` 涂第 3 条
   带（洞穴先涂色，再执行被挪走的 `bl`）。涂之前用 `AT S1E1W` 查一次翻译
   （失败就跳过），涂完用 `dc civac` + `dsb sy` 把 Normal-WB 的像素推到显示引擎可见。

`v0.2.17` 上机的结果是**绿 + 蓝 + 品红 + 青，无黄**；`v0.2.18` 加橙带后再测，读数是
**青 + 橙，仍然无黄**。橙带出现说明 `arm_init` 确实进了、第一条调用点也到了；黄带
缺席则一直按"没走到 `_cpu_data_init`"来读。

`v0.2.19` 查出了真正的原因：**种子守卫写反了**。`v0.2.15` 给 `SEED` 加的守卫是
`cbz x8, 9f`——mapper 成功时 `x8 == 0`，这条指令恰好跳过整个种子，`_vinfo` 一直是
0。黄带画笔读 `_vinfo.v_baseaddr` 得到 0，直接跳过，所以从 `v0.2.15` 起黄带从未
有机会出现。它还有一个反向的隐患：mapper 失败时（`x8 != 0`）种子反而会运行，把
未映射的 framebuffer 写进 `_vinfo`，panic 屏幕照着画就会 fault，而不是走 12 秒兜底。
`v0.2.19` 把它改成 `cbnz x8, 9f`。

同时 stage 4 的洞穴变成两段涂色：调用前涂**橙带 12**（证明调用点到了），
`bl _arm_set_kernel_tbi` 返回后再从 `x19`（`arm_init` 在入口保存的 boot_args，
callee-saved）涂**红带 13**（`0xFF0000FF`，证明调用真的返回了）。这样一次上机就能把
"没进 `arm_init` / 没从第一条调用返回 / 到了调用点"三件事分开。

`v0.2.19` 上机后第一次读到了**白字 panic 屏幕**（panic 屏幕按设计从第 `h/8` 行往下
清屏，第 2 条以下的色带都被清掉，所以只剩绿 + 蓝是正常的）：

```text
CPU property 'cluster-type' has bad size 6 @machine_routines.c:936
```

这一屏同时证明了三件事：种子修好之后 `_vinfo` 可用、panic 屏幕能照着它画、内核已经
走到 `_arm_init` 里的 `ml_parse_cpu_topology`（`_cpu_data_init`、`_PE_init_platform`
都已经返回）。panic 的原因是 ADT 契约错误：生成器把 `cluster-type` 写成了字符串
`"ECore"` / `"PCore"`（6 字节，含结尾 0），而 `ml_readprop()` 只接受 1/2/4/8 字节
整数，拓扑代码再把值跟 `'E'` / `'P'` 比较。`v0.2.20` 把每个 `cpuN` 的
`cluster-type` 改成 4 字节整数——cpu0–3 是 `'E'`，cpu4–7 是 `'P'`——并在
`check-clover.py` 里加了断言，防止它再退化成字符串。内核补丁没有任何变化，只有
payload 里的 `kernel.macho.adt` 换了（`kernel.macho` 的哈希与 `v0.2.19` 相同）。

`v0.2.20` 上机后 panic 前进了一步，但仍然停在拓扑里：

```text
machine_routines.c:1099 Assertion failed: phys_cluster_id <= MAX_CPU_CLUSTER_PHY_ID
```

这次不是属性尺寸，而是**簇的数量超出了内核的模型**。这个内核是
`DEVELOPMENT_ARM64_BCM2837`：`board_config.h` 给它 `MAX_CPU_CLUSTERS 1`，也没有
`HAS_CLUSTER`。于是拓扑代码用 `(cluster_type == 'P')` 反推物理簇号，'P' 得到 1，
而允许的最大值是 0，断言在第一个性能核上失败。一个只支持单簇的内核无法接受
"4 个 E + 4 个 P"的描述。

`v0.2.21` 因此不在 ADT 里发布 `cluster-type`：`ml_readprop` 返回默认值 0，即
`CLUSTER_TYPE_SMP`，8 个核落在一个平坦的簇里——这正是这份 Pi 3 内核在没有 ADT 时
的默认状态，也是它 `MAX_CPU_CLUSTERS 1` 能表示的拓扑。要如实描述 Kryo 260 的双簇，
得把内核按 `MAX_CPU_CLUSTERS >= 2` 重新编译，那是后面独立的一轮；眼下先把拓扑搭
起来。`check-clover.py` 的断言也改成"要么没有该属性，要么是 4 字节 `'E'`，`'P'`
会被拒绝"，把这份内核的限制写进检查里。

`v0.2.21` 上机后又前进了一步，而且这次拓扑确实过了：

```text
Undefined kernel instruction: pc=0xfffffff0079aff58 instr=d500419f @ sleh.c:1045
```

`instr=d500419f` 就是 `msr pan, #1`。`ml_parse_cpu_topology` 返回后，`arm_init`
继续执行到 `arm_vm_init` 之前的 PAN 设置就同步异常了。Kryo 260（Cortex-A73/A53
派生）是 ARMv8.0，没有 `PSTATE.PAN`，这条指令在硬件上就是未定义的。根因是
`configs/clover_defconfig` 沿用了 sheng 的 `CONFIG_XNU_NATIVE_PAN=y`；那是给
ARMv8.1 的 SM8550 开的。

`v0.2.22` 把它改成 `n` 并重建内核：旧镜像里的 15 条 `msr pan` 全部消失，三份曾用
`-march=armv8.1-a` 编译的源文件（`copyio.c`、`pcb.c`、`platform_tests.c`）也回到
v8.0。这是本轮第一次改内核构建配置；探针脚本和 ADT 都没有变化，但 `kernel.macho`
的哈希换了，所以 payload 里的内核是重新编译的版本。重建内核时还要给 Xcode 27 的
clang 显式指定 iPhoneOS SDK（见上面的构建命令）。

`v0.2.22` 上机后七条带全中、没有白字：内核进了 `arm_init`，`_arm_set_kernel_tbi`
返回了，`_cpu_data_init` 调用点也到了，随后画面一直停在菜单加色带。紧挨 PAN 的
下一条指令就是 `bl _arm_vm_init`，所以执行已经进入 `arm_vm_init` 或更后面。到
这里色带帮不上忙了：`arm_vm_init` 中途会执行 `set_mmu_ttb(invalid_ttep)`（源码
注释："From here on out, we're off the bootstrap translation tables"），TTBR0 换成
内核自己的页表，framebuffer 的恒等映射随之消失。之后任何涂色都会被 `AT` 守卫
静默跳过；更糟的是 panic 屏幕会照着种子里的物理地址直接写，panic 若发生在切换
之后，第一笔就 fault，于是既没有白字也没有复位。

`v0.2.23` 因此把信号换回时间，并让 panic 也说得出口：panic 屏幕先用
`AT S1E1W` 查一次基址，查不通就 **3 秒后 PSCI 复位**；另外在 `_arm_init` 的
`bl _arm_vm_init` 处放一个复位洞穴，真正执行完这次调用后 **10 秒复位**。

上机读数是**七条带都出现、约 10 秒后设备自己复位**：10 秒正是那个复位洞穴，
所以 `arm_vm_init` 返回了，卡点在它后面。它之后依次是 `patch_low_glo`、
`PE_consistent_debug_inherit`、`serial_init`、`kernel_startup_initialize_upto(KPRINTF)`、
`PE_create_console`、`PE_init_printf`、`dump_kva_space`、`cpu_machine_idle_init`、
`PE_init_platform(TRUE, &BootCpuData)`。其中 `serial_init` 在 ADT 里找不到
`gpio` / `aux` 节点，注册不了串口便返回 0；**`PE_create_console` 是屏幕可能回来的
地方**：`initialize_screen` 此时看到 `kernel_map == VM_MAP_NULL`，会用
`ml_io_map_unmappable()` 把 `boot_args.Video` 重新映射，把新 KVA 写进
`vinfo.v_baseaddr`，之后 panic 屏幕又能画字。但它的第一件事 `check_for_panic_log`
在这份 BCM2837 内核里写死了 `PLATFORM_PANIC_LOG_PADDR = 0x3c0fc000`（树莓派的
地址）；在 clover 上读它就是这一轮要排除的嫌疑：外部异常发生时 framebuffer
还没重新映射，panic 屏幕也画不出来。

`v0.2.24` 因此把复位洞穴放到**第二次 `PE_init_platform(TRUE, &BootCpuData)`
返回后 15 秒**：它在 `PE_create_console` 之后，既能证明控制台建出来了，也给
`kprintf` 的屏幕输出留出拍照时间。

上机读数是**色带之后什么都没有**：既没有文字，也没有 15 秒复位。复位洞穴没响，
说明卡点在 `arm_vm_init` 返回之后、第二次 `PE_init_platform` 之前。把这一段逐个
过一遍，只剩一个具体的怀疑对象：

- `kernel_startup_bootstrap()` 在 `arm_vm_init` 之前就把所有 `STARTUP_SUB_LOCKS`
  及以下的入口排好并跑完了，所以后面的
  `kernel_startup_initialize_upto(STARTUP_SUB_KPRINTF)` 只会跑一个入口
  `PE_init_kprintf`，它也就是再调一次 `serial_init`——ADT 里没有 `gpio` / `aux`
  节点，立刻返回 0。
- 控制台之前的 `kprintf` 是屏幕空操作：`cons_ops_index` 是 `VC_CONS_OPS`，
  `vcputc_options` 在 `gc_initialized` 为假时直接返回；`log_lock` 和
  `kprintf_lock` 都有 LOCKS 阶段的初始化。
- 剩下的就是 `PE_create_console`。它开头的内联 `check_for_panic_log` 在这份
  BCM2837 构建里被编译成读**物理地址 `0x3c0fc000`**（`PLATFORM_PANIC_LOG_PADDR`，
  树莓派的地址）。clover 的 DRAM 从 0x80000000 起，这个地址在它下方约 1 GiB，
  第一条 load 就会外部异常；而此时视频控制台还不存在，panic 屏幕画不出任何东西，
  正好就是"色带之后什么都没有"。

`v0.2.25` 因此由探针脚本把 `PE_create_console` 开头那四条指令

```text
mov w19, #0x4000        ; panic log 大小
mov w0, #0x3c0000
movk w0, #0x3c0f, lsl #16   ; 0x3c0fc000
mov w1, #0x4000
```

改写成

```text
mov w20, #1
b   +0x94               ; 直接到 initialize_screen 之前的 gPESF 判断
nop
nop
```

跳过映射、读 magic 和 `bzero`。`w20 = 1` 是正常路径在映射之后才写的值，
`cinc w1, w20, eq` 会拿它当视频模式参数；`gPanicBase` 保持为 0，正是
`debug_log_init()` 已经能处理的状态。

上机读数是**色带 + 约 15 秒复位、无文字**：复位洞穴响了，说明跳过生效——
`PE_create_console` 返回了、视频控制台已经建立，第二次 `PE_init_platform`
也返回了。没有文字是预期结果，不是故障：`initialize_screen` 只把
`gc_initialized` 置真，`gc_enabled` 仍为假（只有 `vcattach` → `vc_enable`
才会置真），`vcputc_options` 要两者都为真才画字；而 `arm_init` 调的是
`PE_init_printf(FALSE)`，并不会 attach。所以这一段不能用 `kprintf` 当刻度，
复位点继续往后推。

`v0.2.26` 用从 `v0.2.19` 起一直可靠的 vinfo 画笔，给 `arm_init` 剩下的这段
做逐调用刻度。第二次 `PE_init_platform` 之后的五个调用各配一个独立洞穴：
执行调用、涂一条带、跳回原处。

| 色带 | 调用 |
| --- | --- |
| 6 品红 | `_ml_map_cpu_pio` |
| 7 青 | `_cpu_timebase_init` |
| 8 橙 | `_kptimer_curcpu_up` |
| 9 绿 | `_PE_init_cpu` |
| 10 白 | `_fiq_context_init` |

复位点移到 `_machine_startup` 调用处（`--reset-before`，不执行调用），它的
洞穴先涂第 11 条红带再等 15 秒复位。只有 11 出现时，缺失的 6–10 才能归咎于
对应调用。stage 4 被复位占用，所以橙带 12 和红带 13 这一轮不画；黄带仍在
`_cpu_data_init` 调用点。

| 现象 | 含义 |
| --- | --- |
| 6–10 全出现 + 11 红 + 约 15 秒复位 | 五个调用全部返回，卡点在 `machine_startup` 内部 |
| 6–10 里最后一条缺失、11 出现 | 那条调用没有返回 |
| 11 红也不出现 | 复位洞穴没到，或它也画不出来（vinfo / AT 失效） |
| 色带 + **白字** + 不复位 | 控制台之后发生 panic，屏幕已可画，直接拍照 |

```sh
fastboot flash cache ~/clover-out/round56-payload.img   # 只换这一格，boot 不变
fastboot reboot
```

`boot` 镜像没有变化：固件卷、BootShim 和 DTB 都没动，只有 `cache` 上的 payload
卷换了。黄带探针（stage 3，`_cpu_data_init` 调用点）和四条刻度带都是设备仓
脚本里的默认值，基础仓不需要任何本地改动；`patch-xnu-entry-probe.py --no-paint`
可以关掉黄带，`--no-trace` 关掉刻度带。`board.toml` 的
`entry_probe_reset_delay = 15`、`entry_probe_reset_after = "_machine_startup"`、
`entry_probe_reset_before = true` 和 `entry_probe_reset_occurrence = 1` 把
stage 4 洞穴变成"在调用 `machine_startup` 之前定时复位"；删掉这几项、payload
就会退回 `--no-reset` 的纯涂色模式。

### v0.2.27：让 panic 屏印出调用者

`v0.2.26` 实机照片是**绿 + 蓝 + 白字**：

```
<1>:0 Assertion failed: <d>
TCR=0x0000000022a51a251a
SCTLR=0x0000000034d4593d
pages=0x0000000000000000
free=0x0000000000000000
```

第 2 条以下的色带消失是 panic 屏自己清的（它原本从 `h/8` 一直清到 `h`）；
`pages=free=0` 说明 panic 发生在 VM 页初始化之前，也就是 `arm_init` 后段 /
`machine_startup` 这条线上。`<1>` 和 `<d>` 不是字面量，而是格式器对"读不了的
指针"的写法，真实参数是 `0x1` 和 `0xd`。矛盾在于：这份内核里
`"%s:%d Assertion failed: %s"` 只被 `_Assert` 引用，而 `_Assert(file, line,
expression)` 的冷段调用点传的都是常量（`kptimer.c` 575、
`machine_routines.c` 2404 等），与 `0x1`/`0xd` 对不上——单凭这张照片无法判断
是谁调的断言。

因此 `v0.2.27` 让 panic 屏自己交代调用链：

| 行 | 含义 |
| --- | --- |
| `RA=` | 进屏时的 LR（`_panic` 钩子路径下是 `_Assert` 内的返回地址） |
| `FP=` | 调用者的帧指针 |
| `LR=` | `[FP+8]` 的保存值，即调用 `_Assert` 的那个函数里的返回地址——断言现场 |
| `fmt=` / `args=` | 格式串地址和 va_list 指针 |
| `m2` `m1` `a0`–`a5` | va_list 前后各槽的原值；每槽先做 AT S1E1R 查询，读不了就印 `----` |

清屏范围同时缩到 `[h/8, h/2)`，刻度带挪到 10–14（白、橙、绿、青、品红），复位
控制带挪到 15（红）。它们都在下半屏，panic 之后仍能与白字出现在同一张照片里。

| 色带 | 调用 |
| --- | --- |
| 10 白 | `_ml_map_cpu_pio` |
| 11 橙 | `_cpu_timebase_init` |
| 12 绿 | `_kptimer_curcpu_up` |
| 13 青 | `_PE_init_cpu` |
| 14 品红 | `_fiq_context_init` |
| 15 红 | 复位控制（`_machine_startup` 调用前） |

| 现象 | 含义 |
| --- | --- |
| 10–15 全在 + 白字 | panic 在 `_fiq_context_init` 返回之后、`_machine_startup` 之前 |
| 10–15 里最后一条缺失 + 白字 | 那条带对应的调用内部 panic，`LR=` 就是断言现场 |
| 只有白字、没有 10–15 | panic 在第一个被跟踪调用之前 |
| 10–15 全在 + 约 15 秒复位 | 没有 panic，卡点在 `machine_startup` 内部 |

```sh
fastboot flash cache ~/clover-out/round57-payload.img
fastboot reboot
```

### v0.2.28：白字是探针自己撞出来的

`v0.2.27` 实机照片是**绿 + 蓝 + 白字**，panic 屏把调用链交代得很完整：

```
<1>:0 Assertion failed: <d>
RA=0xfffffff00797a3b0
FP=0xfffffff007baff20
LR=0xfffffff0073f1dc4
fmt=0xfffffff007047677
args=0xfffffff007baff00
m2=0xfffffff0071568d0  m1=0x0000000000000000
a0=0x0000000000000001  a1=0x0000000000000000
a2=0x000000000000000d  a3=0x0000000000000000
a4=0xfffffff007bafff0  a5=0xfffffff0073f1dc4
```

`RA` 是 `_Assert` 里的返回地址，`fmt` 是真实的断言格式串，说明断言路径本身
没错；矛盾在参数。`"%s:%d Assertion failed: %s"` 在这份内核里只有 `_Assert`
引用，而 `_Assert(file, line, expression)` 的调用点传的三个参数是
`(文件名, 行号, 表达式)`，全部是编译期常量；`0x1 / 0 / 0xd` 既不是文件名，
也不是行号。换句话说：**panic 是真的，但它不是内核代码主动调的 `_Assert`。**

于是这一轮先反汇编探针，而不是内核。结果如下：

- 第 14 条刻度洞穴（`_fiq_context_init`）被放在 `0xfffffff0073ed784`，地址
  4 mod 8；`ld.lld` 会把 `.text` 向上对齐，`_start` 实际落在 `0x3ed788`，
  而 patcher 仍按 `0x3ed784` 写入二进制。
- 于是整段 cave 里所有 PC 相对分支都低了 4 字节：`bl _fiq_context_init`
  落到 `_fiq_context_init - 4`，正好是 `_platform_syscall` 冷段里那条
  `bl _Assert`；返回指令 `b site+4` 也退回到调用点本身。
- `x0/x1/x2` 此时仍是 `arm_init` 传给 `_fiq_context_init` 的参数
  `TRUE / 0 / 0xd`，与照片里 `a0/a1/a2` 完全吻合；`LR=0x3f1dc4` 是
  `_Assert` 保存在 `[FP+8]` 的返回地址，也就是"调用者"被认成了
  `_fiq_context_init`。
- 第 10–13 条洞穴都是对齐的，`bl` 与返回目标都正确，所以
  `_ml_map_cpu_pio`、`_cpu_timebase_init`、`_kptimer_curcpu_up`、
  `_PE_init_cpu` **确实都返回了**；`v0.2.27` 那张照片对
  `_fiq_context_init` 和 `_machine_startup` 没有提供任何信息。

`v0.2.28` 在 patcher 里加了三道保险：`assemble()` 用 `llvm-nm` 要求
`_start` 正好等于请求的 VA；刻度洞穴的放置改为先对齐到 16 字节，且对齐后
仍要放得进同一段零填充；写入前解码 `bl`，目标必须等于符号表里的 callee。
五条洞穴现在都从 16 字节边界开始。刻度带、复位点、panic 屏内容都不变：

| 色带 | 调用 |
| --- | --- |
| 10 白 | `_ml_map_cpu_pio` |
| 11 橙 | `_cpu_timebase_init` |
| 12 绿 | `_kptimer_curcpu_up` |
| 13 青 | `_PE_init_cpu` |
| 14 品红 | `_fiq_context_init` |
| 15 红 | 复位控制（`_machine_startup` 调用前） |

| 现象 | 含义 |
| --- | --- |
| 10–14 全在 + 15 红 + 约 15 秒复位 | 五个调用全部返回，卡点在 `machine_startup` 内部 |
| 10–14 里最后一条缺失、15 出现 | 那条调用没有返回 |
| 10–13 + 白字 `machine_routines.c:2404` | `_fiq_context_init` 里中断状态不对，断言现场就是它 |
| 15 红也不出现 | 复位洞穴没到，或它也画不出来（vinfo / AT 失效） |

```sh
fastboot flash cache ~/clover-out/round58-payload.img
fastboot reboot
```

### v0.2.29：跨过交接点

`v0.2.28` 实机读数是**第 10–14 条带全在 + 第 15 条红带 + 约 15 秒复位**。
五条刻度带都是"调用返回后才涂"，所以 `_ml_map_cpu_pio`、`_cpu_timebase_init`、
`_kptimer_curcpu_up`、`_PE_init_cpu`、`_fiq_context_init` 全部返回了；红带出现
说明 `arm_init` 走到了 `_machine_startup` 调用点，15 秒后复位也正常。到这一刻
为止，交接点之前没有任何卡死——问题只能在 `_machine_startup` 里面。

`_machine_startup` 只有两个动作：`bl _machine_conf`，然后 `b _kernel_bootstrap`。
因此 `v0.2.29` 把检查点从"调用前复位"改成"真正调用"，并把复位挪到
`kernel_bootstrap` 里：

| 色带 | 含义 |
| --- | --- |
| 15 红 | `arm_init` 到达交接点，进入 `_machine_startup` |
| 9 绿 | `_machine_conf` 返回 |
| 8 橙 | `_kernel_bootstrap` 已进入（在它的第一条 `printf` 之前涂） |
| 7 青 | 那条 `printf` 返回 |
| 6 白 | 到达 `vm_mem_bootstrap` 调用点；涂完等 15 秒复位 |

每一档都只在上一次调用返回之后才涂，所以**最高的缺失色带就是没返回的那个调用**。
第 10–14 条刻度带保留，读数可与 `v0.2.28` 直接对照。

| 现象 | 含义 |
| --- | --- |
| 10–14 + 15 + 9 + 8 + 7 + 6 + 约 15 秒复位 | 一路走到 `vm_mem_bootstrap` 调用点；卡点在它内部 |
| 10–14 + 15 + 9 + 8 + 7，无 6、无复位 | 首条 `printf` 已返回，卡在它之后、`vm_mem_bootstrap` 之前（`_scale_setup`、`kernel_bootstrap_log`） |
| 10–14 + 15 + 9，无 8 | `_machine_conf` 返回后、进入 `kernel_bootstrap` 前卡住 |
| 10–14 + 15，无 9 | 卡在 `_machine_conf` 内部 |
| 10–14，无 15 | 连交接点都没到（与 `v0.2.28` 读数矛盾，说明探针自身出问题） |
| 出现白字 | panic 屏工作，直接拍照 |

```sh
fastboot flash cache ~/clover-out/round59-payload.img
fastboot reboot
```

### v0.2.30：进 vm_mem_bootstrap

`v0.2.29` 的实机照片把整条梯子都点亮了：15、9、8、7、6 全在，10–14 也在，
约 15 秒后复位。逐带取中位色确认：10 白、11 橙、12 绿、13 青、14 品红、
15 红、6 白，与设计一一对应。也就是说 `_machine_conf` 返回了、
`_kernel_bootstrap` 进了、首条 `printf` 返回了、`vm_mem_bootstrap` 被调用了；
卡点在 `vm_mem_bootstrap` 内部。照片里那条蓝底白字 `clover first boot: source
4K XNU to console` 是 UEFI 菜单被选中项的高亮行，不是内核输出——内核视频控制台
还没有 attach，`printf` 的字仍然画不出来。

`vm_mem_bootstrap` 的顺序是 `vm_page_bootstrap` → `zone_bootstrap` →
`vm_object_bootstrap` → `vm_map_init` → `kmem_init`。`v0.2.28` 里已经证明
`arm_init` 全部走通，所以第 10–14 条带不再跟踪 `arm_init`，改由它们标这五个
调用；复位也随之后移到 `kmem_init` 之后：

| 色带 | 含义 |
| --- | --- |
| 15 红 | `arm_init` 到达交接点，进入 `_machine_startup` |
| 9 绿 | `_machine_conf` 返回 |
| 8 橙 | `_kernel_bootstrap` 已进入（在它的第一条 `printf` 之前涂） |
| 7 青 | 那条 `printf` 返回 |
| 6 白 | 进入 `vm_mem_bootstrap`（在调用之前涂） |
| 10 黄 | 到达 `vm_page_bootstrap` 调用 |
| 11 蓝 | 到达 `zone_bootstrap` 调用 |
| 12 白 | 到达 `vm_object_bootstrap` 调用 |
| 13 橙 | 到达 `vm_map_init` 调用 |
| 14 品红 | 到达 `kmem_init` 调用 |
| 5 红 | `kmem_init` 返回；涂完等 15 秒复位 |

每一档都是**调用之前**涂的，所以最高的缺失色带就是卡在里面的那个调用。
这一次如果卡住，就不会有复位——设备会一直停在画面上，本身也是一种读数。

| 现象 | 含义 |
| --- | --- |
| 全部（含 14 与 5 红）+ 约 15 秒复位 | `kmem_init` 也返回了；卡点在 `vm_mem_bootstrap` 后段（`kernel_startup_initialize_upto`、`vm_fault_init`） |
| 14 在、5 不在 | 卡在 `kmem_init` 内部 |
| 13 在、14 不在 | 卡在 `vm_map_init` 内部 |
| 12 在、13 不在 | 卡在 `vm_object_bootstrap` 内部 |
| 11 在、12 不在 | 卡在 `zone_bootstrap` 内部 |
| 10 在、11 不在 | 卡在 `vm_page_bootstrap` 内部 |
| 6 在、10 不在 | 进了 `vm_mem_bootstrap`，但第一条调用之前就出事（多为 panic，看白字） |
| 出现白字 | panic 屏工作，直接拍照 |

```sh
fastboot flash cache ~/clover-out/round60-payload.img
fastboot reboot
```

### v0.2.31：vm_mem_bootstrap 的后段

`v0.2.30` 的实机照片上，十六条色带全部出现，包括第 5 条红带，约 15 秒后复位。
逐带取中位色：0 绿、1 蓝、2 品红、3 黄、4 青、5 红、6 白、7 青、8 橙、9 绿、
10 黄、11 蓝、12 白、13 橙、14 品红、15 红，与设计一一对应。也就是说
`vm_page_bootstrap`、`zone_bootstrap`、`vm_object_bootstrap`、`vm_map_init`、
`kmem_init` 全部返回，复位档也走到了；卡点在 `vm_mem_bootstrap` 的后段。

后段只剩四个调用：`kernel_startup_initialize_upto(STARTUP_SUB_KMEM)`、
`vm_fault_init`、`kernel_startup_initialize_upto(STARTUP_SUB_ZALLOC)`，以及最后
那条 `kprintf`（IOKit IOMD setownership）。第 10–14 条带改标它们，复位挂在最后
一次返回之后：

| 色带 | 含义 |
| --- | --- |
| 15 红 | `arm_init` 到达交接点，进入 `_machine_startup` |
| 9 绿 | `_machine_conf` 返回 |
| 8 橙 | `_kernel_bootstrap` 已进入（在它的第一条 `printf` 之前涂） |
| 7 青 | 那条 `printf` 返回 |
| 6 白 | 进入 `vm_mem_bootstrap`（在调用之前涂） |
| 5 红 | `kmem_init` 返回，前半段五个调用全部返回 |
| 10 黄 | 到达 `kernel_startup_initialize_upto(KMEM)` 调用 |
| 11 蓝 | 到达 `vm_fault_init` 调用 |
| 12 白 | 到达 `kernel_startup_initialize_upto(ZALLOC)` 调用 |
| 13 橙 | 到达最后那条 `kprintf` 调用 |
| 14 品红 | 那条 `kprintf` 返回；涂完等 15 秒复位 |

前两条 `kernel_startup_initialize_upto` 是同一个被调函数，靠出现次序和调用前
那条 `mov w0`（6 = KMEM、7 = ZALLOC）区分；10、11、12、13 都是**调用之前**涂
的，所以最高的缺失色带就是卡在里面的那个调用。

| 现象 | 含义 |
| --- | --- |
| 13 与 14 都在 + 约 15 秒复位 | 整个 `vm_mem_bootstrap` 返回了；卡点已经移到它之后（`kernel_bootstrap` 的下一段） |
| 13 在、14 不在 | 卡在最后那条 `kprintf` 内部 |
| 12 在、13 不在 | 卡在 `kernel_startup_initialize_upto(ZALLOC)` 内部 |
| 11 在、12 不在 | 卡在 `vm_fault_init` 内部 |
| 10 在、11 不在 | 卡在 `kernel_startup_initialize_upto(KMEM)` 内部或它调的子项里 |
| 5 在、10 不在 | `kmem_init` 返回后、第一个后段调用之前出事（多为 panic，看白字） |
| 出现白字 | panic 屏工作，直接拍照 |

```sh
fastboot flash cache ~/clover-out/round61-payload.img
fastboot reboot
```

### v0.2.32：接上视频控制台

`v0.2.31` 的实机读数是第 13、14 条带都在，约 15 秒后复位：最后那条 `kprintf`
返回了，整个 `vm_mem_bootstrap` 也返回了。卡点在 `kernel_bootstrap` 的下一段，
但这一段调用很多（`atm_init`、`kernel_startup_initialize_upto(OSLOG)`、
`ubsan_minimal_init`、`telemetry_init`、`console_init`、`sched_init`、
`startup_upto(MACH_IPC)`、`machine_init`、`clock_init`……），再一档一档画带
太慢，而且每次都要盲猜下一个调用。

这一轮换手段：**在 `vm_mem_bootstrap` 返回后直接调用 `_vcattach`**，把视频
控制台接上。之后内核所有的 `kprintf` 都会画到屏幕上，卡点自然会以"最后一行
文字"的形式出现，不必再猜。这条路径已经静态核过：`zalloc` 已就绪
（`STARTUP_SUB_ZALLOC` 已跑），`initialize_screen` 不再碰 IOKit，
`gc_enable` 用 `Z_WAITOK` 分配控制台缓冲并清屏，`msgbuf` 是静态数组，
`vcattach` 会把开机以来的消息重放出来。

色带变化只有一处：第 6 条白带仍在进入 `vm_mem_bootstrap` 前涂；调用返回后，
**第 15 条带由红改涂绿**，表示"整个函数返回、即将接控制台"。本轮**没有定时
复位**，设备停住就是在显示它的最后一行。

| 现象 | 含义 |
| --- | --- |
| 色带还在（含第 15 条绿） | 进了 `_vcattach`，但没走到清屏：卡在它内部，或屏幕无法重新取得 |
| 色带被文字或空屏取代 | 控制台已接上并清屏；读屏上的文字（开机消息或 panic） |
| 白字 panic | 接上之后 panic，panic 屏能画（framebuffer 可翻译时它原地停车，不复位） |
| 既无色带也无文字 | 检查是否在清屏后、第一行输出前卡住；重拍一张，屏幕可能有极暗的文字 |

```sh
fastboot flash cache ~/clover-out/round62-payload.img
fastboot reboot
```

### v0.2.33：修掉 AMFI 的前置，并真正接上文字控制台

`v0.2.32` 的实机照片给出了两个答案。第一，**内核根本没有停在
`vm_mem_bootstrap` 之后**：panic 屏打出了真正的内核 panic

```
"AMFI: no amfi-only-platform-code device tree node!"
@AppleMobileFileIntegrity.cpp:5025
RA=0xfffffff004ac9884  LR=0xfffffff004ab4f90
```

两个地址都落在预链接的 AMFI kext 文本里。也就是说它跑完了
`kernel_bootstrap` 的其余部分、IOKit 和预链接 kext 的加载，最后停在 AMFI。
之前"`vm_mem_bootstrap` 之后卡住"的判断，其实是每一轮自己的定时复位把还在
前进的内核切断了。

第二，**`_vcattach` 没有把屏幕接过去**：色带、UEFI 菜单都还在，panic 文字
只出现在 panic 屏清掉的中段。原因在 `vcattach` 的第一行判断：
`PE_create_console` 因为 `PE_state.video.v_display` 非零选了
`kPEGraphicsMode`，`gc_graphics_boot` 因此为 `TRUE`，而 `vcattach` 只在
`gc_graphics_boot == FALSE` 时才清屏、重放 `msgbuf`。

这一轮修两处：

- ADT 的 `/chosen` 加上 `amfi-only-platform-code = <1>`（u32，长度必须正好
  4 字节），与 QEMU fixture 发布的同名属性一致。
- 控制台洞穴在 `_vcattach` 之前先调 `initialize_screen(NULL, kPETextMode)`
  （`x0 = 0`、`w1 = 2`）。它把 `gc_graphics_boot` 置回 `FALSE`；随后
  `vcattach` 里的 acquire 会执行 `gc_enable(TRUE)`，分配控制台缓冲、清屏，
  再重放 `msgbuf`。

洞穴顺序变成：涂第 6 带白 → `bl _vm_mem_bootstrap` → 涂第 15 带绿 →
`bl _initialize_screen` → `bl _vcattach` → 回 `0x2b4278`。本轮仍无定时复位。

| 现象 | 含义 |
| --- | --- |
| 满屏内核文字（或白字 panic） | 控制台已接上；最后一行就是新的卡点 |
| 文字后接着 AMFI 的新 panic | `amfi-only-platform-code` 已过，但 AMFI 还有下一项检查，直接读 panic 串 |
| 色带还在、没有文字 | `kPETextMode` 没生效；这是下一轮要查的点 |
| 屏幕全黑 | 已清屏但还没输出；等 30 秒再拍一张 |

```sh
fastboot flash cache ~/clover-out/round63-payload.img
fastboot reboot
```

### v0.2.34：用文字标记指出 `_bsd_init` 停在哪一个调用

`v0.2.33` 的两张照片把控制台这一关确认了：满屏内核日志，`AMFI: no
amfi-only-platform-code device tree node!` 那条 panic 不再出现，AMFI 正常
加载（`queried model name from device tree: iPad7,1`、`Security policy
loaded: Apple Mobile File Integrity (AMFI)`），Sandbox、CoreAnalyticsHub、
`dill_init`、apfs、AppleImage4 也都走过了。日志最后三行是 NVRAM 同步失败：

```
[System Event] … {Subsystem: nvram} {Event: sync} triggered
IONVRAMController.cpp:serverIsAvailable:615 - No _nvramController
IONVRAMController.cpp:sync:894 - serializedVariables failed, ret=0xe00002bc …
```

没有 panic，屏幕就停在这里。但这三行不能直接当作卡点：`IONVRAMController`
是 kext，它的 `start`/同步可能在后台线程里跑，打印顺序不等于启动线程的顺序。
启动线程可能已经在更后面的 `vfs_mountroot` 里等一个永远不会出现的根设备，
而面板上没有新行。

这一轮不再猜调用、也不再加色带：**在 `_bsd_init` 的每个无条件阶段调用后面
印一行标记**。每个站点被搬进一个洞穴，洞穴先压好 `x0`–`x8`（参数和间接
返回值），执行原来的 `bl`，保存返回值，用 `adr` 取洞尾的字符串调 `_printf`，
恢复寄存器后跳回站点 +4。所以标记一定在调用返回之后才出现：

- `[clover] vfsinit returned`
- `[clover] pre-autoconf inits returned`（`_proc_uuid_policy_init`，条件式
  kext 加载块之后的第一个无条件调用）
- `[clover] kminit returned`（`bsd_autoconf` 被 Clang 内联进了 `_bsd_init`，
  独立的 `_bsd_autoconf` 符号没有调用者）
- `[clover] IOKitBSDInit returned`
- `[clover] loopattach returned`
- `[clover] ether_family_init returned`
- `[clover] net_init_run returned`
- `[clover] vfs_mountroot returned`
- `[clover] devfs_kernel_mount returned`

三个由 boot-arg 守卫的 `OSKextLoadKextWithIdentifier` 站点没有放标记：它们
在本机可能根本没被调用，缺席说明不了问题。

| 现象 | 含义 |
| --- | --- |
| 标记到 `net_init_run` 为止 | 卡在 `vfs_mountroot`：没有块设备/根卷，等 `boot-uuid-media` 超时；若停着超过 60 秒，屏幕应每 60 秒再出现一次 `Still waiting for root device` |
| 标记到 `IOKitBSDInit` 为止 | 卡在 `loopattach`、`ether_family_init` 或 `net_init_run`，最后一行就是答案 |
| 只有 `kminit` | 卡在 `kminit` 之后的伪设备初始化链表或 `IOKitBSDInit`（`publishResource("IOBSD")`）里 |
| 连 `vfsinit` 都没有 | 标记机制或被搬走的调用本身有问题，不是内核卡在更后面 |
| 标记后出现白字 panic | 卡点其实是 panic；panic 屏这次能画，直接读串 |

```sh
fastboot flash cache ~/clover-out/round64-payload.img
fastboot reboot
```

### v0.2.35：把根设备换成不依赖驱动的 md0/mockfs

`v0.2.34` 的照片把这一问一次答死了：`[clover] net_init_run returned` 印了，
`[clover] vfs_mountroot returned` 没印，屏幕最后一行是

```
Waiting on <dict><key>IOProviderClass</key><string>IOMedia</string>
<key>IOPropertyMatch</key><dict><key>UUID</key><string>…</string></dict></dict>
```

即启动线程好好地停在 `IOFindBSDRoot`，等一个永远不会有驱动来发布的根
`IOMedia`。clover 的根盘是 eMMC（`sdhc_1 @ 0xc0c4000`），这份内核没有 SDHCI
驱动，`CONFIG_XNU_NATIVE_UFS=n`，所以匹配表里永远不会有对象进来。这不是 bug，
也不是死锁，是这条路本身在 clover 上不通。

XNU 里有一条不需要驱动的根路径，本仓库在 QEMU 上已经验证过它的形状
（`scripts/probe-darwinboot-xnu.py --native-user`）：

1. `/chosen/memory-map` 里留一个 16 字节的 `RAMDisk` 占位，DarwinBoot 在交接前
   把 initrd 复制进自己的窗口，并写入 `<base, size>`；它只接受 memory-map 里
   **一个** ramdisk 槽，所以 `PackedRAMDisk` 必须撤掉；
2. boot-arg `rd=md0`：`IOFindBSDRoot` 于是走 `mdevadd(-1,
   ml_static_ptovirt(base) >> 12, size >> 12, 0)` + `mdevlookup`，印出
   `BSD root: md0, major …, minor …` 后 `goto iofrootx`，整个等待循环被跳过；
3. 根文件系统是 **mockfs**（已链进这份内核，它的 `vfsconf` 在
   `__DATA.__data` 里，`vfc_mountroot = _mockfs_mountroot`）。它把这颗 md 设备
   本身当作 `/sbin/launchd`，`/` 是它自己造的目录，`/dev` 是 devfs 挂载点。
   带 `N4KROOT` 头的双文件镜像是上游新增的可选容器格式，裸 Mach-O 仍然走
   "legacy single-Mach-O" 这条路，所以本轮就用一个签名过的静态可执行文件。

于是新增两个脚本：

- `scripts/clover-el0-init.S` —— 给 md0/mockfs 当 init 的 EL0 程序：`getpid`
  （`svc` 20）、`sysctl({CTL_HW, HW_PAGESIZE})`（202）、往 fd 1 写一行带这两个
  结果的文字，然后 `exit(0)`。字符串长度在运行时用两个 `adrp`/`add` 相减得到，
  因为汇编器不肯把标签差折进 `mov`。
- `scripts/make-clover-root.py` —— 编译、`ld -static -e _start
  -platform_version ios 17.0 17.0`、`vtool -set-build-version`、`codesign
  --force --sign - --pagesize 4096`，验签，取回 CDHash，产出 `launchd.macho`、
  `trustcache.bundle` 和一份 `report.json`（里面有可以直接粘进 board.toml 的
  `trust_cache_sha256` 一行）。bundle 的形状和上游 fixture 一致：
  `struct.pack('<2I', 1, 8)` 加一条
  `struct.pack('<I16sI', 2, uuid5(NAMESPACE_URL, id:cdhash), 1) + cdhash + b'\x02\x00\x00\x00'`。

板级配置随之改三处：`[payload.adt] packed_data_ramdisk = false`（只留一个
`RAMDisk` 槽，`/chosen` 保持普通 main boot，不加 `ephemeral-storage`）、
`kernel_args` 加 `rd=md0`、`[payload] trust_cache_sha256` 钉住那颗 cdhash。
`scripts/check-clover.py` 现在按这个契约检查：`adt_command` 用 `--data-ramdisk`，
memory-map 必须恰好有 `RAMDisk` 与 `TrustCache` 且不得出现 `PackedRAMDisk`，
`/chosen/amfi-only-platform-code` 必须是 4 字节（AMFI 缺了它就 panic），board
一段则要求 `rd=md0` 和 `trust_cache_sha256` 都在。

探针也加了一条标记，而且是 `_bsd_init` 里最后一条有用的：
`[clover] siginit returned, execing init`。它后面就是 `bsd_utaskbootstrap`，
它在**启动线程自己身上** exec `/sbin/launchd`（`bsd_init` 的最后一行是
`panic("bsd_init returned from call to bsd_utaskbootstrap")`），所以不可能有
标记在它之后。init 主动 `exit(0)`，`kern_exit.c` 会先印
`initproc failed to start -- no exit reason available -- (signal 0, exit status 0)`，
再 panic —— 这是"已经跑到 EL0"最明确、最可读的信号。但那次面板上没有任何
`[clover] el0 init:` 行：这份 XNU 没有 `open_console()`，pid 1 的 fd 0/1/2 是空的，
所以往 fd 1 写什么都不会到面板。要看见用户态写的字，init 得自己
`open("/dev/console")` —— 那就是 v0.2.36。

| 现象 | 含义 |
| --- | --- |
| `BSD root: md0, major 3, minor 0` | `RAMDisk` 占位被 DarwinBoot 填上了，`mdevadd`/`mdevlookup` 也认了 |
| `[clover] siginit returned, execing init` | `vfs_mountroot` 与 `devfs_kernel_mount` 都返回了，下一步就是 exec |
| panic `Process 1 exec of %s failed, errno N` | 根找到了但 exec 失败：cdhash 不被信任、架构/page size 不对、mockfs 布局不合期望（`kern_exec.c:7198`） |
| panic `initproc failed to start` | pid 1 已经建好、已经跑到用户态并且退出了。它**不是** exec 失败：`initproc_spawned` 是 launchd 自己在 `main` 里写 `kern.initproc_spawned` 才置位的（`kern_exit.c:1338`），我们的 init 不写它，所以连干净的 `exit(0)` 也会报成 "failed to start" |
| panic `initproc exited` + 那行 `[clover] el0 init: pid=… sysctl=…` | 一个会写 `kern.initproc_spawned` 的 init 跑完退出；本轮的 init 不会走到这一支 |
| `IOFindBSDRoot: specified root memory device, md0, has not been configured` | `rd=md0` 传到了，但 `mdevadd` 没成功（占位没填、或 `ml_static_ptovirt` 够不到那颗物理地址） |
| 还是那条 `Waiting on … IOMedia …` | `rd=md0` 没进内核，或 payload 里没有 `initrd` |

构建 payload 这一步多了两个输入：

```sh
cd ../UniXNU-clover && /opt/homebrew/bin/python3.13 scripts/make-clover-root.py
cd ../UniXNU
./unixnu build payload --unique-chip-id 0x<device seed> --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-root/trustcache.bundle
```

本轮的 payload 是 `~/clover-out/round65-payload.img`（`12d87a85…`），里面的
`kernel.macho` 是 `1c8e9a40…`（与本地用同一组开关试打的副本逐字节一致），
ADT `e3668f1d…`（13476 字节，比上一轮少 396 字节，正是撤掉 `PackedRAMDisk`
和 `ephemeral-storage` 的结果），根镜像 `4a13c700…`，trust cache `e7c5257f…`，
`unixnu.toml` 里 `initrd = "Payloads/scratch-data.img"` 与
`args = "… rd=md0"` 都在。

```sh
fastboot flash cache ~/clover-out/round65-payload.img
fastboot reboot
```

### v0.2.36：让 init 自己打开 /dev/console

`v0.2.35` 的照片一次答完两问。panic 是

```
initproc failed to start -- no exit reason available -- (signal 0, exit status 0)
```

前面没有 `CS_KILLED`，`a1`/`a2` 两个参数槽都是 0。这一支来自
`proc_handle_critical_exit()`（`bsd/kern/kern_exit.c:1845-1873`），能走到这里
的前提是 pid 1 已经建好、已经跑进用户态、并且**自己退出了**；exec 真失败的话
报的是另一条 `Process 1 exec of %s failed, errno %d`（`kern_exec.c:7198`）。
`signal 0, exit status 0` 就是我们的 `exit(0)`。所以 `md0` + mockfs + 签名静态
EL0 init 这条根是通的，Stage 6 达成：`getpid`、`sysctl`、`write`、`exit` 四个
syscall 都返回了，AMFI 也认了那颗 cdhash。它上面还留着一整屏内核日志
（`zone_init`、`VM bootstrap done`、`IOKit JOINed astownership`、`RTKit`、
`Firehose configured`），说明 v0.2.33 接上的文字控制台一路工作到 exec 之前。

唯一没出现的是 `[clover] el0 init: pid=…` 那行。原因不是写坏了，是**根本没有
fd 1**：这份 XNU 已经没有 `open_console()`（全树 grep 无引用），`bsd_init` 交给
initproc 的描述符表是空的，真正的 launchd 自己去开 `/dev/console`。而
`/dev/console` 在这台机器上是存在的：`devfs_init()` 在挂载 devfs 时做了
`devfs_make_node(makedev(0, 0), …, "console")`
（`bsd/miscfs/devfs/devfs_vfsops.c:115`），`devfs_kernel_mount("/dev")` 由
`bsd_init.c:999` 调用，mockfs 给 `/dev` 留的就是那个挂载点；打开它时
`cnopen()`（`bsd/dev/arm/cons.c:74`）转发到 `km_tty[0]`，那棵 tty 是 `kminit()`
（`bsd/dev/arm/km.c:53`）在 `bsd_autoconf` 里 `ttymalloc()` 出来的——v0.2.34 的
`[clover] kminit returned` 标记印过，所以它一定分配好了；`kmopen()` 再用
`PE_current_console` 把窗口算成 150×120（1200/8 × 1920/16）。写出去就是
`tty → kmputc → PE_char_out`，落到我们已经接上的视频控制台。

本轮因此只换 init，内核、探针、ADT 契约一律不动：

- `getpid` 之后 `sysctl({CTL_HW, HW_PAGESIZE})` 现在把**值**读回来（不只是返回
  码），因为把 4096 抄进用户页同时证明了用户栈的可写映射；
- `open("/dev/console", O_RDWR)`（`svc` 5），拿到 fd 之后所有文字都写到那个 fd；
- banner 变成 `[clover] el0 init: pid=<n> console fd=<n> pagesize=<n>`；
- 退出码是这一轮的读数：**阶段 × 32 + errno**，阶段 1=`getpid`、2=`sysctl`、
  3=`open`、4=`write`，只记第一次失败（后面都是它的后果，加起来会解码成另一个
  阶段）。全对则 `exit(0)`。

| 现象 | 含义 |
| --- | --- |
| `[clover] el0 init: pid=1 console fd=0 pagesize=4096` + `exit status 0` | EL0 有自己的控制台了，Stage 7 的通道打通 |
| `exit status 32+n` / `64+n` | `getpid` / `sysctl` 失败，errno = n；banner 仍然会印出来 |
| `exit status 96+n` | `open("/dev/console")` 被拒：`+6` ENXIO 是 cdevsw 里没有 console 主设备，`+2` ENOENT 是 devfs 没挂到 mockfs 的 `/dev` 上，`+13` EACCES 是 `devfs_check_protected` 那条路径 |
| `exit status 128+n` | fd 开到了但写被拒（`+6` ENXIO 表示 `kmopen` 之后那条 tty 没有输出通道） |
| 只有 panic、没有 banner | 退出码仍然会印出来，用它定位 |

换镜像会换 cdhash，所以 `scripts/make-clover-root.py` 重跑一次要连着更新
`[payload] trust_cache_sha256`（本轮 `df4f2924…`，镜像 33840 字节，DarwinBoot
的窗口仍按 `ALIGN_VALUE(size, 4096)` 取 36864）。构建命令与上一轮相同，只是
`--data-ramdisk` 换上新镜像：

```sh
cd ../UniXNU-clover && /opt/homebrew/bin/python3.13 scripts/make-clover-root.py
cd ../UniXNU
PATH=$HOME/UniXNU-clover/build/shim:$PATH ./unixnu build payload \
  --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-root/trustcache.bundle
```

本轮 payload 是 `~/clover-out/round66-payload.img`（`178c3e46…`，58720256
字节）。逐字节校验过卷内四个文件：`kernel.macho` `1c8e9a40…`（与 round65 同一
份，本轮没碰内核）、ADT `95f9143f…`（13476 字节，`memory-map` 里恰好
`RAMDisk` + `TrustCache` 两个零占位、`amfi-only-platform-code` 4 字节、
`osenvironment = main`）、根镜像 `a7459855…`、trust cache `df4f2924…`；
`unixnu.toml` 的 `initrd` 与 `rd=md0` 都在，菜单标题已换成
`clover md0 root: EL0 init on /dev/console`。

```sh
fastboot flash cache ~/clover-out/round66-payload.img
fastboot reboot
```

### v0.2.37：成功就不退出，让活控制台当读数

round66 的照片和 round65 长得几乎一样：同一屏日志、同一条 panic，只是这次
`exit status 0`。按上一轮的编码这就是四步全通——`getpid`、`sysctl`、
`open("/dev/console")`、`write` 一个错都没报，EL0 到控制台 tty 的 syscall 通道
是通的。但 banner 还是没进照片，而这次原因能指到行：panic 钩子
（`scripts/xnu-panic-screen.c` 的 `panic_screen_main`）只清 `[h/8, h/2)`、从
`h/8+8` 开始印字；`[0, h/8)` 那条窄带留下的是活控制台和 msgbuf 的尾巴，而
**tty 写出去的字从不进 msgbuf**（msgbuf 只收 `kprintf`），所以那一行 banner
画上去之后，又被报告它成功的这条 panic 擦掉了。缺的不是"会写"，是"拍到"。

本轮因此把退出改成**失败通道**：init 在四步全通之后不再 `exit`，而是把 banner
连写 240 遍——120 行的屏整整两屏——然后停在 EL0 的死循环里，让活控制台自己
当读数；任何一步失败才 `exit(阶段 × 32 + errno)`，那条 panic 文字自带动，
不需要 banner 在场。240 遍也顺便排除了"只写一行被滚屏挤掉"的可能。

| 现象 | 含义 |
| --- | --- |
| 整屏 `[clover] el0 init: pid=1 console fd=… pagesize=…`，设备停住不复位 | tty 写确实画到了面板：EL0 到屏幕的输出通道闭环，Stage 7 的地基 |
| 设备停住不复位，但屏上仍是启动日志尾巴、没有 banner | write 被 tty 接受却从没画出来：`cnwrite → kmputc → PE_char_out` 这段在设备侧断，下一轮查它 |
| panic `exit status 96+n` / `128+n` | 与 round66 同一张表：`open` / `write` 被拒，errno = n |
| panic `exit status 32+n` / `64+n` | `getpid` / `sysctl` 失败，比控制台更早的问题 |
| 复位或黑屏 | 不在本轮任何一个分支里：init 成功路径不再触发复位，失败路径只会 panic |

构建与校验同上一轮，只是根镜像换成 33856 字节的新 init（cdhash
`9ca0371f…`），trust cache 因此换成 `e0a7d7ee…` 并已钉进
`[payload] trust_cache_sha256`。本轮 payload 是
`~/clover-out/round67-payload.img`（`b78575fa…`，58720256 字节）；卷内逐字节
校验：`kernel.macho` `1c8e9a40…`（与 round65/66 同一份）、ADT `94963673…`
（13476 字节，`memory-map` 恰好 `RAMDisk` + `TrustCache` 两个零占位属性、
`amfi-only-platform-code` 4 字节、`osenvironment = main`）、根镜像
`4d6da417…`、trust cache `e0a7d7ee…`；菜单标题
`clover md0 root: EL0 init parks behind its banner`。

round67 的读数就是设计里的那第一种：整屏 `[clover] el0 init: pid=1
console fd=0 pagesize=4096`，设备停住、不复位、没有 panic。三个数字各自都是
读数：`pid=1` 是启动线程 exec 之后自己变成的那个进程；`fd=0` 证明描述符表确实
是空的（open 拿到最低的空槽 0）；`pagesize=4096` 是 sysctl 从内核抄进用户页的
值，顺带证明用户栈映射可写。tty 的字画上了面板，EL0 到屏幕的输出通道闭环。

```sh
fastboot flash cache ~/clover-out/round67-payload.img
fastboot reboot
```

### v0.2.38：让 init fork 一个子进程

通道已经证明，240 遍的洪水也就花完了：从本轮起每轮只把证据印一遍然后停住，
活控制台自己把字留在屏上。下一个墙是真实用户态立刻会撞的那个——**两个用户
进程**。launchd 起来之后第一件事就是 fork 服务；fork 要复制 pmap（子进程第一
次写自己的栈就是一次 copy-on-write fault）、调度器要在父进程睡进 wait4 之后
把子进程捡起来跑、exit/wait4 还要把它收尸。这四件事一个都还没上过这块硅。

本轮的 init 因此：banner 印一遍之后 `fork()`；子进程印
`[clover] el0 child: pid=<n> ppid=<n>` 然后 `exit(7)`；父进程
`wait4(-1, &status, 0, 0)` 之后印 `[clover] el0 init: waited pid=<n>
status=<raw>`，然后停住。描述符表是 fork 复制的，子进程直接用继承来的 fd。
失败通道不变且加了两个阶段：5=`fork`（160+errno）、6=`wait4`（192+errno）；
子进程自己的写失败用阶段 7（224+errno），它的码会经父进程印出的 status 传出来。

| 现象 | 含义 |
| --- | --- |
| 三行：banner、`child: pid=2 ppid=1`、`waited pid=2 status=1792`，停住 | fork/CoW/调度/exit/reap/wait4 全通，多进程成立 |
| `status=` 是一个小数字（如 11） | 子进程被信号杀掉：CoW fault 或调度路径 fault，数字即信号 |
| `status=` 是 256 的倍数但不是 1792 | 子进程没走到 `exit(7)`：`status >> 8` 是它自己的阶段码（224+n 写被拒、32+n getpid 失败） |
| 只有 banner 行、停住、无 panic | 子进程从没被调度：父进程在 wait4 里睡死，调度/唤醒是下一面墙 |
| panic `exit status 160+n` / `192+n` | `fork` / `wait4` 被拒，errno = n |

round68 实机读数落在表倒数第二行：banner 是屏幕的**最后一行**，下面既没有
`child:` 也没有 `waited`，没有 panic、没有复位。照片里另外多出来的两行是本轮
的第一份证据：`Added memory device md0`/`BSD root: md0, major 3, minor 0` 说明
根确实挂上了，`dli_init … kernel threads … scheduled at least once. Proceeding.`
说明**内核线程**的调度是好的——但 EL0 的第二个进程是另一条路。问题是这一张图
有三种解释，而它们指向三面不同的墙：fork 在父进程的内核里就没返回；子进程建好
了却从没被调度；子进程跑了但第一次写没落地。tty 是逐字画到面板的，哪怕半行字
也算证据，而连半行都没有，所以最后一种最像、却不能只靠「没有」来定。

### v0.2.39：把 fork 那个点切成三个读数

真实用户态要的是两个进程，但 round68 那张照片还不知道缺的是哪一步，所以本轮
只改 init 的标记，内核、探针、ADT 契约、失败通道一律不动，把 fork 那个
点切成三个独立读数：

- 父进程 `fork()` 一返回就立刻印 `[clover] el0 init: forked pid=<n>`，再进
  阻塞的 `wait4`；
- 子进程的第一条指令就写一行 `[clover] el0 child: alive`，之后才去
  `getpid`/`getppid`、印它自己的 pid/ppid 并 `exit(7)`；
- 父进程 wait4 之后的 `waited pid=/status=` 留在原地。

| 现象 | 含义 |
| --- | --- |
| `forked pid=2` + `alive` + `child: pid=2 ppid=1` + `waited pid=2 status=1792` | 两个用户进程都在这块硅上跑过，多进程成立 |
| 只有 `forked pid=2`，没有任何 `alive` 的字（哪怕半个字母） | 子进程存在却从没上 CPU：次核、它的 pmap、或调度器不收它 |
| `forked pid=2` + 半行 `alive` | 子进程跑起来了，第一次写就挡住或炸掉：CoW fault 或 tty 路径 |
| banner 之后什么都没有（和 round68 一样） | 父进程没从 `fork` 回来——这是内核里 fork 自己的问题，与子进程无关 |
| `waited pid=… status=` 是个小数字 | 子进程被信号杀掉，数字即信号 |
| `status=` 是 256 的倍数但不是 1792 | 子进程没走到 `exit(7)`：`status >> 8` 是它自己的阶段码 |

两条新线共用一个控制台 tty，而这里没有写者锁，所以它们**可能逐字节交错**；读数
要的是「哪条字符串出现过」，不是排版。

构建与校验同前：根镜像 34224 字节（cdhash `c0199561…`），trust cache
`c662d138…` 已钉进 `[payload] trust_cache_sha256`。DarwinBoot 的窗口按
`ALIGN_VALUE(size, 4096)` 仍是 36864。本轮 payload 是
`~/clover-out/round69-payload.img`（`e39eddad…`，58720256 字节）；卷内逐字节
校验：`kernel.macho` `1c8e9a40…`（与 round65–68 同一份，本轮没碰内核）、ADT
`f3e31304…`（13476 字节，`memory-map` 里恰好 `RAMDisk` + `TrustCache` 两个零
占位、`amfi-only-platform-code` 4 字节、`osenvironment = main`）、根镜像
`c37ee29f…`、trust cache `c662d138…`；菜单标题
`clover md0 root: EL0 init reports fork`。round69 的读数不在上面任何一格，见下一节。

```sh
fastboot flash cache ~/clover-out/round69-payload.img
fastboot reboot
```

### v0.2.40：按 x1 分流，因为 Darwin 的 fork 不给孩子 0

round69 的读数不在表里任何一格：屏上是**两行一模一样的**
`[clover] el0 init: forked pid=2`，没有 `alive`、没有 `waited`、无 panic、停住。
`forked pid=` 只有父分支会写，它出现两次，说明**两个进程都跑了、都走了父分支**。
根因不是内核，是我们对 Darwin arm64 的 fork ABI 的猜测，而内核自己就写着答案：

```c
// osfmk/arm64/status.c
void thread_set_child(thread_t child, int pid) {
        child_state = get_user_regs(child);
        set_user_saved_state_reg(child_state, 0, pid);   // x0 = 子进程自己的 pid
        set_user_saved_state_reg(child_state, 1, 1ULL);  // x1 = 1，「我是孩子」
}
// bsd/kern/kern_fork.c:576
        retval[1] = 0;          /* flag parent return for user space */
```

也就是说 `svc` 返回时**两边 x0 都是子进程 pid**，唯一能分清父子的是 x1；把孩子的
x0 改成 0 是 libSystem `fork()` 干的活，手写 `svc` 没有这个服务。我们的
`cbnz x0, _parent` 于是把两个孩子都送进了父分支。这条链顺带把 round68 也解释了：
孩子去 `wait4(-1)`，自己没有子进程，拿到 ECHILD，落进 `_fail`——而 `_fail` 是
`b.cs` 分支进来的、结尾是 `ret`，x30 还停在最后一次 `bl` 的返回点，那里两条指令
的 carry 依然是失败的，于是**原地自旋、一个字都不写**。round68 的「只有 banner」
和 round69 的「两行 forked」是同一个错误的两张照片。

反过来看，这是这几轮里最好的消息：fork 复制了地址空间、次核把孩子调度起来、孩子
一路跑到 `wait4` 才停——**两个 EL0 进程已经在这块硅上同时活着**，屏幕上那两行重
复字就是它们各自打的。上游那份 QEMU fixture（`scripts/qemu-entry/native-user.S`）
从来没有 fork 过，所以这条 ABI 是这个移植第一次踩到。

本轮因此改三处，内核、探针、ADT 契约一概不动：

- `svc` 之后先把 `x0`/`x1` 存进 x26/x25（`mov` 不碰标志位，`b.cs` 仍能看到这次
  syscall 的错误），再用 `cmp x25, #1 / b.ne _parent` 分流；
- 两边都把自己拿到的那一对数字印出来：父进程
  `[clover] el0 init: forked pid=2 x1=0`，子进程
  `[clover] el0 child: pid=2 ppid=1 x0=2 x1=1`；
- `_fail` 结尾从 `ret` 改成 `b _exit_now`：**失败必须出声**。一个会自旋的失败
  和一个已经死掉的进程在照片上长得一样，而一个带着 `阶段×32+errno` 的 panic 不会。

| 现象 | 含义 |
| --- | --- |
| 四行：banner、`forked pid=2 x1=0`、`child: pid=2 ppid=1 x0=2 x1=1`、`waited pid=2 status=1792` | ABI 确认，且 fork/CoW/调度/exit/wait4/唤醒整条链闭合 |
| `child:` 里 `x0` 与它自己的 pid 不等 | 内核给孩子的 pid 与父亲拿到的不是一回事，回到 `thread_set_child` 查 |
| 有 `forked`、有 `child:`、没有 `waited` | 孩子 exit 了父亲没醒：wait4 的 wakeup 链是下一面墙 |
| `waited pid=2 status=` 是小数字 | 孩子被信号杀掉（数字即信号），多半是 CoW fault |
| `status=` 是 256 的倍数但不是 1792 | 孩子没走到 `exit(7)`，`status >> 8` 是它自己的阶段码 |
| 只剩 panic、退出码 `160+n`/`192+n` | `fork` / `wait4` 被拒（本轮 `_fail` 会立刻退出，不会再有沉默） |

构建与校验同前：根镜像 34368 字节（cdhash `4cae5e54…`），trust cache
`d476c073…` 已钉进 `[payload] trust_cache_sha256`，DarwinBoot 的窗口仍按
`ALIGN_VALUE(size, 4096)` 取 36864。本轮 payload 是
`~/clover-out/round70-payload.img`（`cd791908…`，58720256 字节）；卷内逐字节
校验：`kernel.macho` `1c8e9a40…`（与 round65–69 同一份）、ADT `7ecddc57…`
（13476 字节，`memory-map` 里恰好 `RAMDisk` + `TrustCache` 两个零占位、
`amfi-only-platform-code` 4 字节、`osenvironment = main`）、根镜像
`cc41e2d4…`、trust cache `d476c073…`；菜单标题
`clover md0 root: EL0 fork dispatches on x1`。

```sh
fastboot flash cache ~/clover-out/round70-payload.img
fastboot reboot
```

round70 实机读数**正是上面表格的第一行**，五行齐：

```
[clover] el0 init: pid=1 console fd=0 pagesize=4096
[clover] el0 init: forked pid=2 x1=0
[clover] el0 child: alive
[clover] el0 child: pid=2 ppid=1 x0=2 x1=1
[clover] el0 init: waited pid=2 status=1792
```

`x1` 一边 0、一边 1，把 ABI 钉死在照片上；`ppid=1` 说明孩子知道父亲是谁；
`status=1792` 是 `7 << 8`，即孩子真的走到自己那条 `exit(7)`，父亲被唤醒、收尸、
把原始 status 印了出来。fork 复制 pmap、次核调度、CoW 栈可写、exit、wait4 唤醒
——这条链在这块硅上闭合，EL0 那一侧从「一个进程能说话」升级到「进程能生进程、
能等它死」。下一轮因此问再往前一步的那件事：见 v0.2.41。

### v0.2.41：让孩子 `execve`，因为真实的 init 第一件事就是换掉自己

round70 之后「两个用户进程」这件事已经定了，下一面墙不是再多一个进程，而是
**一个进程把自己换成另一个程序**。`execve` 要过的关和 fork 完全不同：路径要
namei、镜像要从 mockfs 读出来、cdhash 要再过一次 AMFI、旧地址空间要在半路被
拆掉换成新的。真实用户的 init 开机第一件事就是它。

syscall 号从这份树的 `bsd/kern/syscalls.master:113` 抄，不是猜的：

```
59	AUE_EXECVE	ALL	{ int execve(char *fname, char **argp, char **envp) NO_SYSCALL_STUB; }
```

（经典 BSD 编号里的 10 在这份表上是 `unlink`，`__mac_execve` 是 380。）master
文件头部写着 `NO_SYSCALL_STUB` 只是「不给 libSystem 生成 stub」，`init_sysent.c`
那张表照样生成——而我们已经在真机上 retire 过的 exit(1)、fork(2)、open(5)、
wait4(7) 全都带着同一个标记。

exec 的目标选 `/sbin/launchd`，也就是 mockfs 根里**我们自己的那颗镜像**：内核
开机时 exec 的就是这个路径，所以这条路已被证明可达，而我们不需要第二个文件。
代价是 exec 成功的孩子会变成「这个程序的第二份副本」，从 `_start` 重跑一遍——
于是本轮加了一条守卫：**只有 pid 1 才 fork**，否则父子俩互相 exec 着翻倍，
屏幕只剩滚动的 banner。守卫同时也是一个读数：pid≠1 的副本停在 `_checkpoint`。

`argv`/`envp` 两个以 NULL 结尾的指针数组在运行时填进 `__DATA`，而不是写
`.quad _exec_path`：这颗镜像 `-static` 链接，数据段里放绝对地址就是要求链接器
预先知道加载地址。

| 现象 | 含义 |
| --- | --- |
| banner 出现**两次**，第二次 `pid=2 console fd=1`，其上有 `child: exec /sbin/launchd`，且没有 `waited` | exec 成功；fd 0 跨 exec 保留，所以新副本自己 open 到 1——描述符表过了边界 |
| 第二次 banner 的 fd 仍是 0 | 内核把旧表关干净了，与 libSystem 语义不符，去 exec 路径的 fd 处理查 |
| `exec errno=N` + `waited pid=2 status=2304` | 内核拒了；N 就是答案（8=ENOEXEC、2=ENOENT、13=EACCES、22=EINVAL） |
| `waited pid=2 status=` 是个小数字 | 孩子被信号打死（数字即信号）：CS/AMFI 在 exec 中途否决，不是 errno |
| 只有 `child: exec /sbin/launchd`，之后孩子再无声、父亲停在 `waited` 之前 | exec 进去了但新镜像第一条指令没跑（入口/映射问题） |
| 两行一模一样的 `pid=1` banner | 守卫没生效，pid 2 也 fork 了——本轮自己的代码问题 |

孩子原来那条 `exit(7)` 被换成 exec，失败退出码因此从 7 改成 **9**：round70 的
1792 已经有含义了，父亲不能把「exec 被拒后退出」读成旧计划里那一次成功。
内核、探针、ADT 契约一概不动，本轮仍然只换 payload 里那颗根镜像。

构建与校验同前：根镜像 34560 字节（cdhash `4d87cef1…`），trust cache
`d7ada04a…` 已钉进 `[payload] trust_cache_sha256`，DarwinBoot 的窗口仍按
`ALIGN_VALUE(size, 4096)` 取 36864。本轮 payload 是
`~/clover-out/round71-payload.img`（`2688932f…`，58720256 字节）；卷内逐字节
校验：`kernel.macho` `1c8e9a40…`（与 round65–70 同一份，探针没碰）、ADT
`5a273522…`（13476 字节，`memory-map` 里恰好 `RAMDisk` + `TrustCache` 两个零
占位、`amfi-only-platform-code` 4 字节、`osenvironment = main`）、根镜像
`1aeeffa0…`、trust cache `d7ada04a…`；菜单标题
`clover md0 root: EL0 child execs launchd`。

```sh
fastboot flash cache ~/clover-out/round71-payload.img
fastboot reboot
```

round71 实机读数**是表格第一行**，六行：

```
[clover] el0 init: pid=1 console fd=0 pagesize=4096
[clover] el0 init: forked pid=2 x1=0
[clover] el0 child: alive
[clover] el0 child: pid=2 ppid=1 x0=2 x1=1
[clover] el0 child: exec /sbin/launchd
[clover] el0 init: pid=2 console fd=1 pagesize=4096
```

最后一行是本轮的 payload：它是 **exec 之后的那个进程**印的，同一个 `_start`、
同一份代码，pid 仍是 2、`ppid` 没再印（新副本没 fork，守卫生效）。`console fd=1`
是这轮最值钱的数字——exec 保留了 fd 0，所以新副本自己 `open("/dev/console")`
才拿到下一个空槽 1：描述符表跨过了镜像边界。父亲那一行 `waited` 不在屏上，
说明孩子换了身皮之后再也没退出，父亲还睡在 `wait4` 里。

于是 namei、mockfs 读、第二次 AMFI 校验、拆旧地址空间、建新栈、把 PC 交到新
`entry` ——整条 exec 路径在这块硅上是通的。EL0 现在能做 fork、exit、wait、exec，
这四件是任何 init 的地基。下一面墙不是再换一个进程，而是**进程之间怎么说话**：
launchd 从头到尾就是一个 mach message 服务器，而我们的 pid 1 是
`bsd_utaskbootstrap` 直接造出来的、不是从某个父亲 `posix_spawn` 出来的，它的
IPC namespace 到底开没开，还没有任何证据。见 v0.2.42。

### v0.2.42：mach trap 的号是负数，而且它不写 carry

round71 证明了一个进程能换掉自己。下一件事不是再换个进程，而是**它能不能对外
说话**：launchd 从头到尾就是一个 `mach_msg` 循环，而我们这个 task 来路不正——
它是 `bsd_utaskbootstrap` 造的 pid 1 fork 出来又 exec 出来的，不是谁的
`posix_spawn` 孩子。它的 IPC space 响不响应，一点证据都没有。

先查 ABI，fork 那次的教训就是猜。这份树的 mach trap **不是**「x16 置高位」那一套
（`SYSCALL_MACH_TRAP_BASE` 在这个版本里根本不存在）：

```c
// osfmk/arm64/sleh.c  handle_svc()
	int trap_no = get_saved_state_svc_number(state);   // x16 截成 signed int
	if (trap_no < 0) { ... mach_syscall(state); }
	else               { ... unix_syscall(state, thread, p); }
// osfmk/arm64/bsd_arm64.c  mach_syscall()
	assert(call_number < 0);          /* Otherwise it would be a Unix syscall */
	call_number = -call_number;
	mach_call = (mach_call_t)mach_trap_table[call_number].mach_trap_function;
```

所以 x16 放的是**表下标的相反数**：`thread_self` = -27、`task_self` = -28、
`host_self` = -29、`mach_reply_port` = -26、`mach_msg` = -31
（`osfmk/kern/syscall_sw.c` 那张表按位置排，注释里写着 `/* 28 */`）。返回也不是
BSD 那一套：`arm_set_mach_syscall_ret` 只写 x0，**不写 x1、也从不碰 carry**，
所以 `b.cs` 这个习惯在 trap 之后读到的是上一次调用的陈旧标志——又是一个静默
bug 的形状。对被标了 `.mach_trap_returns_port` 的四条 trap，x0 就是端口名本身，
失败时是 `MACH_PORT_NULL`。

探针只在 **exec 之后那份副本**里跑（`cmp x19,#1 / b.eq _skip_mach`），pid 1 保持
收尸不动，这样万一 trap 把进程打死，父亲的 `wait4` 会替我们出声。三步，每步一行：

1. 三个 self 端口：`thread_self`/`task_self`/`host_self` —— 名字非零就说明这个
   task 的 IPC space 是活的（`ipc_task_enable` 在造出这个 task 的两条路上都调过：
   `kern_exec.c:4445`、`kern_fork.c:597`）；
2. `mach_reply_port`（0 参数）造一个只有 receive right 的端口；
3. 一个 24 字节的普通消息 `MACH_SEND_MSG` 发给它，再 `MACH_RCV_MSG|TIMEOUT`
   收回来，印出两边的 `kern_return_t` 和收回来头部里的 `msgh_size`。

`msgh_bits` 用 `MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0)` = 20，`message.h`
给这条写的注释就是「Must hold receive right」，而我们手里正是 receive right。
消息头按这份树的 LP64 定义摆：bits/size/remote/local/voucher/id 六个字段 24 字节。
收发放了 3 秒超时，因为「挂住」在这块板上和「死了」长得一样。

探针跑完 `exit(11)`。11 不是阶段码（阶段都是 32 的倍数加 errno），所以父亲印出
`status=2816` 只可能是「exec 的副本把三步走完了」。

| 现象 | 含义 |
| --- | --- |
| `thread=… task=… host=…` 三个非零、`reply=…` 非零、`send=0 rcv=0 size=24`、`waited pid=2 status=2816` | mach 全通：trap 分发、端口分配、消息往返，launchd 的地基到了 |
| 一条 mach 行都没有，父亲 `status=` 是小数字 | trap 分发本身就把进程打死（`goto bad` → `EXC_SYSCALL`），x16 的负数约定在这份内核上不是这么回事 |
| 三个名字全是 0 | trap 回来了但 IPC space 没开：查 `ipc_task_enable` 与 `is_active(space)` |
| 名字非零、`reply=0` | 端口分配失败，`ipc_port_copyout_send` 那条路（`KERN_INVALID_TASK` 会变成 NULL） |
| `send=` 非零 | 发不出去，例如 `MACH_SEND_INVALID_DEST`(0x10000003 = 268435459)：bits 或 right 类型不对 |
| `send=0 rcv=` 非零 | 发出去了但收不回来：队列或 receive right 的问题 |
| `rcv=0 size=` 不是 24 | 收回来了但内容不对，头部布局与这份 `message.h` 不一致 |

本轮仍只换 payload 里那颗根镜像：根镜像 35040 字节（`ALIGN(,4096)` 后窗口还是
36864），cdhash `d5a98805…`，trust cache `ecef35eb…` 已钉进
`[payload] trust_cache_sha256`，菜单标题 `clover md0 root: EL0 sends one mach msg`；
payload `~/clover-out/round72-payload.img` = `e3db450f…`（58720256 字节），卷内
逐字节校验 `kernel.macho` `1c8e9a40…`（与 round65–71 同一份）、根镜像
`3da8e199…`、trust cache `ecef35eb…`、ADT `91232809…`（`memory-map` 里恰好
`RAMDisk` + `TrustCache` 两个零占位、`amfi-only-platform-code` = 1、
`osenvironment = main`）。

```sh
fastboot flash cache ~/clover-out/round72-payload.img
fastboot reboot
```

### round72 实机读数：trap 全通，死在消息那一步

末五行：

```
[clover] el0 init: pid=2 console fd=1 pagesize=4096
[clover] el0 mach: thread=259 task=515 host=771
[clover] el0 mach: reply=1827
Failed to send exception EXC_CORPSE_NOTIFY. error code: 5 for pid 2
[clover] el0 init: waited pid=2 status=9
```

前三行是本轮真正的收获：**x16 的负数约定是对的**，`-27/-28/-29/-26` 四条 trap 都
分发到了，而且四个端口名全是真名字（259、515、771、1827 是这份内核分配的
`MACH_PORT` 下标），所以这个来路不正的 task **IPC space 是活的**——`ipc_task_enable`
在 fork 与 exec 两条路上都调过这件事，第一次有了实机证据。`reply=1827` 还额外证明
`mach_reply_port` 走通了 `ipc_port_alloc_reply` + `ipc_port_copyout_send`，即端口能
分配、能抄回用户页。

死亡发生在消息那一趟：`send=` 那行的字面量前缀都没印出来，父亲读到 `status=9` =
**SIGKILL**（不是 `exit(9)`，那会是 2304）。内核那行 `EXC_CORPSE_NOTIFY` 是
`osfmk/kern/task.c:2461`：进程被回收时给 exception task 发尸体通知，
`exception_triage_thread` 返回 5 = `KERN_FAILURE`——这份内核压根没设 exception 端口，
所以**每一个 EL0 进程的死都会印这一行**，它是死亡的另一条通道，不是死因。

三条静态排除，都在这份树里查过：

- 表的位置对得上：`osfmk/kern/syscall_sw.c:137` 是
  `/* 31 */ MACH_TRAP(mach_msg_trap, 7, 7, munge_wwwwwww)`，不是 `kern_invalid`；
  `goto bad` 只在越界或 `kern_invalid` 时走，而 `bad:` 结尾是
  `panic("Returned from exception_triage()?")`——内核没 panic，说明分发接住了 -31。
- 指针没有被截断：`arm_get_mach_syscall_args`（`osfmk/arm64/bsd_arm64.c:93`）对
  64 位 state 就是 `bcopy(saved_state64, dest, arg_count * sizeof(uint64_t))`，
  原样搬寄存器；那串 `munge_wwwwwww` 只在 `is_saved_state32()` 分支里用。
- 用户数据页可写：这份 exec 出来的副本早就往 `_scratch`（`__DATA`）里抄过页大小、
  也自己往 `_argv` 存过指针，两边都成功。

剩下的分歧我读不出来，因为**我在 send 和 receive 之间一次都没打印**——这是探针自己的
缺陷，不是内核的：照片无法区分「`MACH_SEND_MSG` 那次 `svc` 就没回来」和「发成功了、
死在 `MACH_RCV_MSG`」。round73 因此把沉默切成四个标记，并且在同一轮里给出答案——
死因在源码里就写着，见下一节。

### v0.2.43：legacy `mach_msg_trap` 被策略拒了，拒它的不是 IPC 层

死因是这份内核里一段与 clover 无关的兼容策略。`mach_msg` 的发送腿在拷贝任何端口
right 之前先过一道闸（`osfmk/ipc/mach_msg.c:611`）：

```c
// osfmk/ipc/ipc_policy.c:269
ipc_policy_allow_legacy_send_trap(mach_msg_id_t msgid, mach_msg_option64_t opts)
{
	if ((opts & MACH64_POLICY_HARDENED) == 0) {
#if DEVELOPMENT || DEBUG
		if (allow_legacy_mach_msg) return MACH_MSG_SUCCESS;   // boot-arg
#endif
		if (ipc_policy_allow_legacy_mach_msg_trap_for_platform(msgid)) return MACH_MSG_SUCCESS;
	}
	mach_port_guard_exception(msgid, 0, 0, kGUARD_EXC_INVALID_OPTIONS);
	return KERN_NOT_SUPPORTED;
}
```

我们这个二进制三条路都不通：不是 hardened task（`MACH64_POLICY_HARDENED` 只来自
`TFRO_PLATFORM`/`TFRO_HARDENED`/`csproc_hardened_runtime`），平台白名单要求
`p_platform` 是 iOS/Catalyst/macOS 且 SDK ≤15/12（我们是 ad-hoc 签名的手写 ELF 式
镜像，`default: return false`），`msgh_id` 白名单只有
`{0xd4a, 0xd4d, 0xe13, 0x12c4, 0x12c8}` 五个 MIG 号，而我们发的是 0。

关键在于**这道闸不是返回一个错误码**：`kGUARD_EXC_INVALID_OPTIONS = 3`
（`osfmk/mach/port.h:497`）小于 `MAX_FATAL_kGUARD_EXC_CODE = 1<<7`，所以
`mach_port_guard_exception` 无条件把这次 guard violation 标成 fatal，与
`task_exc_guard` 无关；`thread_guard_violation` 挂上 `AST_GUARD`，回到 EL0 之前
`mach_port_guard_ast`（`osfmk/ipc/mach_port.c:2510`）发现没人注册 EXC_GUARD，于是走
`exit_with_guard_exception` → `exit_with_reason(p, W_EXITCODE(0, SIGKILL), …)`
（`bsd/kern/kern_exit.c:3581`）。`W_EXITCODE(0, SIGKILL)` 就是 **9**——父亲
`wait4` 读到的那个数。这条链也解释了为什么 syscall 已经"返回"了
`KERN_NOT_SUPPORTED`、进程却在下一条用户指令退休前被杀，照片上什么都不剩。

顺带两条从源码里钉死的事实，都排除了我原先的怀疑：`mach_reply_port`
（`osfmk/kern/ipc_tt.c:1431`）用的是 `IPC_PORT_INIT_MESSAGE_QUEUE`，不带
`IPC_PORT_INIT_REPLY`，所以 `ip_is_reply_port` 为假，`ipc_right.c:2184` 那条「reply
端口不许 MAKE_SEND」根本不触发；而 `ipc_mqueue_send` 里没有任何权限检查，自发自收
也没有环检测。

修法用的是 Apple 在同一个函数里给 DEVELOPMENT 内核留的那个旋钮：
`TUNABLE(bool, allow_legacy_mach_msg, "allow_legacy_mach_msg", false)`
（`ipc_policy.c:65`，只在 `DEVELOPMENT || DEBUG` 下编译，而 clover_defconfig 选的
正是 DEVELOPMENT），所以 `kernel_args` 里加一条
`allow_legacy_mach_msg=1`——这是 v0.2.35 以来第一次改内核参数，boot-args 由
DarwinBoot 从 `unixnu.toml` 的 `args` 传给内核，`kernel.macho` 一个字节都不动。
真正的长期解是 `mach_msg2_trap`（表里 47 号，arm64 下存在，它的发送腿不过这道闸），
那是以后的轮次。

同一轮把探针的沉默切成标记（`presend` 打在内核之前、`send=` 打在内核回来之后，
`prercv`/`rcv=` 同理，外加消息缓冲区自己的地址）。这不是第二个问题，而是让这一轮
**自证**：

| 面板读数 | 含义 |
| --- | --- |
| `msg=` 地址、`presend`、`send=0`、`prercv`、`rcv=0 size=24`、父亲 `status=2816` | mach 消息往返闭环，launchd 的地基到齐 |
| `presend` 有、`send=` 没有、又是 `status=9` | boot-arg 没生效（或没被解析），闸还在杀进程 |
| `send=` 是一个 2.68 亿上下的数 | 过了闸，读的是真 `mach_msg_return_t`：`MACH_SEND_INVALID_DEST`=0x10000003、`MACH_SEND_INVALID_HEADER`=0x10000002、`MACH_SEND_INVALID_DATA`=0x10000007 |
| `send=0`、`rcv=` 是小数字 | 发出去了收不回来：`MACH_RCV_INVALID_NAME`=0x10000008、`MACH_RCV_TIMED_OUT`=0x40000000（超时那 3 秒就是为这一步准备的） |
| `rcv=0 size=` 不是 24 | 收回来了但头部布局与这份 `message.h` 不一致 |
| 一条 mach 行都没有 | 退回 round72 的形状，说明 `mach_reply_port` 之后就没再跑 |

本轮改两个文件、都不碰内核：根镜像 35216 字节（`3f5cd218…`，cdhash `8cae9556…`），
`trust_cache_sha256` 换成 `f28765e628305318cb35c0751fdb7e84bcf29b1b2838ae25db948c5f5a7e270e`，
`kernel_args` 多一条 boot-arg，菜单标题 `clover md0 root: allow legacy mach_msg`；
payload `~/clover-out/round73-payload.img` = `200c29eb…`（58720256 字节），卷内逐字节
校验 `kernel.macho` `1c8e9a40…`（与 round65–72 同一份）、根镜像 `3f5cd218…`、trust cache
`f28765e6…`、ADT `929aff15…`（`chosen/memory-map` 里恰好 `RAMDisk` + `TrustCache`、
`amfi-only-platform-code` = 1、`osenvironment = main`；ADT 每轮都带新 `random-seed`，
所以它的哈希本来就不跨轮可比），`unixnu.toml` 的 `args` 末尾确实是
`… rd=md0 allow_legacy_mach_msg=1`。

```sh
fastboot flash cache ~/clover-out/round73-payload.img
fastboot reboot
```

### round73 实机读数：boot-arg 没能救活它，而这一次连「策略没编进去」都不能当解释

照片里 mach 探针那一段是：

```
[clover] el0 mach: thread=259 task=515 host=771
[clover] el0 mach: reply=1827
[clover] el0 mach: msg=4254904160
[clover] el0 mach: presend
```

然后 `send=` 那一行还是没有，父亲的 `waited` 行照旧是 `status=9`（SIGKILL），同屏照旧有内核自己那条 `Failed to send exception EXC_CORPSE_NOTIFY`。

也就是说：`allow_legacy_mach_msg=1` 进了命令行、legacy 闸该被它打开、进程还是死在同一个地方。v0.2.43 的提交信息里那句「死在 legacy 闸」因此是一个**没有被证实**的判断，不能继续当前提写。这一轮把那条推论的每一环都查到了底，而且查的是真机里那份二进制，不是源码树：

- **闸的分支确实是活代码。** `objdump -d out/clover/kernel/kernel.macho` 落在 `_ipc_policy_allow_legacy_send_trap`（`0xfffffff0072543f8`）上，头几条指令就是这个函数的全部结构：

  ```
  tbnz x1, #0x37, <guard>            // opts 的 bit 55 = MACH64_POLICY_HARDENED
  adrp x8, 0xfffffff007158000
  ldrb w8, [x8, #0x1a8]              // _allow_legacy_mach_msg（0x...1581a8）
  tbnz w8, #0x0, 0x...254540         // 为真 -> mov w0, #0; ret（放行）
  ...
  0x...25455c: mov x0, x19; bl _ipc_policy_allow_legacy_send_trap.cold.1   // 闸调用
             mov w0, #0x2e           // = 46 = KERN_NOT_SUPPORTED
  ```

  内核是 `DEVELOPMENT_ARM64_BCM2837`（`out/clover/logs/kernel.log`），`#if DEVELOPMENT || DEBUG` 里的这行 TUNABLE 因此编译进去了；`nm` 也能看到 `_allow_legacy_mach_msg` 与它的 `___startup_TUNABLES_*` 三件套。

- **参数确实到了内核，而且走的就是这块内存。** `PE_boot_args()`（`pexpert/arm/pe_bootargs.c:11`）返回 `((boot_args *)PE_state.bootArgs)->CommandLine`；`PE_init_platform`（`pexpert/arm/pe_init.c:416`）把 booter 递进来的指针原样存下，ARM 上**没有**「设备树 `chosen/boot-args` 覆盖命令行」这条路，所以不存在「ADT 里的旧 args 赢了」这种可能。`rd=md0` 变成 `BSD root: md0, major 3, minor 0` 已经是这条通道的证据，而从 payload 里再抽出来的 `unixnu.toml` 写的是 `args = "-v serial=3 debug=0x14e kt_fixed_zones=120 boot-progress=2 rd=md0 allow_legacy_mach_msg=1"`（89 字符，DarwinBoot 的转换缓冲是 1024，不存在截断）。

- **解析器对「末尾的 `name=1`」也是走得通的。** 键名扫描在 `=` 处停下（bootargs.c:98），所以 `c == '='`、第 126 行那条「没有值」的捷径不会触发；`getval` 看到前导 `=` 就吃掉它并置 `has_value`（bootargs.c:416），`1` 解析成 NUM；`argnumcpy` 的 `case 1` 写一个 `int8_t`（bootargs.c:380）；`kernel_startup_tunable_init` 再把 `*(bool*)var = *(uint8_t*)var` 归一化（startup.c:291）。token 名 21 字符，和 `strlen(arg_string)` 一致，正是第 105 行那个 strncmp+长度判据要的。

四环都成立，进程照旧被杀。**结论只能是：round72/73 的死因未定。**「签名不带 `CS_RUNTIME`，所以不该有 HARDENED 位」这条也查过（`codesign -dvvv` = `flags=0x2(adhoc)`），同样不构成解释。继续猜下去没有意义，所以要换一个调用问。

### v0.2.44：改问 `mach_msg2_trap`，并且把 boot-arg 变成一个独立读数

`mach_msg2_trap` 是 Apple 让现代用户态用的那一个（`syscall_sw.c:154`：`/* 47 */ MACH_TRAP(mach_msg2_trap, 8, 16, munge_llllllll)`），而它在这份源码里**根本不经过 legacy 闸**：`ipc_policy_allow_legacy_send_trap` 的唯一调用点是 `mach_msg_copyin_user_header` 的 return（`osfmk/ipc/mach_msg.c:611`），那个函数整体被 `#if IPC_HAS_LEGACY_MACH_MSG_TRAP` 包着、只被 `mach_msg_overwrite_trap`（`mach_msg.c:859`）走到；`mach_msg2_trap` 是拿打包参数直接合成 `send_uctx`（`mach_msg.c:972-1010`）。

八个参数怎么打包（`struct mach_msg2_trap_args`，`osfmk/mach/mach_traps.h:465`，配合 `MACH_MSG2_SHIFT_ARGS(lo, hi) = (uint64_t)hi << 32 | (uint32_t)lo`，`message.h:1505`）：

| 寄存器 | 低 32 位 | 高 32 位 |
| --- | --- | --- |
| x0 | data（消息缓冲区） | — |
| x1 | options | — |
| x2 | `msgh_bits` | `send_size` |
| x3 | `msgh_remote_port` | `msgh_local_port` |
| x4 | `msgh_voucher_port` | `msgh_id` |
| x5 | `desc_count` | `rcv_name` |
| x6 | `rcv_size` | `priority` |
| x7 | timeout | — |

msg2 也有自己的闸，而且**两道都是静默击杀**：`ipc_preflight_msg_option64`（`ipc_policy.c:144`）要求带 `MACH_SEND_MSG` 的 msg2 调用里 CFI 掩码**恰好一位**（非 macOS 还会先抹掉 `MACH64_SEND_ANY`），`ipc_validate_kmsg_dest_from_user`（`ipc_policy.c:686`）要求这一位和目标端口的类型匹配 —— reply 端口是 message queue，所以必须是 `MACH64_SEND_MQ_CALL`（`4 << 32`）。两条失败路径都调 `mach_port_guard_exception(..., kGUARD_EXC_INVALID_OPTIONS)`，而 3 < `MAX_FATAL_kGUARD_EXC_CODE`（`osfmk/mach/port.h:526`）。这反过来给了本轮一个很好用的性质：**面板上只要出现 `m2: send=<数字>`，就等于已经过了所有策略层**，因为过不去的时候一个字都不会多印。

因此这一轮的顺序是：msg2 发送 → msg2 接收（`MACH_RCV_MSG | MACH_RCV_TIMEOUT`，纯接收不需要 CFI 位）→ **最后**才把 round73 那个 legacy `mach_msg` 原样重放一次。放最后是刻意的：legacy 调用有可能又被杀，而那时 msg2 的读数已经全部在屏上了。

寄存器也跟着动：缓冲区指针从 x7 挪到 **x20**，因为 x7 现在是 msg2 的第八个实参；`_write_console`（x0/x16/x22）和 `_print_dec`（x27-x29、x9-x11）都不碰 x20/x21/x24-x26，x19 仍留给 pid。顺带把重复了十几遍的「打印 label + 数字 + 换行 + 判 carry」收成 `_puts` / `_print_line` 两个小助手，表头六次写死在内存里的 `str` 序列收成 `_mheader`（msg2 的表头来自寄存器、不来自内存，但 legacy 那一路要读内存，所以重放前必须重新填一遍）。

产物（`scripts/verify-clover-payload.py` 逐文件从卷里取出、逐个哈希；内核槽不能做整文件等值，
因为 payload 构建会把 board.toml 的入口探针写进卷内那份副本，脚本改判「差异是否局限在少量
小段」：本轮 5626 字节、35 段，卷内 `kernel.macho` 仍是 `1c8e9a40…`，与 round65–73 同一份）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round74-payload.img` | 58720256 B | `0eb9f0836403a02cfc9aa84d759b8a19e26e47501c7a21d61998275700236c75` |
| `round74-root.img`（launchd.macho） | 35408 B | `37dc0da218ced2466ad8020d438be3b3c774daec92d9feda4b44ebf5049142aa`，cdhash `c04c72719c471a231b465ac88ecbb580eab900da` |
| `round74-trustcache.bundle` | 56 B | `37066666713620155505f9d561d9af8c52f5d6025feb56db3c08f39537ef3f24` |
| `round74-adt.bin` | 13476 B | `0cfd072cb348dcd6413f24f08c86ef02088b47aa8967cde3f18b896f0911da65`（每轮 `random-seed` 都换，不跨轮可比） |

`unixnu.toml` 的 `args` 仍是 `-v serial=3 debug=0x14e kt_fixed_zones=120 boot-progress=2
rd=md0 allow_legacy_mach_msg=1`，本轮没动 boot-arg，它就是留给 legacy 那一路的对照变量。

读数判定（只看面板末尾）：

| 看到 | 结论 |
| --- | --- |
| `m2: presend` 是最后一行 | msg2 也被击杀 ⇒ 问题不在 legacy 闸，而在这个 task 发 mach 消息这件事本身 |
| `m2: send=0` | **EL0 已经能把消息投进自己端口的队列**，legacy 那一路的死活都不影响这一步 |
| `m2: send=268435475` | `MACH_SEND_INVALID_OPTIONS` ⇒ CFI 位还是不对（不过这条会先杀，能印出来说明判据有变） |
| `m2: send=268435459` | `MACH_SEND_INVALID_DEST` ⇒ 过了策略，端口名没解析成权利 |
| `m2: rcv=0 size=24` | 一发一收闭环，initproc 需要的 mach 循环成立 |
| `m2: rcv=268451843` | `MACH_RCV_TIMED_OUT` ⇒ 消息发到了别的队列 |
| `legacy: send=<数字>` | boot-arg **是生效的**，round73 死于别处 |
| `legacy: presend` 之后没了 | boot-arg **没生效**（或闸后面还有东西），且此时 msg2 的读数仍在屏上 |
| 父亲 `status=2816` | 探针跑到 `exit(11)`，全绿 |

刷机：

```sh
fastboot flash cache ~/clover-out/round74-payload.img
fastboot reboot
```

### round74 实机读数：面板停在 `msg=` 的半行上，而且这次连「退出」都没发生

`v0.2.44` 设计的读数一个都没出现。屏上最后九行是：

```
[clover] el0 init: pid=1 console fd=0 pagesize=4096
[clover] el0 init: forked pid=2 x1=0
[clover] el0 child: alive
[clover] el0 child: pid=2 ppid=1 x0=2 x1=1
[clover] el0 child: exec /sbin/launchd
[clover] el0 init: pid=2 console fd=1 pagesize=4096
[clover] el0 mach: thread=259 task=515 host=771
[clover] el0 mach: reply=1827
[clover] el0 mach: msg=▮
```

光标块就停在 `=` 后面，地址的数字没出来，`m2: presend` 没有，`m2: send=` 没有，
`Failed to send exception EXC_CORPSE_NOTIFY` 没有，父亲的 `waited pid=2 status=…`
也没有；拍完之后屏幕一直这样，不自复位。

四个「没有」里最关键的是最后一个。round72/73 已经证明：这份内核上任何 EL0 进程退出，
`task.c:2461` 都会印那行 corpse notify（因为没人设 exception 端口），而父亲正睡在
`wait4` 上、一定会被唤醒并印字。两行都不在 ⇒ **pid 2 没有退出**。它要么卡在
携带数字的那次 `write` 里面，要么还在跑，而这两种情况在一张静态照片上是同一个样子。

于是这一轮比 round73 退了一步：`m2` 的 `svc` 根本没执行到，msg2 的三道策略闸一条都没测到，
`allow_legacy_mach_msg` 那个对照变量也没轮到发言。也就是说 round74 没有回答它自己提出的问题，
但它把「这台板子上 EL0 的 mach 探针为什么会停」这个问题换成了另一个：**停在半行里的，
到底是进程、是 tty 写路径、还是两者一起**。

代码侧先自查过，结论是没有可归罪的缺陷：`objdump build/clover-root/launchd.macho`
对到源码逐条一致 —— `1000005f4 bl _mheader`、`10000060c bl _puts`、`100000610 mov x0, x20`、
`100000614 bl _print_line`，`_print_dec` 的 `udiv/msub/cbnz` 循环必然终止，
`_write_console` 只用 x0/x16 读 x22，`_print_dec` 只用 x27-x29 与 x9-x11，
x19/x20/x21/x22/x25 全程不动；而同一套 `_print_dec` 在**同一屏上**刚印完
`259 / 515 / 771 / 1827`。所以这不是探针自己写坏，是真有个东西停住了。

### v0.2.45：父亲不再睡在 `wait4` 里，用轮询把「挂住 / 死了 / 控制台废了」切成三个读数

孩子那一路**一条指令都不改**（除了 `.data` 里多两个字符串导致地址整体位移），改的只有父亲：
把 `wait4(-1, &scratch, 0, 0)` 换成 `wait4(child_pid, &scratch, WNOHANG, 0)` 的轮询循环，
每轮之间用 `subs x21, x21, #1 / b.ne` 烧掉大约几十毫秒（`mov x21, #0x4000000`，
选 0x4000000 而不是十进制一亿是因为 MOVZ 只能把 16 位字段放在 16 的倍数位上），
前 10 轮各印一行 `[clover] parent: poll <n>`，之后闭嘴但**继续轮询**——
所以一个在第 400 轮才死掉的孩子仍然会被报出来。

为什么用计数循环而不是睡眠：这份内核里能用的等待都要么带 Mach 权利，要么带结构体。
`__semwait_signal(0, 0, 1, 1, sec, nsec)`（libSystem 的 `nanosleep` 形状）在这里**不是延时**——
`port_name_to_semaphore`（`osfmk/kern/sync_sema.c:1168`）对 `!MACH_PORT_VALID(name)` 直接
返回 `KERN_INVALID_NAME`，于是 `semaphore_timedwait_trap_internal` 连等待都不进就回来，
`__semwait_signal_nocancel` 落到 `return EINVAL`（`bsd/kern/kern_sig.c:1090`）。
`select(0, NULL, NULL, NULL, &tv)` 要内核 copyin 一个 `timeval`，`mach_wait_until`（表 90）
要一个绝对 tick 期限而这份表里没有能读当前 tick 的 trap。计数循环什么都不需要，
代价只是「慢」和「卡」之间用估计值而不是时钟分开。

三个读数互斥且穷尽：

| 看到 | 结论 |
| --- | --- |
| `parent: poll 1…10` 一直出、没有 `waited` | **孩子挂在里面**，控制台与父亲都好；mach 不是屏幕停住的原因 |
| `waited pid=2 status=N` | 孩子死了；`N & 0x7f` 是信号（9 = SIGKILL），`N >> 8` 是退出码（11 = 探针跑完） |
| `parent: wait4 error <数字>` | 父亲活着、控制台活着，而内核说它没有孩子可等 |
| poll 行和孩子最后一行同一瞬间停 | **tty 写路径自己死了**，round72-74 关于 mach 的所有结论作废，嫌疑转到内核 |

产物（`scripts/verify-clover-payload.py`）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round75-payload.img` | 58720256 B | `f7e46e319bdec9cc30ba73c54e2196c1503f7a915a9711c4bfb39498a43fd23e` |
| `round75-root.img`（launchd.macho） | 35632 B | `0c807470a6839b8f41b4d01791691e2da6059361e35823488d7141d0741b8eff`，cdhash `805d5519256e555c987a8e7c33122cf370fae758` |
| `round75-trustcache.bundle` | 56 B | `57f7fc5485d889f448e1140ca6f7d168acdc5b3b3f94c79c598d241e368187f3` |
| `round75-adt.bin` | 13476 B | `6c7f1e0965f70018dcb72b84d675fe4bd2a267d116c6d2f8d37ebed9a7121674`（每轮 `random-seed` 都换） |

卷内 `kernel.macho` 仍是 `1c8e9a40…`，探针补丁依旧只占 5626 字节 / 35 段；
boot-args 一个字没动。菜单标题 `clover md0 root: is the mach probe hung or dead`。

刷机：

```sh
fastboot flash cache ~/clover-out/round75-payload.img
fastboot reboot
```

### round75 实机读数：父亲卡在 `write` 里面，孩子连第一个词都没写出来

面板末尾（照片里从内核日志接上 EL0 的那一段）：

```
[clover] vfs_mount returned
[clover] devfs_mount returned
[clover] signit returned, execing init
VM Swap Subsystem is ON
No POST boot-arg set.
load_init_program: attempting to load /usr/appleinternal/sbin/launchd.development
load_init_program: attempting to load /sbin/launchd
[clover] el0 init: pid=1 console fd=0 pagesize=4096
[clover] el0 init: forked pid=2 x1=0
[clover] parent: poll ▮
```

光标块停在 `poll ` 后面，数字没出来。这一行的信息量比它看起来大得多，因为**这次卡在
半行里的是父亲**，而父亲是那个我们已知能把整行印完的进程（round70-73 里它印过
`waited pid=2 status=1792` 和 `status=9`，数字都出来了）。

三条判据把可能性收窄：

- **不是 write 报错。** `_print_line` 之后紧跟 `mov x1, #224 / b.cs _fail`，write 只要返回
  errno 就会走 `_fail` → `exit()` → initproc 退出 → panic 屏。照片上半屏是完整的内核日志，
  没有被 panic hook 清掉，也没有白字 ⇒ **那次 write 没有返回**。
- **孩子一个字节都没写出来。** `[clover] el0 child: alive` 是 fork 之后子分支的**第一条指令**，
  一次写完、自带换行。它不在屏上，`el0 init: pid=2 console fd=1` 也不在，`m2:` 更不在。
  而 round72-74 里孩子正是在这里连着印了四五个端口名。
- **形状和 round74 一模一样。** 那一轮停在 `[clover] el0 mach: msg=`，同样是「标签的 write
  成功、紧接着那一次 write 再也没回来」。两轮、两个不同进程、同一个位置。

于是这一轮真正被排除掉的是「mach」这个嫌疑本身：**round74 与 round75 之间唯一的变化，
是父亲从睡在 `wait4` 里变成了「一边跑一边往控制台写字」**。也就是说，屏幕冻住的充分条件
可能只是**有两个 EL0 进程同时写控制台**，而这台板子上 mach 那一路从头到尾没被真正读到过。

源码里能看到这把锁：EL0 的每一个字符最后都落到 `vcputc_options`
（`osfmk/console/video_console.c:1231`），它在 `hide_cursor / gc_putchar / show_cursor`
外面套了一把 `lck_ticket_t`（`video_console.c:153` 的 `VCPUTC_LOCK`）。ticket lock 是排队自旋，
**拿着不放就等于后面所有人永久等待，这条路上没有任何超时**。同一个函数还有第二种失声方式：
`gc_enabled` 为假时它**静默丢弃**字符，write 照样成功返回——这两种情况在一张照片上完全一样。

### v0.2.46：父亲一个字都不写，超时用 panic 屏说话

孩子那一路仍然**一条指令不改**。改的只有父亲：`wait4(child, WNOHANG)` 的轮询保留，
但轮询期间**不写任何字符**（`[clover] parent: poll ` 这个字符串已经从 `.data` 里删掉），
200 轮之后不再打印，而是 `mov x23, #384 / b _exit_now` —— **用 `exit(384)` 说话**。

为什么超时走 exit 而不是走打印：initproc 退出会进 `proc_handle_critical_exit` → `panic()`，
而这份内核的 panic 是被补丁重定向到 `scripts/xnu-panic-screen.c` 的，它用自己的 printf
把字形**直接画进 framebuffer**，全程不调用 `vcputc`。也就是说，这是这台板子上唯一一条
不经过「正在被测的那个控制台」的通道。384 = 12 × 32 且 errno 0，而这份探针里没有 stage 12，
所以 `exit status 384` 只可能是「父亲轮询到没耐心了，而孩子一直没死」。

objdump 复核过新循环，轮询体内除 `svc #0x80`（wait4）之外没有任何 `svc`：

```
 6d0: svc   #0x80
 6d4: b.hs  _wait_error
 6d8: cbnz  x0, _wait_reaped
 6dc: cmp   x20, #0xc8          // 200
 6e0: b.lo  _parent_poll
 6e4: mov   x23, #0x180         // 384
 6e8: b     _exit_now
```

四个读数，互斥且穷尽（**拍之前等满两分钟**，让 200 轮跑完）：

| 看到 | 结论 |
| --- | --- |
| 孩子的行全在，末尾 `[clover] el0 init: waited pid=2 status=N` | 冻屏是**两个写者**造成的；mach 的读数要按 `N` 重判（2816 = 探针跑到 `exit(11)`，9 = SIGKILL） |
| 孩子停在某半行，两分钟后上半屏变成 panic 屏、写着 `exit status 384` | 父亲活着、面板还画得出来 ⇒ 控制台没问题，**孩子是自己卡住的**，而且此刻根本没有第二个写者 |
| 孩子停在某半行，且永远没有 panic 屏 | 父亲也没能走到 `exit` ⇒ 它堵在一个被要求不要阻塞的 `wait4` 里，或者 ticket lock 真被孩子拿着、连 panic 通道都抢不回像素 |
| 孩子的行全在、也没有 `waited`、也没有 panic | 只有一个写者时控制台又能画了，但孩子仍挂着 ⇒ 嫌疑回到孩子进入的那次 trap 本身 |

产物（`scripts/verify-clover-payload.py` 逐文件校验通过）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round76-payload.img` | 58720256 B | `1709a6d2d8db207691971fec10cdf9a5a221106bf50033309f5ab5376a603674` |
| `round76-root.img`（launchd.macho） | 35568 B | `1ca5fdbf6b32eb5124beb665028175e23022a703e528eb25a7cf517805441aff`，cdhash `c4a8b57dc6abbf404cab68bff9566ac60d4f41ec` |
| `round76-trustcache.bundle` | 56 B | `197a08e136c4a31557cab24e2f13b0b7841f1a394cfa1e7c18c4f6d5f8df749a` |
| `round76-adt.bin` | 13476 B | `292b631a23648c79b0852aa21239bf5c73711aae782534f85d63836f04e62e49`（每轮 `random-seed` 都换） |

卷内 `unixnu.toml` 485 B `af5d2c1cb8d22e9c…`，boot-args 依旧
`-v serial=3 debug=0x14e kt_fixed_zones=120 boot-progress=2 rd=md0 allow_legacy_mach_msg=1`
——这一轮**没有动任何内核侧的变量**，因为要测的就是内核的控制台。卷内 `kernel.macho`
仍是打过入口探针的 `1c8e9a40…`（5626 字节 / 35 段）。菜单标题
`clover md0 root: one writer, panic-screen exit`。

刷机：

```sh
fastboot flash cache ~/clover-out/round76-payload.img
fastboot reboot
```

## 有意省略的部分

- **没有块设备驱动。** XNU 有 UFS 的 `NativeUfs`，但没有 eMMC/SDHCI 的，所以
  sheng 那条"直接从 userdata 读 APFS 卷"的路走不通。clover 走 gta4l 的形状：
  控制台优先的内核，带 `CONFIG_XNU_SOURCE_MOCKFS`，没有
  `CONFIG_XNU_NATIVE_UFS`，`[userdata]` 留在板级文件里，等哪天有 SDHCI 驱动。
  根设备因此不走磁盘：`v0.2.35` 用 `rd=md0` 把 payload 里的 initrd 变成内存盘，
  再由 mockfs 把它当作整个 `/`，`root-matching` 那条 UUID 匹配表还留在
  `/chosen` 里（`rd=md0` 会在等待循环之前就 `goto iofrootx`，所以它不参与）。
- **没有 GPU。** `NativeAdreno` 面向 Adreno 740；这块板子是 Adreno 512。
  所以显示栈缺席，`scripts/build-clover-display-stub.py` 只提供
  DEVELOPMENT-only XNU 补丁无条件调用的那一个空符号。首次启动的目标是 UEFI
  菜单和 XNU 控制台，不是 framebuffer。
- **没有触摸。** `edt-ft5406` 没有 UniXNU 驱动，USB 又只有 device 模式，
  所以在有人补上之前，菜单输入只能走调试串口（`blsp1_uart2`，115200）。
- **没有 Data 卷。** `[payload.adt]` 现在是 `packed_data_ramdisk = false`：本机没有
  `NativeRamBlock`，`--data-ramdisk` 后面那颗镜像不是内核要写的 Data 种子，而是
  `md0`/mockfs 的根（`scripts/make-clover-root.py`）。`/chosen` 因此保持普通
  main boot，不加 `ephemeral-storage`，也不加 `PackedRAMDisk` 槽。owner bundle
  由 `scripts/make-clover-root.py` 自己生成，`trust_cache_sha256` 钉住它。

## 许可证

BSD-2-Clause-Patent，与 UniXNU 一致。`scripts/build-arm-driver.py` 移植自
UniXNU-gta4l（NoNameBuilder），采用同一许可证。
