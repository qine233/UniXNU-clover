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
| 真机启动 | **菜单已到**（Stage 1）：`v0.3.10` 固件在屏幕上渲染 UniXNU 菜单并读到了 payload；菜单 3 秒后默认进 XNU。`v0.2.9` 实测屏幕出现**绿 + 蓝 + 品红**三条色带，即 DarwinBoot 已交接、XNU 入口已执行、引导页表已建好；`v0.2.10` 实测**品红出现约 6 秒后设备自己重启**，即 `sctlr_el1` 打开 MMU 后没有立刻死掉；`v0.2.11` 的 `_arm_vm_init` 复位点没有触发（**品红一直停着**），说明还没走到它；`v0.2.12` 实测**只有绿 + 蓝 + 品红、约 6 秒后自复位**，即 `_PE_init_platform` 返回了、开 MMU 后写屏确实不可用；`v0.2.13` 的 `_thread_bootstrap` 复位点实测是**品红后十几秒才复位、没有白字**，符合"panic 屏幕在 `vinfo` 仍为 0 时走了 12 秒兜底"；`v0.2.14` 扩映射并预填 `_vinfo`、不再主动复位，实测**仍是绿 + 蓝 + 品红，无黄带无白字**，由此定位到窗口在 framebuffer 之上、向上扩 `memSize` 永远够不到；`v0.2.15` 改为直接补页表项，实测**仍是绿 + 蓝 + 品红、无黄无白**，原因是 1 GiB 窗口只能落在高 DRAM bank，与 framebuffer 不在同一个 1 GB 槽，旧代码查错了槽位就退出；`v0.2.16` 换成独立 mapper 洞穴（同槽复用、异槽新建 L2 表），失败时用白色带自报错误码，实测**绿 + 蓝 + 品红 + 第 9 条白带**，即 mapper 走到自检第 4 步：窗口的第一个 L2 项不是 2 MB 块；由此查明 `boot_args.physBase` 锚在 `__PRELINK_TEXT` 上、本身不在引导映射内；`v0.2.17` 改为从 mapper 自己的 PC 反推引导映射，修掉复用 L2 表时未掩掉低位的 bug，并在 MMU 打开后立刻涂第 4 条青色带，实测**绿 + 蓝 + 品红 + 青、无黄**，即 MMU 已开、framebuffer 已经可写，卡点在 `_cpu_data_init` 被调用之前；`v0.2.18` 增加第 12 条橙色带（`arm_init` 入口后的第一条调用，不依赖 vinfo/栈/AT），实测**绿 + 蓝 + 品红 + 青 + 橙、无黄**，确认 `arm_init` 已进入、第一条调用点已到达；由此回查代码发现种子守卫从 `v0.2.15` 起就是反的（`cbz x8` 在 mapper 成功时跳过种子），`_vinfo` 一直为 0，黄带从未有机会出现；`v0.2.19` 改成 `cbnz x8`，并让第一条调用返回后再涂第 13 条红带，实测读到**白字 panic 屏幕**：`CPU property 'cluster-type' has bad size 6 @machine_routines.c:936`，即种子/画屏/早段 `arm_init` 全通，内核已走到 `ml_parse_cpu_topology`；`v0.2.20` 把 `cluster-type` 从字符串改成 4 字节整数 `'E'` / `'P'`，实测接着触发 `machine_routines.c:1099` 的 `phys_cluster_id <= MAX_CPU_CLUSTER_PHY_ID` 断言——这份 BCM2837 内核只支持单簇，无法接受 E/P 双簇描述；`v0.2.21` 改为不发布 `cluster-type`，让 8 核按 `CLUSTER_TYPE_SMP` 建一个平坦簇，实测拓扑已通过，接着读到 `Undefined kernel instruction: pc=0xfffffff0079aff58 instr=d500419f`——`msr pan, #1`，Kryo 260 是 ARMv8.0，没有 PAN；`v0.2.22` 把 `CONFIG_XNU_NATIVE_PAN` 改成 `n` 并重建内核，实测七条带全部出现（绿 + 蓝 + 品红 + 黄 + 青 + 橙 + 红）、没有白字：内核进了 `arm_init`，两个早段探针都返回了，之后死在 `bl _arm_vm_init` 处或更晚，而且没有可见的 panic；`v0.2.23` 因此把信号换回定时复位，并让 panic 屏幕在 framebuffer 不可翻译时也能报出来，实测**色带出现后约 10 秒自复位**，即 `arm_vm_init` 返回了，卡点在它后面；`v0.2.24` 把复位点移到 `PE_create_console` 之后的第二次 `PE_init_platform(TRUE)`，实测**色带之后什么都没有**——既没有文字，也没有 15 秒复位，卡点在 `arm_vm_init` 返回之后、`PE_init_platform` 之前；`v0.2.25` 据此跳过 `PE_create_console` 开头读树莓派 panic log 地址的四条指令，实测**色带 + 约 15 秒复位、无文字**，即控制台与第二次 `PE_init_platform` 都已返回（无文字是 `gc_enabled` 仍为假所致，不是故障）；`v0.2.26` 给随后五个调用各涂一条带（6–10）并把复位点推到 `_machine_startup` 之前；实测**绿 + 蓝 + 白字**（`<1>:0 Assertion failed: <d>`、`TCR=0x…1a251a`、`pages=free=0`），即 `arm_init` 后段有断言 panic、发生在 VM 页初始化之前，但旧清屏把第 2 条以下的带全擦掉、参数又与已知冷段常量对不上；`v0.2.27` 因此改印 RA/FP/LR 与原始参数槽；其实机读数（绿 + 蓝 + 白字、`LR=0xfffffff0073f1dc4`、`a0=1 a1=0 a2=0xd`）指向的不是内核断言，而是第 14 条刻度洞穴自己：它被放在 `0x3ed784`（4 mod 8），`ld.lld` 把 `_start` 对齐到 `0x3ed788`，而 patcher 仍按 `0x3ed784` 写入，整段 `bl` 左移 4 字节落到 `_fiq_context_init - 4`（`_platform_syscall` 冷段里的 `bl _Assert`），`x0/x1/x2` 还留着 `arm_init` 传给 `_fiq_context_init` 的 `TRUE/0/0xd`；`v0.2.28` 修掉打洞对齐并逐条校验 `bl` 目标，第 10–13 条带则证明那四个调用确实都返回了；`v0.2.28` 上机读数是**10–14 全在 + 15 红 + 约 15 秒复位**，即 `_fiq_context_init` 也返回了、`arm_init` 已到 `_machine_startup` 调用点，交接之前没有任何卡死；`v0.2.29` 因此让检查点真正进入 `_machine_startup`，把刻度推进到 `kernel_bootstrap` 的 `vm_mem_bootstrap` 调用处；`v0.2.29` 上机读数是**整条梯子全亮 + 约 15 秒复位**（逐带取中位色：10 白、11 橙、12 绿、13 青、14 品红、15 红、6 白），即 `_machine_conf`、首条 `printf` 都返回了，`vm_mem_bootstrap` 已被调用，卡点在它内部；`v0.2.30` 因此把第 10–14 条带改成 `vm_mem_bootstrap` 内五个调用的探针、复位后移到 `kmem_init` 之后；`v0.2.30` 上机读数是**十六条带全在、含第 5 条红带、约 15 秒复位**，即 `vm_page_bootstrap` 到 `kmem_init` 五个调用全部返回、复位档也到达，卡点在 `vm_mem_bootstrap` 后段；`v0.2.31` 因此摘掉这五个已证调用，第 5 条红带保留为前半段收尾，第 10–14 条带改标后段四个调用；`v0.2.31` 上机读数是**13、14 都在 + 约 15 秒复位**，即最后那条 `kprintf` 返回、整个 `vm_mem_bootstrap` 返回，卡点在 `kernel_bootstrap` 更后面；`v0.2.32` 因此不再加刻度，而是在 `vm_mem_bootstrap` 返回后调用 `_vcattach` 接上视频控制台；`v0.2.32` 上机读数是一块**白字 panic**：`"AMFI: no amfi-only-platform-code device tree node!" @AppleMobileFileIntegrity.cpp:5025`，RA/LR 都落在预链接的 AMFI kext 里，说明内核根本没停在 `vm_mem_bootstrap` 之后，而是一路走到 AMFI；色带还在是因为 `vcattach` 被 `gc_graphics_boot == TRUE` 短路成了空操作；`v0.2.33` 因此补上 ADT 的 `amfi-only-platform-code` 属性，并先用 `kPETextMode` 清掉图形模式标志再 attach；`v0.2.33` 上机读数：**文字控制台已接上**，满屏内核日志（AMFI、Sandbox、apfs、AppleImage4 都走过），最后停在 NVRAM sync 失败那一行、**没有 panic**；但那一行可能来自后台 kext 线程，不能当作启动线程的卡点；`v0.2.34` 因此在 `_bsd_init` 的九个调用返回后各印一行 `[clover]` 标记；`v0.2.34` 上机读数是**九行标记全部印到 `[clover] net_init_run returned`，`vfs_mountroot returned` 没有**，最后一行是 `Waiting on <dict … IOProviderClass IOMedia … IOPropertyMatch … UUID …>`，即启动线程正常停在 `IOFindBSDRoot`，等的根 `IOMedia` 在这块没有驱动的 eMMC 板上永远不会被发布——不是卡死，是这条路不通；`v0.2.35` 因此把根换成 `rd=md0` + mockfs，并让 init 自己 `exit(0)`，用 `initproc exited` 这条 panic 证明 EL0 跑到了；`v0.2.35` 上机读数是 **`initproc failed to start -- no exit reason available -- (signal 0, exit status 0)`、没有 `CS_KILLED`**，也就是 pid 1 已经建好、已经跑进 EL0、四个 syscall 都返回、最后自己 `exit(0)`——Stage 6 达成（exec 真失败会报 `Process 1 exec of … failed, errno N`）。缺的那行 banner 不是 bug：这份 XNU 没有 `open_console()`，initproc 的 fd 0/1/2 是空的，`v0.2.36` 因此让 init 自己 `open("/dev/console")`；round66 实机读数 **`exit status 0`** = `getpid`/`sysctl`/`open`/`write` 四步全通，EL0 到控制台 tty 的 syscall 通道闭环，banner 没进照片是因为 panic 钩子清屏且 tty 输出不进 msgbuf；`v0.2.37` 因此成功不退出：banner 写 240 遍后停住，活控制台当读数；round67 实机读数**整屏 `[clover] el0 init: pid=1 console fd=0 pagesize=4096`、停住不复位** = tty 的字画上了面板，EL0→屏幕闭环（`fd=0` 证明描述符表空、`pagesize=4096` 证明用户栈可写）；`v0.2.38` 因此改问多进程：fork/子进程 exit(7)/父进程 wait4；round68 实机读数是**banner 之后什么都没有**——没有 `child:`、没有 `waited`、无 panic 无复位，而同屏的 `Added memory device md0`/`BSD root: md0` 与 `dli_init … scheduled at least once` 说明根挂上了、内核线程调度是好的，这一张图却有三种解释（fork 没返回 / 子进程没被调度 / 子进程第一次写没落地），`v0.2.39` 因此把 fork 点切成三个标记：父进程一返回就印 `forked pid=`、子进程第一条指令就印 `alive`；round69 实机读数是**两行一模一样的 `forked pid=2`**、没有 `alive`、没有 `waited`，也就是两个进程都跑了、都走了父分支——根因是 Darwin arm64 的 fork ABI：`thread_set_child`（`osfmk/arm64/status.c`）给孩子的是 `x0=自己的 pid, x1=1`，父亲是 `x0=同一个 pid, x1=0`（`kern_fork.c:576` 的 `retval[1] = 0`），把孩子的 x0 归零是 libSystem 的事，手写 `svc` 没有；而孩子随后 `wait4(-1)` 拿到 ECHILD、掉进 `_fail`，`_fail` 是 `b.cs` 进来的、结尾 `ret` 回到陈旧 x30，于是原地自旋一个字都不写——round68 的「只有 banner」是同一个错误的另一张照片。`v0.2.40` 因此按 `x1` 分流、两边都印出自己拿到的 `x0/x1`、并把 `_fail` 改成直接退出（失败必须出声）；round70 实机读数**五行齐**：`forked pid=2 x1=0`、`child: alive`、`child: pid=2 ppid=1 x0=2 x1=1`、`waited pid=2 status=1792`，即 fork 复制 pmap、次核调度孩子、孩子 CoW 栈可写、`exit(7)`、父亲 `wait4` 被唤醒收尸，整条多进程链在这块硅上闭合（`1792 = 7 << 8`）；`v0.2.41` 因此把孩子的 `exit(7)` 换成 `execve`，问下一面墙——一个进程能不能把自己换成另一个程序；round71 实机读到六行、末行 `[clover] el0 init: pid=2 console fd=1 pagesize=4096`，即 exec 成功、fd 0 跨 exec 保留（新副本 open 拿到 1）、父亲再无 `waited` 行——namei、mockfs 读、第二次 AMFI、换地址空间整条 exec 路径已通（v0.2.42 问 mach）；round72 实机读到 `[clover] el0 mach: thread=259 task=515 host=771`、`[clover] el0 mach: reply=1827`——x16 的负数约定是对的，四条 trap 都分发成功，而且这个 fork+exec 出来的 task **IPC space 是活的**（端口名全是内核真分配的），`mach_reply_port` 还证明了端口能分配并抄回用户页；但 `send=` 行的字面量都没出现，父亲读到 `status=9` = **SIGKILL**，死在消息那一趟，同屏多出 `Failed to send exception EXC_CORPSE_NOTIFY. error code: 5 for pid 2`（`task.c:2461`：这份内核没设 exception 端口，所以每个 EL0 进程死时都会印这行，它是死亡的另一条通道而非死因）。源码侧已排除三件事：`syscall_sw.c:137` 的 31 号确实是 `mach_msg_trap`（不是 `kern_invalid`，而 `goto bad` 的结尾是 panic，内核没 panic）、`arm_get_mach_syscall_args` 对 64 位 state 原样 `bcopy` 寄存器（`munge_wwwwwww` 只在 32 位分支用，指针没被截断）、这份副本写 `__DATA` 是成功的。剩下的分歧我读不出来，因为探针在 send 与 receive 之间一次都没打印——`v0.2.43` 因此把这两次 `svc` 各包上「调用前标记 + 调用后立即打印」，并顺手印出消息缓冲区的地址，一次刷机把「死在哪一次调用」切成独立读数；round73 实机读数是 `[clover] el0 mach: msg=4254904160`、`[clover] el0 mach: presend`，然后**还是没有 `send=`**，父亲又读到 `status=9` —— boot-arg 没能救活 legacy `mach_msg`。而这一次不能再拿「策略没编进去」当解释：`objdump out/clover/kernel/kernel.macho` 里 `_ipc_policy_allow_legacy_send_trap+0x20` 确实是先测 `MACH64_POLICY_HARDENED`（bit 55）、再 `ldrb` 读 `_allow_legacy_mach_msg`（`0xfffffff007158000+0x1a8`）、`tbnz` 为真就直接 `mov w0, #0` 返回放行，说明 DEVELOPMENT 分支是活代码；payload 里 `unixnu.toml` 的 args 末尾确实是 `allow_legacy_mach_msg=1`，而 `PE_boot_args()`（`pexpert/arm/pe_bootargs.c:11`）返回的就是 DarwinBoot 填的那块 `CommandLine`（`PE_init_platform` 直接取 booter 递进来的指针，ARM 上不看设备树的 `chosen/boot-args`），`rd=md0` 变成 `BSD root: md0` 已经证明这条通道；解析器对尾 token 也走得通（键名扫描停在 `=`、`getval` 吃掉 `=` 返回 NUM、`argnumcpy` 写 1 字节、`kernel_startup_tunable_init` 归一化成 bool）。**四条都成立，进程照旧被杀**，所以 round72/73 的死因目前只能记为未定，不能继续宣称是 legacy 闸；`v0.2.44` 因此不再和这一个调用纠缠，改问 Apple 真正推荐的 `mach_msg2_trap`（表位 47，八个打包参数），它的发送腿根本不进 `ipc_policy_allow_legacy_send_trap`（唯一调用点 `mach_msg.c:611` 只被 `mach_msg_overwrite_trap` 走到），并把 legacy 调用挪到整轮最后重放一次，让「boot-arg 是死是活」变成一个独立的、不会污染主问题的读数；round74 实机读数是**九个 el0 行到 `[clover] el0 mach: msg=` 就断在半行**——光标停在 `=` 后面、地址数字没出来、`m2: presend` 没有、corpse notify 没有、父亲的 `waited` 也没有，屏幕之后一直不变；两行「死亡通知」同时缺席意味着 pid 2 **根本没有退出**，它要么卡在携带数字的那次 `write` 里面、要么还在跑，而一张静态照片分不开这两者，所以 round74 没答上它自己提的问题（msg2 的 `svc` 一次都没执行到，三道策略闸一条都没测到）；objdump 已核对 v0.2.44 的机器码与源码逐条一致、同一套 `_print_dec` 在同一屏上刚印完 259/515/771/1827，因此不是探针写坏；`v0.2.45` 因此把问题换成「停在半行里的是进程还是 tty」，只改父亲：`wait4(child, WNOHANG)` 轮询 + 前 10 轮各印一行 `parent: poll <n>`（之后继续轮询但闭嘴），轮间用 `subs/b.ne` 计数循环烧时间——因为这份内核里 `__semwait_signal(0,0,1,1,…)` 不是延时（`port_name_to_semaphore` 对非法端口名直接返回 `KERN_INVALID_NAME`，`kern_sig.c:1090` 于是回 EINVAL），而 `select`/`mach_wait_until` 分别要结构体 copyin 和一个本表读不到的绝对期限。**round75 实机读数**：面板停在 `[clover] parent: poll ` 半行、光标在空格后、数字没出来，孩子的第一条 `[clover] el0 child: alive`（自带换行的单次 write）一个字都没有，且**没有 panic 屏**——write 若返回 errno 就会走 `_fail`→`exit()`→panic，所以父亲是没从 write 里回来。这与 round74 停在 `el0 mach: msg=` 是同一个形状（标签成功、紧随的那次 write 没回来），而两轮之间唯一的变化是父亲成了控制台的**第二个写者**：EL0 每个字符都落在 `vcputc_options`（`video_console.c:1231`）里那把 `lck_ticket_t`（`:153`）上，排队自旋、无超时，而同一函数在 `gc_enabled` 为假时还会静默丢字符——照片分不开这两种。⇒ **mach 的嫌疑被暂时搁置，round72-74 关于 mach 的读数全部标记为受污染**。round76 因此只做一个变量：只有一个写者。**round76 实机读数**：父亲两整行**完整**（`el0 init: pid=1 console fd=0 pagesize=4096` + `el0 init: forked pid=2 x1=0`，数字全在），200 轮超时后 `exit(384)` 生效，panic 屏画出 `initproc failed to start -- no exit reason available -- (signal 0, exit status 384 )` 且 `a2=0x0000000000000180`，coredump 三连 `error 2` ⇒ **控制台无罪**（单写者时字画得好好的），panic 屏是第二条能用的通道；而孩子在 `forked pid=2` 之后、父亲自己退出之前**一个字都没写**（`el0 child: alive` 都没有），父亲也从没收到尸 ⇒ 孩子既没死也没输出。于是「两个写者搞死控制台」被实证推翻：从头到尾只有一个写者，另一个根本没到屏幕。回看 round70-74，父亲睡在阻塞 `wait4` 里时同一个孩子能印好几页；round75-76 父亲改成 WNOHANG 轮询 + 计数循环，孩子连一个字形都没有——两轮之间唯一的差别是**父亲有没有让出处理器**。`v0.2.47` 因此只改这一件事：轮询之间父亲真的睡，用 `select(0, NULL, NULL, NULL, &tv)`、每轮 50 ms、共 200 轮（源码依据：nd=0 时内核只 copyin 探针自己 `.data` 里的 timeval，`sys_generic.c:1138`；selcount 计数为 0 于是跳过 selscan，`:1332`；最后落到 `waitq_assert_wait64_leeway` + `tsleep1(.., "select", ..)`，`:1455`，是真正的阻塞睡眠，不需要 Mach 权限）；select 自己失败则 `exit(416+errno)`（13 段，别处没有），超时仍是 384。**round77 实机读数**：孩子的九行全回来了（`alive`、post-exec banner、`thread=259 task=515 host=771`、`reply=1827`），面板停在 `[clover] el0 mach: msg=` + 光标、数字一个都没有，与 round74 同一处 ⇒ **饿死成立**，round75/76「孩子沉默」的读数作废，而这次孩子永久持核，所以那半行是可信读数；同一台机器等满两分钟复拍，**什么都不多出来**（无 panic 屏、无 `waited`、无新字形）⇒ 父亲睡进 50 ms 期限再没醒来，**这份端口没有任何定时器能送达 EL0 绝对期限**，探针用过的每个超时（200 轮轮询、msg2 接收的 3000 ms）都不是上界而是丢进程。`v0.2.48` 因此不用时钟，改等一个不需要时钟的事件：孩子死掉。父亲回到阻塞 `wait4(child, &scratch, 0, NULL)`（既出让核心，又能被 `exit`/`SIGKILL` 叫醒、叫不醒卡住的线程），孩子只在数字之后多一条 `exit(448)`，这一轮不画 panic 屏以便孩子的九行和父亲的状态行留在同一张照片上。**round78 实机读数**：正是第三格——孩子的九行照旧停在 `[clover] el0 mach: msg=`，而父亲的 `waited` 永远没有出现，也没有 panic 屏 ⇒ 孩子**既没死也没被饿死，它是卡在某一次 `write` 里面**，round72-74 关于 `mach_msg` 的猜测正式出局（探针在那条 `svc` 之前就结束了）。但形状里有个细节决定下一轮问什么：字符是在 `write` 返回**之前**就上屏的（`ttwrite` 填 `t_outq` → `ttstart` → `kmstart`/`kmoutput` 同步画字），所以「屏幕上有个 `msg=`」并不等于「标签那次写成功了」，它同样兼容「标签这次写完自己的字之后在返回路上堵住」——那正是 `ttwrite` 的 `ovhiwat` 出口 `ttysleep(…, "ttywri", tp->t_timeout)`（`tty.c:2656`）在这台没有期限的板子上的表现。`v0.2.49` 因此把孩子的 `exit(448)` 往上挪两条指令，切在标签那次写与数字那次写**之间**，其余一字未改。**round79 实机读数**：探针一次都没被执行到，面板照旧停在 `msg=`、父亲的 `waited` 照旧永远不出现 ⇒ 堵住的是 `msg=` **那次标签 write 自己**（把自己的字节画上屏之后再没回来）；而 `ttwrite` 里「画完之后」还能停住的位置只有 `ovhiwat` 出口的 `ttysleep(TSA_OLOWAT, "ttywri", tp->t_timeout)`（`tty.c:2656`）—— 在一块期限从不响的板子上，那就是一次永久睡眠。但这张照片同时兼容三种状态（标签 write 堵住 / `exit` 堵住 / 父亲被叫醒之后自己那次打印堵住），因为它们唯一的输出通道就是那个正在被审查的控制台。`v0.2.50` 因此只换父亲的报告方式：醒来后一个字都不写，改成 `exit(512 + (status & 0xff))`，用 initproc 退出触发的 **panic 屏**说话（round76 已证明它直写 framebuffer、不调 `vcputc`）：`exit status 512` = 孩子退出了、`wait4` 被叫醒、缺的只是打印；`512+N`（N<256）= 孩子被信号 N 弄死；永远没有 panic 屏 = 孩子真的还在那次 write 里面 。`round80` 实机读数落在第三格：面板停在 `msg=`，**永远没有 panic 屏**（间隔两分钟复拍也一样）——父亲的阻塞 `wait4` 从没被叫醒，孩子从没死；而且内核自己那行 `Failed to send exception EXC_CORPSE_NOTIFY … for pid 2` 也没出现在开机日志尾巴上，`kprintf` 走 video console 不走被测的 tty，所以这是**两条独立证据**说同一件事。剩下两个嫌疑：标签那次 write 画完字之后堵住，或者 write 返回了、孩子的 `exit` 里堵住——而「exec 出来的孩子的 `exit()`」在这块板上从来没被证明过（round70 那个孩子是 fork 未 exec，round72/73 是 SIGKILL 不是 exit）。`v0.2.51` 用第三种事件切开它们：孩子不再去死，改成 `kill(getpid(), SIGSTOP, 0)`（Darwin 的 kill 是三个参数，syscalls.master:91，`posix` 传 0），停住是在**进 userland 的路上**办完的，所以哪怕孩子再也没执行一条指令，父亲也收得到；父亲 `wait4` 加 `WUNTRACED`（2）并把状态低 16 位全留着（`W_STOPCODE(17)` = 4479，遮成字节会和「signal 127」撞车）。四个读数：panic `4991` = 512+4479，write 返回了、堵的是孩子的 `exit`；`1024` = kill 成功但没停住、孩子 `exit(2)`；`768` = kill 本身失败（37 号不可达）、孩子 `exit(1)`；永远没有 panic 屏 = 堵住的就是那次 write。**round81 实机读数**：正是第四格——面板停在 `msg=`，复拍什么都不多出来，而且这次能签字：这份树里 SIGSTOP **不是**延后信号，`kill` 的默认动作在 syscall **里面**办完（`kern_sig.c:2349` stopsigmask → `:2362` `p_stat=SSTOP` → `:2373` `stop()`，而 `stop()` 第一步就 `wakeup(parent)`），所以只要那次 `kill` 进了内核，父亲的阻塞 `wait4` 必定被叫醒；父亲一次都没被叫醒 ⇒ 孩子连那条 `svc` 都没碰到 ⇒ 堵住的就是 `msg=` 那次标签 write 自己，孩子的 `exit` 正式出局。同一轮把 `ovhiwat` 头号嫌疑降级：`bsd/dev/arm/km.c` 的 `kmoutput` 是 `while (c_cc > 0)` 一次排空、`t_hiwat = roundup(1216,48) = 1248` 大于整个 cl 池 `TTYCLSIZE = 1024`，那条判断永远到不了。**v0.2.52（round82）换了目标：这台板子从来没有过一次 tick。** 依据来自参考仓 `xVanTuring/UniXNU-tb128fu`（`board.toml:199-212`，`armcpu-timer-intid=19`，注释说 27 是「another source that stays asserted」）与本板真机设备树 `mu-clover/Resources/DTBs/clover.dts:2233`：timer 节点四个 PPI 是 1、2、3、0 ⇒ 真 INTID 17、18、19、16，而 `enableCPUTimeBase` 写的是 `cntv_ctl_el0`（虚拟定时器）⇒ 该走的是 **19**；我们这份 `drivers/ArmCpu` 把 `0xffff` 并上 `(1 << 27)` 写进 redistributor ISENABLER0、又在 `interrupt()` 里只认 27，也就是内核一直在等一条从没被 enable 的线。一条根因统一解释 round77 的两次实测（`select` 的 50 ms 期限睡下去再没醒来 = 期限依赖 `Timer::sleep` 到点；EL0 纯自旋饿死同板另一个进程 = 没有 tick 就没有抢占）。本轮只动内核：`drivers/ArmCpu/` 用 `feeds.py` 的设备仓优先规则覆盖基仓模块，加 `armcpu-timer-intid`（16..31 才接受，不传仍是 27）与 `armcpu-clock-panic=N`（第 N 次 tick 直接 `panic("ArmCpu: clock watchdog after %llu ticks (intid=%u ipi=%llu irq=%u phase=%u)")`），panic 屏多打 `ticks/ipi/sent/irq/phase/intid/typer` 七行；探针与 root 逐字节不变，故 trustcache 不变。看门狗保证「什么都不发生」也一定出声：`ticks=0x7530` 说明时钟在走而进程照旧卡住（时钟无罪，回到 tty），完全没有 panic 屏说明 INTID 19 也没让 tick 起来（用 `typer=` 核算本板 GIC 的 INTID 上限再换线）。这一轮故意不传 `armcpu-self-ipi` 与 `armcpu-spi-ipi`：单核，且 `kIpiSpiBase` 硬编码的 1008 有没有超出本板上限要看 `typer=`。**round82 实机读数：panic 屏第一次出现，而且 `ticks=0x14e5`（5349）⇒ INTID 19 是对的，这块板第一次真的有 tick，看门狗与 panic 屏两条通道都可用。但死因换人了**：panic 内容是 `"AMFI: No PMGR?"`（`ConfigurationSettings.cpp:235`）。AMFI 在 dyld 启动期的 thread-call 工作线程里等一个 `ApplePMGRNub` 服务；过去没有 tick，那次带期限的等待永远不响、线程只是无声吊着，启动继续走到 EL0；有了 tick 以后期限真的到期、匹配返回空，于是 AMFI 自己 panic。`ipi=0 sent=0` 与单核相符；`irq=0x3ff`（=1023，分发计数器的初值）、`phase=0`；`intid=` 与 `typer=` 两行被开机日志的尾部覆盖了，本轮没读到。**v0.2.53（round83）已发布：给 AMFI 造一颗它自己能匹配上的 `ApplePMGRNub`**（`drivers/CloverPMGR` + `CONFIG_XNU_SHENG_PMGR=y` + `clover-pmgr=2`，EL0 探针与 root 一字未动，panic 屏多打 `pmgr=/nub=/aon=` 三行，看门狗 `armcpu-clock-panic` 撤掉）；`v0.2.54` 只把 panic 屏的打印顺序换成「读数在前、寄存器证据在后」，因为 `put()` 越过面板下沿就静默丢字（round82 就是这样丢了 `intid=`/`typer=`），题面与产物语义不变。**round83 实机读数 = 命中判定表最后一格：`CloverPMGR` 一条 panic 都没报、`"AMFI: No PMGR?"` 消失、boot 在有 tick 的状态下走到 EL0 ⇒ 守卫壳 + 原始工厂造的这颗 nub 确实被 AMFI 自己那条匹配抓走了，AMFI 要的只是一颗服务而不是电源功能**；面板照旧停在 `[clover] el0 mach: msg=`、后面什么都没有、没有 panic 屏，与 round81 同形 —— 但「期限从来不响」这个借口现在有两条证据说明它已经没了（round82 那次 panic 本身就是 `waitForMatchingService` 的 deadline 到期返回空才产生的，加上 `ticks=0x14e5`），照片里还有一条正面证据：`AMFI: PMGRAON latched to: …`（内核串 VA `0xfffffff0042dde0f`，紧接 `0xfffffff0042dde02` 那颗 `'ApplePMGRNub\0'`）——AMFI 确实拿着我们发布的 nub 走完了「slot 320 读 AON → OR 0x4 → slot 321 写回」，所以原计划给 CloverPMGR 成功路径补的那条 `IOLog` 取消（信息重复，还要重建内核）。**`v0.2.55`（round84）因此做两件事**：一是把 round79 那一刀在有 tick 的板上重切（孩子回到标签 write 之后立刻 `exit(448)`，删掉 round81 的 `kill(SIGSTOP)` 块）；二是修父亲那条报数通道——查「多出来一行 `parent: status = 114688`」时查明它**根本不可能出自 v0.2.54 的镜像**（payload 原始字节里 `parent: status`/`status = ` 都是 0，43 份归档全为 0，`114688 = 448 << 8` 恰好是 round78/79 那版父亲打印的原始 wait 状态），顺带挖出真 bug：initproc 的退出码到面板要过 `WEXITSTATUS(rv) = (rv >> 8) & 0xff`（`kern_exit.c:1875` + `wait.h:144`，`rv = W_EXITCODE(C,0) = C << 8` 见 `kern_exit.c:1466`），**只有 8 位**，round80-83 的 `512 + 低16` 在到达屏幕前就被折掉了（`exit(448)` 会读成 `exit status 0`、SIGSTOP 会读成 `127`），只是那几轮的读数都是「完全没有 panic 屏」，缺席不受截断影响，所以一直没骗到我们。父亲现在手工把码落在 0..255：`0`=我们的 `exit(448)`、`1`=从 `_fail` 走的、`32+N`=信号 N、`96+N`=停在信号 N、`32`=父亲的 `wait4` 自己失败；永远没有 panic 屏 = 有 tick 也堵，下一刀切进 `kmoutput`/`ttwrite`。**round84 实机读数 = 最后一格**（面板停在 `msg=`、复拍两分钟什么都不多出来），但这张照片第一次把孩子的 `write` 判了无罪：底部八行逐字可读（`alive`、post-exec banner、`thread=259 task=515 host=771`、`reply=1827`、`msg=`），说明 exec 出来的孩子已成功返回过至少八次 write（其中三段是不带换行的裸标签、还有一次换行真的把屏幕滚了一行）⇒ 「exec 孩子 syscall 不可用」与「只有带 `\n` 的写会回来」两条猜测同时出局，`gc_scroll_up`/`vc_scroll_up` 也不是堵点。于是 `msg=` 与前八次成功的写在内容、长度、结尾、光标位置上都没有区别，而这张照片仍兼容三种状态：(a) 那次 write 没回来、(b) write 回来了孩子的 `exit` 没回来、(c) 两个都办了但父亲的阻塞 `wait4` 没被叫醒（round70 那个能叫醒父亲的孩子是 fork 未 exec，exec 之后 `childproc` 还连不连着从未验证）——三者的报告通道恰好就是被审查的那两样东西。⇒ v0.2.56 只换通道：孩子用 `reboot(RB_PANIC\|RB_QUICK, msg)`（syscall 55，`kern_xxx.c:182` 立刻 `panic_with_options("userspace panic: %s")`，不经 sync/卸载、不经 tty、不经父亲；DEVELOPMENT 内核里 `kern_xxx.c:116/142` 连 suser 与 MACF 都跳过）。**round84 读数落盘后 v0.2.56（round85）已发布**：`2873df4f…` payload / 根镜像 `fe27be32…`（35456 B，cdhash `6faaec7e965356b1af732a83f297e605010dcf02`）/ trustcache `d310ae35…` 已同步进 `trust_cache_sha256`，内核逐字节不变（`e98b9659…`）。孩子最后一次动作是 `reboot(0xc00, "clover-el0: write returned")`，紧跟一条不可达的 `exit(449)`（刻意不等于 448，这样父亲报 `1` 而不是 `0`）。读数四格：`userspace panic: clover-el0: write returned` = 标签 write 回来了、(a) 出局；`userspace panic: user space RB_PANIC message copyin failed` = 走到了 reboot 但 copyin 失败（另一条通道，同样证明 write 回来了）；照旧 `initproc exited … exit status 1` = 孩子从 `_fail` 走的；永远没有 panic 屏 = (a) 成立，下一刀把 reboot 挪到那次 write **之前**，一次调用同时切两条边。**round85 实机读数 = 永远没有 panic 屏** ⇒ 四个出口（write 返回 / `exit(448)` / `kill(SIGSTOP)` / `reboot→panic`）在同一处静默，而 round74-84 十一轮同形；剩下的三种状态（write 没回 / `exit` 没回 / 父亲 `wait4` 没被叫醒）**从用户态分不开**，因为它们只能经由被审查的 tty 或那个可能没醒的父亲发言。v0.2.57（round86）因此把报告搬进内核，但搬进**不被测的那条路**：`drivers/ArmCpu` 的 tick 看门狗 panic 带 `elr=%llx spsr=%llx elr2=%llx cnt0=%llx cnt1=%llx freq=%llx`（入口处的 `ELR_EL1` = 被打断上下文当时的指令地址，`spsr` 低 4 位 = 它在哪个 EL：5=EL1h、0=EL0t），`armcpu-clock-panic=60000`（tick 数是启动进度坐标：round82 的 5349 在 AMFI 检查处 = 探针到达点前一步，所以 60000 ≈ 十一倍之后，且不依赖 tick 周期；`cnt0/cnt1/freq` 顺手第一次量出周期）。不用 `kprintf` 插桩是查出来的两件事：`console_write` 自己是环形缓冲 + 写锁（`serial_console.c:572` 无界自旋排空、`:424` 持 `write_lock` 自旋），从被测路径内打印会二次进入被测对象；而 `tree/patches/xnu` 是指向基仓的软链、`feeds.py` 按 `(kind,name)` 取第一个存在的路径 ⇒ 设备仓建 `patches/xnu` 会整体顶掉上游上百个补丁，所以仪器进设备仓已拥有的 `drivers/ArmCpu`。本轮**第一次换的是内核**（`e98b9659…` → `e87a5127…`），EL0 探针与根未动（`fe27be32…`、trustcache `d310ae35…` 照旧），payload `b964c1a5…`，`verify` 四项全绿、卷内 `unixnu.toml` 实到 `armcpu-clock-panic=60000`；读数侧新工具 `scripts/decode-clover-address.py`（`--suspects` 出本镜像的 tty/console 区间表，命名 + 偏移 + 回跳循环判定），已用 `_ttwrite`/`_console_write`/`_cpu_idle` 三个已知地址自测，并能认出 `console_ring_try_empty` 那条 25 指令无调用轮询。 |

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
./unixnu bootimg --uefi ../mu-clover --target RELEASE \
  --dtb ../mu-clover/Resources/DTBs/clover.dtb --ramdisk none
```

最后一行是 boot 分区唯一需要的命令：`unixnu` 没有 `build boot` 这一步（`build` 的子步是
kernel/package/firmware/payload/userdata/prelink），boot 镜像由 `bootimg` 从 `--fd`
（固件卷，`--target RELEASE` 下自动找）和 `--dtb`（固件构建时打过 splash 补丁的那份）
拼出来；`--ramdisk none` 不是省略而是**必须**，否则它会把 payload 卷当 ramdisk 塞进去，
而这块板子的固件根本看不见 ramdisk 里的卷。产物是 `out/clover/boot/boot.img`。

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

### round76 实机读数：只有一个写者时控制台完好，孩子却一个字都没写

两张照片。EL0 尾部（`hflip,vflip` 转正后逐字读）：

```
No POST boot-arg set.
load_init_program: attempting to load /usr/appleinternal/sbin/launchd.development
load_init_program: attempting to load /sbin/launchd
[clover] el0 init: pid=1 console fd=0 pagesize=4096
[clover] el0 init: forked pid=2 x1=0
pid 1 exited -- no exit reason available -- (signal 0, exit 384)
coredump (launchd, pid 1): writing core to /private/var/cores/launchd.core
coredump (launchd, pid 1): failed to open core dump file /private/var/cores/launchd.core: error 2
coredump (launchd, pid 1): core dump failed: error 2

Failed to generate core file for pid: 1: error 2, took 0.130 seconds
```

第二张是 panic 屏：`initproc failed to start -- no exit reason available -- (signal 0,
exit status 384 )`，寄存器块里 `a2=0x0000000000000180` —— 384 就是探针自己传的那个
`exit` 码，这一条自证了 panic 屏画的是这次退出，不是别的什么断言。照片一的上半屏全是
内核日志（`[clover] kmainit/kmKitBSDInit/loopattach/…returned`、apfs、NVRAM sync、
`Added memory device md0/ram0`、`BSD root: md0`），**没有一行是孩子的**。

三条判据，逐条落实：

1. **控制台无罪。** 父亲那两整行是完整的——`pid=1`、`console fd=0`、`pagesize=4096`、
   `forked pid=2`、`x1=0`，五个数字一个都没丢。round74/75 那种「标签印出来了、紧随的
   数字没有」在这里没有发生。所以 `vcputc_options` 那把 ticket lock 没被谁卡住，
   `gc_enabled` 也没在丢字符。
2. **panic 屏是第二条能用的通道。** 清屏、画字、打印寄存器块，全都在。以后「出声」
   不一定非要经过控制台。
3. **孩子零输出，而且没死。** `forked pid=2 x1=0` 之后紧接着就是父亲自己的退出行，
   中间什么都没有：没有 `el0 child: alive`（那是自带换行的单次 write，round70-74 每次都
   印得出来）、没有 post-exec banner、没有 mach 行；父亲也没有 `waited` 行，即 200 轮
   `wait4(WNOHANG)` 一次都没收到尸。所以孩子不是被 SIGKILL 掉的，也不是停在某半行上——
   它根本没往屏幕上放过一个字形。

于是「两个写者把控制台搞死了」这个 round75 立起来的理论被**实证推翻**：从头到尾只有
一个写者，另一个压根没到屏幕。而回看这几轮，有一件事一直没被注意：round70-74 父亲
**睡在阻塞的 `wait4` 里**，同一个孩子能印好几页；round75-76 父亲改成 WNOHANG 轮询、
轮间用 `subs/b.ne` 计数循环烧时间，孩子就一个字都印不出来。两轮之间唯一变化的不是
mach、不是控制台、不是 boot-arg，而是**父亲有没有让出处理器**。

顺带一条标定：panic 屏上方内核日志的时间戳停在 `[10.803757]`，也就是说 200 轮计数
循环远没有「每轮几十毫秒」，那个估计整体偏慢两三个数量级——以后凡是要烧时间的地方，
都不能再拿计数当钟表。

### v0.2.47：父亲在轮询之间真的睡一觉，只测「饿死」还是「卡死」

孩子那一路**一条指令不改**，父亲的 `wait4(child, WNOHANG)` 轮询、200 轮上限、
`exit(384)` 超时也全部保留。改的只有一处：把轮间的计数循环换成一次真正的阻塞睡眠
—— `select(0, NULL, NULL, NULL, &_tv)`，`_tv = {0, 50000}`，每轮让出 50 ms，200 轮
就是十分钟里父亲只占 200 次系统调用的那几微秒。

为什么是 `select`，是从源码里挑出来的而不是试出来的。这份内核在 EL0 能用的等待原语
本来就三个，前两个已经证明不通：`semaphore_timedwait_trap` 与 `__semwait_signal` 都要
一个 Mach 权限，而端口 0 不是权限——`port_name_to_semaphore`（`osfmk/kern/sync_sema.c:1168`）
对非法端口名直接返回 `KERN_INVALID_NAME`，`__semwait_signal` 于是回 `EINVAL`
（`bsd/kern/kern_sig.c:1090`）；`mach_wait_until`（表位 90）要的是绝对 tick，而这张表里
没有一个 trap 能读到 tick。`select` 不一样：`nd = 0` 时 `select_nocancel` 唯一从用户态
取的东西就是那个 timeval（`bsd/kern/sys_generic.c:1138`），而 timeval 是探针自己 `.data`
里的 16 字节；`selcount` 于是给出 0 个计数（`:1332`），`selprocess` 因为 `count == 0`
整段跳过 `selscan`，一路走到 `waitq_assert_wait64_leeway` 加
`tsleep1(.., "select", ..)`（`:1455`）—— 这是真正的阻塞睡眠，会把核交出去。附带好处：
超时从此由内核计时，10 秒就是 10 秒，不再取决于这颗核数得多快。

`.data` 里那个结构是 LP64 的形状，不是 C 里那个：进程是 64 位，`IS_64BIT_PROCESS(p)`
走 `user64_timeval` 分支，`tv_sec` 和 `tv_usec` 各 8 字节，所以 50 ms 写作第二个 8 字节
字段里的 50000。objdump 与反汇编出来的 `.data` 双向复核过：

```
 6a8: add   x20, x20, #0x1
 6c0: mov   x16, #0x7           // SYS_wait4
 6c4: svc   #0x80
 6c8: b.hs  _wait_error
 6cc: cbnz  x0, _wait_reaped
 6d0: mov   x0, #0x0            // nd = 0
 6d4: mov   x1, #0x0
 6d8: mov   x2, #0x0
 6dc: mov   x3, #0x0
 6e0: adrp  x4, ...             // &_tv，落在 0x8b8
 6e8: mov   x16, #0x5d          // 93 = SYS_select
 6ec: svc   #0x80
 6f0: b.hs  _select_error
 6f4: cmp   x20, #0xc8          // 200
 6f8: b.lo  _parent_poll
 6fc: mov   x23, #0x180         // 384 = 超时
 704: mov   x23, #0x1a0         // 416 = 13 * 32
 708: add   x23, x23, x0        //       + errno
```

```
08a0 06000000 07000000   _mib: CTL_HW(6), HW_PAGESIZE(7)
08b0 08000000 00000000   _scratch_size: 8
08b8 00000000 00000000   _tv.tv_sec  = 0
08c0 50c30000 00000000   _tv.tv_usec = 0xc350 = 50000
```

轮询体内恰好两个 `svc`，`_poll_spin` 已经整个删掉，没有任何残留的自旋。

四个读数（**拍之前等满两分钟**，让 200 轮 × 50 ms 跑完）：

| 看到 | 结论 |
| --- | --- |
| 孩子的行回来了（`alive`、post-exec banner、mach 那几行），面板停在 round74 停过的地方 | **饿死成立**：父亲那个计数循环就是孩子不出声的全部原因，round75/76 所有「孩子沉默」的读数作废，mach 那一问可以重新读 |
| 孩子仍然一个字没有，panic 屏还是 `exit status 384` | 父亲确实让出了处理器，孩子拿到了 CPU 还是不写 ⇒ 它是**自己卡在内核里**（最可能在那次 `exec` 或者它没走到的第一次写），与父亲无关 |
| panic 屏写 `exit status 416+errno` | `select` 在这份内核上睡不成（低 5 位就是 errno）。这本身是个值得知道的端口事实，且与 mach 无关 |
| 面板冻住、连 panic 屏都没有 | 父亲睡下去再没醒来——**没有定时器送达 EL0 的绝对期限**。注意这一格并不白搭：孩子从此永久持有那个核，它印出来的东西就是真正的读数 |

产物（`scripts/verify-clover-payload.py` 逐文件校验通过）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round77-payload.img` | 58720256 B | `1d0e7d162bed81ebed6c0bf9c5caf2c888ce1e3be211d0d9a4d231b344bf3267` |
| `round77-root.img`（launchd.macho） | 35600 B | `691c1e182fa5b619ce6105a3e39177e33532be1484c3d61f8ae3749b01263529`，cdhash `ece296150b4de0d3319b1341e79a0d670c606a0d` |
| `round77-trustcache.bundle` | 56 B | `1364b4ec5b11a297142475649e5453caf30dc10383d04a77e7411eb643645b0e` |
| `round77-adt.bin` | 13476 B | `141acf4f777b8edca9ca20761b72d006fd1f928427112426adf636d8ec838491`（每轮 `random-seed` 都换） |

卷内 `unixnu.toml` 483 B `6add9bc6e7618e9d…`，boot-args 依旧
`-v serial=3 debug=0x14e kt_fixed_zones=120 boot-progress=2 rd=md0 allow_legacy_mach_msg=1`
——**内核侧一个字节都没动**（卷内 `kernel.macho` 仍是打过入口探针的 `1c8e9a40…`，
5626 字节 / 35 段），这一轮改的只有用户态探针怎么等。菜单标题
`clover md0 root: parent sleeps between polls`。注意 root 镜像一变，`trust_cache_sha256`
必须跟着换成上面那颗，否则 `./unixnu build payload` 会直接拒绝。

刷机：

```sh
fastboot flash cache ~/clover-out/round77-payload.img
fastboot reboot
```

### round77 实机读数：饿死成立，而这台板子上没有任何定时器能叫醒睡着的线程

一张照片，EL0 段逐字（屏幕边缘以下没有内容）：

```
[clover] siginit returned, execing init
VM Swap Subsystem is ON
No POST boot-arg set.
load_init_program: attempting to load /usr/appleinternal/sbin/launchd.development
load_init_program: attempting to load /sbin/launchd
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

上半屏是连续的内核日志（`vfsinit` → `abninit: done [64 MB total pool size, (64/0) split]` →
`dill_init … scheduled at least once` → `ifnet_attach lo0` → apfs/Zlib kmod →
`AppleImage4: magazine… 30` → NVRAM sync `ret=0xe00002d8` → `Added memory device md0/ram0` →
`BSD root: md0`），**没有 panic 屏、没有清屏带**。

两条结论，第一条是这一轮要问的：

1. **判定表第一格成立：是饿死。** 父亲只是把每轮之间的纯计数换成 `select(0,…,&_tv)`，
   孩子立刻从「零输出」变回「九行全在」。round75/76 关于「孩子沉默」的读数**全部作废**，
   控制台与 `osfmk/ipc` 第二次洗清；真正的机制是 **EL0 的纯自旋会把同板的孩子饿死**。
   而面板停在 `[clover] el0 mach: msg=` + 光标、数字一个都没有，和 round74 停在同一处。
2. **父亲的 `select` 到底回没回，照片本身分不开**（拍照可能早于 10 s 超时）。
   同一台机器再等满两分钟复拍一次：**什么都不多出来**——没有 panic 屏、没有 `waited`、
   连一个新字形都没有。⇒ 第四格成立：父亲睡进 50 ms 的期限就再没醒来，
   **这份端口上没有任何定时器能送达 EL0 的绝对期限**。这条推论很贵，因为它把这份探针
   过去用过的所有超时（200 轮轮询、msg2 接收的 3000 ms）都从「上界」变成了「丢进程」。

顺带一条硬事实：孩子此刻永久持有那个核心，所以它印出来的 `msg=` 半行是**可信读数**——
不是被父亲饿掉的沉默。

### v0.2.48：父亲改回阻塞 `wait4`，孩子多一条 `exit(448)`，只问「卡住还是被杀」

定时器既然叫不醒睡着的线程，这一轮就不用时钟，改等一个不需要时钟的事件：孩子死掉。
`wait4(pid, &scratch, 0, NULL)` 同时办完父亲的两件事——它会出让核心（round70-74 证明这正是
孩子能印好几页的原因），而且 `exit` 和 `SIGKILL` 都会叫醒它、**卡住的线程永远叫不醒**。
孩子再加一个词把第三种情况切开：数字之后、trap 之前一条 `exit(448)`（448 = 14 × 32，
stage 14 别处不用）。这一轮**不画 panic 屏**，好处是控制台不会被清掉，孩子的九行和父亲的
状态行留在同一张照片上。

```
10000060c  bl   _puts           // "[clover] el0 mach: msg="
100000610  mov  x0, x20
100000614  bl   _print_line     //   <地址> 与换行
100000618  mov  x23, #0x1c0     // 448 = 14 * 32
10000061c  b    _exit_now

100000a2c  mov  x0, x19         // 只等这一个孩子
100000a34  add  x1, x1, #0x8    //   &_scratch
100000a38  mov  x2, #0x0        // 没有选项 ⇒ 阻塞
100000a40  mov  x16, #0x7       // SYS_wait4
100000a44  svc  #0x80
100000a48  b.hs _wait_error
100000a4c  cbnz x0, _wait_reaped
100000a50  b    _checkpoint
```

全文件已经没有 `mov x16, #0x5d`（`SYS_select` = 93），`_tv`、`_parent_poll`、`_select_error`
一并删除——`.data` 里不再有 timeval。孩子除了那条 `exit`，其余一条指令未改。

三个读数（拍一张，再等两分钟复拍一张确认第三种）：

| 看到 | 结论 |
| --- | --- |
| `msg=<数字>` 且 `waited pid=2 status=114688`（= 448 << 8） | 数字写出去了、进程跑到探针。round77 少掉那个数字的原因不是那次 `write`，而是父亲**永久睡在 select 里**这件事本身 |
| `msg=`（没数字）且 `waited pid=2 status=N`，N < 256 | 孩子**死在那次写里面**，低字节就是信号；9 = SIGKILL，就是 round72 guard 异常的样子 |
| 只有 `msg=`，永远没有 `waited` | 既没死也没活到探针 ⇒ 那次 `write` 根本没返回：核心是空着的、父亲在睡，停的是 tty 这条路，与 mach 无关 |

产物（`scripts/verify-clover-payload.py` 逐文件校验通过，`scripts/check-clover.py` 通过）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round78-payload.img` | 58720256 B | `f3af38336e78338262eb206ed5c01ec28a1968c388a983a859551a7b96c57b1a` |
| `round78-scratch-data.img`（launchd.macho） | 35520 B | `9fffadac1733c360d3588562e489343f8c6445cf8f12d361285903bdc95d6ffa`，cdhash `5ec2fe213c9190aa57f289fe4cf66d3ff2b8f79a` |
| `round78-kernel.macho.trustcache` | 56 B | `9a91d15d6dbe94f5d95a267db211ca2ab4be9feb5a0d6b98034fa3cddf1b2cb2` |
| `round78-kernel.macho.adt` | 13476 B | `19ed68314e08a06294679d1f65366bc6d75e7c4a10824f24bfb2a8e1b5a3c833`（每轮 `random-seed` 都换） |

卷内 `unixnu.toml` 484 B `0b01131d3909522c…`，boot-args 依旧
`-v serial=3 debug=0x14e kt_fixed_zones=120 boot-progress=2 rd=md0 allow_legacy_mach_msg=1`，
**内核侧一个字节没动**（卷内 `kernel.macho` 仍是打过入口探针的 `1c8e9a40…`，5626 字节 / 35 段）。
菜单标题 `clover md0 root: parked or killed, wait4 says`。root 镜像一变，`trust_cache_sha256`
必须换成上面那颗 trustcache。

刷机：

```sh
fastboot flash cache ~/clover-out/round78-payload.img
fastboot reboot
```

### round78 实机读数：只有 `msg=`，永远没有 `waited` —— 孩子卡在 `write` 里面

第三个读数，逐字与 round77 停在同一个地方：父亲的 `forked pid=2 x1=0` 之后，孩子的九行照常
回来（`alive`、post-exec banner、`thread=259`、`task=515`、`host=771`、`reply=1827`、
`[clover] el0 mach: msg=`），然后面板再没有多出任何一个字形，也没有 `waited`，也没有 panic 屏。

这一轮的探针把三种可能性切成互斥的三格，所以读数是有含义的：

- **孩子没死。** 父亲坐在阻塞 `wait4` 里，`exit` 和 `SIGKILL` 都会把它叫醒并印出
  `waited pid=2 status=`。一行都没有 ⇒ 孩子还活着。
- **孩子没被饿死。** 父亲睡在 `wait4` 里，核心是空着的（round77 刚证明过这件事）。
- ⇒ 它**卡在某一次 `write` 里面**，卡在kernel里，而不是被谁杀了。round72-74 关于
  `mach_msg` 的全部猜测到这里正式出局：孩子根本没走到那条 `svc`，探针在它前面。

但形状里有一个不能忽略的细节，它决定下一轮问什么：**字符是在 `write` 返回之前就已经上屏的**。
`ttwrite`（`bsd/kern/tty.c:2459`）把字节填进 `tp->t_outq`，`ttstart` 调 `kmstart`，
`kmoutput` 经 `console_write` 同步排空 —— 全都在那次系统调用返回之前。所以屏幕上有个 `msg=`
并不说明「标签那次写成功了」：它同样兼容「标签这次写把自己的字节画完之后，在往外返回的路上堵住了」。
`ttwrite` 的 `ovhiwat` 出口正是这种堵法：`ttysleep(tp, TSA_OLOWAT(tp), TTOPRI | PCATCH,
"ttywri", tp->t_timeout)`（`tty.c:2656`），而 round77 已经证明这台板子上没有任何期限会响
⇒ 一旦走进那次睡眠就永远醒不来。

### v0.2.49：把 `exit(448)` 往上挪两条指令，切在「标签那次写」和「数字那次写」之间

文件里只改这一处：孩子原来在 `bl _puts` → `mov x0, x20` → `bl _print_line` 之后才
`mov x23, #448 / b _exit_now`，现在探针紧跟在 `bl _puts` 后面，`_print_line` 变成这一轮
不执行的两条指令（保留原地，下一轮删掉分支就回来了）。父亲一个字没改：仍然是
`wait4(pid, &scratch, 0, NULL)`，仍然不画 panic 屏。

```
10000060c  bl   _puts           // "[clover] el0 mach: msg="
100000610  mov  x23, #0x1c0     // 448 = 14 * 32
100000614  b    _exit_now       // ← 这一轮从这里离开
100000618  mov  x0, x20         //   下面两条这轮不可达
10000061c  bl   _print_line
```

两个读数：

| 看到 | 结论 |
| --- | --- |
| `msg=` 后面跟着 `parent: waited pid=2 status=114688`（448 << 8） | 标签那次 `write` **返回了**，而且父亲被叫醒、状态行也画得出来 ⇒ round78 堵住的是紧随其后的**数字那次写**，是「同一个人连着第二次写控制台」这件事 |
| 还是只有 `msg=`，永远没有 `waited` | 探针没被执行到 ⇒ 堵的就是**标签这次写自己**：它把字节画完，然后在返回路上停住（`ttwrite` 的 `ovhiwat`/`ttysleep("ttywri", t_timeout)` 这条，或者 `tty_lock`）。这一格同时说明父亲的 `wait4` 也再没被叫醒，两个进程一起挂在 tty 上 |

产物（`scripts/verify-clover-payload.py` 逐文件校验通过，`scripts/check-clover.py` 通过）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round79-payload.img` | 58720256 B | `17c422c18ef8f55191852676e194c0c4e3c5ecaa79209ffd22d3ef373f64c840` |
| `round79-scratch-data.img`（launchd.macho） | 35520 B | `4522dff71879b7f1141e19865db4d961a372e06d50feb5491256bc28829c4238`，cdhash `f3992a47accc44bef4c4a583bd45cf4106cee693` |
| `round79-kernel.macho.trustcache` | 56 B | `cabc9d22ea035a2f99eed4479de70b57e0fe8ce499003065545343f02022adef` |
| `round79-kernel.macho.adt` | 13476 B | `2329d37bc72b978d940725ae624fe291b34e44e5e09e41c3171650f68a204098`（每轮 `random-seed` 都换） |

卷内 `unixnu.toml` 493 B `efa4d6955ac69e9c…`，boot-args 依旧
`-v serial=3 debug=0x14e kt_fixed_zones=120 boot-progress=2 rd=md0 allow_legacy_mach_msg=1`，
**内核侧一个字节没动**（卷内 `kernel.macho` 仍是打过入口探针的 `1c8e9a40…`，5626 字节 / 35 段）。
菜单标题 `clover md0 root: which of the two writes never returns`。root 镜像一变，
`trust_cache_sha256` 必须换成上面那颗 trustcache。

刷机（这一轮不需要复拍：两个读数里都没有定时器）：

```sh
fastboot flash cache ~/clover-out/round79-payload.img
fastboot reboot
```

### round79 实机读数：探针一次都没被执行到 —— 堵住的是 `msg=` 那次 write 自己

面板仍然停在 `[clover] el0 mach: msg=`，父亲的 `waited` 仍然永远不出现。探针在标签那次 write
之后、数字那次 write 之前，所以数字那次 write 根本没被请求过 ⇒ **标签这次 write 把自己的字节
画上屏之后就没回来**，而唯一在它「画完之后」还可能停住的位置，就是 `ttwrite` 的 `ovhiwat` 出口
`ttysleep(tp, TSA_OLOWAT(tp), TTOPRI | PCATCH, "ttywri", tp->t_timeout)`（`bsd/kern/tty.c:2656`）：
outq 没排空 → 睡在一个期限内 → 而 round77 已证明这台板子的期限从不响。

但这一张照片值的数据比它看起来少，因为它同时兼容三种状态，而孩子侧无法把它们分开：

1. 标签 write 画完字然后堵住（孩子还在里面）；
2. 标签 write 返回了，孩子的 `exit(448)` 堵住（`exit1` 里还有 `machine_lock_all` 这类等待）；
3. 标签 write 返回了、孩子退出了、父亲的 `wait4` 被叫醒，**缺的是父亲自己那次状态行打印**。

一张照片里三种状态完全同形，因为它们唯一的输出通道就是那个正在被审查的控制台。

### v0.2.50：父亲醒来后一个字都不写，改用 panic 屏回答「孩子到底走到哪儿了」

只改父亲的报告方式：`_wait_reaped` 删掉三次 `write`，改成 `exit(512 + (status & 0xff))`
（512 = 16 × 32，别处不用）；`_wait_error` 同样不再打印，直接 `b _fail`（x1 = 544 = 17 × 32，
回 `544 + errno`）。**initproc 退出会触发 panic**，而 panic 屏由
`scripts/xnu-panic-screen.c` 用自己的 printf **直写 framebuffer、全程不调 `vcputc`**，
round76 已实证它在「控制台本身是嫌疑人」时照样画得出来 —— 这就是这台板子上唯一不依赖被测通道的
出声方式。孩子侧、`wait4`、探针位置一字未改（`.data` 里那三条 `waited`/`wait4 error` 字符串随
代码一起删掉了，root 因此从 35520 B 缩到 35344 B）。

```
100000a54 <_wait_error>:  mov x1, #0x220 / b _fail        // 544 + errno
100000a5c <_wait_reaped>: ldr x24, [x0]                  // 原始 wait status
                          mov x23, #0x200                // 512
                          and x24, x24, #0xff            //   只看信号字节
                          add x23, x23, x24
                          b _exit_now
100000a78 <_exit_now>:    mov x0, x23 / mov x16, #0x1 / svc #0x80
```

三个读数（panic 屏会清掉孩子的九行，这是有意的：那张屏本身就是答案）：

| 看到 | 结论 |
| --- | --- |
| panic 屏 `exit status 512`（`a2=0x200`） | 孩子的 `exit(448)` 走完了、`wait4` 被叫醒、父亲也退出了 ⇒ 两个进程在控制台之外的一切都是好的，**缺的只是「有人往 tty 写字」这件事**：round78/79 的沉默从这一刻起与 mach、与 exit 都无关，tty 从 `msg=` 那个字节起对所有写者都不再返回 |
| panic 屏 `exit status 512+N`（N < 256） | 孩子没走到 `exit`，它是被信号弄死的（9 = SIGKILL 就是 round72 guard 异常的形状）；父亲叫醒、退出，一切正常 |
| 永远没有 panic 屏，面板停在 `msg=` | 孩子从没死 ⇒ 它确实在那次 write 里面，父亲仍在睡 ⇒ 下一轮才需要问「write 里还是 exit 里」 |

产物（`scripts/verify-clover-payload.py` 逐文件校验通过，`scripts/check-clover.py` 通过）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round80-payload.img` | 58720256 B | `6a943fc60be67f200b71109c58a5b11e4667ddc19e5ee889d4e350213f169d19` |
| `round80-scratch-data.img`（launchd.macho） | 35344 B | `e36ef0ff1f9b2a3bc18ce75d3bed54d7bd97f11fcd193f41dee7161755f4aa76`，cdhash `23ed62331718fbff985df2b44ecbe445a741da63` |
| `round80-kernel.macho.trustcache` | 56 B | `544369a3e44d11faf3c881c31bb965ff19acf20f5490b9de31071584e04e3047` |
| `round80-kernel.macho.adt` | 13476 B | `4ca556061356b8fc03d58754e3773f8384fe2d5d6992b7505721f9ffaad3539a`（每轮 `random-seed` 都换） |

卷内 `unixnu.toml` 491 B `73ffe0960b8eb8e2…`，boot-args 依旧
`-v serial=3 debug=0x14e kt_fixed_zones=120 boot-progress=2 rd=md0 allow_legacy_mach_msg=1`，
**内核侧一个字节没动**（卷内 `kernel.macho` 仍是打过入口探针的 `1c8e9a40…`，5626 字节 / 35 段）。
菜单标题 `clover md0 root: did the child ever reach the parent`。root 镜像一变，
`trust_cache_sha256` 必须换成上面那颗 trustcache。

刷机：

```sh
fastboot flash cache ~/clover-out/round80-payload.img
fastboot reboot
```

### round80 实机读数：永远没有 panic 屏 —— 孩子从没死，`exit` 第一次成为嫌疑人

面板停在 `[clover] el0 mach: msg=`，两分钟后再拍一次，**还是什么都没有多出来**：没有 panic 屏，
没有清屏带，没有任何字形。这一轮的仪器是特意换成 panic 屏的，所以这张照片读得出两件独立的事：

1. **父亲的阻塞 `wait4` 从没被叫醒** ⇒ 孩子从没死。这一条把 round78/79 剩下的「孩子已经死了只是没人打印」
   彻底排除掉。
2. **内核自己的 `kprintf` 也没有出声**：一个进程被非预期地弄死时，内核会打
   `Failed to send exception EXC_CORPSE_NOTIFY … for pid 2`，而 `kprintf` 走 video console
   （`osfmk/console/`）而不经过被测的 tty 层。开机日志尾巴上没有这行，是第二张独立证据，
   它和 panic 屏的缺席指向同一个结论。

于是嫌疑只剩两个，而且第二个是**新的**：孩子的 `exit()`。这里必须承认一块空白——
「从一个 fork+exec 出来的进程里调用 `exit`」在这块板上从来没被证明过：round70 那个退出码 1792 的孩子
是 fork 之后**没有 exec** 的副本，round72/73 读到的是 SIGKILL（`status=9`），那是被杀不是退出。
所以「标签 write 返回了、孩子死在 `exit` 里」和「write 自己堵住」这两件事，round80 的照片分不开。

### v0.2.51：孩子不停手去死，改成停下来 —— `kill(getpid(), SIGSTOP, 0)` + 父亲 `WUNTRACED`

这一轮的仪器换成第三种事件：既不是打印（通道是被审的对象）、也不是退出（它本身成了嫌疑人），
而是**停住**。`kill(getpid(), SIGSTOP, 0)` 之后，默认动作是在线程**进入 userland 的路上**办完的
（pending signal → SIGSTOP → `stop()`），所以哪怕孩子此后再也执行不了任何一条指令，只要它越过了
那次 write，父亲就会醒。父亲侧配合两处：选项加 `WUNTRACED`（= 2，`bsd/sys/wait.h:122`），
报告码从 `512 + (status & 0xff)` 改成 `512 + (status & 0xffff)` ——
`W_STOPCODE(17)` = `(17 << 8) | 0x7F` = **4479**（`wait.h:159`），只取一个字节的话它会和
「被信号 127 打死」撞车。`kill` 是这份树的 37 号（`syscalls.master:91`，
`kill(int pid, int signum, int posix)`，**Darwin 是三个参数**，`posix` 传 0 走传统 `psignal` 路径；
`NO_SYSCALL_STUB` 只是不生成 libc 桩，`exit`/`fork`/`wait4` 都带它且都实测可达）。

```
10000060c  bl   _puts              // "[clover] el0 mach: msg="  —— 被测的那次写
100000610  mov  x0, x19            // 自己的 pid
100000614  mov  x1, #0x11          // SIGSTOP = 17（不是 5；5 是 SIGTRAP）
100000618  mov  x2, #0x0           // posix = 0
10000061c  mov  x16, #0x25         // SYS_kill = 37
100000620  svc  #0x80
100000624  b.hs _kill_error
100000628  mov  x23, #0x2          // 回来了却没停 ⇒ exit(2)
10000062c  b    _exit_now
100000630  mov  x23, #0x1          // kill 失败 ⇒ exit(1)

100000a58  mov  x2, #0x2           // wait4 选项：WUNTRACED
100000a8c  and  x24, x24, #0xffff  // 保留状态低 16 位
100000a90  add  x23, x23, x24      // exit(512 + status低16位)
```

四个读数（都只看 panic 屏那一行 `exit status …`，全都不经过 tty）：

| 看到 | 结论 |
| --- | --- |
| `exit status 4991`（= 512 + 4479 = `W_STOPCODE(17)`） | **标签那次 write 返回了**，孩子活着并且已经停住 ⇒ round80 缺的那一声就是孩子的 `exit`：exec 过的进程在这块板上退出不了。信号机制反倒是好的 |
| `exit status 1024`（= 512 + `2 << 8`） | `kill` 成功返回、停却没有发生，孩子照旧退出 ⇒ EL0 用 SIGSTOP 停不住进程（一条独立的端口事实），而「越过那次 write 之后的 `exit`」是**能用**的，那 round80 的沉默只能回到 write |
| `exit status 768`（= 512 + `1 << 8`） | `kill` 自己就失败了（37 号不可达或信号被拒），孩子在错误分支退出 |
| 永远没有 panic 屏 | 孩子既没停住也没退出 ⇒ 它连 `kill` 都没走到，堵住的就是那次 write（`ttwrite` 的 `ovhiwat`→`ttysleep("ttywri", tp->t_timeout)`，`bsd/kern/tty.c:2656`，期限在这块板永不响 ⇒ 永睡） |

产物（`scripts/verify-clover-payload.py` 逐文件校验通过，`scripts/check-clover.py` 通过）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round81-payload.img` | 58720256 B | `afef7d5f67b1a6059f0135499589770dbda1e0cec75218343e2ac6cecf584cee` |
| `round81-scratch-data.img`（launchd.macho） | 35360 B | `563129327e1058ba073aed71398fa0150bad891569809874e295486f28ebc05a`，cdhash `5acf3483ea2afce1c9be6c6470f0d82ab668f7d2` |
| `round81-kernel.macho.trustcache` | 56 B | `bb790ec9e2e3539dc9763af63fa10fe13ccea2d4d39e06dcf394ec3a2a2533ac` |
| `round81-kernel.macho.adt` | 13476 B | `016cbcc382a1d353e55dcacede9b65c8c6b9037eff7769827a612b2e04330d65`（每轮 `random-seed` 都换） |

卷内 `unixnu.toml` 497 B `101f2e2c384c2601…`，boot-args 依旧
`-v serial=3 debug=0x14e kt_fixed_zones=120 boot-progress=2 rd=md0 allow_legacy_mach_msg=1`，
**内核侧一个字节没动**（卷内 `kernel.macho` 仍是打过入口探针的 `1c8e9a40…`，5626 字节 / 35 段）。
菜单标题 `clover md0 root: alive and stopped says the write returned`。root 镜像一变，
`trust_cache_sha256` 必须换成上面那颗 trustcache。

刷机：

```sh
fastboot flash cache ~/clover-out/round81-payload.img
fastboot reboot
```

### round81（v0.2.51）实机读数：永远没有 panic 屏 —— 而且这次能签字

面板照旧停在 `[clover] el0 mach: msg=`，复拍什么都不多出来：四个读数里的第四个。
它比 round80 那张同形的照片硬得多，依据是这份树里 SIGSTOP **不是**延后信号：
`kill` 的默认动作在系统调用**里面**就执行完了 —— `psignal_internal` 见
`stopsigmask`（`bsd/kern/kern_sig.c:2349`）⇒ `p_stat = SSTOP`（`:2362`）⇒ `stop()`
（`:2373`），而 `stop()`（`:3027`–`:3035`）的第一件事就是 `wakeup(parent)`，之后才
`task_suspend_internal`。所以只要那次 `kill` 进了内核，父亲的阻塞 `wait4` 必定在
syscall 里被叫醒，孩子此后还能不能执行指令完全无关。（对照：自杀式 SIGKILL 才是延后的 ——
`case SIGKILL:`（`:2416`）只做 `p_stat = SRUN` + `act_set_astbsd` + `thread_abort`，
真正的退出发生在返回 userland 的 AST 上：`locore.s:1304 check_user_asts` → `:1552
user_take_ast` → `osfmk/kern/ast.c:222` → `bsd_ast`（`kern_sig.c:3305`）→
`postsig_locked` → `exit_with_reason(..., W_EXITCODE(0, signum), ...)`（`:3137`）。
这条区别正是这一轮能下结论的原因，也是 round72/73 那个 `status=9` 从来不算「退出」的原因。）

⇒ 父亲一次都没被叫醒 ⇒ **孩子连那条 `svc` 都没碰到** ⇒ 堵住的就是 `msg=` 那次标签 write
自己，round80 剩下的两个嫌疑里孩子的 `exit` 正式出局。

同一轮把上一轮押的 `ovhiwat` 头号嫌疑也降级了，依据是 `bsd/dev/arm/km.c`：`kmoutput`
在 feed `xnu-0b4b910d` 里是 `while (tp->t_outq.c_cc > 0)` **一次排空**（每批 `buf[80]`）、
无条件清 `TS_BUSY`，`kmstart` 里那次延迟 `timeout(kmtimeout)` 还被 `#if 0` 关掉；
而 `ttwrite` 的 overfull 判据用的 `t_hiwat` 算出来是 `roundup(1216, 48) = 1248`，
比整个 cl buffer 池 `TTYCLSIZE = 1024`（`tty.c:3270`）还大 ⇒ `c_cc > hiwat` 这条判断
永远到不了。真要睡，睡的也是 `tp->t_timeout == 0`（tty 由 `kalloc_type(..., Z_ZERO)`
分配，`tty.c:3265`，km 从没赋值）的 `timo = 0` 永睡，和「期限响不响」无关。

### v0.2.52（round82）：这台板子从来没有过一次 tick

答案来自参考仓（`xVanTuring/UniXNU-tb128fu`、`xVanTuring/uefi-tb128fu`，用户给的）。
它的 `board.toml:199-212` 为同一类骁龙板子写着：`ICC_SGI1R_EL1` 写下去什么都不产生，
所以 IPI 改成软件 pended 的 SPI（`armcpu-spi-ipi=1`、`armcpu-spi-ipi-base=536`），
每个核在 `initCPU` 里给自己发一次 SGI（`armcpu-self-ipi=1`，因为 XNU 只在一个核收过
一次 IPI 之后才清它的 `SIGPdisabled`），而 `armcpu-timer-intid=19` 那句注释直说
ArmCpu 之前取的 27 是「another source that stays asserted」。**全部是 boot-arg，XNU 树一个补丁没打。**

我们这份 `drivers/ArmCpu/ArmCpu.cpp` 里 `armcpu-self-ipi`／`armcpu-spi-ipi`／
`armcpu-diagnostics` 三个开关都在（`kIpiSpiBase` 硬编码 1008），但定时器 INTID 是硬编码的 27：

```
:90   static const uint32_t kSgiPpiEnable = 0xffffU | (1U << 27);   // redistributor ISENABLER0
:565  if ((id & 0xffffff) == 27) { armcpu_timer_interrupts++; rtclock_intr (0); }
:723  enableCPUTimeBase: msr cntv_ctl_el0, #1                       // 武装的是 EL1 *虚拟* 定时器
```

而本板真机设备树（`mu-clover/Resources/DTBs/clover.dts:2233`）：

```
timer { compatible = "arm,armv8-timer";
        interrupts = <0x01 0x01 0xf08  0x01 0x02 0xf08  0x01 0x03 0xf08  0x01 0x00 0xf08>; }
```

type `0x01` = PPI，而 DT 的 PPI 编号 + 16 = 真 INTID ⇒ 四个源依次是 **17、18、19、16**，
按绑定的顺序就是 secure EL1 physical / non-secure EL1 physical / **EL1 virtual** / EL2 physical。
`enableCPUTimeBase` 武装的正是 CNTV（虚拟）⇒ 该走的那条线是 **19**，而 27 根本不在这块板子的
定时器列表里。**结论：内核一直在等一条从没被 enable 的中断线，这台板子上从来没发生过一次
tick。** 这一条从外面看就是 round77 的两次实测：`select` 的 50 ms 期限睡下去再没醒来
（`tsleep1("select", ...)` 要 `Timer::sleep` 的期限到点），以及「EL0 纯自旋饿死同板的另一个进程」
—— 没有 tick 就没有抢占，两件事是同一件事。

同一棵树里的兄弟板子还把「为什么是 27」也一并解释了：`mu-clover/Resources/DTBs/dodge.dts:7290` 写的是
`<0x01 0x0d 0xff08  0x01 0x0e 0xff08  0x01 0x0b 0xff08  0x01 0x0a 0xff08>`，PPI 13/14/11/10 ⇒ INTID
**29/30/27/26**。也就是说 27 和 19 是**同一个源**（non-secure EL1 虚拟定时器），只是两套 PPI 编号：
老树用 13/14/11/10 这套，clover 这棵新树用 1/2/3/0 这套。基仓驱动取 27 对老板子是对的，对这块板子
则是 arm 了一条 GIC 从没拿到过的线。

本轮**只动内核**：EL0 探针与 round81 逐字节相同（root 不变 ⇒ `trust_cache_sha256` 不变），
`drivers/ArmCpu` 按 `feeds.py` 的设备仓优先规则覆盖基仓模块，加两个都由 boot-arg 控制、
不传就完全无效开关；panic 屏加上驱动自己的计数器，看门狗保证「什么都不发生」这件事也一定出声。

三处改动（`drivers/ArmCpu/ArmCpu.cpp`）：

```
:103  volatile uint32_t armcpu_timer_intid = 27;                       // boot-arg 覆盖它
:104  sgiPpiEnable () = 0xffff | (1 << armcpu_timer_intid)             // ISENABLER0 两处用途（:325 / :354）
:389  PE_parse_boot_argn ("armcpu-timer-intid", ...) 只接受 16..31
:392  PE_parse_boot_argn ("armcpu-clock-panic", &clockPanic) → gClockPanic
:592  if ((id & 0xffffff) == armcpu_timer_intid) { 计数++; rtclock_intr (0);
:600        if (gClockPanic && ticks >= gClockPanic) panic ("ArmCpu: clock watchdog after %llu ticks ..."); }
```

panic 屏（`scripts/xnu-panic-screen.c`）在 `pages=`/`free=` 之后多打七行，读数全部来自
`scripts/patch-xnu-entry-probe.py` 新增的符号表（`_armcpu_timer_interrupts`、
`_armcpu_ipi_interrupts`、`_armcpu_ipi_sent`、`_armcpu_irq_id`、`_armcpu_irq_phase`、
`_armcpu_timer_intid`、`_armcpu_gic_boot`）：

```
ticks=<定时器中断次数>  ipi=<收到的 IPI 次数> sent=<发出的 IPI 次数>
irq=<最近一次分发出来的 INTID> phase=<中断处理走到第几步> intid=<当前定时器 INTID>
typer=<GICD_TYPER 读回值，最大 INTID = 32 * (typer & 0x1f) + 31>
```

**这一轮只回答一个问题：给了时钟之后，那块板子动不动。** 判定表：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| panic 屏 `exit status 4991` | 时钟活了，标签 write 返回，SIGSTOP 送达 ⇒ round80/81 缺的只是父亲自己的打印 | 把打印收回 tty，进 `exec` 之后的正常流程 |
| panic 屏 `exit status 1024` / `768` | write 返回了（时钟治好的是那次写），但 EL0 停不住进程／`kill` 不可用 | 单独问信号 |
| panic 屏 `ArmCpu: clock watchdog ...`，`ticks=0x7530`（= 30000） | **时钟在走**，两个进程照旧卡在那次 write 里 ⇒ 时钟无罪，回到 `ttwrite`/`ttywait`/`vcputc_lock` | 用 `irq=`/`phase=` 判断是不是中断上下文里卡的 |
| panic 屏 + `ticks=0`（看门狗没响，是别的 panic） | INTID 19 也没让 tick 起来 | 用 `typer=` 核算这份 GIC 的 INTID 上限，换 PPI 编号（16/17/18）再试一轮 |
| 完全没有任何 panic 屏，仍是 `msg=` 半行 | 看门狗没触发 ⇒ tick 次数为 0，且没有别的 panic：INTID 19 还是错的线 | 同上 |

产物（`scripts/verify-clover-payload.py` 逐文件校验通过，`scripts/check-clover.py` 通过）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round82-payload.img` | 58720256 B | `c30a78932d793925ba566f1a542c76ee50729ec01ba94f4ad79dbdf1b9e7e798` |
| `round82-scratch-data.img`（launchd.macho） | 35360 B | `563129327e1058ba073aed71398fa0150bad891569809874e295486f28ebc05a`，与 round81 逐字节相同 ⇒ 探针没动 |
| `round82-kernel.macho.trustcache` | 56 B | `bb790ec9e2e3539dc9763af63fa10fe13ccea2d4d39e06dcf394ec3a2a2533ac`，与 round81 相同 ⇒ `trust_cache_sha256` 不用改 |
| `round82-kernel.macho.adt` | 13476 B | `7b9461b255e7e958902bfca7777192ada07088a55def94b7329aaa33273754fd`（每轮 `random-seed` 都换） |

卷内 `unixnu.toml` 535 B `e65d5cc946e29609c568cb3ca64487bdd9752356a04b7913d6a28db35540224b`，
boot-args 新增 `armcpu-timer-intid=19 armcpu-clock-panic=30000`；**内核侧这次真的动了**：
卷内 `kernel.macho` 从 `1c8e9a40…` 变成 `3c3e1e632b3587ac489eb2a0a2893fc2bb102c2919fe92af35994e59b3b38359`
（51802112 B，探针补丁 5810 字节 / 35 段，与 round81 同尺寸；未打补丁的那份是
`477230721d729b32d969b50edf54bec8d285668f49fdf1e0b96bef6608cbb4af`）。`strings` 在两份额里都能读到
`armcpu-timer-intid`、`armcpu-clock-panic` 和看门狗那句格式串，说明设备仓那份驱动确实进了这次链接。
菜单标题 `clover md0 root: intid 19 gives the board a clock`。

这一轮**故意不传** `armcpu-self-ipi` 与 `armcpu-spi-ipi`：板上只有 boot core 在跑（单核），
而且 `kIpiSpiBase` 硬编码的 1008 有没有超出本板 GIC 的 INTID 上限，要等下一张照片里的 `typer=` 说话。

刷机：

```sh
fastboot flash cache ~/clover-out/round82-payload.img
fastboot reboot
```

### round82（v0.2.52）实机读数：时钟活了，死因换成 `AMFI: No PMGR?`

判定表里那五格**一格都没命中**，照片落在表外，而且一次给齐三件事：

| 读数 | 含义 |
| --- | --- |
| panic 屏存在，内容 `"AMFI: No PMGR?"` `@ConfigurationSettings.cpp:235` | 面板不再只有半行 tty 文字：内核自己把死因写出来了。上一窗口那句硬要求「永远没有 panic 屏；这个东西是一定要弄没」到这一刻结掉 |
| `ticks=0x14e5`（5349） | **INTID 19 是对的，这块板第一次有了 tick**：`enableCPUTimeBase` 武装的 `CNTV_CTL_EL0` _virtual_定时器，中断线正是 PPI 3 → INTID 19。看门狗没响是因为内核在 5349 次就死了，没到 30000 |
| `ipi=0 sent=0`、`irq=0x3ff`、`phase=0` | 与单核相符（只有 boot core 在跑，没人发过 IPI）；`irq=0x3ff` 是 `armcpu_irq_id` 的初值，说明分发计数器没被非定时器路径写过 |

`RA=0xfffffff004ac9420 LR=0xfffffff004aaed08 fmt=0xfffffff0042dded2`，日志尾部是
`VM bootstrap done: 35 maps, 112 entries and 117 holes left`、`IOKit IORegistrySnapshot ENABLED`、
`RTKit subsystem is initialized`、`RTBuddy configured`、`Log queue configured`；覆盖在下面的
是 `AppleBSIOSIManifest: Error: Failed to open reg-entry /chosen/secure-boot-hashes` 那一组
`/chosen/iboot` 缺失的报错（本板没有 iBoot，无害）。`intid=` 与 `typer=` 两行正好落在
日志尾与覆盖文字之间，被吃掉了。

**为什么修好时钟反而多出一条 panic。** 基仓 `scripts/audit-amfi-pmgr-contract.py` 把这份
未修改的 AMFI 对 PMGR 的依赖钉死了：AMFI 导入 `IOService::serviceMatching` 与
`IOService::waitForMatchingService`，用字符串 `ApplePMGRNub`（`0xfffffff0042dde02`）建匹配表，
取到 nub 之后读它的 provider（`nub+0x88`），调用 provider vtable 第 320 槽（读）拿到
一个 32 位值、`OR 0x4`、再交给第 321 槽（写）。目标机上 provider 是 `AppleT8011PMGR`，
那两槽静态就是「读返回 `0xffffffff`、写什么都不做」。所以**这一步要的不是电源功能，
而是「IORegistry 里存在一个 class 名为 `ApplePMGRNub` 的服务」**。基仓
`scripts/check-owner-dyld-startup-checkpoints.py:73-85` 进一步钉住：这个 panic 的调用栈是
`_thread_call_invoke` → `_thread_call_thread` → `_Call_continuation`，也就是**发生在 dyld
启动期的一个 thread-call 工作线程上**，发生在第一个 exec 之后。

这就把 round70-81 与 round82 的差别解释清楚了：

- 没有 tick 的那十一轮，`waitForMatchingService` 的绝对期限**永不响应**（正是 round77 测到的
  「任何 timed wait 都不会返回」），那个线程只是永久吊着，不 panic，启动照旧走到 EL0 —— 所以
  当时看得见两个进程、看不见 AMFI。
- 有了 tick 的这一轮，期限真的响了，匹配返回空，AMFI 立刻 `panic`。**时钟是显形因素，不是元凶。**

由此两条旧结论要重新排队，都要在有 tick 之后重测：round77 的「EL0 纯自旋会饿死同板另一个进程」
（缺的其实是抢占定时器）与「`select` 的期限睡下去就叫不醒」（缺的其实是 callout）。tty 的
`msg=` 悬案也一样：round78-81 用「缺行」推断出来的卡点，全都发生在一个内核线程正卡在
永不超时的等待里、且从没有过一次 tick 的板子上。

### AMFI 的 PMGR 契约在这块板上核对过了（不需要刷机）

`com.apple.driver.ApplePMGR` 本来就在 `board.toml` 的 prelink roots 里（第 69 行），
`out/clover/kernel/kernel.json` 的 `components[8]` 确认它进了我们这颗内核 —— 类名
`ApplePMGRNub` 是注册着的，只是没有任何实例（Snapdragon 的 DT 里没有 apple 的 compatible，
驱动永远不 probe）。sheng 的解法（公开仓 `UniXNU/UniXNU-sheng`，本窗口拉下来读了
`package/ShengPlatformPower.cpp` 与 `scripts/build-sheng-platform-power.py`）是：

1. `OSKextLoadKextWithIdentifier("com.apple.driver.ApplePMGR")` 把那颗已预链接的 kext 起起来，
   按类名查出 `ApplePMGR`（`getClassSize() == 0x4b8a8`）与 `ApplePMGRNub`（`0x740`）；
2. 用一份**重建出来的原始 ABI 头**（`IOPMGR` 之上补私有填充到 `0x4b8a8`，虚表槽 177..339 全部
   声明为纯虚并 override 成 `panic`，只有第 320/321 槽 `readAon`/`writeAon` 保留原实现）派生出
   一个「守卫壳」provider：任何真正的电源操作都会大声 panic，只有 AMFI 要的那对读写是通的；
3. 用原始 kext 的工厂函数造出真正的 `ApplePMGRNub`，`attach`/`start` 到 platform 上并
   `registerService(kIOServiceSynchronous)`，再自己用 `waitForMatchingService(..., 1000000)`
   复验一次匹配到的就是那颗 nub；
4. 钩子位置由基仓决定：`patches/xnu/zzz-boot-progress.patch` 已经在
   `bsd/kern/bsd_init.c:776-781`（`bsd_bufferinit()` 之后、`IOKitInitializeTime()` 之前）留了
   `#if XNU_SHENG_PMGR` 的 `sheng_pmgr_run()` 调用点，`scripts/build-xnu-4k-baseline.sh` 用
   `XNU_SHENG_PMGR` 控制该宏并 `-Wl,-force_load` 那颗
   `build/sheng-platform-power/libShengPlatformPower.a`；`CONFIG_XNU_SHENG_PMGR=y` 就是打开它的开关。
   `XNU_BOOT_PROGRESS` 关掉时 `boot_progress_stage(s)` 是无参宏（`osfmk/kern/boot_progress.h:25`），
   所以 clover 不须为它把 BOOT_PROGRESS 打开。

sheng 钉住的那些绝对地址，本轮直接拿我们自己的 `out/clover/kernel/kernel.macho` 逐字节验过，
**全部一致**（同一份 kernelcache `f18b4df8…`、同一份 roots 列表、同一个 `-0x1000000` slide）：

| 检查 | 我们内核里的读数 |
| --- | --- |
| `0xfffffff0042dde02` 的 13 字节 | `b'ApplePMGRNub\x00'` |
| AMFI 的 read/OR/write `0xfffffff004aaeb40` | `0x94006c46 0x321e0015 0xaa1403e0 0xaa1503e1 0x94006c45`（契约要求逐条相同） |
| nub 侧 read helper `0xfffffff004a24adc` | `f9404400 b4000080 f9400008 f9450101 d61f0020 12800000 d65f03c0` |
| nub 侧 write helper `0xfffffff004a230ec` | `f9404400 b4000080 f9400008 f9450502 d61f0040 d65f03c0` |
| 基类 stub `0xfffffff004a20fbc` | `12800000 d65f03c0 d65f03c0`（MOV W0,#-1 / RET） |
| `0xfffffff005cfac08`、`0xfffffff005cfac10` 两个指针槽 | 正好指向上面那对 helper |
| 基 vtable `0xfffffff005ce26d8 + 320*8` | `0xfffffff004a20fbc 0xfffffff004a20fc4` |
| AMFI 的两个导入 | `__ZN9IOService15serviceMatchingEPKcP12OSDictionary`、`__ZN9IOService22waitForMatchingServiceEP12OSDictionaryy` |
| `__ZTV6IOPMGR` | `0xfffffff0071b8ed0`（在，vtable 逐槽复验的基准） |

照片里的 `LR=0xfffffff004aaed08` 就落在这处 read/OR/write 的邻域，`fmt=0xfffffff0042dded2`
落在 `ApplePMGRNub` 字符串的邻域 —— 实机与静态对上了。

### v0.2.53（round83）：给 AMFI 发布一颗它自己能匹配上的 nub

**这一轮只回答一个问题：板上有了 `ApplePMGRNub` 服务之后，未修改的 AMFI 还 panic 不 panic。**
EL0 探针一字未动（`scratch-data.img` 与 round81/82 逐字节相同），root 与 trustcache 也没动，
所以照片里凡是走到 initproc 的部分都还是 round81 那道题。

改动全在「让那块板子多出一个电源管理提供者」这一件事上：

| 位置 | 改了什么 |
| --- | --- |
| `drivers/CloverPMGR/CloverPMGR.cpp` | 新增。守卫壳 provider + 用原始工厂造真 nub，形状照 `UniXNU-sheng`，但**每一条失败路径都 `panic` 并报出 stage**（详见 `drivers/CloverPMGR/README.md`） |
| `scripts/build-clover-pmgr.py` | 新增。生成 ABI 头与守卫槽表、复用 `IOPlatformExpert.cpp` 的编译记录编译、逐槽复验 340 项虚表、把 8 颗原始绑定写成绝对符号，产出 `build/sheng-platform-power/libShengPlatformPower.a`（路径与名字是基仓硬编码的） |
| `configs/clover_defconfig` | `CONFIG_XNU_SHENG_PMGR=y`：基仓据此给 `bsd_init.o` 加 `-DXNU_SHENG_PMGR=1` 并 `-Wl,-force_load` 上面那颗归档 |
| `board.toml` | `[kernel] archives` 加 `build-clover-pmgr.py`；`kernel_args` 加 `clover-pmgr=2`、**去掉 `armcpu-clock-panic=30000`**（时钟已实证在走，留着只会用一次定时器 panic 盖住 EL0 的读数）；菜单标题换成 round83 语义 |
| `scripts/patch-xnu-entry-probe.py` | 诊断符号表加 `_clover_pmgr_observation`（128 字节，缺失即构建失败） |
| `scripts/xnu-panic-screen.c` | panic 屏多打三行：`pmgr=/status=/mode=`、`nub=/attached=/registered=`、`aon=…=>…` |
| `scripts/check-clover.py` | 交叉断言：defconfig 开关 ↔ archives 里的 builder ↔ `clover-pmgr=2` 三者必须一致（已用「临时删掉 `clover-pmgr=2`」验证这条断言真的会报错） |
| `scripts/verify-clover-payload.py` | 多一项 `kernel.macho: original-PMGR bindings alive`：在**产出的内核**里查 5 颗原始导出符号是否仍指向被绑定的地址，并确认 `CloverPMGR` 的 panic 串在镜像里 |

刷机前的一处自撞：改了 `configs/clover_defconfig` 并不会自动生效，构建读的是
`out/clover/.config`，要跑 `./unixnu defconfig clover_defconfig`（注意 `defconfig` 收的是**文件全名**）。
第一次 `build kernel` 因此静默出了一份没有 PMGR 的内核，是靠 `grep -ac CloverPMGR kernel.macho`
返回 0 才发现的 —— 凡是「新代码进没进产物」都要有独立证据，不能靠构建成功。

构建侧读数（不需要刷机）：

```
pmgr: …/build/sheng-platform-power/libShengPlatformPower.a (256800 B)
pmgr: reference kernel.macho 616f4a4c8c77345f…, 340 virtual entries, 8 original bindings
PASS: original AON pair inherited, every other power slot panics, no AMFI byte modified
```

`reference` 是本次 relink 出来的那份**不含 PMGR** 的内核（`616f4a4c…`），它仍然逐条满足
`audit-amfi-pmgr-contract.py` 的期望指令 —— 也就是说这份契约扛住了一次重新链接，不是只对
round82 那份成立。加上 verify 在**含 PMGR** 的最终镜像里复查 5 颗符号 `0 moved`，两侧都闭合了。

产物（`scripts/verify-clover-payload.py` 全绿，`scripts/check-clover.py` 通过）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round83-payload.img` | 58720256 B | `aa334f745f5f8064eee1c095c7f7483653784bbc706c7fd9fcf13e5cbe94fcb2` |
| `round83-kernel.macho`（卷内、打过入口探针） | 51830784 B | `e98b96594ff730e525845828c6ebb86c2e4cc38a5652f6b82ab8a2f8db0eef24`（未打探针的那份是 `44866929375b6ba9c83d9fda95e30628d4207e3e2ebf5d4370360562c5fa52a1`，`v0.2.54` 只改 panic 屏、所以它一字未动；比 round82 长 28672 B，就是这颗归档） |
| `round83-scratch-data.img`（launchd.macho） | 35360 B | `563129327e1058ba073aed71398fa0150bad891569809874e295486f28ebc05a`，与 round81/82 逐字节相同 ⇒ 探针没动 |
| `round83-kernel.macho.trustcache` | 56 B | `bb790ec9e2e3539dc9763af63fa10fe13ccea2d4d39e06dcf394ec3a2a2533ac`，不变 ⇒ `trust_cache_sha256` 不改 |
| `round83-kernel.macho.adt` | 13476 B | `f00fc42d3b798ca22a9736561d0ce46fd3dff3711578d52458cf38d9be07c355`（每轮 `random-seed` 都换） |
| `round83-unixnu.toml` | 537 B | `8fc7f13b69198dc3d42af4d038dd4ea9a17d4d4e8e0a2be02287192dc5e85fe1`，args 末尾 `armcpu-timer-intid=19 clover-pmgr=2` |
| `round83-pmgr-build-report.json` | 1092 B | `b61ea6bb06c0a7241c128a8d8c698c9b59669bd7a39c8b291c43e1858e6a4d48`：8 颗绑定地址、`archive_sha256 448a6919e5d310fb…`、`source_sha256 cb09278881bfedb4…` |

### v0.2.54（还是 round83 那张照片）：panic 屏把读数画在前面

`v0.2.53` 推完之后回查 round82 丢尾行的机制，结论是**画不出来的字符会静默消失**：`put()`
里有 `if (g->y + line > g->h) return;`（`scripts/xnu-panic-screen.c:159`），字符高度 `14*SCALE=42`，
起点在 `h/8 + 8`，一旦累计行数（长行还会折行）越过面板下沿，后面的每一行一个字都不落，
既不报错也不留痕。round82 丢的正好是 `intid=` 与 `typer=` —— 它们排在那 12 行寄存器证据后面。
本轮又新加 3 行，按老顺序会把 `pmgr=` 系列推到同样的位置，所以把**读数整组挪到寄存器证据之前**：

```
消息 → pages/free → ticks/ipi+sent/irq+phase+intid+typer → pmgr+status+mode / nub+attached+registered / aon=…=>…
     → RA/FP/LR/fmt/args/m2 m1/a0 a1/a2 a3/a4 a5/TCR/SCTLR
```

只动这一处，`out/clover/kernel/kernel.macho`（`44866929…`）因此不变 —— panic 屏是 payload
阶段由 `patch-xnu-entry-probe.py` 注进卷内副本的。重新 `build payload` 后
`verify-clover-payload.py` 照旧全绿（`probe patch localized`，`original-PMGR bindings alive OK (5 bindings, 0 moved; provider text present)`），
上面三个产物哈希就是重排之后的。判定表与刷机命令不变。

刷机：

```sh
fastboot flash cache ~/clover-out/round83-payload.img
fastboot reboot
```

判定表（`pmgr=` 是那 16 个观测槽的前三个）：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `CloverPMGR: failed at stage N status …` | 发布失败，N 直接指出哪一步：1 不认识的模式 / 2 kext 起不来 / 3 类名或类尺寸不符 / 4-5 元类注册 / 6-7 分配与 `init` / 8 nub 身份 / 9 `nub+0x88` 不是我们的 provider / 10 AON 读写自检 / 11-14 `attach`/`start` / 15-17 AMFI 那条匹配路径自己没找到这颗 nub | 按 N 单独修，别猜 |
| `CloverPMGR: unimplemented power operation S` | 有人真的调了第 S 号电源槽 ⇒ 守卫壳被用在了它不该用的地方 | 看是谁调的，那一槽才是需求 |
| panic 屏是 `"AMFI: No PMGR?"`，而 `pmgr=0x5 status=0x0` | 发布成功、自验也成功，AMFI 仍然报没有 PMGR ⇒ **契约理解有缺**（它要的不止是一颗匹配上的 nub） | 拿 `aon=` 的两个值查它读回来的到底对不对 |
| panic 屏 `… (signal 0, exit status 512+低16位)`（**这一格作废**：那条通道只有 8 位，见下面两节） | 已过 AMFI，回到 round81 那道题：`4991`=孩子停住了（write 返回了，堵的是 `exit`）；`1024`/`768`=信号语义；**永远没有 panic 屏**=仍堵在 `msg=` 那次 write | 按 round81 的判定表继续 |
| 完全没有任何 panic 屏，且面板停在 `msg=` | 走到了 EL0，且没有任何 panic —— 与 round81 同形，但这次**是有 tick 的**，所以 round77 的两条结论（自旋饿死同板进程、`select` 睡下去叫不醒）都必须在有 tick 之后重测 | 下一轮专测这两条 |

### round83 实机读数：命中最后一格，PMGR 这条路通了

照片：**还是只有 `msg=`，后面什么都没有，而且没有 panic 屏**。这一张同时说完两件事：

1. **`drivers/CloverPMGR` 起作用了。** round82 那张照片上的 `"AMFI: No PMGR?"` 消失，boot 第一次
   在「有 tick」的状态下一路走到 EL0 并印出孩子的九行 —— 而 CloverPMGR 的设计是**任何一步失败都
   `panic` 并报 stage**（静默 bail 的照片与 round82 一模一样，所以两种情况在面板上分得开）。
   既没有 `failed at stage N`，也没有 `unimplemented power operation S`，也没有 `"AMFI: No PMGR?"`，
   剩下的唯一解释是：provider 造出来了、nub 用原始工厂挂上并注册了、AMFI 自己那条
   `serviceMatching("ApplePMGRNub")` 匹配到了它。契约就是契约 —— 它要的确实只是「registry 里
   有这么一颗服务」，不是电源功能。
2. **`msg=` 这道题回到了台面上，而且它最要紧的那个借口没了。** round79-81 之所以切不出「标签
   write 回没回来」，是因为那台板子的期限从来不响：`ttwrite` 里任何一次带 `t_timeout` 的睡、
   任何一次指望 `timeout()` 兜底的 `kmstart`，在没有 tick 的板上都等价于永久阻塞。现在有两条独立
   证据说明期限会响 —— round82 那次 `"AMFI: No PMGR?"` 本身就是 `waitForMatchingService` 的
   deadline **到期返回空**才产生的 panic（没 tick 的十一轮里那次等待只是无声吊着，启动照样走到
   EL0），而 `ticks=0x14e5` 是内核自己的计数。⇒ 如果标签 write 在有 tick 之后仍然不返回，
   堵点就在 tty 输出路径内部，而不再是「没人来叫醒它」。

所以 round84 不再加新仪器，只做一件事：**把 round79 那一刀在有 tick 的板上重切一次** ——
孩子回到「标签 write 之后立刻 `exit(448)`」（删掉 round81 的 `kill(SIGSTOP)` 块），父亲一字不动
（阻塞 `wait4` + `WUNTRACED`、醒来后不写任何字符、`exit(512 + 状态低 16 位)`，用 panic 屏说话）。
读数是三格：`exit status 115200`（= `512 + (448<<8)`）= 标签 write 返回了、而且 fork+exec 出来的
进程 `exit()` 也通 ⇒ round81 的「堵在 write 里」是「没有时钟」的产物；`512 + N`（N<256）= 孩子这次
是被信号 N 弄死的（新现象，log 尾巴上的 corpse notify 会指着它）；**永远没有 panic 屏** = 有 tick
也堵 ⇒ 下一刀要切进 `kmoutput`/`ttwrite` 里面，而不是继续绕着那次 write 打转。

原计划里还有一条「给 CloverPMGR 的成功路径补一句 `IOLog`」，本轮**取消**了：那张照片自己就带着
正面证据 —— AMFI 印了 `AMFI: PMGRAON latched to: …`。这条串在内核里的 VA 是
`0xfffffff0042dde0f`，紧接在 `0xfffffff0042dde02` 那颗 `'ApplePMGRNub\0'` 后面 13 字节，也就是
round82 那个 `fmt=0xfffffff0042dded2` 的同一段；能印出它，说明 AMFI 确实拿着我们发布的 nub 走完
了「slot 320 读 AON → OR 0x4 → slot 321 写回」。加一条 `IOLog` 要重建内核，而它换来的信息与照片
重复，所以不动。

### 关于「多出来一行 `parent: status = 114688`」：不是本轮的镜像，而且它牵出一个真 bug

先把话说明白：**`v0.2.54` 的镜像打不出这一行。** 三条独立证据：

| 检查 | 结果 |
| --- | --- |
| `round83-payload.img` 原始字节里搜 `parent: status` / `status = ` | 0 / 0 |
| 卷内根镜像（35360 B）里全部 `[clover]` 标签 | 7 组，没有任何一条含 `status`；含 `waited pid= status=` 的是 round78/79 那两颗 35520 B 的镜像 |
| 全部 43 份归档 payload 扫 `status = `（带空格） | 每一份都是 0 —— 这个字面量从来没进过任何产物 |

而 `114688 = 448 << 8` 恰好是 round78/79 那版父亲打印的**原始 wait 状态**（孩子 `exit(448)`）。
所以那一行要么来自旧照片，要么那次开机刷的还是 `round79-payload.img`。

顺着这个数字查下去，查出了一件更要紧的事：**父亲用来报数的通道只有 8 位，round80-83 的编码
在到达面板之前就被折掉了。** initproc 退出走的是

```c
panic_with_options(..., "%s -- no exit reason available -- (signal %d, exit status %d %s)",
                   prefix_str, WTERMSIG(rv), WEXITSTATUS(rv), ...);   // bsd/kern/kern_exit.c:1875
```

而 `exit(C)` 先变成 `rv = W_EXITCODE(C, 0) = C << 8`（`kern_exit.c:1466`），
`WEXITSTATUS(x) = (x >> 8) & 0xff`（`bsd/sys/wait.h:144`）。两头一夹，屏幕上只可能出现
`C & 0xff`：

| 我们写的码 | 面板上会印 | 后果 |
| --- | --- | --- |
| round80-83 的 `512 + (status & 0xffff)`，孩子 `exit(448)` → `115200` | `exit status 0` | 与「孩子 `exit(0)`」完全同形 |
| 同上，孩子被 SIGSTOP 停住 → `4991` | `exit status 127` | 正是当初加低 16 位想躲开的「signal 127」撞车 |
| 同上，孩子被信号 N 杀死 → `512 + N` | `exit status N` | 只有这一格是好的 |

好消息是这条通道还没骗过我们：round80/81 的读数都是**「完全没有 panic 屏」**，缺席不受 8 位截断
影响；round82/83 那两张 panic 屏分别是 AMFI 与 PMGR 的，跟父亲的码无关。所以这个 bug 一直没机会
误导判断 —— 但它会在 round84 命中答案的那一刻正好把它抹平（正解 `exit(448)` 会读成 `0`）。

### round84（v0.2.55）实机读数：命中最后一格，而且这张照片第一次把孩子的 write 判了无罪

两张照片（已确认刷的是 `round84-payload.img`，第二张隔两分钟复拍，**什么都没多出来**）：面板停在
`[clover] el0 mach: msg=` + 光标，**没有 panic 屏**。⇒ 判定表最后一格成立：有 tick、有
`ApplePMGRNub` 的前提下，那次 write 之后仍然没有任何报告回来，下一刀必须切进 tty 输出路径内部。

但这一张是十轮里信息量最大的一张，因为它是第一张能把孩子的**全部**历史读出来的照片。底部八行逐字：

```
[clover] el0 init: pid=1 console fd=0 pagesize=4096
[clover] el0 init: forked pid=2 x1=0
[clover] el0 child: alive
[clover] el0 child: pid=2 ppid=1 x0=2 x1=1
[clover] el0 child: exec /sbin/launchd
[clover] el0 init: pid=2 console fd=1 pagesize=4096
[clover] el0 mach: thread=259 task=515 host=771
[clover] el0 mach: reply=1827
[clover] el0 mach: msg=|
```

三条新事实，每条都是以前只能靠推断的：

1. **exec 出来的孩子，`write` 已经成功返回过至少八次。** 每行都是标签 + 数字 + 换行的多次
   `_puts`/`_print_dec`/`_newline` 调用，`_write_console` 一旦拿到 errno 就走 `_fail`→`exit`，
   而这些行全都印完了下一行才出现 ⇒ round80 立起来的「exec 出来的孩子 syscall 不可用」这条嫌疑
   **正式出局**。
2. **不带换行的裸 write 也能返回。** `thread=`、` task=`、` host=` 三段都是 `_puts` 的裸标签，
   后面才跟数字 ⇒ 「只有以 `\n` 结尾的写会回来」这个猜测不成立。
3. **屏幕滚动也已经证明可用。** `reply=1827` 那行的换行把整屏往上推了一行，才腾出 `msg=` 现在
   这一行 ⇒ `gc_scroll_up` / `vc_scroll_up` / `video_scroll_up` 这条路径不是堵点，round85 原本
   想问的「停点是不是跟着面板最后一行走」被这张照片自己答掉了。

于是 `msg=` 这次 write 与前八次成功的写，**在内容、长度、结尾、光标位置上都没有区别**——它是
第九次。这把「tty 输出路径有某个必堵的条件」这个形状削弱了，同时暴露出 round84 设计时没算到的
第三种状态。这张照片**兼容三件事**，而且三件事的读数一模一样：

| 状态 | 为什么这张照片分不开 |
| --- | --- |
| (a) 标签那次 write 画完字就没回来 | 唯一的报告通道正是那条 tty |
| (b) write 返回了，孩子的 `exit(448)` 回不来 | 孩子活着 ⇒ 父亲睡在 `wait4` 里 ⇒ 没有 panic 屏 |
| (c) write 和 `exit` 都办了，父亲的阻塞 `wait4` 没被叫醒 | 报告码只能由父亲打印，而它还在睡 |

(b) 与 (c) 都不是新嫌疑（round81 用 `SIGSTOP` 排除过 (b)，但那次的结论建立在「`kill` 进了内核
父亲必定被叫醒」上，同样没排除 (c)），而 round70 那个能叫醒父亲的孩子是 **fork 未 exec** 的：
exec 换掉地址空间时 `childproc` 指针是否还连着，本板从未验证。

⇒ **round85 只解决一件事：换一条既不经过 tty、也不经过父亲的通道。**

### v0.2.55（round84）：孩子切在两次 write 之间，父亲的码改成 8 位以内

改动两处，都不碰内核：

| 位置 | 改了什么 |
| --- | --- |
| `scripts/clover-el0-init.S` 孩子 | 删掉 round81 的 `kill(getpid(), SIGSTOP, 0)` 整块，换成标签 write 之后立刻 `exit(448)`（数字 write 留在原地不执行，下一轮只删上面这块即可） |
| `scripts/clover-el0-init.S` 父亲 | 不再算 `512 + 低16`，改成按 `wait.h` 的规矩解状态字后**手工落在 0..255 的三个带里**：`0` / `1` / `32+N` / `96+N` |
| `board.toml` | `trust_cache_sha256` 跟着新镜像换成 `b5b064c4…`；菜单标题换成 round84 语义；boot-args 一字未动（`clover-pmgr=2` 与 `armcpu-timer-intid=19` 都留着） |

反汇编复验过编译结果（`objdump -d --macho build/clover-root/launchd.macho`）：孩子那边是
`mov x23, #0x1c0` + `b _exit_now`，紧跟其后不可达的 `_print_line`；父亲那边是
`and x0, x24, #0x7f` → `cbnz _not_exited` → `lsr x24, #8` / `and #0xff` / `cmp x24, #0x1c0`，
`_not_exited` 分 `cmp x0, #0x7f`（停 → `+0x60`）与 `_signaled`（`+0x20`）。

产物（`verify-clover-payload.py` 全绿：`original-PMGR bindings alive OK (5 bindings, 0 moved)`；
`check-clover.py` 通过；`round84-kernel.macho` 与 round83 逐字节相同 `e98b9659…`，本轮没碰内核）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round84-payload.img` | 58720256 B | `a7df626cd9aaab2c756c5e03742eec14c68884fcfafb63e7f7324acb77fe7683` |
| `round84-scratch-data.img`（launchd.macho） | 35424 B | `b104481bc98d1e91419a0a97df619a18ae286d57f27f60dddc8d39b77c07cc04`，cdhash `0875709718f5555d498dbf6c3b21ecd5e386624c` |
| `round84-kernel.macho.trustcache` | 56 B | `b5b064c41801cab0f0386cda47059963b5a150a31b09e4b4d862e57fd7c97099` ⇒ `trust_cache_sha256` 已同步 |
| `round84-kernel.macho.adt` | 13476 B | `68cb44f674ead4b99a14b8f15e62b2326adbbedcb1bb092e1c58cdbe65b44e9b`（每轮 `random-seed` 都换） |
| `round84-unixnu.toml` | 539 B | 标题 `clover tty: exit between the two writes, now that the clock runs` |

刷机：

```sh
fastboot flash cache ~/clover-out/round84-payload.img
fastboot reboot
```

判定表（panic 屏第一行末尾那个 `exit status`，只有一位数到三位数）：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `exit status 0` | 标签 write **返回了**，而且 fork+exec 出来的进程 `exit()` 能叫醒父亲 ⇒ round81 的「堵在 write 里」是没有时钟的产物，`msg=` 之后的一切重新变成活题 | 把探针放回去，回到 `mach_msg` 那道题 |
| `exit status 1` | 孩子正常退出了，但不是 `exit(448)` —— 它从 `_fail` 走的，切点之前有调用回了 errno | 看控制台那行有没有多出来的数字 |
| `exit status 32+N` | 孩子被信号 N 弄死（`33` = SIGKILL）；有 tick 之后才可能出现的新现象 | log 尾巴上的 corpse notify 会指着同一次死亡 |
| `exit status 96+N` | 孩子停在了信号 N 上，本轮没人要求停 | 查是谁给它发的 |
| `exit status 32`（单独） | 父亲的 `wait4` 自己失败（`_wait_error` 的 `544+errno` 低 8 位） | 多半是 ECHILD，即 fork 那侧的问题 |
| **永远没有 panic 屏** | 有 tick 也堵 ⇒ 堵点确实在 tty 输出路径内部 | 下一刀切进 `kmoutput`/`ttwrite`，不再绕着那次 write 打转 |

### v0.2.56（round85）：孩子自己把内核 panic 掉，问「那次 write 到底回没回来」

只改一处，仍然不碰内核（`round85-kernel.macho` 与 round83/84 逐字节相同 `e98b9659…`）。孩子在
`msg=` 标签 write 之后不再 `exit(448)`，改成调 `reboot(2)`：

| 寄存器 | 值 | 依据 |
| --- | --- | --- |
| `x0` | `0xc00` = `RB_QUICK(0x400) \| RB_PANIC(0x800)` | `bsd/sys/reboot.h:89-90` |
| `x1` | `0x10000415e` = `"clover-el0: write returned\0"` | `copyinstr` 要 NUL，`.asciz` |
| `x16` | `55` | `syscalls.master:109` `{ int reboot(int opt, char *msg) NO_SYSCALL_STUB; }` |

选这一条通道是因为它把被审查的两样东西全部绕开了：`kern_xxx.c:182` 判的正是 `RB_PANIC \| RB_QUICK`
这一对，命中就立刻 `panic_kernel()` → `panic_with_options(.., "userspace panic: %s", message)`
（`kern_shutdown.c:167`），**在 `system_inshutdown` 之前、在等 swap 线程之前、在 sync 与卸载之前**，
所以没有任何东西能把它卡在这块没有真文件系统的板上；`message` 由 `kern_xxx.c:132` 的 `copyinstr`
抄进内核，指针写错也只会变成 `"user space RB_PANIC message copyin failed"` 照旧 panic。而且这份
`DEVELOPMENT_ARM64_BCM2837` 内核对 `RB_PANIC` 连 `suser`（`:116`）和 MACF 的 reboot 检查（`:142`）
两处都跳过 —— 需要授权的事情恰恰是这个移植反复踩空的地方。

`reboot` 万一返回，孩子用 `exit(449)` 离开（**故意不是 448**），父亲的 `_exited_other` 读成 `1`。
父亲的解码块、`_print_line`、boot-args 一字未动。

反汇编复验（`objdump -d --macho build/clover-root/launchd.macho`）：

```
100000610:  adrp x1, 4 ; 0x100004000        100000624:  mov x23, #0x1c1
100000614:  add  x1, x1, #0x15e             100000628:  b    _exit_now
100000618:  mov  x0, #0xc00                 10000062c:  mov  x0, x20      <- 不可达的数字 write
10000061c:  mov  x16, #0x37                 100000630:  bl   _print_line
100000620:  svc  #0x80
```

消息串在文件偏移 `0x415e`，26 字节 + NUL，全镜像只有一份。

产物（`verify-clover-payload.py` 全绿：`scratch-data.img == signed root OK`、
`trustcache == published bundle OK`、`original-PMGR bindings alive OK (5 bindings, 0 moved)`；
`check-clover.py` 通过）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round85-payload.img` | 58720256 B | `2873df4fda59ebb663a2f4e549daa2fe3d9b991667972d8d61e7cc06e75edccb` |
| `round85-scratch-data.img`（launchd.macho） | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084`，cdhash `6faaec7e965356b1af732a83f297e605010dcf02` |
| `round85-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` ⇒ `trust_cache_sha256` 已同步 |
| `round85-kernel.macho.adt` | 13476 B | `fc1c71182e88ba173f2ddba9e2946078cc458d0a4e445ae2ac69011a5f59bc6d`（每轮 `random-seed` 都换） |
| `round85-unixnu.toml` | 542 B | 标题 `clover tty: the child panics the kernel itself, past the msg= write` |

刷机：

```sh
fastboot flash cache ~/clover-out/round85-payload.img
fastboot reboot
```

判定表（这一轮 panic 屏的第一行不再是 `initproc exited`，而是孩子自己递进去的那句）：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `userspace panic: clover-el0: write returned` | 标签 write **返回了**，而且 exec 出来的孩子 syscall 一路通到 `reboot` ⇒ (b)(c) 上位，堵点全部移到 `exit`/`wait4` 唤醒那一侧（`childproc` 跨 exec 还连不连着是第一个要问的） | 父亲侧动刀：让父亲不靠 `wait4` 也能出声 |
| `userspace panic: user space RB_PANIC message copyin failed` | write 返回了，`reboot` 也办了，只是消息指针不对 | 读数等价于上一格，指针问题下轮顺手修 |
| `initproc exited ... exit status 1` | write 返回了，但 `reboot(2)` 自己回了 errno ⇒ 有一层策略没编进来或被拒 | 把 errno 挤进父亲的报告带里再刷一次 |
| **永远没有 panic 屏** | 只剩两种：那次 write 真没回来，**或** `reboot` 这次调用自己在内核里堵住（`kalloc_data(Z_WAITOK)` / `copyinstr`） | 下一刀把 `reboot` 挪到标签 write **之前**，一次调用两条边 |

### v0.2.57（round86）：把报告搬进内核，但搬进**不被测的那条路**

round85 的实机读数是**永远没有 panic 屏**。到这里，同一个指令指针上的四个出口全部试过并且全部静默：
write 返回（78/79）、`exit(448)`（84）、`kill(SIGSTOP)`（81）、`reboot(RB_PANIC)`（85）；而 round84
的照片已经证明这个孩子的前八次 write 都回来了。剩下的三种状态 —— write 没回、`exit` 没回、父亲的
`wait4` 没被叫醒 —— **从用户态分不开**，因为它们唯一的发言渠道就是那块被审查的 tty，或者那个可能本来
就没醒的父亲。所以这一轮不再改探针，而是把报告搬到内核里。

搬的位置有讲究，两条都是查出来的而不是假设的：

- **不能在 `ttwrite`/`kmoutput` 里调 `kprintf`。** `console_write`（`osfmk/console/serial_console.c:526`）
  本身就是一个环形缓冲加写锁：`:572` 在缓冲排不完时无界自旋，`:424` 的 `console_ring_try_empty` 里
  `while (nreserved > 0) cpu_pause()` **持着 write_lock 自旋**。从被测路径内部打印等于二次进入被测对象，
  读数将被仪器自己污染。
- **核心 XNU 源码没有设备仓补丁位。** `out/clover/tree/patches/xnu` 是指向基仓的软链，而 `feeds.py`
  的候选顺序是「按 `(kind,name)` 取第一个存在的路径」⇒ 设备仓一旦建 `patches/xnu`，会**整体顶掉**上游
  上百个补丁，不是加一个。于是仪器进 `drivers/ArmCpu/`：这块板子已经拥有这个模块（`archives` 里的
  `build-arm-driver.py ArmCpu`），改动只影响 clover。

选中的通道是**刻度中断自己**：tick 由定时器在 EL1 取走，而被它打断的正是占着核的那个上下文，
那个上下文的地址就在 `ELR_EL1` 里 —— 这块板第一次能产出一个**不触碰悬案本身**的读数。

panic 文案（`drivers/ArmCpu/ArmCpu.cpp`，7 个转换，panic 屏自己那份 printf 渲染器上限是 8）：

```
ArmCpu: clock watchdog at 60000 ticks (elr=… spsr=… elr2=… cnt0=… cnt1=… freq=…)
```

| 字段 | 取法 | 读数 |
| --- | --- | --- |
| `elr` | `ArmCpu::interrupt()` **入口处**读 `ELR_EL1`，早到这里还不可能嵌套第二次异常 | 被打断上下文当时在执行的指令地址 |
| `spsr` | 同一条 `mrs`  sequence 里的 `SPSR_EL1` | 低 4 位是那个地址所在的 EL：`5` = EL1h 内核，`0` = EL0t 用户 |
| `elr2` | 在 `panic()` 调用点再读一次 | 与 `elr` 不同 ⇒ 第一个是样本、第二个是进 panic 的路 |
| `cnt0` / `cnt1` / `freq` | 第一次 tick 与第 N 次 tick 的 `CNTVCT_EL0`，加上 `CNTFRQ_EL0` | `(cnt1-cnt0)/(N-1)` 对比 `freq` = 这块板**第一次被量出来的 tick 周期** |

N 取 60000 而不是 round82 的 30000，理由是 tick 数是**启动进度坐标**而不是时长：round82 在 launchd
exec 内部的 AMFI 检查处读到 `ticks=0x14e5`=5349，那是 initproc 进 EL0 的前一步，所以 60000 ≈ 探针到达点
之后约十一倍，且这个推断不依赖 tick 到底有多长。`armcpu-timer-intid=19` 与 `clover-pmgr=2` 原样保留，
EL0 探针与根镜像一字未动 —— 所以 `scratch-data.img` 仍是 round85 那份 `fe27be32…`、`trust_cache_sha256`
仍钉 `d310ae35…`。

产物里**第一次换的是内核而不是根**：`e98b9659…`（round83/84/85 三份相同）→ `e87a5127…`。新串在镜像里、
旧串 0 命中（`grep -ac 'clock watchdog at' kernel.macho` = 1）。

`verify-clover-payload.py` 全绿、`check-clover.py` 通过：

```
scratch-data.img == signed root            OK
trustcache == published bundle             OK
kernel.macho: probe patch localized        OK (5960 bytes in 35 runs, sha256 aa054e0d…)
kernel.macho: original-PMGR bindings alive OK (5 bindings, 0 moved; provider text present)
```

boot-args 也要自证：从卷里抽出的 `unixnu.toml` 实到 `… clover-pmgr=2 armcpu-clock-panic=60000`，
567 B / `d2e42cbd…`，标题 `clover tty: the tick panic reports the address the child is hung on`。

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round86-payload.img` | 58720256 B | `b964c1a57eae3ee69265777091375764871699ba66dcde1319f0cba101ffcaad` |
| `round86-kernel.macho`（构建产物） | 51830784 B | `e87a51278c6b4c5b7eea53ee79299e26ebf2a244516ffcba5760cb65fdb4e83b` |
| `round86-in-volume-kernel.macho`（卷内副本） | 51830784 B | `aa054e0d66b98fb9ff7d5f93e53c8e618cc7c2b56029d023929482a52e9fe06c` |
| `round86-scratch-data.img`（launchd.macho，与 round85 相同） | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round86-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` ⇒ 与 `trust_cache_sha256` 一致，无需改 |
| `round86-kernel.macho.adt` | 13476 B | `5ecf261fd787ac7d0b538438b4db380d6af497bc12a3de8be5b6c5459fd70e37`（每轮 `random-seed` 都换） |
| `round86-unixnu.toml` | 567 B | `d2e42cbd936c517f4d2175ceb54a9916524a78402221deadf4061a4f8bb22ca3` |

刷机（永远冷刷）：

```sh
fastboot flash cache ~/clover-out/round86-payload.img
fastboot reboot
```

**拍两张**：tick 周期这轮才第一次被量，所以 60000 tick 到底几分钟不知道（1 ms 一 tick ≈ 60 s，
10 ms ≈ 10 min）。到约 2 分钟拍第一张，没有 panic 屏就等到十分钟再拍第二张。

判定表（三格，`elr=` 用 `scripts/decode-clover-address.py` 离线命名）：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| panic 屏、`spsr` 低位 = `5`、`elr=` 落在 `ttwrite`/`kmoutput`/`console_*`/`vc*` 区间里 | 孩子当时**就在 tty 输出路径内部**，函数名一次拍照定死；解码器还会报它是否在一个回跳循环里、循环体内有没有 `bl` | 按那个函数的具体回跳点决定切哪一刀，不再猜 |
| panic 屏、`elr=` 落在 `cpu_idle`/`machine_idle`，或循环体里有 `wfi` | 孩子不是自旋而是**睡了**，核是空的 ⇒ 「EL0 自旋饿死同板进程」这条 round77 结论作废，`wait4` 叫不醒才是问题 | 回到调度/唤醒那一侧，`msg=` 的 write 无罪 |
| panic 屏、`spsr` 低位 = `0` | 被打断的是**用户态**上下文 ⇒ 孩子在跑指令、没堵在内核里，十一轮的整个前提翻转 | 命名那个 EL0 地址在探针镜像里的位置 |
| **仍然永远没有 panic 屏** | 60000 次 tick 都没到 ⇒ 要么 tick 慢到没到（`cnt1` 会直接告诉我们是几种），要么**那个上下文把 IRQ 屏蔽了**：`ttysleep` 出局，`_lpint`/`disable_interrupts` 内的自旋上位 | 下一轮把 N 降到 5349 附近，先确认看门狗在探针到达点之前会响 |

### 读数侧：`scripts/decode-clover-address.py`

`elr=` 是个数字，不是读数。这个脚本把它变成三样东西：哪个函数、进去多深、以及那些指令在干什么。

```sh
python3.13 scripts/decode-clover-address.py --suspects            # 本镜像的 tty/console 区间表
python3.13 scripts/decode-clover-address.py 0xfffffff0073b9b00     # 命名 + 前后 12 条指令
python3.13 scripts/decode-clover-address.py --no-dis 0xADDR        # 只命名
python3.13 scripts/decode-clover-address.py --kernel ~/clover-out/round86-in-volume-kernel.macho 0xADDR
```

地址同一性是**验过的**：`llvm-nm` 在构建产物 `out/clover/kernel/kernel.macho` 与卷内副本
`round86-in-volume-kernel.macho` 上给同一批符号同一批地址（`_ttwrite` = `0xfffffff00772c9a0`、
`_console_write` = `0xfffffff0073b9bd8`）—— 入口探针与 panic 屏都是就地覆盖字节，不动 section。
`llvm-addr2line` 只有函数名、行号一律 `??:0`（这份构建没有 DWARF 行表），所以解码靠函数名 + 指令窗口。

两条机器上的实测事实决定了脚本的写法：`llvm-objdump --macho -d --start-address=…` 在这份内核上
**什么都不输出**（Mach-O reader 只看字面名为 `__TEXT,__text` 的 section，而代码在 `__TEXT_EXEC,__text`，
另外还有 `__KLD` 与两颗 prelink 的 `__text`）；`--section __TEXT_EXEC,__text --dis-symname _name` 才对，
且只用 0.1 s —— 同一个调用不带 `--dis-symname` 会吐 210 万行、花 4 秒。所以反汇编一律按符号名整取，
窗口在 python 里切。

本轮自测（三个已知地址，全部命名正确）：

```
0xfffffff00772c9a0  _ttwrite + 0x0            0xfffffff00772c9a0 .. 0xfffffff00772cef0 (< 0x550 B)
0xfffffff0073b9c00  _console_write + 0x28     0xfffffff0073b9bd8 .. 0xfffffff0073b9f14 (< 0x33c B)
0xfffffff0073e0c30  _cpu_idle + 0x34          0xfffffff0073e0bfc .. 0xfffffff0073e0f0c (< 0x310 B)
```

回跳检测也自测过：`console_ring_try_empty` 里 `0xfffffff0073b9ad0 .. 0xfffffff0073b9b30` 被报成
「25 条指令、循环体内没有任何 `bl` ⇒ 每轮重读同一块内存的轮询」，正是 `while (nreserved > 0) cpu_pause()`；
`cpu_idle` 的 `wfi` 则被报成「parked…这不是自旋」加一条「循环体在 `wfi` 上歇 ⇒ 是等待环」。**这条仪器现在
已经能认出那块板上唯一一处已知的无界自旋**，所以它报出来的东西值得信。

`--suspects` 里 `ttwait`/`gc_putchar`/`vc_scroll_up` 三个名字在这份镜像中不存在（`ttwait` 被内联、字形路径
叫 `gc_putc_normal`/`gc_putc_gotpars`、搬像素的那个叫 `video_scroll_up`），脚本会照实说 `absent from this
image`，表里的每一条都是拿 `llvm-nm` 逐条查过名字的。

### round86 的实机读数：第 60000 次 tick 打断的是**用户态**，位置在 `_puts + 4`

照片（两行的接缝在 `spsr=fffffff5` + `feff6d50`，其余字段逐字清楚）：

```
ArmCpu: clock watchdog at 60000 ticks (elr=100000b3c spsr=fffffff5feff6d50
                                       elr2=100000b3c cnt0=124f800 cnt1=0 freq=124f800)
ticks=0xea60  ipi=0x0  sent=0x0  irq=0x13  phase=0x3  intid=0x13  typer=0x7b04f3
pmgr=0x5 status=0x0 mode=0x2  nub=0xfffffff818932800 attached=0x1 registered=0x2
aon=0xffffffff=>0xffffffff
LR=0xfffffff007968b40
```

**判定表命中的是第三格**：`spsr` 低 4 位 = `0` = EL0t，而 `elr` 是个用户态地址 ⇒ 被 tick 打断的那个
上下文当时**在跑用户态指令**，没有堵在内核里。

`elr` 对着**根镜像**解（不是内核 —— 它是 EL0 地址）：`llvm-nm -n build/clover-root/launchd.macho`
给 `_puts = 0x100000b38`，于是 `0x100000b3c` = `_puts + 4`：

```
100000b38:  bl   _write_console     <- write(2) 的 svc 在它里面（x16=4, fd 在 x22）
100000b3c:  mov  x1, #0xe0          <- 第 60000 次 tick 打断在这里
100000b40:  b.hs _fail
100000b44:  ret
```

`_puts` 的八个调用点全落在 `0x10000060c .. 0x10000078c`，也就是 `_start` 的**孩子分支**（父亲在
`_parent` 里直接 `bl _write_console`，不经过 `_puts`）。⇒ 从 round74 挂到今天的那条「孩子卡在那次
`write` 里面」**正式出局**：write 返回了，而核在它返回后的第三条指令上被采样到。

但一轮一张照片只买得起一个「出局」，因为它同时买下一个说不通的分支：`_puts + 4` 往后只有三条指令就
`ret`，正常执行时它活不过几纳秒，在 60000 个 tick 里恰好采到它 —— 要么是撞巧，要么那个核**根本没往下
走**。一次采样分不开这两条，因为两种情况给出的都是同一个地址：

- 核真的在跑 ⇒ `msg=` 之后本该有下一行，问题从「write 堵了」变成「输出为什么丢了」；
- 或者 `ELR_EL1` 在这个 handler 里是陈旧的（这里是第一次读它，没有自查手段）⇒ 要换的是仪器。

所以下一轮把 tick 从秒表改成**采样器**：在几个阈值各存一份 `(elr, spsr, sp_el0)`，同时存内核
`current_thread()` 里那份**软件保存的** PC —— 硬件视角与内核视角对得上、且五份全同，才谈得上「线程真的
冻在那条指令上」。这条排在 eMMC 之后（2026-10-25 用户的话：「你先编写 emmc 驱动吧」），见 v0.2.58。

其余五条读数是本轮的副产品，按字面收：

| 读数 | 含义 |
| --- | --- |
| `ticks=0xea60` = 60000、`intid = irq = 0x13` = 19 | 看门狗在自己设定的数上响；INTID 19 第二次独立确认 |
| `phase=0x3` | 是在 `rtclock_intr()` **之后**才撞上看门狗 ⇒ XNU 的时钟子系统被真实 tick 喂了 60000 次 |
| `typer=0x7b04f3` | 第一次读到 GICD_TYPER：ITLinesNumber = 19 ⇒ 最大 INTID 639 ⇒ `drivers/ArmCpu/ArmCpu.cpp:122` 硬编码的 `kIpiSpiBase = 1008` **出界**（tb128fu 当年为此把基址挪到 536）。今天无害 —— 只跑一个核、没走 SPI IPI —— 但这条账从此是已知量而不是猜测 |
| `pmgr=0x5 status=0x0 mode=0x2 aon=0xffffffff=>0xffffffff` | CloverPMGR 走完全程并用 AMFI 自己的查询证实了发布 ⇒ round83 那颗守卫壳是真答案，不是碰巧没 panic |
| `LR=0xfffffff007968b40` = `ArmCpu::interrupt() + 0x280` | caller evidence 指回我们自己的 `panic` 调用点，说明照片上的数字没读错 |

仪器上留着一处**没修好的 bug**：`cnt0 = freq = 0x124f800 = 19,194,112` = 19.2 MHz（Snapdragon 的标准
计数频率 ✓），而 `cnt1 = 0`。60000 个 tick 之后 `CNTVCT_EL0` 不可能是 0；`panic()` 的七个实参与七个转换
也确实一一对得上（`drivers/ArmCpu/ArmCpu.cpp:630-632`），所以矛盾没法用「读错一位」打发动掉，**tick 周期
仍然没量到**。改法与采样器一起做：两个 counter 值先写进全局再打印，别让实参求值参与。

顺带从同一份反汇编里补上一条一直缺的事实：`_exit_now` 是
`mov x0, x23; mov x16, #1; svc #0x80; b _checkpoint`，而 `_checkpoint` 就是 `b _checkpoint` 的永久自旋
（`0x100000ad8`）；父亲的 `wait4` 返回 0 也走同一条 ⇒ 「`exit` 不干活」在这份探针里的**表现**是卡在
`0x100000ad8`。这次采到的是 `_puts + 4` 而不是它，于是这条也顺带被排掉一次。

本轮只读照片，代码一行没动，`round86-*` 仍是当时的产物。

### v0.2.58（round87）：第一次把手伸向那颗 eMMC —— 这一轮只看不写

用户 2026-10-25 的话是「你先编写 emmc 驱动吧」，而它后面还挂着更早的一条要求：「你能不能让它自己把日志
存在某个分区，然后我们读取」。两条合起来需要的能力只有一个：**内核能写闪存**。

第一个硬事实决定了这一轮不能直接写驱动。这块板上唯一的 SDHCI 实现是固件里的二进制 `SdccDxe`
（`uefi-clover/Binaries/clover/QcomPkg/Drivers/SdccDxe/SdccDxe.inf`）—— 没有源码，所以初始化序列、
CMD 序列、HS400 tuning 全在 blob 里，**没有可抄的东西**；XNU 侧也只有 UFS 的 `NativeUfs`（tb128fu 那台
其实是 UFS 2.2，它的 `package/BootLog.c` 形状能抄，硬件通路不能）。于是 round87 采取的形状是
**继承而不是重新初始化**：先看固件把 `0xc0c4000` 留成什么样子。

这轮只问一个问题：**这块寄存器窗口在 XNU 手里是活的吗？** 答法是把窗口映射起来、把固件留在里面的状态
原样读出来、用 panic 屏拍照。**一个命令都不发，一个字节都不写，一次复位都不做。**

| 改动 | 作用 |
| --- | --- |
| `drivers/CloverSdhci/SdhciRegs.h` | SDHCI 系统地址块的每个偏移与位域，外加 Qualcomm SDCC v5 的**两套**扩展布局；每一条都带 `file:line` 出处 |
| `drivers/CloverSdhci/CloverSdhci.cpp` | mode 1：映射 → 读约 40 个寄存器 → 手拼一份 512 B 报告 → `panic ("CloverSdhci: %s", …)` |
| `drivers/CloverSdhci/README.md` | 为什么是继承、以及读回来之后下一步做什么 |
| `drivers/CloverPMGR/CloverPMGR.cpp` | `sheng_pmgr_run()` **尾部**调用 `clover_emmc_run()` |
| `scripts/build-arm-driver.py` | `CloverSdhci` 进 `choices` 与 out 目录表 |
| `configs/clover_defconfig` | `CONFIG_XNU_EXTRA_LD_ARCHIVES` 追加 `libCloverSdhci.a` |
| `board.toml` | `[kernel] archives` 加一行构建器、`kernel_args` 加 `clover-emmc=1`、菜单标题换成本轮的问题 |
| `scripts/check-clover.py` | 四条 round87 规则：构建器 / 归档 / boot-arg / `SHENG_PMGR` 四者必须同时成立 |
| `scripts/verify-clover-payload.py` | 新增 `check_sdhci()`：在最终内核里钉三条串 |

调度点的选择不是偷懒。设备仓没有 core XNU 的补丁位（round86 查到的那条：`feeds.py` 按 `(kind,name)`
取第一个存在的路径，设备仓一旦建 `patches/xnu` 就会**整体顶掉**上游上百个补丁），也就没有地方登记一个
IOKit personality；而 `bsd_init.c` 里那个 `sheng_pmgr_run()` 钩子是基仓给 clover 留好的，round83 已实机
证明它能跑。调用点放在 **PMGR 发布成功之后**，这样「eMMC 的读数」和「PMGR 某一步失败」永远不会挤进
同一张照片 —— 后者的 panic 先来，也先报名字。

链接形状沿用 display stub：归档以**裸路径**进 `CONFIG_XNU_EXTRA_LD_ARCHIVES`，不带
`-Wl,-force_load`，因为成员是被一个已存在的引用拉进来的（`clover_emmc_run` 由 `CloverPMGR.cpp` 调用）。
代价是「没人引用就不进镜像」，所以这条风险被写成断言：`check_sdhci()` 直接在最终内核里钉
`clover-emmc`、`look-only dump of c0c4000`、`CloverSdhci: failed at stage` 三条串。

一张照片能读什么（报告字段 → 偏移 → 为什么是它）：

| 字段 | 偏移 | 读它是因为 |
| --- | --- | --- |
| `cap` / `cap2` | 0x40 / 0x44 | 总线宽度、速率位、时钟基数。设备树声称这颗 SDCC 有 8-bit 总线与 HS400/HS200/DDR 1p8v，所以 `cap2` 的速率位**应当非零** |
| `ver` … `slot` | 0xFC 那颗 u32（15:0 slot 中断状态，31:16 主机版本） | 规格版本；`ver=0x0300` 是 SDHCI 3.0，`0x0000` 就是没人应答 |
| `pst` | 0x24 | Command Inhibit (bit 0) / Data Inhibit (bit 1)：固件退出时留着一笔没做完的传输，会直接读出来 |
| `int` / `en` / `sig` | 0x30 / 0x34 / 0x38 | 中断状态与两级屏蔽。SDCC 的 IRQ 是 `GIC_SPI 110` ⇒ INTID **142**，和 27 那个 PPI 家族无关 |
| `hcl` `pw` `clk` `hcn` `tmo` `rst` | 0x28 / 0x29 / 0x2C / 0x3E / 0x2E / 0x2F | 总线控制、电压位、时钟控制、Host Control 2（速率模式）、超时、软件复位 —— 复位位若还置着，说明控制器仍在 reset 里 |
| `blk` `cnt` `tm` `cmd` `arg` `a2` `acmd` `max` | 0x04 / 0x06 / 0x0C / 0x0E / 0x08 / 0x00 / 0x3C / 0x48 | 固件最后一次事务的残留：块大小、块数、传输模式、命令、两个参数（`a2` 就是 `SDHCI_DMA_ADDRESS` 那颗）、auto-command 状态、电流能力 |
| `rsp`（四颗） | 0x10 / 0x14 / 0x18 / 0x1C | 响应寄存器；`rsp0` 是 RCA/CSD 首字，能验证卡当年真的答过话 |
| `clk…MHz x… div… sd…MHz` | 由 `cap` 的 15:8 与 `clk` 的分频位算 | 这块板的 SD 时钟到底是多少 MHz，以及 Linux 同款算法给出的分频 |
| `msm v50/318`、`dll 100/200`、`dsts 108/208`、`gen 70/320` | 两套候选布局各读一遍 | `sdhci-msm.c:206` 用 0x318/0x320/0x200/0x208，`:239` 用 0x050/0x070/0x100/0x108。设备树没说这颗 SDCC 是哪一种 ⇒ 一次拍照把这个问题**定死** |

看门狗保持武装（`armcpu-clock-panic=60000` 原样留着）。这不是装饰：如果某一次裸读把互联挂住，照片会是一张
tick 看门狗 panic，`elr=` 用 `scripts/decode-clover-address.py --kernel ~/clover-out/round87-in-volume-kernel.macho`
就能命名到 `inspectHost` 里的那条 `ldr`。

产物：本轮换的是**内核**（`e87a5127…` → `eb1e2356…`），根镜像与 trust cache 一字未动。

```
scratch-data.img == signed root            OK
trustcache == published bundle             OK
kernel.macho: probe patch localized        OK (5960 bytes in 35 runs, sha256 014c4950…)
kernel.macho: original-PMGR bindings alive OK (5 bindings, 0 moved; provider text present)
kernel.macho: CloverSdhci linked           OK (三条串全在)
```

boot-args 自证：卷里抽出的 `unixnu.toml` 实到
`… clover-pmgr=2 clover-emmc=1 armcpu-clock-panic=60000`，585 B / `a6eb9731…`，
标题 `clover emmc: the sdhci register dump answers whether the window is live`。

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round87-payload.img` | 58720256 B | `c2fe92b33a3b7f951c14b2af5c3fd0a767fe868a00d6e638f7b2763fead34663` |
| `round87-kernel.macho`（构建产物） | 51847168 B | `eb1e23566247d25e66e135207b0f06b15c9a8cb0294ac49075cb41843109f6b8` |
| `round87-in-volume-kernel.macho`（卷内副本） | 51847168 B | `014c49502948bbcf7da978f2c5113352944f20926b286d481218ff2296ab9b8c` |
| `round87-in-volume-scratch-data.img`（与 round85/86 相同） | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round87-in-volume-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` ⇒ 与 `trust_cache_sha256` 一致，无需改 |
| `round87-in-volume-kernel.macho.adt` | 13476 B | `1390d6bcceceb1383b25ca8393ac852e93d6c62336e66d726853d3c9d9340d28`（每轮 `random-seed` 都换） |
| `round87-boot.img`（只有固件，无 ramdisk） | 1613824 B | `196b284fc15119610260480c767a5d0a14bf18b4eed31d2e267548be1ba7d00b` |
| `round87-in-volume-unixnu.toml` | 585 B | `a6eb97311a82e6281e80abee28fc26cef4367f2d6f2a988586d35ca43cd07d67` |

刷机（永远冷刷）。这一轮**只换 `cache`**：boot 分区装的是固件，round87 一个字节没碰它
（新的 `round87-boot.img` 与上一份 `clover-boot.img` 只差 21 字节 —— gzip 头里那个 `OS`
字节和由它派生的 boot 头 `id` 哈希；解开后的固件是**同一份** `af09f6b3…`，1600 万字节里
一个字都不差）：

```sh
fastboot flash cache ~/clover-out/round87-payload.img
fastboot reboot
```

要连 boot 一起刷（从不必要，只是让分区状态可复现；**冷刷，先刷 boot 再刷 cache**）：

```sh
fastboot flash boot  ~/clover-out/round87-boot.img
fastboot flash cache ~/clover-out/round87-payload.img
fastboot reboot
```

`round87-boot.img` 由 `./unixnu bootimg --uefi ../mu-clover --target RELEASE --dtb
../mu-clover/Resources/DTBs/clover.dtb --ramdisk none -o ~/clover-out/round87-boot.img`
生成（两次构建逐字节相同）；头是 `ANDROID!`、`header_version 0`、`kernel_addr 0x8000`、
`tags_addr 0x100`、`ramdisk_size 0`，kernel 段 1609203 B = gzip(BootShim + 固件卷) 加串联
DTB。**不要**给这条命令塞 `--ramdisk`：这块板子的固件没有 ramdisk 文件系统驱动，卷塞进
boot 里它根本看不见（见「payload 放在哪」）。

**拍一张就够**：这是启动早期的一次 panic，不等 tick，也不等 EL0。

判定表：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `CloverSdhci: look-only dump of c0c4000`，且 `cap` / `ver` 像样（`ver` 非 0，`cap` 的 15:8 是个正常基数） | 窗口在 XNU 手里**是活的**，固件留下的状态可读 | round88 发第一颗命令：CMD13 `SEND_STATUS`（ac, R1），只问「卡还认不认我们」，仍不碰 Data |
| 所有字段全 0 | 接口时钟被门掉了（`iface_clk` / `core_clk` 没开），寄存器读起来是常量 0 | 这一轮变成时钟树问题。MMCC 里 SDCC1 的 RCGR 偏移**还没查到**（两次 curl 都 404），届时要么从 `SdccDxe` 二进制里扫出来，要么按 `clover.dts` 的 `clock-names` 顺序试并让 `cap` 出声 |
| 所有字段全 `0xffffffff` | 映射成功但总线不该我们拿：NoC 侧 abort，或者那颗 SDCC 的电源域没开 | 转到 GCC / L4 电源域那一侧，别再往窗口里读 |
| `pst` 的 Command 或 Data Inhibit 还置着 | 固件留了一笔没做完的传输 | 下一轮把 `pst` 读两次（前后各一次），判断它是自己会清还是必须发一次 `SOFTWARE_RESET` |
| `clk` 那行给出一个说得通的 MHz，`msm v50/318` 里**只有一侧**像版本号 | 顺带定死两件事：SD 时钟到底多少，以及这颗 SDCC 用哪套 vendor 布局 | 后续寄存器表按那一侧写死，另一侧不再读 |
| `CloverSdhci: failed at stage 3 detail c0c4000` | `IODeviceMemory::withRange` / `map` 拒了这块窗口 | 查 ADT 与设备树里 `sdhc_1` 是否被 `reserved` / `no-map` 摘掉了 |
| 没有 `CloverSdhci`，而是 tick 看门狗先响、`elr=` 落在 `CloverSdhci` / `inspectHost` 区间 | 某一次裸读把互联挂住了 | 退到「只读 0x40 一颗寄存器」的最小版本，然后一颗一颗加回去 |
| 什么照片都没有 | 连 `sheng_pmgr_run()` 都没到，或者 PMGR 先失败并自己报了 | 与 round86 的采样器同轮处理：先让调度点自己出声 |

**这轮之后仍然没有块设备。** 读日志分区还差三样：CMD 序列（CMD0/CMD1/CMD2/CMD3/CMD9 那一串）、Data
通路（`SDHCI_BUFFER` 的 32-bit 读或 ADMA2 表），以及 `IOBlockStorageDevice` 的形状（在
`/Users/zhouzhou/UniXNU/drivers/NativeRamBlock/NativeRamBlock.cpp` 里有可抄的骨架）。窗口这次映射了
就不再释放 —— 一次启动只出一张照片，没有第二次使用它的人。

### v0.2.59：只修文档 —— `./unixnu build boot` 这一步根本不存在

这一轮不动代码、不刷机，只把「boot 分区从哪来」写对。README 的构建步骤里一直挂着
`./unixnu build boot`，而 `unixnu` 的 `build` 只认识
`kernel / package / firmware / payload / userdata / prelink`，跑这条会直接
`invalid choice: 'boot'` 退出。boot 镜像真正的入口是顶层子命令 `unixnu bootimg`
（`scripts/unixnu/bootimg.py`），它把 `gzip(BootShim + 固件卷)` 和 DTB 串成 Android
header v0 的镜像；不传 `-o` 时落在 `out/clover/boot/boot.img`。

两个参数是坑：`--target RELEASE`（不写会去找只有 DEBUG 才有的
`Build/cloverPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd`，这台机器上只有
`RELEASE_CLANGPDB`），以及 `--ramdisk none`（不写就把 payload 卷当 ramdisk 塞进 boot，
而这块板子的固件没有 ramdisk 文件系统驱动，塞了它也不亮）。`--dtb` 不会自动找，得显式给
`../mu-clover/Resources/DTBs/clover.dtb`（固件构建时打过 splash 补丁的那份）。

顺带把 round87 的产物做成可复现：`round87-boot.img`（1613824 B，`196b284f…`）两次构建
逐字节相同，与上一份 `clover-boot.img`（`af500654…`）只差 21 字节 —— gzip 头里的 `OS`
字节和由它派生的 boot 头 `id` 哈希，解开后都是同一份固件 `af09f6b3…`。所以 round87
**只需要刷 `cache`**；连 boot 一起刷只是让分区状态可复现，不是修什么。

### round87 读数（v0.2.58 已发布）：窗口是活的，固件留下的是一条 HS200 总线

照片只有一屏 panic，`CloverSdhci: look-only dump of c0c4000` 下面是全部读数：

```
cap=742dc8b2 cap2=0000808f ver=6b02
pst=01f800f0 int=00000000 en=07ff003f sig=00000000
hcl=38 pw=0b clk=0007 hcn=0003 tmo=0f rst=00
blk=0200 cnt=0000 tm=0033 cmd=0c02 arg=00000000 a2=00000000 acmd=00030000
rsp 00000000 00000000 00000000 00000000
msm v50/318 00000000/1000006b
dll 100/200 00000000/0705642c
dsts 108/208 00000000/00000184
gen 70/320 00000000/00f04003
```

逐项读完，能确定的事比预想的多：

| 读数 | 含义 |
| --- | --- |
| `cap=742dc8b2` | 时钟基数位 15:8 = `0xc8` = **200 MHz**；1.8 V + 3.3 V、64-bit、ADMA2、High Speed 都在 |
| `cap2=0000808f` | SDR50/SDR104/DDR50、Driver Type D、HS400 —— 设备树声称的速率档位在控制器里是真的 |
| `ver=6b02` | vendor `0x6b`（Qualcomm）、spec `0x02`；窗口不是常量 0，也不是 `0xffffffff` |
| `pst=01f800f0` | Command/Data Inhibit **都没置**；DAT[3:0] 与 CMD 都是高电平；bits 4-7 是保留位、这里读 1；card-inserted 0（焊死的卡） |
| `int=0` / `en=07ff003f` | 没有挂起的中断；两级屏蔽都没挡住命令完成位 |
| `hcl=38` | 8-bit 总线 + ADMA64 |
| `pw=0b` | 1.8 V 那一路是开的（vendor 自己驱动供电） |
| `clk=0007` | 内部时钟已使能且稳定、卡时钟已开、**分频 0** ⇒ SDCLK = 200 MHz |
| `hcn=0003` | UHS SDR104 选择位 —— `sdhci-msm` 正是用它表示 HS200 |
| `tm=0033` | DMA + block count + read + multi，**auto CMD12 关着** |
| `cmd=0c02` | CMD12（STOP_TRANSMISSION）、短响应、不查 CRC/索引 —— 固件最后一件事是手工补了一次 CMD12 |
| `msm v50/318`、`dll 100/200`、`dsts 108/208`、`gen 70/320` | 只有 v5 那套布局应答：`0x318=0x1000006b`（core 版本 1.0x6b）、`0x200=0x0705642c`（DLL_EN + CK_OUT_EN）、`0x208=0x00000184`（**CORE_DLL_LOCK 已锁**）、`0x320=0x00f04003`；旧 msi 那套（`0x050/0x070/0x100/0x108`）全是 0 |

所以 round87 的判定是：**窗口在 XNU 手里是活的；固件把卡和控制器留在了一条 8-bit、1.8 V、
200 MHz、DLL 已锁的 HS200 状态里；最后一笔事务是一次多块 DMA 读之后的 CMD12。** 总线此刻
是静的（没有 inhibit，DAT/CMD 都是高），下一颗命令可以直接发。

这张照片还暴露了报告自己的三个 bug，round88 一并修掉：`SDHCI_CLOCK_BASE_MASK` 写成了
SDHCI 1.0 的 6 位字段 `0x3F00`（把 `0xc8` 印成 `008`）、MHz 换算乘了 `100000` 而不是
`1000000`、以及把「分频 0」当成「时钟停了」而报 `sd0MHz`。寄存器本身的读数没有错，错的
只是那行字。

另一件必须记账的事：照片里 `ticks=0`、`ipi=0`、`sent=0`。`sheng_pmgr_run()` 在
`bsd_bufferinit` 之后、`ml_set_interrupts_enabled()` 之前，定时器中断在这里根本不会被
服务 ⇒ **tick 看门狗抓不到 `CloverSdhci` 里的挂死**。从这一轮起，驱动里的每一个等待都
必须自带上限。

### v0.2.60（round88）：发第一颗命令 —— CMD13 问「卡还认不认我们」

这一轮只问一个问题：**从内核发出的第一条命令，会不会得到应答？** 选的命令是 CMD13
`SEND_STATUS`（ac，R1 响应，无数据），因为它是唯一一条「只证明命令通路、不碰 Data」的
命令。mode 1 原样保留，作为任何时候的对照。

| 改动 | 作用 |
| --- | --- |
| `drivers/CloverSdhci/CloverSdhci.cpp` | mode 2：只写三颗寄存器 —— `INT_STATUS`（只清命令组，W1C）、`ARGUMENT`、`COMMAND`；带上限地轮询 `INT_STATUS`；读 RSP0/RSP1 解 R1 |
| `drivers/CloverSdhci/SdhciRegs.h` | 时钟基数掩码改成 `0x0000FF00`，并把「分频 0 = 基频」写清楚 |
| `board.toml` | `clover-emmc=2`、菜单标题换成这一轮的问题 |
| `scripts/verify-clover-payload.py` | `check_sdhci()` 加第四条串 `CMD13 rca=1` |
| `scripts/patch-xnu-entry-probe.py` | 探针洞穴的分配器：向后找不到洞时，改到可执行落点段里找；并记录已写区域，绝不二次分配 |

命令的具体形状：RCA 用 **1**（UEFI SD/MMC 栈给第一张不可移除卡的默认相对地址；固件真正的
RCA 没有记录在任何这个移植读得到的地方）。`ARGUMENT = 1 << 16`，`COMMAND` 由
`SDHCI_MAKE_CMD(13, RESP_SHORT | CRC | INDEX)` 拼成 **`0x0d1a`**。发之前先确认
`PRESENT_STATE` 的 Command/Data Inhibit 都没置；轮询上限是 `1 << 22` 次读，任何情况下都会
落到一个 panic，不会无声自旋。R1 的低 16 位是错误/当前状态，`(rsp0 >> 9) & 0xf` 是卡状态
（4 = tran，5 = data），`(rsp0 >> 8) & 1` 是 ready。

这一轮还有一次构建事故值得写下来，因为它会重复发生：payload 的入口探针要在内核的零填充里
挖 15 个 0x100 字节的洞，外加一个 0xe13 字节的 panic 屏。round88 新增的 CMD13 代码让
`__TEXT_EXEC` 长了 `0x1930` 字节，正好吃掉了入口后面最后一块够大的填充，构建直接以
`No 0x100-byte padding for the marker cave` 失败。查清之后的边界是：

- `__KLD` 虽然 prot 是 `r-x`，但 `OSKext` 在 prelink bootstrap 之后会 `bzero` 再
  `ml_static_mfree`（`OSRuntime.cpp:349`、`OSKext.cpp:1262`）⇒ 放在那儿的代码可能在一次
  更晚的 panic 之前就消失，**不能用**。
- `__PLK_TEXT_EXEC` 是预链接 kext 的文本段，一直映射、一直是 `r-x`，里面有成片从没被
  使用过的零填充（开头就有 0x49000 字节）⇒ 可以用。
- 洞穴代码本身是位置无关的，每个调用点都会重新验算自己刚编出来的分支；所以「更早的洞」
  和「更晚的洞」等价。分配器现在先向后找，再按地址顺序扫 `__TEXT_EXEC` /
  `__PLK_TEXT_EXEC`，并且把已经写过的区间记下来（汇编出来的代码自己也会含零字）。
- 这一轮 panic 屏因此落在 **`0xfffffff004920000`**（`__PLK_TEXT_EXEC` 的零填充，
  0xe13 字节），从 `_panic` 过去的 `b` 偏移是 −50.7 MB，仍在 ARM64 的 ±128 MB 内；
  `branch()` 会拒绝越界。`_panic_trap_to_debugger` 同理。

产物：本轮换的是**内核**（`eb1e2356…` → `7ea53a94…`），根镜像和 trust cache 一字未动。

```
scratch-data.img == signed root            OK
trustcache == published bundle             OK
kernel.macho: probe patch localized        OK (5960 bytes in 35 runs, sha256 7bc0667e…)
kernel.macho: original-PMGR bindings alive OK (5 bindings, 0 moved; provider text present)
kernel.macho: CloverSdhci linked           OK (四条串全在)
```

boot-args 自证：卷里的 `unixnu.toml` 是
`… clover-pmgr=2 clover-emmc=2 armcpu-clock-panic=60000`，575 B / `2edd1834…`，
标题 `clover emmc: CMD13 asks the card whether anyone is still home`。

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round88-payload.img` | 58720256 B | `6551c580a9b2238c2e2c0d1fcf1c7be6a1b8a956ab0786f77a323726d158a7a0` |
| `round88-kernel.macho`（构建产物） | 51847168 B | `7ea53a94efb462b167628146b448100b62fe6d32443ded283fc10bd763028a07` |
| `round88-in-volume-kernel.macho`（卷内副本，含探针与 panic 屏） | 51847168 B | `7bc0667ec7a6dd8ef0c1356ae56895b3b7555586aad7758bed00f81c54dbc6f3` |
| `round88-in-volume-scratch-data.img`（与 round85–87 相同） | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round88-in-volume-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round88-in-volume-kernel.macho.adt` | 13476 B | `777c622d1a4242af92c6c85ea1fd94a9a4a6791cfb64d38fb123ad9ed0e55eaa` |
| `round88-in-volume-unixnu.toml` | 575 B | `2edd183400ef124683c06e165a76650b65595629dbec55f10b7a5a4270a6e832` |

固件与 boot 分区这一轮一个字节没碰，`round87-boot.img`（`196b284f…`）继续有效。刷机只需
换 `cache`（永远冷刷）：

```sh
fastboot flash cache ~/clover-out/round88-payload.img
fastboot reboot
```

要让分区状态完全可复现就连 boot 一起刷（先 boot 后 cache）：

```sh
fastboot flash boot  ~/clover-out/round87-boot.img
fastboot flash cache ~/clover-out/round88-payload.img
fastboot reboot
```

**拍一张就够。** 判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `CMD13 rca=1 answered`，`R1 state=4`（tran）或 `5`（data），`err=0000` | 命令与响应通路都通，RCA=1 就是这张卡的地址 | round89：CMD17 读 LBA 1，前五字节应当自己说出 `EFI PART` |
| `CMD13 rca=1 timeout: no rsp`（`int` 里有 `0x10000`） | 命令发出去了，但没有卡应答 —— RCA 不是 1，或者卡不在 transfer 状态 | 走 CMD0/CMD1/CMD2/CMD3 重新拿 RCA；仍不碰 Data |
| `CMD13 rca=1 rsp error`（`int` 是 CRC/END_BIT/INDEX） | 总线的电气/时序与固件留下的 HS200 状态不匹配（我们没有 tuning，也没有改时钟） | 先降速：把 `hcn` 改回默认速度、只留 1-bit，再发一次 CMD13 |
| `CMD13 rca=1 no completion`（`spins` 到上限，`int` 无命令位） | 控制器接受了写但没产生完成事件 | 先看 `pst` 前后有没有 inhibit 变化；下一步读 `INT_STATUS` 的原始值并把 `en` 一起印出来 |
| `CMD13 rca=1 not sent: busy` | 固件留下的那笔事务还占着 Command/Data 线 | 只对命令/数据通路做一次 `SOFTWARE_RESET`，然后再发 CMD13 |
| 只有 `CloverSdhci: failed at stage …` | 窗口映射或 mode 参数错 | 按 stage 定位：2 = mode 不认识、3 = 映射失败、4/5 = 写入路径 |
| **什么 panic 屏都没有** | 对 `COMMAND`/`ARGUMENT` 的那次写把互联挂住了（这里 `ticks=0`，看门狗不会响） | 下一轮把写拆成单颗寄存器、每次写前用 `inspectHost` 的顺序重读一遍；或者先把窗口映射改成 device-nGnRnE 再试 |

**这轮之后仍然没有块设备。** 已经有的是一条被证明能读的窗口和一条待证明的命令通路；还差
CMD 序列（如果需要重新识别）、Data 通路（`SDHCI_BUFFER` 32-bit 或 ADMA64 表）和
`IOBlockStorageDevice`。日志分区那件事要等这三样齐了才开始。

### round88 读数（v0.2.60 已发布）：CMD13 发出去了，卡没有回话

照片上的判定行是：

```
CloverSdhci: CMD13 rca=1 timeout: no rsp
```

这句话能读成三件事，每一件都排除了别的解释：

- **命令确实发出去了。** 写 `COMMAND` 之前 `PRESENT_STATE` 没有 Command/Data Inhibit，
  判定不是 `not sent: busy`；随后置位的是控制器自己的超时中断（`INT_STATUS` 的
  `0x10000`），不是我们轮询到上限的 `no completion`。这颗 host 的命令超时是 `2^14` 个
  TMCLK；`cap` 低 8 位 `0xb2` 里 5:0 = 50、bit 7 = 1（单位 MHz，Linux 的
  `SDHCI_TIMEOUT_CLK_UNIT`）⇒ TMCLK = 50 MHz，超时约 **0.33 ms**。
  「等够了才放弃」是控制器的结论，不是循环的结论。
- **不是电气噪声。** `INT_CRC` / `INT_END_BIT` / `INT_INDEX` 一个都没置。有响应但采样错，
  通常先在这三位上出声；`timeout` 只说明 **CMD 线上没有任何一位被驱动**。
- **命令字没有抄错。** 事后又用 Linux `drivers/mmc/host/sdhci.h` 核了一遍：响应类型
  1:0 是 00 无响应 / 01 136 位 / 10 48 位 / 11 48 位带 busy，`0x0d1a` = index 13、
  R1、CRC + index 检查，正是 CMD13 该有的形状。

于是只剩两个嫌疑：

1. **RCA 不是 1。** eMMC 只回答与自己 RCA 相符的 CMD13；地址不对时它按规范保持沉默，
   从 host 看与「卡不在」完全一样。固件在它自己的识别过程里分配 RCA，而且不把它留在
   任何这个移植读得到的寄存器里。顺带查了 EDK2 的 `SdMmcPciHcDxe`
   （`EmmcDevice.c`，`EmmcIdentification`）：`Rca = Slot + 1`，slot 0 就是 1 —— 与这一轮
   试的地址相同。所以这固件的 `SdccDxe`（二进制、无源码）要么不是那套栈，要么卡已经不在
   会应答的状态。
2. **卡不在会应答 CMD13 的状态。** 例如在 handoff 前被复位回 idle、被断电，或者不在
   stand-by/transfer。round87 留下的最后一条命令是 CMD12（一次多块读的收尾）且
   `PRESENT_STATE` 无 inhibit，这让「还在 transfer」更像真的，但不能证明。

这两个嫌疑不需要新的硬件状态来区分：同一个时钟、同一套三颗寄存器，把地址换掉再问一遍。
CMD13 是只读命令，stand-by 与 transfer 都合法，问错地址不会改变卡上任何东西。

### v0.2.61（round89）：把地址问一遍 —— CMD13 扫 RCA

这一轮只问一个问题：**卡是不是在另一个 RCA 上醒着？** 时钟、位宽、电压、DLL 全部保持
round87 读到的固件状态；仍然只写三颗寄存器（`INT_STATUS` 命令组 W1C、`ARGUMENT`、
`COMMAND`），仍然不碰 Data，仍然不重置控制器。

| 改动 | 作用 |
| --- | --- |
| `drivers/CloverSdhci/CloverSdhci.cpp` | mode 3：对 16 个地址各发一次 CMD13；地址顺序 1..15、最后 0；1 放最前面，同时重测 round88 那一次静默 |
| `board.toml` | `clover-emmc=3`、菜单标题换成这一轮的问题 |
| `scripts/verify-clover-payload.py` | `check_sdhci()` 加第五条串 `CMD13 sweep` |

具体形状：

- 每次发命令前先有界地等 `CMD_INHIBIT` 落下 —— 超时的命令会在寄存器里多占一会儿，
  否则下一颗命令是写给 host 而不是写给卡的；
- 只清命令组的 W1C，然后 `ARGUMENT = rca << 16`、`COMMAND = 0x0d1a`；
- 轮询上限沿用 mode 2 的 `1 << 22`。控制器自己的命令超时只有 `2^14 / 50 MHz ≈ 0.33 ms`，
  这个上限高出几个数量级，足以把「控制器超时」和「我们的循环先放弃」分开；
- 第一个带 `INT_RESPONSE` 且没有错误位的地址就停；报告 R1 的 state/ready/err；
- 一个都没有时，报告 `none: 0..15 silent`，并给出 `to`（超时）、`re`（CRC/END/INDEX）、
  `si`（循环到上限也没事件）、`busy`（根本没发出去）四个计数。

报告仍然是 7 行，第一行二选一：

```
CMD13 sweep answered rca=2
CMD13 sweep none: 0..15 silent
```

产物：这一轮仍然换内核（`7ea53a94…` → `92eb373a…`），根镜像与 trust cache 一字未动；
卷内副本变成 `9ef324ac…`（代码比 round88 长了约 16 KiB，panic 屏和探针的位置由分配器
重新落在 `__PLK_TEXT_EXEC` 的零填充里）。

```
scratch-data.img == signed root            OK
trustcache == published bundle             OK
kernel.macho: probe patch localized        OK (5959 bytes in 35 runs, sha256 9ef324ac…)
kernel.macho: original-PMGR bindings alive OK (5 bindings, 0 moved; provider text present)
kernel.macho: CloverSdhci linked           OK (五条串全在)
```

boot-args 自证：卷里的 `unixnu.toml` 是
`… clover-pmgr=2 clover-emmc=3 armcpu-clock-panic=60000`，557 B / `fb7217b4…`，
标题 `clover emmc: which RCA is the card awake on`。

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round89-payload.img` | 58720256 B | `2a58ce508b0d34f8b0a46cf4ddabd638d0ce13aa3f78ab2a4f79fe7b7f2be580` |
| `round89-kernel.macho`（构建产物） | 51863552 B | `92eb373a0a008244985f7f7abd8ee2c14554bd50706a42bcb5b9a211400e3e17` |
| `round89-in-volume-kernel.macho`（卷内副本，含探针与 panic 屏） | 51863552 B | `9ef324aca1be9e58e56f40146deae30e57ce517468d5399e1731be38cd5ac244` |
| `round89-in-volume-scratch-data.img`（与 round85–88 相同） | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round89-in-volume-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round89-in-volume-kernel.macho.adt` | 13476 B | `09577650b1439030af5b904ab93fb9bcd2c512e596ec2be1957b0e787b31999e` |
| `round89-in-volume-unixnu.toml` | 557 B | `fb7217b4855adf02b4ba6514db47655bced69f1c3cc19c9ea06194dee7077bb5` |

固件与 boot 分区仍然一个字节没碰，`round87-boot.img`（`196b284f…`）继续有效。刷机只需
换 `cache`（永远冷刷）：

```sh
fastboot flash cache ~/clover-out/round89-payload.img
fastboot reboot
```

要让分区状态完全可复现就连 boot 一起刷（先 boot 后 cache）：

```sh
fastboot flash boot  ~/clover-out/round87-boot.img
fastboot flash cache ~/clover-out/round89-payload.img
fastboot reboot
```

**拍一张就够。** 判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `CMD13 sweep answered rca=N`，`R1 state=4`（tran）或 `5`（data），`err=0000` | RCA 就是 N，卡醒着、命令与响应通路都通 | round90：CMD17 读 LBA 1，前五字节应当自己说出 `EFI PART` |
| `none: 0..15 silent`，`to` 接近 16 | 这块时钟下没有任何常见 RCA 应答 —— 不是「地址差一个」的问题 | round90：降速到 400 kHz，走 CMD0/CMD1/CMD2/CMD3/CMD9 重新识别并把 RCA 定成 1，再发 CMD13 |
| `none`，但 `re` > 0 | 有东西在线上，只是 CRC/INDEX/END_BIT 不过 —— 200 MHz 下没有做过 tuning，采样相位可疑 | round90：同样先降到 400 kHz；如果低速能答，说明是时序/tuning 而不是卡 |
| `none`，`si` > 0 | 有几次是我们的循环先到上限，不是控制器超时 | 先看对应 `spins`；若与 `to` 混合，直接进 re-init，不用再纠结局部的循环长度 |
| `none`，`busy` > 0 | 控制器的 Command/Data Inhibit 没落下来，地址根本没问出去 | 只对命令/数据通路做一次 `SOFTWARE_RESET`，再重跑本模式 |
| 只有 `CloverSdhci: failed at stage …` | 窗口映射或 mode 参数错 | 2 = mode 不认识、3 = 映射失败、4/5 = 写入路径 |
| **什么 panic 屏都没有** | 仍然要怀疑那次 `COMMAND`/`ARGUMENT` 写把互联挂住了（`ticks=0`，看门狗不会响） | 与 round88 相同：拆成单颗写、或把窗口改成 device-nGnRnE 再试 |

**这轮之后仍然没有块设备。** 已经有的是一条能读的窗口、一条确定能发出去的命令通路；
还差一个会应答的地址（或一次重新识别）、Data 通路（`SDHCI_BUFFER` 32-bit 或 ADMA64 表）
和 `IOBlockStorageDevice`。日志分区那件事要等这三样齐了才开始。

### round89 读数（v0.2.61 已发布）：16 个地址全静默

照片里只有第一行有信息量：

```
CloverSdhci: CMD13 sweep none: 0..15 silent
```

RCA 1..15 与 0 各问了一次，没有一次带 `INT_RESPONSE`。round89 判定表的第二格命中：
**不是「地址差一个」的问题。** 但这一行只否掉了一个猜测，没有指出剩下的路：

- 从规范上说，RCA 是 1..65535 里的任意值，固件完全可以在 16 以外挑一个。所以「16 个地址
  全灭」并不能证明「卡不应答 CMD13」——它只能证明**这 16 个地址里没有它**；
- CMD13 是「有响应才算数」的命令。它分不开「命令根本没发出去」和「卡收下但不回话」，
  而这两件事指向完全不同的下一步（MMCC 时钟门 vs 卡的状态/供电）；
- round88 已经在这颗地址上等到过一次控制器自己的超时，round89 把同一件事重复了 16 次。
  继续换地址、换命令都不会多出信息。

所以 round90 不再问「你在哪个地址」，而是先问一个更底层的事实：**这颗时钟还在把命令移出去吗？**

### v0.2.62（round90）：重头识别 —— CMD0 两次、CMD1/CMD2/CMD3

这一轮只问两个问题，第二个问题的答案只有在第一个成立时才有意义：

1. 固件交给我们的那条 200 MHz 总线上，命令通路还在不在？
2. 如果在，卡能不能从 CMD0 开始被重新识别一遍，并把 RCA 定成我们自己选的 1？

CMD13 答不了第一个问题，因为它的成功条件是**卡回话**。CMD0 不一样：它没有响应类型
（`RESP_NONE`），SDHCI 只要把命令字移出去就置 `INT_COMMAND_COMPLETE`。于是
`cmd0a ok` = 时钟在跳、命令在走；`cmd0a to` = 什么都没移出去，卡是否活着根本还没轮到它说话。
这里要克制一种误读：`ok` 只证明**命令移出去了**，卡收没收下、认不认，CMD0 不会说（它没有响应）；
「卡活着」要靠随后的 CMD1 R3 来证明。

CMD0 发两次，这是这一轮的全部技巧：

- **cmd0a**：在固件留下的时钟上（`clk=0007`，分频 0 = 200 MHz），什么都不改；
- **cmd0b**：在我们把卡时钟改成 392 kHz 之后，同一条命令再来一次。

两次之间夹着一次时钟改写。于是四种读数各自指向不同结论，「时钟早就没了」与「是我们把它弄坏的」
不再混在一起。

| 改动 | 作用 |
| --- | --- |
| `drivers/CloverSdhci/CloverSdhci.cpp` | mode 4：`SOFTWARE_RESET`（只重置命令/数据两组）→ CMD0a → 降到 392 kHz → CMD0b → CMD1 循环 → CMD2 → CMD3 |
| `board.toml` | `clover-emmc=4`、菜单标题换成这一轮的问题 |
| `scripts/verify-clover-payload.py` | `check_sdhci()` 的第六条串改成 `no pwr`（原因见下） |

具体形状：

- 先只重置 `RESET_CMD | RESET_DATA`，不用 `RESET_ALL` —— 后者会把供电与时钟一起清掉，
  而这两个正是本轮要量的东西。重置位自清，等它自清也自带 `1 << 22` 的上限；
- **CMD0a**：`ARGUMENT=0`、`COMMAND=0x0000`（`RESP_NONE`），等 `INT_COMMAND_COMPLETE`；
- 降速：先清 `CARD_EN`，再写 `INT_EN | (255 的低 8 位 << 8) | (255 的高 2 位 << 6)`，
  轮询 `INT_STABLE`，稳定后才重新置 `CARD_EN`。基数取 `CAPABILITIES` 的 15:8（round87 读到
  `0xc8` = 200 MHz），10 位分频 255 ⇒ `200 MHz / (2 × 255) = 392 kHz`，落在识别时钟
  100–400 kHz 的窗口里；
- **CMD0b**：同一条无响应命令，在 392 kHz 上再发一次；
- **CMD1**：`OCR = 0x40FF8080`（EDK2 的 `EMMC_CMD1_CAPACITY_GREATER_THAN_2GB`），
  短响应、不查 CRC/索引（R3 本来就没有）。最多 200 次，只有 R3 的 bit31（PowerUp）置起来才算过；
  卡回了话但 bit31 一直不置时报告 `no pwr`；
- **CMD2**：长响应 + CRC（R2，CID）；**CMD3**：短响应 + CRC + 索引（R1），
  `ARGUMENT = 1 << 16`，RCA 由我们自己定成 1。

顺序与常量都有本地一手源码可对：`Mu_Basecore/EmbeddedPkg/Universal/MmcDxe/MmcIdentification.c`
的 `:572`（CMD0）、`:588`（CMD1 循环）、`:619`（进入 `EmmcIdentificationMode`）、`:141` 起
（CMD2 → `RCA = ++mEmmcRcaCount` = 1 → CMD3 → CMD9 → CMD7），以及
`EmbeddedPkg/Include/Protocol/MmcHost.h:64-65` 的 `0x00FF8080` / `0x40FF8080`。

报告仍然是 7 行（最长一行 58 字符，小于面板的约 66，不会折行）。下面是形状示例，不是读数：

```
CloverSdhci: pre clk=0007 pw=0b hc=38 hn=0003 pst=01f800f0
cmd0a ok rst=1 int=00000001
clk2=ff07 st=1 en=1 div=255 392kHz
cmd0b ok int=00000001
cmd1 ok tries=2 rsp=c0ff8080
cmd2 ok rsp=15010047...
cmd3 ok rca=0 state=2 rsp=00010000
```

`cmd3` 那一行有个**必须提前说清的坑**：eMMC 的 R1 里 bits 31:16 是保留位，RCA 不像 SD 那样
回读，所以 `rca=0` 是正常的；判定成功看的是 `ok` 与 `state=2`（identification）。state 反映的是
命令生效**之前**的状态；CMD3 之后卡才进 stand-by，`state=3` 要等下一颗命令（下一轮的
CMD9/CMD7）才会出现在 R1 里。

产物：这一轮又换内核（`92eb373a…` → `e0adb6c5…`，+16384 B），根镜像与 trust cache 一字未动；
卷内副本 `fb905074…`。

```
scratch-data.img == signed root            OK
trustcache == published bundle             OK
kernel.macho: probe patch localized        OK (5960 bytes in 35 runs, sha256 fb905074…)
kernel.macho: original-PMGR bindings alive OK (5 bindings, 0 moved; provider text present)
kernel.macho: CloverSdhci linked           OK (六条串全在)
```

第六根钉子是 `no pwr` 而不是报告里的任何标签，这是构建逼出来的：mode 4 的 `emitText("…")`
字面量都被 clang 折叠成立即数直接写进 `__TEXT,__text`，`__cstring` 里一个都不留；只有被取过
地址的串还在。`no pwr` 是本轮新增、只在三元表达式里返回的串，所以它才是「mode 4 真的链接
进来了」的证据。六条串是：`clover-emmc`、`look-only dump of c0c4000`、`CMD13 rca=1`、
`CMD13 sweep`、`no pwr`、`CloverSdhci: failed at stage`。

boot-args 自证：卷里的 `unixnu.toml` 是
`… clover-pmgr=2 clover-emmc=4 armcpu-clock-panic=60000`，574 B / `c5fdac95…`，
标题 `clover emmc: start over -- CMD0, CMD1, CMD2, CMD3 at 392 kHz`。

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round90-payload.img` | 58720256 B | `f0a3a71f612b6a0c3fc1fd16b2dfd4f80af8e61fcd37b7875943ad676a65d942` |
| `round90-kernel.macho`（构建产物） | 51879936 B | `e0adb6c5dc520d356407a0245a92249f2d0a92a1e4a90a28e4658512550e9ad2` |
| `round90-in-volume-kernel.macho`（卷内副本，含探针与 panic 屏） | 51879936 B | `fb905074823c9f53944148ff2aad24e249993e54a5358745343ee2ced73befa5` |
| `round90-scratch-data.img`（与 round85–89 相同） | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round90.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round90.adt` | 13476 B | `c2714d473382144e2d2b4e299a49d93cd5aad3ad68e14781eca17cba25575112`（每轮 `random-seed` 都换，payload 因此每次构建都不同） |
| `round90-unixnu.toml` | 574 B | `c5fdac952e26ab9828b6bbe2849d10b80371b18115f6488303af5959a10b7ea0` |

固件与 boot 分区仍然一个字节没碰，`round87-boot.img`（`196b284f…`）继续有效。刷机只需换 `cache`：

```sh
fastboot flash cache ~/clover-out/round90-payload.img
fastboot reboot
```

要让分区状态完全可复现就连 boot 一起刷（先 boot 后 cache）：

```sh
fastboot flash boot  ~/clover-out/round87-boot.img
fastboot flash cache ~/clover-out/round90-payload.img
fastboot reboot
```

**拍一张就够。** 判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `cmd0a to`（`rst=1`） | 固件交出的时钟下，命令一个字都没移出去 —— `clk=0007` 是寄存器的读数，不是线上的事实；嫌疑转向 MMCC 对 SDCC1 的门控 | round91：先把 MMCC 那路根时钟查出来（`clover.dts` 的 `sdhc_1` 时钟表 + RCGR 偏移），或对时钟控制做一次完整 `RESET_ALL` 后重发 CMD0 |
| `cmd0a ok`，`clk2` 的 `st=1 en=1 div=255`，`cmd0b to` | 命令通路本来活着，是**我们改时钟把它弄坏了**（分频写法、PLL 位或稳定等待） | round91：保留固件时钟只做 `RESET_CMD`，或只改分频不碰 `INT_EN`，把三种写法分开 |
| `cmd0b ok`，`cmd1 to` | 时钟与命令通路都通，卡不答 CMD1 —— 卡不在 idle（CMD0 没被它收下）、停在 sleep，或供电/复位没放 | round91：先做一次完成的上电循环（`POWER_CONTROL` `0x0f` → `0x0b`）再 CMD0/CMD1 |
| `cmd1 no pwr`，`tries=200` | 卡收下了 CMD1 但不置 PowerUp —— OCR 电压窗口与这条 1.8 V 轨不匹配，或卡忙 | round91：把 200 次拉长并打印 R3 原值，按 OCR 的电压位改 `ARGUMENT` |
| `cmd1 ok` + `cmd2 ok` + `cmd3 ok`，`state=2` | 卡被重新识别，RCA=1 是我们自己写下去的 | round91：CMD9（CSD）→ CMD7（选中）→ CMD17 读 LBA 1，前五字节应当自己说出 `EFI PART` |
| `cmd2 crc` / `cmd3 crc` | 命令出去了但响应 CRC 不过：392 kHz 下仍像采样相位或线上完整性 | round91：再降一档（分频 511 ≈ 196 kHz），并确认重置后卡回到 1-bit |
| 只有 `CloverSdhci: failed at stage …` | 窗口映射或 mode 参数错 | 2 = mode 不认识、3 = 映射失败；4/5/6 是本轮三个等待点 |
| **什么 panic 屏都没有** | `COMMAND`/`ARGUMENT`/`CLOCK_CONTROL` 其中一次写把互联挂住了（`ticks=0`，看门狗不会响） | 把三颗写拆成三轮单写，或用 device-nGnRnE 映射重试 |

**这轮之后仍然没有块设备。** round90 若成功，得到的是「总线是活的 + 卡在 392 kHz 上完成了
识别 + RCA=1」；还差 CMD9/CMD7 与 Data 通路（`SDHCI_BUFFER` 32-bit 或 ADMA64 表）和
`IOBlockStorageDevice`。日志分区那件事要等这三样齐了才开始。

### round90 读数（v0.2.62 已发布）：命令通路活着，但卡收到的从不是 392 kHz

照片上是五行：

```text
cmd0a ok int=00010000
clk2 st=0 en=1 div=255 clk=ff05
cmd0b ok int=00010000
cmd1 to tries=1 to=1 rsp=00000000
cmd2 skip
cmd3 skip
```

由此确定：

- **两次 CMD0 都完成。** 第一次在固件交出的时钟下，第二次在写进 10 位分频之后，所以
  SDHCI 的内部时钟与命令移位器都活着；`RESET_CMD`、`ARGUMENT`、`COMMAND` 三条写没有把
  互联挂住。
- **CMD0 完成不等于卡收到了时钟。** CMD0 是 `RESP_NONE`，`Command Complete` 由主机自己
  产生；卡有没有看到这根命令线，CMD0 证明不了。`clk2 st=0`（`INT_STABLE` 为 0）与
  `clk=ff05` 也在提示：分频字段没有被硬件当真。
- **CMD1 是第一次静默。** `to=1`、`rsp=0`，没有 CRC、没有 index 错误 —— 命令线上没有
  噪声，是没人答。round90 的修法是在第一次超时上 `break`，把「还没准备好」当成「永远
  不会」。

真正的原因是 Linux 的 MSM 驱动自己写下来的：`sdhci-msm.c:1766` 附近，
`__sdhci_msm_set_clock()` 明说 **“MSM controller do not use clock divider”**，写
`CLOCK_CONTROL` 时连分频都不放；卡时钟来自 GCC 的根时钟。固件为 HS200 把
`sdcc1_apps_clk_src` 留在约 200 MHz，`div=255` 只是一个没有硬件读的数。200 MHz 不是
识别时钟，CMD1 从一开始就没有机会。

### v0.2.63（round91）：把 400 kHz 写进 GCC 根时钟

这一轮不再动 SDHCI 的分频，改在硬件真正取时钟的地方：

1. 映射 GCC 窗口 `0x116000`（`clock-controller@100000`，reg 0x100000 + 0x94000；
   SDCC1 的 RCG 在 +0x16000）。
2. 先 `SOFTWARE_RESET = RESET_ALL` 停住主机，再确认并使能两路分支门控
   `gcc_sdcc1_apps_clk`（0x16004）与 `gcc_sdcc1_ahb_clk`（0x16008）。
3. 按 `clk-rcg2.c` 的顺序写 M/N/D → CFG → 置 update 位并等它自清。目标是
   `gcc-sdm660.c:735` 频率表里唯一一行 400 kHz：`F(400000, P_XO, 12, 1, 4)` =
   XO 19.2 MHz / 12 × 1/4。`clk-rcg.h:10` 把分频 h 存成 `2h-1`，所以 CFG[4:0] =
   23 = 0x17；MND 是 M=0x01、N=~3=0xfc、D=~4=0xfb，模式位 12 置 1，CFG = 0x2017，
   mask 0x0010371f。Linux 给 update 500 µs，这里的上界是一个自旋；面板上的 `up=0`
   就是上界到期。
4. 主机按 EDK2 的冷启动顺序回到 legacy：`TIMEOUT_CONTROL = 0x0e`、1-bit
   （`HOST_CONTROL = 0`）、`HOST_CONTROL2 = 0`、`POWER_CONTROL` 先 0x0a 再 0x0b
   （1.8 V 的开关循环，`SdMmcPciHci.c:975`），然后 `INT_EN` 等 `INT_STABLE`，**不写
   分频**。
5. 卡时钟仍关着时，先把 vendor mux（`sdhci-msm.c:78`，0x20c）的 10:8 指回默认的
   自由运行 MCLK（2）、清掉 HS400 的输入选择（bit18 / 21:19），再清 0x250 的
   `PWRSAVE_DLL`，最后 `INT_EN | CARD_EN`。
6. CMD0 → CMD1（最多 200 次，超时不再当作终点；每次之间自旋一段）→ CMD2 → CMD3。
   CMD1 的 R3 只有 bit31 置位才算 PowerUp 完成；R3 到了但 bit31 为 0，判 `no pwr`。

报告是八行，最长约 58 字符：

```text
gcc0 cmd=00000000 cfg=0000a007 br=8000000f ahb=0000000f
gcc1 cfg=00002017 m=01 n=fc d=fb up=1 br=0000000f
sdh rst=1 clk=0007 pw=0b hc=00 h2=0000
vs 00000200 vs3 00000000 ps 01ff0000
cmd0 ok int=00010000
cmd1 ok t=1 to=0 rsp=c0ff8080
cmd2 ok rsp=...
cmd3 ok rca=1 st=2 rsp=...
```

（这些数值是**格式示意**，不是预测；`cfg`、`br`、`ps` 和三个响应字以实机照片为准。）

构建时撞到一个新错误：clang 把 `volatile uint32_t` 的自增当成
`-Werror,-Wdeprecated-volatile`，两处延时循环改成 `spin = spin + 1` 才能过。

产物（内核这一轮**变了**，root 与 trust cache 没动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round91-payload.img` | 58720256 B | `5ab7295505d56230391fe07120e995c68545f030422205990bcd352256a2435a` |
| `round91-kernel.macho`（构建产物） | 51879936 B | `5940ab2173a3756f0b2d01f4b5498265ee3c33d55cb0e8399e7146bd44e98e85` |
| `round91-in-volume-kernel.macho`（卷内副本，含探针与 panic 屏） | 51879936 B | `a6c94014f24cb03da5904a011abf63f44319888f88014c23746bc461f00f65e7` |
| `round91-scratch-data.img`（与 round85–90 相同） | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round91-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round91-kernel.macho.adt` | 13476 B | `8e74ea739459d0e156ed0a69fd3f6700cd6d2deb66d921078c7959be84ef5ceb`（`random-seed` 每轮都换，payload 因此每次构建都不同） |
| `round91-unixnu.toml` | 565 B | `7d077dc01f2daa105b36dd9696466b0e953bdf376edd92167244fb9b2e2916b4` |

校验器读数：探针补丁定位 35 处、5958 字节；PMGR 5 条绑定 0 位移；CloverSdhci
六条串全在。卷内 boot-args 末尾是
`… clover-pmgr=2 clover-emmc=5 armcpu-clock-panic=60000`，标题
`clover emmc: clock at GCC -- 400 kHz, then identify`。固件与 boot 分区不变，
`round87-boot.img`（`196b284f…`）继续有效：

```sh
fastboot flash cache ~/clover-out/round91-payload.img
fastboot reboot
```

**拍一张就够。** 判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `gcc1 … up=0` | RCG 的 update 位没有自清：窗口/偏移认错，或门控没真正打开，卡看到的还是旧时钟 | 回读 0x11602c 与两路门控；按 Linux 的 `clk_rcg2_configure` 逐字段比对 |
| `gcc1 cfg` 不是 `00002017` | CFG 的 mask 或源选择写歪了 | 只改 CFG 一位一位试，保住 M/N/D 不动 |
| `sdh rst=0` | `RESET_ALL` 没有自清，主机没停住 | 先查 GCC 是否在复位域里被关掉；必要时先不动时钟只做复位 |
| `sdh clk` 的 `INT_STABLE` 为 0 | 内部时钟没稳，等于 GCC 没输出 | 门控电平与 RCG 实际频率；这会把问题钉在时钟树上而不是卡上 |
| `cmd1 ok`（`rsp` bit31=1）+ `cmd2 ok` + `cmd3 ok` | 卡在真正的 400 kHz 上完成了识别，RCA=1 是我们写下去的 | 下一轮：CMD9 读 CSD → CMD7 选中 → CMD16 设块长 → CMD17 读 LBA 1，前五字节应自己说出 `EFI PART` |
| `cmd1 to`，`to=200` | 换了真识别时钟、重试 200 次仍然静默 | 问题不再是时钟：查供电/复位与 CMD 线，先做完整上电循环再 CMD0/CMD1 |
| `cmd1 no pwr`，`t=… to=0` | 卡回了 R3 但不置 PowerUp：OCR 电压窗口与这条 1.8 V 轨不匹配，或卡忙 | 打印 R3 全值，按 OCR 的电压位改 `ARGUMENT`，并拉长重试 |
| `cmd2 crc` / `cmd3 crc` | 命令出去了、响应 CRC 不过：400 kHz 下不像采样相位问题，更像线上完整性 | 检查 1-bit 与上拉；必要时降到约 200 kHz 再试 |
| 只有 `CloverSdhci: failed at stage 4` | GCC 窗口映射失败 | 窗口地址/大小；2/3/4 分别对应参数、host 窗口、GCC 窗口 |
| 只有 `failed at stage 5/6/7` | 分别停在 GCC 编程、主机初始化、CMD1 | 用前一行读数定位哪一个等待点 |
| **什么 panic 屏都没有** | 某次 GCC 写把互联挂住了（`ticks` 不涨，看门狗也不响） | 把门控、MND、CFG、update 拆成四轮单写 |

eMMC 的 R1 有一个容易误读的地方：**bits 31:16 在 eMMC 上是保留位**，所以
`cmd3 rca=0` 是正常的；判定靠 `ok` 与 `state`，而 `state` 是转换**前**的状态（stand-by=3
要等下一条命令才出现，CMD3 之后读到的 2 是 identification）。

**这轮之后仍然没有块设备。** 若识别通过，得到的只是「卡在 400 kHz 上活着、RCA=1」；
还差 CMD9/CMD7/CMD16/CMD17 与 Data 通路（`SDHCI_BUFFER` 32-bit 或 ADMA64 表）和
`IOBlockStorageDevice`。日志分区要等这三样齐了才开始。

### round91 读数（v0.2.63 已发布）：GCC 那一半全对，主机那一半没跑

照片上的报告：

```text
CloverSdhci: gcc0 cmd=00000000 cfg=0000050f br=00004221 ahb=20008001
gcc1 cfg=00002017 m=01 n=fc d=fb up=1 br=00004221
sdh rst=0 clk=0003 pw=00 hc=00 h2=0000
vs 00240a0e vs3 02626440 ps 01f800f0
cmd0 skip int=00000000
cmd1 skip t=0 to=0 rsp=00000000
cmd2 skip
cmd3 skip
```

（`ahb=` 在面板上折成两行，完整值是 `20008001`。）

这轮把一个假设切成了两半，两半的答案相反：

- **GCC 那一半完全成功。** `cfg=00002017`、`m=01 n=fc d=fb`、`up=1`，说明
  `sdcc1_apps_clk_src` 接收了频率表里的 400 kHz 行，update 位也自清了；两路门控
  `br=00004221` 与 `ahb=20008001` 的 bit0 都是 1、bit31 都是 0。round91 关于
  「MSM 的卡时钟来自 GCC 根时钟」的推断因此被寄存器证实。
- **主机那一半一步都没走。** `rst=0`：`RESET_ALL` 在我们轮询的上界内没有自清，
  于是 `resetDone` 为假，后面的 legacy 初始化与 CMD0–CMD3 全部被跳过。`clk=0003`
  与 `pw=00` 不是「主机上电后的读数」，而是那次复位写留下的现场。
- `vs=00240a0e` 里 bit18（`SELECT_IN_EN`）为 1、bits 21:19 = 4，正是固件留下的
  HS200 DLL 选择；`vs3=02626440` 的 `PWRSAVE_DLL` 已经是 0。

所以 round91 没有回答「400 kHz 够不够」——它回答的是 **GCC 编程正确，而在固件的
HS200 卡时钟仍在运行时写 `RESET_ALL` 会把主机卡住**。Linux 的 `sdhci_set_clock(0)`
在关时钟时会先把 vendor mux 指回默认、再停时钟；顺序反了，复位就可能等不到它要的
条件。

### v0.2.64（round92）：先停卡时钟，绕开 `RESET_ALL`

mode 6 只改主机入口，GCC 那一段原样保留（这轮的报告仍带 `gcc0`/`gcc1`，回归一眼看得见）：

1. 读 `CLOCK_CONTROL`（报告里的 `clk=`）。
2. 写 `CLOCK_CONTROL = 0`，轮询 `INT_EN` 与 `CARD_EN` 都清零；读回来的值就是 `clr=`。
   **如果 `clr=` 非零，整段 GCC 编程被跳过**——在活着的卡时钟上改根时钟不是这版驱动
   敢冒的险。
3. 只复位命令与数据状态机（`RESET_CMD|RESET_DATA = 0x06`，round90 已经见它自清），
   不再碰 `RESET_ALL`。`rst=` 是这个掩码的自清结果。
4. GCC：门控 → M/N/D → CFG=0x2017 → update，和 round91 相同。
5. 主机仍关着时钟时，先把 vendor mux 的 10:8 指回默认 MCLK 2、清 `SELECT_IN_EN`
   与 bits 21:19、清 `PWRSAVE_DLL`；然后 `TIMEOUT_CONTROL=0x0e`、1-bit、
   `HOST_CONTROL2=0`、`POWER_CONTROL` 0x0a→0x0b、`INT_EN` 等 `INT_STABLE`，最后
   `INT_EN|CARD_EN`。
6. CMD0 → CMD1（最多 200 次）→ CMD2 → CMD3。

报告变成九行：

```text
gcc0 cmd=00000000 cfg=0000050f br=00004221 ahb=20008001
gcc1 cfg=00002017 m=01 n=fc d=fb up=1 br=00004221
sdh0 clk=0007 rst=1 clr=0000 vs3 02626440
sdh1 clk=0007 pw=0b hc=00 h2=0000 vs 00240a0e ps 01f800f0
cmd0 ok int=00010000
cmd1 ok t=1 to=0 rsp=c0ff8080
cmd2 ok rsp=...
cmd3 ok rca=1 st=2 rsp=...
```

（同样只是格式示意。）

产物（内核又变了，root / trust cache 仍未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round92-payload.img` | 58720256 B | `3d005d526187abcb5375f3998e210ace1fd7c0c1a8f861a739a5226891d6185a` |
| `round92-kernel.macho`（构建产物） | 51879936 B | `bf388d39293a3fb5d98b6c4d0a006186255bbfb339c3ee6f2ea16394dc83a4b7` |
| `round92-in-volume-kernel.macho`（卷内副本） | 51879936 B | `b11e4e0169f3e31bbe8d37bcf9672763609e4a27019f25403e394ed41e32bf60` |
| `round92-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round92-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round92-kernel.macho.adt` | 13476 B | `66d3b4430e42ec268e610121c9e75aaffa268d4c26134513d8c4ee2a55ea8807` |
| `round92-unixnu.toml` | 562 B | `a92e9b0d72282abcd3b829e376439cceef4e983bfdcf689d4486f97a930c1789` |

校验器：探针 35 处 5959 字节、PMGR 5 条绑定 0 位移、CloverSdhci 六条串全在。卷内
boot-args 末尾是 `… clover-pmgr=2 clover-emmc=6 armcpu-clock-panic=60000`。boot 仍是
`round87-boot.img`（`196b284f…`）：

```sh
fastboot flash cache ~/clover-out/round92-payload.img
fastboot reboot
```

**拍一张就够。** 判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `clr=0007`（`CARD_EN` 没清掉） | 写 `CLOCK_CONTROL` 无效；代码按设计跳过了 GCC 与全部命令 | 先查为什么卡时钟关不掉：供电/`POWER_CONTROL` 与 vendor DLL 的关系，或改用 vendor 侧停时钟 |
| `clr=0000`、`rst=0` | 连 `RESET_CMD|RESET_DATA` 都不自清，主机状态机是真的卡住 | 下一轮完全不复位；只停时钟 + 改 GCC + 直接发 CMD0 |
| `clr=0000 rst=1 up=1`，`sdh1 clk=0007 pw=0b` | 主机真的上电、真的有时钟，legacy 路径成立 | 看 CMD 四行 |
| `sdh1 clk=0003 pw=0b` | 供电写进去了，但 `CARD_EN` 仍不置位 | 控制器拒绝开卡时钟：怀疑 vendor `SELECT_IN`/DLL 或需要一次完整复位；这是下一轮要切的点 |
| `cmd0 ok`，`cmd1 ok`（bit31=1），`cmd2/3 ok` | 卡在真正的 400 kHz 上完成识别，RCA=1 | 下一轮 CMD9 → CMD7 → CMD16 → CMD17 读 LBA 1，前五字节应说出 `EFI PART` |
| `cmd1 to`，`to=200` | 时钟与命令都通了，卡仍静默 | 问题转到供电/复位/CMD 线，不再是时钟 |
| `cmd1 no pwr` | 卡回了 R3 但不置 PowerUp：OCR 电压窗口或卡忙 | 打印 R3 全值，按 OCR 电压位改 `ARGUMENT` |
| 只有 `CloverSdhci: failed at stage 4` | GCC 窗口映射失败 | 窗口地址/大小 |
| **什么 panic 屏都没有** | 某次寄存器写把互联挂住（`ticks` 不涨） | 把停时钟、复位、GCC、上电拆成四轮单写 |

**这轮之后仍然没有块设备。** 若识别通过，还差 CMD9/CMD7/CMD16/CMD17 与 Data 通路
（`SDHCI_BUFFER` 32-bit 或 ADMA64 表）和 `IOBlockStorageDevice`。

### round92 读数（v0.2.64 已发布）：主机活了，卡答了，CMD2 倒在 CRC

照片上的报告（面板宽度会折行，数值以这里为准）：

```text
CloverSdhci: gcc0 cmd=00000000 cfg=0000050f br=00004221 ahb=20008001
gcc1 cfg=00002017 m=01 n=fc d=fb up=1 br=00004221
sdh0 clk=0007 rst=1 clr=0002 vs3 02626440
sdh1 clk=0005 pw=0b hc=00 h2=0000 vs 00000a0e ps 01f800f0
cmd0 ok int=00010000
cmd1 ok t=3 to=0 rsp=c0ff8080
cmd2 crc rsp=00000000
cmd3 skip
```

这一轮把「主机能不能动」和「卡还认不认我们」一起答完了：

- **停卡时钟是那把钥匙。** `clk=0007`（`INT_EN|INT_STABLE|CARD_EN`）写 0 之后读回
  `clr=0002`：`INT_EN` 与 `CARD_EN` 都清了，只剩 `INT_STABLE` 常 1。随后
  `RESET_CMD|RESET_DATA` 一次就自清（`rst=1`）——round91 卡住的从来不是那个小复位，
  而是仍在跑的 HS200 卡时钟。
- **GCC 半边没被这轮改动碰坏。** `cfg=00002017`、`m=01 n=fc d=fb`、`up=1`、门控开着，
  与 round91 逐位相同。
- **主机真的上电、真的有时钟。** `sdh1 clk=0005` 是 `INT_EN|CARD_EN`（`INT_STABLE`
  在上电切换后被清掉，正是 EDK2 那套顺序的结果），`pw=0b`（1.8 V + 开），
  `hc=00`（1-bit、legacy），`h2=0000`，vendor mux 回到 MCLK 默认
  （`vs=00000a0e`：`SELECT_IN_EN` 与 bits 21:19 都清了）。
- **卡在 400 kHz 上认出了自己。** `cmd0 ok` 说明命令还在往外移；`cmd1 ok t=3 to=0
  rsp=c0ff8080` 是卡自己的 OCR，bit31 在第三次轮询时置起——前两次是「上电中」，
  不是超时。**48 位响应的采样通路是对的**（CRC 也过了）。
- **CMD2 是第一颗要求主机校验响应的命令，它倒了。** `cmd2 crc` 且 `rsp=00000000`：
  136 位响应一个字节都没被接受，`commandVerdict` 把 `CRC|END_BIT|INDEX` 合成一个
  `crc`，所以这张照片还分不出是哪一位说的。CMD3 因此在设计里被跳过。

一个必须记住的卡侧事实：**CMD2 只在 ready 态合法，而卡一旦收到 CMD2 就进入
identification，无论主机是否接受那个响应**。所以「再发一次 CMD2」不能直接发，
必须先 CMD0 + CMD1 轮询把卡放回 ready。

### v0.2.65（round93）：CMD2 发两次，一次带 CRC、一次不带

mode 7 保留 mode 6 的整套 bring-up（停时钟 → 小复位 → GCC → legacy 上电 → CMD0 →
CMD1），只改 CMD2 这一段。两个答案指向不同的硬件，所以这一轮把两种都问：

1. **A = mode 6 的命令逐字节不变**（`LONG|CRC`），但报告打印原始 `INT_STATUS` 与
   **四个响应字**，不再只给一个合成的判定。
2. A 成功 → 直接发 CMD3。
3. A 失败 → 先 `RESET_CMD` 清命令电路，再 **CMD0 + 完整的 CMD1 轮询**把卡重新武装
   回 ready（这一段有自己的报告行）。
4. **B = 同一条 CMD2，但关掉 CRC 门**（只有 `LONG`）。CID 完整到达 ⇒ 坏的是主机的
   CRC 校验；B 也失败 ⇒ 坏的是 136 位响应的采样/锁存通路。B 之后立刻读一次
   `PRESENT_STATE`，让「响应到了但报错」与「线仍是静的」多一条旁证。
5. 只要有一次 CMD2 到达了卡，就发 **CMD3**（`CRC|INDEX`）：一条干净的 R1 把
   「响应通路是坏的」和「只有 136 位 CRC 不同意」分开。

报告是下面这个形状（A 成功时没有 `reinit`/`cmd2b`/`cidB` 三行）：

```text
gcc0 cmd=00000000 cfg=0000050f br=00004221 ahb=20008001
gcc1 cfg=00002017 m=01 n=fc d=fb up=1 br=00004221
sdh0 clk=0007 rst=1 clr=0002 vs3 02626440
sdh1 clk=0005 pw=0b hc=00 h2=0000 vs 00000a0e ps 01f800f0
cmd0 ok int=00010000
cmd1 ok t=3 to=0 rsp=c0ff8080
cmd2 a=crc int=00008004
cidA 00000000 00000000 00000000 00000000
reinit r=1 c0=ok c1=1 t=3 to=0
cmd2b ok int=00010000 ps 01f800f0
cidB 15010044 334d4331 0042e3b3 1ecaf6ff
cmd3 ok rca=1 st=2 int=00010000
```

（格式示意，不是读数。）`cidA`/`cidB` 是 MSB 字在前，与 `RESPONSE[0x1c]→[0x10]`
的存放顺序一致；CID 的第一个字节是 Manufacturer ID（`15` Samsung、`45` SanDisk、
`90` SK Hynix、`11` Toshiba/Kioxia、`13` Micron）。

产物（内核又变了，root / trust cache 仍未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round93-payload.img` | 58720256 B | `822b7dca29d2ced8b2e7bcc46a3350a30d52747ae932faa32ca69de96d858cb7` |
| `round93-kernel.macho`（构建产物） | 51900416 B | `5fcfad1aa73d78a79ce05601d22e5958c22006c04dbff4eb51af57c49092b17c` |
| `round93-in-volume-kernel.macho`（卷内副本） | 51900416 B | `47558b35816509dd9b49f35c5a7a88869d82b0598a4c95cb1795155502c0d8dd` |
| `round93-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round93-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round93-kernel.macho.adt` | 13476 B | `88159e46a0289421dcb502047bb96f71522e10e151abb0af6042a8286f4faaa5` |
| `round93-unixnu.toml` | 563 B | `fad0de6ebb97116a0c054e1b634b20a3b7e218adbe77e783139263b8dadf5ea8` |

校验器：探针 35 处 5959 字节、PMGR 5 条绑定 0 位移、CloverSdhci 六条串全在。卷内
boot-args 末尾是 `… clover-pmgr=2 clover-emmc=7 armcpu-clock-panic=60000`。boot 仍是
`round87-boot.img`（`196b284f…`）：

```sh
fastboot flash cache ~/clover-out/round93-payload.img
fastboot reboot
```

**拍一张就够。** 判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `cmd2 a=ok` + 合理的 `cidA` + `cmd3 ok` | round92 的 CRC 是一次性的；识别可以继续 | CMD9 读 CSD → CMD7 选中 → CMD16 设块长 → CMD17 读 LBA 1，前五字节应是 `EFI PART` |
| `cmd2 a=crc`、`reinit c1=1`、`cmd2b ok` + 合理 `cidB` + `cmd3 ok` | **只有 R2 的 CRC 门不同意**，CID 本身是好的 | 后续只读命令关掉 CRC 校验并逐字记录响应；R2 的 CRC 差异单独查（采样边沿/延迟） |
| `cmd2b end` 或 `cmd2b crc` | 关掉 CRC 门后仍报错 ⇒ 响应通路本身有问题 | 查 136 位锁存与 DLL：下一轮改采样点/关 DLL、或把 4 个响应字在两次读里各取一遍 |
| `cmd2b to` | 卡在 400 kHz 上根本不答 CMD2，可它给的 OCR 是真的 | 下一步查 CMD1 的 `ARGUMENT`（sector-mode/access-mode 位）与卡侧状态，不再查响应通路 |
| `reinit c1=0` | 连重武装都没成功，`int=`/`rsp=` 在那一行上说出是 CMD0 还是 CMD1 停的 | 按那一行的位先修 CMD0/CMD1 |
| `cmd3 crc`/`idx` 而两次 CMD2 都 `ok` | 136 位响应能用、48 位 R1 反而不能用 | 矛盾点集中在响应长度切换（R2→R1）上，下一轮在 CMD2 之后先复位命令电路再发 CMD3 |
| 只有 `CloverSdhci: failed at stage 4` | GCC 窗口映射失败 | 窗口地址/大小 |
| **什么 panic 屏都没有** | 某次寄存器写把互联挂住（`ticks` 不涨） | 把 A/B 拆成两轮单写 |

### round93 读数（v0.2.65 实机）：只有 R2 的 CRC 门不同意

照片上的六行：

```text
cmd2 a=crc int=00028000
cidA 00c0ff80 80000000 00000000 00000000
reinit r=1 c0=ok c1=1 t=3 to=0
cmd2b ok int=00000001 ps 01f800f0
cidB 00150100 4448b644 41420281 ffcaaac5
cmd3 ok rca=0 st=2 int=00000001
```

逐条读：

- `int=00028000` = `INT_ERROR`(0x8000) + `INT_CRC`(0x20000)，没有 `INT_RESPONSE`：
  命令发出去了、响应也来了，是主机的 CRC7 校验拒绝它。
- `cidA` 不是坏 CID，是**上一次 CMD1 的 OCR**（`c0ff8080` 按 R2 的 136 位对齐后正好
  落成 `00c0ff80 80000000 …`）。被拒的命令不锁存响应字，所以失败行上的 `cid*` 只能按
  「没更新」读，不能按内容读。
- `cmd2b ok int=00000001` 加上完整 CID ⇒ 关掉 CRC 门后 136 位响应一位不差。
- `cidB 00150100 4448b644 41420281 ffcaaac5` 按 SDHCI 的 R2 约定（前 8 位为 0，随后
  120 位内容）解出 MID=0x15（Samsung）、OID=0x0100、PRV=0x02、PSN=0x81ffcaaa；PNM 的
  第三字节读成 `0xb6`（不是 ASCII）、MDT 读成 `0xc5`（解成 2002 年），两处都像第 7 位
  被翻——这是猜测，要看两次读的 CID 是否逐字节一致才能定成稳定的内容。
- `cmd3 ok rca=0 st=2`：eMMC 的 R1 里 RCA 域是保留的（`rca=0` 正常），`st=2` 是转换前
  的 identification ⇒ CMD3 被接受，卡已进入 stand-by、地址是 1。

所以唯一失败的部件是 **CMD2 响应的 CRC7 门**。但 A 与 B 同时变了两个量：CRC 开关，
以及 CMD1 ready 之后的等待时间（B 前面多一次 CMD0 与三轮 CMD1）。round93 分不开它们。

### v0.2.66（round94）：把 CRC 门挪到重武装的另一侧

mode 8 保留整套 bring-up，只把两次 CMD2 的内容对调：

1. **X** = `LONG`（关 CRC），放在 round93 A 的槽位：CMD1 报 ready 之后的第一条命令，
   中间什么都不插。
2. X 出错才 `RESET_CMD`，然后 **CMD0 + 完整 CMD1 轮询**重新武装。
3. **Y** = `LONG|CRC`，放在 round93 B 的槽位（重武装之后）。
4. Y 只要发出去，就发 **CMD3**（`CRC|INDEX`，RCA 1）。
5. CMD3 成功 → **CMD9**（SEND_CSD，`LONG|CRC`，第二条 136 位响应）与 **CMD13**
   （`CRC|INDEX`，RCA 1，看状态是不是 3 = stand-by）。

报告形状（示意；Y 失败时 `cidB` 又会是上一次响应的残留）：

```text
g cfg=00002017 m=01 n=fc d=fb up=1 br=00004221 ab=20008001
h clk=0007 rst=1 clr=0002 pw=0b vs 00000a0e vs3 02626440 ps 01f800f0
c0 ok c1 ok t=3 to=0 rsp=c0ff8080
c2a ok int=00000001
cidA 00150100 4448b644 41420281 ffcaaac5
ra c0=ok c1=1 t=3 to=0
c2b crc int=00028000
cidB 00c0ff80 80000000 00000000 00000000
c3 ok rca=0 st=2 int=00000001
c9 ok int=00000001
csd 00000000 00000000 00000000 00000000
c13 ok st=3 rsp=00000000
```

为塞进 511 字节，表头从四行压成三行（`g`/`h`/`c0 c1`），`hc=`/`h2=` 不再单列；
最坏情况 461 字节。

产物（内核又变了；root / trust cache 仍未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round94-payload.img` | 58720256 B | `a060238b3af868e8ce08a2cb0c0926fc72bee88a4098583b35c0479609405ea0` |
| `round94-kernel.macho`（构建产物） | 51900416 B | `d53c39b33da504a22f047d9fadf4c87f3823315100b74c58fbdec4f0424cc10e` |
| `round94-in-volume-kernel.macho`（卷内副本） | 51900416 B | `0d170bc7e08e7705fd657aa2e4b4bd9044d7dec7aa83b5aacc9ee47bdd843b7a` |
| `round94-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round94-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round94-kernel.macho.adt` | 13476 B | `46d91777b21094f52bfc62da160b0b46f922d8f99bd3134dcfb2a281bb662911` |
| `round94-unixnu.toml` | 578 B | `b7efce90ea81d8e6a499ee6915e0b5c536827bc28fd134d63f0c941fdfd5c312` |

校验器：探针 35 处 5960 字节、PMGR 5 条绑定 0 位移、CloverSdhci 六条串全在。卷内
boot-args 末尾是 `… clover-pmgr=2 clover-emmc=8 armcpu-clock-panic=60000`。boot 仍是
`round87-boot.img`（`196b284f…`）：

```sh
fastboot flash cache ~/clover-out/round94-payload.img
fastboot reboot
```

**拍一张就够。** 判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `c2a ok` + `c2b ok` | round93 的 A 是发得太早，CRC 门本身没问题 | 下一轮在 CMD1 ready 后加一段等待，然后 CMD9→CMD7→CMD16→CMD17 |
| `c2a ok` + `c2b crc`，两次 `cidA`/`cidB` 一致 | 不是等待问题：这个时钟下 CID 的 CRC 就是过不了（或校验电路本身） | 用软件 CRC7 校验同一份 CID，分清采样与校验；只读命令先关 CRC |
| `c2a ok` + `c2b crc`，两次 CID 逐字不同 | 采样边沿不稳 | 改采样点/DLL，或重复读到一致 |
| `c2a crc`/`c2a to` + `c2b ok` | 第一槽位本身坏，重武装才让它好 | 查 ready 之后 CMD 线的建立时间；下一轮固定等待 |
| `c9 ok` 而 `c2b crc` | CRC 门对 CSD 能用、只对 CID 不行 | 问题集中在 CID 那 17 字节的采样，不在校验电路 |
| `c9 crc` | 所有 R2 都过不了 CRC 门 | 校验通路本身：软件 CRC7 或对 R2 关校验 |
| `c3 to`/`crc` | CMD2 之后卡没进 identification | 先复位命令电路再发 CMD3 |
| 只有 `CloverSdhci: failed at stage …` | 主机侧初始化失败 | 按 stage 查对应的寄存器 |

### round94 读数（v0.2.66 实机）：settle 时间出局，R2 的锁存是完好的

照片上的画面（表头两行在 66 字符处折行，这里还原成一行）：

```text
g cfg=00002017 m=01 n=fc d=fb up=1 br=00004221 ab=20008001
h clk=0007 rst=1 clr=0002 pw=0b vs 00000a0e vs3 02626440 ps 01f800f0
c0 ok c1 ok t=3 to=0 rsp=c0ff8080
c2a ok int=00000001
cidA 00150100 4448b644 41420281 ffcaaac5
ra c0=ok c1=1 t=3 to=0
c2b crc int=00028000
cidB 00c0ff80 8048b644 41420281 ffcaaac5
c3 to rca=0 st=0 int=00018000
c9 skip
csd none
c13 skip
```

这落在判定表的「`c2a ok` + `c2b crc`，两次 `cidA`/`cidB` 一致」一行，而且比那一行多说三件事：

- **等待时间出局。** X 在 round93 A 的槽位（CMD1 ready 之后的第一条命令，中间什么都不插）
  返回 `ok`，Y 在 B 的槽位（重武装之后）返回 `crc`。A 失败不是「发得太早」，而是 CRC 门
  对这份 CID 的判定。
- **两次 boot 读出的 CID 完全一致**（`00150100 4448b644 41420281 ffcaaac5`）⇒ 采样是确定性
  的，不是边沿抖动；PNM 里的 `0xb6` 和 MDT 的 `0xc5` 要么是这颗卡的真实值，要么是同一个
  确定性的位错，但绝不是随机噪声。
- **`cidB` 不再是上一次的 OCR，而是「半个新 CID」。** 按 R2 的排布（`RESPONSE[3]` 是最高字）
  和 `cidA` 对齐：`RESPONSE[1]`、`RESPONSE[0]`、`RESPONSE[2]` 的低 3 字节逐字节等于真 CID
  （88 位），只有 `RESPONSE[2]` 的最高字节 `0x80` 和整个 `RESPONSE[3]`（`00c0ff80`）还是
  CMD1 的 OCR 顺移后的残影。⇒ **136 位帧确实到了、也确实移进了响应寄存器**，控制器是在校验
  失败那一刻停止锁存，而不是没收到。
- **`c3 to rca=0 st=0 int=00018000`**（`INT_ERROR|INT_TIMEOUT`）：CMD3 发出去了、没有人答。
  代码在这一步之前**已经**跑过 `RESET_CMD`（`canSend3` 的错误分支），所以这不是「命令电路
  还脏」：一次被 CRC 拒绝的 136 位响应之后，`RESET_CMD` 修不回下一条命令的通路。round93 的
  `cmd3 ok` 都是在**校验通过**的 CMD2 之后发的，两者不能直接对比。
- 所以 `c9 skip`、`csd none`、`c13 skip` 只是没轮到，不含信息。

于是下一轮的形状被钉死：**不再让 CMD2 走带校验的那条路**，用 round94 的 X（`LONG`，关校验）
完成识别；要问「R2 的 CRC 门是不是对 CID 特别坏」，唯一干净的问法是 CMD9（CSD，同样是 136
位），而且要在地址已经拿到、命令通路健康的时候发。

### v0.2.67（round95）：把卡第一次送进 transfer

mode 9 沿用整套 bring-up（GCC 400 kHz、停卡时钟、CMD+DATA 复位、主机 legacy 初始化），
然后把识别链走到底：

1. **CMD2** `LONG`（关 CRC）——round94 的 X，本机唯一接受的 CMD2；卡进入 identification。
2. **CMD3** `SHORT|CRC|INDEX`，RCA 1——卡拿到地址、进入 stand-by。R1 报的是转移**前**的状态，
   所以这一行 `st=2` 是期望值而不是失败。
3. **CMD13** `SHORT|CRC|INDEX`，RCA 1——stand-by 应为 `st=3`；这也是拿到地址后的第一条 R1，
   把「地址可用」与「CSD 读取的结果」分开。
4. **CMD9** `LONG|CRC`，RCA 1——第二条 136 位响应。`c9 ok` 说明只有 CID 过不了校验门；
   `c9 crc` 说明所有 R2 都过不了。CMD9 只在 stand-by 合法，所以必须在 CMD7 之前。
5. **CMD7** `SHORT|CRC|INDEX`，RCA 1——stand-by → transfer，所有数据命令的前提。
6. **CMD13** 再来一次——`st=4` 就是选中的证据，也是 round96 的 CMD16/CMD17 的起点。

每一步都以上一步成功为前提；任何一条命令报错，下一条命令之前先 `RESET_CMD`（并且把这次
复位**不够**当成 round94 已记录的读数，而不是当成修复）。表头去掉了 `ps`（`present state`
已稳定为 `01f800f0`），报告改为在标签之后另起一行：round94 的照片证明面板在 66 字符处折行，
`CloverSdhci: ` 这 13 个字符会吃掉第一行的预算。最坏情况约 400 字节。

报告形状（示意；`rsp`/`csd` 是占位，真实值由卡决定）：

```text
g cfg=00002017 m=01 n=fc d=fb up=1 br=00004221 ab=20008001
h clk=0007 rst=1 clr=0002 pw=0b vs 00000a0e vs3 02626440
c0 ok c1 ok t=3 to=0 rsp=c0ff8080
c2 ok int=00000001
cid 00150100 4448b644 41420281 ffcaaac5
c3 ok st=2 int=00000001
c13 ok st=3 rsp=00000000
c9 ok int=00000001
csd 00000000 00000000 00000000 00000000
c7 ok int=00000001
c13b ok st=4 rsp=00000000
```

产物（内核变了；root / trust cache 仍未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round95-payload.img` | 58720256 B | `e82a6cc9d9c9047bc78b79b32df9470743d861cdaec8412d3d0f99514fd59b80` |
| `round95-kernel.macho`（构建产物） | 51916800 B | `ea1165f2efe5f4d90e81cb7dfe302027cf3b31e29f370d60cac7ed9c2133ac04` |
| `round95-in-volume-kernel.macho`（卷内副本） | 51916800 B | `844cd33aa2238ffbafecd923edc07ac49a1352b9a4fe7b106c419725e32d2961` |
| `round95-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round95-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round95-kernel.macho.adt` | 13476 B | `74c1fef02b8d80a968cbef6fd9ad28b07cce6655903f1087b8e8f687957ea25a` |
| `round95-unixnu.toml` | 568 B | `7b832f02eec57d1b1ae0c9836792302a178b3c72737ea994b46783fcbe053058` |

校验器：探针 35 处 5960 字节、PMGR 5 条绑定 0 位移、CloverSdhci 六条串全在；反汇编
`__ZL11coldBringUpbj` 里能数到两条 `#0x909`（mode 8 与 mode 9 的 CMD9）和一条 `#0x71a`
（只有 mode 9 会发 CMD7）。卷内 boot-args 末尾是
`… clover-pmgr=2 clover-emmc=9 armcpu-clock-panic=60000`。boot 仍是
`round87-boot.img`（`196b284f…`）：

```sh
fastboot flash cache ~/clover-out/round95-payload.img
fastboot reboot
```

**拍一张就够。** 判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `c9 ok` + 合理的 `csd` | 只有 CID 过不了 R2 门；CSD 能读、地址可用、卡能选中 | CMD16 设 512 字节块长 + CMD17 读 LBA 1（前五字节 `EFI PART`） |
| `c9 crc` + `c7 ok` + `c13b st=4` | 所有 R2 都过不了校验门，但数据命令不受影响 | CSD 只用于容量；跳过它或关校验，直接 CMD16/CMD17 |
| `c9 crc` + `c7 to` | 校验失败之后命令通路确实不可用，与 round94 的 `c3 to` 同源 | 先修「CRC 错误后的恢复」：试 `RESET_ALL`，或先 CMD13 探活再继续 |
| `c3 to`/`crc` | 干净的 CMD2 之后 CMD3 不答，说明 round94 的 `c3 to` 与 CRC 无关 | 查 48 位响应通路与 CMD3 的参数 |
| `c13 to`/`crc` 而 `c3 ok` | 地址拿到了，但 R1 不答 | 查 RCA 与 R1 通路 |
| `c7 to`/`crc` 而 `c13 ok st=3` | 选中本身失败 | 查 CMD7 参数与卡状态 |
| `c13b` 不是 `st=4` | CMD7 回了但卡没进 transfer | 看 R1 的错误位与当前状态 |
| `c2 to`/`crc` 或 `c0`/`c1` 退步 | bring-up 退化（round94 之后不该发生） | 对照 round94 的表头与 `c0`/`c1` |
| 只有 `CloverSdhci: failed at stage …` | 主机侧初始化失败 | 按 stage 查对应的寄存器 |

## round95 读数：整条识别链通了，卡第一次在 transfer

round95 的照片一次把 mode 9 走完：`c2 ok`、`c3 ok st=2`、`c13 ok st=3 rsp=00000700`、
`c9 ok int=00000001`、`c7 ok`、`c13b ok st=4 rsp=00000900`。表头与 `c0`/`c1` 和 round94
逐字相同（`cfg=00002017`、`up=1`、`c1 ok t=3 rsp=c0ff8080`），所以 bring-up 没有退化。

照片里另外两个事实，决定了 round96 不再问识别：

1. **CSD 是干净的。** SDHCI 捕获 136 位响应时会丢掉 CRC 字节，把 `csd` 左移 8 位后与
   `facts/81ffcaaa-20261022-210928/mmc_device.txt` 的真 CSD（`d02701320f5903fff6dbffef8e404000`）
   **逐位相同** ⇒ 主机的 CRC7 门算得对，`c9 ok` 不是偶然。
2. **CID 只差一位。** 同一比较下 `cid` 的 byte 5 读到 `0xb6`，真值是 `0x36`（bit 87），
   其余全部一致。带着关掉的校验读一次是 `41420281…` 里的同一段，两次启动也一样 ⇒ 不是时钟
   或采样抖动，而是这颗控制器对 CMD2 响应某一位的固定偏差。CID 之后不再需要：容量从 CSD
   来，地址用 CMD3 分配的，选中用 CMD7。

## v0.2.68（round96）：第一颗数据命令 —— CMD16 + CMD17 读 LBA 1

mode 10 = mode 9 去掉 CMD9，然后第一次碰 Data：

1. **CMD2** `LONG`（关 CRC）→ **CMD3**（RCA 1）→ **CMD7**（RCA 1）→ **CMD13** 要求
   `st=4`。这四步 round95 已经证明过，这里只当回归门；任何一步失败，后面的数据命令不发。
2. **CMD16** `SHORT|CRC|INDEX`，arg=512（SET_BLOCKLEN）。失败不致命（复位后的块长本来就是
   512），先 `RESET_CMD` 再继续；`c16` 那一行是独立读数。
3. 数据设置：`TIMEOUT_CONTROL=0x0e`、`BLOCK_SIZE=512`、`BLOCK_COUNT=1`、
   `TRANSFER_MODE=0x10`（READ，DMA 位为 0 ⇒ PIO）、清 `INT_STATUS`、`ARGUMENT=512`。
   eMMC 按**字节**寻址，所以 LBA 1 = byte 512。
4. **CMD17** = `0x113a`（`index 17 | SHORT | CRC | INDEX | DATA`）。先等槽位（
   `CMD_INHIBIT|DATA_INHIBIT`），再等 CMD 组，再把 FIFO 前两个字存下来、排空剩余 126 个字
   （缓冲不空 `DATA_END` 不报），最后等 `DATA_END`。392 kHz 下一个块约 10 ms，所有等待都带
   `1<<22` 上限——这个调度点上中断没开，丢掉的完成事件就是永远的沉默。
5. **`efi 1`** 当且仅当那两个字是 `0x20494645 0x54524150`——"EFI PART" 的小端 32 位字。

报告形状（示意；`rsp`/`int` 都是占位）：

```text
g cfg=00002017 m=01 n=fc d=fb up=1 br=00004221 ab=20008001
h clk=0007 rst=1 clr=0002 pw=0b vs 00000a0e vs3 02626440
c0 ok c1 ok t=3 to=0 rsp=c0ff8080
c2 ok int=00000001
c3 ok st=2 int=00000001
c7 ok int=00000001
c13 ok st=4 rsp=00000900
c16 ok int=00000001
c17 ok int=00000023 ps=01f800f0 bs=0200
hdr 20494645 54524150
efi 1
```

没有发生传输时（`c16` 失败且复位没自清）不打印 `hdr`/`efi` 两行，避免把零当成块内容。
最坏情况约 430 字节，仍在面板的 512 字节以内。

产物（内核变了；root / trust cache 仍未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round96-payload.img` | 58720256 B | `4dcb915b1f555ede4cd0b5705710cb79d3ce4848bf310a34bee9a26db5ab1e87` |
| `round96-kernel.macho`（构建产物） | 51933184 B | `5730ddb094a8f1a18d8f78c9fe7642c423ba5132fc5510bc762b14f717fb2b70` |
| `round96-in-volume-kernel.macho`（卷内副本） | 51933184 B | `d1093dd7d8a725365e18b0e907ffc4158a453ac9bb6b65023decd21568af2998` |
| `round96-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round96-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round96-kernel.macho.adt` | 13476 B | `822b2883c5d63273d8a15161f354fd4967604ffb5242eb791cab46f5b397e948` |
| `round96-unixnu.toml` | 557 B | `d7acc34856707c34a2d47d87124a267e7c75ff7466e69b33ce9725a1be7b874b` |

校验器：探针 35 处 5959 字节、PMGR 5 条绑定 0 位移、CloverSdhci 六条串全在；反汇编
`__ZL11coldBringUpbj` 里各有一条 `#0x113a`（CMD17）与 `#0x101a`（CMD16），两条 `#0x909`
（mode 8/9 的 CMD9）和两条 `#0x71a`（CMD7）；`0x20494645`/`0x54524150` 的立即数对也在。
卷内 boot-args 末尾是 `… clover-pmgr=2 clover-emmc=10 armcpu-clock-panic=60000`。boot 仍是
`round87-boot.img`（`196b284f…`）：

```sh
fastboot flash cache ~/clover-out/round96-payload.img
fastboot reboot
```

**拍一张就够。** 判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `c17 ok` + `efi 1` | 数据通路工作、块寻址正确、LBA 1 真是 GPT 头 | 读 GPT 分区表 → 找目标分区 → 写/读日志块 |
| `c17 ok` + `efi 0` + `hdr` 非零 | 传输完成但字节不同：地址单位（块 vs 字节）或 FIFO 字序 | 用 `hdr` 前几字节对照「块寻址 = 0 号块」的假设，必要时改 `ARGUMENT` |
| `c17 ok` + `hdr 00000000 00000000` | 事件到了、FIFO 没内容（数据通路没真正搬数） | 查 `TRANSFER_MODE` 的 DMA 位、`BLOCK_COUNT` 与主机 `HOST_CONTROL` 的 DMA 选择 |
| `c17 dto` | 卡收了命令但不发数据：数据阶段超时 | 查卡状态（`c13 st=`）、块长与 `TIMEOUT_CONTROL` |
| `c17 dcrc` / `dend` | 数据到了但 CRC/结束位错 | 400 kHz 下不太可能是信号问题，先查总线宽度（应为 1-bit）与电源 |
| `c17 to` / `crc` 而 `c13 st=4` | 卡拒绝了读命令 | 看 `c16` 的结果与卡当前状态；可能需要先 CMD16 成功 |
| `c16` 不是 `ok`，`c17` 仍 `ok` | SET_BLOCKLEN 被拒但读照常（块长本就 512） | 把 `c16` 记成卡的怪癖，继续 |
| `c16`/`c17` = `busy` | 命令槽一直不空 | 数据/命令状态机卡住；下一轮先 `RESET_CMD|RESET_DATA` 再问 |
| `c2`/`c3`/`c7`/`c13` 任何一步退步 | bring-up 退化（round95 之后不该发生） | 对照 round95 的表头与 `c0`/`c1` |
| 只有 `CloverSdhci: failed at stage …` | 主机侧初始化失败 | 按 stage 查对应的寄存器 |

## round96 读数：命令全通、数据没来 —— 坏在主机这一侧的两处

照片原文（v0.2.68 报告的形状，末尾的 `@CloverSdhci.cpp:2155` 是 panic 调用点，不参与读数）：

```text
g cfg=00002017 m=01 n=fc d=fb up=1 br=00004221 ab=20008001
h clk=0007 rst=1 clr=0002 pw=0b vs 00000a0e vs3 02626440
c0 ok c1 ok t=3 to=0 rsp=c0ff8080
c2 ok int=00000001
c3 ok st=2 int=00000001
c7 ok int=00000001
c13 ok st=4 rsp=00000900
c16 ok int=00000001
c17 nodata int=00000021 ps=01f80af6 bs=0200
hdr 00000000 00000000
efi 0
```

逐项：

- **识别与选中没有退化。** `c2`/`c3`/`c7`/`c13` 与 round95 逐字相同，`c13` 的 R1 仍是
  `00000900`；表头与 `c0`/`c1` 也和 round92 起的所有读数一致。
- **CMD16 被接受**（`int` 只有 RESPONSE）。块长 512 成立，不是被拒后继续。
- **CMD17 的响应到了，数据阶段没有完成。** `int=00000021` = RESPONSE|DATA_AVAIL：命令收下了，
  FIFO 报告过“有数据”。但没有 DATA_END，也没有任何数据错误位（DATA_TIMEOUT/CRC/END_BIT/ADMA
  全为 0）—— 卡没有拒绝，也没有把 CRC 弄坏，只是没有把一块送完。
- **`ps=01f80af6` 说明主机方向是对的、并且还在等。** 逐位：bit1 DATA_INHIBIT=1、bit2
  DAT_LINE_ACTIVE=1、bit9 DOING_READ=1、bit8 DOING_WRITE=0、bit11 DATA_AVAILABLE=1，
  DAT[7:4] 与 DAT[3:0] 全为 1（固件留下的 8-bit 总线），bit24 CMD_LVL=1（命令线空闲）。
  读方向、FIFO 有字、数据机仍占线。
- **`hdr` 的两个零不能当成“0 号扇区”。** `DATA_AVAILABLE` 只保证“至少有一个字”，而 mode 10
  在拿到第一个 DATA_AVAIL 后无条件读满 128 个字；FIFO 空时读出来就是 0。这一轮没有证据说卡
  送过任何非零字节。

问题因此收敛到主机这一侧。对着 `sdhci-msm.c` 与 Linux 的 `sdhci.c` 逐行比，mode 10 与这条
路有两处不同：

1. **少写了 `SDHCI_TRNS_BLK_CNT_EN`。** `sdhci-msm` 不设 `SDHCI_QUIRK2_SUPPORT_SINGLE`，
   所以 Linux 对单块传输也总是置块计数使能（`sdhci.c:1483`，`ds-sdhci.c:1173`）。mode 10
   只写了 `TRANSFER_MODE=0x10`；MSM 的数据机可能根本没有“无块计数”的单块模式。
2. **地址差 512 倍。** OCR `c0ff8080` 的 bit 30 是 CCS（高容量/扇区寻址）。Linux 在
   `ds-mmc.c:2092` 看到它就 `mmc_card_set_blockaddr`，块层对这类卡**不左移 9 位**
   （`ds-block.c:1661`）。mode 10 按字节寻址传了 512，实际问的是 LBA 512。

另外把收尾的协议查清了：数据没结束时，Linux 用 CMD12 停传输、用 R1（`0x1a`，不是 R1B）解析
应答，并且**只有停止命令可以越过 DATA_INHIBIT**（`sdhci.c:1675`）；CMD13 只走命令线，同样
不必等数据机。

## v0.2.69（round97）：补上块计数使能与扇区寻址，并把数据阶段逐步记下来

mode 10 的路由不变（CMD2 关 CRC → CMD3 RCA 1 → CMD7 → CMD13 要求 `st=4`），改的是数据：

1. **CMD16** 照旧 512，`c16` 仍单独报 verdict/INT_STATUS/R1。
2. **读参数由这张卡自己的 OCR 决定**：`rsp10 & (1<<30)` 为真时 `ARGUMENT=1`（扇区号），
   否则 `ARGUMENT=512`（字节）。报告里的 `ar=` 就是实际写进去的值。
3. **`TRANSFER_MODE = READ | BLK_CNT_EN`**（`0x12`），`BLOCK_COUNT=1`。
4. **FIFO 按 PRESENT_STATE 的实时电平排空**，不再按 INT_STATUS 的 DATA_AVAIL 事件：
   后者是写 1 清除的粘滞位，round96 在两字已读完之后仍看到它。排空循环以 DATA_END 或数据错误
   为出口，整体有 `1<<22` 上限；报告新增 `nw=`（实际取到的字数）与 `sm=`（它们的 32 位和），
   “满块零”和“一个字节都没送”从此分得开。
5. **DATA_END 没来就问卡**：CMD12（`R1`，`0x1a`）越过 DATA_INHIBIT 停下，再 CMD13 读状态，
   两行分别是 `c12` 与 `c13b`。这样即使这一轮仍然没有数据，也能看到卡是被停住的、还是本来就
   不在 data 状态。

报告多了五行，所以 mode 11 的表头省掉 `vs`/`vs3` 两个自 round92 起没变过的 vendor 选择子
（`clk=`/`rst=`/`clr=` 仍能回答时钟问题），最坏约 416 字节。

```text
g cfg=00002017 m=01 n=fc d=fb up=1 br=00004221 ab=20008001
h clk=0007 rst=1 clr=0002 pw=0b
c0 ok c1 ok t=3 to=0 rsp=c0ff8080
c2 ok int=00000001
c3 ok st=2
c7 ok int=00000001
c13 ok st=4
c16 ok int=00000001 r=00000900
c17 ok int=00000003 ar=1 tm=12
ev nc=3 ia=00000001 ns=8 nw=128
ps a=01f80000 e=01f80af6
hdr 20494645 54524150 sm=00000000
efi 1
```

（示意值；`efi 1` 当且仅当排空到的头两个字是 `0x20494645 0x54524150`。）

判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `c17 ok` + `efi 1` | 数据通路工作、扇区寻址正确、LBA 1 真是 GPT 头 | 读完整 GPT 头与分区表，找目标分区 |
| `c17 ok` + `efi 0` + `nw=128` + `sm` 非零 | 传输完成但字节不同：地址单位或字序 | 用 `hdr` 对照 LBA 0/1 与字节交换 |
| `c17 ok` + `nw=128` + `sm=00000000` | 块到了但全零 | 换一个已知非零的 LBA（如 GPT 备份头在最后） |
| `c17 nodata` + `nw=0` | 卡仍然一个字节都不送 | 看 `c12`/`c13b`：卡是否在 data 状态、CMD12 是否被接受 |
| `c17 nodata` + `nw>0` | 送了一部分就停 | `ns=` 是数据机轮询次数；查 `TIMEOUT_CONTROL` 与块长 |
| `c17 dto` / `dcrc` / `dend` | 数据阶段自己报错 | 400 kHz 下不像信号问题，先查总线宽度/电源 |
| `c17` = `to`/`crc`/`idx` | 卡拒绝读命令 | 看 `c16` 与 `c13`，可能需要先修 SET_BLOCKLEN |
| `c12`/`c13b` 也 `busy` | 数据机占住命令槽，复位也不自清 | 下一轮先 `RESET_CMD|RESET_DATA` 再重问 |
| `c2`…`c13` 任何一步退步 | bring-up 退化 | 对照 round95/96 的表头与 `c0`/`c1` |

产物（内核变了；root / trust cache / boot 镜像仍未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round97-payload.img` | 58720256 B | `7421daa6c142a234ddc87eb1547ae059d58a13219121d2e36867eeb00842df86` |
| `round97-kernel.macho`（构建产物） | 51949568 B | `c2258d4c27a9a781feaaa97b246438379c8592cf09609b3bd373f951e74397a6` |
| `round97-in-volume-kernel.macho`（卷内副本） | 51949568 B | `efaff4b2a84dd7490319ec1ce209f31e3e3aa9a466bb096202e526794e24527e` |
| `round97-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round97-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round97-kernel.macho.adt` | 13476 B | `cc440e68f47903c7df9fe2d195012ed98c392b507a54ace922c1945b1d88c75d` |
| `round97-unixnu.toml` | 554 B | `8dcf7bb48644fdea7129a7c5188539e7dfca0b0462051a992806ec14b4d64cd7` |

校验器：探针 35 处 5960 字节、PMGR 5 条绑定 0 位移、CloverSdhci 六条串全在。反汇编
`__ZL11coldBringUpbj` 里有一条 `#0x12`（`TRANSFER_MODE = READ|BLK_CNT_EN`）紧跟 `#0x113a`
（CMD17），两条 `#0x101a`（CMD16）、一条 `#0xc1a`（CMD12）与三条 `#0xd1a`（CMD13，`0x1a`
即不带 BUSY 的 R1）；OCR 判断是 `tst w9, #0x40000000` + `csinc w12, w9, wzr, eq`，位 30
置位时参数为 1、否则 512。报告字面量这次被编译器直接折成立即数写进缓冲区（函数变大后
`emitText` 被内联，`__cstring` 里因此找不到 `c17`；写 `gReportLength` 与
`clover_emmc_report` 的 `strb` 序列在反汇编尾部可见）。卷内 boot-args 末尾是
`… clover-pmgr=2 clover-emmc=11 armcpu-clock-panic=60000`。boot 仍是
`round87-boot.img`（`196b284f…`）：

```sh
fastboot flash cache ~/clover-out/round97-payload.img
fastboot reboot
```

**拍一张就够。**

## round97 读数：卡把整块送完了，是主机读得太快

照片（`sips -r -90` 之后正是正向；末尾 `@CloverSdhci.cpp:2476` 是 panic 调用点）：

```text
g cfg=00002017 m=01 n=fc d=fb up=1 br=00004221 ab=20000001
h clk=0007 rst=1 clr=0002 pw=0b
c0 ok cl ok t=3 to=0 rsp=c0ff8080
c2 ok int=00000001
c3 ok st=2
c7 ok int=00000001
c13 ok st=4
c16 ok int=00000001 r=00000900
c17 nodata int=00000021 ar=1 tm=12
ev nc=517 ia=00000001 ns=4194304 nw=128
ps a=01f802f6 e=01f80af6
hdr 20494645 20494645 sm=24a32280
efi 0
c12 to st=4 int=00018000
c13b ok st=4 r=00400900
```

三件事同时被这张照片钉住：

1. **地址和块计数是对的。** `ar=1`、`tm=12`，FIFO 第一字是 `0x20494645`，小端即 `"EFI "`；
   LBA 1 的头五个字节正是 GPT 签名。
2. **128 次读回的是同一个字。** `sm = 0x24a32280 = 128 × 0x20494645`，一字节不差；
   `hdr` 前后两字相同也是同一事实。卡没有送 128 遍，是主机在卡送到第一个字之后
   又读了 127 次空端口。
3. **卡把整块送完并且回到了 idle。** `c13b ok st=4 r=00400900`：state 4 (tran) +
   `READY_FOR_DATA`（`0x100`）。`c12 to` 的 `int=00018000` 是主机的
   ERROR|TIMEOUT：命令阶段 200 ms 内没等到 CC，不是卡的问题。

`ps a=01f802f6` 是 CMD17 刚发完时读的：bit11（BUFFER_READ_ENABLE）= 0，DOING_READ=1，
DAT_ACTIVE=1 ⇒ 那一刻还没有一个字到达。`ps e=01f80af6` 的 bit11 = 1，是末尾电平仍高
（最后那次读已经把缓冲清空，电平位是上次事件留下的），所以「按电平排空」在 400 kHz 上
等于「在头几微秒里把 128 次读全花掉」。

算一下：400 kHz 单线，512 字节 = 4096 bit = 10.24 ms，平均每个 32 位字约 80 µs；
而 `ns=4194304` 说明排空循环在几个毫秒内就跑满了上限。读得比卡快不会丢数据——
缓冲满了会反压卡的时钟——**所以修法不是等事件，而是读得慢一点**。

## v0.2.70（round98）：按卡的节奏排空 FIFO

mode 12 = mode 11 的整条链，只换排空策略，并补三组读数：

1. **用 `CNTVCT_EL0` 给每次 FIFO 读定节奏**：`gap = CNTFRQ_EL0 / 6250`（19.2 MHz 下
   160 µs，是 400 kHz 下每字 80 µs 的两倍），整体期限 `CNTFRQ_EL0 / 5`（200 ms）。
   读数侧先看 `DATA_END`/数据错误，再看期限，最后才在 `PRESENT_STATE` bit11 为真且
   **距上次读已过一个 gap** 时读一个字。
2. **报告 `nd=`**：逐字与上一字比较、不同的次数。一个字后面跟 127 个重复读成 `nd=0`，
   真的一整块读成 `nd=127`（第一字没有前驱，所以是 127 而不是 128）。
3. **报告 `dt f=/l=`**：第一字与最后一字落在块内第几微秒，直接量出这张卡的取数速度；
   `hc`/`HC2`、末尾 `CLOCK_CONTROL` 也照原样印出来，用来排除「读的过程中总线被降宽/停钟」。
4. **解 GPT 头**：`gpt lba=`（当前 LBA，偏移 24）、`el=`（分区表 LBA，偏移 72）、
   `en=`（表项数，偏移 80）、`es=`（表项大小，偏移 84），各取一个小端 32 位字。

`efi 1` 仍要求头两字是 `"EFI PART"`。报告比 mode 11 多两行，最坏约 400 字节。

判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `c17 ok` + `efi 1` + `nd=127` | 数据通路通，GPT 头在屏上 | 读分区表项，找可写的日志分区 |
| `c17 ok` + `efi 1` + `nd<127` | 头对了，后半段有重复或跳过 | 看 `dt l=` 与 `ps e=` 判断节奏是否仍然太快 |
| `c17 ok` + `efi 0` + `nd=127` | 整块到了，但内容不是 GPT 头 | 用 `gpt` 四字段与 `hdr` 校准地址/字节序 |
| `c17 nodata` + `nd=0` + `nw=128` | 卡仍旧只给一个字 | 数据机在等一个我们没给的条件：查 `hc`/`HC2`、块长、`TIMEOUT_CONTROL` |
| `nw<128` | 200 ms 期限先到 | `dt f=` 说明第一个字什么时候来；`ns=` 说明循环跑了几次 |
| `dt f=` 很小、`dt l=` 接近 10 ms | 节奏正确，按理论速度在走 | 可以进入分区表读取 |
| `c17 dto`/`dcrc`/`dend` | 数据阶段自己报错 | 仍先查总线宽度与电源 |
| `c12`/`c13b` 变化 | 数据机不再卡住 | 对照 round97 的 `st=4`/`READY_FOR_DATA` |

产物（内核变了；root / trust cache / boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round98-payload.img` | 58720256 B | `4fd012a054ac2ebd77cddf46eb6db5f5e9e154da807d5669d452fecb42349197` |
| `round98-kernel.macho`（构建产物） | 51949568 B | `2f9e2b9d0bcc36203a33bbf2bbbe76f0e6ec927f7c29ada3c491117d6d5bc590` |
| `round98-in-volume-kernel.macho`（卷内副本） | 51949568 B | `6f51135a0adbfa3f17f7be6e96b744e70ebd22a59ed1aa5e239120fe10d6e00e` |
| `round98-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round98-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round98-kernel.macho.adt` | 13476 B | `d920d0eca4597abffbf7a5c253c1c6a38aa47bae0aa951018f8badff324041d5` |
| `round98-unixnu.toml` | 561 B | `2ecd39111f54668be1a886282e6e9f1d03b3a7e59cc638149ee8ea26c285ca70` |

校验器：探针 35 处 5959 字节、PMGR 5 条绑定 0 位移、CloverSdhci 六条串全在。反汇编
`__ZL11coldBringUpbj`：`mrs x8, CNTFRQ_EL0`（对象 `0x192f4`）、两处 `mrs x2, CNTVCT_EL0`；
`freq/6250` 走 `umulh 0x29f16b11c6d1e109, lsr #9`，`freq/5` 走 `umulh 0xcccccccccccccccd, lsr #2`；
读字路径在 `0x195e0`（对象）读 `[host,#0x20]`，`ccmp`+`cset ne` 累加 `nd`，`mul 1000000`
后 `udiv` 出微秒、`csel` 保住第一次的 `firstUs`。卷内核里
`\ngpt lba=` 的逐字符 `strb` 序列后面紧跟 `bl emitDec`，四个字段偏移对得上。
卷内 boot-args 末尾是 `… clover-pmgr=2 clover-emmc=12 armcpu-clock-panic=60000`。
boot 仍是 `round87-boot.img`（`196b284f…`）：

```sh
fastboot flash cache ~/clover-out/round98-payload.img
fastboot reboot
```

**拍一张就够。**

## round98 读数：节奏对了，FIFO 端口没有前进

照片（正向；末尾是 panic 调用点）：

```text
g cfg=00002017 m=01 n=fc d=fb up=1 br=00004221 ab=20008001
h clk=0007 rst=1 clr=0002 pw=0b
c0 ok c1 ok t=3 to=0 rsp=c0ff8080
c2 ok int=00000001
c3 ok st=2
c7 ok int=00000001
c13 ok st=4
c16 ok int=00000001 r=00000900
c17 nodata int=00000021 ar=1 tm=12
hc 00 0000
dt f=10415 l=30747 nw=128 nd=0
ps a=01f802f6 e=01f80af6 clk=0007
hdr 20494645 20494645 sm=24a32280
gpt lba=541673029 el=541673029 en=541673029 es=541673029
efi 0
c12 to st=4 int=00018000
```

这张照片把 round97 的「读得太快」正式结掉，并把嫌疑锁到一个寄存器位上：

1. **节奏完全正确。** 第一字落在 `f=10415` µs：400 kHz、1-bit 总线、512 字节
   = 4096 bit ≈ 10.24 ms。最后一字 `l=30747`，正好是 `f + 128 × 160 µs`。
   也就是说排空循环一直在按卡的节奏走，没有抢跑。
2. **128 次读仍是同一个字，而且现在能证明是端口问题。** `nd=0`、`sm` 仍是
   128 × `0x20494645`，`ps e` 的 bit11（buffer read enable）到最后一刻还是 1。
   如果 SDHCI 缓冲在消费数据，读第二次电平就该变；它没变 ⇒ **每次读都落在同一条
   不前进的端口上**，而不是「卡只送了一个字」。
3. **`hc 00 0000` 是新的读数。** 这次 `RESET_ALL` 之后驱动只恢复了时钟和电源，
   `HOST_CONTROL` 是 0x00（1-bit 总线、DMA 选 SDMA），`HC2` 是 0。PIO 不看 DMA
   选择位，1-bit 也与 `dt f` 的理论值吻合，所以**不是**这轮的嫌疑；它只是说明
   round96/97 提到的「8-bit 总线」是固件留下的读数，不是我们设的。
4. **GPT 四字段全是同一个数** `541673029 = 0x20494645`，即 `"EFI "` 被当作
   LBA/表项数读了出来；`efi 0` 同理。地址没错，错的是块内容只有第一个字。
5. `c12 to st=4 int=00018000` 与 round97 逐字相同：卡早已回到 transfer，
   CMD12 只是没等到 CC。

那么「端口为什么不前进」。round87 读到的 `vs3=02626440` 里 bit 10 是 1 ——
这是 `CORE_FIFO_ALT_EN`（`sdhci-msm.c:144`），Linux 在 `sdhci_msm_init` 里专门清它，
注释是 “Ensure SDHCI FIFO is enabled by disabling alternative FIFO”（`:5586`）。
固件把 eMMC 交给了 Qualcomm 的备用 FIFO，我们的 PIO 读的是 SDHCI 的 `0x20`；
电平位来自数据机，读窗口却通向另一条没被喂的队列——这正好解释「指针不动、
电平不落、每读一次都是同一个字」。

## v0.2.71（round99）：把 FIFO 从备用路径收回 SDHCI

mode 13 = mode 12 原样，只加一次写：

1. 在 `RESET_ALL` 之后、卡时钟恢复之后、任何传输之前，读 `VENDOR_SPEC3`（0x250），
   清 bit 10，再读回。这一步只做一次，没有命令在飞、数据机已复位。
2. 报告把 mode 12 的 `hc <HOST_CONTROL> <HC2>` 换成 `v3 <before> <after>`（各取低
   16 位），用来分开三种情况：位原本就没置、写进不去、写进去了但 FIFO 还是不动。
3. 其余一个字节不改：同一条识别链、同一个 `TRANSFER_MODE=0x12`、`ar=1`、同样的
   160 µs 节奏与 200 ms 期限、同样的 `dt`/`ps`/`hdr`/`gpt`/`efi`。

报告约 480 字节，仍在 512 字节缓冲内（`hc` 占 11 字节，`v3` 占 13 字节）。

判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `v3` 前一个数 bit10=1、后一个 bit10=0，且 `nd>0` | 备用 FIFO 就是元凶；端口恢复前进 | 看 `efi 1`，读 GPT 分区表项 |
| 同上但 `nd` 仍为 0 | 位清了，端口还是不动 | 下一轮换 ADMA2/ADMA64（固件用的是 ADMA64，`hcl=38`） |
| `v3` 前一个数 bit10=0 | 固件这次没开备用 FIFO，假设出局 | 直接查数据机的 `TIMEOUT_CONTROL`/块长/总线宽度 |
| 后一个数 bit10 仍为 1 | 写没落下去 | 查该寄存器的时钟/电源门控 |
| `v3 0000 0000` | 冷启动没走到这一步（读回 0） | 先解决 bring-up，不看 FIFO |

产物（内核变了；root / trust cache / boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round99-payload.img` | 58720256 B | `3b0c5e6c5ca7fd6d68b31db9e7f118629cd5aa6956389ee171538dd4bd6f7b06` |
| `round99-kernel.macho`（构建产物） | 51949568 B | `0141659d0537d69b8a9038e098ad340e755e6f3a9c984289bcb6ab2812418574` |
| `round99-in-volume-kernel.macho`（卷内副本） | 51949568 B | `1eccfbc44315571e1b2b0f13d2dc0f242e5841406952dc9ef0ccbff21ef04760` |
| `round99-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round99-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round99-kernel.macho.adt` | 13476 B | `7e9427f8c50d55f8503e6d26275ae41e9770bf1b9868a891fc9834d67fbee778` |
| `round99-unixnu.toml` | 581 B | `063bbd45d37aba28b4cc2864a3a02aaaa63bd36b5e94039285d5c14d64dd81a4` |

校验器：探针 35 处 5960 字节、PMGR 5 条绑定 0 位移、CloverSdhci 六条串全在。反汇编
`__ZL11coldBringUpbj`：mode 13 的门是 `cmp w1,#0xd; b.lo`（对象 `0xa50c`），随后
`ldr w9,[x9,#0x250]`、`and w10,w9,#0xfffffbff`、`str w10,[x11,#0x250]`、再读一次；
报告侧 `cmp w19,#0xd; b.lo` 分去 mode 12 的 `hc`，否则逐字符写 `\n` `v` `3` 空格
（对象 `0x1256c`–`0x125c8`），两次 `emitHex(…,4)` 中间隔一个空格。卷内 boot-args
末尾是 `… clover-pmgr=2 clover-emmc=13 armcpu-clock-panic=60000`。
boot 仍是 `round87-boot.img`（`196b284f…`）：

```sh
fastboot flash cache ~/clover-out/round99-payload.img
fastboot reboot
```

**拍一张就够。**

## round99 读数：清掉备用 FIFO 之后，整块 LBA 1 一次到齐

照片（相机转了 90°，`sips -r -90` 之后读到的就是下面这个顺序；末尾
`@CloverSdhci.cpp:2667` 是 panic 调用点）：

```text
g cfg=00002017 m=01 n=fc d=fb up=1 br=00004221 ab=20008001
h clk=0007 rst=1 clr=0002 pw=0b
c0 ok c1 ok t=3 to=0 rsp=c0ff8080
c2 ok int=00000001
c3 ok st=2
c7 ok int=00000001
c13 ok st=4
c16 ok int=00000001 r=00000900
c17 ok int=00000003 ar=1 tm=12
v3 6440 6040
dt f=10416 l=30748 nw=128 nd=23
ps a=01f802f6 e=01f800f0 clk=0007
hdr 20494645 54524150 sm=e853917c
gpt lba=1 el=2 en=64 es=128
efi 1
```

这一张把三件事同时钉住：

1. **备用 FIFO 选择位就是 round98 的堵点。** `v3 6440 6040`：写前低 16 位是
   `0x6440`（bit 10 = `CORE_FIFO_ALT_EN` 置位，和 round87 读到的 `02626440` 的低半
   一致），写后是 `0x6040`，位清掉、别的一个都没动。写进去之后端口立刻开始前进。
2. **数据阶段自己走完了。** `c17 ok int=00000003` = RESPONSE|DATA_END，所以这次没有
   `c12`/`c13b`（那条只在 DATA_END 不来时才发）。`dt f=10416` 是第一个字到达的时刻，
   和 400 kHz 单线下 512 字节的 10.24 ms 对得上；`dt l=30748` 约等于 128 × 160 µs 加
   10 ms，读数侧没有比卡快。`nd=23`：92 字节的 GPT 头后面全是零，正好在这个形状里。
3. **主机认得出这张表。** `hdr 20494645 54524150` = `"EFI PART"`，`gpt lba=1 el=2
   en=64 es=128`：当前 LBA 1、表项数组从 LBA 2 开始、64 项、每项 128 字节。
   `efi 1`。`ps a=01f802f6` 的 bit11 在命令刚发完时还是 0（缓冲区空），
   `ps e=01f800f0` 是整块读完之后的数据机状态，`clk=0007` 说明读的过程中卡钟没停。

到此为止 eMMC 的读通路不再有未知项。下一件事不是再证明它能读，而是用它读一张
有名字的表：GPT 表项数组里应该有一个 `logfs`，固件把它叫作 `LOGFS:`，Linux 把它
暴露成 `mmcblk0p39`（sysfs：`start=12320768`、`size=16384` 个 512 字节扇区，
即首 LBA 12320768、末 LBA 12337151）。

## v0.2.72（round100）：读 GPT 表项数组，按名字找 logfs

mode 14 = mode 13 的整条链，在 LBA 1 读完之后多走一段：

1. 用刚读到的头字段决定表项在哪里：`el`（表项数组首 LBA，2）、`en`（项数，64）、
   `es`（每项字节数，128）。每个 512 字节块装 `512/es` 项，一共要读
   `ceil(en/(512/es))` 块；上限 16 块，正好覆盖 64 项 × 128 字节。
2. 每块都走同一套 PIO：CMD17、`TRANSFER_MODE=0x12`、`CNTVCT_EL0` 的 160 µs 节奏、
   每块 200 ms 期限、DATA_END 之后再按电平位补排空。这条路径就是 round99 证明过的那条，
   只是循环了十次。第一块仍然是 LBA 1 自己的内联路径，没有改成通用函数。
3. 对每一项，名字在偏移 56 的 UTF-16LE：`"logfs"` 加结尾零正好是 128 字节表项里的
   word 14/15/16（`0x6f6c`、`0x6667`、`0x0073`）。连结尾零一起比，
   `logfsx` 这类名字不会误中。
4. 报告换成扫描自己的读数：
   - `sc el= en= es= b= ok=`：表项数组的位置、项数、项大小、实际读了几块、每块是否
     都干净地 DATA_END 且收满 128 字。头部字段不可用时另有 `hd=1`；某块失败时补
     `st=<INT_STATUS> w=<该块收到几个字>`。
   - `lg i= a= b=`：命中的 GPT 序号（1 起，0 = 没找到）、该项的首 LBA 与末 LBA。
   - `ty <word0> <fields 2-3>`：类型 GUID 的第一个 32 位字与第二、三字段的合并值，
     用来确认它是不是 Linux 文件系统类型（`0FC63DAF-8483-4772-8E79-3D69D8477DE4`
     在盘上是 word0 = `0fc63daf`、word1 = `47728483`，合并后的第二段是
     `84834772` = data2<<16 | data3）。
   - `ps e=` 与 `clk=` 保留，`efi` 保留为 LBA 1 的回归；`dt`/`ps a`/`hdr`/`gpt lba`
     在 mode 14 里省掉，`gpt` 的三个字段已经在 `sc` 里。
5. `v3` 仍然打印，清位这一步在 mode 14 也照做。

报告最坏约 440 字节，仍在 511 字节以内（`emitByte` 在上限处会丢字符，所以留了余量）。

判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `sc … ok=1` + `lg i=39 a=12320768 b=12337151` | 名字找到了，范围与 sysfs 独立吻合 | 往 logfs 首块写一段日志再读回 |
| `sc … ok=1` + `lg i=0` | 十块都读干净，但没有这一项叫 `logfs` | 先按 `ty` 与相邻项的名字校准，再决定写哪里 |
| `lg i=` 不是 39 但 `a=12320768` | 名字对、编号假设错（GPT 有空项） | 以 `a/b` 为准，不动 Linux 的编号 |
| `sc … ok=0 st=… w=…` | 某一块没读完，`st`/`w` 指到那一块 | 对照 round99 的 `int=00000003`，查块号与数据机 |
| `sc … hd=1` | 头字段不是 2/64/128，LBA 1 的回归坏了 | 回到 mode 13 看 `gpt`/`efi` |
| `sc … b=` 小于 10 且 `i=0` | 扫描被 16 块上限或 `en` 截断 | 用 `el/en/es` 重算块数 |
| `efi 0` | LBA 1 不再是 GPT 头 | 先修 round99 这条读通路 |

产物（内核变了；root / trust cache / boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round100-payload.img` | 58720256 B | `9f490ddf540e3ae86a2ad1624345abff0f5da75e04ed5ec742035e2dbb8c7cd4` |
| `round100-kernel.macho`（构建产物） | 51949568 B | `61f47bb47f9b9cc901b40951688cd49906fc0f071f62b722e52bfef4759a47b7` |
| `round100-in-volume-kernel.macho`（卷内副本） | 51949568 B | `aae92753b50c87a10ef296a090e56695f32914fd9da02028f88d578b78d52ae6` |
| `round100-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round100-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round100-kernel.macho.adt` | 13476 B | `787e27233760b4a1884b9e4e75a2eda8fc96f46474139806fff5b92f6af7a05f` |
| `round100-unixnu.toml` | 566 B | `24b620712f7ee18ce996bad9cbf54f622077d82adfce9eb479349f1541a25cae` |

校验器：探针 35 处 5960 字节、PMGR 5 条绑定 0 位移、CloverSdhci 六条串全在。
反汇编 `__ZL11coldBringUpbj`：mode 13/14 共用的清位门仍是 `cmp w1,#0xd; b.lo`
（对象 `0xa50c`），随后 `and w10,w9,#0xfffffbff`、`str w10,[x11,#0x250]`；
扫描循环在对象 `0x1ab3c` 调 `__ZL12readPioBlockjPjRjS0_`（内核里
`0xfffffff00798ccdc`），名字比较的三条立即数是 `0x6f6c`/`0x6667`/`0x73`；
报告侧按顺序能找到逐字符写出的 `\nsc el=`、`\nlg i=`、`\nty `、`\nps e=`、`\nefi `。
`readPioBlock` 用 `CNTFRQ/6250`（160 µs）与 `CNTFRQ/5`（200 ms），结尾再按
`PRESENT_STATE[11]` 补满，和 mode 12 的排空策略一致。卷内 boot-args 末尾是
`… clover-pmgr=2 clover-emmc=14 armcpu-clock-panic=60000`。
boot 仍是 `round87-boot.img`（`196b284f…`）：

```sh
fastboot flash cache ~/clover-out/round100-payload.img
fastboot reboot
```

**拍一张就够。**

## round100 读数：十六块都读完了，名字一个也没对上

照片：

```text
sc el=2 en=64 es=128 b=16 ok=1
lg i=0
ps e=01f800f0 clk=0007
efi 1
@CloverSdhci.cpp:2850
```

这张把「读」和「比」干净地切开了：

1. **十六块全干净。** `b=16 ok=1`：`el=2 en=64 es=128` 算出来正好 16 块，每块都
   DATA_END 且收满 128 字，没有 `hd=1`、没有 `st=`/`w=`。表项数组从 LBA 2 起
   8192 字节整段读出来了。round99 证明过的单块通路，循环十几次也没退化。
2. **LBA 1 的回归还在。** `efi 1`、`ps e=01f800f0 clk=0007`，和 round99 一模一样。
3. **但 `lg i=0`：没有任何一项的名字等于 `logfs`。** 读侧已经无可怀疑，问题只能在
   比较这一步。

诊断很直接：GPT 的名字是 **UTF-16LE**，`"logfs"` 在盘上是
`6c 00 6f 00 67 00 66 00 73 00 00 00`，装进表项偏移 56 起的 32 位字就是
`0x006f006c`、`0x00660067`、`0x00000073`。mode 14 比的是
`0x6f6c`、`0x6667`、`0x0073`——把每个字高半边的零字节都挤掉了。第三个字
（`0x0073` 对 `0x00000073`）碰巧数值相同，前两个永远不可能相等。所以这是一处常量
错误，不是读错；十六块读得再好也命中不了。

## v0.2.73（round101）：按 UTF-16 的整字比名字，并加一行首项标定

mode 15 = mode 14 的整条链，只动两处：

1. **名字按整字比。** `entry[14] == 0x006f006c`、`entry[15] == 0x00660067`、
   `entry[16] == 0x00000073`。结尾零仍然一起比，`logfsx` 这类名字不会误中。
2. **加一行标定，回答「表项数组到底读对没有」。** 当没找到 `logfs` 时，报告打出
   第一个非空表项（`type GUID` 四个字不全为零）的原始读数：
   - `e0 i=`：它的 GPT 序号（1 起）；
   - `w=` 三个字：表项偏移 56、60、64 处的原始 32 位字；
   - `a=`：偏移 32 处的首 LBA。

   这块板上第一项是 `xbl`（LBA 131072），名字 `"xbl"` 是
   `78 00 62 00 6c 00`，所以预期读数是
   `e0 i=1 w=00620078 0000006c 00000000 a=131072`。只要这行对上，就说明
   ① 表项数组确实从 `el` 指向的地方读起，② 名字确实是 UTF-16LE 整字存放，
   ③ 那么 `lg i=0` 就是「这 64 项里真没有叫 logfs 的」，而不是编码问题。

报告最坏约 470 字节（`lg` 与 `e0` 互斥，不会同时出现），仍在 511 字节以内。
`v3`、`sc`、`ps e=`、`clk=`、`efi` 都保留。boot 参数末尾是 `clover-emmc=15`，
panic 调用点现在是 `@CloverSdhci.cpp:2881`。

判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `lg i=39 a=12320768 b=12337151` + `ty 0fc63daf 84834772` | 名字找到，范围与 sysfs 独立吻合，类型是 Linux 文件系统 | 往 logfs 首块写一段日志再读回 |
| `lg i=39 …` 但 `ty` 不是 `0fc63daf` | 名字对上了，类型是别的 GUID | 记下 `ty`，不影响写日志（按名字定位即可） |
| `lg i=0` + `e0 i=1 w=00620078 0000006c 00000000 a=131072` | 表项数组读对了、编码也对了，64 项里确实没有 `logfs` | 改扫全部 64 项之外的线索：查名字是否带尾随空格，或 `logfs` 不在 GPT 里 |
| `lg i=0` + `e0 i=1` 但 `w=` 与预期不同 | 表项数组位置或字偏移不对 | 用实际 `w=` 反推读到的偏移，回头查 `el` |
| `lg i=0` 且没有 `e0` 行 | 第一项就是全零（表项数组处不是有效数据） | 检查 `sc el=` 与 `b=`，回到 mode 13 看 `gpt` |
| `sc … ok=0 st=… w=…` | 某一块没读完，`st`/`w` 指向那一块 | 对照 round99 的 `int=00000003`，查块号与数据机 |
| `efi 0` | LBA 1 不再是 GPT 头 | 先修读通路 |

产物（内核变了；root / trust cache / boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round101-payload.img` | 58720256 B | `c6af5e3c6d161aacede5f18b92c620a0fc48acad73a61c95c19b7516beb20433` |
| `round101-kernel.macho`（构建产物） | 51949568 B | `3358dcd895f188c1b748f760bc3e3fc681325d6d00d3cc7fb729934de294f39c` |
| `round101-in-volume-kernel.macho`（卷内副本） | 51949568 B | `5e5c42d45980154a67362b76c276a60bf80b53830295f4c897e82ffa2faf1c7b` |
| `round101-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round101-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round101-kernel.macho.adt` | 13476 B | `f5816b7cd7a3f2e310592f6a8c6acbfde43ceb55ad8fb56f962450969cbd8aa8` |
| `round101-unixnu.toml` | 573 B | `31f63cda33ae113e28f8cdd9439f1cccadf4b0b9c34c87b6dbd1c81d0547ffc2` |

校验器：探针 35 处 5960 字节、PMGR 5 条绑定 0 位移、CloverSdhci 六条串全在。
反汇编 `__ZL11coldBringUpbj`：名字比较现在是
`mov w13,#0x6c; movk w13,#0x6f,lsl #16`（对 `entry[14]`）、
`mov w13,#0x67; movk w13,#0x66,lsl #16`（对 `entry[15]`）、
`cmp w12,#0x73`（对 `entry[16]`），在对象里从 `0x1ae7c` 起；标定行的逐字符发射
`\ne0 i=` 在对象 `0x14a2c`，后面紧跟 `emitDec`、` w=` + 三次 `emitHex`、` a=` +
`emitDec`。扫描循环仍调 `readPioBlock`（内核 `0xfffffff00798cffc`）。分发器
`mode >= 7 && mode <= 15`，门里加了 `mode != 15`。卷内 boot-args 末尾是
`… clover-pmgr=2 clover-emmc=15 armcpu-clock-panic=60000`。
boot 仍是 `round87-boot.img`（`196b284f…`）：

```sh
fastboot flash cache ~/clover-out/round101-payload.img
fastboot reboot
```

**拍一张就够。**

## round101 读数：logfs 就在第 39 项，首末 LBA 与 sysfs 独立吻合

照片里转述来的这一行：

```text
lg i=39 a=12320768 b=12337151
```

这一行同时钉住三件事：

1. **名字按整字比之后命中了。** round100 的 `lg i=0` 确实是常量把高半字节挤掉，
   不是读错；改回 UTF-16LE 的三个整字后，同一段表项数组立刻对上了。
2. **序号与 Linux 的编号一致。** `i=39` 就是 `mmcblk0p39`，说明这台设备的 GPT 里
   没有空项把编号顶偏。
3. **范围是独立吻合的。** `a=12320768`、`b=12337151` 与 sysfs 的
   `start=12320768`、`size=16384` 个 512 字节扇区（末 LBA 12337151）一字不差。
   读到的 GPT 和内核眼中的分区表是同一张。

读通路和定位这一步到此结束。logfs 的坐标不再需要推导：接下来的写实验直接用它。

## v0.2.74（round102）：往 logfs 首块写一块再读回

mode 16 = mode 15 的整条链，在扫描命中之后多走一段。这一段就是整条 eMMC 链一直
在准备的那件事：**卡能不能写**。

1. **先按坐标设门。** 只有 `a=12320768` 且 `b=12337151` 才继续；坐标不对就只打
   `wr g=1`，一个字也不写。这样即使解析出错，写也落不到 logfs 外面。编译后调用点
   的参数被窄化成 `w0=0xbc0000`（= 12320768），这本身也是一道静态校验。
2. **先读一遍旧内容。** 用 mode 15 证明过的 `readPioBlock` 读首块，记下 128 字的
   和与第一个字（`old <sum> <w0>`）。没有这一步，读回不一致时分不清「原来就有别的
   东西」和「读坏了」。
3. **造一块能自证的载荷。** 128 字先填 `0x554e5855 + i×0x01000193`，再覆盖头部：
   `w0 = 554e5855`（内存序的 `"UXNU"`）、`w1 = 首 LBA`、`w2 = 16`（mode 号）、
   `w3 = 第 4..127 字的和`。既不是全零也不是全一，写错了或没写都能从读回里看出来。
4. **CMD24 写入。** `writePioBlock` 与读路径同形：`TIMEOUT 0x0e`、块大小 512、
   块计数 1、`TRANSFER_MODE = BLK_CNT_EN`（READ 位为 0 = 主机到卡）、
   `COMMAND = 0x183a`（CMD24 + R1 + CRC + INDEX + DATA）。FIFO 侧仍然用
   `CNTFRQ/6250`（160 µs）的节奏、200 ms 期限，门控位换成 `SPACE_AVAILABLE`；
   结尾再按电平补排空。比卡写得慢是安全的：缓冲区空时 SDHCI 会停卡钟。
5. **等卡编程完。** 写完立刻打 CMD13，直到 R1 的 `READY_FOR_DATA`（bit 8）置位；
   最多 100 次，`p=` 就是实际问了几次。卡一直不 ready 本身也是读数。
6. **读回逐字比。** 再走一次 `readPioBlock`，数出 128 字里有多少字与载荷相同
   （`cmp=`），并记下读回的和（`sum=`）。最后 `w0` 打读回的头三个字，用来直接
   认出 `554e5855` / `00bc0000` / 校验和。

报告在 mode 16 里省掉 `sc` 与 `ty`（坐标已知），保留 `lg`、`ps e=`、`clk=`、`efi`：

```text
lg i=39 a=12320768 b=12337151
wr a=12320768 n=128 ok=1 int=00000003
st 00000900 s=4 p=1
old 00000000 00000000
rd ok=1 cmp=128 sum=...
w0 554e5855 00bc0000 ...
ps e=01f800f0 clk=0007
efi 1
```

判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `wr … n=128 ok=1` + `rd ok=1 cmp=128` + `w0 554e5855 00bc0000` | 写、编程、读回整条链路都对 | 把真正的日志文本按同一路径写进 logfs，再读回解码 |
| `wr g=1` | 扫描给的坐标不是 12320768/12337151，门主动拦住 | 回到 mode 15 看 `lg`，先修定位再放开 |
| `wr … ok=0 int=…` | 写命令本身没干净结束，`int` 指到哪一段 | 按 `int` 的位查命令/数据阶段；`n=` 说明递了几个字 |
| `wr ok=1` 但 `n` 小于 128 | 命令结束了，FIFO 没喂满 | 查 `SPACE_AVAILABLE` 与卡钟；这句本身说明主机侧有停滞 |
| `st … s=` 不是 4、`p=100` | 卡一直没回到 transfer/ready | 卡把写当成失败或忙在内部；先看 `wr int` |
| `rd ok=0` 或 `cmp<128`，但 `w0` 以 `554e5855` 开头 | 写进去了，读回不完整 | 读路径回归；对照 round99 的 `int=00000003` |
| `rd ok=1 cmp=128` 但 `old` 与上次不同 | logfs 首块内容变了（或本来就非零） | 正常：每次写都会改它；只要 `w0` 对就行 |
| `efi 0` | LBA 1 的回归坏了 | 先修读通路 |

产物（内核变了；root / trust cache / boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round102-payload.img` | 58720256 B | `cf4dfd1ac8246fda06255b8aed24060c1e1a098d25265df7ee636bdd53ac6ebc` |
| `round102-kernel.macho`（构建产物） | 51949568 B | `9740967661e5b926611f50c8421a4efc5febf5654ae70bdd7169f78cc1a0a619` |
| `round102-in-volume-kernel.macho`（卷内副本） | 51949568 B | `d0fac878e1c3886472a208cb327cb88aebd7d1ad0f6742e98572b41f88251341` |
| `round102-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round102-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round102-kernel.macho.adt` | 13476 B | `7fdafb898b82076a1380169492d0bb273a5884cec30734e0ce09620e06708dc6` |
| `round102-unixnu.toml` | 574 B | `ab259d48009a91e042f646cf47429c26f829f5e71f0f7950a8aa48520d9d9161` |

校验器：探针 35 处 5960 字节、PMGR 5 条绑定 0 位移、CloverSdhci 六条串全在。
反汇编：`writePioBlock` 在对象里按 `0x0e`/`0x200`/`1`/`0xffffffff`/arg/`0x2`/`0x183a`
的顺序写寄存器，调用点是 `mov w0,#0xbc0000; bl __ZL13writePioBlockjPKjRjS1_`；
内核里 `coldBringUp` 在 `0xfffffff00797ba50`、`readPioBlock` 在
`0xfffffff00798e610`、`writePioBlock` 在 `0xfffffff00798e85c`。报告侧逐字符发射的
`\nwr a=`、`\nst `、`\nold `、`\nrd ok=`、`\nw0 ` 分别在对象 `0x15f34`、`0x16160`、
`0x162a8`、`0x1636c`、`0x16570`。门是 `mode != 16`，分发器
`mode >= 7 && mode <= 16`。卷内 boot-args 末尾是
`… clover-pmgr=2 clover-emmc=16 armcpu-clock-panic=60000`。
boot 仍是 `round87-boot.img`（`196b284f…`）：

```sh
fastboot flash cache ~/clover-out/round102-payload.img
fastboot reboot
```

**拍一张就够。** 这一块写的是 logfs 的首块；`old` 行会记下它原来的样子。

## round102 读数：写进去、等它编程完、原样读回来

照片：

```text
lg i=39 a=12320768 b=12337151
wr a=12320768 n=128 ok=1 int=00000003
st 00000900 s=4 p=0
old 00000000 00000000
rd ok=1 cmp=128 sum=6e53ce59
w0 554e5855 00bc0000 0c24bafa
ps e=01f800f0 clk=0007
efi 1
```

这一张把写通路整段闭合了，而且每个数都能独立复算：

1. **写命令干净结束。** `wr … n=128 ok=1 int=00000003`：RESPONSE|DATA_END，
   128 个字全部递给数据口，没有超时/CRC。
2. **卡自己说它编程完了。** `st 00000900 s=4 p=0`：R1 的 bit 8
   （`READY_FOR_DATA`）置位、状态机在 transfer（4）；`p=0` 说明第一次 CMD13
   就问到了，不需要第二轮。
3. **读回是逐字一致的，不是「看着像」。** `rd ok=1 cmp=128`，并且
   `sum=6e53ce59` 与按载荷公式（`0x554e5855 + i×0x01000193`，头四个字被覆盖）
   独立算出的和完全相同。
4. **头部字段也都对得上。** `w0 554e5855 00bc0000 0c24bafa`：`"UXNU"`、
   首 LBA 12320768（`0x00bc0000`）、以及第 4..127 字的和——这个校验和同样是
   离线算好后与照片逐位相同的。
5. **旧内容是零。** `old 00000000 00000000`：写之前 logfs 首块是空白的，
   所以这次读回的一致只能来自刚写进去的那一块。
6. **回归没动。** `ps e=01f800f0 clk=0007`、`efi 1`、`v3 6440 6040` 与 round99
   一致。

写通路不再有未知项。下一件事不是再证明能写，而是写**有意义的内容**：把这份报告
本身作为文本存进 logfs，再从盘上读回来。这样日志才真正「存在分区里」，Linux
侧 `dd` 就能取走。

## v0.2.75（round103）：把报告文本本身存进 logfs 再读回

mode 17 = mode 15 的整条链，跳过 mode 16 的测试块，改成在报告拼完之后把它自己
写进盘。盘上是一条两块的记录，布局固定：

| LBA | 内容 |
| --- | --- |
| `a`（12320768） | 头块，128 个字：`[0]=554e5855`（"UXNU"）、`[1]=31474f4c`（"LOG1"）、`[2]=a`、`[3]=总字节数`、`[4]=字节和`、`[5]=17`、`[6]=正文长度`，其余为零 |
| `a+1` | 文本块：`unixnu clover emmc log\n` + 报告正文 + 一个换行，后面补零到 512 字节 |

写入顺序与 round102 完全相同（CMD24、同节奏 FIFO、`READY_FOR_DATA` 轮询），两块
各做一次；每块写完立刻读回：头块数 128 个字里有多少字相同（`hc=`），文本块数
实际长度内有多少字节相同（`tc=`）。报告追加两行：

```text
tx a=12320768 n=343 h=1 t=1 hc=128 tc=343
txh 554e5855 31474f4c 00000157 <checksum>
```

`n` 是盘上文本的总字节数（前缀 + 正文 + 换行；按 round102 那张照片的行宽算是 343），
`0x157` 就是 343，`h`/`t` 是两次写的判定，
`hc`/`tc` 是两次读回的逐字/逐字节相同数，`txh` 是从盘上读回的头四个关键字段。

判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `tx … h=1 t=1 hc=128 tc=n` + `txh 554e5855 31474f4c <n> <sum>` | 报告文本已按固定布局落盘并能原样读回 | Linux 侧 `dd` 取 logfs 前两块，读同一段文本 |
| `tx g=1` | 坐标门拦住，没写 | 回到 mode 15 看 `lg` |
| `tx i=0` | 扫描没找到 logfs | 回到 mode 15 查名字比较 |
| `h=1 t=1` 但 `hc<128` 或 `tc<n` | 读回不完整 | 对照 round102 的 `rd ok=1 cmp=128` 查数据机 |
| `h=0` 或 `t=0` | 某一次写没干净结束 | 报表里补打 `hs=`/`ts=` 的 INT_STATUS |

产物（内核变了；root / trust cache / boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round103-payload.img` | 58720256 B | `13b67e16c9200ffa1f90d7e17bdf716b26ac17efdf90479f7750ff8fc38bbea9` |
| `round103-kernel.macho`（构建产物） | 51949568 B | `07850bad3663b3634f9dc5d2c469b8f7d79eb6a41892493b96742aa71406d835` |
| `round103-in-volume-kernel.macho`（卷内副本） | 51949568 B | `61418591efc7b2fbc026594d83aff6a8cd65d29ef59b32b6102b43de5c73e6db` |
| `round103-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round103-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round103-kernel.macho.adt` | 13476 B | `d82c818dd60a84f4fdcc5931d9ceeab438fe56386b5cc6ba1617bcb925269c78` |
| `round103-unixnu.toml` | 575 B | `dcd9c79ef24d26dfd2d4ab75817c0d0e0554a19dce5c2058528de43b4b07eb60` |

校验器：探针 35 处 5959 字节、PMGR 5 条绑定 0 位移、CloverSdhci 六条串全在。
反汇编自查：对象里 `writePioBlock`/`waitCardReady`/`readPioBlock` 的调用对一共四组——
mode 17 的头块 `0x19fc4/0x19fd8/0x19ff4`、文本块 `0x1a12c/0x1a140/0x1a160`，mode 16 的
旧块读 `0x1c4f4`、写 `0x1cc60/0x1cc74`、读回 `0x1c644`；头块常量 8 字节
`55 58 4e 55 4c 4f 47 31` 在对象里可查，文本块参数是 `mov w0,#1; movk w0,#0xbc,lsl#16`
（= 12320769）；`\ntx a=` 的逐字符发射在对象 `0x1a72c` 起，`\ntxh ` 在 `0x1ab00` 起，
后接 `emitHex`。内核符号：`coldBringUp` `0xfffffff00797ba50`、
`readPioBlock` `0xfffffff00798ebf4`、`writePioBlock` `0xfffffff00798ee40`、
`waitCardReady` `0xfffffff00798f08c`。门是 `mode != 17`，分发器
`mode >= 7 && mode <= 17`。卷内 boot-args 末尾是
`… clover-pmgr=2 clover-emmc=17 armcpu-clock-panic=60000`。
boot 仍是 `round87-boot.img`（`196b284f…`）：

```sh
fastboot flash cache ~/clover-out/round103-payload.img
fastboot reboot
```

**拍一张就够。** 这一轮会覆盖 logfs 的首两块：块 0 是新的头块，块 1 是报告文本。

Linux 侧取日志（设备已 root；这也是「日志真的存在分区里」的独立验证）：

```sh
adb shell su -c 'dd if=/dev/block/by-name/logfs bs=512 count=2' > logfs.bin
xxd logfs.bin | head
dd if=logfs.bin bs=512 skip=1 count=1 2>/dev/null | tr -d '\0'
```

## round103 读数：343 字节的原样记录，从卡上读了回来

照片：

```text
tx a=12320768 n=343 h=1 t=1 hc=128 tc=343
txh 554e5855 31474f4c 00000157 000059b2
```

这一次把「写」和「读」都走完了，而且每一层都有独立对证：

1. **两块都写干净、读干净。** `h=1 t=1`：头块与文本块的 CMD24 都以 DATA_END 结束，
   中间那次 CMD13 也没报错。`hc=128` 是头块读回后逐字相同 128/128；`tc=343` 是
   文本块在实际长度内逐字节相同 343/343。
2. **长度自洽。** `n=343`，头块里存的是 `00000157`，`0x157` 就是 343。前缀
   `unixnu clover emmc log\n` 占 23 字节，报告正文 319 字节，末尾换行 1 字节。
3. **校验和自洽。** 头块里存的是 `000059b2`。把落盘的那段文本（前缀 + 正文 +
   换行，恰好 343 字节）离线逐字节相加，结果就是 `0x59b2`。这不是回读循环的副产品：
   回读只数「哪些字节相同」，而 `txh` 里的和是写之前另外算的。
4. **所以盘上现在有一条自描述记录。** 头 8 字节是 `UXNU`/`LOG1`，之后是起始 LBA、
   总字节数、字节和、mode 号。任何能读 logfs 的系统都能先认头、再取文本。

还没证的是它能不能跨启动。`hc`/`tc` 都是同一次启动里写的、同一次启动里读的，
缓冲区可能只是内存里的副本。mode 18 只读不写，就是来把这条缝补上。

## v0.2.76（round104）：下一次启动，只把上一轮存下的记录读回来

mode 18 = mode 15 的整条链，之后**一个字也不写**：

1. GPT 扫描仍按 UTF-16 整字找 `logfs`，坐标仍必须是 `12320768..12337151`；
   不满足就只打 `bk i=0` 或 `bk g=1`，不碰任何块。
2. 读块 0（`a`），取出 `m0=entry[0]`、`m1=entry[1]`、`n=entry[3]`、`s=entry[4]`。
   只有 `m0=554e5855`、`m1=31474f4c` 且 `n<=512` 时才读块 1（`a+1`）；
   否则不动块 1，`n` 与 `s` 原样打出来，错的头也是读数。
3. 读回块 1 后，对前 `n` 字节重新求和，与头里的 `s` 比，得到 `c=`。
4. 再打前 48 个字节：换行印成 `|`，不可打印字印成 `.`，其余原样。这一段是
   「读到的确实是文本」的肉眼证据，不依赖任何数值。

预期（上一轮 round103 写下的记录仍在）：

```text
bk m=554e5855 31474f4c n=343 s=000059b2 c=1
bt unixnu clover emmc log|...
```

`c=1` 的意思是：这次启动从盘上读到的 343 字节，其和等于上一次启动写进头块的数。
它没有写任何东西，所以这个等式只可能来自卡上的记录。

判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `bk m=554e5855 31474f4c n=343 s=000059b2 c=1` + `bt unixnu clover emmc log\|…` | 记录跨启动存活，读回的字节与上一轮写下的相同 | 把真正的内核/panic 日志接到这条记录路径上 |
| `bk … n=343 s=… c=1` 但 `n`/`s` 与 round103 的 `343`/`000059b2` 不同 | 有记录，但不是上一轮那条（有人重跑过 mode 17 或改过盘） | 先看 `bt` 是否仍是本项目的文本，再决定 |
| `bk m=554e5855 31474f4c … c=0` | 头是对的，文本字节与头里的和不一致 | 读 `bt` 看坏在哪一段；若全为零，说明块 1 没写进去 |
| `bk m=00000000 00000000 n=0 …` | 块 0 不是本项目的头（空块或别的数据） | 回 mode 17 重新写一遍，或检查 `a` 是否仍是 logfs 首块 |
| `bk m=` 是别的值 | 块 0 有数据但不是本项目的记录 | 记下这两个字，它说明 logfs 首块被别的系统用过 |
| `bk i=0` / `bk g=1` | 没找到 logfs 或坐标不符，什么都没读 | 回 mode 15 查 `lg` |
| 有 `hs=`/`ts=` | 某次读没干净结束，状态字直接打出来 | 对照 round102/103 的 `int=00000003` |

产物（内核变了；root / trust cache / boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round104-payload.img` | 58720256 B | `6d566aeef7de7b8dc18716816755dc41f274b0d3aea2313a068405a58fc5625f` |
| `round104-kernel.macho`（构建产物） | 51965952 B | `ac916c15fdaf68771e1eadabdad9cb19c292262403d02ea2ed6f5083dfb9332e` |
| `round104-in-volume-kernel.macho`（卷内副本） | 51965952 B | `255cbc1042e48b34232045eb2bc8feb4d45a3fe9ba41a71d8a7de3725dace346` |
| `round104-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round104-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round104-kernel.macho.adt` | 13476 B | `f32ab7fb67eaf9007acaa5dfc0cb10ed7a80f4f74de3e7577ae0a656b19aa84f` |
| `round104-unixnu.toml` | 580 B | `c3b56e3ce263c71e3ae25540bf67b804da8d50032f94558586115296b5ab560f` |

校验器：探针 35 处 5960 字节、PMGR 5 条绑定 0 位移、CloverSdhci 六条串全在。
反汇编自查：mode 18 只有两次 `readPioBlock` 调用，在对象 `0x15e6c`（头块）与
`0x15ec0`（文本块，参数 `mov w0,#1; movk w0,#0xbc,lsl#16`），这一区没有任何
`writePioBlock`；magic 比较是 `mov w27,#0x5855; movk w27,#0x554e,lsl#16` 与
`mov w26,#0x4f4c; movk w26,#0x3147,lsl#16`，长度上限 `cmp w19,#0x200`。
`\nbk i=0`/`\nbk g=1` 的逐字符发射在 `0x15fb4`/`0x16078` 起，`\nbk m=` 在
`0x1ba40` 起，`\nbt ` 在 `0x1bccc` 起，字节映射常量 `.`=`0x2e`、`|`=`0x7c` 在
`0x1bd4c`/`0x1bd50`。内核符号：`coldBringUp` `0xfffffff00797ba50`、
`readPioBlock` `0xfffffff00798f214`、`writePioBlock` `0xfffffff00798f460`、
`waitCardReady` `0xfffffff00798f6ac`；编译进的 panic 行号是 3269。
门是 `mode != 18`，分发器 `mode >= 7 && mode <= 18`。卷内 boot-args 末尾是
`… clover-pmgr=2 clover-emmc=18 armcpu-clock-panic=60000`。
boot 仍是 `round87-boot.img`（`196b284f…`）：

```sh
fastboot flash cache ~/clover-out/round104-payload.img
fastboot reboot
```

**拍一张就够。这一轮什么都不写。** 它读的记录应该是你上一次刷 round103 后留在
logfs 首两块的那条；只要中间没有别的系统写过 logfs，`n=343 s=000059b2` 就应当
原样出现。再刷一次同一颗 payload 再拍一张，如果两次数值相同，就说明读的是盘，
不是这一次启动的内存。

## round104 读数：记录跨启动还在，这一轮一个字也没写

照片（节选）：

```text
lg i=39 a=12320768 b=12337151
bk m=554e5855 31474f4c n=343 s=000059b2 c=1
bt unixnu clover emmc log|g cfg=00002017 m=01 n=fc
@CloverSdhci.cpp:3269
```

这是第一次「上一次启动写、这一次启动读」的闭环，而且四层证据互相对得上：

1. **`c=1` 只能来自卡。** mode 18 的整条链里没有一次 `writePioBlock`，缓冲区块 1
   是这次启动刚从盘上读出来的；它对前 343 字节重新求和，等于头块里上次写下的
   `000059b2`。内存里没有任何东西能提供这个等式。
2. **头没变。** `m0=554e5855`、`m1=31474f4c`、`n=343`，与 round103 写下的完全一致；
   `s` 也还是 `000059b2`。`lg` 那行同时给出坐标 `12320768..12337151`，说明读的仍是
   `mmcblk0p39`，不是别处碰巧同样带 magic 的块。
3. **`bt` 是肉眼证据。** 前 48 字节打印出来是 23 字节前缀
   `unixnu clover emmc log\n`（换行印成 `|`）加上正文的头 25 字节
   `g cfg=00002017 m=01 n=fc`（第 48 个字节是 `d=` 前的空格，照片上看不见）——
   正是 round103 报告的第一行。也就是说盘上躺着的不只是数字，是那一轮屏幕上
   报告的原文。
4. **窗口只有 48 字节。** 23 + 25 正好截在第一行中间，所以 `bt` 故意不打全；
   要读全文用 Linux 侧 `dd`。

结论：一条自描述记录能在两次冷启动之间存活，读回路径不依赖写路径的内存副本。
还没证的是「下一条能不能写在不覆盖它的地方」。mode 19 只改选址，不改写/读本身。

## v0.2.77（round105）：不再覆盖首块，改为追加到下一个空槽

mode 19 = mode 15 的整条链 + mode 18 的只读检查 + mode 17 的记录布局，多出的一步是
**先找空位**。logfs 开头按固定的两块一格划分，共 16 格：

| 格 | LBA | 内容 |
| --- | --- | --- |
| 0 | `a+0`、`a+1` | 已有记录（round103 写的） |
| 1 | `a+2`、`a+3` | 本轮的记录 |
| … | … | … |
| 15 | `a+30`、`a+31` | 最后一格 |

`a` 仍是 logfs 首 LBA（`12320768`）。扫描从格 0 开始逐格读头块：头两个字仍是
`554e5855`/`31474f4c` 就认为这一格被占，继续下一格；第一格读不到 magic 就是空位。
扫描最多 16 格，满了只打 `ap f=`，绝不回绕到格 0——后一次启动不能悄悄吃掉最老的记录。

找到空位 `k` 后，写头块（`[5]=19`、`[6]=正文长度`、`[7]=k`）与文本块，节奏与
mode 17 完全相同（CMD24 → 同节奏 FIFO → CMD13 等 `READY_FOR_DATA`），每块写完
立刻读回，数 `hc=`/`tc=`。报告追加两行：

```text
ap s=1 ss=2 a=12320770 n=343 h=1 t=1 hc=128 tc=343
bta unixnu clover emmc log|g cfg=...
```

`s` 是落盘的格号，`ss` 是这次一共读了几次头块（含发现空位的那一次；`s=1 ss=2`
表示格 0 已占、格 1 是空位），`a` 是新记录的头块 LBA，`n` 是文本总字节数，
`h`/`t` 是两次写的判定，`hc`/`tc` 是两次读回的相同数，`bta` 是从盘上读回的新记录
前 48 字节。

预期（上一轮 round103 的记录仍在格 0，round104 只读没写；`bk`/`bt` 是 mode 18
那一段对格 0 的复读，`ap`/`bta` 是本轮新写的格 1）：

```text
bk m=554e5855 31474f4c n=343 s=000059b2 c=1
bt unixnu clover emmc log|...
ap s=1 ss=2 a=12320770 n=343 h=1 t=1 hc=128 tc=343
bta unixnu clover emmc log|...
```

判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `ap s=1 ss=2 a=12320770 n=343 h=1 t=1 hc=128 tc=343` + `bta unixnu clover emmc log\|…` | 格 0 被正确认成已占，新记录写在格 1 并原样读回；旧记录没被碰 | 再刷一次：`s` 应变成 2、`a` 变成 `12320772`，格 1 的记录仍在 |
| `ap s=0 ss=1 …` | 格 0 的头没认出来，本轮覆盖了旧记录 | 查 magic 比较与 `bk` 那条链；旧记录可能已丢 |
| `ap s=1 … h=1 t=1` 但 `hc<128` 或 `tc<343` | 格找对了，回读不完整 | 对照 round103 的 `hc=128 tc=343` 查数据机 |
| `ap f=16 ss=16` | 16 格全被占，什么都没写 | 用 Linux 侧把 logfs 前 64 KB 清掉，或扩大格数 |
| `ap r=<k> st=…` | 扫描第 k 格头块读失败 | 看状态字，对照 round102 的 `int=` 序列 |
| `ap i=0` / `ap g=1` | 没找到 logfs 或坐标不符 | 回 mode 15 看 `lg` |

产物（内核变了；root / trust cache / boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round105-payload.img` | 58720256 B | `ff695c1b9041b08dd9496b3a93e4f578da6cbf5e11a5886b1a36112d9a28fc63` |
| `round105-kernel.macho`（构建产物） | 51965952 B | `15fafb114b7f06b9ca87ac402834fb6469ef9f39dd7179b631a35a745d67e7a5` |
| `round105-in-volume-kernel.macho`（卷内副本） | 51965952 B | `31a434da698fa2a97b8fc3724f95f32b868efc14dbd6c417e97800687878ac5a` |
| `round105-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round105-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round105-kernel.macho.adt` | 13476 B | `c3d41454c34072c1a7c4ad621b989b4201f547e1fe8dff67447ddd08e75a6295` |
| `round105-unixnu.toml` | 573 B | `f3c8442cb439a2c56ddc68f1cdd74ad46b086f2d38739f0f1780a5d4c2613ac3` |

校验器：探针 35 处 5960 字节、PMGR 5 条绑定 0 位移、CloverSdhci 六条串全在。
反汇编自查（对象 `CloverSdhci.o`）：

- 扫描循环：起点 LBA `0xbc0000`，`add w22, w22, #2` 在 `0x1c27c`，上限
  `cmp w20, #0x10` 在 `0x1c280`/`0x1c344`；扫描头块读在 `0x1c260`。
- magic 常量 `55 58 4e 55 4c 4f 47 31` 在 `__TEXT,__literal8` `0x1f638`
  （`UXNULOG1`），mode 17/19 共用。
- 头块构造：magic 8 字节在 `0x1c75c`，`[2..3]` 在 `0x1c760`，`[4..5]`
  在 `0x1c768`（`mov w10,#0x13` 在 `0x1c764`，即 `[5]=19`），`[6..7]`
  在 `0x1c76c`（`w20` 是格号）。
- PIO 调用对：头块 `0x1c77c`/`0x1c790`/`0x1c7a8`，文本块
  `0x1d01c`/`0x1d030`/`0x1d04c`。
- 字面量逐字符发射：`\nap s=` 在 `0x1d220` 起，`\nbta ` 在 `0x1d7a4` 起，
  后接 `emitDec`/`emitByte` 与 `.`=`0x2e`、`|`=`0x7c` 的映射
  （`0x1d84c`/`0x1d850`）。

内核符号：`coldBringUp` `0xfffffff00797ba50`、
`readPioBlock` `0xfffffff007990610`、`writePioBlock` `0xfffffff00799085c`、
`waitCardReady` `0xfffffff007990aa8`；编译进的 panic 行号是 3418
（`mov w8,#0xd5a` 在 `0x1f530`）。门是 `mode != 19`，分发器
`mode >= 7 && mode <= 19`。卷内 boot-args 末尾是
`… clover-pmgr=2 clover-emmc=19 armcpu-clock-panic=60000`。
boot 仍是 `round87-boot.img`（`196b284f…`）：

```sh
fastboot flash cache ~/clover-out/round105-payload.img
fastboot reboot
```

**拍一张就够。这一轮只写格 1，格 0 应保持 round103 的原样。** 如果还想证追加，
再刷一次同一颗 payload：`s` 应变成 2、`a=12320772`，而格 0、格 1 两条旧记录
仍然在盘上（Linux 侧 `dd` 可读）。

Linux 侧取日志（设备已 root；现在 logfs 开头是两格连续记录）：

```sh
adb shell su -c 'dd if=/dev/block/by-name/logfs bs=512 count=4' > logfs.bin
xxd logfs.bin | head
# 格 0 的文本（round103）
dd if=logfs.bin bs=512 skip=1 count=1 2>/dev/null | tr -d '\0'
# 格 1 的文本（本轮）
dd if=logfs.bin bs=512 skip=3 count=1 2>/dev/null | tr -d '\0'
```

## round105 读数：格 0 被认成已占，新记录落在格 1 并原样读回

照片（节选）：

```text
bk m=554e5855 31474f4c n=343 s=000059b2 c=1
bt unixnu clover emmc log|g cfg=00002017 m=01 n=fc
ap s=1 ss=2 a=12320770 n=439 h=1 t=1 hc=128 tc=439
bta unixnu clover emmc log|g cfg=00002017 m=
@CloverSdhci.cpp:3418
```

追加这条路一次就通了，而且每一层都对得上：

1. **`s=1 ss=2` 就是扫了两次。** 读格 0 的头块，magic 对，判为已占；再读格 1
   的头块，magic 不在，停在这里。空位 `s=1`，因此写的位置 `a=12320768+2=12320770`
   ——不是事先算好的常量，是扫出来的结果。
2. **`n=439` 是 343 加上新添的两行。** 上一轮正文 343 字节；mode 19 在它后面多打了
   `\nbk …`（44 字节）和 `\nbt …`（52 字节），343 + 44 + 52 = 439，一个字节不多。
   前缀和结尾换行没变，所以增长完全来自这两行。
3. **`h=1 t=1 hc=128 tc=439` 是原样读回。** 头块写对、文本块写对，读回字节数与
   写入完全相同；`tc=439` 也再次印证第 2 点——盘上那条记录和屏幕上这一条一样长。
4. **`bta` 是格 1 的肉眼证据。** 前缀之后是 `g cfg=00002017 m=`，和 `bt`（格 0）
   的开头同源但截得更早——它读的是刚写的格 1，不是残留的格 0。
5. **格 0 没被动。** `bk … n=343 s=000059b2 c=1`：长度、校验和、跨启动读回判定
   与 round104 一字不差，说明这一次只碰了格 1。
6. **`@CloverSdhci.cpp:3418` 与构建一致**，所以屏上跑的就是归档的 round105 payload。

结论：自描述记录能连续占两格而不互相覆盖，选址来自盘上内容而不是内存里的假设。
下一步该让记录里装的不再只是驱动的自述，而是**别处产生的文本**——panic 屏幕那份。

## v0.2.78（round106）：panic 屏把画出的文本交给驱动，写进下一格

这一轮换掉记录的**作者**。之前每一条记录都是驱动在正常启动里自己拼的报告；现在
panic 屏把它已经画在 framebuffer 上的文本（panic 消息 + 屏幕自己的读数）整段交给
驱动，由驱动追加成一条 kind 20 的记录。下一次启动用 mode 20 把最新一条读回来。

链路分四段：

1. `scripts/xnu-panic-screen.c` 新增一个 480 字节的栈上捕获缓冲。`put()` 在画字时
   顺手把字符复制进去（换行保留，其它不可打印字节变空格），所以捕获的就是屏幕
   实际会显示的内容，而不是另拼一份。
2. 所有文字和诊断画完之后、写「第一次 panic」标记之前，屏调用
   `PANIC_SINK(capture, captured)`。`PANIC_SINK` 由
   `scripts/patch-xnu-entry-probe.py` 从卷内内核的符号表里解析
   `_clover_emmc_panic_append` 得到，和其它诊断地址一样是**符号**，不是固定地址。
   没有这个符号的内核保持旧行为（`PANIC_SINK=0`）。
3. 驱动导出 `extern "C" unsigned int clover_emmc_panic_append(const char*, unsigned)`。
   它不复用任何启动链：只检查 `gPanicLogReady`（本启动确实读完过一条 logfs 记录）、
   `gHost`、`gPanicLogFirst` 和文本指针，然后从格 0 开始读头块，找到第一个没有
   magic 的格，按 mode 17 的布局写头块与文本块。正文截到 480 字节，前缀 23 + 正文
   480 + 换行正好 504，仍是一块。做完两块都等 `READY_FOR_DATA`，**不做读回**——
   panic 路径上再读一次只会多一个可能挂住的点；读者是下一次启动。
4. mode 20 在正常启动里扫描 16 格，记住**最后一个** magic 合法的格，读它的文本块、
   按头块长度重算字节和，打印 `rd s= n= m= s= c= ss=` 和两段 32 字节预览
   （`rt` 开头、`re` 结尾）。只有成功读完一条记录才把 `gPanicLogFirst` 设成 logfs
   首 LBA 并置 `gPanicLogReady=1`，panic 钩子从此可以用。

panic 钩子和它调用的 PIO 辅助函数都按 `-mgeneral-regs-only` 编译：panic 时内核
不能保存 FP/SIMD 状态。整份 `CloverSdhci` 因此都不再使用 SIMD，这也让两个 12 字节
的 `CMD13 rca=1`/`CMD13 sweep` 字面量被 clang 折成立即数、不再出现在 `__cstring`，
所以 `scripts/verify-clover-payload.py` 的「成员在镜像里」检查改成这两个字符串之外
的四个字符串加上 `_clover_emmc_run` / `_clover_emmc_panic_append` 两个导出符号
（六项，含义不变）。

第一次刷同一颗 payload（本启动读到的是 round105 留在格 1 的记录，kind 19）：

```text
rd s=1 n=439 m=19 s=000059b2 c=1 ss=2
rt unixnu clover emmc log|g cfg=00
re ...（round105 报告最后 32 字节，末尾是 bta 预览行的尾部）
...
elog=102
```

`elog=` 是 panic 屏画在最后的判定：`102` = 100 + 格号 2，头块和文本块都写成功。
`s=1`、`m=19`、`n=439`、`s=000059b2` 与 round105 的照片逐项相同，说明 mode 20
读的确实是上一轮那条；`c=1` 是这次启动从卡上重算的和。随后 panic（`@CloverSdhci.cpp:3606`）
把屏上文本写进格 2。

再刷同一颗 payload（不换镜像，只重启）应读到刚写的那条：

```text
rd s=2 n=504 m=20 s=<新和> c=1 ss=3
rt unixnu clover emmc log|CloverSdh
re ...（捕获文本的第 457..480 字符，落在诊断区）
...
elog=103
```

`m=20` 证明这条不是报告链写的；`n=504` 证明正文被 480 字节截断；`rt` 的
`CloverSdh` 是 panic 消息 `CloverSdhci:\n...` 的开头，报告链永远不会有这个开头；
`elog=103` 说明这次又把新的 panic 文本写进了格 3。**两条记录都在盘上，格 0、1
没被覆盖。**

判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| 第一刷 `rd s=1 n=439 m=19 s=000059b2 c=1 ss=2` + `elog=102` | mode 20 读到 round105 的记录并武装；panic 屏把文本写进格 2 | 再刷同一颗 payload，应出现 `m=20 n=504` 和 `elog=103` |
| 第二刷 `rd s=2 n=504 m=20 c=1 ss=3` + `elog=103` | 记录的作者已经换成 panic 屏；捕获、追加、重算和跨启动读回全闭合 | 可以开始把 `IOBlockStorageDevice` 接到这条已经会写日志的路径上 |
| 有 `rd …` 但 `elog=1` | 钩子拒绝：未武装、主机空、首 LBA 为 0 或文本指针空 | 先看 `rd` 是否成功；`rd` 成功而 `elog=1` 说明卷内屏没有拿到/没有调用 sink |
| 屏上只有 `elog=` 没有 `rd` | 记录写成功，但 mode 20 在这之前就没读到记录 | 看 `rd e=0`/`rd i=0`/`rd g=1`，回 mode 19 先写一条 |
| `elog=2` | 扫描头块读失败 | 对照 mode 19 的 `ap r=<k> st=…` |
| `elog=3` | 16 格全满，什么都没写 | 用 Linux 侧清 logfs 前 64 KB |
| `elog=4` / `elog=5` | 头块 / 文本块写失败 | 看 PIO 路径；panic 时已禁用中断，只能靠状态字 |
| 第二个 32 字节预览里没有 `CloverSdh` 之类的 panic 文本 | 写的不是 panic 屏捕获的缓冲 | 查 `PANIC_SINK` 是否解析到 `_clover_emmc_panic_append` |
| `rd … c=0` | 读回文本与头块 `s` 不符 | 写入未完成或长度错；先看 `elog` 是否 102/103 |

产物（内核变了；root / trust cache / boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round106-payload.img` | 58720256 B | `fc0b2ba082a1425860e62f39cff91e0661303f95ca9953ef1f87eb14514f1d42` |
| `round106-kernel.macho`（构建产物） | 51965952 B | `43d94be24f67d2381a5ae7c18f9bcb4685cdc8eba473e8d71f14ec7266951a58` |
| `round106-in-volume-kernel.macho`（卷内副本） | 51965952 B | `6b2a0d4dbe5c7876c71f4d15698515ee2fea314320812c6aaa8d32b53dfc62b2` |
| `round106-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round106-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round106-kernel.macho.adt` | 13476 B | `babba3e5b1395b1727429be5ed54d73f2337dac12da8810992e79ffadc9482fe` |
| `round106-unixnu.toml` | 573 B | `c1f071d6652ab5c6c04fca009bff8473de08b51189312de2ba55c3c7337720a6` |

校验器：探针 35 处 6039 字节、PMGR 5 条绑定 0 位移、CloverSdhci 六项全在
（`clover-emmc`、`look-only dump of c0c4000`、`no pwr`、`CloverSdhci: failed at stage`、
`_clover_emmc_run`、`_clover_emmc_panic_append`）。卷内内核与本地按同样参数重打的
内核逐字节相同（`6b2a0d4d…`），所以下面这个 sink 调用确实在要刷的镜像里。

反汇编自查：

- 驱动对象 `CloverSdhci.o` 里 `_clover_emmc_panic_append` 是 `__text` 的第一个符号。
  槽上限 `cmp w22,#0x10` 在 `0xbc`；扫描读 `bl readPioBlock` 在 `0x9c`；
  magic 常量在 `0x64`–`0x70`；正文上限 `cmp w19,#0x1e0` 在 `0x10c`；
  `mov w8,#0x14` 在 `0x1dc`（头块 `[5]=20`）；头块写 `bl writePioBlock` 在 `0x1f0`；
  文本块写在 `0x3f0`；返回值 `add w8,w22,#0x64` 在 `0x4c4`。整份对象没有
  `movi.16b`/`ushll`/`addv`/`fmov`/`ldp q` 之类的 SIMD 指令。
- mode 20 的武装点在对象 `0x1e7a0`/`0x1e7ac`：因为 gate 已经把唯一可能的非零
  `entryFirst14` 钉成 `12320768`，clang 把 `gPanicLogFirst` 写成字节 `1` 当哨兵，
  并把真正的 LBA 折成常量。panic 钩子里 `mov w26,#0xbc0000` 在链接后
  `0xfffffff007971934`，`csel` 在 `0xfffffff007971948`，与哨兵检查配套；
  这不是丢高位，是编译器的全程序推断。
- panic 屏从 `0xfffffff007a32610` 起 `0xe7a` 字节。sink 调用在屏内偏移 `0x9f4`：
  `movn x8,#0xE73F; movk x8,#0x797,lsl#16; movk x8,#0xfff0,lsl#32`（对象里写成
  `mov x8,#-59200; movk x8,#1943,lsl#16; movk x8,#65520,lsl#32`）把 `x8` 装成
  `0xfffffff0079718c0`，紧接 `blr x8` 在偏移 `0xa00`；之后才画 `elog=`。
  捕获缓冲 480 字节，`logcap` 在 `put()` 里限住。
- 内核符号：`_clover_emmc_panic_append` `0xfffffff0079718c0`、
  `_clover_emmc_run` `0xfffffff00797232c`、`coldBringUp` `0xfffffff00797c4f8`、
  `readPioBlock` `0xfffffff007971d98`、`writePioBlock` `0xfffffff007971fe4`、
  `waitCardReady` `0xfffffff007972230`。编译进驱动对象的 panic 行号是 3606
  （`mov w8,#0xe16` 在对象 `0x200dc`），与屏上的 `@CloverSdhci.cpp:3606` 对应。
  门是 `mode != 20`，分发器 `mode >= 7 && mode <= 20`。卷内 boot-args 末尾是
  `… clover-pmgr=2 clover-emmc=20 armcpu-clock-panic=60000`。
- boot 仍是 `round87-boot.img`（`196b284f…`）。

```sh
fastboot flash cache ~/clover-out/round106-payload.img
fastboot reboot
```

**拍一张就够，但这一轮值得拍两张。** 第一张是 round106 的进入状态：读到
round105 的 `m=19` 记录、`elog=102`。如果只拍这一张，只能证明屏写了；再刷一次
同一颗 payload、冷启动，第二张出现 `m=20 n=504` 和 `elog=103`，才证明它写下的
文本能被下一次启动读回。两次之间不要换镜像、不要动 logfs。

Linux 侧取三格（设备已 root；格 k 的文本在块 `2k+1`）：

```sh
adb shell su -c 'dd if=/dev/block/by-name/logfs bs=512 count=8' > logfs.bin
# 格 1：round105 的报告（kind 19）
dd if=logfs.bin bs=512 skip=3 count=1 2>/dev/null | tr -d '\0'
# 格 2：panic 屏第一次写的文本（kind 20）
dd if=logfs.bin bs=512 skip=5 count=1 2>/dev/null | tr -d '\0'
# 格 3：第二次启动再写的那条（kind 20）
dd if=logfs.bin bs=512 skip=7 count=1 2>/dev/null | tr -d '\0'
```

## v0.2.79（round107）：把 eMMC 发布成 IOBlockStorageDevice，由 IOMedia 读 GPT 与 logfs

这一轮换掉读者。前 106 轮里，卡上的每一个字节都是 `drivers/CloverSdhci` 自己发 CMD17、
自己按节奏排空 FIFO 读出来的。这一轮它只负责把卡交出去：新的 `drivers/CloverBlock`
在**同一条 PIO 路径**上发布一台 `IOBlockStorageDevice`，IOStorageFamily 加载后自己
挂上 `IOBlockStorageDriver`、建出 `IOMedia`，然后 GPT 头、表项数组、logfs 格头和正文
全部**经 IOMedia** 读回来。屏上第一段仍是驱动自己那条链，第二段是存储栈的读数，
两段在同一个 panic 里（`panic("CloverSdhci:\n%s\n%s", …)`）。

链路分四段：

1. mode 21 在 mode 20 的链（识别、GPT 头、`logfs` 定位、最新格扫描）后面多发一条
   CMD8 `SEND_EXT_CSD`（arg 0，512 字节，仍用同一条 paced PIO 排空）。字 53 是
   SEC_COUNT。只有它大于 logfs 末 LBA（12337151）才写 `gBlockSectors` 并置
   `gBlockReady`——geometry 来自这张卡自己的 EXT_CSD，而不是常量；一张连这个端口
   已经读过 16384 个扇区的分区都到不了的卡，也不该被交给存储栈。
2. 驱动导出 `clover_emmc_block_ready/sectors/read`。`read` 一块一条 CMD17，整次请求
   用一个两秒的 `CNTVCT` 预算，并用一次 4 字节比较交换挡住重入；未就绪、越界、
   读失败都返回**已经完成的块数**，不挂起。
3. `CloverBlockDevice` 手写 `MetaClass`。IOStorageFamily 是运行时从 boot kext collection
   加载的，链接期没有 `IOBlockStorageDevice` 的元类地址，所以标准
   `OSDefineMetaClass*` 宏不能用：`superClass` 在 `OSKextLoadKextWithIdentifier`
   之后填，再用 `OSMetaClass::preModLoad/postModLoad` 注册本类。类大小必须恰好等于
   基类上报的 `0x90`，否则停在 stage 2——偏移错一格，下面所有 override 都会落在
   错的虚槽上。基类 ctor/dtor 的两个符号（同样不导出）由
   `scripts/build-clover-block.py` 按名别名到 `IOService`，并与全部 190 个 vtable
   条目、26 个必须覆盖的槽一起审计。
4. 注册用 `registerService(kIOServiceSynchronous)`，`IOBlockStorageDriver` 的
   attach/start 就在这次调用里完成。stage 6 在 service plane 找到驱动，stage 7 找到
   `IOMedia` 并比较它的 size/block size 与本驱动的 geometry；stage 9 以读者身份
   `open`；stage 10 起只走 `IOMedia::read`。设备的 `doAsyncReadWrite` 只接受
   `kIODirectionIn` 并**内联**调用完成回调，所以整条 `IOMedia::read` 链在本启动点
   同步返回；设备层依旧用异步签名是因为推迟与否只有栈自己知道。

`IOMedia::read` 的完成标志是一个静态结构：回调写 done/status/actual，调用返回后
立刻检查。若请求被推迟到工作环而在这个启动点没有定时器能等它，函数返回
`kIOReturnTimeout`（`0xe00002d6`），屏上就是 `gp=0 … hs=e00002d6`——这是
「存储栈没有内联完成」的读数，不是卡读失败。换成同步形态则可能在 `IOSleep` 里
永久停住，而这块板子的唯一输出就是这一张照片。

成功时两段应长这样（`<n>` 是这张卡的 SEC_COUNT，`<k>/<m>/<len>` 是盘上最新一条
logfs 记录；round106 已在实机上写进过 kind 20 的 panic 记录，所以 `m/n` 多半是
`20/504`）：

```text
CloverSdhci:
...（驱动链，与 round106 相同）
ex 1 n=128 sc=<n> st=<...>

b st=15 ld=0 cs=0090 pm=0
b dv=1 md=1 sz=<n> bl=512
b op=1 wp=0 gp=1 ix=39
b lf s=<k> a=12320768 b=12337151 m=<m> n=<len> c=1 p=1
b tx <最新一条记录的正文开头>
...
elog=1
```

各字段：

- `ex` 是驱动侧：`ok=1` 表示 EXT_CSD 512 字节整块读完，`n=128` 是字数，
  `sc` 是发布给块设备的扇区数，`st` 是数据阶段结束时的 `INT_STATUS`。
- `b st=15` 是存储栈走完；`ld` 是 `OSKextLoadKextWithIdentifier` 的返回，
  `cs=0090` 是存储栈自己报的类大小（也是本次编译时断言的），`pm` 是
  `postModLoad` 的返回。
- `dv=1 md=1` 是 service plane 里真的出现了 `IOBlockStorageDriver` 和 `IOMedia`；
  `sz` 必须等于 `ex` 的 `sc`，`bl=512`。
- `op=1` 是媒体以读者身份打开；`wp=0` 表示不可写（这一轮只发布读）；
  `gp=1` 是 GPT 头经 IOMedia 读到并认出 `EFI PART`；`ix=39` 是 `logfs` 在表项
  数组里的 1 起序号。
- `b lf` 是经 IOMedia 读到的 logfs 坐标与最新格：`a/b` 是首末 LBA，
  `m/n` 是 kind 与长度，`c=1` 是以头块长度重算的字节和与头块一致，`p=1` 是
  正文前缀 `unixnu clover emmc log\n` 对得上。`b tx` 是正文预览。
- `elog=1` 在这里是**预期**的：mode 21 不武装 panic 钩子（`probeMode == 20`
  才武装），因为它已经把卡交给了另一个子系统，这一轮也不写 logfs。`rd` 那行
  同样不会出现，它是 mode 20 的。round106 的照片里 `elog=` 已经落在固定屏高之外，
  所以这一行看不看得见都不影响判定；`b st=15` 那几行才是判定依据。

判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `ex 1 n=128 sc=<n>`，`sz=<n>`，`gp=1 ix=39`，`c=1 p=1` | XNU 的存储栈自己读到了这张卡：geometry 来自 EXT_CSD，GPT、表项数组、logfs 头和正文都经 IOMedia 返回 | 存储栈这条线闭合；下一轮可以把 logfs 挂成真正的块设备消费者 |
| `ex 0`，随后 `b st=1` | EXT_CSD 没读完（看 `n`/`st`），或 SEC_COUNT 不大于 logfs 末 LBA；块设备从未发布 | 先修 `ex`；`n<128` 与 `st` 一起区分是少了数据还是命令失败 |
| `b st=2` | IOStorageFamily 加载失败（`ld≠0`），或类大小不是 `0x90`（`cs≠0090`） | 看 `ld`/`cs`；后者说明基类布局变了，override 必须重排 |
| `b st=3` | `preModLoad` 失败，或 `postModLoad` 返回非零（`pm`） | 类没注册上，不会出现驱动 |
| `b st=4` | `new`/`init` 失败 | 内存或属性初始化 |
| `b st=5` | `attach`/`start` 失败 | 平台节点或 `start` 的返回值 |
| `b st=6` | 服务注册了，但没有 `IOBlockStorageDriver` 子节点（`dv=0`） | IOStorageFamily 已加载但没匹配；看类名与 provider |
| `b st=7` | 没有 `IOMedia`，或它的 size/block size 与 geometry 不符（`md=1` 时看 `sz`/`bl`） | 比较 `sz` 与 `ex` 的 `sc` |
| `b st=9` | 媒体拒绝以读者身份打开 | 上一阶段的挂载/独占状态 |
| `b st=10`，`hs=e00002d6` | `IOMedia::read` 没有内联完成；请求被推迟，而这里没有定时器等它 | 这是「栈没同步完成」的读数；卡本身已在此前的 stage 证明可读 |
| `b st=10`，`hs=` 其它值 | IOMedia 读 GPT 头返回错误 | `hs` 就是那个 `IOReturn` |
| `b st=11` | 表项数组读失败（`hs`） | 与 round101 的 `lg` 链对照 |
| `b st=12` 且有 `hs` | 格头读失败 | 与 round104 的 `rd r=` 对照 |
| `b st=12` 且无 `hs`、`lf s=0 m=0 n=0` | 第一格没有 magic：logfs 是空的 | 先让 mode 19/20 写一条 |
| `b st=13` 且有 `ts` | 正文块读失败 | |
| `st=15` 但 `c=0` | 读回的正文与头块里的字节和不符 | 写入端或读取端有一处错位 |
| `st=15` 但 `p=0` | 正文前缀不是 `unixnu clover emmc log\n` | 读到的不是这份布局 |

产物（内核变了；root / trust cache / boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round107-payload.img` | 58720256 B | `2508c2ff173e5d2b0eab9c520e945e8fbb15dd41648de6b517345eddda33b37f` |
| `round107-kernel.macho`（构建产物） | 51990528 B | `fe7e9f272089bb1132821fe673c310610b64959df82de88f6a5a6f08a4ee4969` |
| `round107-in-volume-kernel.macho`（卷内副本） | 51990528 B | `13cb96411082ab15eed597ac8eddb42c36e041c58f49803ee7d451c0d7efdb52` |
| `round107-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round107-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round107-kernel.macho.adt` | 13476 B | `ffc817be657b9b3a8ff3ace99d243e6eea854ba1157044beab87656bf550e19c` |
| `round107-unixnu.toml` | 557 B | `60c985c31523b1c6b6c43471d80496d3f40e139bbf8775108ee5c61b9e594dd1` |

校验器：探针 35 处 6040 字节、PMGR 5 条绑定 0 位移、CloverSdhci 六项全在
（`clover-emmc`、`look-only dump of c0c4000`、`no pwr`、`CloverSdhci: failed at stage`、
`_clover_emmc_run`、`_clover_emmc_panic_append`）。`scripts/build-clover-block.py`
报告 `libCloverBlock.a` 276288 字节、190 个 vtable 条目、26 个必须覆盖的槽，
每个槽都是运行时类的 override，每个 import 都有归属，两条基类别名与三个 eMMC
导入都记录在 `build-report.json`。

反汇编自查：

- 分发白名单已从 1..20 扩到 1..21：clang 把它折成
  `sub w9,w1,#0x16; cmn w9,#0x16; b.hi`，21 在界内。mode 21 的分支是
  `cmp w8,#0x15; b.ne`。`coldBringUp` 的 `probeMode` 由
  `sub w9,w8,#7; cmp w9,#0xf; csel w1,w8,wzr,lo` 生成，即 7..21。
- `clover_emmc_run` 末尾先 `bl _clover_block_run` 再 `bl _panic`；panic 的格式串
  `CloverSdhci:\n%s\n%s` 在 `0xfffffff00712f8d8`，两个参数分别是
  `0xfffffff007b1a0a8`（`clover_emmc_report`）与 `0xfffffff007b1a330`
  （`clover_block_report`）。
- `readMediaBlock` 只有一次 vtable 调用，槽偏移 `0x558`（`IOMedia::read`），
  紧随其后的 `done` 检查失败即返回 `mov w0,#0x2d6; movk w0,#0xe000,lsl#16`；
  完成回调地址是 `mediaReadDone`。
- 内核符号：`_clover_emmc_block_ready` `0xfffffff0079718c0`、
  `_clover_emmc_block_sectors` `0xfffffff0079718d0`、
  `_clover_emmc_block_read` `0xfffffff0079718e0`、
  `_clover_emmc_panic_append` `0xfffffff007971a20`、
  `_clover_emmc_run` `0xfffffff007972244`、`_clover_block_run` `0xfffffff007991da0`、
  `readMediaBlock` `0xfffffff007993d60`、`mediaReadDone` `0xfffffff007993e4c`、
  `__GLOBAL__sub_I_CloverBlock.cpp` `0xfffffff007993e60`。卷内 boot-args 末尾是
  `… clover-pmgr=2 clover-emmc=21 armcpu-clock-panic=60000`。
- boot 仍是 `round87-boot.img`（`196b284f…`）。

```sh
fastboot flash cache ~/clover-out/round107-payload.img
fastboot reboot
```

**拍一张就够。** 这一张回答的问题是：XNU 自己的存储栈能不能经 `IOBlockStorageDevice`
→ `IOBlockStorageDriver` → `IOMedia` 读到这张卡。`st=15`、`dv=1 md=1`、`gp=1`、
`c=1 p=1` 同时出现，就是能；`st` 小于 15 时，它自己会在 `b st=` 上停下并指名阶段。
这一轮不写 logfs（panic 钩子没武装，`elog` 即使可见也是 1），所以刷完前后盘上的
记录不受影响。

## v0.2.80（round108）：让 payload 自己报出身份、命令行与块栈阶段

round107 的照片是一次**普通启动**：固件日志、XNU 的 `using 4098 buffer headers …`、
`apfs_module_start`、`AMFI: PMGRAON latched to: ffffffff`、
`AMFI: Enabling developer mode …`、`IOCPUInterruptController: not registry member`，
然后就没有了——没有 `CloverSdhci:` 的 panic。PMGR 的 nub 显然已经发布（AMFI 都 latch
到了它），说明 `sheng_pmgr_run()` 跑过；而 mode 21 必定 panic，所以只能是
`clover_emmc_run()` 在缺参数的静默 `return` 处退出：**这次内核收到的命令行里没有
`clover-emmc`**。核对下来，round83/84/86 那几代的 boot-args 恰好是
`clover-pmgr=2` 而没有 `clover-emmc`，最可能的解释是固件这次启动的并不是刚刷进去的
那一份 payload。但屏幕上没有任何一个字能证明这一点，这一轮先把这台「测谎仪」装上。

四处改动，各自回答一个问题：

1. `sheng_pmgr_run()` 在任何分支之前无条件打印本次收到的完整命令行
   （`CloverPMGR r108: args [...]`，数据源是 `PE_boot_args()`）。有这一行，就证明
   跑的是这份内核；没有，就证明刷写没有落到固件实际启动的那个卷上。
2. 缺 `clover-emmc` 不再静默返回，改为
   `panic("CloverSdhci r108: no clover-emmc boot arg\nargs [%s]", …)`。参数拼错是
   payload 构建错误，不该伪装成「启动正常」。
3. mode 21 的 panic 末尾再打印一次命令行（`args [...]`）。上面的 IOLog 会随启动日志
   滚出屏幕，判定依据必须和读数在同一块屏上。
4. `clover_block_run()` 在每个阶段之前打一行 `CloverBlock r108: <step>`：`2 storage
   family`、`3 metaclass`、`4 allocate`、`5 attach`、`5 register`、`6 driver`、
   `7 media`、`9 open`、`10 gpt`、`11 entries`、`12 slots`、`13 text`、`15 done`。
   如果 `b st=` 那几行没出现，最后一行标记就是卡住的阶段——round107 连「卡住」还是
   「没跑」都分不出来，这一轮分得出来。

成功时屏上应含（`<...>` 与 round107 相同）：

```text
CloverPMGR r108: args [-v serial=3 debug=0x14e kt_fixed_zones=120 boot-progress=2
 rd=md0 allow_legacy_mach_msg=1 armcpu-timer-intid=19 clover-pmgr=2 clover-emmc=21
 armcpu-clock-panic=60000]
...
CloverBlock r108: 2 storage family
...
CloverBlock r108: 15 done
...
CloverSdhci:
...（驱动链）
b st=15 ld=0 cs=0090 pm=0
b dv=1 md=1 sz=<n> bl=512
b op=1 wp=0 gp=1 ix=39
b lf s=<k> a=12320768 b=12337151 m=<m> n=<len> c=1 p=1
b tx <最新一条记录的正文开头>
args [-v serial=3 … clover-emmc=21 armcpu-clock-panic=60000]
```

命令行那一行在屏上可能被折行；判定只看三个词：`clover-pmgr=2`、`clover-emmc=21`、
`armcpu-clock-panic=60000` 是否都在。

判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| 有 `CloverPMGR r108: args [...]`，且其中含 `clover-emmc=21`，末尾 `b st=15 …` | 这就是 round107 该有的读数，存储栈这条线真正闭合 | 下一轮把 logfs 挂成真正的块设备消费者 |
| 有 `CloverPMGR r108: args [...]`，但**不含** `clover-emmc`，随后 `CloverSdhci r108: no clover-emmc boot arg` | 固件启动的确实是这份内核，是 payload 的命令行拼错了 | 对 `board.toml` 的 `kernel_args` 重建 payload |
| **没有** `CloverPMGR r108` 这一行 | 固件启动的不是这份内核：刷写没有落到它实际读取的卷/分区，或菜单启动的是另一个条目 | 核对 `fastboot flash cache` 的目标与固件启动卷；这一轮不再猜参数 |
| 有 `CloverPMGR r108`，随后只有部分 `CloverBlock r108: <step>`，没有 `b st=` | 块栈在最后一个 `<step>` 之后停住；参数是对的，问题在那一阶段内部 | 按 `<step>` 定位；这是 round107 缺的那个「停在哪」 |
| 固件日志里出现 `DARWINBOOT: boot args rejected at ...` | 命令行在 DarwinBoot 就被拒（超长或非 ASCII），内核拿不到任何参数 | 缩短 `kernel_args`；本行在 UEFI 阶段就已画出 |

产物（内核变了；root / trust cache / boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round108-payload.img` | 58720256 B | `b61f8194e22dd06df97beda5caa4b3475fdaed0144ec5aa399793ad532ab0a12` |
| `round108-kernel.macho`（构建产物） | 51990528 B | `4dcf80b9873d83f55251e66a7219450b7ee38d2c1cc13086780225f44385e43c` |
| `round108-in-volume-kernel.macho`（卷内副本） | 51990528 B | `dcc1105a96cebb225afea32b49d4e14654a1bb5e1655539e9d40dc9130cf2eec` |
| `round108-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round108-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round108-kernel.macho.adt` | 13476 B | `7dce589dda054f64d9448d05ce45c45e1ce4d6e4baaad3ca0dc124de37df7c1d` |
| `round108-unixnu.toml` | 567 B | `a91845a1a2eb02f9af1e41d98776a02dd3dc0d0b02261e31373e00849b932f39` |

校验器：探针 35 处 6038 字节、PMGR 5 条绑定 0 位移、CloverSdhci 六项全在，
卷内 boot-args 末尾仍是 `… clover-pmgr=2 clover-emmc=21 armcpu-clock-panic=60000`。
三条新字符串在卷内内核里都在：`CloverPMGR r108: args [%s]`、
`CloverSdhci r108: no clover-emmc boot arg`、`CloverBlock r108: %s`。

内核符号：`_sheng_pmgr_run` `0xfffffff007971274`、`_clover_emmc_run`
`0xfffffff007972254`、`_clover_block_run` `0xfffffff007991d6c`。
boot 仍是 `round87-boot.img`（`196b284f…`）。

```sh
fastboot flash cache ~/clover-out/round108-payload.img
fastboot reboot
```

**拍一张就够（可能两张）。** 这一张回答的问题是：这次到底是哪个 payload 在跑、
它拿到的命令行是什么、块栈停在哪个阶段。先看屏上有没有
`CloverPMGR r108: args [...]`，再读那行里的参数，最后看 `b st=`；
三者在同一张照片里能同时判定。若参数缺失，屏上会出现 `CloverSdhci r108` 的 panic，
同样是完整读数，不需要重刷。这一轮仍然不写 logfs（panic 钩子未武装），盘上记录不变。

## round108 读数（照片，v0.2.80 实机）：payload 对了，块栈停在同步注册里面

照片开头是 `CloverPMGR r108: args [...]`，括号里完整包含
`clover-pmgr=2 clover-emmc=21 armcpu-clock-panic=60000`——**刷进去的这份 payload
确实在跑，round107 的「旧 payload」疑问排除**。接着是
`CloverBlock r108: 2 storage family`、`3 metaclass`、`4 allocate`、`5 attach`、
`5 register`，然后在同一块屏上出现 `apfs_module_start…`、`ktrrglue_register_subsystem_strings`、
`AMFI: PMGRAON latched to: ffffffff`、`AMFI: Enabling developer mode…`、
`IOCPUInterruptController: not registry member at registerService()`，之后没有 `6 driver`、
没有 `b st=`、也没有 mode 21 的 panic。

读法是确定的，不是猜：

- `5 register` 与 `6 driver` 之间只有一条语句，`device->registerService(kIOServiceSynchronous)`；
  `blockStage("6 driver")` 在它返回后无条件执行，mode 21 又在 `clover_block_run()` 返回后
  无条件 panic。两者都没出现 ⇒ **调用没有返回**，卡在 `registerService` 内部。
- PMGR 自己在这一轮之前的两条 `registerService(kIOServiceSynchronous)`（provider 和 nub）
  都返回了（否则走不到块栈），所以「同步注册」这个机制本身在这台机器上是通的。
- 差别在于匹配对象：`ApplePMGR`/`ApplePMGRNub` 没有 kext 认领，匹配集是空的；我们的
  `CloverBlockDevice` 会被 IOStorageFamily 的 `IOBlockStorageDriver` 认领。
- 因此那几行 APFS/AMFI/IOCPU **不是** `bsd_init` 在 `sheng_pmgr_run()` 之后的正常续跑
  （若续跑，就必然先看到 `6 driver` 和 panic），而是这次 inline 匹配把早期的 kext 启动
  级联拉进了自己的调用栈。最后一行的 `IOCPUInterruptController … registerService()` 是
  级联里另一次注册，之后停在某个 kext 启动里——这个启动点还没到
  `IOKitInitializeTime()`，任何依赖定时器/后续服务的等待都不会被满足。

所以这张照片否定的是「块设备 ABI 不对」：加载、类大小、元类注册、分配、attach/start
全部走过（2–5 attach）。真正卡住的是「让 IOCatalogue 现场做同步匹配」这一步。round109
不再经过它。

## v0.2.81（round109）：不经过匹配，按名字手工启动 IOBlockStorageDriver

`clover_block_run()` 的 stage 5 改为：

1. `5 attach`：设备 attach 到 platform 并 `start`（与上一轮相同，已证明会返回）。
2. `5 driver meta`：`OSMetaClass::getMetaClassWithName("IOBlockStorageDriver")`。
3. `5 driver alloc`：`driverMeta->alloc()` + `init(nullptr)`。
4. `5 driver attach`：`driver->attach(device)`。
5. `5 driver start`：`driver->start(device)`。

这三步正是 `IOService::startCandidate()` 对一个候选驱动做的事，只是候选集由我们自己
指定、不经过 `findDrivers`/`isModuleLoaded`/kext 加载。我们的设备从未进入
`kIOServiceSynchronousState`，因此驱动内部为 IOMedia 做的注册是异步的，不会再从
这里进入那条早期 kext 级联。每步都先打 `CloverBlock r109: <step>`，卡住时最后一行
就是答案：停在 `5 driver start` 说明问题在驱动的媒体创建/注册里，不是匹配。

`b st=` 行新增两个读数：`dm=`（元类找到）与 `ds=`（`driver->start` 返回 true）。
`CloverPMGR r109` / `CloverSdhci r109` 同步改号，照片仍能自证是哪一版在跑。

成功时，除 `<step>` 编号外，期望屏与上一轮相同：`b st=15 ld=0 cs=0090 pm=0 dm=1 ds=1`、
`b dv=1 md=1 sz=<n> bl=512`、`b op=1 wp=0 gp=1 ix=39`，以及 `b lf`/`b tx`。
若停在 `5 driver start`，则 `b st=5 … dm=1 ds=0` 会出现在 panic 屏上（mode 21 的
panic 在块段之后，始终会画）。

成功时屏上应含：

```text
CloverPMGR r109: args [-v serial=3 debug=0x14e kt_fixed_zones=120 boot-progress=2
 rd=md0 allow_legacy_mach_msg=1 armcpu-timer-intid=19 clover-pmgr=2 clover-emmc=21
 armcpu-clock-panic=60000]
...
CloverBlock r109: 2 storage family
CloverBlock r109: 3 metaclass
CloverBlock r109: 4 allocate
CloverBlock r109: 5 attach
CloverBlock r109: 5 driver meta
CloverBlock r109: 5 driver alloc
CloverBlock r109: 5 driver attach
CloverBlock r109: 5 driver start
CloverBlock r109: 6 driver
...
CloverBlock r109: 15 done
...
CloverSdhci:
...（驱动链）
b st=15 ld=0 cs=0090 pm=0 dm=1 ds=1
b dv=1 md=1 sz=<n> bl=512
b op=1 wp=0 gp=1 ix=39
b lf s=<k> a=12320768 b=12337151 m=<m> n=<len> c=1 p=1
b tx <最新一条记录的正文开头>
args [-v serial=3 … clover-emmc=21 armcpu-clock-panic=60000]
```

判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| 标记一路走到 `15 done`，末尾 `b st=15 … dm=1 ds=1`、`b dv=1` | 手工启动绕开了同步匹配，块栈闭合 | 下一轮把 logfs 挂成真正的块设备消费者 |
| 停在 `5 driver meta`，`b st=5 … dm=0 ds=0` | `IOBlockStorageDriver` 这个元类此刻还不存在（IOStorageFamily 尚未启动） | 先按标识符加载 IOStorageFamily，再取元类 |
| 停在 `5 driver alloc` / `5 driver attach`，`dm=1 ds=0` | 元类找到了，实例化或 attach 失败（类大小/ABI 不符） | 读 `cs=` 与驱动的类大小对照 |
| 停在 `5 driver start`，`dm=1 ds=0` | 驱动起来了但 `start` 未返回或未成功：媒体创建/注册是新的卡点 | 反汇编 `IOBlockStorageDriver::start` 的媒体创建段 |
| 停在 `5 attach` 或更早（2/3/4） | 上一轮已通过的位置出现回归 | 按最后一行标记回看该阶段 |
| 没有 `CloverPMGR r109` 这一行 | 固件启动的不是这份 payload | 核对 `fastboot flash cache` 的目标与固件启动卷 |

产物（内核与 payload 变了；root / trust cache / boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round109-payload.img` | 58720256 B | `05e8e1b1e5819057b453487ff6e94ac1e62a58bd116871c9919f238f9f04572d` |
| `round109-kernel.macho`（构建产物） | 51990528 B | `1e84dbc2949a3415491f2cdf3b20bef0e2cb36d7677c0ed15b8397c251010365` |
| `round109-in-volume-kernel.macho`（卷内副本） | 51990528 B | `dd528b77eef73132348ae21a9fe5dde19fa90245bb6c1a78e860a41189a15586` |
| `round109-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round109-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round109-kernel.macho.adt` | 13476 B | `4fcc133045865ca1d372c17975529741a5700f39a0b130d2dcf007af7d4742f0` |
| `round109-unixnu.toml` | 583 B | `8d334ae9966ef043f78ffb5788e28dec11b69892f3b1626b4a40c5a98053d1c0` |

校验器：探针 35 处 6040 字节、PMGR 5 条绑定 0 位移、CloverSdhci 六项全在，
卷内 boot-args 末尾仍是 `… clover-pmgr=2 clover-emmc=21 armcpu-clock-panic=60000`。
三条新字符串在卷内内核里都在：`CloverPMGR r109: args [%s]`、
`CloverSdhci r109: no clover-emmc boot arg`、`CloverBlock r109: %s`。
卷内文件是只读挂载取出后逐字节校验的（本轮的 payload 曾因一次可写挂载被改写，
已重建并从只读挂载重新归档）。

内核符号：`_sheng_pmgr_run` `0xfffffff007971274`、`_clover_emmc_run`
`0xfffffff007972254`、`_clover_block_run` `0xfffffff007991d6c`、
`lookupMetaClass` `0xfffffff0079940f8`。
boot 仍是 `round87-boot.img`（`196b284f…`）。

```sh
fastboot flash cache ~/clover-out/round109-payload.img
fastboot reboot
```

**拍一张就够（可能两张）。** 先看 `CloverPMGR r109: args [...]` 是否在、参数是否齐全，
再顺着 `CloverBlock r109:` 的标记数到停在哪一条，最后读 panic 屏上的
`b st=… dm=… ds=…`。三者同屏即完整读数。这一轮仍不写 logfs（panic 钩子未武装），
盘上记录不变。

## round109 读数（v0.2.81 实机）：手工启动经住了，块栈闭合

上一轮的判定表走到了成功分支：标记一路到 `15 done`，`b st=15 ld=0 cs=0090 pm=0
dm=1 ds=1`、`b dv=1 md=1 …`，`b lf … c=1 p=1` 与 `b tx` 都在。这一读数说明：

- 按名字取 `IOBlockStorageDriver` 元类、`alloc`/`init(nullptr)`/`attach`/`start` 这条
  绕开 IOCatalogue 的路径，在这台机器上返回了；round108 卡住的从来不是 ABI，而是同步匹配。
- 驱动确实挂成了设备的子节点（`dv=1`、`driver == handDriver`），它创建的 IOMedia 也
  通过了 `sz=`/`bl=512` 的几何核对（`md=1`）。
- GPT、表项数组、logfs 头和正文全部经 IOMedia 读回，校验和与前缀都对（`c=1 p=1`）。
  也就是说，XNU 的存储栈已经是一个真实的**读**消费者。

这一轮没有照片，读数按判定表的成功分支记录。剩下的一半是写。

## v0.2.82（round110）：让 logfs 的写入也经 IOMedia

上一轮只证明了读。写仍然只有 CloverSdhci 的私有 PIO（mode 17/19 和 panic 钩子），
块设备对上层声明的是只读。这一轮把写这一半也交给存储栈：

1. `reportWriteProtection()` 改答 `false`，IOBlockStorageDriver 据此建出可写媒体；
   stage 9 的 `open` 从 `kIOStorageAccessReader` 改为 `ReaderWriter`。
2. CloverSdhci 新增 `clover_emmc_block_write()`，门控与 `block_read` 相同（ready、
   EXT_CSD 几何、单调用者 guard、计数器 2 秒上限），底层复用 mode 17/19 已验证的
   `writePioBlock`（CMD24，160 µs 节奏、200 ms 数据相位上限）。
3. `CloverBlockDevice::doAsyncReadWrite()` 现在同时处理 `kIODirectionOut`：
   从 descriptor 取出一块，交给 `clover_emmc_block_write`。完成回调契约与读方向一致。
4. 新 stage 14 `14 write`：按 mode 19 的规则找第一个没有本驱动 magic 的空槽，把一条
   固定记录（前缀 + `block write through IOMedia`）的头块和正文块经 **IOMedia 的写路径**
   写入，再经 **IOMedia 的读路径**把两块整块读回、逐字节比较。报告新增
   `b w s=<slot> wr=1 rb=1`；失败时 `ws=` 带上 IOReturn。
5. 16 个槽写满时停在 stage 14、`wr=0`，不绕回槽 0；panic 钩子仍未武装，panic 屏上的
   记录写入仍归 mode 20。

成功时屏上应含：

```text
CloverPMGR r110: args [-v serial=3 debug=0x14e kt_fixed_zones=120 boot-progress=2
 rd=md0 allow_legacy_mach_msg=1 armcpu-timer-intid=19 clover-pmgr=2 clover-emmc=21
 armcpu-clock-panic=60000]
...
CloverBlock r110: 13 text
CloverBlock r110: 14 write
CloverBlock r110: 15 done
...
b st=15 ld=0 cs=0090 pm=0 dm=1 ds=1
b dv=1 md=1 sz=<n> bl=512
b op=1 wp=1 gp=1 ix=39
b lf s=<旧槽> a=12320768 b=12337151 m=<kind> n=<len> c=1 p=1
b w s=<新槽> wr=1 rb=1
b tx <上一启动记录的正文开头>
args [-v serial=3 … clover-emmc=21 armcpu-clock-panic=60000]
```

`wp` 从 0 变 1 是这一轮的契约变化：媒体现在是可写的。`b lf` 仍是**本次启动读到**的
上一条记录；`b w` 才是这次经块栈写进去的那一条。

判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `14 write`、`15 done`、`b w s=<n> wr=1 rb=1` | 写和读回都经 IOMedia 完成，存储栈成为真正的读写消费者 | 同一 payload 再重启一次：stage 13 应读到这条 `b tx block write through IOMedia` |
| `wr=1 rb=0`，`ws=` 为读回错误 | 写完成，但从媒体读回的内容不一致 | 对照 `ws`，查写/读路径的块偏移与缓存 |
| `wr=0 rb=0`，停在 `14 write`，`ws=` 非零 | 写请求在块设备或卡上失败 | 按 `ws` 的 IOReturn 定位：不可写、参数、卡错误 |
| 停在 `14 write` 且 `s=16` | 16 个日志槽已满，按设计不绕回 | 下一轮实现窗口回收或手动清槽 |
| 停在 `9 open` | 媒体仍不可写，ReaderWriter 打开被拒 | 查 `reportWriteProtection` 是否被 IOMedia 缓存 |
| 停在 13 或更早 | 读路径出现回归 | 按最后一条标记回看该阶段 |
| 没有 `CloverPMGR r110` | 固件启动的不是这份 payload | 核对刷写目标与启动卷 |

产物（内核与 payload 变了；root / trust cache / boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round110-payload.img` | 58720256 B | `8196ed2e74dfe5bd6965e285214adde51beecc70e766c01f7bdfa33db13f22e2` |
| `round110-kernel.macho`（构建产物） | 51990528 B | `a10c3863a1cbd6c56c02fb11f9b8b555f19334a8787604108e279e095513f57c` |
| `round110-in-volume-kernel.macho`（卷内副本） | 51990528 B | `bbdcb286aad8cd20e3606e8856294345a11f845954e6e65c228b237e121512d2` |
| `round110-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round110-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round110-kernel.macho.adt` | 13476 B | `d2d7793d43669c447f253604962b6cff902c258022ad2a0836c17892858b9daa` |
| `round110-unixnu.toml` | 579 B | `dc5fb66dd90a97c1073419fedaf5ed93c8425f57a3e0164a5d3564c723916db3` |

校验器：探针 35 处 6040 字节、PMGR 5 条绑定 0 位移、CloverSdhci 八项全在（含
`_clover_emmc_block_read` 与 `_clover_emmc_block_write` 两个符号），卷内 boot-args 末尾
仍是 `… clover-pmgr=2 clover-emmc=21 armcpu-clock-panic=60000`。三条 r110 字符串在卷内
内核里都在：`CloverPMGR r110: args [%s]`、`CloverSdhci r110: no clover-emmc boot arg`、
`CloverBlock r110: %s`。

内核符号：`_sheng_pmgr_run` `0xfffffff007971274`、`_clover_emmc_block_write`
`0xfffffff007971a30`、`_clover_emmc_run` `0xfffffff00797231c`、`_clover_block_run`
`0xfffffff0079920a0`、`lookupMetaClass` `0xfffffff007994b48`。
boot 仍是 `round87-boot.img`（`196b284f…`）。

```sh
fastboot flash cache ~/clover-out/round110-payload.img
fastboot reboot
```

**拍一张就够；想验证跨启动再重启一次。** 先看 `CloverPMGR r110` 与参数，再看
`14 write` / `15 done`，最后读 `b w s=… wr=1 rb=1` 和 `wp=1`。同一次启动里 `b tx`
仍是上一条记录；再按一次重启（不重刷）后，stage 13 应把这次写的槽读出来，
`b lf s=` 变成这次的槽，`b tx` 变成 `block write through IOMedia`。

## v0.2.83（round111）：写描述符在提交前填好，之后不再碰

round110 的照片停在 `14 write`，panic 屏上是：

```text
Assertion failed: !(kIOMemoryPreparedReadOnly & _flags)
@/xnu/iokit/Kernel/IOMemoryDescriptor.cpp:2612
```

RA / LR 分别落在 `panic_with_data` 与 `IOMemoryDescriptor::writeBytes`。问题不在卡，也不在
块栈：`writeMediaBlock()` 原来在每次 `media->write` 前用 `buffer->writeBytes(0, in, 512)`
往同一个 `outBuffer` 里填块。第一块提交时，IOStorageFamily 按设备读方向 `prepare()` 了这个
`kIODirectionOut` 描述符，置上 `kIOMemoryPreparedReadOnly`（`UPL_COPYOUT_FROM`，
IOMemoryDescriptor.cpp:4405）；第二块的 `writeBytes` 因此命中 2612 的断言。第一块其实已经
写进槽里了，所以那个槽现在只有一个有效的头块和并不匹配的正文块。

这一轮只改描述符的生命周期：

1. `writeMediaBlock()` 不再改写 buffer，只负责提交；内容由调用者在提交前填好。
2. stage 14 为头块和正文块各建一个
   `IOBufferMemoryDescriptor::withBytes(…, 512, kIODirectionOut, true)`：创建即带内容，
   提交后不再改动。两个 `OSSharedPtr` 声明在函数作用域，活过整次块栈运行。
3. 去掉 stage 9 里共享的 `outBuffer`。

其余路径（只读扫描、槽选择、可写打开、读回比较）与 round110 相同。

成功时屏上应含：

```text
CloverPMGR r111: args [-v serial=3 debug=0x14e kt_fixed_zones=120 boot-progress=2
 rd=md0 allow_legacy_mach_msg=1 armcpu-timer-intid=19 clover-pmgr=2 clover-emmc=21
 armcpu-clock-panic=60000]
...
CloverBlock r111: 13 text
CloverBlock r111: 14 write
CloverBlock r111: 15 done
...
b st=15 ld=0 cs=0090 pm=0 dm=1 ds=1
b dv=1 md=1 sz=<n> bl=512
b op=1 wp=1 gp=1 ix=39
b lf s=<上一槽> a=12320768 b=12337151 m=<kind> n=<len> c=<0/1> p=<0/1>
b w s=<本次槽> wr=1 rb=1
b tx <上一槽正文开头>
args [-v serial=3 … clover-emmc=21 armcpu-clock-panic=60000]
```

如果 `b lf` 指向 round110 留下的半条记录，`c=0 p=0` 是**预期读数**而不是本次失败：头块
里的长度和校验和是真的，正文块却是旧的。这一轮的答案看 `b w s=… wr=1 rb=1`。

判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| `15 done`、`b w s=<n> wr=1 rb=1` | 写和读回都经 IOMedia 完成 | 不重刷再重启一次：stage 13 应读出 `b tx block write through IOMedia` |
| `b lf … c=0 p=0`，但 `b w … wr=1 rb=1` | round110 的半条记录按预期判无效，新记录完好 | 重启一次确认新记录被读回 |
| 停在 `14 write`，panic 又是 2612 | 仍有描述符在提交后被改写 | 看 LR 是否为 `writeBytes`；核对 `withBytes` 的调用路径 |
| 停在 `14 write`，`ws=` 非零 | 媒体写或读回失败 | 按 `ws` 的 IOReturn 定位 |
| `wr=1 rb=0`，`ws=` 为读回错误 | 写完成但读回内容不一致 | 查块偏移与缓存 |
| 停在 `9 open` | 媒体仍不可写 | 查 `reportWriteProtection` 与 open 权限 |
| 停在 13 或更早 | 读路径出现回归 | 按最后一条标记回看该阶段 |
| 没有 `CloverPMGR r111` | 固件启动的不是这份 payload | 核对刷写目标与启动卷 |

产物（内核与 payload 变了；root / trust cache / boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round111-payload.img` | 58720256 B | `8bc00486aea486138ded73f46bb90d6c15fea534fd4c81e6f896809a8d1e05dc` |
| `round111-kernel.macho`（构建产物） | 51990528 B | `fb1a2df9258bf635f95a08a4bee6f6707b27fbe1e830ade3d263c44f13784817` |
| `round111-in-volume-kernel.macho`（卷内副本） | 51990528 B | `07f0d091022b198ce43344edcd1eb8ab90fde80eb526d005b133c7b371fad8b0` |
| `round111-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round111-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round111-kernel.macho.adt` | 13476 B | `acc16b7d9a9be048295e4168d35c687aee20ec2792647207f6ee5a4f380ce6d6` |
| `round111-unixnu.toml` | 579 B | `dc5fb66dd90a97c1073419fedaf5ed93c8425f57a3e0164a5d3564c723916db3` |

校验器：探针 35 处 6040 字节、PMGR 5 条绑定 0 位移、CloverSdhci 八项全在（含
`_clover_emmc_block_read` 与 `_clover_emmc_block_write`），卷内 boot-args 末尾仍是
`… clover-pmgr=2 clover-emmc=21 armcpu-clock-panic=60000`。三条 r111 字符串都在卷内内核里：
`CloverPMGR r111: args [%s]`、`CloverSdhci r111: no clover-emmc boot arg`、
`CloverBlock r111: %s`。

内核符号：`_sheng_pmgr_run` `0xfffffff007971274`、`_clover_emmc_block_write`
`0xfffffff007971a30`、`_clover_emmc_run` `0xfffffff00797231c`、`_clover_block_run`
`0xfffffff0079920a0`、`lookupMetaClass` `0xfffffff007994b78`。
boot 仍是 `round87-boot.img`（`196b284f…`）。

```sh
fastboot flash cache ~/clover-out/round111-payload.img
fastboot reboot
```

**拍一张就够。** 先确认 `CloverPMGR r111` 与参数，再顺着 `13 text` / `14 write` 看停在哪，
最后读 `b w s=… wr=1 rb=1`；`b lf` 若仍指向 round110 的半槽，`c=0 p=0` 属预期。

读数（实机，口头）：回报「没问题了」，未拍照。按判定表的成功分支记录——stage 14 的
写入经 IOMedia 完成、两块读回一致，存储栈从此是真正的读写消费者。跨启动读回
（`b tx block write through IOMedia`）这次没有单独验证。

## v0.2.84（round112）：GPT 交给 XNU 自己的分区方案，根设备走到 bdevvp

round111 之后开始收仪器。`drivers/CloverBlock` 不再自己解析 GPT、不再往 `logfs` 里写记录，
也不再维护面板读数和 512 字节报告文本；它只做一件事，把 eMMC 接进 XNU 的存储栈，然后让
XNU 自己的代码回答“根设备在哪”。

这一轮的链路（boot 仍是 `rd=md0`，不写任何分区）：

1. `clover_emmc_block_ready()` 先证明 PIO 读路径可用，`CloverBlockDevice` 再作为
   `IOBlockStorageDevice` 挂到平台上；`IOStorageFamily` 按标识符加载，类大小必须正好
   `0x90`。
2. 手工启动 `IOBlockStorageDriver`（round109 证明同步匹配在这条引导路径上不返回），整盘
   `IOMedia` 的扇区数必须等于 eMMC 的扇区数。
3. 用 `IOGUIDPartitionScheme` 对整盘做 `probe()` / `start()`：LBA 0、LBA 1 和分区表项都由
   XNU 自己读，不再有第二份手写解析器。分区对象由 scheme 发布为子节点。
4. 在 scheme 的子节点里找 `Partition ID == 39`（`logfs`），数出 scheme 发布的总数。
5. 先给整盘、再给目标分区启动 `IOMediaBSDClient`；后者是正常启动时给每块盘起名的那段
   代码，目标分区应得到 `BSD Name`、`BSD Major`、`BSD Minor`。
6. 复现 `setconf()` 对 `rd=<bsd name>` 做的事：
   `IOBSDNameMatching()` + `IOService::waitForService(1s)` 必须返回目标分区本身。
7. 用 `major << 24 | minor` 调 `bdevvp()`——`vfs_mountroot()` 用的就是这个调用。它成功
   说明 `bdevsw` 的 minor 已由 BSD 客户端登记，根挂载所需的 vnode 可以建立。
8. 最后验证偏移映射：同一块数据，从整盘 `base` 处读一次，从分区 0 处读一次，512 字节必须
   逐字节相同。

成功时只有一行（随后继续引导到 `rd=md0` 的 EL0 init）：

```text
CloverPMGR r112: args [-v serial=3 debug=0x14e kt_fixed_zones=120 boot-progress=2
 rd=md0 allow_legacy_mach_msg=1 armcpu-timer-intid=19 clover-pmgr=2 clover-emmc=21
 armcpu-clock-panic=60000]
...
CloverBlock r112: p=<n> disk0s39 <maj>:<min> bd=1 map=1
...
initproc failed to start ...
```

`p=` 是 scheme 实际发布的分区数；`disk0s39` 是正常命名结果（若之前已有别的磁盘占用了锚点，
名字会是 `disk<N>s39`，成功行会照实打印）。`bd=1` 表示 `bdevvp` 返回 0 且有 vnode，
`map=1` 表示两次读取一致。

失败一律 panic，一行一段，格式 `CloverBlock r112: <step> <value>`：

| 停在哪 | 含义 |
| --- | --- |
| `ready` / `storage` / `abi` | PIO 未就绪、`IOStorageFamily` 未加载、类大小不是 0x90 |
| `modload` / `postload` | 自己的 metaclass 没注册上 |
| `device` / `attach` / `driver` / `media` | 设备、驱动或整盘 IOMedia 没起来 |
| `gpt` / `gpt-probe <score>` / `gpt-start` | scheme 没分配到、没认出 GPT、没启动 |
| `gpt-children` / `gpt-target <n>` | 没有子节点；或 `n` 个分区里没有 `Partition ID 39` |
| `bsd-whole` / `bsd-root` | BSD 客户端在整盘或目标分区上没启动 |
| `bsd-props` | 目标分区缺 `BSD Name` / `BSD Major` / `BSD Minor` |
| `root-match <ptr>` | `IOBSDNameMatching` + `waitForService` 没返回目标分区 |
| `bdevvp <err>` | `dev_t` 打不开，返回 XNU 的 errno |
| `buffer` / `open-whole` / `read-whole` / `open-part` / `read-part` | 缓冲区或读路径失败，值为 IOReturn |
| `map` | 整盘 `base` 处与分区 0 处的内容不同 |
| 没有 `CloverPMGR r112` | 固件启动的不是这份 payload |

判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| 成功行 + 走到 `initproc failed to start` | GPT 发布、BSD 命名、根设备发现、`bdevvp`、偏移映射全部由 XNU 自己验证 | 下一轮把 `rd=` 指向这个 BSD 名做真正的根挂载 |
| 停在 `gpt-probe`，`score` 为 0 | scheme 的 `probe` 没接受整盘（GPT 读不到或 `isFormatted` 不成立） | 先按上一轮已知的 LBA 读核对整盘媒体大小 |
| `gpt-target <n>`，`n>0` | scheme 能读表，但没有 39 号分区 | 核对哪一份 GPT 被读到 |
| `bsd-props` | BSD 客户端启动但没写属性 | 查 `IOMediaBSDClient::start` 的返回与 `bdevsw_add` |
| `root-match 0` | 名字匹配不到刚注册的服务 | 查 `IOBSDNameMatching` 用的名字与注册顺序 |
| `bdevvp <err>` | 名字对，但 minor 打不开 | 按 errno 查 `bdevsw` 登记 |
| `map` | 分区偏移映射不一致 | 查 `getBase()` 与 PIO 的 LBA 换算 |
| 停在更早的步骤 | 存储栈出现回归 | 按 step 名回看该阶段 |

产物（内核与 payload 变了；root / trust cache / boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round112-payload.img` | 58720256 B | `e3504615fa65b599576a165e9d094f2b335315509f5f763a1ad3b164c04d0e7b` |
| `round112-kernel.macho`（构建产物） | 51974144 B | `6c0aa97bdf357b5bc0e526ad7adb567e4fa3b4ffc6ad5bbcb4c9204630c542ac` |
| `round112-in-volume-kernel.macho`（卷内副本） | 51974144 B | `e524faf3e857540527a51ea6321eecda6ef42ef674a86067ac174fdbcb1a363a` |
| `round112-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round112-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round112-kernel.macho.adt` | 13476 B | `4dfcb1239ad9e7377a9d2ad1b2d40678bf39aebd1a1319d85e995e0177e5fb61` |
| `round112-unixnu.toml` | 579 B | `80df7ecf30bd2cb87ee2a72cf6e828a367d701d2e6eb0b242af0f3b008f5689a` |

校验器：`verify-clover-payload.py` 逐文件通过（探针 35 处 6040 字节、PMGR 5 条绑定 0 位移、
CloverSdhci 十项全在），`check-clover.py` 通过（ADT 13476 字节、8 核、splash 与 FD 不变）。
卷内 boot-args 末尾仍是 `… clover-pmgr=2 clover-emmc=21 armcpu-clock-panic=60000`。两条 r112
字符串都在卷内内核里：`CloverPMGR r112: args [%s]`、`CloverBlock r112: p=%u %s %u:%u bd=1 map=1`。

内核符号：`_sheng_pmgr_run` `0xfffffff007971274`、`_clover_emmc_block_write`
`0xfffffff007971a30`、`_clover_emmc_run` `0xfffffff00797231c`、`_clover_block_run`
`0xfffffff007992054`。boot 仍是 `round87-boot.img`（`196b284f…`）。

```sh
fastboot flash cache ~/clover-out/round112-payload.img
fastboot reboot
```

**一次刷机只回答一个问题：GPT 发布和根设备发现是否由 XNU 自己走通。** 成功行出现后，
本轮结论就成立；下一轮才把 `rd=` 从 `md0` 换成这个 BSD 名。

## v0.2.85（round113）：分区方案先 attach 再 probe

round112 的照片是 `Kernel data abort.`，pc `0xfffffff0049754ec`、lr `0xfffffff0049754e8`，
两个地址都落在 `IOStorageFamily` 的 `IOGUIDPartitionScheme::scan()` 里。把反汇编对到源码：

```cpp
IOMedia *media = getProvider();               // ldr x8,[x0]; ldr x8,[x8,#0x370]; blr x8
UInt64 mediaBlockSize = media->getPreferredBlockSize();   // ← 这里取到了 NULL
```

`getProvider()` 返回 `NULL`，`scan()` 没有检查就继续取块大小，所以是零地址数据异常
（`far=0`、`x22=0` 与照片一致）。原因不在存储栈，而在调用顺序：round112 对一个**从未
attach 过**的 scheme 直接调了 `probe()`。`IOService::probeCandidates()` 的顺序是
alloc → init → **attach** → probe → detach，`startCandidate()` 在 start 前还会再 attach
一次；`getProvider()` 读的就是 attach 时挂在 service plane 上的那层父子关系。少了这一步，
probe 里的 `scan()` 看不到整盘媒体。

round113 只补顺序，不改后面任何一步：

1. `scheme->init(nullptr)` 之后先 `scheme->attach(whole)`，失败即 `gpt-attach`。
2. 再 `probe()` / `start()`；对象在 `start()` 期间保持 attach，与 `startCandidate()` 一致。
3. 其余链路（找 `Partition ID 39`、整盘与目标分区各起 `IOMediaBSDClient`、读 BSD 属性、
   `IOBSDNameMatching` + `waitForService` 找根设备、`bdevvp(major << 24 | minor)`、
   整盘 `base` 处与分区 0 处的 512 字节比较）与 round112 相同。

boot 仍是 `rd=md0`，不写任何分区。成功时仍是一行，随后继续引导到 EL0：

```text
CloverPMGR r113: args [-v serial=3 debug=0x14e kt_fixed_zones=120 boot-progress=2
 rd=md0 allow_legacy_mach_msg=1 armcpu-timer-intid=19 clover-pmgr=2 clover-emmc=21
 armcpu-clock-panic=60000]
...
CloverBlock r113: p=<n> disk0s39 <maj>:<min> bd=1 map=1
...
initproc failed to start ...
```

失败一律 panic，一行一段，格式 `CloverBlock r113: <step> <value>`：

| 停在哪 | 含义 |
| --- | --- |
| `ready` / `storage` / `abi` | PIO 未就绪、`IOStorageFamily` 未加载、类大小不是 0x90 |
| `modload` / `postload` | 自己的 metaclass 没注册上 |
| `device` / `attach` / `driver` / `media` | 设备、驱动或整盘 IOMedia 没起来 |
| `gpt` / `gpt-attach` | scheme 没分配到；或 attach 到整盘失败 |
| `gpt-probe <score>` / `gpt-start` | 已 attach 但没认出 GPT、或没启动 |
| `gpt-children` / `gpt-target <n>` | 没有子节点；或 `n` 个分区里没有 `Partition ID 39` |
| `bsd-whole` / `bsd-root` | BSD 客户端在整盘或目标分区上没启动 |
| `bsd-props` | 目标分区缺 `BSD Name` / `BSD Major` / `BSD Minor` |
| `root-match <ptr>` | `IOBSDNameMatching` + `waitForService` 没返回目标分区 |
| `bdevvp <err>` | `dev_t` 打不开，返回 XNU 的 errno |
| `buffer` / `open-whole` / `read-whole` / `open-part` / `read-part` | 缓冲区或读路径失败，值为 IOReturn |
| `map` | 整盘 `base` 处与分区 0 处的内容不同 |
| 没有 `CloverPMGR r113` | 固件启动的不是这份 payload |

判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| 成功行 + 走到 `initproc failed to start` | attach 顺序对了，`scan()` 读到整盘媒体；GPT 发布、BSD 命名、根设备发现、`bdevvp`、偏移映射全部由 XNU 自己走通 | 把 `rd=` 指向这个 BSD 名做真正的根挂载 |
| 仍是 `Kernel data abort`，pc 在 `IOGUIDPartitionScheme` | attach 没生效，`getProvider()` 仍为空 | 核对 `attach()` 的 provider 是否为 scheme 将要使用的那个整盘对象 |
| 停在 `gpt-attach` | 对象拒绝 attach 到整盘 | 查整盘 `IOMedia` 是否已 `registerService` 且在同一 plane |
| 停在 `gpt-probe`，`score` 为 0 | scheme 能拿到媒体但没接受整盘（GPT 读不到或 `isFormatted` 不成立） | 用已知 LBA 核对整盘媒体大小与读路径 |
| `gpt-target <n>`，`n>0` | scheme 能读表，但没有 39 号分区 | 核对读到的是哪一份 GPT |
| `bsd-props` / `root-match 0` / `bdevvp <err>` | 命名或 `bdevsw` 登记失败 | 按 step 名回看 `IOMediaBSDClient::start` |
| `map` | 分区偏移映射不一致 | 查 `getBase()` 与 PIO 的 LBA 换算 |
| 停在更早的步骤 | 存储栈出现回归 | 按 step 名回看该阶段 |

产物（内核与 payload 变了；root / trust cache / boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round113-payload.img` | 58720256 B | `904235f7fe323ac5e6d463aa61033cd2a7c755a7cabed5b8884f2db4a0960a50` |
| `round113-kernel.macho`（构建产物） | 51974144 B | `2d6587c1356d182c2eaae6f7759072045e7a0743e2d44a4c63f847dcf0382d00` |
| `round113-in-volume-kernel.macho`（卷内副本） | 51974144 B | `e4472e8aca238436358c29971b1b974177dac30197304247f9ddfa092f9d5165` |
| `round113-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round113-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round113-kernel.macho.adt` | 13476 B | `35b5b94ec009db29c8e4c2aa076ad0fd6b15ed2ef95afccf45b4e61f0feec788` |
| `round113-unixnu.toml` | 567 B | `a90aca6f64ad7b692bfbec586f7ab5c69d5c312ec38ea73e4ad4c645f06ac05d` |

校验器：`check-clover.py` 通过（ADT 13476 字节、8 核、splash `0x9d400000-0x9f800000`、
FD `0x9fc00000-0x9ff00000`、`clover_defconfig`、`CLOVERESP`、模块可见）。卷内 boot-args
末尾仍是 `… clover-pmgr=2 clover-emmc=21 armcpu-clock-panic=60000`。两条 r113 字符串都在
卷内内核里：`CloverPMGR r113: args [%s]`、`CloverBlock r113: p=%u %s %u:%u bd=1 map=1`。

内核符号：`_sheng_pmgr_run` `0xfffffff007971274`、`_clover_emmc_block_write`
`0xfffffff007971a30`、`_clover_emmc_run` `0xfffffff00797231c`、`_clover_block_run`
`0xfffffff007992054`。boot 仍是 `round87-boot.img`（`196b284f…`）。

```sh
fastboot flash cache ~/clover-out/round113-payload.img
fastboot reboot
```

**一次刷机只回答一个问题：补上 attach 之后，`IOGUIDPartitionScheme::scan()` 能不能看到整盘。**
看到成功行，根设备这条链就由 XNU 自己闭合了；下一轮才把 `rd=` 从 `md0` 换成这个 BSD 名。

## v0.2.86（round114）：把块栈挪到 vfsinit 之后

round113 的照片已经越过了 `Kernel data abort`，停在 `CloverBlock r113: bsd-props 0`：GPT 认出来了、
分区发布了、整盘和目标分区上的 `IOMediaBSDClient` 都 `start()` 成功了，但目标分区上读不到
`BSD Name` / `BSD Major` / `BSD Minor`。

原因不在块栈，而在它跑得太早。`bsd_init()` 里 `sheng_pmgr_run()` 在 `vfsinit()` 之前（本树
`bsd_init.c:779` 对 `:791`），而 `vfsinit()` 里的 `devfs_init()` → `devfs_sinit()` 是唯一把
`devfs_ready` 置 1 的地方（`devfs_tree.c:163`、`:218`）。在那之前：

- `devfs_make_node()` 打一行 `devfs_make_node: not ready for devices!` 就返回 NULL
  （`devfs_tree.c:1622`）。
- `MinorTable::insert()` 拿到空的 bdev/cdev 节点就返回 `kInvalidMinorID`
  （`IOMediaBSDClient.cpp:3581-3605`），`createNodes()` 跳到 `createNodesErr`，那四行
  `media->setProperty("BSD Name" / "BSD Unit" / "BSD Major" / "BSD Minor")`（`:595-598`）
  根本没执行。
- 但 `IOMediaBSDClient::start()` 不看 `createNodes()` 的返回值（`:351`），照样
  `registerService()` 并 `return true`（`:362-364`）。所以 `bsd-whole` / `bsd-root` 都过了，
  直到自己读属性才暴露。

`bdevvp()` 也有同一前提：它要的是 `vfsinit()` 里 `vntblinit()` / `nchinit()` 建好的 vnode 表。
所以整个块栈必须整体后移，而不是只补某一步。

round114 只换触发点：

1. `clover-emmc=21` 仍由 `sheng_pmgr_run()` 走到 CloverBlock，但只调
   `clover_block_request()`：用 `IOService::resourceMatching("IOBSD")` 注册一个一次性的
   `gIOPublishNotification`。
2. `IOKitBSDInit()` 在 `bsd_autoconf()` 末尾发布 `IOBSD` 资源（`IOKitBSDInit.cpp:110`），此时
   `vfsinit()` 已完成、`setconf()` / `vfs_mountroot()` 还没开始。发布走
   `gIOResources->registerService()` → `startMatching(0)`，匹配通知在 IOKit 配置线程上派发。
3. 处理器只做一件事：调一次 `clover_block_run()`，然后返回 false 把自己摘掉。round113 的整条
   链路（attach → probe → start → 子节点 → 两个 BSD 客户端 → 属性 → `IOBSDNameMatching` +
   `waitForService` → `bdevvp` → `base` 比较）一行没改。

为什么用通知而不是自己起线程或延时：`bsd_autoconf()` 之后没有别的设备侧插入点——device
`patches/xnu` 会遮住基础仓库的整批补丁，`bsd_init.c` 本身也动不了；而资源发布点由
`IOKitBSDInit()` 保证在 `vfsinit()` 之后。自己 `kernel_thread_start` 加延时是竞态，不能当作
「已经初始化」的证据。

boot 仍是 `rd=md0`，不写任何分区。成功时是一行，后面照旧继续引导：

```text
CloverPMGR r114: args [-v serial=3 debug=0x14e kt_fixed_zones=120 boot-progress=2
 rd=md0 allow_legacy_mach_msg=1 armcpu-timer-intid=19 clover-pmgr=2 clover-emmc=21
 armcpu-clock-panic=60000]
...
CloverBlock r114: post-vfsinit p=<n> disk0s39 <maj>:<min> bd=1 map=1
...
initproc failed to start ...
```

注意这行现在由 IOKit 配置线程打印，不再和 boot 线程同步：它可能落在 `initproc failed to start`
之前或之后，也可能稍晚才出现。判定时先看整屏有没有 `CloverBlock r114`，不要拿它和 `initproc`
的先后当证据。

失败一律 panic，一行一段，格式 `CloverBlock r114: <step> <value>`：

| 停在哪 | 含义 |
| --- | --- |
| `notify 0` / `notify 1` | `resourceMatching("IOBSD")` 或 `addMatchingNotification` 返回空，通知没装上；此后不会再有 CloverBlock 行 |
| `ready` / `storage` / `abi` | PIO 未就绪、`IOStorageFamily` 未加载、类大小不是 0x90 |
| `modload` / `postload` | 自己的 metaclass 没注册上 |
| `device` / `attach` / `driver` / `media` | 设备、驱动或整盘 IOMedia 没起来 |
| `gpt` / `gpt-attach` | scheme 没分配到；或 attach 到整盘失败 |
| `gpt-probe <score>` / `gpt-start` | 已 attach 但没认出 GPT、或没启动 |
| `gpt-children` / `gpt-target <n>` | 没有子节点；或 `n` 个分区里没有 `Partition ID 39` |
| `bsd-whole` / `bsd-root` | BSD 客户端在整盘或目标分区上没启动 |
| `bsd-props` | 目标分区仍缺 `BSD Name` / `BSD Major` / `BSD Minor` |
| `root-match <ptr>` | `IOBSDNameMatching` + `waitForService` 没返回目标分区 |
| `bdevvp <err>` | `dev_t` 打不开，返回 XNU 的 errno |
| `buffer` / `open-whole` / `read-whole` / `open-part` / `read-part` | 缓冲区或读路径失败，值为 IOReturn |
| `map` | 整盘 `base` 处与分区 0 处的内容不同 |
| 没有 `CloverPMGR r114` | 固件启动的不是这份 payload |
| 有 `CloverPMGR r114`、没有 `CloverBlock r114`、也没有 panic | `IOBSD` 发布时通知没派发（资源未发布或 IOKit 配置线程没跑起来） |

判定：

| 看到 | 结论 | 下一步 |
| --- | --- | --- |
| 成功行 + 走到 `initproc failed to start` | vfsinit 就是 round113 的缺口：devfs 节点、BSD 属性、`bdevvp`、偏移映射一次走通 | 把 `rd=` 从 `md0` 换成 `disk0s39`，去问根挂载本身 |
| 仍是 `bsd-props 0` | 通知确实晚于 vfsinit，但节点还是没建成 | 把 `devfs_ready` 与 `createNodes()` 的返回值一起打进 panic，查 `getWholeMedia` / anchor |
| `notify 0` / `notify 1` | IOBSD 匹配字典或通知没装上 | 核对 `resourceMatching` 的符号导出与 `gIOPublishNotification` |
| 只有 `CloverPMGR r114`，没有任何 CloverBlock 行 | 通知从未触发 | 查 `gIOResources` 是否真的被 `registerService`，以及配置线程有没有起来 |
| 仍是 `Kernel data abort`，pc 在 `IOGUIDPartitionScheme` | attach 顺序回归 | 核对 `attach(whole)` 用的整盘对象 |
| 停在 `gpt-*` / `bsd-whole` / `root-match` / `bdevvp` / `map` | 说明已经跑到 vfsinit 之后，但后面的老链路出问题 | 按 step 名回看 round113 的对应阶段 |
| 停在更早的步骤 | 存储栈出现回归 | 按 step 名回看该阶段 |

产物（内核与 payload 变了；root / trust cache / boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round114-payload.img` | 58720256 B | `7d7a3c1212687a0a56625bc45df5b5082c1e2783ef11068e1a87dccaa8372c84` |
| `round114-kernel.macho`（构建产物） | 51974144 B | `87dbf082d5b8bb55c76f6998b580229b8ea86895d5af3f470f3af52eea7bbd00` |
| `round114-in-volume-kernel.macho`（卷内副本） | 51974144 B | `ba142946c1fcd41939342a2f9c6154a125e7425a16949443d544dc680fbc0adf` |
| `round114-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round114-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round114-kernel.macho.adt` | 13476 B | `60a323bdb922adf86db9a97c17b6c49a00cca239f03ad739f7407701fd972fbb` |
| `round114-unixnu.toml` | 561 B | `ed168370cb15cf36ce4ac858834e5a850c8973ecd041c081ac6d88f7250b996b` |

校验器：`check-clover.py` 通过（ADT 13476 字节、8 核、splash `0x9d400000-0x9f800000`、
FD `0x9fc00000-0x9ff00000`、`clover_defconfig`、`CLOVERESP`、模块可见）。卷内 boot-args
末尾仍是 `… clover-pmgr=2 clover-emmc=21 armcpu-clock-panic=60000`。r114 的字符串都在
卷内内核里：`CloverPMGR r114: args [%s]`、`CloverBlock r114: post-vfsinit p=%u %s %u:%u bd=1 map=1`，
panic 行末尾照旧带 `@CloverBlock.cpp:<line>`。`CloverBlock` 的导入与 vtable 审计仍然通过，
`clover_block_request` 是这次新增的唯一导出口。

内核符号：`_sheng_pmgr_run` `0xfffffff007971274`、`_clover_emmc_run`
`0xfffffff00797231c`、`_clover_block_run` `0xfffffff007992054`、`_clover_block_request`
`0xfffffff00799296c`。boot 仍是 `round87-boot.img`（`196b284f…`）。

```sh
fastboot flash cache ~/clover-out/round114-payload.img
fastboot reboot
```

**一次刷机只回答一个问题：把块栈放到 `vfsinit()` 之后，BSD 命名、属性和 `bdevvp` 能不能一次走完。**
看到成功行，`rd=md0` 就可以换成 `disk0s39`，下一轮问的是根挂载本身。

## v0.2.87（round115）：把 bsd-props 拆成掩码

round114 的照片仍是 `CloverBlock r114: bsd-props 0`。这行本身就否掉了上一轮的结论：

- 屏幕上出现了 CloverBlock 行，说明 `IOBSD` 通知确实触发了，处理器跑在 IOKit 配置线程上。
- 链路走到了最后一步，说明 GPT 解析、39 号分区查找、整盘与目标分区上两个
  `IOMediaBSDClient::start()` 都返回了 true。

也就是说「放到 `vfsinit()` 之后」不是全部原因。`start()` 把 `createNodes()` 的失败整个吞掉了
（`IOMediaBSDClient.cpp:351` 不看返回值，`:362-364` 照样 `registerService()` 并返回 true），
剩下的可能都长成同一行：

- 整盘那一步就失败：`getWholeMedia()` 走不到 `isWhole()`，或 `whole->getProvider()` 为空，
  或 `AnchorTable` / `MinorTable` 插入失败；
- 只在分区那一步失败：`getWholeMedia()` 路过某个中间 `IOMedia` 时 `getLocation()` 为空而返回
  NULL，或 `slicePath` 不是 `s39`，或 minor 表插入失败；
- devfs 在这个时刻仍不可用，或客户端要用的名字已经被别人占了——`devfs_make_node()` 返回 NULL
  同样只表现为缺少属性。

round115 不改流程，只改这一行的内容：在同一次运行里读两个媒体对象的属性，得到一个 19 位掩码；
再用 `devfs_make_node()` / `devfs_remove()` 探一个私有名字和客户端本来会用的四个名字。掩码以
十六进制打印，因为 19 位十进制在照片上读不出来。

```text
CloverPMGR r115: args [-v serial=3 debug=0x14e kt_fixed_zones=120 boot-progress=2
 rd=md0 allow_legacy_mach_msg=1 armcpu-timer-intid=19 clover-pmgr=2 clover-emmc=21
 armcpu-clock-panic=60000]
...
CloverBlock r115: post-vfsinit p=<n> disk0s39 <maj>:<min> bd=1 map=1
```

失败仍是 panic 一行：`CloverBlock r115: bsd-props 0x<mask>`；其它 step 的格式不变。

掩码位（`0x` 后从低位读）：

| 位 | 值 | 含义 |
| --- | --- | --- |
| 0 | `0x00001` | 整盘媒体有 `BSD Name` |
| 1 | `0x00002` | 整盘媒体有 `BSD Major` |
| 2 | `0x00004` | 整盘媒体有 `BSD Minor` |
| 3 | `0x00008` | 目标分区有 `BSD Name` |
| 4 | `0x00010` | 目标分区有 `BSD Major` |
| 5 | `0x00020` | 目标分区有 `BSD Minor` |
| 6 | `0x00040` | 目标分区 `getLocation()` 正好是 `"39"` |
| 7 | `0x00080` | 目标分区有 provider |
| 8 | `0x00100` | 整盘媒体有 provider |
| 9 | `0x00200` | 整盘媒体 `isWhole()` 为真 |
| 10 | `0x00400` | 目标分区 `isWhole()` 为真（正常必须为 0） |
| 11 | `0x00800` | 私有名 `cloverbdprobe` 被 devfs 接受：devfs 就绪且可写 |
| 12 | `0x01000` | 探针之后 `disk0` 仍空闲（即没有这个节点） |
| 13 | `0x02000` | `disk1` 仍空闲 |
| 14 | `0x04000` | `disk0s39` 仍空闲 |
| 15 | `0x08000` | `disk1s39` 仍空闲 |
| 16 | `0x10000` | 手动启动整盘客户端之前，整盘就已有 `BSD Name` |
| 17 | `0x20000` | 手动启动分区客户端之前，分区就已有 `BSD Name` |
| 18 | `0x40000` | 目标分区 `getBase()` 非 0 |

名字探针只说明「这个名字此刻能不能建」，建完立刻 `devfs_remove()`，不改动任何已有节点。
位 12–15 为 1 表示名字空闲；为 0 表示已经存在同名节点（`dev_add_name` 的 `EEXIST`），
这正好回答「是不是自动匹配的 BSD 客户端先占了名字」。

判定：

| 掩码 | 结论 | 下一步 |
| --- | --- | --- |
| 位 11 为 0 | devfs 在此刻仍不可用，位 12–15 一并无意义 | 说明通知在 `vfsinit()` 之前就发了，或 `devfs_init()` 没跑到；改为核对通知来源，而不是继续动 `IOMediaBSDClient` |
| 位 11 为 1，位 0–2 为 0 | 整盘的 `createNodes()` 失败：问题在锚点/整盘定位 | 看位 8（provider）、位 9（`isWhole`）、位 12/13（名字是否被占） |
| 位 0–2 为 1，位 3–5 为 0 | 整盘已命名，只有 39 号分区失败 | 看位 6（location）、位 7（provider）、位 10（被误判 whole）、位 18（base）、位 14/15（名字） |
| 位 3–5 为 1 | 属性其实都在，失败来自类型转换 | 查 `BSD Name/Major/Minor` 是否被写成非 `OSString`/`OSNumber` |
| 位 16 / 17 为 1 | IOKit 自动匹配已经给媒体起过 BSD 客户端 | 手动再起一次只会撞名；改为直接读已有名字继续 |
| 位 18 为 0 | 分区 `base` 是 0，偏移映射不可能成立 | 查 `IOGUIDPartitionScheme::scan` 是否给媒体设了起始块 |

产物（内核与 payload 变了；root / trust cache / boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round115-payload.img` | 58720256 B | `8a215e2533e775987fa73655134cea6d011b1d1286e14e5e2c6cd0f2658ce7ee` |
| `round115-kernel.macho`（构建产物） | 51974144 B | `a7978a14053e0c7a018705f1077a541d479c1da45e04c3754f08e464afc037fe` |
| `round115-in-volume-kernel.macho`（卷内副本） | 51974144 B | `6cbece2ce054fb6ee538e6f046c7a4d73db7db5c214af85cbfadd545e9a51d00` |
| `round115-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round115-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round115-kernel.macho.adt` | 13476 B | `81a95fe1a9dda238778356fafe829fca12aba59554ca19e9d9b09aa012bbd1bd` |
| `round115-unixnu.toml` | 556 B | `9cc24899eaca80856244483463cee6ba1a086d6286c77d3358c2bfb3d49aa387` |

校验器：`check-clover.py` 通过（ADT 13476 字节、8 核、splash `0x9d400000-0x9f800000`、
FD `0x9fc00000-0x9ff00000`、`clover_defconfig`、`CLOVERESP`、模块可见）。卷内 boot-args
末尾仍是 `… clover-pmgr=2 clover-emmc=21 armcpu-clock-panic=60000`。两条 r115 字符串都在
卷内内核里：`CloverPMGR r115: args [%s]`、`CloverBlock r115: post-vfsinit p=%u %s %u:%u bd=1 map=1`。
`CloverBlock` 的导入与 vtable 审计仍通过（190 个槽、26 个覆写）；本轮新增的导入只有
`_devfs_make_node` 与 `_devfs_remove`，两者都是内核自身导出的符号。

内核符号：`_sheng_pmgr_run` `0xfffffff007971274`、`_clover_emmc_run`
`0xfffffff00797231c`、`_clover_block_run` `0xfffffff007992054`、`_clover_block_request`
`0xfffffff007992d60`、`_devfs_make_node` `0xfffffff0074a5e58`、`_devfs_remove`
`0xfffffff0074a4e24`。boot 仍是 `round87-boot.img`（`196b284f…`）。

```sh
fastboot flash cache ~/clover-out/round115-payload.img
fastboot reboot
```

**一次刷机只回答一个问题：`bsd-props` 的失败发生在整盘、分区，还是 devfs 本身。**
掩码会指出唯一的一段，下一轮只改那一段；之后才把 `rd=` 换成 `disk0s39`。

## 有意省略的部分

- **内核里没有现成的块设备驱动。** XNU 有 UFS 的 `NativeUfs`，但没有 eMMC/SDHCI 的，所以
  sheng 那条"直接从 userdata 读 APFS 卷"的路走不通。clover 走 gta4l 的形状：
  控制台优先的内核，带 `CONFIG_XNU_SOURCE_MOCKFS`，没有
  `CONFIG_XNU_NATIVE_UFS`，`[userdata]` 留在板级文件里。
  `v0.2.58` 起的 `drivers/CloverSdhci` 先把这条缺口量清楚：它先是一份只读的寄存器
  探测，`v0.2.60` 起才发第一颗命令（CMD13 `SEND_STATUS`），`v0.2.61` 起在 15 个 RCA 上
  逐个问同一颗命令，`v0.2.62` 起改成从 CMD0 重头识别（CMD0/CMD1/CMD2/CMD3，降低时钟），
  `v0.2.63` 起把卡时钟改到 GCC 的 `sdcc1_apps_clk_src`（400 kHz）再重做整套识别，
  `v0.2.64` 起先停卡时钟、只复位命令/数据状态机（`RESET_ALL` 会把主机卡住），
  `v0.2.65` 起把 CMD2 发两遍（一遍带 CRC、一遍关掉 CRC，中间 CMD0+CMD1 重武装），
  `v0.2.66` 起把 CRC 开关与重武装的先后对调（先关 CRC 的首槽位，再带 CRC 的重武装后槽位，
  并补上 CMD9/CMD13），`v0.2.67` 起不再走带校验的 CMD2，改用 CMD2(关校验)/CMD3/CMD13/CMD9/CMD7
  把卡第一次送进 transfer，`v0.2.68` 起发第一颗数据命令（CMD16 设块长 + CMD17 PIO 单块读
  byte 512，前五个字节应是 `EFI PART`），`v0.2.69` 起按 Linux 的 MSM 路径给单块传输置块计数
  使能、并按 OCR bit 30 改用扇区号，`v0.2.70` 起用 `CNTVCT_EL0` 把 FIFO 读速降到卡的节奏以下
  （round97 的 128 次读全是同一个字：主机比卡快了一个数量级），并把 GPT 头的四个字段解出来，
  `v0.2.71` 起在传输前清掉 `VENDOR_SPEC3` 的备用 FIFO 选择位（round98 证明节奏已经对了，
  是端口本身不前进），`v0.2.72` 起按 GPT 头的 `el/en/es` 逐块读表项数组、按 UTF-16LE 名字
  找 `logfs` 并报出它的序号与首末 LBA，`v0.2.73` 起把名字比较改成 UTF-16 整字
  （round100 的 `lg i=0` 是常量把高半字节挤掉，不是读失败），并在没命中时打出一行
  首项标定 `e0` 来区分「编码错」与「确实没有」（round101 命中
  `lg i=39 a=12320768 b=12337151`，与 sysfs 的 `mmcblk0p39` 独立吻合），
  `v0.2.74` 起按这个坐标往 logfs 首块写一块自证载荷（CMD24 + 同节奏 FIFO +
  CMD13 等 `READY_FOR_DATA`）再读回逐字比，写前先读旧内容、写错位置则一个字不写
  （round102 读回 `cmp=128`、`sum` 与离线重算一致），`v0.2.75` 起把报告本身按固定
  两块的布局存进 logfs（头块 `UXNU`/`LOG1` + 长度 + 字节和，文本块前缀 + 正文）
  并逐块读回比对（round103 读回 `n=343`、`hc=128 tc=343`，和 `0x59b2` 与离线重算
  一致），`v0.2.76` 起只读不写，下一次启动从 logfs 取回上一轮存下的记录、
  校验 magic/长度/字节和并打出前 48 字节（round104 读回 `c=1`，`bt` 正是上一轮
  报告的第一行），用来证明它跨启动存活，`v0.2.77` 起把 logfs 开头当成 16 个固定
  两块格，扫描到第一格没有 magic 就写那一格（头块多一个格号字），不再覆盖格 0，
  读完逐字/逐字节比对新记录，`v0.2.78` 起 panic 屏把自己画出的前 480 字节
  交给驱动导出的 `clover_emmc_panic_append`，追加成下一格 kind 20 记录，下一次
  启动由 mode 20 读回最新一格、校验并武装供再下一次追加使用，`v0.2.79` 起 mode 21
  通过 `drivers/CloverBlock` 发布 `IOBlockStorageDevice`，GPT、表项数组和 logfs
  记录改由 `IOMedia` 读回，`v0.2.80` 起 PMGR 在切换前打印本次收到的完整命令行、
  缺 `clover-emmc` 改为 panic、块栈每个阶段先打一行标记（见上一节）。这些轮次量的是
  「固件留下的 SDHCI 窗口活不活、
  卡还答不答、在哪个地址答、时钟是不是还在移命令、数据阶段能不能自己走完」。
  根设备因此仍然不走磁盘：`v0.2.35` 用
  `rd=md0` 把 payload 里的 initrd 变成内存盘，
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
