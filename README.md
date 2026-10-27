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
| 移植记录 | `porting.toml`（`schema_version = 1`）按上游要求的 13 个 area 记录覆盖与证据；`.github/workflows/porting-record.yml` 调用 UniXNU 的 `porting-record.yml@main` 跑同一份校验；`AGENTS.md` 写明仓库边界与当前活跃的 area |
| ADT | `scripts/make-clover-adt.py` 为 8 个 Kryo 260 核生成 13,880 字节；检查器会把编码读回来，并断言 `ArmDTPlatform` 和 DarwinBoot 需要的节点 |
| 内核 | 已构建：`DEVELOPMENT_ARM64_BCM2837` 的 51.8 MB `kernel.macho` |
| payload / boot.img | 已构建：56 MiB Fat32 payload 卷，以及 kernel 段就是固件的 boot 镜像；payload **不**走 ramdisk（见下文）。`v0.2.13` 起 payload 里的 `kernel.macho` 是一份打了探针的副本：入口色带、panic 屏幕，以及 `arm_init` 某个调用返回后触发的定时复位；`v0.2.16` 起在开 MMU 前调用一个 mapper 洞穴，把 framebuffer 恒等映射进 TTBR0（与窗口同槽就复用 L2 表，否则用窗口最后一页新建一张），并预先填好 `_vinfo`；mapper 失败会涂一条白色诊断带；`v0.2.17` 改成用自身 PC 定位引导页表（`boot_args.physBase` 落在 `__PRELINK_TEXT` 上、本身没有被映射），并在 MMU 打开后立刻涂第 4 条青色带；`v0.2.18` 再加一条独立的橙色带（第 12 条），在 `arm_init` 的第一条调用处直接从 `boot_args` 涂，不依赖栈、AT 和 `_vinfo`；`v0.2.19` 修掉种子守卫写反的 bug（`cbz x8` 在 mapper 成功时跳过种子，`_vinfo` 因此一直是 0，黄带从 `v0.2.15` 起从未画出来过），并在第一条调用返回后再涂一条红带（第 13 条）；`v0.2.20` 修掉 ADT 里 `cluster-type` 的尺寸契约（字符串改为 4 字节整数 `'E'` / `'P'`）；`v0.2.21` 又因为这份内核只支持单簇而移除该属性，让 8 个核落在一个 `CLUSTER_TYPE_SMP` 平坦簇里；`v0.2.22` 关掉 `CONFIG_XNU_NATIVE_PAN` 并重建内核（Kryo 260 是 ARMv8.0，没有 PAN），这是本轮第一次改内核构建配置，探针脚本不变；`v0.2.23` 换回定时信号：`arm_vm_init` 返回后 10 秒复位，panic 屏幕在 framebuffer 不再翻译时 3 秒复位；`v0.2.24` 把复位点移到第二次 `PE_init_platform(TRUE)` 返回后 15 秒，这一段跨过 `serial_init`、`kernel_startup_initialize_upto(KPRINTF)` 和 `PE_create_console`，后者会重新映射 framebuffer，成功的话 panic 屏幕又能画字（见"菜单之后看什么"）；`v0.2.25` 修掉 `PE_create_console` 开头内联的 panic-log 探测（这份 BCM2837 内核读树莓派地址 `0x3c0fc000`，在 clover 上是外部异常），复位点不变；`v0.2.26` 把 `arm_init` 后段五个调用各接一个涂色洞穴（第 6–10 条带），复位点移到 `_machine_startup` 调用处，复位洞穴先涂第 11 条红带再等 15 秒；`v0.2.27` 把刻度带移到第 10–14 条、控制带移到第 15 条（都在 panic 屏不清的下半屏），并让 panic 屏印出 RA/FP/LR、fmt/args 和 va_list 前后各槽，用于从照片定位断言调用点；`v0.2.28` 修掉刻度洞穴的 4 字节对齐缺陷（第 14 条洞穴整体左移、`bl` 落到 `_fiq_context_init - 4`，`v0.2.27` 的白字是探针自己的误报），并在打洞时校验每条 `_start` 与每个 `bl` 目标；`v0.2.29` 把检查点从 `_machine_startup` 调用前推进到交接之后：第 15 带表示进入 `_machine_startup`，随后 9/8/7/6 带依次覆盖 `_machine_conf` 返回、`_kernel_bootstrap` 入口、首条 `printf` 返回和 `vm_mem_bootstrap` 调用点，复位也移到最后一档；`v0.2.30` 把第 10–14 条带从 `arm_init` 改标 `vm_mem_bootstrap` 里的五个调用（`vm_page_bootstrap`、`zone_bootstrap`、`vm_object_bootstrap`、`vm_map_init`、`kmem_init`），第 5 条红带表示 `kmem_init` 返回、随后 15 秒复位；`v0.2.31` 把已经证明返回的五个调用摘掉，第 5 条红带保留为前半段的收尾，第 10–14 条带改标后段（`kernel_startup_initialize_upto(KMEM)`、`vm_fault_init`、`kernel_startup_initialize_upto(ZALLOC)`、最后那条 `kprintf` 的进入与返回），返回后 15 秒复位；`v0.2.32` 在 `vm_mem_bootstrap` 返回后直接调用 `_vcattach`，第 15 条带从红改涂绿表示调用返回，控制台接上后屏幕交给内核文字，本轮不再定时复位；`v0.2.33` 在 ADT 的 `/chosen` 发布 `amfi-only-platform-code = <1>`（AMFI 启动的前置），并让控制台洞穴先调 `initialize_screen(NULL, kPETextMode)` 再调 `_vcattach`（单独 `vcattach` 在本机是空操作：`PE_create_console` 选了图形模式，`gc_graphics_boot` 为真）；`v0.2.34` 不再画带，改为在 `_bsd_init` 的九个无条件调用（`vfsinit`、自动配置前的初始化块、`kminit`、`IOKitBSDInit`、`loopattach`、`ether_family_init`、`net_init_run`、`vfs_mountroot`、`devfs_kernel_mount`）返回后各印一行 `[clover] … returned`，最后一行标记直接指出没返回的调用；`v0.2.35` 起 payload 多了两个文件，`Payloads/scratch-data.img`（签过名的静态 EL0 init，DarwinBoot 把它放进窗口并填 `/chosen/memory-map/RAMDisk`，内核当 `md0` 挂成 mockfs 根）和 `Payloads/kernel.macho.trustcache`（那颗 cdhash 的离线信任），`unixnu.toml` 里因此有 `initrd` 和 `rd=md0`，ADT 只有一个 `RAMDisk` 槽、不再有 `PackedRAMDisk`；`v0.2.36` 只换根镜像里的 init 程序（它自己 `open("/dev/console")`，退出码按「阶段 × 32 + errno」编码），`kernel.macho` 与探针逐字节不变 ；`v0.2.37` 把退出改成失败通道：四步全通就把 banner 连写 240 遍（两整屏）并停在 EL0，让活控制台当读数，只有失败才 `exit(阶段 × 32 + errno)` ；`v0.2.38` 花掉洪水、改问多进程：banner 一遍后 `fork`，子进程印自己的 pid/ppid 后 `exit(7)`，父进程 `wait4` 后印 reaped pid 与原始 status 再停住；`v0.2.39` 仍只换那颗根镜像：父进程 `fork` 一返回就印 `forked pid=`，子进程第一条指令先印 `alive`，把 round68「banner 之后什么都没有」的三义性切成三个独立读数；`v0.2.40` 仍只换那颗根镜像：按 Darwin arm64 的 fork ABI 用 `x1` 分流（孩子拿到 `x0=自己 pid, x1=1`），两边各印自己拿到的 `x0/x1`，`_fail` 从 `ret` 改成直接 `exit`，失败不再可能变成静默自旋；`v0.2.41` 仍只换这颗根镜像：只有 pid 1 才 fork，孩子印完自己的行就 `execve("/sbin/launchd", argv, envp)`（syscall 59，`syscalls.master` 抄的；`argv`/`envp` 运行时填进 `__DATA`，因为 `-static` PIE 的数据段放不了绝对地址），exec 成功就等于屏幕上出现第二份这个程序、且它的 `console fd=1`；`v0.2.42` 仍只换这颗根镜像：exec 出来的那份副本改发 mach trap（x16 = 表下标的相反数：`thread_self` -27、`task_self` -28、`host_self` -29、`mach_reply_port` -26、`mach_msg` -31；返回只写 x0、不碰 carry），三个 self 端口 + 一个 reply 端口 + 一条 24 字节消息自己发给自己再收回来，跑完 `exit(11)`；`v0.2.43` 仍只换这颗根镜像：mach 探针在 send 前后、receive 前后各加一次出声打印，并把消息缓冲区的地址也印出来，把 round72「`send=` 那行没出现」这一张图能做的两种解释切分开；`v0.2.44` 仍只换这颗根镜像：消息改走 `mach_msg2_trap`（x16 = -47，八个打包参数，发送腿带 `MACH64_SEND_MQ_CALL`），缓冲区指针从 x7 挪到 x20（x7 现在是 msg2 的 timeout 实参），末尾再用 legacy `mach_msg` 重放一次作为 boot-arg 对照；`v0.2.45` 仍只换这颗根镜像：孩子那一路一条指令不改，父亲把阻塞的 `wait4` 换成 `WNOHANG` 轮询、每轮烧几十毫秒计数并印一行，用来把「孩子挂在里面」「孩子已死但父亲被堵在悼词写」「tty 写路径自己死了」这三种共用一张照片的情况切开；`v0.2.46` 仍只换这颗根镜像，且孩子那一路一条指令不改：父亲把轮询期间的打印**全部拿掉**（一个字符都不写），200 轮之后用 `exit(384)` 报超时，因为 panic 屏是直写 framebuffer 的、不经过 `vcputc`，是这台板子上唯一不受被测控制台影响的通道 |
| 固件 | 由 `uefi-clover` 构建（3 MiB 固件卷，内含 UniXNU 与 DarwinBoot）；`v0.3.10` 起 DXE 尾部完整派发，`CloverGopDxe` 发布 GOP |
| 设备事实 | 2026-10-22 采集过一次，位于 `facts/81ffcaaa-20261022-210928/` |
| 真机启动 | **菜单已到**（Stage 1）：`v0.3.10` 固件在屏幕上渲染 UniXNU 菜单并读到了 payload；菜单 3 秒后默认进 XNU。`v0.2.9` 实测屏幕出现**绿 + 蓝 + 品红**三条色带，即 DarwinBoot 已交接、XNU 入口已执行、引导页表已建好；`v0.2.10` 实测**品红出现约 6 秒后设备自己重启**，即 `sctlr_el1` 打开 MMU 后没有立刻死掉；`v0.2.11` 的 `_arm_vm_init` 复位点没有触发（**品红一直停着**），说明还没走到它；`v0.2.12` 实测**只有绿 + 蓝 + 品红、约 6 秒后自复位**，即 `_PE_init_platform` 返回了、开 MMU 后写屏确实不可用；`v0.2.13` 的 `_thread_bootstrap` 复位点实测是**品红后十几秒才复位、没有白字**，符合"panic 屏幕在 `vinfo` 仍为 0 时走了 12 秒兜底"；`v0.2.14` 扩映射并预填 `_vinfo`、不再主动复位，实测**仍是绿 + 蓝 + 品红，无黄带无白字**，由此定位到窗口在 framebuffer 之上、向上扩 `memSize` 永远够不到；`v0.2.15` 改为直接补页表项，实测**仍是绿 + 蓝 + 品红、无黄无白**，原因是 1 GiB 窗口只能落在高 DRAM bank，与 framebuffer 不在同一个 1 GB 槽，旧代码查错了槽位就退出；`v0.2.16` 换成独立 mapper 洞穴（同槽复用、异槽新建 L2 表），失败时用白色带自报错误码，实测**绿 + 蓝 + 品红 + 第 9 条白带**，即 mapper 走到自检第 4 步：窗口的第一个 L2 项不是 2 MB 块；由此查明 `boot_args.physBase` 锚在 `__PRELINK_TEXT` 上、本身不在引导映射内；`v0.2.17` 改为从 mapper 自己的 PC 反推引导映射，修掉复用 L2 表时未掩掉低位的 bug，并在 MMU 打开后立刻涂第 4 条青色带，实测**绿 + 蓝 + 品红 + 青、无黄**，即 MMU 已开、framebuffer 已经可写，卡点在 `_cpu_data_init` 被调用之前；`v0.2.18` 增加第 12 条橙色带（`arm_init` 入口后的第一条调用，不依赖 vinfo/栈/AT），实测**绿 + 蓝 + 品红 + 青 + 橙、无黄**，确认 `arm_init` 已进入、第一条调用点已到达；由此回查代码发现种子守卫从 `v0.2.15` 起就是反的（`cbz x8` 在 mapper 成功时跳过种子），`_vinfo` 一直为 0，黄带从未有机会出现；`v0.2.19` 改成 `cbnz x8`，并让第一条调用返回后再涂第 13 条红带，实测读到**白字 panic 屏幕**：`CPU property 'cluster-type' has bad size 6 @machine_routines.c:936`，即种子/画屏/早段 `arm_init` 全通，内核已走到 `ml_parse_cpu_topology`；`v0.2.20` 把 `cluster-type` 从字符串改成 4 字节整数 `'E'` / `'P'`，实测接着触发 `machine_routines.c:1099` 的 `phys_cluster_id <= MAX_CPU_CLUSTER_PHY_ID` 断言——这份 BCM2837 内核只支持单簇，无法接受 E/P 双簇描述；`v0.2.21` 改为不发布 `cluster-type`，让 8 核按 `CLUSTER_TYPE_SMP` 建一个平坦簇，实测拓扑已通过，接着读到 `Undefined kernel instruction: pc=0xfffffff0079aff58 instr=d500419f`——`msr pan, #1`，Kryo 260 是 ARMv8.0，没有 PAN；`v0.2.22` 把 `CONFIG_XNU_NATIVE_PAN` 改成 `n` 并重建内核，实测七条带全部出现（绿 + 蓝 + 品红 + 黄 + 青 + 橙 + 红）、没有白字：内核进了 `arm_init`，两个早段探针都返回了，之后死在 `bl _arm_vm_init` 处或更晚，而且没有可见的 panic；`v0.2.23` 因此把信号换回定时复位，并让 panic 屏幕在 framebuffer 不可翻译时也能报出来，实测**色带出现后约 10 秒自复位**，即 `arm_vm_init` 返回了，卡点在它后面；`v0.2.24` 把复位点移到 `PE_create_console` 之后的第二次 `PE_init_platform(TRUE)`，实测**色带之后什么都没有**——既没有文字，也没有 15 秒复位，卡点在 `arm_vm_init` 返回之后、`PE_init_platform` 之前；`v0.2.25` 据此跳过 `PE_create_console` 开头读树莓派 panic log 地址的四条指令，实测**色带 + 约 15 秒复位、无文字**，即控制台与第二次 `PE_init_platform` 都已返回（无文字是 `gc_enabled` 仍为假所致，不是故障）；`v0.2.26` 给随后五个调用各涂一条带（6–10）并把复位点推到 `_machine_startup` 之前；实测**绿 + 蓝 + 白字**（`<1>:0 Assertion failed: <d>`、`TCR=0x…1a251a`、`pages=free=0`），即 `arm_init` 后段有断言 panic、发生在 VM 页初始化之前，但旧清屏把第 2 条以下的带全擦掉、参数又与已知冷段常量对不上；`v0.2.27` 因此改印 RA/FP/LR 与原始参数槽；其实机读数（绿 + 蓝 + 白字、`LR=0xfffffff0073f1dc4`、`a0=1 a1=0 a2=0xd`）指向的不是内核断言，而是第 14 条刻度洞穴自己：它被放在 `0x3ed784`（4 mod 8），`ld.lld` 把 `_start` 对齐到 `0x3ed788`，而 patcher 仍按 `0x3ed784` 写入，整段 `bl` 左移 4 字节落到 `_fiq_context_init - 4`（`_platform_syscall` 冷段里的 `bl _Assert`），`x0/x1/x2` 还留着 `arm_init` 传给 `_fiq_context_init` 的 `TRUE/0/0xd`；`v0.2.28` 修掉打洞对齐并逐条校验 `bl` 目标，第 10–13 条带则证明那四个调用确实都返回了；`v0.2.28` 上机读数是**10–14 全在 + 15 红 + 约 15 秒复位**，即 `_fiq_context_init` 也返回了、`arm_init` 已到 `_machine_startup` 调用点，交接之前没有任何卡死；`v0.2.29` 因此让检查点真正进入 `_machine_startup`，把刻度推进到 `kernel_bootstrap` 的 `vm_mem_bootstrap` 调用处；`v0.2.29` 上机读数是**整条梯子全亮 + 约 15 秒复位**（逐带取中位色：10 白、11 橙、12 绿、13 青、14 品红、15 红、6 白），即 `_machine_conf`、首条 `printf` 都返回了，`vm_mem_bootstrap` 已被调用，卡点在它内部；`v0.2.30` 因此把第 10–14 条带改成 `vm_mem_bootstrap` 内五个调用的探针、复位后移到 `kmem_init` 之后；`v0.2.30` 上机读数是**十六条带全在、含第 5 条红带、约 15 秒复位**，即 `vm_page_bootstrap` 到 `kmem_init` 五个调用全部返回、复位档也到达，卡点在 `vm_mem_bootstrap` 后段；`v0.2.31` 因此摘掉这五个已证调用，第 5 条红带保留为前半段收尾，第 10–14 条带改标后段四个调用；`v0.2.31` 上机读数是**13、14 都在 + 约 15 秒复位**，即最后那条 `kprintf` 返回、整个 `vm_mem_bootstrap` 返回，卡点在 `kernel_bootstrap` 更后面；`v0.2.32` 因此不再加刻度，而是在 `vm_mem_bootstrap` 返回后调用 `_vcattach` 接上视频控制台；`v0.2.32` 上机读数是一块**白字 panic**：`"AMFI: no amfi-only-platform-code device tree node!" @AppleMobileFileIntegrity.cpp:5025`，RA/LR 都落在预链接的 AMFI kext 里，说明内核根本没停在 `vm_mem_bootstrap` 之后，而是一路走到 AMFI；色带还在是因为 `vcattach` 被 `gc_graphics_boot == TRUE` 短路成了空操作；`v0.2.33` 因此补上 ADT 的 `amfi-only-platform-code` 属性，并先用 `kPETextMode` 清掉图形模式标志再 attach；`v0.2.33` 上机读数：**文字控制台已接上**，满屏内核日志（AMFI、Sandbox、apfs、AppleImage4 都走过），最后停在 NVRAM sync 失败那一行、**没有 panic**；但那一行可能来自后台 kext 线程，不能当作启动线程的卡点；`v0.2.34` 因此在 `_bsd_init` 的九个调用返回后各印一行 `[clover]` 标记；`v0.2.34` 上机读数是**九行标记全部印到 `[clover] net_init_run returned`，`vfs_mountroot returned` 没有**，最后一行是 `Waiting on <dict … IOProviderClass IOMedia … IOPropertyMatch … UUID …>`，即启动线程正常停在 `IOFindBSDRoot`，等的根 `IOMedia` 在这块没有驱动的 eMMC 板上永远不会被发布——不是卡死，是这条路不通；`v0.2.35` 因此把根换成 `rd=md0` + mockfs，并让 init 自己 `exit(0)`，用 `initproc exited` 这条 panic 证明 EL0 跑到了；`v0.2.35` 上机读数是 **`initproc failed to start -- no exit reason available -- (signal 0, exit status 0)`、没有 `CS_KILLED`**，也就是 pid 1 已经建好、已经跑进 EL0、四个 syscall 都返回、最后自己 `exit(0)`——Stage 6 达成（exec 真失败会报 `Process 1 exec of … failed, errno N`）。缺的那行 banner 不是 bug：这份 XNU 没有 `open_console()`，initproc 的 fd 0/1/2 是空的，`v0.2.36` 因此让 init 自己 `open("/dev/console")`；round66 实机读数 **`exit status 0`** = `getpid`/`sysctl`/`open`/`write` 四步全通，EL0 到控制台 tty 的 syscall 通道闭环，banner 没进照片是因为 panic 钩子清屏且 tty 输出不进 msgbuf；`v0.2.37` 因此成功不退出：banner 写 240 遍后停住，活控制台当读数；round67 实机读数**整屏 `[clover] el0 init: pid=1 console fd=0 pagesize=4096`、停住不复位** = tty 的字画上了面板，EL0→屏幕闭环（`fd=0` 证明描述符表空、`pagesize=4096` 证明用户栈可写）；`v0.2.38` 因此改问多进程：fork/子进程 exit(7)/父进程 wait4；round68 实机读数是**banner 之后什么都没有**——没有 `child:`、没有 `waited`、无 panic 无复位，而同屏的 `Added memory device md0`/`BSD root: md0` 与 `dli_init … scheduled at least once` 说明根挂上了、内核线程调度是好的，这一张图却有三种解释（fork 没返回 / 子进程没被调度 / 子进程第一次写没落地），`v0.2.39` 因此把 fork 点切成三个标记：父进程一返回就印 `forked pid=`、子进程第一条指令就印 `alive`；round69 实机读数是**两行一模一样的 `forked pid=2`**、没有 `alive`、没有 `waited`，也就是两个进程都跑了、都走了父分支——根因是 Darwin arm64 的 fork ABI：`thread_set_child`（`osfmk/arm64/status.c`）给孩子的是 `x0=自己的 pid, x1=1`，父亲是 `x0=同一个 pid, x1=0`（`kern_fork.c:576` 的 `retval[1] = 0`），把孩子的 x0 归零是 libSystem 的事，手写 `svc` 没有；而孩子随后 `wait4(-1)` 拿到 ECHILD、掉进 `_fail`，`_fail` 是 `b.cs` 进来的、结尾 `ret` 回到陈旧 x30，于是原地自旋一个字都不写——round68 的「只有 banner」是同一个错误的另一张照片。`v0.2.40` 因此按 `x1` 分流、两边都印出自己拿到的 `x0/x1`、并把 `_fail` 改成直接退出（失败必须出声）；round70 实机读数**五行齐**：`forked pid=2 x1=0`、`child: alive`、`child: pid=2 ppid=1 x0=2 x1=1`、`waited pid=2 status=1792`，即 fork 复制 pmap、次核调度孩子、孩子 CoW 栈可写、`exit(7)`、父亲 `wait4` 被唤醒收尸，整条多进程链在这块硅上闭合（`1792 = 7 << 8`）；`v0.2.41` 因此把孩子的 `exit(7)` 换成 `execve`，问下一面墙——一个进程能不能把自己换成另一个程序；round71 实机读到六行、末行 `[clover] el0 init: pid=2 console fd=1 pagesize=4096`，即 exec 成功、fd 0 跨 exec 保留（新副本 open 拿到 1）、父亲再无 `waited` 行——namei、mockfs 读、第二次 AMFI、换地址空间整条 exec 路径已通（v0.2.42 问 mach）；round72 实机读到 `[clover] el0 mach: thread=259 task=515 host=771`、`[clover] el0 mach: reply=1827`——x16 的负数约定是对的，四条 trap 都分发成功，而且这个 fork+exec 出来的 task **IPC space 是活的**（端口名全是内核真分配的），`mach_reply_port` 还证明了端口能分配并抄回用户页；但 `send=` 行的字面量都没出现，父亲读到 `status=9` = **SIGKILL**，死在消息那一趟，同屏多出 `Failed to send exception EXC_CORPSE_NOTIFY. error code: 5 for pid 2`（`task.c:2461`：这份内核没设 exception 端口，所以每个 EL0 进程死时都会印这行，它是死亡的另一条通道而非死因）。源码侧已排除三件事：`syscall_sw.c:137` 的 31 号确实是 `mach_msg_trap`（不是 `kern_invalid`，而 `goto bad` 的结尾是 panic，内核没 panic）、`arm_get_mach_syscall_args` 对 64 位 state 原样 `bcopy` 寄存器（`munge_wwwwwww` 只在 32 位分支用，指针没被截断）、这份副本写 `__DATA` 是成功的。剩下的分歧我读不出来，因为探针在 send 与 receive 之间一次都没打印——`v0.2.43` 因此把这两次 `svc` 各包上「调用前标记 + 调用后立即打印」，并顺手印出消息缓冲区的地址，一次刷机把「死在哪一次调用」切成独立读数；round73 实机读数是 `[clover] el0 mach: msg=4254904160`、`[clover] el0 mach: presend`，然后**还是没有 `send=`**，父亲又读到 `status=9` —— boot-arg 没能救活 legacy `mach_msg`。而这一次不能再拿「策略没编进去」当解释：`objdump out/clover/kernel/kernel.macho` 里 `_ipc_policy_allow_legacy_send_trap+0x20` 确实是先测 `MACH64_POLICY_HARDENED`（bit 55）、再 `ldrb` 读 `_allow_legacy_mach_msg`（`0xfffffff007158000+0x1a8`）、`tbnz` 为真就直接 `mov w0, #0` 返回放行，说明 DEVELOPMENT 分支是活代码；payload 里 `unixnu.toml` 的 args 末尾确实是 `allow_legacy_mach_msg=1`，而 `PE_boot_args()`（`pexpert/arm/pe_bootargs.c:11`）返回的就是 DarwinBoot 填的那块 `CommandLine`（`PE_init_platform` 直接取 booter 递进来的指针，ARM 上不看设备树的 `chosen/boot-args`），`rd=md0` 变成 `BSD root: md0` 已经证明这条通道；解析器对尾 token 也走得通（键名扫描停在 `=`、`getval` 吃掉 `=` 返回 NUM、`argnumcpy` 写 1 字节、`kernel_startup_tunable_init` 归一化成 bool）。**四条都成立，进程照旧被杀**，所以 round72/73 的死因目前只能记为未定，不能继续宣称是 legacy 闸；`v0.2.44` 因此不再和这一个调用纠缠，改问 Apple 真正推荐的 `mach_msg2_trap`（表位 47，八个打包参数），它的发送腿根本不进 `ipc_policy_allow_legacy_send_trap`（唯一调用点 `mach_msg.c:611` 只被 `mach_msg_overwrite_trap` 走到），并把 legacy 调用挪到整轮最后重放一次，让「boot-arg 是死是活」变成一个独立的、不会污染主问题的读数；round74 实机读数是**九个 el0 行到 `[clover] el0 mach: msg=` 就断在半行**——光标停在 `=` 后面、地址数字没出来、`m2: presend` 没有、corpse notify 没有、父亲的 `waited` 也没有，屏幕之后一直不变；两行「死亡通知」同时缺席意味着 pid 2 **根本没有退出**，它要么卡在携带数字的那次 `write` 里面、要么还在跑，而一张静态照片分不开这两者，所以 round74 没答上它自己提的问题（msg2 的 `svc` 一次都没执行到，三道策略闸一条都没测到）；objdump 已核对 v0.2.44 的机器码与源码逐条一致、同一套 `_print_dec` 在同一屏上刚印完 259/515/771/1827，因此不是探针写坏；`v0.2.45` 因此把问题换成「停在半行里的是进程还是 tty」，只改父亲：`wait4(child, WNOHANG)` 轮询 + 前 10 轮各印一行 `parent: poll <n>`（之后继续轮询但闭嘴），轮间用 `subs/b.ne` 计数循环烧时间——因为这份内核里 `__semwait_signal(0,0,1,1,…)` 不是延时（`port_name_to_semaphore` 对非法端口名直接返回 `KERN_INVALID_NAME`，`kern_sig.c:1090` 于是回 EINVAL），而 `select`/`mach_wait_until` 分别要结构体 copyin 和一个本表读不到的绝对期限。**round75 实机读数**：面板停在 `[clover] parent: poll ` 半行、光标在空格后、数字没出来，孩子的第一条 `[clover] el0 child: alive`（自带换行的单次 write）一个字都没有，且**没有 panic 屏**——write 若返回 errno 就会走 `_fail`→`exit()`→panic，所以父亲是没从 write 里回来。这与 round74 停在 `el0 mach: msg=` 是同一个形状（标签成功、紧随的那次 write 没回来），而两轮之间唯一的变化是父亲成了控制台的**第二个写者**：EL0 每个字符都落在 `vcputc_options`（`video_console.c:1231`）里那把 `lck_ticket_t`（`:153`）上，排队自旋、无超时，而同一函数在 `gc_enabled` 为假时还会静默丢字符——照片分不开这两种。⇒ **mach 的嫌疑被暂时搁置，round72-74 关于 mach 的读数全部标记为受污染**。round76 因此只做一个变量：只有一个写者。**round76 实机读数**：父亲两整行**完整**（`el0 init: pid=1 console fd=0 pagesize=4096` + `el0 init: forked pid=2 x1=0`，数字全在），200 轮超时后 `exit(384)` 生效，panic 屏画出 `initproc failed to start -- no exit reason available -- (signal 0, exit status 384 )` 且 `a2=0x0000000000000180`，coredump 三连 `error 2` ⇒ **控制台无罪**（单写者时字画得好好的），panic 屏是第二条能用的通道；而孩子在 `forked pid=2` 之后、父亲自己退出之前**一个字都没写**（`el0 child: alive` 都没有），父亲也从没收到尸 ⇒ 孩子既没死也没输出。于是「两个写者搞死控制台」被实证推翻：从头到尾只有一个写者，另一个根本没到屏幕。回看 round70-74，父亲睡在阻塞 `wait4` 里时同一个孩子能印好几页；round75-76 父亲改成 WNOHANG 轮询 + 计数循环，孩子连一个字形都没有——两轮之间唯一的差别是**父亲有没有让出处理器**。`v0.2.47` 因此只改这一件事：轮询之间父亲真的睡，用 `select(0, NULL, NULL, NULL, &tv)`、每轮 50 ms、共 200 轮（源码依据：nd=0 时内核只 copyin 探针自己 `.data` 里的 timeval，`sys_generic.c:1138`；selcount 计数为 0 于是跳过 selscan，`:1332`；最后落到 `waitq_assert_wait64_leeway` + `tsleep1(.., "select", ..)`，`:1455`，是真正的阻塞睡眠，不需要 Mach 权限）；select 自己失败则 `exit(416+errno)`（13 段，别处没有），超时仍是 384。**round77 实机读数**：孩子的九行全回来了（`alive`、post-exec banner、`thread=259 task=515 host=771`、`reply=1827`），面板停在 `[clover] el0 mach: msg=` + 光标、数字一个都没有，与 round74 同一处 ⇒ **饿死成立**，round75/76「孩子沉默」的读数作废，而这次孩子永久持核，所以那半行是可信读数；同一台机器等满两分钟复拍，**什么都不多出来**（无 panic 屏、无 `waited`、无新字形）⇒ 父亲睡进 50 ms 期限再没醒来，**这份端口没有任何定时器能送达 EL0 绝对期限**，探针用过的每个超时（200 轮轮询、msg2 接收的 3000 ms）都不是上界而是丢进程。`v0.2.48` 因此不用时钟，改等一个不需要时钟的事件：孩子死掉。父亲回到阻塞 `wait4(child, &scratch, 0, NULL)`（既出让核心，又能被 `exit`/`SIGKILL` 叫醒、叫不醒卡住的线程），孩子只在数字之后多一条 `exit(448)`，这一轮不画 panic 屏以便孩子的九行和父亲的状态行留在同一张照片上。**round78 实机读数**：正是第三格——孩子的九行照旧停在 `[clover] el0 mach: msg=`，而父亲的 `waited` 永远没有出现，也没有 panic 屏 ⇒ 孩子**既没死也没被饿死，它是卡在某一次 `write` 里面**，round72-74 关于 `mach_msg` 的猜测正式出局（探针在那条 `svc` 之前就结束了）。但形状里有个细节决定下一轮问什么：字符是在 `write` 返回**之前**就上屏的（`ttwrite` 填 `t_outq` → `ttstart` → `kmstart`/`kmoutput` 同步画字），所以「屏幕上有个 `msg=`」并不等于「标签那次写成功了」，它同样兼容「标签这次写完自己的字之后在返回路上堵住」——那正是 `ttwrite` 的 `ovhiwat` 出口 `ttysleep(…, "ttywri", tp->t_timeout)`（`tty.c:2656`）在这台没有期限的板子上的表现。`v0.2.49` 因此把孩子的 `exit(448)` 往上挪两条指令，切在标签那次写与数字那次写**之间**，其余一字未改。**round79 实机读数**：探针一次都没被执行到，面板照旧停在 `msg=`、父亲的 `waited` 照旧永远不出现 ⇒ 堵住的是 `msg=` **那次标签 write 自己**（把自己的字节画上屏之后再没回来）；而 `ttwrite` 里「画完之后」还能停住的位置只有 `ovhiwat` 出口的 `ttysleep(TSA_OLOWAT, "ttywri", tp->t_timeout)`（`tty.c:2656`）—— 在一块期限从不响的板子上，那就是一次永久睡眠。但这张照片同时兼容三种状态（标签 write 堵住 / `exit` 堵住 / 父亲被叫醒之后自己那次打印堵住），因为它们唯一的输出通道就是那个正在被审查的控制台。`v0.2.50` 因此只换父亲的报告方式：醒来后一个字都不写，改成 `exit(512 + (status & 0xff))`，用 initproc 退出触发的 **panic 屏**说话（round76 已证明它直写 framebuffer、不调 `vcputc`）：`exit status 512` = 孩子退出了、`wait4` 被叫醒、缺的只是打印；`512+N`（N<256）= 孩子被信号 N 弄死；永远没有 panic 屏 = 孩子真的还在那次 write 里面 。`round80` 实机读数落在第三格：面板停在 `msg=`，**永远没有 panic 屏**（间隔两分钟复拍也一样）——父亲的阻塞 `wait4` 从没被叫醒，孩子从没死；而且内核自己那行 `Failed to send exception EXC_CORPSE_NOTIFY … for pid 2` 也没出现在开机日志尾巴上，`kprintf` 走 video console 不走被测的 tty，所以这是**两条独立证据**说同一件事。剩下两个嫌疑：标签那次 write 画完字之后堵住，或者 write 返回了、孩子的 `exit` 里堵住——而「exec 出来的孩子的 `exit()`」在这块板上从来没被证明过（round70 那个孩子是 fork 未 exec，round72/73 是 SIGKILL 不是 exit）。`v0.2.51` 用第三种事件切开它们：孩子不再去死，改成 `kill(getpid(), SIGSTOP, 0)`（Darwin 的 kill 是三个参数，syscalls.master:91，`posix` 传 0），停住是在**进 userland 的路上**办完的，所以哪怕孩子再也没执行一条指令，父亲也收得到；父亲 `wait4` 加 `WUNTRACED`（2）并把状态低 16 位全留着（`W_STOPCODE(17)` = 4479，遮成字节会和「signal 127」撞车）。四个读数：panic `4991` = 512+4479，write 返回了、堵的是孩子的 `exit`；`1024` = kill 成功但没停住、孩子 `exit(2)`；`768` = kill 本身失败（37 号不可达）、孩子 `exit(1)`；永远没有 panic 屏 = 堵住的就是那次 write。**round81 实机读数**：正是第四格——面板停在 `msg=`，复拍什么都不多出来，而且这次能签字：这份树里 SIGSTOP **不是**延后信号，`kill` 的默认动作在 syscall **里面**办完（`kern_sig.c:2349` stopsigmask → `:2362` `p_stat=SSTOP` → `:2373` `stop()`，而 `stop()` 第一步就 `wakeup(parent)`），所以只要那次 `kill` 进了内核，父亲的阻塞 `wait4` 必定被叫醒；父亲一次都没被叫醒 ⇒ 孩子连那条 `svc` 都没碰到 ⇒ 堵住的就是 `msg=` 那次标签 write 自己，孩子的 `exit` 正式出局。同一轮把 `ovhiwat` 头号嫌疑降级：`bsd/dev/arm/km.c` 的 `kmoutput` 是 `while (c_cc > 0)` 一次排空、`t_hiwat = roundup(1216,48) = 1248` 大于整个 cl 池 `TTYCLSIZE = 1024`，那条判断永远到不了。**v0.2.52（round82）换了目标：这台板子从来没有过一次 tick。** 依据来自参考仓 `xVanTuring/UniXNU-tb128fu`（`board.toml:199-212`，`armcpu-timer-intid=19`，注释说 27 是「another source that stays asserted」）与本板真机设备树 `mu-clover/Resources/DTBs/clover.dts:2233`：timer 节点四个 PPI 是 1、2、3、0 ⇒ 真 INTID 17、18、19、16，而 `enableCPUTimeBase` 写的是 `cntv_ctl_el0`（虚拟定时器）⇒ 该走的是 **19**；我们这份 `drivers/ArmCpu` 把 `0xffff` 并上 `(1 << 27)` 写进 redistributor ISENABLER0、又在 `interrupt()` 里只认 27，也就是内核一直在等一条从没被 enable 的线。一条根因统一解释 round77 的两次实测（`select` 的 50 ms 期限睡下去再没醒来 = 期限依赖 `Timer::sleep` 到点；EL0 纯自旋饿死同板另一个进程 = 没有 tick 就没有抢占）。本轮只动内核：`drivers/ArmCpu/` 用 `feeds.py` 的设备仓优先规则覆盖基仓模块，加 `armcpu-timer-intid`（16..31 才接受，不传仍是 27）与 `armcpu-clock-panic=N`（第 N 次 tick 直接 `panic("ArmCpu: clock watchdog after %llu ticks (intid=%u ipi=%llu irq=%u phase=%u)")`），panic 屏多打 `ticks/ipi/sent/irq/phase/intid/typer` 七行；探针与 root 逐字节不变，故 trustcache 不变。看门狗保证「什么都不发生」也一定出声：`ticks=0x7530` 说明时钟在走而进程照旧卡住（时钟无罪，回到 tty），完全没有 panic 屏说明 INTID 19 也没让 tick 起来（用 `typer=` 核算本板 GIC 的 INTID 上限再换线）。这一轮故意不传 `armcpu-self-ipi` 与 `armcpu-spi-ipi`：单核，且 `kIpiSpiBase` 硬编码的 1008 有没有超出本板上限要看 `typer=`。**round82 实机读数：panic 屏第一次出现，而且 `ticks=0x14e5`（5349）⇒ INTID 19 是对的，这块板第一次真的有 tick，看门狗与 panic 屏两条通道都可用。但死因换人了**：panic 内容是 `"AMFI: No PMGR?"`（`ConfigurationSettings.cpp:235`）。AMFI 在 dyld 启动期的 thread-call 工作线程里等一个 `ApplePMGRNub` 服务；过去没有 tick，那次带期限的等待永远不响、线程只是无声吊着，启动继续走到 EL0；有了 tick 以后期限真的到期、匹配返回空，于是 AMFI 自己 panic。`ipi=0 sent=0` 与单核相符；`irq=0x3ff`（=1023，分发计数器的初值）、`phase=0`；`intid=` 与 `typer=` 两行被开机日志的尾部覆盖了，本轮没读到。**v0.2.53（round83）已发布：给 AMFI 造一颗它自己能匹配上的 `ApplePMGRNub`**（`drivers/CloverPMGR` + `CONFIG_XNU_SHENG_PMGR=y` + `clover-pmgr=2`，EL0 探针与 root 一字未动，panic 屏多打 `pmgr=/nub=/aon=` 三行，看门狗 `armcpu-clock-panic` 撤掉）；`v0.2.54` 只把 panic 屏的打印顺序换成「读数在前、寄存器证据在后」，因为 `put()` 越过面板下沿就静默丢字（round82 就是这样丢了 `intid=`/`typer=`），题面与产物语义不变。**round83 实机读数 = 命中判定表最后一格：`CloverPMGR` 一条 panic 都没报、`"AMFI: No PMGR?"` 消失、boot 在有 tick 的状态下走到 EL0 ⇒ 守卫壳 + 原始工厂造的这颗 nub 确实被 AMFI 自己那条匹配抓走了，AMFI 要的只是一颗服务而不是电源功能**；面板照旧停在 `[clover] el0 mach: msg=`、后面什么都没有、没有 panic 屏，与 round81 同形 —— 但「期限从来不响」这个借口现在有两条证据说明它已经没了（round82 那次 panic 本身就是 `waitForMatchingService` 的 deadline 到期返回空才产生的，加上 `ticks=0x14e5`），照片里还有一条正面证据：`AMFI: PMGRAON latched to: …`（内核串 VA `0xfffffff0042dde0f`，紧接 `0xfffffff0042dde02` 那颗 `'ApplePMGRNub\0'`）——AMFI 确实拿着我们发布的 nub 走完了「slot 320 读 AON → OR 0x4 → slot 321 写回」，所以原计划给 CloverPMGR 成功路径补的那条 `IOLog` 取消（信息重复，还要重建内核）。**`v0.2.55`（round84）因此做两件事**：一是把 round79 那一刀在有 tick 的板上重切（孩子回到标签 write 之后立刻 `exit(448)`，删掉 round81 的 `kill(SIGSTOP)` 块）；二是修父亲那条报数通道——查「多出来一行 `parent: status = 114688`」时查明它**根本不可能出自 v0.2.54 的镜像**（payload 原始字节里 `parent: status`/`status = ` 都是 0，43 份归档全为 0，`114688 = 448 << 8` 恰好是 round78/79 那版父亲打印的原始 wait 状态），顺带挖出真 bug：initproc 的退出码到面板要过 `WEXITSTATUS(rv) = (rv >> 8) & 0xff`（`kern_exit.c:1875` + `wait.h:144`，`rv = W_EXITCODE(C,0) = C << 8` 见 `kern_exit.c:1466`），**只有 8 位**，round80-83 的 `512 + 低16` 在到达屏幕前就被折掉了（`exit(448)` 会读成 `exit status 0`、SIGSTOP 会读成 `127`），只是那几轮的读数都是「完全没有 panic 屏」，缺席不受截断影响，所以一直没骗到我们。父亲现在手工把码落在 0..255：`0`=我们的 `exit(448)`、`1`=从 `_fail` 走的、`32+N`=信号 N、`96+N`=停在信号 N、`32`=父亲的 `wait4` 自己失败；永远没有 panic 屏 = 有 tick 也堵，下一刀切进 `kmoutput`/`ttwrite`。**round84 实机读数 = 最后一格**（面板停在 `msg=`、复拍两分钟什么都不多出来），但这张照片第一次把孩子的 `write` 判了无罪：底部八行逐字可读（`alive`、post-exec banner、`thread=259 task=515 host=771`、`reply=1827`、`msg=`），说明 exec 出来的孩子已成功返回过至少八次 write（其中三段是不带换行的裸标签、还有一次换行真的把屏幕滚了一行）⇒ 「exec 孩子 syscall 不可用」与「只有带 `\n` 的写会回来」两条猜测同时出局，`gc_scroll_up`/`vc_scroll_up` 也不是堵点。于是 `msg=` 与前八次成功的写在内容、长度、结尾、光标位置上都没有区别，而这张照片仍兼容三种状态：(a) 那次 write 没回来、(b) write 回来了孩子的 `exit` 没回来、(c) 两个都办了但父亲的阻塞 `wait4` 没被叫醒（round70 那个能叫醒父亲的孩子是 fork 未 exec，exec 之后 `childproc` 还连不连着从未验证）——三者的报告通道恰好就是被审查的那两样东西。⇒ v0.2.56 只换通道：孩子用 `reboot(RB_PANIC\|RB_QUICK, msg)`（syscall 55，`kern_xxx.c:182` 立刻 `panic_with_options("userspace panic: %s")`，不经 sync/卸载、不经 tty、不经父亲；DEVELOPMENT 内核里 `kern_xxx.c:116/142` 连 suser 与 MACF 都跳过）。**round84 读数落盘后 v0.2.56（round85）已发布**：`2873df4f…` payload / 根镜像 `fe27be32…`（35456 B，cdhash `6faaec7e965356b1af732a83f297e605010dcf02`）/ trustcache `d310ae35…` 已同步进 `trust_cache_sha256`，内核逐字节不变（`e98b9659…`）。孩子最后一次动作是 `reboot(0xc00, "clover-el0: write returned")`，紧跟一条不可达的 `exit(449)`（刻意不等于 448，这样父亲报 `1` 而不是 `0`）。读数四格：`userspace panic: clover-el0: write returned` = 标签 write 回来了、(a) 出局；`userspace panic: user space RB_PANIC message copyin failed` = 走到了 reboot 但 copyin 失败（另一条通道，同样证明 write 回来了）；照旧 `initproc exited … exit status 1` = 孩子从 `_fail` 走的；永远没有 panic 屏 = (a) 成立，下一刀把 reboot 挪到那次 write **之前**，一次调用同时切两条边。**round85 实机读数 = 永远没有 panic 屏** ⇒ 四个出口（write 返回 / `exit(448)` / `kill(SIGSTOP)` / `reboot→panic`）在同一处静默，而 round74-84 十一轮同形；剩下的三种状态（write 没回 / `exit` 没回 / 父亲 `wait4` 没被叫醒）**从用户态分不开**，因为它们只能经由被审查的 tty 或那个可能没醒的父亲发言。v0.2.57（round86）因此把报告搬进内核，但搬进**不被测的那条路**：`drivers/ArmCpu` 的 tick 看门狗 panic 带 `elr=%llx spsr=%llx elr2=%llx cnt0=%llx cnt1=%llx freq=%llx`（入口处的 `ELR_EL1` = 被打断上下文当时的指令地址，`spsr` 低 4 位 = 它在哪个 EL：5=EL1h、0=EL0t），`armcpu-clock-panic=60000`（tick 数是启动进度坐标：round82 的 5349 在 AMFI 检查处 = 探针到达点前一步，所以 60000 ≈ 十一倍之后，且不依赖 tick 周期；`cnt0/cnt1/freq` 顺手第一次量出周期）。不用 `kprintf` 插桩是查出来的两件事：`console_write` 自己是环形缓冲 + 写锁（`serial_console.c:572` 无界自旋排空、`:424` 持 `write_lock` 自旋），从被测路径内打印会二次进入被测对象；而 `tree/patches/xnu` 是指向基仓的软链、`feeds.py` 按 `(kind,name)` 取第一个存在的路径 ⇒ 设备仓建 `patches/xnu` 会整体顶掉上游上百个补丁，所以仪器进设备仓已拥有的 `drivers/ArmCpu`。本轮**第一次换的是内核**（`e98b9659…` → `e87a5127…`），EL0 探针与根未动（`fe27be32…`、trustcache `d310ae35…` 照旧），payload `b964c1a5…`，`verify` 四项全绿、卷内 `unixnu.toml` 实到 `armcpu-clock-panic=60000`；读数侧新工具 `scripts/decode-clover-address.py`（`--suspects` 出本镜像的 tty/console 区间表，命名 + 偏移 + 回跳循环判定），已用 `_ttwrite`/`_console_write`/`_cpu_idle` 三个已知地址自测，并能认出 `console_ring_try_empty` 那条 25 指令无调用轮询。 |

上面这张表之外，不做任何额外声称。

## 当前进度：已解决 / 待解决

最新一帧是 r169 的冷刷，它把 r168 提的那个问题答掉了：**这一帧屏上没有 EIO**。
`disk1: general error.` 不再出现，`apfs: buf_bread(0x20): … failed, error = 5`、
`ctx->dev_read(507): disk1 dev_get err 5 reading blkno …`、
`apfs_ntree_read(805): … retval 5` 这一整套级联全部消失，r164/r166 那两轮的
APFS jhash 自我校验错（`Data hash mismatch for 16384 bytes …`）也没有再出现，
而且没有 panic。唯一一条驱动读数是 `CloverSdhci r169: block guard waited 31 ms`
——等锁 31 毫秒，不是三十秒，也没有 `card busy`、没有 `read <stage>`：读路径这
一次根本没走到失败分支。r169 加的「读失败先发 CMD12 再复位主机」+「每个读请求
在第一条命令前等卡 `DATA_INHIBIT` 落下、CMD13 报 `READY_FOR_DATA|TRAN`」把
r168 那条不对称补上了，级联因此断根。

但这一帧同时暴露了我们自己的仪器问题：**屏被自家探针占满**。照片上几乎只有
`clover r153: check_np dyld_flags=0x0` 这一行在重复，另有一条
`clover r153: open /System/Library/Caches/com.apple.dyld … err=0`，尾部还有
`launchd [Event: finish-restore] during boot task`。`shared_region_check_np` 每
颗进程都会调、而且调好几次（dyld 的 `reuseExistingCache` 与后面的映射都走它），
r153 那版探针一次都不节流，于是这一行把整块面板刷成了同字复读。这条读数还留着
一个我们读不出来的洞：**这一行不带 pid**，所以「重复很多次」既可能是几颗进程
各调几次，也可能是一百颗进程各调一次；而 exec 出去的守护进程死掉时，XNU 只对
critical process 打印原因，普通子进程的死亡在控制台上是完全静默的。r162/r164
那种「launchd 反复拉、pid 一路涨」的循环，在这一帧里根本没法判读。

所以 v0.5.24（round170）只做两件事，都不动存储栈：**给缓存探针加节流**
（`check_np` 只在值变化或 pid≤8 时打，总上限 48 行；缓存目录的 open 上限 16 行，
失败与主缓存/`.01` 永不封顶），**以及让每颗进程的生与死出声**——`exec` 一行带
pid/ppid/秒/comm，`exit` 一行带 pid/ppid/秒/退出码/`osr_namespace`/`osr_code`/
comm，各上限 256 行。这一轮的问题是：**launchd 是在反复拉同一批死掉的子进程，
还是在往前走**。如果 `clover r170: exit … rv=… ns=… code=…` 成串回来、comm 换来
换去，问题在退出原因那一列；如果只有 `exec` 没有 `exit`、时间戳 `t=` 一路拉长，
问题在「谁还没跑完」。

### 已经解决（有真机读数）

| 事项 | 读到的证据 |
| --- | --- |
| 固件与菜单 | `uefi-clover` v0.3.10 渲染 UniXNU 菜单、读到 payload，3 秒后默认进 XNU |
| XNU 引导链 | 内核经 `arm_init`、`machine_startup`、`vm_mem_bootstrap` 走到 `_bsd_init`；v0.2.34 的九个 `[clover] … returned` 标记里前八个逐条印出，`vfs_mountroot` 当时停在 `IOFindBSDRoot`，eMMC 驱动接上后根挂载已经通了（round137） |
| 时钟与抢占 | round82 起 INTID 19 真的来 tick（`ticks=0x14e5`）；`select` 期限、`wait4` 唤醒和抢占第一次有依据 |
| AMFI | `amfi-only-platform-code` 加上 `CloverPMGR` 发布的 `ApplePMGRNub` 后，AMFI 的 `No PMGR?` panic 消失，`PMGRAON` 走完读写（round83） |
| EL0 与 syscall | mockfs 根 `rd=md0` 上 pid 1 跑进用户态；`getpid`、`open("/dev/console")`、`write`、`fork`、`wait4`、`execve` 都有读数 |
| APFS 根 | round137 挂上 OS 卷；round150 owner 审计接受未封印的根，三模块信任缓存放行 |
| SystemOS | round143/145 在 exec 时把 SystemOS 卷 union 到 `/`；round153 dyld 打开缓存目录、主缓存和 `.01`，三处 err=0，两个头都是 `dyld_v1   arm64` |
| eMMC 高速读 | round159 帧：`CloverBlock r159:` 的三条 manifest/data 行都在；签名块 16 KiB 读 7 ms、224 KiB 读 105–189 ms，约 1–2 MB/s，比 round156 之前的 25 KB/s 快 40–80 倍；CMD6 高速链路、8 线与 CMD18 批量成立 |
| eMMC 读路径（不再级联失败） | round169 帧：`general error`、`buf_bread … error = 5`、`dev_get err 5`、`apfs_ntree_read … retval 5`、decmpfs/`WEP_GETNTRC errno 5` 与 APFS 的 jhash 校验错在同一次冷刷里全部消失，唯一驱动读数是 `block guard waited 31 ms`；读失败现在先 CMD12 停卡再复位主机，读前等卡空闲 |
| Data 卷 | round159 帧：`seed data … bytes=33554432`，`bsd=disk5s1`；APFS `mount-complete volume DawnData`，卷 UUID `FC1A580E-2D67-48FC-B8CF-87422A319340` 与钉住的种子一致；owner 的 `/private/var` 有卡背书，且失败行没有出现 |
| dyld 缓存打开与预检 | round159 帧：`open /System/Library/Caches/com.apple.dyld`、主缓存、`dyld_shared_cache_arm64.01` 全 err=0；`.01` 头 `len≈1.07 GiB sig=11491332+229376`、`err=0/0`，224 KiB 签名 blob 一次读完 189 ms；随后每个子缓存的 `F_ADDFILESIGS` 都是 `err=0 resid=0` |
| 诊断通道 | panic 屏直写 framebuffer（不经过 tty）、无人值守的 IMEM `0x77665501` + PSCI 复位、`read-clover-log.py` 取 logfs、`decode-clover-address.py` 把地址翻成函数 |

### 已实现，等下一帧（round154/157/158/160/164/165/169/170）

| 轮次 | 内容 | 状态 |
| --- | --- | --- |
| round154（v0.5.7） | syscall 536 的入口/返回值、setup 阶梯与 `mapfile` 探针 | 探针仍在镜像里；r169 帧被自家 `check_np` 刷屏，仍无 `map2` 读数 |
| round157（v0.5.10） | CMD12 改 R1b、拒收 CRC/TIMEOUT/END_BIT/ADMA 损坏块 | 读路径工作；错误分支尚未被触发过 |
| round158（v0.5.11） | 照 sheng 对齐 `porting.toml`/`AGENTS.md`/工作流，锁超时放宽到约 21 s | 契约层，无需上机 |
| round160（v0.5.13） | 宿主闸门 `verify-userdata-window.py`；镜像与内核一字未动 | 无需上机 |
| round163/164（v0.5.17/18） | AMFI 两条拒绝分支 + 16 KiB 签名的 dyld 缓存跳过整文件哈希 | r164 帧：AMFI kill 消失；85 与 APFS jhash 错仍在，根因指到读路径 |
| round165（v0.5.19） | 读前清 FIFO、失败复位数据通路、首次满批 A/B 对照并可按结果禁用批量读 | 已并入 round166/167 |
| round166（v0.5.20） | 整轮单块读、补上命令响应失败的复位、exec 侧打印前八个签名 blob 的头四字 | r166 帧：单块读没有消掉故障；APFS 报 `disk0s1 failed to read` 与 `Container corruption detected by apfs_scan_verify`，故障落在可写的 Data 卷 |
| round167（v0.5.21） | 写完成必须 CMD13 确认 `READY_FOR_DATA|TRAN`、停命令失败复位数据通路、写节奏改 level-gated、两条请求死线随块数缩放、EXT_CSD `CACHE_CTRL` 清零；批量读须三份样本一致才信任 | r167 帧：没有 panic，APFS 在两块盘上返回 EIO（`dev_get err 5`），一次 `tx_flush` 用 22.7 秒；EIO 落在我们自己的 200 ms/2 秒超时上 |
| round168（v0.5.22） | 写落盘闸门 4 秒、guard 等待 30 秒、读写死线与每块份额放宽、批量/单块失败各自重试、写批次超时后重发一次；放弃时上屏块号与 INT_STATUS | r168 帧：写墙过了（无 `tx_flush` 长行、`disk5` 不再 general error），但 16 KiB 读级联 EIO；驱动只在写失败时停卡，读失败不停 |
| round169（v0.5.23） | 读失败先 CMD12 再复位主机；读前等卡 `DATA_INHIBIT` 落下 + CMD13 `READY_FOR_DATA\|TRAN`（30 秒）；写落盘 30 秒、写重试前等卡；读失败按 `guard/idle/deadline/gave up` 分阶段打点 | r169 帧：EIO 与 jhash 错全消失、无 panic、`block guard waited 31 ms`；读路径收干净。副作用是同帧被 `check_np` 复读刷屏，进程生死读不出 |
| round170（v0.5.24） | 缓存探针节流（`check_np` 48 行、目录 open 16 行，失败与主缓存/`.01` 不封顶）；`kern_exec.c`/`kern_exit.c` 各加一行 `clover r170: exec`/`exit`，带 pid、ppid、开机秒数与退出 namespace/code，各 256 行封顶 | 等冷刷；读数是 exec/exit 的配对与 `rv`/`ns`/`code` 三列 |

### 还没解决

1. **共享区映射（syscall 536）仍没有读数**——`.01` 的头字段与签名注册两段已经被
   r159 帧关掉；r169 帧上只有 `check_np` 在重复，看不到 `map2`。按 dyld 的
   `reuseExistingCache()`，`__shared_region_check_np` 命中且头部有效时根本不会再
   打开缓存文件，这一帧「只有一条目录 open、没有文件 open」的形状与它一致，但
   面板只留尾部、复读行会把别的字挤出去，所以这仍不是结论。r170 的节流就是为
   了把这一格变成可读的：封顶之后，屏上剩下的才是 open/map2/exit。
2. **守护进程能不能起来还不知道**——r162/r164 帧的形态是每个子进程在
   `ubc_cs_blob_add` 的结构校验里返回 `EBADEXEC`（85），round164 帧第一次证明
   那来自读回的字节不对（APFS jhash 期望 `2e6c78f0…`、实得 `0d13…`）。r169 帧
   把 jhash 错和 `errno 5` 一起消掉了，照片上也没有 85 行，但同一帧被 `check_np`
   占满，尾部之外不能算数——所以 85 是消失还是被挤掉，本轮还判不了。r170 的
   `clover r170: exec pid=… comm=…` 直接给出「哪些二进制真的换进去了」。
3. **完整 `/data` 没有解决**——现在只是 32 MiB 的 APFS scratch 种子，追加在
   userdata 分区的两个 GPT 副本之间，不在 GPT 里；原 54.4 GB 的 userdata 内容
   没有迁移。r159 帧的 Data 挂载只证明那块种子可用。
4. **launchd 到底在循环还是在前进**——r159 帧之前看到的循环是「exec 主程序 →
   dyld → 注册整组共享缓存签名 → 主程序签名结构校验失败 → 85」，pid 一路涨，
   `mediaserverd` 以 `Too many corpses being created` 收场。r169 帧看不到这条
   链的任何一环，因为普通子进程退出不出声。r170 就是问这一件事：exec 与 exit
   成对出现、comm 在换 = 在前进；只有 exec 没有 exit、`t=` 一路拉长 = 有东西
   堵在里面；exit 成串带同一个 comm = 还在循环。只能断电；断电后回到 fastboot，
   不需要再刷任何分区。
5. **显示与 GPU**——没有 MDP5/DSI 驱动，屏幕只服务于启动诊断；Adreno 512 没有
   驱动，没有 iOS 显示栈。
6. **输入**——触摸（`edt-ft5406` / focaltech `fts`）没有驱动。
7. **网络、音频、USB**——WCN3990 WiFi/蓝牙、WCD 音频、USB device 模式都没有。
8. **电源管理**——除 AMFI 需要的那颗 nub 外，充电、电池、PMIC 都没有真正的驱动。
9. **eMMC 高速写的绝对速度还没量到**——读已经快了两个数量级，写从 r168 帧起不
   再是墙（22.7 秒的 `tx_flush` 消失）。但 r169 帧上没有 `write settle took … ms`
   的可见读数（每启动只打一次，被复读挤出面板），卡自己的编程时间仍然未知；要
   不要换 HS400/CQE/DMA，等这一行真的被读到再定。
10. **内核构建不可逐字节复现**——每次 prelink UUID 不同；`verify-clover-payload.py`
    只比较两个可执行段。
11. **存储栈形态**——没有 UFS、virtio-block 或 native verified-root；sheng 的
    存储栈是 SM8550 专有的，不能照搬。

### 下一步（按顺序）

1. 冷刷 `round170-payload.img`，只问一件事：`clover r170: exec` 与
   `clover r170: exit` 的配对形状——comm 是否在换、`rv`/`ns`/`code` 报的是什么、
   `t=` 走了多远。附带读数：节流之后，`check_np` 只在该轮出现 48 行以内，屏上
   应该重新看得见 open/map2 与任何一条 APFS 或签名错误。
2. 如果 exec 一路铺开、exit 带着 `ns=`（签名或 AMFI 的退出原因）成串回来，下一轮
   就按 r166 那条 `clover r166: sig` 的头四字继续量 blob；如果 exec 停在某颗
   comm 上不再出现新的，问题在 exec 内部而不是循环。
3. 如果 `clover r154: map2` 在节流后出现，共享区映射（536）这一格同时打开；否则
   536 排在用户态起来之后。完整 `/data` 迁移、显示、输入、网络都排在它后面。


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
4 字节的 `amfi-only-platform-code`、4 字节且为 1 的 `debug-enabled`）、`/chosen/memory-map` 里恰好只有 `RAMDisk`
和 `TrustCache` 两个占位（出现 `PackedRAMDisk` 就报错：DarwinBoot 不接受两个
ramdisk 槽）、
带八个 MPIDR 值的 `/cpus`、`/psci`、带 `ranges` 与 GICv3 节点的 `/arm-io`、
`/defaults`、`/options` 和 `/product`。board 一段钉住 `v0.4.3` 的根契约：
`packed_data_ramdisk` 必须为假，`kernel_args` 必须且只能有一个 `rd=md0s39`
（四字符别名只喂给 `bsd_rooted_ramdisk()` 的前缀比较，不参与根声明），必须有
`cs_enforcement_disable=1`（r138 的 exec 拒绝点在 AMFI 的 label-update 钩子），
`/chosen/debug-enabled` 必须存在且为 4 字节 1（AMFI 只在
`PE_i_can_has_debugger()` 为真时才接受那个参数，r139 的 panic 就出在这里），
`/chosen/root-matching` 必须是对 OS 卷 UUID 的 `IOMedia` 属性匹配，
`clover-apfs` 必须为 `3`，`trust_cache_sha256` 必须非空。

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

实机掩码是 `0x5abc7`，答案见下一节。

## v0.2.88（round116）：接住内核自己建好的那棵树

round115 的照片给出的掩码是 `0x5abc7`，开机日志里还有一整列

```text
devfs: disk0s39: name slot allocation failed (Errno=17)
```

（从 `disk0s1` 到 `disk0s39` 各一行）。两者合起来只指向一个结论：**IOKit 的自动匹配已经把这棵树建完了，手工方案建的是一棵重复的树。**

掩码逐位读（位号沿用上一节的表）：

| 位 | 读数 | 说明 |
| --- | --- | --- |
| 0–2 | 1 | 整盘 `IOMedia` 已经有 `BSD Name/Major/Minor` |
| 3–5 | 0 | 手工方案发布的那个 logfs 分区一个属性也没有 |
| 6,7,8,9,18 | 1 | 目标分区 Location 是 `"39"`、有 provider；整盘有 provider、`isWhole`；分区 base 非 0 |
| 10 | 0 | 目标分区没有被误判成整盘 |
| 11 | 1 | 私有探针名被 devfs 接受：devfs 就绪且可写 |
| 12,14 | 0 | `disk0` 与 `disk0s39` 已经被占用 |
| 13,15 | 1 | `disk1` 与 `disk1s39` 空闲 |
| 16 | 1 | 手工整盘客户端启动之前，整盘就有 BSD 名字 |
| 17 | 0 | 手工分区客户端之前，我们的分区没有名字 |

所以 devfs 不是问题，手工方案的解析也不是问题（39、provider、base 都对），失败只在于「名字已经被别人占掉」。日志说明了是谁：`IOBlockStorageDriver` 在 `acceptNewMedia` 里用 `kIOServiceAsynchronous` 注册整盘，IOKit 的配置线程随即自动匹配 `IOGUIDPartitionScheme` 与 `IOMediaBSDClient`；`pingConfig` 按 `gMaxConfigThreads` 会起多条配置线程，自动树与本处理器（同样跑在配置线程上）是并发的。自动树先一步把 `disk0`、`disk0s1…s39` 全部命名，本轮再手工建的 39 个分区与客户端只能一个个撞 `EEXIST`，那一列日志就是这么来的。

round116 因此不再建第二棵树：

1. 手工的部分只到整盘为止：设备 → `IOBlockStorageDriver` → 整盘 `IOMedia`，并核对整盘大小等于 `clover_emmc_block_sectors() * 512`。
2. 不再 attach/probe 分区方案，也不再手工启动任何 `IOMediaBSDClient`。
3. 在整盘的子树里轮询（`IOSleep(10)`，最多 100 次，共 1 秒），按内核自己写下的 `BSD Name` 找 `disk0s39`，同时统计带名字的分区数。
4. 找到后只做验证：`IOBSDNameMatching` + `waitForService`（`setconf()` 对 `rd=` 用的就是这两步），`bdevvp(major << 24 | minor)`，再比较「整盘在分区 base 处读一块」与「分区在 0 处读一块」，两者必须逐字节相等。
5. 成功打印一行：

```text
CloverBlock r116: auto p=39 disk0s39 1:39 bd=1 map=1
```

`rd=` 本轮仍是 `md0`。这一轮不问根挂载，只问自动树能不能独立长到 `disk0s39`，以及从它出发的 `bdevvp` 与偏移映射成不成立。

失败行：

| 失败行 | 含义 |
| --- | --- |
| `auto <n>` | 1 秒内没找到 `disk0s39`，或整盘没有名字。高 16 位 `0x10000` 表示整盘已命名；低 16 位是最后一轮看到的有名字分区数。只有 `0x10000` = 整盘命了名但分区树没长出来；`0` = 自动树完全没动 |
| `bsd-type` | 找到了 `disk0s39`，但三个属性不是 `OSString`/`OSNumber` |
| `root-match <ptr>` | 名字匹配等到的不是这个媒体（`0` = 超时没人应答） |
| `bdevvp <err>` | `bdevvp` 失败，`err` 是它返回的错误码 |
| `buffer` / `open-whole` / `read-whole` / `open-part` / `read-part` / `map` | 读块或偏移比较失败；`map` 是两处内容不一致 |
| `ready` / `storage` / `modload` / `postload` / `device` / `attach` / `driver` / `media` | 与上一轮相同的前置步骤 |

轮询必须让出 CPU（`IOSleep`），否则自动树没有机会被别的配置线程推进；`waitMatchIdle()` 不能在这里调用，它等的是 `gNumConfigThreads` 归零，而调用者自己就是一条配置线程。

`CloverPMGR` 本轮只把行首标签改成 `r116`，参数行内容不变，便于和上一张照片逐字对照。

产物（内核与 payload 变了；root / trust cache / boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round116-payload.img` | 58720256 B | `a24b1c511822b0a8a95852a58170a03648934370e38c45b7c780123aa1d08250` |
| `round116-kernel.macho`（构建产物） | 51974144 B | `2a9e065bd341265e986b80896255e8338e6a15d0f930829e8c3a19bec7ca5467` |
| `round116-in-volume-kernel.macho`（卷内副本） | 51974144 B | `54d5f05526a7c0586a084c7021c71b6400579458808334a1d9669e3c4a90cca7` |
| `round116-kernel.macho.adt` | 13476 B | `d1ccad298d3101f6c2dfeb3b876a729b91f40b20917dee158b3f76a9832948c8` |
| `round116-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round116-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round116-unixnu.toml` | 558 B | `1b0367775cecd587d0ab8a6f22527a1e5c7585736aa17c0dfd225458f8e8e148` |

校验器：`check-clover.py` 通过（ADT 13476 字节、8 核、splash `0x9d400000-0x9f800000`、FD `0x9fc00000-0x9ff00000`、`clover_defconfig`、`CLOVERESP`、模块可见）；`verify-clover-payload.py` 通过（probe 补丁只落在本地化、PMGR 绑定仍在、CloverSdhci 已链接）。`CloverBlock` 的导入与 vtable 审计通过（264544 字节、190 个槽、26 个覆写，导入全部有来源；本轮去掉了 `_devfs_make_node` / `_devfs_remove`）。卷内 boot-args 末尾仍是 `… clover-pmgr=2 clover-emmc=21 armcpu-clock-panic=60000`；卷内内核只有 r116 字符串（`CloverPMGR r116: args [%s]`、`CloverBlock r116: %s %llu`、`CloverBlock r116: auto p=%u %s %u:%u bd=1 map=1`），没有 r115 残留。

内核符号：`_sheng_pmgr_run` `0xfffffff007971274`、`_clover_emmc_run` `0xfffffff00797231c`、`_clover_block_run` `0xfffffff007992054`、`_clover_block_request` `0xfffffff007992884`。boot 仍是 `round87-boot.img`（`196b284f…`）。

```sh
fastboot flash cache ~/clover-out/round116-payload.img
fastboot reboot
```

**一次刷机只回答一个问题：不手工建第二棵树时，自动树会不会在 1 秒内交出 `disk0s39`，并让 `IOBSDNameMatching` → `bdevvp` → 偏移比较一路走通。** 成功之后，下一轮才把 `rd=` 换成 `disk0s39`，去碰真正的根挂载。

## round116 读数（v0.2.88 实机）：自动树交出了 `disk0s39`，崩在字典的第二次释放

照片是内核自己的数据异常，不是 `fail()`：

```text
Kernel data abort, at pc 0xfffffff00799238c, lr 0xfffffff007992384
(saved state: 0xfffffffbd980b890)
... x26: 0xfffffff946e66cb8 ...
esr: 0x96000028
```

反汇编 round116 自己的内核，这一小段正好对上 `pc`/`lr`：

```text
0x...2378  add  x1, sp, #0x38
0x...237c  mov  x26, x0           ; x26 = IOBSDNameMatching() 的字典
0x...2380  bl   IOService::waitForService
0x...2384  mov  x25, x0           ; lr：等待的返回地址
0x...2388  ldr  x8, [x26]         ; 取字典的 isa
0x...238c  ldr  x8, [x8, #0x28]   ; pc：取 vtable 里的 release 槽
0x...2390  mov  x0, x26
0x...2394  blr  x8
```

`pc 0x...238c` 就是第二次释放字典时读 vtable 的那一条，`lr 0x...2384` 是它前一条 `waitForService` 的返回地址。

原因在 `IOService::waitForService()` 本身：

```cpp
result = waitForMatchingService(matching, timeoutNS);
matching->release();      // 调用者的字典在这里被释放一次
if (result) result->release();
```

而 `IOFindBSDRoot()` 在每次等待前都 `matching->retain()`，等完再统一释放，正是为了抵掉这一下。round116 先 `waitForService()`、又 `OSSafeReleaseNULL(matching)`，把字典释放了两次；第二次读到的 vtable 已经不是 `OSDictionary`，于是数据异常。

这条读数的价值在于**它前面的每一步都过了**：

- 自动树在 1 秒内给出了名为 `disk0s39` 的 `IOMedia`（否则 `auto` 就 panic 了）；
- 三个属性都能转成 `OSString`/`OSNumber`（否则 `bsd-type`）；
- `IOBSDNameMatching()` 返回了非空字典（否则 `root-match 0`）；
- `waitForService()` 正常返回（否则走不到释放）。

所以 v0.2.88 的设计成立，只是收尾多释放了一次。块栈、自动分区方案、BSD 命名都没有问题。

## v0.2.89（round117）：匹配字典只释放一次

只改第 6 步：拿到 `IOBSDNameMatching()` 的字典后先 `retain()`，`waitForService()` 返回后再 `OSSafeReleaseNULL()` 一次，与 `IOFindBSDRoot()` 的用法逐字对应。其余步骤、超时、日志格式都不动。

这一轮剩下的问题是 `waitForService()` 的返回值和后面的两步：

| 失败行 | 含义 |
| --- | --- |
| `root-match <ptr>` | 名字匹配等到的不是这个媒体。`0` = 1 秒超时没人应答；非 0 = 等到了另一个服务，指针就是它 |
| `bdevvp <err>` | `bdevvp(major << 24 | minor)` 失败，`err` 是返回的错误码 |
| `buffer` / `open-whole` / `read-whole` / `open-part` / `read-part` / `map` | 读块或偏移比较失败；`map` 是整盘@base 与分区@0 的内容不一致 |
| `auto <n>` / `bsd-type` | 与上一轮相同 |

成功行：

```text
CloverBlock r117: auto p=39 disk0s39 1:39 bd=1 map=1
```

`rd=` 仍是 `md0`。`CloverPMGR` 只把行首标签升到 `r117`，参数行内容不变。

产物（内核与 payload 变了；root / trust cache / boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round117-payload.img` | 58720256 B | `77d50eb25da74658c578d78224e6baef88d0990e2502fd88da6c47c36b2d5882` |
| `round117-kernel.macho`（构建产物） | 51974144 B | `aabc5143c4983310f195147978e86997f298d5efd469cc555bc071a4948560b4` |
| `round117-in-volume-kernel.macho`（卷内副本） | 51974144 B | `d1ad310ca5fadc97d4b0edfac5a615b118f6b68a203714242f35c5ca312f3e17` |
| `round117-kernel.macho.adt` | 13476 B | `3a8227d6aa6e55015e0aad1f29760c60695282213c8bf3e1ce4b9278b9692685` |
| `round117-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round117-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round117-unixnu.toml` | 558 B | `1b0367775cecd587d0ab8a6f22527a1e5c7585736aa17c0dfd225458f8e8e148` |

校验器：`check-clover.py` 通过（ADT 13476 字节、8 核、splash `0x9d400000-0x9f800000`、FD `0x9fc00000-0x9ff00000`、`clover_defconfig`、`CLOVERESP`、模块可见）；`verify-clover-payload.py` 通过（probe 补丁只落在本地化、PMGR 绑定仍在、CloverSdhci 已链接）；`CloverBlock` 导入与 vtable 审计通过（264584 字节、190 个槽、26 个覆写）。卷内 boot-args 末尾仍是 `… clover-pmgr=2 clover-emmc=21 armcpu-clock-panic=60000`；卷内内核只有 r117 字符串，没有 r116 残留。反汇编确认新序列先 `matching->retain()` 再 `waitForService`，返回后才释放。

内核符号：`_sheng_pmgr_run` `0xfffffff007971274`、`_clover_emmc_run` `0xfffffff00797231c`、`_clover_block_run` `0xfffffff007992054`、`_clover_block_request` `0xfffffff007992894`。boot 仍是 `round87-boot.img`（`196b284f…`）。

```sh
fastboot flash cache ~/clover-out/round117-payload.img
fastboot reboot
```

**一次刷机只回答一个问题：只释放一次匹配字典之后，`waitForService` 会不会返回的正是 `disk0s39`，并让 `bdevvp` 与偏移比较都通过。** 三条验证都通过后，下一轮才把 `rd=` 换成 `disk0s39` 去碰真正的根挂载。

## round117 读数（v0.2.89 实机）：通知重投让第二次 run 撞上重复类

照片不是 `CloverBlock` 的 `fail()` 行，也不是成功行，而是一屏 data abort 异常面板：

```text
pc 0xfffffff007a231a4
lr 0xfffffff00782ef6c
```

把这两个地址对着 `round117-kernel.macho` 反汇编，落点很清楚：

- pc 落在 `OSMetaClass::postModLoad.cold.1` 里面，也就是冷路径上那条重复类 panic 分支；它要读的是 `orig->reserved->kext`，而 `reserved->kext` 是 0。
- lr 是 `postModLoad` 正常体里 `bl` 的返回地址，即「刚调完 `preModLoad` 就进了冷路径」。

要让这条分支成立，`postModLoad` 必须被调用第二遍，而 `sAllClassesDict` 里已经有 `CloverBlockDevice`。也就是说 `clover_block_run()` 在这台机器上**跑了两次**：

1. 第一次走完 6 步，注册 `CloverBlockDevice`，建 `IOBlockStorageDriver`/`IOMedia`，拿到 `disk0s39`，全部通过 —— 屏幕上启动日志一路走到 APFS 和 launchd，就是它成功的证据；
2. 第二次又进 `preModLoad` → `clover_block_static_init()`（重建 `gMetaClass`，把 `reserved->kext` 重新写成 NULL）→ `postModLoad`，这一次 `sAllClassesDict` 命中，走 panic 分支，读 `reserved->kext` 就是空指针。

第二次是怎么来的，是这一轮真正学到的东西。`clover_block_request()` 用 `IOService::resourceMatching("IOBSD")` 挂 `gIOPublishNotification`，而 `resourceMatching` 匹配的是**常驻的 `gIOResources` 服务**，不是某个一次性对象。XNU 那边：

- `IOService::doServiceMatch()` 每趟都会把仍在匹配的 `gIOPublishNotification` 重新收集、重新调用一遍；
- 调用点是 `invokeNotifier(notify)`，**返回值被丢掉**，所以 handler 里 `return false` 并不能退役这个 notifier；
- `IOKitBSDInit.cpp` 里每次 `publishResource()`（第 113、126、808、909 行等）都会重新匹配 `gIOResources`，于是通知一而再地投过来。

代码里原来那句注释「one shot: the resource is published once per boot」因此是错的，本轮已改。v0.2.89 的 release 修复本身没问题，只是它挡不住第二次进入。

## v0.2.90（round118）：闩锁让注册只发生一次

只改一处：`clover_block_run()` 开头加一对原子计数，让函数体**只被放行一次**，其余步骤、超时、日志格式都不动。

```cpp
uint32_t fires = __atomic_add_fetch(&gCloverBlockFires, 1u, __ATOMIC_RELAXED);
uint32_t expected = 0;
if (!__atomic_compare_exchange_n(&gCloverBlockRan, &expected, 1u, false,
                                 __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
  IOLog("CloverBlock r118: refire n=%u (already ran)\n", fires);
  return;
}
```

`__atomic_*` 是 GCC 内建，不需要头文件，和本仓库 `CloverSdhci.cpp` 里 `gBlockGuard` 用的那对 `__atomic_load_n`/`__atomic_compare_exchange_n` 是同一套写法。handler 依旧 `return false`，但注释现在写的是实情：false 不会退役 notifier，是闩锁保证只跑一次。

行数自证：成功行末尾多了 `fires=<k>`（跑通那一趟通常是 1），后面每一次重投各打一行 `refire n=<k> (already ran)`。一张照片同时回答「注册跑完了吗」和「被投了几次」。

| 失败行 | 含义 |
| --- | --- |
| `notify` / `auto <n>` / `bsd-type` / `root-match <ptr>` / `bdevvp <err>` / `buffer` / `open-*` / `read-*` / `map` | 与 v0.2.89 完全相同，只是标签升到 `r118` |

成功行：

```text
CloverBlock r118: auto p=39 disk0s39 1:39 bd=1 map=1 fires=1
```

`rd=` 仍是 `md0`。`CloverPMGR` 只把行首标签升到 `r118`，参数行内容不变。

产物（内核与 payload 变了；root / trust cache / boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round118-payload.img` | 58720256 B | `3a9f91d89443fcf92a1f91ddce384e528652ea33b875b7a894eec43af4b8de3b` |
| `round118-kernel.macho`（构建产物） | 51974144 B | `5ed34258c60e2bc03cea01b16ae873241100553ac4c1e98cb0cfc138265ee856` |
| `round118-in-volume-kernel.macho`（卷内副本） | 51974144 B | `94cc3ac6109fd3b0d4c8b30b784478b3d5b823694623028a97aca065c80a8e0b` |
| `round118-kernel.macho.adt` | 13476 B | `0e25bd558a9c9698c66262e09c28c2601bd770e6a3556e24405d0a7de9df88d4` |
| `round118-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round118-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round118-unixnu.toml` | 562 B | `def713de19bb1b050747a0e868c60c6cebdb7ddd4bf400be6cc129b3c6274dd1` |

校验器：`check-clover.py` 通过（ADT 13476 字节、8 核、splash `0x9d400000-0x9f800000`、FD `0x9fc00000-0x9ff00000`、`clover_defconfig`、`CLOVERESP`、模块可见）；`verify-clover-payload.py` 通过（probe 补丁只落在本地化、PMGR 绑定仍在、CloverSdhci 已链接）；`CloverBlock` 导入与 vtable 审计通过（265592 字节、190 个槽、26 个覆写）。卷内 boot-args 末尾仍是 `… clover-pmgr=2 clover-emmc=21 armcpu-clock-panic=60000`；卷内内核只有 r118 字符串，没有 r117 残留。反汇编确认第 6 步仍是 `matching->retain()` → `waitForService` → 释放一次，而函数入口先是 `__atomic_add_fetch` 再是 CAS。

内核符号：`_sheng_pmgr_run` `0xfffffff007971274`、`_clover_emmc_run` `0xfffffff00797231c`、`_clover_block_run` `0xfffffff007992054`、`_clover_block_request` `0xfffffff0079928e8`。boot 仍是 `round87-boot.img`（`196b284f…`）。

```sh
fastboot flash cache ~/clover-out/round118-payload.img
fastboot reboot
```

**一次刷机只回答一个问题：加了闩锁之后，`clover_block_run()` 是否只执行了一次，让注册不再撞重复类，并把 `CloverBlock r118: auto … fires=` 这条成功行留在屏幕上。** 只要这一行出现（且没有第二次 `preModLoad`），下一轮就把 `rd=` 换成 `disk0s39` 去碰真正的根挂载。

## round118 读数（v0.2.90 实机）：注册只跑一次，启动第一次走到 EL0 的 mach 行

这一屏没有异常面板，从上到下是一条完整的启动日志（挑出与块栈有关的部分）：

```text
[Clover] IOKitBSDInit returned
ifnet_attach: All kernel I/O threads created for interface lo0 have been scheduled at least once, Proceeding,
[Clover] loopattach returned
[clover] ether_family_init returned
skip attaching fsw to lo0 using legacy VX model(clover) net_init_run returned
PMGR: pmgrInit done!
AMFI: Enabling developer mode since protected data is not available
apfs_module_start:3232: load: com.apple.filesystems.apfs, v2236.140.6, apfs-2236.140.6, 2025-07-11
CloverBlock r118: auto p=39 disk0s39 1:39 bd=1 map=1 fires=1
Setting up notifier for CoreAnalyticsNub
AppleImage4: magazine(cptx): magazine data cannot be written: 30
AppleImage4: magazine(zlns): magazine write failed; may be expected: 30
[System Event] [Subsystem: nvmall] [Event: sync triggered
TOUCHPANELCHandler.cpp:serialIzzeVariables:615 - No _nvramController
TOUCHPANELCHandler.cpp:sync:894 - serializeVariables failed, ret=0x00002d0
install_apfs_vfs_init: apfs: speculative telemetry feature ENABLED by boot-arg
apfs_syscall_register:4007: apfs registering sysctls.
CloverBlock r118: refire n=2 (already ran)
Adding ramdisk device md0/rdmd0 (83886080/83886080) at FFFFFFF008395000 for 0000000000009000
BSD root: md0, major 3, minor 0
[clover] v4_mount returned
[clover] devfs_kernel_mount returned
[clover] siginit returned, execing init
VM Swap Subsystem is ON
load_init_program: attempting to load /usr/appleinternal/sbin/launchd.development
load_init_program: attempting to load /sbin/launchd
[clover] el0 init: pid=1 console_fd=0 pagesize=4096
[clover] el0 init: forked pid=2 x!=0
[clover] el0 child: alive
[clover] el0 child: exec /sbin/launchd
[clover] el0 init: pid=2 console_fd=1 pagesize=4096
[clover] el0 mach: thread=259 task=575 host=771
[clover] el0 mach: reply=1827
[clover] el0 mach: msg=
```

三条结论：

- **闩锁按设计工作。** 成功行 `auto p=39 disk0s39 1:39 bd=1 map=1 fires=1` 打在最前面，
  后面只有一行 `refire n=2 (already ran)`。也就是说这一趟启动里 IOBSD 通知被投了两次，
  第一次跑完六个步骤，第二次被 CAS 挡在门外 —— round117 的第二次 `preModLoad` 从此不存在，
  `postModLoad` 的重复类 panic 也就没有了。
- **块栈的时间点是对的。** 两行 `CloverBlock` 都夹在 `apfs_syscall_register` 与
  `Adding ramdisk device md0/rdmd0` 之间，也就是根设备发现阶段之内：XNU 自己在找
  `rd=` 的时候，我们的盘已经在下面服务了。`bd=1 map=1` 说明整盘@base 与分区@0 读到的
  那一块 512 字节逐字节相同，`bdevvp(1<<24|39)` 也拿到了 vnode。
- **启动走到了用户态。** `siginit returned, execing init` → `load_init_program` →
  `[clover] el0 init` → `el0 child: exec /sbin/launchd` → `el0 mach: thread/task/host`、
  `reply=1827`，最后停在 `msg=`。这是 mockfs + `rd=md0` 那条路的已知终点，不是块栈的失败。

顺带记下的既有噪声（与本轮无关，都是 sheng 板子的老面孔）：`AppleImage4: magazine … cannot
be written: 30`、`TOUCHPANELCHandler … No _nvramController`、`nvmall sync … ret=0x00002d0`、
`IOGPUInterruptController not registry mount at registerService()`。

## v0.2.91（round119）：把根交给内核点名的那个分区

只翻一个 boot-arg：`rd=md0` → `rd=disk0s39`。`CloverBlock` 的六个步骤、闩锁、日志格式一行都不动，只把身份串升到 `r119`。

先确认这件事**不会自锁**。`rd=` 不是 `md?` 形状时，`IOFindBSDRoot()` 跳过 `mdevadd` 那条捷径（IOKitBSDInit.cpp:849），改用 `IOBSDNameMatching("disk0s39")`（:917）去 `waitForService()` 等（:958，超时 `ROOTDEVICETIMEOUT`，最多十次，iOS 分支十次之后 `panic("Failed to mount root device")` :984）。等的是媒体，而媒体早就在了：本驱动的 run 由 `IOKitBSDInit()` 里那句 `publishResource("IOBSD")`（:113）触发，round118 的照片已经把时序钉死 —— 两行 `CloverBlock` 都落在 `apfs_syscall_register` 与 `Adding ramdisk device md0` 之间，也就是根设备查找开始之前。所以这一趟 `waitForService` 应当立刻返回，接着打 `Got boot device = <服务路径>` 与 `BSD root: disk0s39, major 1, minor 39`，`rootdev = makedev(1, 39)`。

真正被问的是**之后**那一步。`vfs_mountroot()` 对 `rootdev` 做 `bdevvp()`，然后遍历 vfstable：只有 mockfs 带 `vfc_mountroot`，于是 `mockfs_mountroot()` 用 `buf_meta_bread(rvp, 0, 4096)` 从**设备**读第一个 4096 字节（mockfs_vfsops.c:134）。这条读要穿过 specfs/缓冲区缓存/`VOP_STRATEGY`/`IOMediaBSDClient`/`IOBlockStorageDriver`，最后才是 CloverSdhci 的 PIO —— 是本板第一次做不由 `CloverBlock.cpp` 自己发起的读。round118 证过 `media->read()`（`map=1`）和 `bdevvp()` 本身（`bd=1`），唯独没证过拿到的那个 vnode 能不能读。

logfs 开头**故意不放镜像**。mockfs 读不到 `N4KROOT` 魔数就退回它的 legacy 单 Mach-O 模式：`/sbin/launchd` 就是整块设备从 0 开始（`mockfs_fsnode.c:82` 把文件大小设成 `si_devsize`，也就是这 8 MiB）。挂载应当成功、exec 应当因为读到垃圾而失败 —— 这一刀正好把「根管道通了」和「那里还没有镜像」切开。镜像要写进 logfs，是破坏性操作，等它单独一轮、单独一次同意。

| 屏幕上的行 | 含义 |
| --- | --- |
| `Got boot device = …` + `BSD root: disk0s39, major 1, minor 39` | 匹配与编号都对，`rootdev` 已经指向 eMMC 分区 |
| `initproc failed to start … (error N)` | 根已经挂在 eMMC 上，只差镜像 —— 本轮想要的读数 |
| `Failed to mount root device`（panic） | 根查找时媒体还不在，时序假设错了 |
| `CloverBlock r119: <step> <value>` | 与上一轮相同，只是标签升到 `r119` |
| 停在 `Waiting on <dict … IOMedia …>` | `rd=` 没送到内核，或名字没被发布 |

产物（内核、payload、ADT、unixnu.toml 变了；root / trust cache / scratch / boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round119-payload.img` | 58720256 B | `fad9ef7c6f6b21857cb125288f741705745a341b2166adfc3c8e0fa7ecbe71ac` |
| `round119-kernel.macho`（构建产物） | 51974144 B | `4934b8ad5abd6db1046852a502800ddf1145fd27f38e6bf65b5c869a6a22934f` |
| `round119-in-volume-kernel.macho`（卷内副本） | 51974144 B | `79f041afc1d12703f157afbd09e66534ae349180b7be8bc911141a0fd8a926bd` |
| `round119-kernel.macho.adt` | 13476 B | `d96c13d5cdf11697646a83845439540b5d91f36fb76e0a8398b156199023fa97` |
| `round119-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round119-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round119-unixnu.toml` | 571 B | `04d5770e8f7c0cc16847b3b0f887d45d96cf965e27d1c4478c5bef92e4d7dc8c` |

校验器：`check-clover.py` 通过（ADT 13476 字节、8 核、splash `0x9d400000-0x9f800000`、FD `0x9fc00000-0x9ff00000`、`clover_defconfig`、`CLOVERESP`、模块可见），它的根契约断言这次要求的是 `rd=disk0s39`；`verify-clover-payload.py` 通过（probe 补丁只落在本地化、PMGR 绑定仍在、CloverSdhci 已链接）；`CloverBlock` 导入与 vtable 审计通过（265592 字节、190 个槽、26 个覆写）。卷内 `args` 已是 `-v serial=3 debug=0x14e kt_fixed_zones=120 boot-progress=2 rd=disk0s39 allow_legacy_mach_msg=1 armcpu-timer-intid=19 clover-pmgr=2 clover-emmc=21 armcpu-clock-panic=60000`；卷内内核只有 r119 字符串（4 处），没有 r118 残留。菜单标题改成「clover emmc: root on the partition the kernel named」。

内核符号：`_sheng_pmgr_run` `0xfffffff007971274`、`_clover_emmc_run` `0xfffffff00797231c`、`_clover_block_run` `0xfffffff007992054`、`_clover_block_request` `0xfffffff0079928e8`（与上一轮逐个相同：这轮只换了字符串，长度没变）。boot 仍是 `round87-boot.img`（`196b284f…`）。

`armcpu-clock-panic=60000` 仍然挂着，这是有意的：万一 vnode 那条读路真的挂住，它是本板唯一不受 tty 影响的报告者。round118 之后它按点到过枪 —— `ArmCpu: clock watchdog at 60000 ticks (elr=10000b3c spsr=…50 …)`，`spsr` 低 4 位是 0（EL0t）、`elr` 不是 `0xfffffff0…` 的内核地址，说明开枪时被打断的是**用户态**，也就是启动已经跑完、用户在跑。等根真挂上之后再把它摘掉。

```sh
fastboot flash cache ~/clover-out/round119-payload.img
fastboot reboot
```

**一次刷机只回答一个问题：`bdevvp(1<<24|39)` 拿到的那个 vnode，XNU 自己读得动吗 —— 也就是 mockfs 经缓冲区缓存读 eMMC 的头 4096 字节，能不能让根挂载走完。** 看到 `initproc failed to start` 就算通：那时唯一的缺件是分区里还没有镜像，下一轮再单独问「能不能把它写进去」。

## round119 读数（v0.2.91 实机）：`rd=` 被读进来了，也被丢了

照片上能辨认的是 `Failed to mount root device @IOKitBSDInit.cpp:984`，`ticks=0x0000000000000b0c`，也就是 2820 个 tick。这个数字先排除一种可能：它不是 `armcpu-clock-panic=60000` 那颗看门狗，而是根设备等待本身——十次超时之后 iOS 分支在 :984 开枪（`ROOTDEVICETIMEOUT` 一轮一轮地睡）。

然后是一条源码事实，把 v0.2.91 的整张预测表推翻了一半。`IOFindBSDRoot()` 确实解析了 `rd=`（IOKitBSDInit.cpp:781），但它在同一个函数里被丢了：

```cpp
data = OSDynamicCast(OSData, regEntry->getProperty( "root-matching" ));
if (data) {
    unserializedContainer = OSUnserializeXML((char *)data->getBytesNoCopy());
    matching = OSDynamicCast(OSDictionary, unserializedContainer);
    if (matching) { continue; }           // :790-797
}
...
if ((!matching) && rdBootVar[0]) {        // :888  —— matching 已经非空，整块跳过
    ...
    matching = IOBSDNameMatching( look ); // :917
}
```

也就是说等的一直是本板设备树里那颗从 sheng 抄来的卷 UUID（`root_match_volume_uuid = 04769D26-…`），而这颗板上没有任何服务带它。`grep -rn root-matching` 扫过 `.cache/xnu-work` 全树，只有 :790 这一处读点——这颗属性除了抢在 `rd=` 前面，什么都不做。

三条结论：

- round118 钉住的时序没坏，`disk0s39` 的媒体、BSD 名字、major/minor 都还在（那两行 `CloverBlock` 的读数本轮也仍按 round118 的形状出现）；本轮失败点不在块栈，在根查找选字典的那一步。
- 「根契约」从来不是两项而是一项半：payload 带镜像、ADT 留 `RAMDisk` 占位，这些都对，但**只要 ADT 替内核点了名，`rd=` 就是死文本**。v0.2.91 的预测表里那一行「停在 `Waiting on <dict … IOMedia …>`」写错了方向，实际会是 `Waiting on <dict …UUID…>`。
- vnode 那条读路本轮根本没走到，问题原封不动留着。

## v0.2.92（round120）：设备树不再替内核点名根

改动只有一处语义：`[payload.adt]` 的 `root_match_volume_uuid` 删除，ADT 里不再有 `/chosen/root-matching`。连带 `scripts/make-clover-adt.py` 的 `if args.root_match_volume_uuid:` 走空分支（脚本本身不用改），`check-clover.py` 从 `REQUIRED_CHOSEN` 里把它挪进新的 `FORBIDDEN_CHOSEN`，再断言 `[payload.adt]` 也不许带这颗键（`scripts/unixnu/payload.py:150` 会把键直接翻成 `--root-match-volume-uuid`），自检用的生成命令行同步摘掉。驱动只升身份串：`CloverBlock` 三处、`CloverPMGR` 一处 `r119` → `r120`。

**为什么是删掉而不是改写成 `BSD Name=disk0s39`。** 两处声明「哪块是根」正是本轮量出来的缺陷本身；而 `IOBSDNameMatching()`（:191）造的字典是 `{IOProviderClass=IOService; BSD Name=<名字>}`，和 CloverBlock 第 6 步自己构造并等过的那颗一模一样，round118 已经实机证明它在这块板上应答得来。留着 ADT 那条路等于把同一件事交给两条都没验证过的通道。

**从源码读出来的下一堵墙（这轮还没量到）。** 根一旦不是 `md0`，`bsd_rooted_ramdisk()`（bsd_init.c:450，只对前缀 `md0` 为真）就是假，`require_rootauth` 成立，于是内核在挂载成功之后对根 vnode 发 `FSIOC_KERNEL_ROOTAUTH`（:1072）。mockfs 的 vnop 表把 ioctl 接到 `err_ioctl`，返回 `ENOTSUP`（vfs_support.c:279），而 owner-trust 那条逃生门要 `XNU_OWNER_ROOT_AUDIT`/`XNU_OWNER_ROOT_TRUST` 编进 `bsd_init.o`，本板的 `clover_defconfig` 明确不开。所以本轮**最好的期望读数是 `rootvp not authenticated after mounting`（bsd_init.c:1098）**，它等价于「根已经挂在 eMMC 上了」；exec 那一步还轮不到出场。

| 屏幕上的行 | 含义 |
| --- | --- |
| `Waiting on <dict … BSD Name=disk0s39>` | ADT 让开了，`rd=` 第一次真正被用上 |
| `Got boot device = …` + `BSD root: disk0s39, major 1, minor 39` | 匹配与编号都对，`rootdev = makedev(1, 39)` |
| `rootvp not authenticated after mounting` | **本轮想要的读数**：缓冲区缓存那条读通了、mockfs 挂上了，只差认证 |
| `Failed to mount root device` | 还有别的东西抢在 `rd=` 前面（或媒体没等到） |
| `CloverBlock r120: <step> <value>` | 块栈自己出问题，与根契约无关 |
| 只有看门狗（60000 tick） | vnode 那条读挂住了；`elr=` 就是挂点的地址 |

产物（内核、payload、ADT、unixnu.toml 变了；root / trust cache / scratch / boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round120-payload.img` | 58720256 B | `dc9aa749c3073a91f5da86345a3d767d1efb3b24cef555c7636ffa7c2316023c` |
| `round120-kernel.macho`（构建产物） | 51974144 B | `2f43b4124b479e66c658b9e464d4594c56454ab8b01da8cfbe4138994bb04a9d` |
| `round120-in-volume-kernel.macho`（卷内副本） | 51974144 B | `6fdf9be1d180275ac84e148ec86cae903e85d2419e8165ca3a4aef1cfc9c2af6` |
| `round120-kernel.macho.adt` | 13268 B | `7840f41a986d4cb247bafd84ab16de9f1f46179299de5e09c436eada08b8c7c2` |
| `round120-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round120-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round120-unixnu.toml` | 571 B | `340036be1ba79a82b15f02aaa5571782ca82b21ebbf004c146f4a58148cec1ea` |

ADT 从 13476 掉到 13268 字节，少的正是那颗属性。校验器：`check-clover.py` 通过（打印 `ADT 13268 bytes`，根契约要求 `rd=disk0s39` 且 `[payload.adt]` 不带 `root_match_volume_uuid`）；`verify-clover-payload.py` 通过，卷内 ADT 13268 字节、`strings` 里 `root-matching` 出现 0 次；`CloverBlock` 审计通过（265600 字节、190 个槽、26 个覆写）。卷内 `args` 与上一轮逐字相同（`… rd=disk0s39 … armcpu-clock-panic=60000`），内核只有 r120 字符串 4 处、无 r119 残留。菜单标题改成「clover emmc: let the kernel's own rd= name the root」。

内核符号：`_sheng_pmgr_run` `0xfffffff007971274`、`_clover_emmc_run` `0xfffffff00797231c`、`_clover_block_run` `0xfffffff007992054`、`_clover_block_request` `0xfffffff0079928e8`（与上一轮逐个相同）。boot 仍是 `round87-boot.img`（`196b284f…`）。

一条工务记录，值得写下来：`out/clover/kernel/kernel.macho` 不只是产物，它还是 `build-clover-pmgr.py` 与 `build-clover-block.py` 的 `--reference` 输入（审计要拿上一轮链接好的内核比对符号）。本轮清产物时把它删了，构建立刻在 archive 10/12 报 `kernel.macho missing: build the kernel once without this knob first`，只能从归档 `round119-kernel.macho` 还原才继续得下去。删 `out/` 之前先想清楚谁在读它。

```sh
fastboot flash cache ~/clover-out/round120-payload.img
fastboot reboot
```

**一次刷机只回答一个问题：把 ADT 让开之后，`rd=disk0s39` 能不能让内核走到那个 vnode 上，经缓冲区缓存读 eMMC 的头 4096 字节并把 mockfs 根挂上。** 看到 `rootvp not authenticated after mounting` 就算通——那时挂载已经完成，下一轮单独问认证；还看到 `Failed to mount root device` 就说明另有东西抢在前面。

## round120 读数（v0.2.92 实机）：根挂在 eMMC 上了，只差认证

照片第一行就是本轮要的那一条：`rootvp not authenticated after mounting @bsd_init.c:1098`。

地址逐个钉回内核：

| 屏上的值 | 内核里的位置 | 含义 |
| --- | --- | --- |
| `RA=0xfffffff0079f02c4` | `_bsd_init.cold.5+0x0` | panic 的返回地址落在 `bsd_init` 的冷段里，也就是 :1098 那一句自己 |
| `fmt=0xfffffff0070bc5cd` | `__TEXT` 文件偏移 `0xb85cd` | 这颗指针指向的字面量逐字是 `rootvp not authenticated after mounting @%s:%d`，与屏上第一行对齐 |
| `LR=0xfffffff0076ac294` | `_bsd_autoconf+0x0` | 这是从栈帧 `+8` 槽里读出来的旧值，正好是函数入口，不是调用点，本轮不解释它 |
| `ticks=0xd3` | 211 | round119 是 `0xb0c`=2820（十次超时）。等待被立刻回答，正是「媒体早就在」的那个时序 |

排他的证据在主设备号上，而它已经在上一轮的照片里了。round118 的日志里，`CloverBlock r118: auto p=39 disk0s39 1:39` 与 `Adding ramdisk device md0/rdmd0 … BSD root: md0, major 3, minor 0` 出现在同一次启动里：内存盘拿到的主号是 `mdevadd()` 用 `bdevsw_add(-1, &mdevbdevsw)` 动态要的（memdev.c:665），跟 IOMediaBSDClient 的槽位不是同一个。所以本轮 `rootdev = makedev(1, 39)` 那个 vnode 只可能是 eMMC 分区的节点，不可能是内存盘 —— `vfs_mountroot()` 交给 mockfs 的 `buf_meta_bread(rvp, 0, 4096)` 确实走了缓冲区缓存 → `VOP_STRATEGY` → IOMediaBSDClient → IOBlockStorageDriver → CloverSdhci 的 PIO，并且返回了成功，挂载走完了。这是本板第一次由内核自己发起、而不是由 `CloverBlock.cpp` 里的调用发起的读。

屏上滚过的日志还留了两条边角事实：`using 4980 buffer headers and 4582 cluster 10 buffer headers`（缓冲池起来了，这条读要有它才有意义），以及 `No BMST boot_arg set` / `BootBMSTManifest: Error: Failed to find entry /chosen/boot-manifest-hash`（owner 的启动清单在这块板上确实不存在，见下）。`CloverPMGR r120: args [… rd=disk0s39 … armcpu-clock-panic=60000)` 自证了镜像身份；`pmgr=0x5 status=0 mode=0x2`、`nub=… attached=1 registered=2`、`aon=0xffffffff=>0xffffffff` 与上一轮一致，PMGR 那条路没被本轮动过。

于是墙只剩一道，而这一道墙在源码里只有两个门。

第一个门是名字。`bsd_init.c:1057` 用 `bsd_rooted_ramdisk()` 决定要不要认证，那个函数只看 `rd` 是否以 `RAMDEV` 开头，而 `RAMDEV` 是 `bsd_init.c:447` 的 `"md0"`。`IOFindBSDRoot` 的 `md?` 捷径另有形状要求 `rdBootVar[3] == 0`（:849），所以像 `md0s39` 这样的名字**不会**走内存盘捷径，仍然按 BSD 名字匹配到我们发布的媒体、拿到 `1:39`，却让前缀检查把整段认证跳过。`grep -rn bsd_rooted_ramdisk` 全树另一个读者只有 `exclaves_storage.c:219`。代价是要 `CloverBlock` 在媒体上多发布一个 `BSD Name`，全在板内代码，不碰上游。

第二个门是上游自己的 owner 链：`patches/xnu/offline-owner-root-binding-audit.patch` 里 `XNU_OWNER_ROOT_TRUST` 认 `native_verified_block_root_authorized(device)`。那个函数只在 `drivers/NativeVirtioBlock` 有定义，构建脚本又把它锁在 `XNU_VERIFIED_ROOT_PROBE` + `XNU_SOURCE_CORECRYPTO14` + `XNU_NATIVE_VIRTIO_BLOCK` 三道门后（build-xnu-4k-baseline.sh:252-269），而它的实现是沿 provider 链找 `NativeVirtioBlock` 实例（NativeVirtioBlock.cpp:285-303）——本板的 provider 链是 CloverBlockDevice 与 CloverSdhci，除非改上游驱动，它永远回答「不认」。sheng 能挂真根，靠的是这一整套（`docs/zh/devices/sheng.md` 的 defconfig 段：`CONFIG_XNU_OWNER_ROOT_AUDIT=y`、`CONFIG_XNU_OWNER_ROOT_TRUST=y`、`CONFIG_XNU_NATIVE_UFS=y`、`CONFIG_XNU_SOURCE_CORECRYPTO14=y`）加上真 userdata 上的 APFS 卷，不是 mockfs 镜像。

本轮不改代码：读数落盘，`v0.2.92` 的产物继续有效。下一轮在两个门里选一个 —— 这是方向决定，单独问一次，不替用户挑。

还有第三条看着最短的路，先在这里判死：在设备仓放 `patches/xnu/*.patch`，直接改 `bsd_init.c` 的判定或 mockfs 的 ioctl 表。`tree/patches/xnu` 是指向基仓的软链，`feeds.py` 按 `(kind, name)` 取第一个存在的路径，所以设备仓一建 `patches/xnu` 就会整体顶掉上游上百个补丁 —— v0.2.58 那轮已经为这件事付过学费，仪器因此才进设备仓已拥有的 `drivers/ArmCpu`。这条路不在候选里。

## v0.2.93（round121）：给 eMMC 上的根起个内核不用验证的名字

round120 把墙量清楚了：根确实挂在 eMMC 分区上，拦路的只有 `bsd_init.c:1098` 那颗认证。这轮走的是**改名**这条路：CloverBlock 在第 9 步把这块媒体的 `BSD Name` 从存储族自己写的 `disk0s39` 改写成 `md0s39`，`rd=` 跟着改成同一个名字。`bsd_rooted_ramdisk()`（bsd_init.c:450）只比 `rd` 的前三个字符和 `RAMDEV`（:447，就是 "md0"），于是 `require_rootauth` 那一整块被跳过；而 `IOFindBSDRoot` 的内存盘短路要求 `rdBootVar[3] == 0`（:849），`md0s39` 过不了这条，所以根的来路一点没变：仍然走 :888 的 BSD 名分支、仍然按这块媒体自己的 `BSD Major`/`BSD Minor` 凑出 `makedev(1, 39)`。字节仍然出自 eMMC，变的只是内核被告知「这是个内存盘」。

**这是绕过，不是通过。** 代价写在这里：内核被谎报了根的位置，而 mockfs 那份镜像本来就没有封条可验。第 9 步要问的是谎报之后前面是什么——按源码推断是 exec：`init_programs[]`（kern_exec.c:7977-7985）里 `/sbin/launchd` 全load不动时，:8104 会 panic `Process 1 exec of %s failed, errno %d`。分区头 4096 字节现在还是没有镜像，所以那一步大概率失败，但那正是下一个可量的东西。

改动四处：`drivers/CloverBlock/CloverBlock.cpp` 加第 9 步（快照旧名 → `setProperty("BSD Name", "md0s39")` → 读回比对 → 用新名字现造一颗 `IOBSDNameMatching` 字典、`retain()` 后 `waitForService()` 一秒，要求解出的对象就是这块媒体）；`board.toml` 的 `rd=` 与菜单标题；`scripts/check-clover.py` 的根契约改成要求 `rd=md0s39` 并禁止残留的 `rd=disk0s39`；驱动身份串 `r120` → `r121`（`CloverBlock` 四处、`CloverPMGR` 一处）。前八步一行未动，第 5 步仍按 `disk0s39` 找媒体、第 8 步仍按原名比对，所以第 9 步的失败不会被误读成块栈的问题。

这轮另外读了三处源码，都是「改名到底动了谁」的前置：`bsd_rooted_ramdisk()` 在全树只有两个读者（bsd_init.c:1057 与 exclaves_storage.c:219），本板走不到 exclaves 那条；`FSIOC_KERNEL_ROOTAUTH` 连同那颗 panic 整体都在 `if (!bsd_rooted_ramdisk())` 的花括号里（:1057-1098），所以前缀命中是真的能整个跳过而不是少跳一段；`IOFindBSDRoot` 匹配成功之后从服务上取 `kIOBSDNameKey`/`kIOBSDMajorKey`/`kIOBSDMinorKey` 再 `makedev`（:1013-1042），不查 `/dev` 节点名，所以 devfs 里那个仍叫 `disk0s39` 的节点不会来搅局。`mdevadd(-1, …, 0)`（:843）的设备号是显式传参、不读 `rd=`，RAMDisk 占位符照旧。

| 屏幕上的行 | 含义 |
| --- | --- |
| `CloverBlock r121: auto p=39 disk0s39>md0s39 1:39 bd=1 map=1 fires=1` | 第 9 步写完、读回、别名匹配三关全过 |
| `BSD root: md0s39, major 1, minor 39` | 别名被根搜到，编号还是 eMMC 那一套 |
| `Process 1 exec of /sbin/launchd failed, errno <n>` | **本轮想要的读数**：认证跳过去了，boot 跑到 exec，卡在分区里还没有镜像 |
| `rootvp not authenticated after mounting` | 前缀测试没看见这个名字，`rd=` 与属性不一致 |
| `Failed to mount root device` | 没有任何服务带 `md0s39`：改名或别名匹配没成 |
| `CloverBlock r121: alias / alias-read / alias-match <值>` | 第 9 步自己出问题（写属性、读回、别名解出别的对象；最后一行带解到的指针） |
| `CloverBlock r121: <其它步骤>` | 块栈在第 9 步之前就出问题 |
| 只有看门狗（60000 tick） | 缓冲区缓存那条读挂住了；`elr=` 就是挂点的地址 |

产物（内核、payload、ADT、unixnu.toml 变了；root / trust cache / scratch / boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round121-payload.img` | 58720256 B | `d3ee588f31058d8eea3ee50dce5cc878addfdc886cf77dbb928791eb5b766ee5` |
| `round121-kernel.macho`（构建产物） | 51974144 B | `48a52153232c9e9160bf93e34d378993f98079d342f86ac57d11d3240dea338e` |
| `round121-in-volume-kernel.macho`（卷内副本） | 51974144 B | `559ab0323052b3eec4fa6c33e3177f660ec34174a436ff164e9c93430d3ee6e8` |
| `round121-kernel.macho.adt` | 13268 B | `7769674295e62a47145613378da156ac1d1699a6a4d6425c5c790fb811812a88` |
| `round121-kernel.macho.trustcache` | 56 B | `d310ae35d7a7e1d5aa23bcbce2c872405abb94e3bed349bfd71bc05ac9c314b4` |
| `round121-scratch-data.img` | 35456 B | `fe27be32fd5d9443cb516d16a55b8cb7ab48c5c30175ca61f320b80c36a2d084` |
| `round121-unixnu.toml` | 574 B | `450ca56000c0472d1ab7f4a82aa668c10f2c31358960e54a8c46b2ec6ac5e8e3` |

ADT 字节数仍是 13268、`root-matching` 仍出现 0 次，只有 `random-seed` 变了对应哈希。校验器：`check-clover.py` 通过（`clover ok: ADT 13268 bytes`，根契约要求 `rd=md0s39`、见到 `rd=disk0s39` 直接报错）；`verify-clover-payload.py` 通过；`CloverBlock` 审计通过（`libCloverBlock.a` 268216 B、190 个槽、26 个覆写，比上一轮大的就是第 9 步）。卷内 `args` 是 `… rd=md0s39 … armcpu-clock-panic=60000`，内核只有 r121 字符串 4 处、r120 残留 0 处。菜单标题改成「clover emmc: name the eMMC root md0s39 and skip the seal」。

内核符号：`_sheng_pmgr_run` `0xfffffff007971274`、`_clover_emmc_run` `0xfffffff00797231c`、`_clover_block_run` `0xfffffff007992054` 与上一轮逐个相同，`_clover_block_request` 从 `0xfffffff0079928e8` 移到 `0xfffffff007992a0c`（+0x124，第 9 步的代码就插在它前面）。boot 仍是 `round87-boot.img`（`196b284f…`）。

```sh
fastboot flash cache ~/clover-out/round121-payload.img
fastboot reboot
```

**一次刷机只回答一个问题：把根的名字报成 `md0s39`，`bsd_init.c` 的认证块跳过去之后，boot 能不能走到 exec。** 看到 `Process 1 exec of /sbin/launchd failed, errno <n>` 就算通——那时根、挂载、认证三关都在身后，下一轮单独问分区里的镜像；再看到 `rootvp not authenticated after mounting` 就说明前缀测试根本没看见这个名字。

## round121 读数（v0.2.93 实机）：认证跳过去了，exec 读到的是 eMMC 的字节

照片第一行就是本轮想要的 pass line：

```
Process 1 exec of /sbin/launchd failed, errno 8 @kern_exec.c:8104
```

`errno 8` 是 `ENOEXEC`。这一行同时结掉了三件事：`bsd_rooted_ramdisk()` 的前缀测试认了这个名字，整块 `FSIOC_KERNEL_ROOTAUTH` 被跳过（round120 那堵墙没了）；根搜到了别名并按它挂了 mockfs（否则这里会是 `Failed to mount root device`）；exec 真的从根上读出字节、并且读完了才发现不是 Mach-O（否则会是 `ENOENT`）。

地址逐个对回本轮的卷内内核（`round121-in-volume-kernel.macho`，`__TEXT` 在 `0xfffffff007004000`）：

| 屏上的字段 | 值 | 解出来 |
| --- | --- | --- |
| `fmt` | `0xfffffff0070c12d1` | 文件偏移 `0xbd2d1`，字面量就是 `Process 1 exec of %s failed, errno %d @%s:%d` |
| `RA` | `0xfffffff0076d68e8` | `_load_init_program + 0x1c0`，即 :8104 那颗 panic 自己 |
| `LR` | `0xfffffff0076ac3dc` | `_bsdinit_task + 0x68`，它的调用者，与 :8104 的栈一致 |
| `FP` / `args` | `0xfffffff0b77…` | 内核栈地址，不是符号 |
| `ticks` | `0x124` = 292 | round120 是 `0xd3` = 211；多出来的这段正是挂载之后走到 exec 的路 |
| `pages` / `free` | `0x3af7c` = 241532 / `0x3a121` = 237857 | 16 KiB 一页 ≈ 3.96 GB 总、3.89 GB 空闲：没吃进任何镜像，内存账本还是干净的 |

`load_init_program()` 对 `ENOENT` 是静默跳过的（:8099 `else if (error != ENOENT)` 才打 `failed loading …` 那行），而 panic 只报最后试的那颗（:8104 `init_programs[i - 1]`）。屏上既没有 `launchd.debug` 也没有 `launchd.development` 的失败行，最后落在 `/sbin/launchd` 且错号是 8 —— 也就是前两颗根本不存在（mockfs 那个 legacy 目录里只有一个名字），第三颗存在、可读、字节不对。这跟 round119/120 写在 `rd=` 注释里的 mockfs 形状严丝合缝：没有 `N4KROOT` 魔数就退化成「整块设备从 0 起就是 `/sbin/launchd`」。

还有一条排除值得记下来：万一这个根其实是 RAMDisk 上那份 mockfs 镜像，`/sbin/launchd` 会是真的、签名过的 Mach-O，exec 就往下走了，不会报 8。报 8 恰恰证明读上来的是 eMMC 分区自己的字节 —— 块栈、缓冲区缓存、mockfs、exec 这一整条链是通的，缺的只是分区里还没有镜像。

这一帧里 `CloverBlock r121: auto …` 和 `BSD root: …` 已经被滚出画面，但上面那串推论不依赖它们：别名没写成功就挂不上根，挂不上根就到不了 exec。

## round121 之后（同一份 v0.2.93 负载）：把根镜像写进 logfs，EL0 从 eMMC 起来

**这一轮没有构建任何新产物。** 板上的内核、payload、ADT、trust cache、`unixnu.toml` 与 round121 逐字相同，七个文件的 SHA-256 一个没变。改动的只有 eMMC 里 `logfs` 分区的内容：

```sh
fastboot flash logfs ~/clover-out/round121-scratch-data.img   # 35456 B，fe27be32…
fastboot reboot                                                # 冷刷
```

`round121-scratch-data.img` 就是 payload 里那份 scratch data 镜像，开头四个字节 `cf fa ed fe`（Mach-O 64 位小端），它的 cdhash 已经在 `round121-kernel.macho.trustcache`（`d310ae35…`）里 —— 也就是 mockfs legacy 模式下「整块设备从 0 起就是 `/sbin/launchd`」这句话，现在字面上成立了。

一次刷机回答的那一个问题：**上一轮报 `errno 8` 的那段字节，是不是只差内容？** 屏上给的是「是」：

```
Waiting on <dict … <key>BSD Name</key><string …>md0s39</string></dict>
Got boot device = IOService:/ArmDTPlatform/CloverBlockDevice/IOBlockStorageDriver/Clover eMMC Media/IOGUIDPartitionScheme/logfs@39
BSD root: md0s39, major 1, minor 39
 [clover] vfs_mountroot returned
 [clover] devfs_kernel_mount returned
 [clover] signit returned, execing init
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
[clover] el0 mach: msg=          ← 画面在这里被截断
```

| 屏上的行 | 为什么是新的 |
| --- | --- |
| `Got boot device = …/IOGUIDPartitionScheme/logfs@39` | 注册路径里带着 GPT 表项的名字和编号：命中的正是 39 号 `logfs`，而 `BSD Name` 已经是别名 `md0s39`。宿主侧那次 `fastboot flash logfs` 落对了地方 |
| `signit returned, execing init` | round121 卡在这之前的 `errno 8`；字节合法了，exec 走完 |
| `el0 init: pid=1` → `forked pid=2` → `el0 child: exec /sbin/launchd` | EL0 真的在跑，而且是从 eMMC 上那份镜像里跑起来的 |
| `el0 mach: thread=259 task=515 host=771` + `reply=1827` | round118 那三行 mach 握手第一次以「根在磁盘上」的身份重现；`reply=1827` 是拿到回复，不是超时 |
| `Added memory device md0/rdmd0 (03000000/BE000000) …` | RAMDisk 还在（major 3），但它不再是根。两回事同时成立，正是别名设计要的形状 |
| `CloverBlock r121: refire n=2 (already ran)` | 闩锁照常，重投只记账不重注册 |

`vnode_create_internal: alias vnode (vid = 818394427) … (start)/(end)` 两行、`VM Swap Subsystem is ON`、`No POST boot-arg set.`，加上画面顶部的 `AppleImage4: magazine write failed; may be expected: 30`、`IONVRAMCHRHandler.cpp:serializeVariables:615 - No _nvramController` / `ret=0xe00002d8`、`install_apfs_vfs_fs:8240`、`apfs_sysctl_register:1407`，都是此前每轮就在的常态噪声，本轮不解释。

一条 fastboot 工务记录，值得单独写下来，因为它推翻了一个看起来显然的推断：**`fastboot getvar partition-size:<name>` 的名字表不等于 `fastboot flash <name>` 的名字解析。** 本机 `getvar all` 只列 `vendor 0x80000000`、`cache 0x10000000`、`userdata 0xCAD753E00`、`system 0xC0000000` 四颗分区大小，`partition-size:boot` 与 `partition-size:logfs` 都回 `FAILED (remote: 'GetVar Variable Not found')` —— 而 `boot` 早就刷过、`logfs` 本轮也刷成功了（`Sending 'logfs' (34 KB) OKAY` / `Writing 'logfs' OKAY`）。所以「getvar 查不到」不能当「分区不存在」用。刷机时伴随一句 `Warning: skip copying logfs image avb footer (logfs partition size: 0, …)`，那是 fastboot 自己的大小表缺项导致的跳过，不是写入失败。

一条还没被量到的代价：`logfs` 是 8 MiB（round101 的 `lg i=39 a=12320768 b=12337151`，一万六千三百八十四扇区），镜像只有 35456 字节。mockfs 的 legacy 模式把整块设备当作那一个文件，所以「`/sbin/launchd`」的第 35457 字节往后仍是这个分区早先留下的自证记录。目前为止 launchd 只读到 `msg=` 那一行，还没暴露出越界读；真要用这条路由，下一轮要么把镜像补齐到分区大小，要么换成带 `N4KROOT` 魔数的正经布局。

```sh
fastboot flash logfs ~/clover-out/round121-scratch-data.img
fastboot reboot
```

**下一帧要问的：** `el0 mach: msg=` 后面被截断的那一行写了什么。它是本轮唯一没读回来的仪器数，也是「EL0 从 eMMC 起来之后，mach 握手到底交换了什么」这个问题的答案本体。

## round121 之后·第二帧（同一份负载）：看门狗到点，样本落在 EL0 自己那句 write 后面

第二张照片给的是同一次启动的下一时刻。`msg=` 那半行仍然没有下文，取而代之的是看门狗：

```
ArmCpu: clock watchdog at 60000 ticks (elr=100000b3c spsr=ffffffff14084cd50 elr2=100000b3c
       cnt0=124f800 cnt1=0 freq=124f800) @ArmCpu.cpp:632
pages=0x000000000003af7c   free=0x0000000000039530   ticks=0x000000000000ea60
ipi=0 sent=0 | irq=0x13 phase=0x3 intid=0x13 typer=0x7b04f3
```

先把 `elr` 解干净。我们那份 EL0 镜像（`build/clover-root/launchd.macho`，与刷进 `logfs` 的 `round121-scratch-data.img` 逐字节相同，`fe27be32…`）的 `__TEXT` 就在 `0x100000000`，没有 slide，所以 `0x100000b3c` = `_puts + 4`：

```
_write_console:                      _puts:
  mov x0, x22                          100000b38  bl  _write_console
  mov x16, #4            // write      100000b3c  mov x1, #0xe0   ← 样本
  svc #0x80                            100000b40  b.hs _fail
  ret                                  100000b44  ret
```

`spsr` 低四位是 0 ⇒ **EL0t**：中断打在用户态，不是某个 syscall 里面。`scripts/clover-el0-init.S` 里八处 `_puts` 全在 mach 探针那一段，而整段被 `cmp x19, #1` / `b.eq _skip_mach`（:425）挡在 pid 1 之外 —— 只有 exec 之后的 pid=2 会走；`_parent`（:1040）那一路的出声全直接调 `_write_console` 与 `_print_dec`，一次 `_puts` 都不经过。所以被打断的线程是孩子，而它刚刚从一次 `write()` 回来、连返回值都还没测。

于是这一帧内部是矛盾的，而且矛盾本身就是读数：`_puts` 返回后三条指令就是 round85 那颗自 panic（`mov x0,#0xc00` / `mov x16,#0x37` / `svc` 在 `0x10000061c`，`reboot(RB_PANIC|RB_QUICK, "clover-el0: write returned")`），屏上却没有 `userspace panic: clover-el0: write returned`，而 panic screen 只保留第一个 panic。⇒ 到 60000 tick 为止那颗 `reboot(2)` 没走完，可样本又说孩子正停在它前面四步的位置上。线性推进的解释不成立，下一帧必须说明这两件事里哪一件是画面在骗人。

两条附带账目：`free` 从上一帧的 `0x3a121` 掉到 `0x39530`（约 2844 页 ≈ 44 MB），说明这一帧确实晚于上一帧、且中间有真实分配发生；`cnt0 == freq == 0x124f800`、`cnt1 == 0` 是 round82 那节就记下的仪器缺陷（`gTick0Counter` 取到了 CNTFRQ 的值，`counter()` 在 panic 时读回 0），所以**这帧仍然给不出 60000 tick 是多久**，tick 数只能继续当启动进度坐标用。

```sh
# 负载与分区内容都没变，仍然只是冷刷
fastboot reboot
```

**下一帧要问的（一次刷机一问）：** 把 round85 那颗 `reboot(2)` 从「`msg=` 那次 write 之后」挪到「之前」——`scripts/clover-el0-init.S:651` 的注释早就写好了这一刀。看到 `userspace panic: clover-el0: before write`，就说明孩子能走到那儿、被吃掉的是那次 write；仍然只看门狗到点，就说明孩子根本没走到，而 `elr` 会告诉我们它在哪。

## v0.2.94（round122）：把那颗 reboot(2) 挪到吃掉 `msg=` 的那次 write 前面

第二帧留下的是一处自相矛盾，而矛盾本身是唯一的读数来源：看门狗在 60000 tick 打到的样本是 `_puts + 4`，`spsr` 低四位为 0 ⇒ EL0 用户态；而 `_puts` 的八个调用点全在 `b.eq _skip_mach` 之后，pid 1 一个都到不了 —— 被打断的必须是 exec 出来的孩子。孩子从 `_puts` 回来之后第四条指令就是 round85 那颗 `reboot(2)`，可 `userspace panic: clover-el0: write returned` 从来没上过屏，而 panic 屏只保留它收到的第一个 panic。两件事不能同时是「线性推进」，而放在那次 write 之后的任何代码都说不清这两半里哪一半是画面在骗人。⇒ 把调用搬到 write 前面。

改动只有一处，`scripts/clover-el0-init.S` 里那段 `msg=` 探针：`reboot(RB_PANIC|RB_QUICK, "clover-el0: before write")` 现在坐在标签 write 之前，原来的「write + reboot」整对留在下面当够不着的脚手架（本文件一直的做法是保留被替换的仪器，而不是删掉它）。新增的字节只有七条指令和一句 `.asciz`：

```asm
  adrp x1, _panic_msg_before@PAGE
  add  x1, x1, _panic_msg_before@PAGEOFF   // 0x100004179: "clover-el0: before write\0"
  mov  x0, #0xc00                          // RB_PANIC | RB_QUICK
  mov  x16, #55                            // reboot(int opt, char *msg)
  svc  #0x80                               // 0x100000608
  mov  x23, #450                           // 只有 syscall 回了 errno 才走到这里
  b    _exit_now
```

孩子每帧都走到这个点（它前面那行 `reply=` 每次都印出来），所以读数是两值的：panic 屏出现 ⇒ `reboot(2)` 从 EL0 打得通，这一条正是 round85 买来却没验过的前提；仍然只有看门狗 ⇒ 孩子根本没走到调用，而 `elr` 会告诉我们它究竟在哪。

| 屏幕上的行 | 含义 | 下一刀 |
| --- | --- | --- |
| `userspace panic: clover-el0: before write` | **本轮想要的两条之一**：EL0 → `reboot(2)` → `panic_kernel()` 这条通道在这台机器上是活的，与被吃掉的 write 无关 | 通道既然活着，回去问 write：把探针放回 write 之后、但让 `msg=` 的地址先经 panic 屏出声 |
| 只有看门狗，`elr` 落在 `_write_console`/`_print_dec`/`_puts` 之内（本轮 0xb08 / 0xb18 / 0xb54 附近） | 孩子仍然停在一次 write 里面 —— 堵点在 tty 输出路径 | 切点进 `vcputc_options` 那一侧 |
| 只有看门狗，`elr` 落在别处 | 「`_puts + 4` 就是孩子」这个推断本身错了 | 按新的 `elr` 重解 |
| `initproc exited … exit status 1` | `reboot(2)` 回了 errno（`x23=450`），父亲的报告带把它记成 1 | 有一层策略没编进来或被拒；把 errno 挤进报告带 |
| 出现 `el0 mach: msg=` | 不可能 —— 这行本轮写在调用之后，够不着；见了就是刷进去的镜像不是本轮那份 | 核对 `logfs` 内容与 trust cache 指纹 |

**内核一行没动。** 校验器把卷内 `kernel.macho` 读回来是 `559ab0323052b3ee…`，与 round121 逐字节相同（构建产物 `48a52153…` 也相同），变的只有 EL0 那份根镜像、它的 trust cache、负载和菜单标题。所以这一轮的成本是三行命令：重签根镜像 → 钉住新的 `trust_cache_sha256` → 重建 payload。

```sh
cd ~/UniXNU-clover && python3.13 scripts/make-clover-root.py     # launchd.macho + trustcache.bundle
# board.toml: trust_cache_sha256 换成新 bundle 的 sha256，title 换成本轮
cd ~/UniXNU && ./unixnu build payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-root/trustcache.bundle
```

产物（root 镜像 / trust cache / scratch / payload / unixnu.toml 变了；内核、ADT、boot 未动）：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `round122-payload.img` | 58720256 B | `8de1d10a77567f456fc53179bd652dd9fba43ae8bb67aebae3a4bf7bcb5e6052` |
| `round122-scratch-data.img`（= 新的 `launchd.macho`） | 35488 B | `890e94417bf79dfde8090e07a2ef13e606b8097eb65d6cd7d95dd34ee3339281` |
| `round122-kernel.macho.trustcache` | 56 B | `6062a6a45c2ff822563a3c6ef698fdef8222f13e4800a1cf02ec93818709a434` |
| `round122-in-volume-kernel.macho`（卷内副本，与 round121 相同） | 51974144 B | `559ab0323052b3eec4fa6c33e3177f660ec34174a436ff164e9c93430d3ee6e8` |
| `round122-kernel.macho`（构建产物，与 round121 相同） | 51974144 B | `48a52153232c9e9160bf93e34d378993f98079d342f86ac57d11d3240dea338e` |
| `round122-in-volume-kernel.macho.adt`（未动） | 13268 B | `4bb2cadb6b0302c9e25e046fb8b7de0c366590df38200ac659a1ffc6d1c65c88` |
| `round122-unixnu.toml` | 572 B | `a19e11806f87f7d98814ef1442d3a8d64ff48fdffdc036b4837dc452cb9bcdef` |

根镜像的 cdhash 是 `769d413c00980dc4f745be7f67c57c13122f4c13`，`board.toml` 的 `trust_cache_sha256` 已钉成上面那颗 bundle。校验：`check-clover.py` 通过（`clover ok: ADT 13268 bytes…`）；`verify-clover-payload.py` 通过，并逐条确认卷内 `scratch-data.img == 签名根镜像`、`trustcache == 本轮 bundle`、探针补丁 / PMGR 绑定 / `CloverSdhci` 链接全在。卷内 `args` 与 round121 逐字相同：`-v serial=3 debug=0x14e kt_fixed_zones=120 boot-progress=2 rd=md0s39 allow_legacy_mach_msg=1 armcpu-timer-intid=19 clover-pmgr=2 clover-emmc=21 armcpu-clock-panic=60000`。boot 仍是 `round87-boot.img`（`196b284f…`）。

新一份 EL0 镜像的符号表（`__TEXT` 仍在 `0x100000000`，无 slide；`0x100000604` 之后全部 +0x1c）：`_start` `0x380`、`_mach_done` `0x7b4`、`_skip_mach` `0x7c0`、`_parent` `0x9d8`、`_wait_error` `0xa84`、`_wait_reaped` `0xa8c`、`_exited_other` `0xab8`、`_not_exited` `0xac0`、`_signaled` `0xad8`、`_exit_now` `0xae4`、`_checkpoint` `0xaf4`、`_fail` `0xaf8`、`_write_console` `0xb08`、`_print_dec` `0xb18`、`_puts` `0xb54`、`_print_line` `0xb64`、`_mheader` `0xb94`（照片读到的 `elr` 要按这张表解，不要再用 round121 的 `0xb38`）。

三条已经记在账上的缺陷，本轮都不动它们：

- **父亲的 448 记号进不了八位通道。** `_wait_reaped` 先 `and x24, x24, #0xff` 再 `cmp x24, #448`（`.S:1257-1258`），掩过之后最大 255，这条比较永不成立 —— 于是 round84 那张表里的 `exit status 0` 从那轮起就是够不着的一行，孩子无论以 448、449 还是本轮的 450 退出，父亲都报 1。修法很小（比 `#192`，或让孩子用八位以内的码），但它要一次自己的刷机。
- `logfs` 是 8 MiB，镜像只有 35488 字节：mockfs 的 legacy 单文件视图仍然越过镜像尾巴读旧自证记录。
- `cnt0 == freq`、`cnt1 == 0` 的仪器缺陷仍在，所以看门狗那行仍然给不出 60000 tick 是多久。

```sh
fastboot flash cache ~/clover-out/round122-payload.img
fastboot flash logfs ~/clover-out/round122-scratch-data.img   # 写 eMMC 分区：本轮 exec 的是这份
fastboot reboot                                                # 冷刷
```

**一次刷机只回答一个问题：从 EL0 发一颗 `reboot(2)`，panic 屏收不收得到它。** 收到，就证明 round85 之后所有「孩子没走到」的推断都得推翻，`msg=` 那次 write 重新变成活题；收不到而看门狗照旧，就把 `elr` 交出来，按它重解孩子究竟卡在哪。

## round122 读数（v0.2.94 实机）：panic 屏收到了 EL0 的 reboot(2)

第一行就是本轮要的 pass line：

```
[9.941193]: OSMC subsystem is initialized
[9.942761]: debug_log_init: Error!! gPanicBase is still not initialized
userspace panic: clover-el0: before write
pages=0x000000000003af7c
free=0x00000000000394ad
ticks=0x0000000000000229
ipi=0x0000000000000000 sent=0x0000000000000000
irq=0x00000000000003ff phase=0x0000000000000000 intid=0x0000000000000013 typer=0x000000000007b04f3
pmgr=0x0000000000000005 status=0x0000000000000000 mode=0x0000000000000002
nub=0xfffffff1c8bdf800 attached=0x0000000000000001 registered=0x0000000000000002
aon=0x00000000fffffffc=>0x00000000ffffffff
RA=0xfffffff0079f539c
FP=0xfffffff70bf53cb0
LR=0xfffffff0079f539c
fmt=0xfffffff0070c444f
args=0xfffffff70bf53cc0
…a1=0xfffffff0079f539c   …a2=0xfffffff56256b000   …a3=0xfffffff0073e0b60
```

**通道是活的。** 两值读数落在第一支：EL0 的 `svc #0x80`（x16=55、x0=`RB_PANIC|RB_QUICK`、x1 指向自己 `__DATA` 里那句 `"clover-el0: before write"`）确实把内核 panic 了，而且 panic 屏把那句话原样画了出来。round85 买来没验的前提，到此结掉。

而且这一帧把整条链在中核那一侧也钉死了 —— panic 屏自己交出的寄存器逐个能解：

| 屏上的数 | 解出来 | 依据 |
| --- | --- | --- |
| `RA` = `LR` = `0xfffffff0079f539c` | `_panic_kernel + 0xf0` | `nm` 给 `_panic_kernel` = `0xfffffff0079f52ac`（下一个符号 `_reboot_kernel.cold.1` 在 `0xfffffff0079f53f0`，所以确实落在它体内）。`panic_kernel()` 在 kern_shutdown.c:153-168，`+0xf0` 就是 :167 那句 `panic_with_options(...)` 的返回地址 |
| `fmt` = `0xfffffff0070c444f` | 字符串 `userspace panic: %s` | `__TEXT` 的 `vmaddr` 是 `0xfffffff007004000`、`fileoff` 0 ⇒ 文件偏移 `0xc044f`，那里逐字节是 `\0userspace panic: %s\0Killing all processes ` |
| `FP` = `0xfffffff70bf53cb0`、`args` = `0xfffffff70bf53cc0` | 同一帧内核栈，`args = FP + 0x10` | `0xfffffff70b…` 是线程内核栈区，正是 `panic_with_options` 的 va_list 落点 |
| `a1` = `0xfffffff0079f539c`、`a3` = `0xfffffff0073e0b60` | panic 上下文的后续参数槽 | 与 `RA` 同值即返回地址被原样带进 panic record，属常态 |

顺带把源码那三处「为什么非 root 的 EL0 也能 panic」变成实测：`reboot()` 在 `suser()` 失败后有一段 `#if (DEVELOPMENT \|\| DEBUG)` 专门把 `error` 清零（kern_xxx.c:114-125），MACF 的 `mac_system_check_reboot` 在同一条件里被 `goto skip_cred_check` 跳过（:140-150），而 `kern_reboot()` 见 `(howto & (RB_PANIC\|RB_QUICK)) == (RB_PANIC\|RB_QUICK)` 直接调 `panic_kernel(howto, message)`（kern_shutdown.c:182-183）。我们传的 `0xc00` 正是这一对，且不含 `RB_PANIC_ZPRINT`(0x1000)/`RB_PANIC_FORCERESET`(0x2000)，所以 `opts` 保持 `DEBUGGER_OPTION_NONE` —— 屏上没有 zprint 段，与此一致。

**看门狗这次没参与。** `ticks=0x229` = 553，而 deadline 是 60000 —— panic 比看门狗早约 108 倍，所以 panic 屏没有「被看门狗抢先占位」的可能，round121 那帧「屏上只有看门狗」不能拿 deadline 当解释。

这一帧还第一次让 tick 数跟一个独立时钟同框：紧挨 panic 行上面的两条 IOKit 日志带绝对时间戳 `9.941193` / `9.942761` 秒。553 tick 对 9.94 秒 ⇒ **候选 tick 周期 ≈ 18 ms（约 55.6 Hz）**，那么 60000 tick ≈ 18 分钟 —— 与 round121 那两帧之间「等了很久才拍到第二帧」的经过时间相符。但这条只能当候选：round82 在 exec 内部 AMFI 检查处读到的是 `ticks=0x14e5`=5349，那是 initproc 进 EL0 的**前一步**，按 55.6 Hz 就是 96 秒，比本轮的 10 秒晚 —— 要么那轮启动真的慢一个数量级，要么这个计数器的速率不恒定。`cnt0 == freq`、`cnt1 == 0` 的缺陷仍然挡着直接测量，所以 tick 数继续只当启动进度坐标用。

其余读数与既往 panic 屏同类：`pages=0x3af7c` 三轮未变（总页数当然不变）；`free=0x394ad`，介于 round121 第一帧的 `0x3a121` 与第二帧的 `0x39530` 之间；`irq=0x3ff`（1023 = 没有中断在处理中）、`phase=0`、`intid=0x13`、`typer=0x7b04f3`、`pmgr=0x5 status=0x0 mode=0x2`、`nub=0xfffffff1c8bdf800 attached=0x1 registered=0x2`、`aon=0xfffffffc=>0xffffffff`，都是常态噪声。屏底部寄存器 dump 与 boot-args 回显那几行**互相压字**（画到屏子末尾了），只能勉强认出 `…zones=120 boot-progress=2 rd=md0s39 allow_legacy_mach_msg=1 armcpu-timer-intid=19 clover-pmgr=…`，本轮不需要它们。屏顶已滚掉，所以这张照片没有重读 `reply=` 那行 —— 但 panic 串本身就是孩子走到 `0x100000604` 的证据。

**于是 round121 那处矛盾换了嫌疑人。** `reboot(2)` 从 EL0 到 panic 屏只要约 10 秒，那么 round85 放在 `msg=` write 之后的那颗 `reboot(2)` 若被执行，屏上必然先有 `userspace panic: clover-el0: write returned`；它没出现 ⇒ 孩子没执行那条 `svc`。而看门狗那一帧的单点样本说孩子正停在 `_puts + 4`。两者之间现在只剩一个能活：**要么那次 write 根本没返回（样本不可信），要么 `elr` 不是「孩子此刻在哪」的忠实读数**。round121 那节里我按后者推，本轮的结果把它压回到前者一侧，但一次刷机只能结一题，所以这一题留给下一轮用通道本身来量。

**下一题（round123，仍然只改 EL0 镜像）：** 既然 panic 屏已经是一条能印字的通道，就别再拿它只当哨兵 —— 在孩子尾段每一次 write **之后**各放一颗 `reboot(2)`，各带一句不同的串（`w1`／`w2`／`w3`…）。panic 屏只保留第一个 panic，所以画出来的那句就指名「最后一次返回的 write」是哪一次，一次刷机多值。这样 `msg=` 那次 write 到底回没回来，就不再需要靠看门狗的单点样本去猜。

## v0.2.95（round123）：把 panic 屏从哨兵改成打印机 —— 尾段六档 reboot 阶梯

round122 结掉的是前提（EL0 的 `reboot(2)` 能在约 10 秒内把一句话送上 panic 屏），留下的是 round85 那一题：孩子尾段那几次 write，最后一次「回来了」的是哪一次。上一轮的读数表里，这一题只能靠看门狗那一个采样点去猜。既然通道已经证明能印字，本轮就不再用它当哨兵，而是当打印机。

**改动全部在 `scripts/clover-el0-init.S`，只删只加，不动任何驱动与内核。**

1. 删掉路径上那两颗孤立 panic：round122 放在 `msg=` 标签 write **之前**的 `reboot(2)` 和它的 `mov x23, #450 / b _exit_now`，round85 放在那次 write **之后**的 `reboot(2)` 和它的 `mov x23, #449 / b _exit_now`。两句旧报告串（`"clover-el0: write returned"`、`"clover-el0: before write"`）留在 `__DATA` 当存档，没有档位再用它们。
2. 在孩子尾段每一次 write 落地之后立刻放一颗 `reboot(2)`，各带一句指名「它紧跟哪一行」的串。六档，按程序顺序：

```
  sub x2, x2, x1
  bl _puts                        // 刚写完 "[clover] el0 mach: msg="
  adrp x1, _w_msg_label@PAGE      // 档 1：紧跟这次 write
  add x1, x1, _w_msg_label@PAGEOFF
  mov x0, #0xc00                  // RB_PANIC | RB_QUICK
  mov x16, #55                    // reboot(int opt, char *msg)
  svc #0x80
```

档位是六条而不是十一条，因为 `_puts` 只写半行、`_print_line` 补数字和换行，真正有意义的边界是「这一行的最后一个 write 也回来了」；`_m2_prercv` 那半行不单独立档，它被下一档（rcv 整行）覆盖。`_mach_done` 前面那档例外，它是「整段探测全部返回」。

| 屏上出现 | 含义 |
| --- | --- |
| `userspace panic: clover-el0: after write msg-label` | `msg=` 那半行的 write 回来了，此后没有任何 write 回来 —— 也正是 round121 看门狗样本 `_puts + 4` 指的那一次 |
| `… after write msg-line` | 完整一行 `msg=<address>\n` 都回来了：round78–83 的嫌疑人洗清，问题在它之后 |
| `… after write m2-presend` | 而且 `presend` 行也回来了 ⇒ 孩子正站在 msg2 send 那条 `svc` 前面 |
| `… after write m2-send-line` | 而且 send 的返回码行也回来了 ⇒ `mach_msg2_trap` 应答了，没杀线程 |
| `… after write m2-rcv-line` | 而且 `rcv=<code> size=<n>` 整行回来了 ⇒ 两次 trap 都过 |
| `… after write probe-done` | 整段探测（含 legacy send）每一次 write 都回来了 —— 这一档直接反驳 round84 那张照片 |
| 只有看门狗、没有 panic 屏 | 阶梯第一档那次 write 根本没返回。round121 的样本说的就是这句话，本轮把它单独切出来了 |
| 父亲那行的 `exit status` 对应 11<<8 | 六颗 `reboot(2)` 全都带 errno 返回了（否则 panic 会终止启动）⇒ `reboot(2)` 本身失效；round122 已排除，这条只作兜底 |

每档后面**不再跟 exit 码**：一颗返回了 errno 的 `reboot(2)` 就直接落到下一次 write，六颗全返回则孩子走到 `_mach_done` 以 11 退出，由父亲的 `wait4` 行报出来。这样「panic 屏画了 probe-done 同屏还有父亲的 wait 行」不可能出现（panic 终止启动），而只出现 wait 行则是上面那条兜底读数。

**构建**（EL0-only 轮，内核不动，无需 `build kernel`）：

```
cd /Users/zhouzhou/UniXNU-clover && /opt/homebrew/bin/python3.13 scripts/make-clover-root.py
cd /Users/zhouzhou/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" /opt/homebrew/bin/python3.13 ./unixnu build payload \
  --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-root/trustcache.bundle
cd /Users/zhouzhou/UniXNU-clover && /opt/homebrew/bin/python3.13 scripts/check-clover.py && /opt/homebrew/bin/python3.13 scripts/verify-clover-payload.py
```

`check-clover` 通过（ADT 13268 字节、8 核、splash `0x9d400000-0x9f800000`）；`verify-clover-payload` 全绿：卷内 `scratch-data.img` 与签名根逐字节相同、trustcache 与发布的 bundle 相同、内核探测补丁与 original-PMGR 绑定与 CloverSdhci 链接都在。boot-args 一字未动。

| 产物（`~/clover-out/`） | 字节 | SHA-256 |
| --- | --- | --- |
| `round123-payload.img` | 58720256 | `f2b05de704f24835091ce579bdb63995553fbd9a5a9ce5a0180b0418e2a0c241` |
| `round123-scratch-data.img` = `launchd.macho` | 35648 | `90d6c8fb689acb503e4367f9356ae53bd67906283219708a83e1366157d476c3` |
| `round123-in-volume-kernel.macho.trustcache` | 56 | `dcf66eeaa2f1bb95fc68856578b562d15e1f98104b35d5caadd019e874d91b29` |
| `round123-in-volume-kernel.macho` | 51974144 | `559ab0323052b3eec4fa6c33e3177f660ec34174a436ff164e9c93430d3ee6e8`（与 round121/122 逐字节相同） |
| `round123-in-volume-kernel.macho.adt` | 13268 | `1e9a52d2d417f6af06388abba3ba10733aa26aff6fdeded609db7c2694e6d431` |
| `round123-in-volume-unixnu.toml` | 588 | `c463cac60b843b240ae954e950cdfaad42827c555b631f8b906d177fd9e24b63` |
| `round123-SHA256SUMS` | — | 七项全部 `OK` |

ADT 换了哈希但不是改动：与 round122 的 ADT 逐字节比对只差 **一段 64 字节、偏移 352–415** 的数据，即 `/chosen` 的 `random-seed` —— 每次构建重掷，所以 ADT 哈希从来不是跨轮不变量。体积（13268）与 `check-clover.py` 的断言都没动。`round123-unixnu.toml` 从 572 变 588 字节同样只是 `[entry] title` 变长。

**新符号表**（`__TEXT` 基址 `0x100000000`、不 slide，屏上地址直接减基址；相对 round122，`_start` 之后所有东西整齐上移 `0x40` = 删 14 条指令、加 30 条）：

| 符号 | round123 | round122 |
| --- | --- | --- |
| `_start` | `0x100000380` | 同 |
| `_mach_done` | `0x1000007f4` | `0x7b4` |
| `_skip_mach` | `0x100000800` | `0x7c0` |
| `_parent` | `0x100000a18` | `0x9d8` |
| `_wait_error` / `_wait_reaped` | `0x100000ac4` / `0x100000acc` | `0xa84` / `0xa8c` |
| `_exited_other` / `_not_exited` / `_signaled` | `0x100000af8` / `0x100000b00` / `0x100000b18` | `0xab8` / `0xac0` / `0xad8` |
| `_exit_now` / `_checkpoint` / `_fail` | `0x100000b24` / `0x100000b34` / `0x100000b38` | `0xae4` / `0xaf4` / `0xaf8` |
| `_write_console` / `_print_dec` | `0x100000b48` / `0x100000b58` | `0xb08` / `0xb18` |
| `_puts` / `_print_line` / `_mheader` | `0x100000b94` / `0x100000ba4` / `0x100000bd4` | `0xb54` / `0xb64` / `0xb94` |

六档 `svc` 站点与串地址（已用 `objdump` 逐个对着 `__DATA` 偏移核过顺序，也在 `board.toml` 的 `trust_cache_sha256` 注释里留了同一张表）：

| 档 | `svc` | 串地址 | 串 |
| --- | --- | --- | --- |
| 1 | `0x100000620` | `0x100004192` | `clover-el0: after write msg-label` |
| 2 | `0x10000063c` | `0x1000041b4` | `clover-el0: after write msg-line` |
| 3 | `0x100000668` | `0x1000041d5` | `clover-el0: after write m2-presend` |
| 4 | `0x1000006d0` | `0x1000041f8` | `clover-el0: after write m2-send-line` |
| 5 | `0x100000778` | `0x10000421d` | `clover-el0: after write m2-rcv-line` |
| 6 | `0x1000007f0` | `0x100004241` | `clover-el0: after write probe-done` |

**三个仍然开着的缺陷**（本轮都不动，理由各自不同）：

- 父亲的 `cmp x24, #448`（.S:1254-1263 一带）：`WEXITSTATUS` 只留 8 位，而 448 = `0x1c0` 需要第 9 位，所以「孩子以 448 退出」这条永远读不出来。本轮删掉了档位上的 exit 码，这条缺陷不影响读数，但下一轮想让孩子用退出码说话之前必须修。
- 镜像 35648 字节躺在 8 MiB 的 `logfs` 里，mockfs 的 legacy 视图把整个分区当 `/sbin/launchd` 读，所以文件尾后面是上一轮的残留字节。exec 只用前面的真字节，本轮仍然没踩到，但阶梯把镜像又推大了一点。
- `cnt0 == freq`、`cnt1 == 0`：tick 周期还是量不出来，`ticks=` 继续只当启动进度坐标。

**上机**（冷刷，绝不 `fastboot boot`；这里会写两个 eMMC 分区 —— `cache` 装 payload ESP、`logfs` 装被真正 exec 的根镜像，写分区是破坏性操作，确认设备在 fastbootd 且你同意覆盖这两个分区再跑）：

```
fastboot flash cache /Users/zhouzhou/clover-out/round123-payload.img
fastboot flash logfs /Users/zhouzhou/clover-out/round123-scratch-data.img
fastboot reboot
```

`Warning: skip copying logfs image avb footer (logfs partition size: 0, …)` 仍然是分区表缺条目、不是写失败。一次刷机只回答一个问题：屏上那一句 `after write <名字>` 就是最后一次返回的 write 的名字。

## round123 读数（v0.2.95 实机）：阶梯一档都没响 —— `msg=` 那次 write 停在里面

整屏是一条连续的 console 日志，**没有 panic 屏**，最后一行是半行加光标块：

```
AppleImage4: magazine[cptx]: magazine data cannot be written: 30
AppleImage4: magazine[zlne]: magazine write failed; may be expected: 30
[System Event] [254410393] [INFO] [Subsystem: nvram] [Event: sync]
IONVRAMCHRPHandler.cpp:serializeVariables:615 - No _nvramController
IONVRAMCHRPHandler.cpp:sync:894 - serializeVariables failed, ret=0xe00002d8
install_apfs_vfs_fs:8248: apfs: speculative telemetry feature ENABLED by boot-arg
apfs_sysctl_register:1407: done registering sysctls.
CloverBlock r121: refire n=2 (already ran)
Added memory device md0/rd0 (03000000/0E000000) at FFFFFFF00839…
Waiting on <dict ID="0"><key>IOProviderClass</key><string ID="1"…
Got boot device = IOService:/ArmDTPlatform/CloverBlockDevice/IOB…
BSD root: md0s39, major 1, minor 39
vnode_create_internal: alias vnode (vid = 63544114) is in state (start) v_fflags = 0x828 v_numoutput = 0
vnode_create_internal: alias vnode (vid = 63544114), ls in state (end) v_fflags = 0x828 v_numoutput = 0
[clover] vfs_mountroot returned
[clover] devfs_kernel_mount returned
[clover] signit returned, execing init
VM Swap Subsystem is ON
No POST boot-arg set.
load_init_program: attempting to load /usr/appleinternal/sbin/launchd
load_init_program: attempting to load /sbin/launchd
[clover] el0 init: pid=1 console fd=0 pagesize=4096
[clover] el0 init: forked pid=2 x1=0
[clover] el0 child: alive
[clover] el0 child: pid=2 ppid=1 x0=2 x1=1
[clover] el0 child: exec /sbin/launchd
[clover] el0 init: pid=2 console fd=1 pagesize=4096
[clover] el0 mach: thread=259 task=515 host=771
[clover] el0 mach: reply=1027
[clover] el0 mach: msg=▮
```

**先确认这是 round123 的字节在跑**，因为屏上没有任何 EL0 身份串可比。链条是：本轮 payload 里的 trust cache 只有**一个**条目 —— `422d125bc557cd0e…`，也就是 `round123-scratch-data.img` 的 cdhash（56 字节的 bundle 装一颗）；屏上有 `signit returned, execing init`、有 exec 之后的 `[clover] el0 child: exec /sbin/launchd` 和 exec 副本自己那句 `el0 init: pid=2 console fd=1`，说明 AMFI 放行了那次 exec。所以被 exec 的 `logfs` 字节只能是 round123 那颗镜像 —— 换成 round122 的会先撞上 `before write` 的 panic 屏，换成更早的会因 cdhash 不匹配而起不来。

**于是六档阶梯的读数落在最空的一支：一档都没响。** 按本轮的读数表，这意味着 `msg=` 那 23 字节的 write **没有返回**。而它已经画出来了：屏上最后一行正是那 23 个字符，光标块停在 `msg=` 后面，地址和换行永远没跟上。

这条结论的强度来自「返回了却不可能不响」的穷举：`bl _puts` 一返回，接下来是 `mov x1,#0xe0`、`b.hs _fail`、`ret`，然后就是档 1 那五条直线指令。

- 若 write 返回且 carry 置起（errno），`_fail` 会走 `and x0,x0,#31 / add x23,x1,x0 / b _exit_now`，孩子带码退出 ⇒ 父亲那句阻塞 `wait4` 会醒，屏上必然多出 `el0 init: child …` 一行。没有。
- 若 write 返回且无 errno，档 1 的 `svc #0x80` 就在眼前，round122 已经量过这条路径从 EL0 到 panic 屏只要约 10 秒。没有。
- 若 `reboot(2)` 本身带 errno 返回，六档全返回才轮到 `_mach_done` 的 exit(11)，同样会叫醒父亲。没有。
- 剩下的可能只有：那次 `write` 停在里面（或者孩子在 syscall 里被杀掉、根本没回到 EL0），两种都是「这次 write 没回来」。

**round121 那处矛盾就此结掉，而且结在样本那一侧。** 那帧看门狗交出的 `elr=0x100000b3c` = `_puts + 4`，是「`bl _write_console` 已返回之后」的那条指令；本轮直接测 write 本身回没回来，答案是没回来。所以那个单点 `elr` 不是「孩子此刻在哪」的忠实读数 —— 看门狗的采样侧有缺陷，这条从本轮起算既定事实，任何后续 `elr` 读数都要按「不可信」处理，直到采样缺陷被单独修好。

顺带两个新数据点：`reply=1027`（端口号每启动都不同，round84/122 读到的是 1827，无意义）；`BSD root: md0s39, major 1, minor 39` 与 `CloverBlock r121: refire n=2 (already ran)` 说明根仍然走 eMMC 的 39 号分区、块栈与固件侧一字未动 —— 本轮只换了 EL0 镜像。

**一个还没排掉的解释：拍照时机。** 「write 停在里面」和「write 还在飞、照片拍早了」在这一帧里分不开。区分它不需要新固件：候选 tick 周期 ≈18 ms 下 60000 tick ≈ 18 分钟，看门狗到点会自己占屏（round121 第二帧证明它确实会）。所以**同一份 v0.2.95 复拍一张、间隔足够长**：如果屏变成看门狗 panic，那 `msg=` 那一刻起就一直没动，本帧结论从「没返回」升级成「永久卡死」；如果复拍出现某档 `after write …`，那才是「慢但会回来」，问题整体挪到父亲/exit 那一侧。

**下一题（round124，仍然只改 EL0 镜像）：** 现在嫌疑人被钉在「第九次 console write 进去了、没出来」，而前八次同一个孩子、同一条路径都出来了。要切的是「**控制台是不是对所有人都不再应答**」—— 因为 `cons_write` 这类路径是串行的，如果第九次调用持着某把锁卡住，那么任何进程的下一次 write 都会一起卡。做法：父亲从 round78 的阻塞 `wait4` 改回**带截止时间的静默轮询**，每若干轮 `_puts` 一句 `el0 init: waiting <n>`，到点用同一颗已证明能上屏的 `reboot(2)` 报 `clover-el0: parent deadline`。读数三值：父亲的心跳行出现 ⇒ 控制台对别的进程还活着，卡的是那一次调用；心跳行没出现但 panic 上了屏 ⇒ 父亲没走 write 就到达了 deadline（还能顺带证明 panic 通道不受控制台影响）；心跳和 panic 都没有 ⇒ 父亲也卡在 write 里，控制台全局废了。一次刷机三值。（这条计划里「静默轮询 + deadline panic」那半截在动手前被推翻，见下面 v0.2.96 那节的第一段：`sched_yield` 在本树是 nosys，轮询体没法让步；发出去的是第二个写者，不是心跳。）

## round123 读数·第二帧（同一份 v0.2.95，等到看门狗自己占屏）：整个剩余寿命里一档都没响 —— 「拍早了」排掉

第一帧之后没有断电、没有重刷，只等。屏最终换成了这一帧：

```
（上方残留的开机 console 文本，10.312421 … 10.541290，最后一行是
  [ 10.541290]: debug_log_init: Error!! gPanicBase is still not initialized）

ArmCpu: clock watchdog at 60000 ticks (elr=100000b98 spsr=fffffff6e65dbd50 elr2=100000b98
       cnt0=124f800 cnt1=0 freq=124f800) @ArmCpu.cpp:632
pages=0x000000000003af7e
free=0x0000000000039527
ticks=0x000000000000ea60
ipi=0x0000000000000000 sent=0x0000000000000000
irq=0x0000000000000013 phase=0x0000000000000003 intid=0x0000000000000013 typer=0x000000000007b04f3
pmgr=0x0000000000000005 status=0x0000000000000000 mode=0x0000000000000002
nub=0xfffffff8ffedc800 attached=0x0000000000000001 registered=0x0000000000000002
aon=0x00000000ffffffff=>0x00000000ffffffff
RA=0xfffffff007a31024
FP=0xfffffff106ffcee0
LR=0xfffffff007968b40
fmt=0xfffffff00712e7ec
a0=0x000000000000ea60  a1=0x0000000100000b98
a2=0xfffffff6e65dbd50  a3=0x0000000100000b98
a4=0x000000000124f800  a5=0x0000000000000000
TCR=0x00000022a51a251a
```

`spsr=` 那一格跨行折了一次（第一行末 `…spsr=fffffff6`，第二行首 `e65dbd50`），所以它的位数最容易读错；屏底部 `a0…a5` 是同一批 panic 参数溢出到栈上的槽，逐格与表头对得上，于是这一帧的所有数都被同屏读过了两遍。

**这一帧把上一节留下的「拍照时机」备选排掉了。** `panic()` 只保留**第一个** panic —— 这是 round78 以来一直用来做排除的那条性质。本轮孩子在尾段一共埋了六颗 `reboot(2)`，任何一颗被执行，屏上第一个 panic 就应该是 `userspace panic: clover-el0: after write …`，看门狗根本没机会占屏；round122 又已经量过这条通道从 EL0 的 `svc` 到 panic 上屏只要约 10 秒。现在屏上是看门狗，而且 `ticks=0xea60` 正好等于 deadline：⇒ **从第一个 tick 到 deadline，六档一颗都没被执行过**。按 round122 那对候选值（553 tick ↔ 9.94 秒 ⇒ ≈18 ms/tick）算，这段等待是**十几分钟**量级。所以第一帧的结论从「那次 write 没返回」升级成「在这次启动剩下的全部寿命里，它一次也没返回」。

屏上方那些带时间戳的行不能用来给 panic 定时：panic 屏只清自己光标行到半屏（`xnu-panic-screen.c:364`），上方留的是开机时写上去的旧文本 —— 同一句 `debug_log_init: Error!! gPanicBase is still not initialized` 在 round122 那帧读的是 `9.942761` 秒，本轮读 `10.541290` 秒，它们只说明这次启动至少走到了 10.5 秒。

**panic 屏自己的账目仍然逐格能解**，而且这次解出来的是「谁开的枪」：

| 屏上的数 | 解出来 | 依据 |
| --- | --- | --- |
| `RA` = `0xfffffff007a31024` | `__ZN6ArmCpu9interruptEv.cold.2 + 0x0` | `nm -n round123-in-volume-kernel.macho`；下一个符号 `signalCPU…cold.1` 在 `0xfffffff007a31064`，所以这是那个 cold 块的开头 |
| `LR` = `0xfffffff007968b40` | `ArmCpu::interrupt() + 0x280` | 同一张表：`_ZN6ArmCpu9interruptEv` 起于 `0xfffffff0079688c0`，下一个符号 `signalCPUEP5IOCPU` 在 `0xfffffff007968b60` ⇒ 落在 `interrupt()` 体内，就是跳进上面那个 cold 块的那条分支的返回地址 |
| `fmt` = `0xfffffff00712e7ec` | 看门狗自己那句格式串 | `__TEXT` vmaddr `0xfffffff007004000`、fileoff 0 ⇒ 文件偏移 `0x12a7ec`，那里逐字节是 `ArmCpu: clock watchdog at %llu ticks (elr=%llx spsr=%llx elr2=%llx cnt0=%llx cnt1=%llx freq=%llx) @%s:%d` |
| `FP` = `0xfffffff106ffcee0` | 内核栈帧 | `0xfffffff10…` 与 round122 的 `0xfffffff70b…` 同一量级 |

⇒ 开枪的确实是 `drivers/ArmCpu/ArmCpu.cpp:624` 那个 `armcpu_timer_interrupts >= gClockPanic` 分支，不是别人冒名。附带地，`irq=0x13 phase=0x3` 说的是同一件事：`phase` 在 :621 被写成 3，也就是 `rtclock_intr()` 已经返回、正走到看门狗判断；round122 那帧是 `irq=0x3ff phase=0`（没有中断在处理中），因为那次 panic 是孩子在 EL1 里自己发起的，跟 tick 无关。`intid=0x13`=19 与 `armcpu-timer-intid=19` 一致；`typer=0x7b04f3` 不变；`ipi=0 sent=0` 仍然说明这块板上一个 IPI 都没到过。

**样本这一侧，两个字段各自坏得很整齐。**

`elr` / `elr2` = `0x100000b98`。本轮镜像（`build/clover-root/launchd.macho`，与刷进 `logfs` 的 `round123-scratch-data.img` 逐字节相同）`__TEXT` 在 `0x100000000`、无 slide，所以这就是 `_puts + 4`：

```
_write_console:                       _puts:
  100000b48  mov x0, x22                100000b94  bl  _write_console
  100000b4c  mov x16, #4  // write      100000b98  mov x1, #0xe0   ← 样本
  100000b50  svc #0x80                  100000b9c  b.hs _fail
  100000b54  ret                        100000ba0  ret
```

`objdump` 数过：全镜像八处 `bl _puts` 落在 `0x10000060c … 0x1000007d4`，全部在 `_mach_done`（`0x7f4`）之前，也就是 exec 之后那段只有 pid=2 会走的 mach 探针里；`_parent`（`0xa18`）出声只经 `_write_console` 与 `_print_dec`。所以被打断的线程只能是孩子，而样本给它的位置是「某次 write 的返回点」—— 八处共用同一个返回点，它连是哪一次 write 都说不清。

关键在于这个「不巧」跨了两轮：round121 那帧的样本是 `0x100000b3c`，正是**那一版**镜像的 `_puts + 4`；本轮镜像的 `_puts` 整体后移了 `0x5c`，样本跟着后移 `0x5c`。随机采不到这种一致性，但**「读的是被阻塞那次调用的返回地址」能**：孩子停在 `_write_console` 的 `svc`（`0x100000b50`）里出不来，它挂在栈上的返回地址恰好是 `0x100000b98`。这样两帧不再互相打脸，而是同一件事的两种画法 —— 采样侧少算了一层帧。上一节把 `elr` 判成「不可信」，这一帧把它坏的方式钉准了：**它报的是「这次 write 该回到哪儿」，不是「孩子现在在哪儿」**，而按这个读法，它和六档阶梯完全吻合。

`spsr` = `0xfffffff6e65dbd50` 则坏得更干脆：SPSR_EL1 的 `[63:32]` 是 RES0，`mrs` 不可能读出高 32 位全满的值；round121 那帧同格记的是 `0xffffffff14084cd50` —— 同样是「一长串 `f` 顶满高 32 位、低半截还多出第九位」的形状（那一帧也跨行折过，具体几位不必再较真），而那个量级看着更像内核指针而不是状态字。低四位仍是 0（EL0t），所以「中断打在用户态」这句结论没被推翻，但这一格除了 mode 之外不能再用。

其余读数与既往同类：`pages=0x3af7e`（round121/122 那两帧是 `0x3af7c`，总页数在不同启动之间本来就会差几页）；`free=0x39527`，比 round121 第二帧的 `0x39530` 又少 9 页 —— 卡住一个孩子并没有让内核停摆，别的分配还在发生。`pmgr=0x5 status=0 mode=0x2`、`nub=0xfffffff8ffedc800 attached=0x1 registered=0x2`、`aon=0xffffffff=>0xffffffff` 与 round122 同型（`nub` 的地址每次启动都换）。`cnt0=124f800 cnt1=0 freq=124f800` 是 round82 那节记下的老缺陷，原样复现：`counter()` 在 panic 时读回 0，tick 周期仍然测不出来，18 分钟只能继续当候选。

**round124 照旧，而且更值了。** 一次刷机只问一题：控制台此刻是不是对**所有人**都不再应答。父亲从阻塞 `wait4` 改成带截止时间的静默轮询，每若干轮 `_puts` 一句 `el0 init: waiting <n>`，到点用同一颗已证明能上屏的 `reboot(2)` 报 `clover-el0: parent deadline`。三值读数不变（心跳出现 ⇒ 卡的是那一次调用；只有 panic ⇒ 父亲绕开了 write；两者都没有 ⇒ 父亲也卡在 write 里）。新增一件顺手可做的：轮询体里直接 `mrs x0, cntvct_el0` —— 这块板上没有任何定时等待能返回（round77），而架构计数器的 EL0 读是另一条路；把它读到的值打进心跳行，第一次能同时给出「父亲等了多久」和「一个 tick 到底多长」。风险也写清楚：若 `CNTKCTL_EL1.EL0VCTEN` 没开，那条 `mrs` 在 EL0 是未定义指令，孩子会带 SIGILL 死掉 —— 而父亲的 `wait4` 会把它报出来，所以这条也不会变成静默失败。（这一段的两条都被否掉了：`sched_yield` 在本树是 nosys，「带截止时间的静默轮询」做不成让步轮询；`mrs cntvct_el0` 的可用性取决于 `_enable_timebase_event_stream()` 是否为本板置了 `CNTKCTL_EL1.PL0VCTEN`，而它只在 `user_timebase_type() != USER_TIMEBASE_NONE` 时置位 —— 本核没走到那条分支。发出去的 round124 因此不问时间，只另起一个写者；见下面 v0.2.96 那节。）

## v0.2.96（round124）：再 fork 一个只写控制台的探子

上面两段写的 round124 计划在动手前被推翻了两处，先记账，再写真正发出去的东西 —— 因为「计划」如果只留在段落里，下一轮读数就会去解一颗根本没上屏的 panic。

第一处：`sched_yield` 在这棵树里不存在。`bsd/kern/syscalls.master` 的 326 号是 `{ int nosys(void); }`，所以「让步轮询」里那句让出 CPU 的调用会变成一次 nosys 返回 EINVAL 的 svc —— 轮询体照样是热的，round76-77 那两轮的饿死问题一点没解决。第二处更硬：`mrs x0, cntvct_el0` 在 EL0 是否可用取决于 `CNTKCTL_EL1.PL0VCTEN`，而这棵树里只有 `_enable_timebase_event_stream()`（`osfmk/arm64/machine_routines.c:2380`）在 `user_timebase_type() != USER_TIMEBASE_NONE` 时才置这一对位（:488-497 按 `HAS_ACNTVCT` / `__ARM_ARCH_8_6__` 三分支返回 NOSPEC_APPLE / NOSPEC / SPEC），本板的 dtb 没走到那条路上。于是「顺手量 tick 周期」这条也拿掉了 —— 它原本只是搭车，不该拿主问题陪绑。

发出去的 round124 不问时间，只问**还有没有人写得动**。`_parent` 在 `forked pid=…` 那句横幅之后立刻 `fork()` 出 B（x20=B 的 pid，x21 分岔，B 走 `_prober`），B 只做一件事：二十四个 `[clover] b<n>` 行，写完 `exit(5)`。父亲全程留在**阻塞** `wait4` 里 —— 先等 B，醒来把 `[clover] el0 init: prober status=<原始 wait 状态>` 打上屏，再落回 round78 那颗等 A 的 `wait4`。本轮没有任何 `reboot(2)`，所以六档阶梯照旧在 A 身上、看门狗仍会在 60000 tick 自己占屏。

读数四值，全部来自同一张照片：

- 屏上有 `b1`…`b24` 和 `prober status=1280`（5<<8）⇒ 另一个进程照样能把字打上去，卡的是那一次调用本身，不是控制台；下一刀切进 `msg=` 那次 write 的内侧。
- `b1`…`b<k>`（k<24）之后就没了、也没有 prober 行 ⇒ 控制台答了 k 行之后不再接受写者，屏上的编号指名它是在哪一行接管的。
- 一行 `b` 都没有，只剩看门狗那帧 ⇒ B 第一次 write 就没回来。这一支残留一个二义：「B 卡在 tty 里」和「B 从没被调度」在屏上长得一样；前者才是主嫌疑，因为父亲这四个轮次一直是阻塞 `wait4`，round70-74 证明过这个形状下别的进程能跑。
- `b` 行不在但 `prober status=` 出现了，且值是 `(224+errno)<<8` ⇒ B 的 write 是带着 errno 返回的：控制台没堵住，是在丢字。

`b` 行的编号还顺带治了 round123 那张照片的一处不便：`elr` 不可信，而 B 的行是**自己报自己到哪**，不依赖任何采样。

本轮要写两颗 eMMC 分区，冷刷，绝不 `fastboot boot`：

```
fastboot flash cache /Users/zhouzhou/clover-out/round124-payload.img
fastboot flash logfs /Users/zhouzhou/clover-out/round124-scratch-data.img   # 35888 B，efb2cd85…，exec 的就是这份
```

`cache` 必须重刷：`trust_cache_sha256` 换成了 32508dc5…（B 的新代码改了 cdhash），而音量里的信任缓存只有一份，`logfs` 上的根镜像 cdhash 对不上就是下一颗 exec 失败。kernel.macho 仍是 559ab032…，与 round123 逐字节相同。boot 仍是 round87-boot.img，本轮不动。

拍两张：一张尽快，一张等看门狗那帧自己占屏（≈18 分钟）—— 本轮没有任何 `reboot(2)`，所以第二张必然有 panic 屏，它顺带给出「B 到底有没有走完」的第二个坐标。

## round124 实机（第一帧）：B 的字上了屏，控制台并没有对所有人废

倒数第七行不在四值里，也不在两支预设的分支里。逐字（尽快拍的那一张，尾部九行，上面是引导自己的日志）：

```
[clover] el0 init: pid=1 console fd=0 pagesize=4096
[clover] el0 init: forked pid=2 x1=0
[clover] el0 child: alive
[clover] b[clover] el0 child: pid=2 ppid=1 x0=2 x1=1
[clover] el0 child: exec /sbin/launchd
[clover] el0 init: pid=2 console fd=1 pagesize=4096
[clover] el0 mach: thread=259 task=515 host=771
[clover] el0 mach: reply=1827
[clover] el0 mach: msg=▮
```

身份先钉死：`[clover] b` 这十个字节只写在 round124 的 `_prober` 里，round123 及之前没有任何路径能产出它；而 `el0 init: pid=2 console fd=1` 这行证明 exec 过得了 AMFI —— 它跑的就是 `efb2cd85…` 那份 logfs 根镜像，`53d1583f…` 那颗 `cache` 里的信任缓存（只有 32508dc5… 一份）认的就是它。所以这张照片读的是本轮的字节，不是残留。

四值一支都没中。屏上有一个 `b`（无编号、无换行，因为 `_prober_mark` 故意不带 `\n`，等 `_print_line` 补上），没有 `b2`…`b24`，也没有父亲的 `prober status=`；而 A 在 `b` 之后**又成功写出了五行**，最后落在 round123 那个一模一样的位置 `[clover] el0 mach: msg=`，编号不出。

于是本轮的主问题有了答案，而且是负的那个：**控制台没有对所有人废**。B 被创建、被调度、写进了屏幕，父亲和 A 也照写不误；卡住的是**那一次调用**，不是那台设备。

接着读源码，把「一次 write 卡死而别人照写」这件事在 `bsd/dev/arm/km.c` + `bsd/kern/tty.c` + `osfmk/console/serial_console.c` 里钉出三种可能的落点（树是 `out/feeds/xnu-0b4b910d`）：

1. **tty 层有两处睡眠，两处都永久，且两处都释放 tty 锁。** `ttwrite` 进来先查载体：`TS_CONNECTED` 不在就 `ttysleep(tp, TSA_CARR_ON(tp), TTIPRI|PCATCH, "ttydcd", 0)`（tty.c:2489，`timo=0`）；写满出队再查水位：`ttysleep(tp, TSA_OLOWAT(tp), TTOPRI|PCATCH, "ttywri", tp->t_timeout)`（tty.c:2656）。而 `ttysleep` = `msleep0(chan, &tp->t_lock, …)`，**睡着的时候锁是放开的** —— 这正是「一个人永久卡住、其他人照样写得动」唯一能成立的形状。
2. **这两处睡眠都没人喊醒。** 出队唤醒只发生在 `ttwwakeup()`（tty.c:2897，条件 `TS_SO_OLOWAT && c_cc <= t_lowat`），全树的调用点只有三处：`ttyioctl_locked` 的 TIOCSETA/CLOCAL 跳变（:1340，km 走不到 —— `kmioctl` 在拷入前把 CLOCAL 强行或进用户结构，而 `kmopen` 早就把 `t_cflag` 设成 `CREAD|CS8|CLOCAL`）、`ttyflush(FWRITE)`（:1888）、`ttymodem(tp, 1)`（:2054，km 从不调用）。**`km.c` 里一个 `ttwwakeup`/`ttwakeup` 都没有**，`kmoutput` 排空出队之后只 `(*linesw[t_line].l_start)(tp)`。所以进了 `ttywri` 就出不来。
3. **`tp->t_timeout` 恒为 0。** 全树唯一给它赋值的地方是 `TIOCSDRAINWAIT`（tty.c:1603，`*(int*)data * hz`；顺带 `wakeup(TSA_OCOMPLETE)` + `wakeup(TSA_OLOWAT)`），本板没人发过这颗 ioctl —— 上一轮笔记里我把它记成了 `TIOCSTIME`，本树的 spelled 名字是 `TIOCSDRAINWAIT` = `_IOW('t', 87, int)`。`ttymalloc` 用 `Z_ZERO` 清零，所以 `timo=0`，`msleep0` 就是无期限。

水位这几个数也顺手算了出来（免得下次再猜）：`kmopen` 把 `t_ospeed = TTYDEF_SPEED`（9600），`ttsetwater` 于是给出 `cps = 960`、`t_lowat = CLAMP(480, 256, 32) = 256`、`t_hiwat = roundup(1440, CBSIZE) = 1440`；而 `ttymalloc` 给 `t_outq` 只 `clalloc(…, TTYCLSIZE=1024, 0)`。**`hiwat(1440) > 出队容量(1024)`，所以 tty.c:2541 那句 `c_cc > hiwat` 永远不成立，进 `overfull:` 只能靠 `b_to_q` 在 1024 字节的出队塞满时返回非零。** 换句话说：`ttywri` 这一支要有 1024 字节没排空才会触发，本轮屏上总共才几十字节 —— 除非排空动作本身没发生（`kmstart` 见 `TS_TIMEOUT|TS_BUSY|TS_TTSTOP` 就 `goto out`）。这条留给下一轮的数字去判，不在这张照片里。

console 层反过来支持「不是它」：`console_write`（serial_console.c:524）**从不为硬件阻塞** —— 它把字塞进 ring，末尾只做一次 `if (get_preemption_level() == 0) console_ring_try_empty(true);` 的 fail-fast 冲刷；ring 满才进 `while (reserve == NULL) { if (preempt != 0) return; console_ring_try_empty(false); }`，而 `try_empty(false)` 会 `lck_mtx_lock(&flush_lock)` 真睡。**关键：这一条整段都跑在 `tty_lock(tp)` 里面**（`kmwrite` 持锁不放）。所以如果 B 卡在 console 层，它是**带着 tty 锁睡的**，A 的下一颗 write 就绝对进不去 —— 而 A 明明又写了五行。加上 ring 的落屏顺序就是各次 reserve 的先后（屏上画的是被排空过的字节，不是「已返回的 write」，这一点本身是个坑），`b` 之后 A 能连着 reserve 五次，就证明 B 当时既不持 tty 锁、也不持 flush_lock。**B 的第二次 write 要么睡在 tty 那两处（放开锁的睡眠）里，要么根本没再被调度 —— 这张照片分不开这两支。**

A 的位置仍是 round123 的那个位置、形状一字不差（`msg=` 上了屏、编号不出），这条已经复现四轮，可以当常量用。

### 为什么下一刀不能再用 EL0 探针切

本来想得很美：让 B 发一颗 `ioctl(1, TIOCSDRAINWAIT, &secs)` —— 它既需要 `tty_lock`（能测锁的活性），又会 `wakeup(TSA_OLOWAT)` 并给 `t_timeout` 装上期限（既能喊醒睡在 `ttywri` 的人，又让以后每次那种睡最多数秒就带着 EIO 回来）。三支读数干净利落。

**但它排不过去。** 探针必须发生在 A 卡住**之后**，而 EL0 没有任何可靠的时间源：`sched_yield` 是 `nosys`，`mrs cntvct_el0` 要 `CNTKCTL_EL1.PL0VCTEN`（本板没走到 `_enable_timebase_event_stream()`），`select` 的 deadline 从不触发（round77 证过），自旋 N 次要多久更是无从标定 —— 赌赢了多一个读数，赌输了整颗白刷。B 自己又是那种「第一行写完就没了」的状态，连把读数带出来的机会都没有。

于是 round125 换成内核侧一刀，而且**仍是一题**：看门狗在 60000 tick 占屏这件事已经复现两轮，panic 屏又是直写帧缓冲、不经被试的控制台的通道。就让它在那一刻把这几个字打出来：

- `console_ring.len / used / nreserved` —— ring 满不满、有没有人 reserved 之后再没归还（>0 且 used 不减 = console 层卡死）。
- `disableConsoleOutput / console_suspended` —— 是不是有人在把字**静默丢掉**（`console_write` 有两处直接 `return` 的丢字出口：`disableConsoleOutput` 和 `get_preemption_level() != 0`）。
- `km_tty[0]->t_state / t_outq.c_cc / t_lowat / t_hiwat / t_timeout` —— 有没有 `TS_SO_OLOWAT`、`TS_BUSY`、`TS_CONNECTED`，出队到底攒了多少字节。

两行读数互斥：`t_state` 带 `TS_SO_OLOWAT` 且 `c_cc` 顶着 1024 ⇒ 上面第 2、3 条（km.c 漏 `ttwwakeup` + `t_timeout=0`）就是根因，修法是驱动排空后补一次 `ttwwakeup()`；`ring.used == len` 且 `nreserved > 0` ⇒ 根因在 console 层的锁，tty 那两处睡眠是无辜的。

第二帧（本轮同一次启动，等到 ≈18 分钟看门狗占屏）仍值得拍：它只补一个坐标 —— B 的沉默是不是永久的。不拍也不影响 round125 的读数。

## v0.2.97（round125）：看门狗占屏那一刻，panic 屏把 console ring 和 km tty 打出来

问题还是上一轮那句，只是换了问的对象：**A 卡住的那一次 `write` 停在哪一层** —— tty 那两处会放开 `tty_lock` 的睡眠（`"ttydcd"` / `"ttywri"`），还是 console 层的 `flush_lock`/ring。上一轮已经证明 EL0 探针排不过去（没有可靠时间源），所以这一刀完全落在内核侧：复现两轮的 60000-tick 看门狗 panic 就是触发器，panic 屏又是直写帧缓冲、不经被试控制台的通道。

改动三处，EL0 镜像与根镜像一字未动：

- `scripts/patch-xnu-entry-probe.py`：`assemble_c()` 里新增一条与 `MAX_MEM`/`PMGR_OBS` 同规格的符号解析表（`CONSOLE_RING`/`KM_TTY`/`DIS_CONS`/`CONS_SUSP`），每个都查自然对齐与所属段边界，缺失的符号让 `.c` 里的 0 默认值接管，于是没有 tty 控制台的内核渲染如旧。`console_ring` 是 `serial_console.c` 里的 file-static 变量，靠本核带 STABS 才拿到名字：`b ffffffff007b3bcf0 _console_ring`。
- `scripts/xnu-panic-screen.c`：`format(&g, fmt, args)` 之后**第一件事**就是画这六行（画在 `pages=` 之前，因为本轮的答案在这里，屏的高度只留得下二十来行，尾部 `RA/FP/LR/slots` 是被挤掉的那一段）。新增的 `word()` 与 `slot()` 同规矩：每个地址先过 `readable()` 的硬件翻译查询，不过就画十六个 `-`，所以一个陈旧的 `km_tty[0]` 只会留下一行破折号，不会把 panic 变成无声的死机。
- `board.toml`：`title = "clover r125: panic screen reads the console ring"`。本轮 EL0 没有任何变化，`[clover] b`、`msg=` 那些行 round124 也画得出来 —— **菜单第一行有没有 `r125` 是这张照片属于哪一轮的唯一判据**。

屏上六个坐标与它们的偏移，全部取自这份镜像自己的反汇编（`llvm-objdump --macho -d --section=__TEXT_EXEC,__text --dis-symname …`），不是头文件里的猜测：

| 屏上标签 | 地址 | 意义 | 怎么读 |
| --- | --- | --- | --- |
| `ring len=` | `_console_ring + 0x8` | `console_ring.len` | `vm_map_round_page(256*(MAX_CPUS+1))`，`used` 与它相等就是 ring 满 |
| ` used=` | `+ 0xc` | `used` | 已塞进 ring、还没排空给硬件的字节数 |
| ` nres=` | `+ 0x10` | `nreserved` | 正在被写的区段数；`>0` 且不再降 = 有人 reserve 之后没归还，排空者就在 `while (nreserved > 0) cpu_pause()` 里转 |
| `buf= / wr= / rd=` | `+0x0 / +0x18 / +0x20` | `buffer`、`write_ptr`、`read_ptr` | `wr-rd` 与 `used` 对不对得上；偏移取自 `_console_ring_try_empty` 的 `ldr x23,[x10,#0x20]`、`stur …,[x9,#0x14]` |
| `fl=` | `+ 0x28 + 8` | `flush_lock` 的锁字 | `_lck_mtx_try_lock` 在结构 +8 处 `ldaxr/cbnz/stxr`，存进去的是持有者的 cpu 号（取自 `TPIDR_EL1+0x510`）：非 0 ⇒ 锁被占，0 ⇒ 空闲**或者**被 cpu 0 占着，这份镜像分不开这两种 |
| ` wt=` | `+ 0x38 + 8` | `write_lock` 的票字 | `_lck_ticket_lock` 在结构 +8 处 `ubfx w10,w9,#16,#8; cmp w10, w9 lsr #24`，**只有 bits[23:16] == bits[31:24] 才算空闲**；不相等 ⇒ 有人在锁里排队，而 `console_write` 的 ring 满重试正好要拿它 |
| `dis=` | `_disableConsoleOutput`（`int`） | 静默丢字出口一 | `console_write` 开头 `if (disableConsoleOutput) return`（:526） |
| `susp=` | `_console_suspended`（`bool`） | 静默丢字出口二 | `console_io_allowed()` 里 `console_suspended || !is_console_initialized() || pmap_in_ppl()`（:536） |
| `tty=` | `_km_tty[0]` | 这块板上唯一那颗 tty | `bsd/dev/arm/km.c:39 struct tty *km_tty[1]` |
| `state=` | `tp + 0xc0` | `t_state` | 下一行直接把它问出来，不用手算 |
| `cc=` | `tp + 0x80` | `t_outq.c_cc` | 出队里攒了多少字节；`ttymalloc` 只给了 `clalloc(…, TTYCLSIZE=1024, 0)` |
| `low= / hi= / to=` | `+0x1d0 / +0x1cc / +0x1d4` | `t_lowat`、`t_hiwat`、`t_timeout` | 预期 `256 / 1440 / 0`；`to=0` 就是「进了 `ttywri` 出不来」的那半句话 |
| `owner=` | `tp + 0x1e8` | `t_locked_thread` | `_tty_lock` 的 `str x0,[x19,#0x1e8]`。**非空 ⇒ panic 那一刻确实有线程持着 `tty_lock`**，也就是有人还在 `kmwrite` 里面；空 ⇒ 没人持锁，卡住的那位在睡眠或者根本没被调度 |
| `OLOWAT yes/no` | `tp + 0xc0` 的 bit 0 | `TS_SO_OLOWAT` | 照片上手算 hex 容易看错，所以让屏自己回答 |

三个读数，前两个互斥：

1. **`OLOWAT yes` + `cc=` 顶到 1024** ⇒ 上一轮源码账的第 2、3 条（`km.c` 从不 `ttwwakeup`、`t_timeout` 恒为 0）就是根因。修法是驱动排空出队之后补一次 `ttwwakeup()`；这颗探针顺便成了验证。
2. **`used == len` 且 `nres > 0`**（或者 `fl=`/`wt=` 显示锁被占）⇒ 根因在 console 层的锁，tty 那两处睡眠是无辜的。这时 `owner=` 也应当非空，因为整段 ring 代码都跑在 `tty_lock` 里面。
3. **六个坐标全是干净值**（`used=0`、`nres=0`、`fl=0`、票字相等、`OLOWAT no`、`owner=0`）⇒ panic 那一刻**没有人在 console 层**，卡住的 A 只能是在 tty 睡眠里或者根本不再被调度；那么 `cc=` 与 `state=` 的其余位（`TS_BUSY 0x4`、`TS_CONNECTED 0x20000`）就是下一个问题，而不是这一轮的失败。

顺手把这条也记下来，免得下一次又当成回归：`kernel.macho.adt` 的哈希每轮构建都变，因为 `make-clover-adt.py:211` 每次重新 `os.urandom(64)` 写进 `/chosen` 的 `random-seed`。本轮两份 ADT（`bfab69bb…`、`b0c6697b…`）来自同一颗内核的两个构建，就是这个原因。

### 构建、校验与本轮该刷哪一格

```
# 只重打 payload：panic 屏是补丁器在 build payload 时编译进内核文本空洞的
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-root/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && scripts/verify-clover-payload.py
```

归档在 `~/clover-out/round125-*`（`round125-SHA256SUMS`）：payload `58720256` 字节 `cb4cc89b…`；卷内 `kernel.macho` `5beb1f97…`（补丁 6679 字节 / 35 段，比 round124 多出 panic 屏那六行）；**`scratch-data.img` 仍是 `efb2cd85…`，与 round124 逐字节相同**，信任缓存仍是唯一那一份 `32508dc5…`，boot-args 一个字符没动。所以本轮**只刷 `cache` 一格**，`logfs`（第 39 格，装着 exec 的根镜像）和 boot 都不碰：

```
fastboot flash cache ~/clover-out/round125-payload.img
```

冷刷，不用 `fastboot boot`。开机认出菜单第一行的 `clover r125:`，然后让 A 照旧卡在 `msg=`，等 ≈18 分钟看门狗自己占屏 —— 那一帧就是本轮的全部答案，读上面六行。第一帧（引导日志停在 `msg=` 那张）也顺手拍，用来确认 `r125` 与 A 的位置没变。

## round125 读数（v0.2.97 实机）：写路径整条是空的 —— 「卡在 tty/console」这条模型死了

一帧，全部读数都在那张照片里（照片是竖屏拍的，`sips -r 270` 转正；`-r 90` 会得到一张上下倒过来的）。逐字：

```
ArmCpu: clock watchdog at 60000 ticks (elr=100000c80 spsr=fffffff1
6eb44d50 elr2=100000c80 cnt0=124f800 cnt1=0 freq=124f800) @ArmCpu.
cpp:632
ring len=0x0000000000001000 used=0x0000000000000000 nres=0x00000000
00000000
buf=0xfffffffbffda4000 wr=0xfffffffbffda48d8 rd=0xfffffffbffda48d8
fl=0x0000000000000000 wt=0x0000000022220144 dis=0x0000000000000000
susp=0x0000000000000000
tty=0xfffffff3883d5800 state=0x0000000000020028 cc=0x0000000000000
000
low=0x0000000000000100 hi=0x00000000000004e0 to=0x0000000000000000
 owner=0x0000000000000000
OLOWAT no
pages=0x000000000003af7c
free=0x00000000000394ff
ticks=0x000000000000ea60
ipi=0x0000000000000000 sent=0x0000000000000000
irq=0x0000000000000013 phase=0x0000000000000003 intid=0x000000000000
0013 typer=0x00000000007b04f3
```

先看仪器自己有没有说谎：`ticks=0xea60`=60000 与 `armcpu-clock-panic=60000` 对上，`phase=3` 是 `rtclock_intr()` 已返回、正走到看门狗判断（ArmCpu.cpp:621），`intid=0x13` 与 `armcpu-timer-intid=19` 一致，`typer=0x7b04f3` 三轮不变，`ipi=0 sent=0` 照旧 —— 这块板上一个 IPI 都没到过。新增的十二格全部有效，没有一格画成 `----------------`，也就是 `readable()` 十二次全过、偏移全落在段内。

**主问题（那次 write 停在 tty 层还是 console 层）的答案是：都不在。** 十二格里有六格，每一格恰好关掉一个睡眠点，而且关的都是「必要条件」而不是「侧面迹象」：

| 读数 | 关掉的分支 | 依据 |
|---|---|---|
| `state=0x20028` 含 `TS_CARR_ON`(0x8) | `ttwrite` 的 "ttydcd" 永久睡 | tty.c:2489 那一支的前提是载体位**不在** |
| `OLOWAT no`、`cc=0` | "ttywri" 水位睡 | tty.c:2655 紧挨在 `ttysleep` 上面一行做 `SET(tp->t_state, TS_SO_OLOWAT)`；这一位不在，说明它没进过那一支 |
| `owner=0` | 有人持 tty_lock / 有人排在 tty_lock 上 | `tty_lock()` 是先 `lck_mtx_lock` **后**才 `os_atomic_store(&tp->t_locked_thread, current_thread())`（tty.c:275-281），`tty_unlock` 在释放**前**清成 NULL（:333）。所以「持锁者为 0」不是「没记录」而是「没人持锁」；没人持锁就不可能有人在等锁 |
| `used=0`、`nres=0`、`wr==rd` | ring 里压着没排的字节 / 有人 reserve 之后没归还 | `serial_console.c` 的 `while (reserve == NULL)` 只在 ring 满时才进；ring 空 |
| `fl=0`、`wt=0x22220144` | `flush_lock` / `write_lock` 被谁带走 | `_lck_mtx_try_lock` 把持有者的 cpu 号写进 arg+8，0 是自由（除「cpu 0 持有」这一支分不开，但 `nres=0` 已独立排掉冲空者）；`wt` 是 ticket，`[23:16]==[31:24]`（0x22==0x22）即自由，第 0 字节还是标签 `'D'`(0x44) |
| `dis=0`、`susp=0` | write 返回了但字被静默丢掉 | `console_write` 那两处直接 `return` 的出口 |

偏移这套东西不是猜的，照片自己证了：`low=0x100`=256 正是上一轮从 `kmopen`（`t_ospeed = TTYDEF_SPEED`）经 `ttsetwater` 算出来的 `t_lowat`；`hi=0x4e0`=1248（我算成 1440，错了，`roundup` 的模子取错）；`tty=0xfffffff3883d5800` 是个真堆地址；`state` 那三位（`TS_CARR_ON|TS_ISOPEN|TS_CONNECTED`）正是一个开着的控制台该有的形状。**读的是一个真 tty，不是零页。**

顺手记一笔：`wr - buf = 0x8d8` = 2264 字节，是这次启动里所有 write 进 ring 的字节总数。屏上那几十行按一行四十字算正好两千字上下 —— **没有任何人在自旋重画**，「A 在 EL0 里绕圈」这一支被这一个减法排掉了。

于是「write 没返回」和「写路径是空的」必须同时成立。前者不是推测：A 在 `msg=` 那行标签之后紧接的就是第一档 `reboot(RB_PANIC|RB_QUICK, "clover-el0: after write msg-label")`（`scripts/clover-el0-init.S:659-663`，镜像里 `0x100000620` 那颗 `svc`），而 `reboot_kernel` 对 `RB_PANIC|RB_QUICK` 是**进门就 `panic_kernel`**（kern_shutdown.c:182-184，`__abortlike`，`RB_PANIC`=0x800、`RB_QUICK`=0x400 见 reboot.h:89-90），round85 已经证明这条通道画得出 `userspace panic: …`。它没画，看门狗才占屏（first-panic-wins）。所以 A 既没走出那次 write，也没死（rounds 78-81 父亲阻塞 `wait4(A)` 时从来没有 `exit status` 屏）。

**两条合起来只剩一个落点：`write(2)` 里在 tty 之前的那一段** —— `vn_write`/`VNOP_WRITE` 那颗 vnode 锁、`spec_write` 到 `cdevsw` 的派发，或者 `uiomove` 触发的缺页。这一段既不碰 `t_locked_thread`，也不碰 ring，正好就是照片读出来的形状。

`elr=100000c80` 要跟着降级。它等于本轮镜像（`scratch-data.img` 与 round124 逐字节相同，`efb2cd85…`）的 `_puts + 4`，也就是 `bl _write_console` 的返回点，和 round121/123 那两帧一样「恰好」落在同一个符号偏移上；而 `_print_dec` 是 `b _write_console` 尾调用的，数字那一次 write 的返回点是 `_print_line + 4`(`0xc90`)，所以这一格确实指名「标签那一次 write」。可疑的是它的搭档：同一对 `mrs` 读出的 `spsr=0xfffffff16eb44d50` 把 `SPSR_EL1[63:32]` 这段 RES0 读满了，`mrs` 不可能读出这种值，round121/123 两帧同格也是这个形状。**这一对里至少有一格是帧里带出来的陈旧值，不能当活样本用**；上一轮我把它读成「被阻塞那次调用的返回地址」，本轮的直接测量说：就算它指的是那次调用，那次调用也不在 tty 里。它的正确用法到此为止 —— 是一个**标签**，不是**坐标**。

一处本轮的代价，写下来免得下次再踩：panic 块连标题占了 19 行，把屏上半截的旧日志和 EL0 尾部一起盖掉，照片底边又切掉最后两三行，**本轮读不到 A 的最后一行**。EL0 镜像与 round124 相同，所以尾部按 round124 那张照片算；下一轮要么把块缩短，要么拍照时把屏挪全。

### round126 的问题：同一颗 fd 上，一颗不进 tty 的 syscall 回不回得来

上面那张表把 tty 和 console 都排掉了，但排掉的方式是「读状态位」，状态位只能说明「那一刻没人在这」。要钉死，得从**另一侧**再切一次：让 A 在 `msg=` 标签 write **之前**插一颗 `fstat64(fd, buf)`（`syscalls.master:520` 的 339 号，`{ int sys_fstat64(int fd, user_addr_t ub); }`；走 `specfs` 的 `VNOP_GETATTR`，全程不进 `kmwrite`、不进 tty、不进 ring），后面紧跟一档自己的 `reboot(RB_PANIC|RB_QUICK, "clover-el0: after fstat")`。

排在**前面**而不是后面，是这架梯子的形状决定的：`reboot` 一响就停住启动，先到的那一档就是屏上唯一的那句话（first-panic-wins），所以放在卡死之后的 rung 永远等不到人走到它 —— 同一个理由也否掉了「在旁边再加一颗 `getpid` 对照档」的想法。

读数三值，互斥，而且**两值几秒就能拍**，正好接住「等十八分钟太久了」：

- 屏上出现 `userspace panic: clover-el0: after fstat` ⇒ `fstat64` 回来了 ⇒ 同一个线程、同一颗 fd、几秒之差，getattr 答了而 write 不答 ⇒ 卡点属于 write 自己那条链，不是它前面那颗 vnode。**代价：这一档把本轮的其余六档全挡死了，write 本轮不再被重新观察，只是被对照。**
- 屏上出现 `exit status <64+e>` ⇒ A 带着 errno 退出了（`_fail` 那条路），没有任何人卡住，是**报错/丢字**，不是被堵住。`e = errno & 31`（探针的失败 stage 是 18×32=576，屏上读到的低字节就是 64+e）：78（ENOSYS 78&31=14）意味着 339 号在这台内核里是空的，本轮读数作废；73（EBADF）意味着 x22 不是它自称的那颗 fd。这一支要父亲先等 A 才报得出来，见下。
- 两者都没有，只有看门狗那一帧 ⇒ `fstat64` 也停在里面 ⇒ **卡点在 tty 之前**，就是那颗 vnode 锁或者它的派发链。

配套改的两处都在父亲身上。第一，`_parent` 的等待顺序倒回来：**只等 A**（`mov x0, x19`），醒来照 round80 的老规矩一颗字都不写、直接 `exit`，让 panic 屏去报 `exit status`。round124 挂在原地的 `wait4(B)` 被删掉了 —— B 几秒就死、A 永远不死，所以「谁先变更状态」永远轮到 B，父亲那份报告花在证人身上而不是嫌疑人身上了；`_prober_status` 那行字随之删除，本轮也一并读不到 B 的 wait status。B 本身留在原位，`b1..b24` 上屏仍然是「控制台在 A 停住之后还答了二十四次」的对照组，只是判据从「有没有 `prober status=`」改成「b 流停在第几行」。第二，`_wait_reaped` 的解码加了一条通道：A 的退出码落在 64..95 时原样透传，不再折成粗读的 `1`，否则第二值只剩「A 从 `_fail` 走了」这一句，分不清是哪颗 errno。

## v0.2.98（round126）：把 `fstat64` 插到那次 write 前面，并让父亲只等 A

只改 `scripts/clover-el0-init.S`，四处：

1. A 在 `msg=` 标签 write 之前多了一段：`fstat64(x22, _statbuf)`（`x16=#339`）→ `mov x1,#576`（stage 18×32）→ `b.cs _fail`，紧跟一档 `reboot(0xc00, "clover-el0: after fstat")`。镜像里 `svc` 分别在 `0x100000608`（fstat64）和 `0x100000624`（这一档的 reboot），被试的那次 `bl _puts` 在 `0x10000063c`。
2. `.data` 尾部新增 `_statbuf: .space 256`（`struct stat` 在这个 ABI 里 152 字节，`bsd/sys/stat.h`；原来的 `_scratch` 只有 8 字节，装不下）。放在整个 `.data` 最后，是因为本文件所有写长度都是运行期算的标签差，任何 `.ascii` 与它的 `_end` 之间都不许插东西。
3. 父亲删掉 `wait4(B)` 与 `_prober_report`，fork 之后一路落到原来的 `mov x0, x19` 那颗阻塞 `wait4`，醒来仍然一个字不写、直接 `exit`。`_prober_status` 那串字符随之删除，`_parent` 里那条 `mov x20, x0`（B 的 pid）成了死码也一并删；B 本身、`_prober_mark`、`b1..b24` 全留作对照组。
4. `_wait_reaped` 的解码加一条通道：A 的退出码落在 64..95 时原样透传（`_not_fstat_band`），其余仍折成 `1`。屏上 `exit status 64+e`，`e = errno & 31`（`_fail` 的 `and x0, x0, #31`）。

本轮镜像的座标表（对象地址 +0x380 = 镜像地址；helper 相对 round125 整体下移 0x40，因为 `_parent` 里删掉的 `_prober_report` 正好 64 字节，而新探针那 ~48 字节被 `_parent` 之后的删除抵消了）：`_parent` `0x100000a48`、`_wait_reaped` `0x100000b18`、`_not_fstat_band` `0x100000b4c`、`_prober` `0x100000b88`、`_fail` `0x100000be0`、`_write_console` `0x100000bf0`（它的 `svc` 在 `0xbf8`）、`_print_dec` `0x100000c00`、`_puts` `0x100000c3c`、`_print_line` `0x100000c4c`、`_mheader` `0x100000c7c`。**如果看门狗仍然占屏，`elr` 该读 `0x100000c40`（= `_puts + 4`）而不是上一轮的 `0xc80`** —— 这一格本轮是可证的：`elr` 若是陈旧值就仍落在别处，若是活的样本就必然跟着镜像移动。

```
cd ~/UniXNU-clover && python3.13 scripts/make-clover-root.py   # 35888 B, sha256 614b8356…, cdhash 4f63be63…
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-root/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && scripts/verify-clover-payload.py
```

归档在 `~/clover-out/round126-*`（`round126-SHA256SUMS`）：payload `0c23f1d5…`；卷内 `kernel.macho` 仍是 `5beb1f97…`，与 round125 逐字节相同（本轮没动内核也没动 panic 屏）；`scratch-data.img` `614b8356…`（变了，EL0 换了）；信任缓存 `c3ed2152…`（变了，钉的是那颗 cdhash）；ADT `53c65564…`（每轮都变，`make-clover-adt.py` 重画 random-seed）。**本轮要刷两格：`logfs`（第 39 格，exec 的根镜像换了）和 `cache`（负载 + 信任缓存）。** 这两条命令都要写 eMMC 分区，请你自己确认后执行；boot 仍不碰（还是 round87 那份）：

```
fastboot flash logfs /Users/zhouzhou/clover-out/round126-scratch-data.img   # 35888 B，614b8356…
fastboot flash cache /Users/zhouzhou/clover-out/round126-payload.img
```

冷刷，不用 `fastboot boot`。开机认出菜单第一行的 `clover r126:`，然后**几十秒内**就该看到答案：屏上是 `userspace panic: clover-el0: after fstat` 还是 `exit status <64+e>`；两个都没有才需要等到 ≈18 分钟的看门狗那一帧（那一帧仍然读 round125 那六行，顺便验证 `elr` 是否跟着镜像移到了 `0xc40`）。拍照注意：panic 块连标题占 19 行，会把屏上半截旧日志和 EL0 尾部盖掉，所以第一帧（引导日志停在哪儿的那张）也顺手拍。

## v0.2.99（round127）：把媒体换成 logfs 那 8 MiB，问 apfs 能爬到多深

一刷一个问题，这一轮问的不是根，是文件系统：Apple 自己的 `com.apple.filesystems.apfs`
能不能 attach 到 `drivers/CloverBlock` 发布的那颗 `IOMedia`，读到一个容器超级块，并一路爬到
「发布出一个卷」。答案照旧画在 panic 屏上，因为 round125 已经证明 console 那一刻是空的。

三处改动：

1. `scripts/make-clover-apfs-container.py`（新）用宿主的 `diskutil apfs createContainer` 造一份
   恰好 8388608 字节（= logfs 的 16384 扇区）的裸容器：容器从镜像第 0 字节开始，超级块 `NXSB`
   落在 0x20，块大小 4096、块数 2048，一个卷 `CLOVER127` 里放一个 `MARKER.txt`。这份字节就是
   Apple 的 kext 预期看到的字节，不是本移植手搓的。APFS 容器里带自己的 UUID 与时间戳，每重造
   一次字节都不同，所以 sha 不钉在板级文件里，而是每次由脚本写进 `build/clover-apfs/report.json`；
   本轮要刷的那份是 `~/clover-out/round127-logfs.img`，sha256 `9a362cdc…`。
2. `drivers/CloverSdhci` 把窗口端出来：mode 14 那条 GPT 表项走查里，一旦表项 39 恰好是
   `12320768..12337151`（round101 从卡上自己读到的那段，也是 mode 16-20 写 logfs 时把的那道门），
   就把首 LBA 与扇区数 release-store 进 `gBlockWindow*`，新导出的 `clover_emmc_block_window(&first,
   &sectors)` 让 CloverBlock 取走；不匹配就两个字保持 0。顺带把 `gBlockGuard` 从「有人占着就直接
   返回失败」改成最多等 2 秒的交接：文件系统会把读并发派发下来，一次被拒的读对它是挂载失败，
   不是「待会儿再来」。
3. `drivers/CloverBlock` 把描述的几何换成窗口：`mediaSectors()`/`reportMaxValidBlock()` 报 16384
   而不是整卡，`doAsyncReadWrite` 给每个块号加上 `gWindowFirst` —— 于是媒体的第 0 字节就是 logfs
   的第 0 块，也就是那个容器超级块。卡上的 GPT 一个字没动，也没有事后属性手术（匹配发生在媒体
   注册的那一刻，round115 那棵手工第二树就是死在 EEXIST 上的）。步骤改成 1b 解析 `clover-apfs=1`
   并闩窗、4 之后走 4b 的 `apfsProbe`；步骤 5-9（找 `disk0s39`、改名 `md0s39`）只在没带这个参数时
   照跑，round118-121 那条路随时可复现。

`apfsProbe` 就是本轮的全部，而且它不返回：先 `whole->open(..., kIOStorageAccessReader)` 读媒体的
第 0 块、`memcmp` 0x20 处的 `NXSB`（`m=`），然后 `close` —— 容器方案要 open 它的 provider，探针
不能占着自己的租约去量它；接着 `OSKextLoadKextWithIdentifier("com.apple.filesystems.apfs")`（`l=`），
这个 kext 和 IOStorageFamily 一样在 boot kext collection 里而不在链接好的内核里，加载它同时也就是
给它的 personality 排匹配；最后把读写计数清零，3000 次 × `IOSleep(10)` = 30 秒轮询 `whole` 在
`gIOServicePlane` 上的后代（深度 ≤ 4、每层 ≤ 64），`metaCast` 三个类名
`AppleAPFSContainerScheme` / `AppleAPFSContainer` / `AppleAPFSVolume`。挂在这个媒体下面的服务
按构造就是挂在**这块盘**上的，不需要匹配字典（也就没有 round116 那次双释放），也不需要跟别人
比 provider。用轮询而不是 `waitForService`：要等的这次 attach 正是由当前正在跑的配置机械执行的，
把控制权交回去是唯一不阻塞自己的办法。30 秒相对看门狗很宽：round119 十次一秒等待烧掉
`ticks=0xb0c` ⇒ 一个 tick ≈ 3.5 ms ⇒ 60000 tick ≈ 210 秒。

`Whole=Yes` 这条依据是在宿主上验过的：`ioreg -l -c IOMedia` 里那颗没有分区图的整媒体带
`Whole=Yes`，而 apfs.kext 的 `AppleAPFSContainerSchemeWholeDisk` personality 就是
`IOPropertyMatch {"Whole"=Yes}`、`IOProbeScore` 2000；`IOBlockStorageDriver` 在这里合成的 `IOMedia`
本来就带这个字。`emit_prelink.py` 的 root 列表里也一直有 `com.apple.filesystems.apfs`。

根这一轮退回 `rd=md0`：本轮不在乎根挂在哪儿，在乎活到 CloverBlock 跑完；挂着窗口去 `rd=md0s39`
只会让 `IOFindBSDRoot` 的十次等待全部落空、死在 `IOKitBSDInit.cpp:984`。`scripts/check-clover.py`
现在同时断言 `rd=md0`、禁止 `rd=md0s39` 与 `rd=disk0s39`、并且 `clover-apfs=1` 必须和
`clover-emmc=21` 同时在场。

```
cd ~/UniXNU-clover && python3.13 scripts/make-clover-apfs-container.py   # 8388608 B + report.json
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-root/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && scripts/verify-clover-payload.py
```

`build kernel` 这一轮是必须的：两个驱动都归 `[kernel] archives` 管，只重打 payload 会用上一轮的
`.a`。本轮链接出的内核 sha256 `5514f6df…`（本地）/ 卷内 `82a84dd1…`（打补丁后，探针偏移 6680 字节
分布在 35 段里，其余逐字节相同）；`scripts/verify-clover-payload.py` 新加一格
`kernel.macho: CloverBlock linked`，查 `clover-apfs`、`CloverBlock r127: apfs`、
`AppleAPFSContainerScheme`、`_clover_emmc_block_window`、`_clover_block_run` 五个字都在卷内内核里，
和 `check_sdhci` 一样是对「链没链进来」的独立证明。EL0 根镜像与信任缓存这轮没动，仍是
`614b8356…` / `c3ed2152…`。归档在 `~/clover-out/round127-*`（`round127-SHA256SUMS`，另加
`round127-logfs.img` 与 `round127-apfs-report.json`）。

**本轮刷两格：`logfs`（第 39 格，换成 8 MiB APFS 容器 —— 这会盖掉 round121 之后写进那格的根镜像，
本轮根走 `rd=md0`，不需要它）和 `cache`（负载）。** 两条命令都写 eMMC 分区，请你自己确认后执行；
boot 仍不碰（还是 round87 那份）：

```
fastboot flash logfs /Users/zhouzhou/clover-out/round127-logfs.img   # 8388608 B，9a362cdc…
fastboot flash cache /Users/zhouzhou/clover-out/round127-payload.img  # 56 MiB，260648eb…
```

冷刷，不用 `fastboot boot`。菜单第一行应认得 `clover r127: apfs attaches to the logfs window`。
答案在 CloverBlock 那一步，几十秒内就该上屏，一行读数各字段：

- `CloverBlock r127: window 0` —— 卡自己报的表项 39 不是 `12320768..12337151`，窗口不存在。
- `CloverBlock r127: first-read <IOReturn>` —— 媒体的第 0 块读不出来，块路径本身有问题。
- `apfs f=<first> n=<sectors> m=0 …` —— 读出来了但 0x20 处不是 `NXSB`：窗口起点或镜像不对。
- `l=` 非 0 —— apfs.kext 没加载起来，后面 `s/c/v` 全 0 是必然结果，不是 apfs 的失败。
- `d>`0 而 `s=0` —— 有服务挂上了这颗媒体，但不是 apfs（或 apfs 停在 scheme 之前）。
- `s=1 c=0 v=0` —— 容器方案 attach 上了，读不到或不认这个容器。
- `v=1` —— 通了，卷都出来了（等待会提前结束，`r=` 是它读过的块数）。
- `r=`/`w=` —— 那 30 秒里存储栈经过这颗媒体读/写了多少块；全 0 说明 attach 之后根本没人来读。

## round127 读数（v0.2.99 实机）：apfs 挂上了这颗媒体，一路发布出卷

开机 **9.8 秒**，panic 屏第一行就是本轮要的那一句：

```
CloverBlock r127: apfs f=12320768 n=16384 m=1 l=0 d=7 s=1 c=1 v=1
r=56 w=0 @CloverBlock.cpp:767
```

逐字段：

- `f=12320768 n=16384` —— 卡自己报的表项 39 恰好是 round101 量到的那段，窗口闩上了，媒体**就是** logfs。
- `m=1` —— `NXSB` 在 0x20。这一格最值钱：那次读是 `whole->open` + `IOMedia::read` 走的，不是本移植自己的 PIO，所以「媒体第 0 字节 → 卡 LBA 12320768 → 容器超级块」这条偏移链是端到端验过的。
- `l=0` —— `OSKextLoadKextWithIdentifier("com.apple.filesystems.apfs")` 返回 `KERN_SUCCESS`。
- `s=1 c=1 v=1` —— `Whole=Yes` 那条 personality 真的在**没有分区图的整媒体**上开了：容器方案 attach 上、容器对象解析了超级块、并且发布出了一个卷。Apple 自己的 apfs.kext 驱动了一块本移植做出来的盘。
- `d=7` —— 媒体下面一共 7 个服务，三个是 apfs 的类，另外四个是 `IOMediaBSDClient` 这一路的常规乘客。
- `r=56 w=0` —— 那 30 秒窗口里上面的栈经过这颗媒体读了 56×512 = 28 KiB，**一个字都没写**（挂载前的读阶段不需要写）。

30 秒预算一秒都没花：`v=1` 让轮询在头几轮就短路退出，同帧 `ticks=0x127`=295 —— 按 round119 标定的 ≈282 tick/秒算约 1.0 秒，这是那个数字的第二次独立印证（一次 attach 全程 ≈1 秒，合理）。

有一栏必须和 rounds 122-126 分开读：这一帧的 tty 段**全是零**（`state=0`、`low=hi=0`、`owner=0`、`OLOWAT no`、`used=nres=0`、`wr==rd=0xfffffffbbfda43a2`、`fl=0`、`wt` 的两个锁字节 0x38==0x38 自由）。原因不是「write 卡住了」也不是「console 废了」，而是**这次 panic 取在 EL1、EL0 还不存在、`kmopen` 从来没跑过**。所以它是那六个字段在开机 9.8 秒时的一个下界，不是写路径的第四次读数；顺带也说明屏上那些引导日志并不是从这颗 tty 出去的（控制台优先的内核直接画帧缓冲）。`pages=0x3af7e`/`free=0x3a222`（≈941/907 MiB）与 `irq=0x3ff phase=0 intid=0` 是同一帧的其余栏位，本轮不作结论。

**这一格把 iOS 路线最关键的前提立住了**：块设备 → `IOBlockStorageDriver` → `IOMedia` → apfs.kext → 发布出卷，全通。问题从「能不能挂上这颗盘」变成「挂上之后能不能真的 mount 并读出文件字节」—— 下一格就在同一个探针里再走一步：拿 `measureApfsDepth` 已经找到的那个卷对象，向 vfs 要一次挂载，然后把容器里那颗 `MARKER.txt` 通过 vnode 读回来，读数照旧画在 panic 屏上。

本轮不改代码，读数落盘；`v0.2.99` 的产物继续有效（`~/clover-out/round127-*`）。

## v0.3.0（round128）：把量测对象换成 iPadOS 自己的容器字节

round127 通了，但它通的是**本端口喂给 apfs 的那 8 MiB** —— 宿主机 `diskutil apfs createContainer` 造出来
的一颗裸容器 + 一个 `MARKER.txt`。iOS-first 路线上真正的问号在别处：Apple 自己的 apfs.kext，面对
Apple 自己发布的那颗容器，在一颗本移植做的盘上，能爬到多深。所以这一格先把「真容器」搬上卡，mount
那一步留给 round129（`root_match_volume_uuid` + 读回文件字节）。

三处改动，加一份镜像：

1. **镜像**：`[userdata]` 就是这颗卡的 Data 分区（`mmcblk0p64`：首 LBA 15795488、末 LBA 122142686、
   512 字节扇区，单位自己在 sysfs 上报的那段），`./unixnu build userdata` 按 `drivers/NativeUfs/RootLayout.h`
   的 SHNGROOT 布局写出一张 9382658048 字节的镜像：第 0 块是 4 KiB 清单，1 MiB 对齐之后依次放
   IPSW 里**原封不动**的两张盘 —— `OS`（`044-76040-038.dmg`，6077580800 B，落在 0x100000）和
   `Cryptex1,SystemOS`（`044-76034-038.dmg`，3303014400 B，落在 0x16a600000）。两个 DMG 的 sha256 钉在
   `[ipsw.sha256]`，写进镜像的字节是解包后的原像，不是重新组装的东西。
2. `drivers/CloverSdhci` 把 Data 窗口也端出来：mode 14 那条 GPT 表项走查原本在认到表项 39 就 `break`，
   现在改成 39 与 64 都找、两个都找到才停；表项 64 必须恰好是 `15795488..122142686`（round101 从卡上
   自己读到的那段）才把首 LBA 和扇区数 release-store 进 `gBlockUserData*`，新导出
   `clover_emmc_block_userdata_window(&first, &sectors)`。`scripts/build-clover-block.py` 的符号清单同步
   加了 `_clover_emmc_block_userdata_window` —— 那张清单必须**正好**等于 `.a` 真正引用的东西，所以驱动
   和脚本只能一起改。
3. `drivers/CloverBlock` 的 `clover-apfs=2` 分支：先用 PIO 读分区头 8 个块（4 KiB）当 SHNGROOT 清单解析，
   取名为 `OS` 的那条项目的 `offset`/`size`，把媒体的几何改成它 —— 卡 LBA `15795488 + 0x100000/512 =
   15797536`，`6077580800/512 = 11870275` 扇区。清单里那颗 SHA-256（同块第 4064 字节）**故意不验**：这个
   文件没链任何哈希，而边界、属性、名字三道检查自己就会拒绝撕坏的或外来的清单。每一条拒绝都是独立的
   panic 步骤，步骤名就是读数：`root-read n`（PIO 没交回八个块）、`root-magic <首字>`（那儿什么都没刷 ——
   原装卡上这个字是 Android 的 ext4 超级块区域，不是 "SHNG"）、`root-version`、`root-count`、
   `root-attr i`、`root-bounds <offset>`、`root-geometry <sectors>`、`root-name <count>`（清单里没有 OS 窗口）、
   `udwindow 0`（表项 64 不是记录的那段，也就是说这不是 board.toml 描述的那颗卡）。

这一格唯一的真意外是**那 6 GiB 不是裸容器**：宿主机 `hdiutil imageinfo` 对 IPSW 自带的
`044-76040-038.dmg` 说「已压缩为UDIF只读(lzfse)」，里面装着**一整张带 GPT 的盘**（MBR、主 GPT 头、主
GPT 表、一条 `Apple_APFS`（类型 GUID `7C3457EF-0000-11AA-AA11-00306543ECAC`，LBA 34..11870241）、备份表），
而宿主上对将要刷进卡的那段字节的复核也确认了：窗口 +0x200 是 `EFI PART`，再往后 34 块才是 `NXSB`。
所以媒体的第 0 字节是 MBR，`m=` 的含义随之改成「GPT 头在清单说的位置上」，而**切盘交给 IOKit 自己**：
`IOBlockStorageDriver` 是异步注册整媒体的，round116 已经读过后面会发生什么 —— 那棵手工搭的第二棵树被
自动那一趟抢光了名字、每一颗都撞 EEXIST。让 Apple 的 `IOGUIDPartitionScheme` 去扫 Apple 的分区表，意味着
这轮量的 apfs 栈挂在本文件写过的任何东西下面**两层**，于是走查的深度上限 4 → 7、每层服务数 64 → 256。

两份预算得跟着镜像长，而且用的是同一件仪器的数：按 round127 量到的节奏 —— 56 个块花掉这块板自己的
计数器称为「约一秒」的 295 个 tick ⇒ 约 20 ms/512 B ⇒ 约 164 ms/4 KiB APFS 块 —— 真容器的头几页就是几十秒，
所以轮询从 3000 轮改成 12000 轮 × `IOSleep(10)`；看门狗跟着抬到 `armcpu-clock-panic=180000`（≈10.6 分钟），
因为这一轮要的答案是探针自己的 panic，不是定时器的。根仍然 `rd=md0`：这轮不在乎根挂在哪儿。
`scripts/check-clover.py` 现在要求 `clover-apfs=2` 与 `clover-emmc=21` 同时在场、`rd=md0` 在场、
`rd=md0s39`/`rd=disk0s39` 缺席，并且 `[userdata]` 必须真的带 `windows`。

```
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build userdata kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-root/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && scripts/verify-clover-payload.py
```

两个驱动都在 `[kernel] archives` 里，所以 `kernel` 这一格不能省。本轮链接出的内核 sha256 `38e19577…`（本地）
/ 卷内 `d743631c…`（打补丁后，探针偏移 6680 字节分布在 35 段里，其余逐字节相同）；`payload.img` `441a3169…`
（56 MiB）；ADT `bd6969f5…`、信任缓存 `c3ed2152…`、`scratch-data.img` `614b8356…` 这轮都没动。
`userdata-root.img` 是 `fd43ee70…`。归档在 `~/clover-out/round128-*`（`round128-SHA256SUMS`）。

**本轮刷两格：`userdata`（第 64 格，9382658048 字节）和 `cache`（负载）。** 两条命令都写 eMMC 分区，
请你自己确认后执行 —— 其中 **`fastboot flash userdata` 会抹掉平板上 Android 的 `/data`**（ contacts、微信记录、
登录态、下载的 App 数据全没，等价于恢复出厂设置），而且**之后如果再把原装 Android 引导起来，它很可能
重新初始化 `/data`，这张镜像就白刷了**；这一轮之后请一直冷刷我们的负载，别回头开 Android。
`cache` 与 `boot` 之外什么都不碰（`logfs` 里还是 round127 那颗 8 MiB 裸容器，本轮 `clover-apfs=2` 不看它）：

```
fastboot flash userdata /Users/zhouzhou/clover-out/round128-userdata-root.img   # 8.74 GiB，fd43ee70…，要刷很久
fastboot flash cache    /Users/zhouzhou/clover-out/round128-payload.img         # 56 MiB，441a3169…
```

**上面第一条在 macOS 上刷不动，这是宿主的限制，不是设备的。** 实测 macOS 把单次 `read()` 截在
2147483647 字节（请求 9.38 GB 只回 2 GiB − 1），而 fastboot 的 raw 路径是「一次读完整张文件」，
于是报 `fastboot: error: Failed reading from userdata` —— 一个字节都没写到卡上。
`fastboot -S 512M` 能绕开这次大读，但它把连续全零的块编成 DONT_CARE，也就是**那些块不写**，留在卡上的
是原装 Android `/data` 的旧字节；`img2simg` 同样做零检测。所以要「Apple 的字节一个不改」就别用它俩：
`scripts/make-clover-sparse.py`（新）把原镜像封装成 **只有 CHUNK_TYPE_RAW 块**的 Android sparse 容器
（280 块 × 32 MiB，块大小 4096，头 `image_checksum` 是展开数据的标准 CRC-32，不加 per-chunk CRC —— 写坏了
会让刷到一半失败），fastboot 走 sparse 路径就是逐块读、逐块写，字节与原镜像逐位相同。产物
`round128-userdata-root.sparse.img`（9382661436 B，`be354a4a…`），`--verify` 已经把 280 块逐块 sha256
比过原镜像：

```
fastboot flash userdata /Users/zhouzhou/clover-out/round128-userdata-root.sparse.img
```

冷刷，不用 `fastboot boot`。菜单第一行应认得 `clover r128: apfs reads iPadOS's own container off userdata`。
读数还是同一行形状 `CloverBlock r128: apfs f=… n=… m=… l=… d=… s=… c=… v=… r=… w=…`：

- `f=15797536 n=11870275` —— 清单里的 OS 窗口被闩进了媒体几何；`f` 不对就是偏移链或清单读错了。
- 任何 `root-*` / `udwindow` 步骤名 —— 见上面那张表，每个都自证是哪一道门拦的。
- `m=1` —— 窗口 +512 处是 `EFI PART`：Apple 的 GPT 在预期地址上，媒体确实是一张整盘。`m=0` 则 `s/c/v` 全 0 不构成 apfs 的结论。
- `l=` 非 0 —— apfs.kext 没加载，后面的 0 是必然。
- `s=1 c=1 v=1` —— Apple 的容器字节在自己的 `IOGUIDPartitionScheme` 切出来的那片上被认出来了，**这才是「真容器」的答案**；`s=0` 而 `d>0` 说明分区那趟没把 APFS 切片交上去。
- `d=`/`r=` —— 这轮要多穿两层，`d=` 该比 round127 的 7 大；`r=` 是这段里经过这颗媒体读掉的 512 字节块数，真容器的头几页应当远大于 56。

## round128 读数（v0.3.1 实机）：iPadOS 原装容器字节在自己的盘上发布出卷

开机约 **7.97 秒**上屏：`CloverBlock r128: apfs f=15797536 n=11870275 m=1 l=0 d=10 s=1 c=1 v=1 r=464 w=0 @CloverBlock.cpp:861`。

- `f/n` 就是清单里 OS 那一条换算成 512 字节扇区（`15795488 + 0x100000/512`、`6077580800/512`）⇒ SHNGROOT 清单
  和卡上 GPT 表项 64 两件事同时成立，一个 `root-*` / `udwindow` 步骤都没触发。
- `m=1` 是 UDIF 那个意外的形状被实机确认：**窗口第 512 字节是 `EFI PART`**，媒体是一张整盘，容器在 `0x104400`。
  这也正是本轮把切分交给 `IOGUIDPartitionScheme` 而不是自己动手的理由。
- `s=1 c=1 v=1` 是本轮要的那一格：**Apple 未经修改的 iPadOS `OS` 容器**，逐字节就是 IPSW 里那颗 DMG 展开出来的东西，
  被 apfs.kext 分区 → 认容器 → **发布出卷**，跑在本移植自己驱动的盘上。round127 证的是「我们造的容器它能读」，
  这轮证的是「它自己发的容器它能读」，后者才是 iOS 路线的前提。
- `d=10`（round127 是 7）= 多穿的两层（scheme + slice）连同它们的乘客；`r=464` = 上面那层读掉 232 KiB；`w=0` = 挂载前不写。
- 两笔涨上去的预算都没用上：同帧 `ticks=0x892`=2194，按 ≈275 tick/秒 ≈ 7.9 秒，和屏上时间戳对得上；
  但**别把它当秒表** —— round127 那一帧 9.8 秒只走了 295 tick，两者不互洽，tick 仍然只是「 boot 进度坐标」。
- 怎么刷上去的也是一条结果：`round128-userdata-root.sparse.img`（本轮 `make-clover-sparse.py` 出的 all-RAW 容器）
  **一条命令刷完**，bootloader 逐块接受了 sparse 流。这是这台机器的 fastboot 认 sparse 的第一次实机证据，
  也顺带绕过了宿主那个 2 GiB 单次 `read()` 上限（raw 文件正是死在它上面）。

**下一格的问题**：容器已经能被认出来、卷已经能发布，缺的是「真 mount + 把文件字节读回来」。下一轮在同一个探针里再走一步：
拿 `measureApfsDepth` 找到的那颗卷向 vfs 要一次挂载，读回容器里一个已知路径的字节，仍然画在 panic 屏上。

## v0.3.2（round129）：向内核要一次真挂载，把卷里的一个文件字节读回屏上

round128 的 `v=1` 只是一次 registry 计数：`AppleAPFSVolume` 发布出来了，但没有任何人 mount 过它，更没有谁从它读过
一个字节。本轮在**同一个探针、同一颗卡、同一份镜像、同一串 boot args** 里再走一步：把那颗卷的媒体交给 vfs，
让它做一次真正的挂载，然后读回 `System/Library/CoreServices/SystemVersion.plist` 的头 1024 字节 —— 全部印在同一行
panic 上。`f/n/m/l/d/s/c/v` 八字段原样保留，新的是后面那串。

**先记一条被否掉的路（下一轮别重新试）**：不走 `kernel_mount()`。`vfs_mount()` 只在 `VFC_VFSLOCALARGS` 下解析设备名 ——
`copyin(fsmountargs, &devpath, 8)` 之后 `NDINIT(..., UIO_USERSPACE, devpath)` + `namei()`
（`bsd/vfs/vfs_syscalls.c:1312-1400`），也就是要一个**用户态地址**和一个**解析得出的 `/dev` 节点**；clover 的根是 mockfs、
扁平、没有子目录，`/dev` 根本不存在。按角色挂载（`KERNEL_MOUNT_DATAVOL` 那一族，:1490-1607）则要一个已经挂好的 origin
`mp`，而本轮要问的恰恰是第一个挂载。

**本轮的形状 = 把引导根自己那段代码抄一遍**（`bsd/vfs/vfs_subr.c:1240-1294`），逐档都有出处：

1. 卷的 BSD 名与 major/minor：`IOMediaBSDClient` attach 到那颗 per-volume 媒体时写上去的，和 round116 起给 `disk0s39`
   命名的是同一趟异步配置。`volumeBsd()` 用 1000 轮 × `IOSleep(10)` 等它，等法是 step 5 从 round116 就在用的那种
   —— 轮询而不是 semaphore，因为干活的配置机器正在跑我们自己。
2. `bdevvp(major<<24 | minor, &devvp)` —— :1240。
3. `vfs_rootmountalloc("apfs", <卷自己的 BSD 名>, &mp)` —— :1176 沿 `vfsconf` 比 `vfc_name`（:1181），:1258 是它的调用点。
4. `VFS_MOUNT(mp, devvp, 0, ctx)` —— :1265，引导根那条「无 data 参数」的形状。
5. `vfs_unbusy(mp)`（:1284）+ `mount_list_add(mp)`（:1286）—— 这两档不是可选的：`vfs_rootmountalloc_internal()` 在
   :1146 就 `vfs_busy()` 过这颗挂载点，只有 `vfs_unbusy()` 会放掉那把共享 rwlock；而不在 `mountlist` 上的挂载点对
   `vfs_work_thread()` 是不可见的，那正是消化 `mnt_workerqueue` 的线程 —— apfs 把卷的延后拆解挂在那条队列上。
   `mount_list_add()` 要写 `mp->mnt_list`，但那个偏移在内核里读，我们的不透明指针不付代价。
6. `VFS_START(mp, 0, ctx)` → `VFS_ROOT(mp, &dir, ctx)`。
7. 四次 `vnode_lookupat(<单个组件>, 0, &next, ctx, dir)` 走 `System` / `Library` / `CoreServices` / `SystemVersion.plist`。
8. `vn_rdwr(UIO_READ, vp, buf, 1024, 0, UIO_SYSSPACE, IO_UNIT, cred, &resid, NULL)`。

**明确承认的缺口**：`mp->mnt_devvp = rootvp` 和它后面那句 `v_specflags |= SI_MOUNTEDON` 没抄 —— `struct mount` 的布局只在
`bsd/sys/mount_internal.h`，不是本归档编译时用到的头，所以设备 vnode 只经由 `VFS_MOUNT` 的第二个参数进文件系统。树里读
`mnt_devvp` 的地方（bootcache `vfs_cluster.c:537/546`、`vnode_pager.c:248`、`vm_compressor_backing_file.c`、
`kpi_vfs.c:847/1766`、`vfs_syscalls.c:2042/2895/14707`、`mockfs_fsnode.c:81`）都不在这条路上；如果 apfs 需要它，
`mt=` 这一栏会替它说话。`vfs_init_io_attributes()`（:1294）同样跳过：它只为根的换页调 I/O 尺寸，本轮是一个文件的一次缓冲读。

**namei 的引用归属是读源码定的，不是猜的**：`namei` 不吃调用方的 `start_dvp` —— `usedvp_dp` 短路掉那份额外的 usecount
（`vfs_lookup.c:427`），`lookup()` 用 `vnode_getwithref(dp)`（:1125）自取一份、在 :931 丢掉；返回的 `*vpp` 带一个 iocount。
所以阶梯里那句 `vnode_put(dir)` 放的正是「它来到时所在的那一级」，既不泄漏也不放空。

**为什么读 `SystemVersion.plist`**：`measureApfsDepth` 只能说「卷发布了」，读什么得自己挑一个「一定在、而且是二进制格式」的
路径。`System/Library/CoreServices/SystemVersion.plist` 的头 8 字节是 `bplist00`，按小端两个字读就是 `0x696c7062`、`0x30307473` ——
一行 panic 里能直接放得下「这些字节真是从卡上来的」的凭据。路径拆成四次单组件查找，因为一次拍照只带回一行，四级里停在哪一级
必须能说出来。没有挑 round128 缓存里那颗 `044-76040-038.dmg`（5,157,406,069 字节）：`round128-userdata-root.json` 记的是
`"source": "remote"`，文件不在本机，宿主机上算不出它的 inode 与偏移，也就没法预先知道该读哪个 LBA。

**不 fail-fast**：每一档都 `return` 而不 `panic`，全部 errno 汇成一行读数；`0xffff`（65535）= 「这一档没走到」，`0` = 走了且过了。
第一个 65535 就是失败点，它前面的字段依然是答案。这是「一次刷机只回答一个问题」的写法：挂载死在第三个路径组件，也得把前两级的
结果带回来。

**预算这轮换了性质**：round128 那 464 块是配置线程读的，而挂载 + 读文件是**缓冲缓存经由这颗设备**发 I/O，走的还是 PIO 那条
≈20 ms/512 B 的路（round127 实测）。真容器头几页就是几十秒，所以轮询仍是 12000 轮、`armcpu-clock-panic=180000`（≈11 分钟
@ ≈275 tick/秒）都留着。**如果这轮死在定时器上而不是死在这行上，`elr=` 是那笔慢读，不是 bug。**

**这台机器已经证明这级梯子爬得动**：round120 的 `rootvp not authenticated after mounting @bsd_init.c:1098` 出自一次
成功的根挂载**之后** —— 也就是 `bdevvp(makedev(1,39))` → mockfs → `buf_meta_bread(rvp, 0, 4096)` → `VOP_STRATEGY` →
`IOMediaBSDClient` → `IOBlockStorageDriver` → CloverSdhci PIO 全通过；round121 的 `Process 1 exec of /sbin/launchd failed,
errno 8 @kern_exec.c:8104` 同理。本轮只是把梯子指向一颗真卷，而不是 mockfs。

构建：

```
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-root/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && scripts/verify-clover-payload.py
```

`kernel` 这格照旧不能省（两个驱动都归 `[kernel] archives` 管）。本轮 `userdata` 不重建 —— 镜像一个字节都没变。
产物：内核本地 `dac3deef…` / 卷内 `1ea78352…`（51,994,624 B，探针偏移 6680 字节分布在 35 段里，其余逐字节相同）；
`payload.img` `327b215b…`（58,720,256 B = 56 MiB）；ADT `fd99843a…`（13,268 B）；`unixnu.toml` `33375ba3…`（600 B）；
`libCloverBlock.a` 296,888 字节。
根镜像 `614b8356…` 与信任缓存 `c3ed2152…` 这轮没动，boot 仍是 round87 那份 `196b284f…`，`userdata` 仍是
`round128-userdata-root.sparse.img` `be354a4a…`。归档在 `~/clover-out/round129-*`（`round129-SHA256SUMS`）。

checker 增量：`verify-clover-payload.py` 的 `check_block()` 新增 `CloverBlock r129: apfs`、`AppleAPFSVolume`、
`SystemVersion.plist` 三条（`AppleAPFSContainerScheme` 那类旧断言都保留）；`check-clover.py` 的根契约文案升到 round129，
断言本身不变：`rd=md0` 在场、`rd=md0s39`/`rd=disk0s39` 缺席、`clover-apfs=2` 与 `clover-emmc=21` 同时在场、
`[userdata]` 必须真的带 `windows`。

**本轮只刷一格：`cache`（负载）。** 这是有意声明的：**本轮不写任何新的 eMMC 分区** —— `userdata` 保持 round128 刷进去的那张
iPadOS 容器镜像，一个字节都不再动，`logfs` 与 `boot` 也不碰。

```
fastboot flash cache /Users/zhouzhou/clover-out/round129-payload.img   # 56 MiB，327b215b…
fastboot reboot
```

冷刷，不用 `fastboot boot`。菜单第一行应认得 `clover r129: mount iPadOS's published APFS volume and read a file back`。
一次刷机只回答一个问题：**这级梯子能不能走到 `rd=0 g=1024 x=0x696c7062 y=0x30307473`**（取在 `CloverBlock.cpp:1073`）。

`CloverBlock r129: apfs f= n= m= l= d= s= c= v= dv=major:minor bd= al= mt= st= rt= k=a/b/c/d rd= g= x=0x.. y=0x.. r= w=`
各字段怎么读：

- 前八字段与 round128 同义；`v=0` 时后面**全部**是 65535，本轮结论就退回「卷没发布」，不是挂载失败。
- `dv=major:minor` —— 卷媒体上的 BSD 号。`65535:65535` = `IOMediaBSDClient` 没给这颗叶子媒体命名，梯子第一步就无从下手。
- `bd=` —— `bdevvp()` 的 errno，非 0 是 vnode 表那一层的旧账（round120 已证明这条路本身在这台机器上走得通）。
- `al=` —— `vfs_rootmountalloc()`；非 0（`ENODEV`=19）说明 `vfsconf` 里没有名为 `apfs` 的 vfstable，也就是 apfs.kext 没把
  自己注册进去 —— 和 `l=` 一道读。
- `mt=` —— `VFS_MOUNT` 的 errno，**本轮真正的答案**：0 就是 Apple 的 apfs 挂载了一颗由本移植驱动的卷。`mnt_devvp` 那个缺口
  如果不够用，也是在这一栏说话。
- `st=` —— `VFS_START`。它不 fail-fast，所以即使非 0，`rt=` 与后面依然会有值。
- `rt=` —— `VFS_ROOT`；非 0 时 `k=` 全是 65535。
- `k=a/b/c/d` —— 四级各自的 errno：第一个既非 0 又非 65535 的位置就是路径断在哪一级（`ENOENT`=2 说明卷根里没有 `System`，
  那要么读的是 Data 卷，要么 BSD 号取错了卷）。
- `rd=` —— `vn_rdwr()` 的 errno。
- `g=` —— 实际读回的字节数（`1024 - resid`）。**>0 就是卡上的字节**，这是本移植第一次读到文件内容。
- `x=/y=` —— 头 8 字节的两个小端字；`0x696c7062 0x30307473` = `bplist00`，那就是读对了。
- `r=/w=` —— 现在里面是缓冲缓存的流量，应当**远大于** round128 的 464；`w=` 若不再是 0，说明 apfs 在挂载过程中写了东西，
  那是下一条要记的结果（本移植的 PIO 写路径至今没被真的文件系统碰过）。

**下一格的问题**（拿到答案之后）：如果读到的是 Data 卷而不是 `OS` 卷，`dv=` 与 `k=` 会先把它说出来，下一轮就按 BSD 号挑卷；
如果 `mt=` 非 0，下一格是把 `mnt_devvp` 那块布局补上；如果 `g=1024 x=0x696c7062 y=0x30307473` 真到了屏上，下一格就不再需要
探针 —— 该把 `rd=` 指向这颗卷、让内核拿它当根，走 `/sbin/launchd` 那条路。

## round129 读数（v0.3.2 实机）：apfs 认下了 `"apfs"` 这个名字，然后在挂载那一步回了 EACCES

屏上是完整的一行，八个 r128 字段一位没变，后面全部读到了值：

```
CloverBlock r129: apfs f=15797536 n=11870275 m=1 l=0 d=10 s=1 c=1 v=1 dv=1:3 bd=0 al=0 mt=13
st=65535 rt=65535 k=65535/65535/65535/65535 rd=65535 g=0 x=0x0 y=0x0 r=464 w=0 @CloverBlock.cpp:1082
```

伴生读数：`ticks=0x8b4`（2228，round128 是 2194）、`pages=0x3af7b free=0x3a21f`、tty 字段全空（EL1 的 panic，本该如此），
引导日志把这一帧落在 8.082 s 的 AMFI 初始化之后。

**第一个 65535 在 `st=`，所以失败点是 `mt=13`。** 逐栏读：

- `@CloverBlock.cpp:1082` —— `panic(` 从 :1073 起，宏报的是调用的收尾行，和 round128 的 `:861` 同一个约定。这句话的用处是
  证明刷进去的确实是补了 `vfs_unbusy` + `mount_list_add` 的那份 v0.3.2，而不是更早的 `f9c4a9d9…` 内核。
- `bd=0` —— `bdevvp(1<<24 | 3)` 收下了这颗卷的 dev_t，设备 vnode 建出来了。
- `al=0` —— `vfs_rootmountalloc("apfs", …)` 成功，也就是 **`"apfs"` 这个名字真的在 `vfsconf` 链上**（`vfs_subr.c:1181` 比的
  就是 `vfc_name`）。这是 round128 那次 kext 加载的后续：apfs.kext 不只被 load 了，它把文件系统注册进去了。
  `mp` 也分配成功（`ENOMEM` 会是 12 而不是 0），`MNT_RDONLY|MNT_ROOTFS` 也照 :1153 设上了。
- `mt=13` = **EACCES，而且只能是 apfs 自己回的**。`VFS_MOUNT` 的 KPI 壳（`kpi_vfs.c:192-213`）只有三种出口：
  `dead_mountp`、没有 `vfs_mount` 槽、`!vfs_64bitready` —— 三种都回 `ENOTSUP`(45)。除此之外它尾调
  `(*mp->mnt_op->vfs_mount)(mp, devvp, data, ctx)`。13 是那颗函数指针返回的。
- `r=464` **和 round128 一字不差** —— 也就是这次挂载尝试**一个字节的新块都没从卡上读**。这条最值钱：它把「apfs 读了容器头、
  读完觉得不对」和「apfs 看都没看容器就拒了」切开，剩下的是前者之外的另一种 —— 一次参数/状态层面的拒绝。
- `st=` 往后全 65535，`g=0 x=0x0 y=0x0 w=0`：没有任何文件被读，也没有任何东西被写。

**宿主机上拆掉的两条嫌疑（没花刷机）**：`hdiutil attach -nomount -readonly out/ipsw/21H461/044-76040-038.dmg` 之后
`diskutil apfs list disk31` 给出容器 `50F8A67F-…`、**只有一个卷** `disk31s1 = 04769D26-…`、role **System**、卷名
`DawnUpdate21H461.J120J121J207J208OS`（区分大小写）、**Sealed: No、FileVault: No**。所以 SSV 封印和加密都解释不了
这次 EACCES；读完 `diskutil` 就把镜像卸了。同一趟还校准了 BSD 编号：宿主 `rdisk30`→`0x1000042` … `rdisk31s1`→`0x1000045`，
major 恒为 1、minor 顺序分配 —— `dv=1:3` 的形状和那颗卷对象对得上，但**「这个 1:3 就是那颗卷」本身还没证死**。

**凭据不是问题**（源码层面关掉的第三条）：`vfs_context_kernel()` 返回的就是 `&vfs_context0`（`kpi_vfs.c:1601`），
`vfs_context_ucred()` 只是 `ctx->vc_ucred`（:1624），而 `vfs_context0.vc_ucred = posix_cred_create(&kernel_cred_template)`
（`kern_credential.c:3415`）—— 是 root 凭据，任何 `suser` 式的检查都该过。

**本轮读源码读出来的一条新事实**：真正的引导根并不总是 `VFS_MOUNT`。`vfs_subr.c:1251-1266` 是遍历 `vfsconf`，
`vfc_mountroot == NULL` 且没有 `VFC_VFSCANMOUNTROOT` 的直接跳过，命中的文件系统走
`(*vfsp->vfc_mountroot)(mp, rootvp, ctx)`，只有它为空才退到 `VFS_MOUNT`。而 kext 注册文件系统走的 `vfs_add` 在
`kpi_vfs.c:985` 把 `newvfstbl->vfc_mountroot` **硬性置 NULL**。所以对 apfs 这种 kext 文件系统，内核自己会走的正是我们抄的
那条 `VFS_MOUNT` —— 这条疑路可以关掉了（内置的 mockfs 不一样：`vfs_conf.c:204` 给它填了 `mockfs_mountroot`，那才是
round119/120 那次根挂载的形状）。

**之前那句「树里没人读 `mnt_devvp`，跳过不付代价」有个洞，这里补上**：那份枚举只覆盖了**这份归档里的源码**，而
apfs.kext 是闭源的、不在这棵树里。现在 `bd=0 al=0 mt=13 r=464` 的形状 —— 设备 vnode 建好了、名字认下了、一次块都没读就回
EACCES —— 和「mount 例程先看 `mp->mnt_devvp`，看到 NULL 就把这颗挂载点当成不可信来源」是同一句话。`mnt_devvp` 是头号嫌疑。

## v0.3.3（round130）：把 `mnt_devvp` 那一档补上，并且让结构体自己先作证

round129 的框停在 `mt=13`：`bd=0`、`al=0`、`r=464` 一个字节没涨。那一行把「apfs 读了容器再拒」和「apfs 看都没看就拒」
分开了，剩下能解释它的只有本轮唯一没抄的那一档 —— `vfs_mountroot()` 在调 mount 之前先写了 `mp->mnt_devvp = rootvp`
（`bsd/vfs/vfs_subr.c:1259`），而 apfs.kext 是闭源的，「树里没人读这个字段」那句话从来没覆盖过它。本轮就补这一档，
而且**不信自己的测量**：先让内核自己写进这颗 `mount` 的三个标记说话，证对了才往 `0x980` 写指针。

**偏移是从这次构建自己的 DWARF 里量出来的**，不是猜的，也不是抄别处的头文件：链接后的内核不带 DWARF，但构建目录里
有 `mach.development.bcm2837.dSYM`，`llvm-dwarfdump --name=mount --show-children` 在里面打出 **63 份** `struct mount`
（每个编译单元一份），63 份全部一致：

| 字段 | 偏移 | 出处 |
| --- | --- | --- |
| `mnt_list` | 0x00 | `mount_internal.h:109`，`mount_list_add()` 写的就是它 |
| `mnt_vfsstat` | 0x84 | :123 |
| `mnt_devvp` | 0x980 | :146，本轮要写的这一格 |
| `struct mount` 大小 | 0xa08 | —— |

`struct vfsstatfs` 同样是 63 份全一致：`f_fstypename` 在 +0x50、`f_mntonname` 在 +0x60、`f_mntfromname` 在 +0x460。
换算到 `mp` 的绝对偏移是 `0xd4` / `0xe4` / `0x4e4`。

**自证用的是内核自己写进去的三个标记**，都在 `vfs_rootmountalloc_internal()` 里，一个都不归我们填：
`strlcpy(mp->mnt_vfsstat.f_fstypename, vfsp->vfc_name, MFSTYPENAMELEN)`（:1161）、
`mp->mnt_vfsstat.f_mntonname[0] = '/'`（:1162）、`copystr(devname, ...f_mntfromname, ...)`（:1164）。
所以布局对的话，`mp+0xd4` 的 16 个字节必须是 `apfs` —— 按小端两个字读就是 `0x73667061` 后面跟三个 0 —— 紧挨着的
`mp+0xe4` 必须是 `0x2f`，`mp+0x4e4` 应该是这颗卷自己的 BSD 名前四个字节（`disk` ⇒ `0x6b736964`）。
**闸门是前两个**（名字 + `/`），第三个只是画出来给人看：它把 `dv=1:3` 从「某个被 BSD 客户端命名的媒体」变成
「这个名字的这颗卷」。三个标记是内核写的、而且没有一个是本轮要写的那一格，所以证的是整颗布局，不是自说自话。

**写与不写的话都落在同一行上**：证过了才 `*(void **)((uint8_t *)mp + 0x980) = devvp`，然后立刻把 `0x980` 读回来画进
`dp=高:低`。如果 `t=` 不是 `apfs`，`dp=` 就还是 `0x0:0x0`，`mt=` 依然有值 —— 那一帧说的是「布局没证成立，所以这一档
仍然没补」，而不是「补了还是被拒」。

**`SI_MOUNTEDON` 走导出符号，不再量第二个布局**：:1282 那句 `mp->mnt_devvp->v_specflags |= SI_MOUNTEDON` 里的
`v_specflags` 是 `v_specinfo->si_flags`（`bsd/miscfs/specfs/specdev.h:100`），比的是 `struct vnode` 和 `struct specinfo`
两颗结构，等于再赌两份偏移。内核导出了做同一件事的 `vnode_setmountedon()`（`bsd/vfs/kpi_vfs.c:2257`，它自己
`vnode_lock_spin()` 再置位），所以本轮调它，一个偏移都不多量。它在 `kernel.macho` 的导出表里（`nm` 里 `S`），
`build-clover-block.py` 的导入审计直接放行 —— 这也是把它加进 `verify-clover-payload.py` 断言的原因。

**顺序照内核**：`:1258` 分配 → `:1259` 写 `mnt_devvp` → `:1265` `VFS_MOUNT` → `:1282` `SI_MOUNTEDON` → `:1284`
`vfs_unbusy` → `:1286` `mount_list_add` → `VFS_START` → `VFS_ROOT` → 四级 `vnode_lookupat` → `vn_rdwr`。
本轮仍然没抄两件事：`vfs_init_io_attributes()`（:1294，只给根的换页调 I/O 尺寸）和 :1268-1279 那段
「`vfc_mountroot` 可能换掉 rootvp」的引用重算 —— 上一轮已经从源码关掉了前者的可能性：`vfs_add` 把
`newvfstbl->vfc_mountroot` 硬写成 NULL（`kpi_vfs.c:985`），kext 注册的文件系统只会走 `VFS_MOUNT`。

**预算没变**：round129 那次挂载尝试读的块数是 0（`r=464` 与 round128 一字不差），所以这一轮只要 `mt=` 还是非 0，
代价和上一帧同级；真过了 `mt=`，后面的路径查找才会开始花钱。`rd=md0`、`clover-apfs=2`、`clover-emmc=21`、
12000 轮 × `IOSleep(10)` 的等待与 `armcpu-clock-panic=180000` 全部原样。**如果这轮死在定时器上而不是死在这行上，
`elr=` 是那笔慢读，不是 bug。**

构建与上一轮同一条命令（两个驱动都在 `[kernel] archives` 里，`kernel` 这格照旧不能省），本轮 `userdata` 不重建 ——
镜像一个字节都没变：

```
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-root/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && scripts/verify-clover-payload.py
```

产物：内核本地 `dfbf2bca…` / 卷内 `1fa6c496…`（51,994,624 B，探针偏移 6680 字节分布在 35 段里，其余逐字节相同）；
`payload.img` `45961b9e…`（58,720,256 B = 56 MiB）；ADT `3ef0309d…`（13,268 B，`[payload] title` 进了 ADT，所以每轮都换）；
`unixnu.toml` `70185d82…`（593 B）；`libCloverBlock.a` 301,936 字节。根镜像 `614b8356…` 与信任缓存 `c3ed2152…` 没动，
boot 仍是 round87 那份 `196b284f…`，`userdata` 仍是 `round128-userdata-root.sparse.img` `be354a4a…`。
归档在 `~/clover-out/round130-*`（`round130-SHA256SUMS`）。

checker 增量：`verify-clover-payload.py` 的 `check_block()` 把身份串断言升到 `CloverBlock r130: apfs`，并新增
`_vnode_setmountedon` —— 本轮那一档是靠这个符号接上的，它要是没链进来，这轮问的就不是它。`check-clover.py` 的根契约
文案升到 round130，断言不变。

**本轮只刷一格：`cache`（负载）。** 这是有意声明的：**本轮不写任何新的 eMMC 分区** —— `userdata` 保持 round128 刷进去
的那张 iPadOS 容器镜像，`logfs` 与 `boot` 也不碰。

```
fastboot flash cache /Users/zhouzhou/clover-out/round130-payload.img   # 56 MiB，45961b9e…
fastboot reboot
```

冷刷，不用 `fastboot boot`。菜单第一行应认得 `clover r130: hand apfs the device vnode and ask the mount again`。
一次刷机只回答一个问题：**把 `mnt_devvp` 交回去以后，apfs 还拒不拒这次挂载**（`panic` 调用在 `CloverBlock.cpp:1124`，
屏上按惯例报收尾行 `:1137`）。

`CloverBlock r130: apfs f= n= m= l= d= s= c= v= dv=major:minor bd= al= t=0x../0x../0x../0x.. on=0x.. fm=0x..
dp=0x..:0x.. mt= st= rt= k=a/b/c/d rd= g= x=0x.. y=0x.. r= w=` 里新增的四栏：

- `t=` —— `mp+0xd4` 的 16 个字节。**期望 `0x73667061/0x0/0x0/0x0`**；不是这个样子就说明 dSYM 量出来的布局在这台机器
  上不适用，那 `dp=` 一定是 `0x0:0x0`，本轮结论退回「布局没证成立」，`mt=` 不再是答案。
- `on=` —— `mp+0xe4`。**期望 `0x2f`**，内核在 :1162 写的那一个 `/`。它挨着 `f_fstypename`，等于在另一个字节上再证一次。
- `fm=` —— `mp+0x4e4` 的前四个字节，也就是我们交给 `vfs_rootmountalloc()` 的那颗卷的 BSD 名。期望是 `disk…`
  （`0x6b736964`）。这一栏是 `dv=major:minor` 的身份证明。
- `dp=` —— 写完立刻从 `mp+0x980` 读回来的指针，高 32 位:低 32 位。非 0 说明这一档真的补上了；它和 `mt=` 一起读，
  才能把「补了还是被拒」和「没补成所以还是被拒」分开。

**下一格的问题**（拿到答案之后）：`mt=0` 就往 `st=`/`rt=`/`k=`/`g=` 走，第一个非 65535 的字段是下一条要问的东西；
`mt` 仍是 13 而 `t=`/`on=` 都对，那 `mnt_devvp` 就被排除，下一格该问的是「内核自己在这种情形下还会先做什么」——
也就是 `MNT_ROOTFS`/`mnt_kern_flag` 那一族状态位，或者 apfs 需要先挂容器再挂卷；届时最省事的一步是先让探针把
`mnt_data`（`mp+0x8f8`，挂载成功才会被文件系统填非 0）和 `vfc_flags` 画出来，而不是再猜一个 errno 的理由。

## round130 读数（v0.3.3 实机）：结构体自己作了证，`mnt_devvp` 于是被排除

```
CloverBlock r130: apfs f=15797536 n=11870275 m=1 l=0 d=10 s=1 c=1
v=1 dv=1:3 bd=0 al=0 t=0x73667061/0x0/0x0/0x0 on=0x2f fm=0x6b736964
dp=0x?????????:0xa004dc30 mt=13 st=65535 rt=65535 k=65535/65535/65535/65535
rd=65535 g=0 x=0x0 y=0x0 r=464 w=0 @CloverBlock.cpp:1137
```

`pages=0x3af7b free=0x3a25f`，`@CloverBlock.cpp:1137` —— 正是发布前算好的收尾行，所以这一帧确定是 round130 的负载。
前八字段与 round128/129 一字不差（同一颗容器、同一颗卷、同一棵树）。`dp=` 的高 32 位在照片上糊了，低位是
`0xa004dc30`；本轮的结论不依赖那半格，因为要读的是「非 0 与否」。

三件事一次做完了：

1. **dSYM 量出来的布局在这台机器上是对的。** `t=0x73667061/0x0/0x0/0x0`、`on=0x2f`、`fm=0x6b736964` —— 内核自己写进
   这颗 `mount` 的三个标记全部落在量出来的字节上：`mp+0xd4` 拼出 `apfs`，`mp+0xe4` 是那个 `/`，`mp+0x4e4` 是 `disk`。
   闸门因此放行，这是「让结构体自己作证」这一手第一次真的作成了证 —— 它同时也把 `dv=1:3` 从「某个被 BSD 客户端命名的
   媒体」钉成了「这颗名字叫 `disk…` 的卷」。
2. **`mnt_devvp` 这一档确实补上了。** `dp=` 非 0，是从 `mp+0x980` 读回来的值，不是我们手里的局部变量。
3. **`mt=13` 一个字节都没变，`r=464` 也一个字节都没变。** apfs 拿到设备 vnode 之后，仍然在没有从卡上读一个新块的情况
   下回了 EACCES。

于是被排除的是本轮的假设本身：**apfs 不是因为 `mnt_devvp` 空着才拒这次挂载。** 这是能解释 13 的假设里被证伪的第二个
（第一个是「卷被 sealed / 加密」和「凭据不是 root」，round129 之前就在宿主上用 `hdiutil` + `diskutil apfs list` 和
`vfs_context0.vc_ucred` 排掉了）。剩下的候选全在参数与状态这一层 —— 因为零读盘意味着它连容器头都没去看：

- `data` 传的是 0。真机上这颗文件系统是被 `mount_apfs()` 挂上来的，那里有一份 `apfs` 私有的挂载选项结构（卷的角色、
  快照、钥匙串引用都在里面），我们什么都没给。
- iOS 上 apfs 挂载要拿 keybag（即便卷是 no-protection 的），拿不到 `AppleKeyStore` 就是 EACCES，而且不读盘。
- 顺序：可能要求先挂容器再挂卷，或者要求 `devvp` 是整盘/容器媒体而不是卷。
- 状态位一族（`MNT_RDONLY|MNT_ROOTFS` 由 `vfs_rootmountalloc_internal()` 写好了，所以这条最弱）。

**方法上的转折**：到这一帧为止，我们唯一没做过的事就是**去看那个回了 13 的函数**。前面三轮的问法都是「把内核的梯子
抄全一点，看它会不会点头」，而它每轮只是把同一个 13 再画一次。下一步应该换成读它，而不是猜它 —— 而且这件事大部分可以
在宿主上做，不花刷机：apfs.kext 的代码就在链接好的 `kernel.macho` 里（`nm` 里 67,964 个符号没有一个是它的内部函数，
Apple 的 kernelcache 同样只剩 `kern_apfs_reflock_*` 这十个，所以内部符号是剥掉的），但 `struct mount` 里存着走到那个
函数入口的路径，全部量得出：`mnt_op` 在 `mp+0x28`（`vfs_rootmountalloc_internal()` 在 `vfs_subr.c:1151` 就是照着
`vfsp->vfc_vfsops` 填的），`struct vfsops` 大小 0xa0、**第一格就是 `vfs_mount`**（同一份 dSYM，63 份一致）。所以
`op=*(mp+0x28)`、`mm=*op` 两个字画出来，就是「谁回了 13」的地址，回到宿主上直接反汇编那个函数。

**同一份 dSYM 顺手量出的另外三格**（本轮没用到，是下一格的候选）：`mnt_flag` 在 `0x70`、`mnt_kern_flag` 在 `0x74`、
`mnt_data` 在 `0x8f8`。`mnt_flag` 这一格值得单独说：`vfs_rootmountalloc_internal()` 在 :1153 写的是
`MNT_RDONLY | MNT_ROOTFS`（`0x1 | 0x4000 = 0x4001`，`bsd/sys/mount.h:296` 与 :326），再 `|=` 上 vfstable 的可见标志。
也就是说**我们这颗 mount 从分配的那一刻起就自称是根文件系统**，而这正是内核自己走 `vfs_mountroot()` 时才有的形状。

**上游仓库里有一条现成的证据说这件事有分量**（本轮才发现，出处都是 UniXNU 主干，不是猜测）：
`scripts/qemu-entry/native-apfs-mount.inc` 是一份**在 QEMU 里真挂成功过**的 apfs 数据挂载 —— 它走 `mount(2)`
（`"apfs"`、挂载点、`MNT_RDONLY`、一份 0x150 字节的 `mount_apfs`），那份参数的形状在 :45-47 写得很清楚：
`fspec` 在 0（一个 `/dev/disk…` 路径字符串）、`MNT_RDONLY` 在 8、**`mode` 在 0x10，`1` 是普通数据挂载，`0` 是「请求当启动根」**，
`0x144/0x148` 是显式的 uid/gid 99。`scripts/check-native-apfs-file-checkpoints.py:115` 断言那次挂载的返回是
`mount_results=(0,0,0,0,2)`（挂载 errno 0，走到了第 2 档），而 `:122` 记的是 `original_user_root_mount_mode_refused=True`
—— **mode 0 那种「把我当根挂」的请求，apfs 是拒的**。`scripts/build-native-user-fixture.py:23` 的 `--apfs-mount-mode`
只有 `(0,1)` 两个取值、默认 1，第 32 行还硬性要求做缓存探针时 mode 必须是 1。

把这两边对上，本轮剩下的候选就排出了顺序，而且第一条便宜到可以立刻问：**我们交给 apfs 的那颗 `mount` 带着
`MNT_ROOTFS`，而 `MNT_ROOTFS` 形状的唯一合法来源是 `vfs_mountroot()` 自己**。清掉这一位再挂一次，是同一个「结构体自己
作证」的套路（`mnt_flag` 在 :1153 之后应该正好是 `0x4001 | vfc_flags`，期望值本身就是偏移正确的证明），一次刷机一个变量。

顺带记一笔抄漏的差别：内核在 :1254 传给 `vfs_rootmountalloc_internal()` 的 devname 是字面量 `"root_device"`，
不是 BSD 名；我们传的是 `"disk0s64"`（`fm=0x6b736964` 证的就是这一格真的写进去了）。这条不是本轮的假设，只是抄的时候
和原作不一样的一处，画在这里免得以后当成别的证据读。

**下一格按读数分支**（本轮的分支）：`mt=13` 且 `t=`/`on=`/`fm=` 全对 —— 走「读那个函数 + 清 `MNT_ROOTFS`」这条路，
不再猜它。

## v0.3.4（round131）：清掉 `MNT_ROOTFS` 再问一次，并把那个回 13 的函数地址画在同行

**一次刷机问一件事**：这颗 `mount` 自称是启动根（`mnt_flag` 里的 `MNT_ROOTFS`），apfs 是不是就为这个说不。
同一个变量之外，本轮顺手做一件一直没做的事 —— 把「谁在回答」的地址取出来：`mnt_op` 在 `mp+0x28`，`struct vfsops`
第一格就是 `vfs_mount`，所以 `mm=` 那两个字就是过去三轮一直回 `13` 的那个函数的地址。它不改变任何行为，纯观察，
而且无论这轮的 `mt=` 是 0 还是 13 都不亏 —— 前者说明根标志就是理由，后者给下一格一份能直接反汇编的目标。

**为什么值得问这一位**（两条都是量出来或抄来的，不是猜的）：

- `vfs_rootmountalloc_internal()` 在 `bsd/vfs/vfs_subr.c:1153` 写死 `mp->mnt_flag = MNT_RDONLY | MNT_ROOTFS`
  （`0x1 | 0x4000 = 0x4001`，`bsd/sys/mount.h:296` 与 `:326`），再 `|=` 上 vfstable 的可见标志。也就是说凡是走这个
  分配器出来的 `mount`，都带「我是启动根」的形状 —— 而这正是只有 `vfs_mountroot()` 才该有的形状。
- 主干里有一份**真挂成功过**的 apfs 数据挂载：`scripts/qemu-entry/native-apfs-mount.inc` 走 `mount(2)`，参数
  0x150 字节，`mode` 在 +0x10，`:45-47` 写得很明白 —— 1 是普通数据挂载，**0 是「请求当启动根」**；
  `scripts/check-native-apfs-file-checkpoints.py:115` 断言 mode 1 那次 `mount_results=(0,0,0,0,2)`（errno 0），
  `:122` 记的是 `original_user_root_mount_mode_refused=True`。apfs 对「把我当根挂」这种请求是拒的。

**布局证明照旧，而且这一位自带期望值**：写之前仍然要求 round130 那两个内核自己写的标记成立（`t=` 拼出 `apfs`、
`on=0x2f`），再加第三条 —— `(fl & 0x4001) == 0x4001`。`mnt_flag` 的期望值本身就是这一格偏移正确的证明，形状和
上一轮的三颗名字标记一样。闸门过了才做 `*flagSlot = fl & ~MNT_ROOTFS`，并且**立刻把这个槽读回来**画进 `fc=`。
`fk=`（`mnt_kern_flag`，`mp+0x74`）只画不写，`op=`/`mm=` 也只读不写：这一轮一共只动了一个位。

新的五栏这样读：

- `fl=0x..` —— `mnt_flag` 在内核手里时的原值。至少该含 `0x4001`；若不含，说明这颗 `mount` 不是 :1153 那条路填的，
  本轮的清除没发生，`fc=` 会是 `0x0`。
- `fc=0x..` —— 清完立刻读回来的值。期望 `fl & ~0x4000`。它是「清掉了」的凭证：`fc=0x0` 配 `fl` 正常，就是闸门没放行，
  结论退回「这一位没得清」而不是「清了还被拒」。
- `fk=0x..` —— `mnt_kern_flag`，本轮没有依据去动它，画出来只为下一格留一份基线。
- `op=0x..:0x..` —— `*(mp+0x28)`，这颗文件系统 vtable 里的 `struct vfsops` 指针；非 0 才谈得上下一栏。
- `mm=0x..:0x..` —— `*op`，也就是 `vfs_mount` 的地址。减去内核的加载偏移就是 `kernel.macho` 里的文件偏移，
  在宿主上 `objdump --start-address=` 直接读那个函数。

**`mt=` 的三种分岔**：`0` —— 根标志就是理由，本轮之后按 `st=`/`rt=`/`k=`/`g=` 往下走；仍是 `13` —— 停止猜它，
下一格拿 `mm=` 反汇编，看它到底查哪一样（`mnt_data`/keybag/容器顺序都会在那里露头）；`fc=0x0` —— 布局或被写入的
那一格和预期不符，`fl=` 是答案。

**预算不变**：清除一个标志位不花钱，本轮仍然大概率零新读块（`r=464` 是上一帧的数）。`rd=md0`、`clover-apfs=2`、
`clover-emmc=21`、等待轮数与 `armcpu-clock-panic=180000` 全部原样；真过了 `mt=`，后面的查找才开始花钱。
**如果这轮死在定时器上而不是死在这行上，`elr=` 是那笔慢读，不是 bug。**

构建命令与上一轮相同（两个驱动都在 `[kernel] archives` 里，`kernel` 那格照旧不能省），本轮 `userdata` 不重建 ——
镜像一个字节都没变：

```
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-root/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

产物：内核本地 `07ef9d17…` / 卷内 `b5580172…`（51,994,624 B，探针偏移仍是 6680 字节分布在 35 段里，其余逐字节相同）；
`payload.img` `c269a2eb…`（58,720,256 B = 56 MiB）；ADT `3b0af500…`（13,268 B，`[payload] title` 进了 ADT，所以每轮都换）；
`unixnu.toml` `24ddb306…`（602 B）；`libCloverBlock.a` 307,400 字节。根镜像 `614b8356…` 与信任缓存 `c3ed2152…` 没动，
boot 仍是 round87 那份 `196b284f…`，`userdata` 仍是 `round128-userdata-root.sparse.img` `be354a4a…`。
归档在 `~/clover-out/round131-*`（`round131-SHA256SUMS`）。

checker 增量：`check_block()` 的身份串升到 `CloverBlock r131: apfs`，并新增两条 panic 行片段断言
`fl=0x%x fc=0x%x fk=0x%x` 与 `op=0x%x:0x%x mm=0x%x:0x%x` —— 本轮一个新导入符号都没有，全靠量出来的偏移，
所以格式串里的这五个字段是「这轮真的在问这件事」的唯一可见证据。`check-clover.py` 的根契约文案升到 round131，
断言不变。

**本轮只刷一格：`cache`（负载）。** 这是有意声明的：**本轮不写任何新的 eMMC 分区** —— `userdata` 保持 round128 刷进去
的那张 iPadOS 容器镜像，`logfs` 与 `boot` 也不碰。

```
fastboot flash cache /Users/zhouzhou/clover-out/round131-payload.img   # 56 MiB，c269a2eb…
fastboot reboot
```

冷刷，不用 `fastboot boot`。菜单第一行应认得 `clover r131: clear the boot-root flag and draw the function that refuses`。
一次刷机只回答一个问题：**把 `MNT_ROOTFS` 清掉之后，apfs 还拒不拒这次挂载**（`panic` 调用从 `CloverBlock.cpp:1175` 起，
屏上按惯例报收尾行 `:1192`）。

`CloverBlock r131: apfs f= n= m= l= d= s= c= v= dv=major:minor bd= al= t=0x../0x../0x../0x.. on=0x.. fm=0x..
dp=0x..:0x.. fl=0x.. fc=0x.. fk=0x.. op=0x..:0x.. mm=0x..:0x.. mt= st= rt= k=a/b/c/d rd= g= x=0x.. y=0x.. r= w=`
里前九栏与 round130 逐字相同（同一颗容器、同一颗卷、同一棵树、同一个 `mnt_devvp`），新增的就是上面那五栏。

## round131 读数（v0.3.4 实机）：根标志不是理由 —— 把 `mm=` 摊开之后，函数里回 13 的只有两处，两处都不是标志

屏上这一行（`@CloverBlock.cpp:1192`，和发布前预测的收尾行一致，所以镜像身份无需再验）：

```
CloverBlock r131: apfs f=15797536 n=11870275 m=1 l=0 d=10 s=1 c=1 v=1 dv=1:3 bd=0 al=0
t=0x73667061/0x0/0x0/0x0 on=0x2f fm=0x6b736964 dp=0xffffffff:0xa6dc9e20
fl=0x4005001 fc=0x4001001 fk=0x0 op=0xfffffff0:0x05f2b3c8 mm=0xfffffff0:0x056c5fac
mt=13 st=65535 rt=65535 k=65535/65535/65535/65535 rd=65535 g=0 x=0x0 y=0x0 r=464 w=0
```

照片把 `dp=`/`op=`/`mm=` 的高半字糊成 9 位（和 round130 一样），这一格在宿主上判定而不是靠再数一遍像素：
`op` 落进 `__PLK_DATA_CONST`、`mm` 落进 `__PLK_TEXT_EXEC`，而且 **`op` 槽 0 读回来正好等于 `mm`**，
所以两者的高半字是 `0xfffffff0`；`dp` 是一颗内核数据里的 vnode，取 `0xffffffff`（和同一屏控制台段那些
`0xffffffff…` 指针同形）。

**本轮那一件事的答案**：

- `fl=0x4005001` 含 `0x4001` —— `mnt_flag`（`mp+0x70`）确实是 :1153 写出来的形状，这是这颗 `mount` 的第四颗布局钉子。
- `fc=0x4001001` 恰等于 `fl & ~0x4000` —— `MNT_ROOTFS` 真的被清掉了，一位不多一位不少。
- `mt=13`、`r=464` 一个字没变 —— 于是**启动根声明也被排除**。三个假设到此全没了：round129 之前挂在宿主上的
  密封/非 root 凭据、round130 的 `mnt_devvp`、round131 的 `MNT_ROOTFS`。

**`mm=` 白送的那一格立刻兑现**（不花刷机）：`0xfffffff0056c5fac` 在 `kernel.macho` 的文件偏移是 `0x268dfac`，
落在 apfs.kext 的 `__PLK_TEXT_EXEC`。Xcode 的 objdump 不认这段（fileset 里它只是裸的段数据，既没有节头，
`--section=` 报 not found，`-b binary`/`--file-type=obj` 也不接），所以按 `otool -l` 的段表用 capstone 手工走。
函数自己报了名字：`apfs_vfsops.c` 的 **`apfs_vfsop_mount`**。它确认 `VFS_MOUNT(mp, devvp, 0, ctx)` 的
`(*mnt_op->vfs_mount)(mp, devvp, data, ctx)` 四参形状（`kpi_vfs.c:193`），也确认 `data=0` 时它把那份
0x144 字节的挂载参数**全清零**再往下走。

整颗函数（约 1400 条指令，覆盖到 `0x56c758c` 之后的收尾）里 `mov w?, #0xd` **只有一处**：

- `0xfffffff0056c69c8`，前面六条指令引用 `handle_crypto_mount`，日志格式在 `0xfffffff0045d04e9`：
  `"%s:%d: rejecting crypto mount: OS variant is not recovery"`（`apfs_vfsops.c:1540`）。它的闸门在
  `0xfffffff0056eb540`：读 boot-arg `rd`（没有再退 `rootdev`），要求值是 `md?` 三个字符 —— 「根是内存盘」。
  ~~**我们的 `rd=md0` 满足它**，所以这道门按宿主推断是开的~~ —— 这一句写错了，两个错：我们的 `rd=` 是 `md0s39`
  不是 `md0`，而且这道门是「谓词**为假**才拒」。见下一节的复核，它连我们这条路都不在。

第二处拒绝在同一颗函数后半，走的不是硬编码常量而是同一句 `return`：快照查找返回空（`cbz x25` @`0x56c7b5c`）、
且卷对象 `+0x38` 那颗字节的 bit5 置位时，跳到 `0xfffffff0056c81c8`，画的是 `0xfffffff0045d009d`：

```
"Failed to find the root snapshot: %s (%d). Rooting from the live fs of a sealed volume is not allowed on a RELEASE build"
```

（`apfs_vfsops.c:2829`。）而快照名从哪来，同一段文本旁边就写着：**`/chosen` 的 `root-snapshot-name`**，
读点在 `0xfffffff005651a54`，且只在块大小 4096 那条路上读。周边还能挖出几颗板级开关：
`/chosen` 的 `boot-uuid`/`apfs-preboot-uuid`、`/options` 的 `enable-remap-mode`、`/defaults` 的
`no-effaceable-storage`、`/filesystems` 的 **`DoNotUseSEP`**（`0xfffffff0056d1f14`）。我们的 ADT 一颗都没发布。

**这一条主干里有现成对照**，而且 checker 的名字就叫「保留密封根的拒绝」——
`~/UniXNU/scripts/check-original-apfs-root-checkpoints.py`：

- `:51` 记录真 iPadOS 镜像里那颗 OS 卷的属性：`Status=Online`、**`Sealed=No`**、
  `FullName=DawnUpdate21H461.J120J121J207J208OS`、`BSD Name=disk1s1`。
- `:56-60`：挂这颗**没密封**的 OS 卷，`vfs_mount` 在同一颗栈上回 **0**，167 次读、0 次失败。
- `:66-68`：**SystemCryptex** 那颗卷挂的时候确实是被上面那句拒的（`sealed_cryptex_root_refused=True`），
  主干把它当负对照钉住。
- `:104-118`：根的选择走 ADT `/chosen` 的 `root-matching` = `{IOProviderClass: IOMedia,
  IOPropertyMatch: {UUID: <卷的 UUID>}}`，那一轮 `rd=` 与 `boot-uuid=` 都不写。
- `:84-90`：挂上之后还有下一颗钉子 `rootvp not authenticated after mounting` —— 那是 `mt=0` 之后的事。

**而我们从头到尾不知道自己在挂哪一颗卷。** `measureApfsDepth()` 是第一颗 `AppleAPFSVolume` 赢
（`if (!depth->volumeMedia)`），`dv=1:3` 只是主次设备号，帧里没有一个字说这颗卷叫什么、密封不密封。
上游的对照恰好是：同一颗 apfs、同一个内核，对没密封的 OS 卷回 0，对密封的 Cryptex 回这句 EACCES。
**`mt=13` 与「我们挑中的是那颗密封的卷」完全一致，而且这是第一个不用猜标志位的解释。**

**这一句在 round132 的宿主预检里被推翻了。** 我们把这一轮真正要挂的那段字节挂上去只读了一遍：
round128 manifest 里的 OS 窗口（偏移 1048576、长 6077580800、sha `e5634d04a8ac…`）用 `hdiutil attach -readonly`
挂起来，`diskutil apfs list` 报出来的只有**一颗卷，而且它就是没密封的那颗**。也就是说「挑错了卷」在宿主上不成立；
它唯一还没被证掉的地方是**实机的 IOKit 到底枚举出什么** —— 这台板子的 `AppleAPFSVolume` 从来没有被画过。
所以它仍然值得刷一次，但它是「把未知画成已知」，不是「答案找到了」。下一节把预检读到的东西全部记下来。

**下一格的候选**（排序短清单，都只动一件事）：

1. ~~**选卷，不改挂载**：挂之前把卷的身份画出来，并挑那颗 `Sealed=No` 的 OS 卷。~~ —— round132（v0.3.5）就是这一格，
   已按原话实现：新增 `vt=/un=/vs=/vo=/vu=/vc=/vn=` 七栏身份，选中规则改成优先 `Sealed=No`，`VFS_MOUNT` 调用一个字节没动。
   宿主对它的期望是 `vt=1 un=1 vs=0x6f4e`（"No"）—— 也就是身份应当**和今天一样**，这一轮买的是实机的证明。
2. 若仍 13：**ADT 加 `/chosen { root-snapshot-name = <快照名>; }`**，给密封卷一个根快照名（宿主先解析
   容器里到底有没有快照，这一步不花刷机就能先证伪）。
3. `mt=0` 之后：**`/filesystems { DoNotUseSEP; }`** 与 `rootvp not authenticated` 那颗钉子。

## round132 的宿主复核：`apfs_vfsop_mount` 到底怎么走到 13 的

round131 交出 `mm=0xfffffff0056c5fac`，就是 `apfs.kext` 的 `apfs_vfsop_mount`（`kernel.macho` 里的文件偏移
`0x268dfac`，段 `__PLK_TEXT_EXEC`；objdump 走不了 fileset 段，所以用 capstone 按段映射反汇编）。
复核把上一条主干里我自己讲错的两处纠正了，也把「密封根那句确实在这颗函数里」这一条钉住了。

**函数边界。** 序言 `stp x28,x27,[sp,#-0x60]!` 在 `0x56c5fac`；热路径上的金丝雀检查 + `add sp,sp,#0x820` + `ldp…` + `ret`
在 `0x56c7604` —— 但这**不是**结尾。`0x56c7608..0x56c81ec` 那些冷块用的是同一套 0x820 帧槽（`[sp,#0x370]`、`[sp,#0x60]`、
`[sp,#0xd8]`、`[sp,#0x44]`），并且分支回到函数体内部的地址（`0x56c7570` / `0x56c71d8` / `0x56c6554`）。
`0x56c81f0..0x56c8210` 是 mode 跳转表的字（capstone 把它们渲染成 `udf #0x454/#0x368/#0x10`），下一颗函数的
`sub sp,sp,#0x110` 要到 `0x56c8214` 才出现。**结论：那句「从密封卷的 live fs 起根是不允许的」（log 行 `0xac0`=2752、
字符串 `0xfffffff0045d009d`）就在 `apfs_vfsop_mount` 里面**（`container_mount` 被内联了）。上一节发布的那句成立。

**mode 开关，以及我们其实走哪条路。** 参数是一块 zone 分配的 0x144 字节 `mount_apfs`；`data != NULL` 走 copyin
（`bl 0x576847c`），`data == NULL` 走 `memset`（`0x56c60f4..0x56c6118` 那串 stp q0）再填 `-1` 哨兵
（`movi v0.2d,#-1; str d0,[x21,#0x28]`）。`mode` 是 `args+8` 处的 u16：`ldrh w8,[x19,#8]; cmp w8,#8; b.hi`，
表基 `adr x10,#0x56c6388` ⇒ mode0→`0x56c67dc`，mode1/8→`0x56c66f0`，mode2/3→`0x56c66ec`，**mode4..7→`0x56c6398`**。
我们调的是 `VFS_MOUNT(mp, devvp, 0, ctx)` ⇒ **mode 0，args 全零**。

**那颗函数里唯一硬编码的 EACCES 不在我们这条路上。** `mov w26,#0xd` 在 `0x56c69c8` —— 4000 条指令里只出现这一次 ——
而它只从 **mode 4..7** 那条臂可达：门在 `0x56c64bc ldrh w8,[x19,#8]; cmp w8,#4; b.ne 0x56c6888`，
然后 `0x56c6894 bl 0x56eb540` → `0x56c6898 tbz w0,#0, 0x56c69ac`（log `handle_crypto_mount` 行 `0x604`，
`"rejecting crypto mount: OS variant is not recovery"`）。**方向也讲反了：谓词为假才拒。**
`0x56eb540` 读 boot-arg `"rd"`（退回 `"rootdev"`），要求恰好三个字符 `md?`（`buf[0]=='m'`、`buf[1]=='d'`、`buf[3]==0`）；
我们的 alias 是 `rd=md0s39`，四个字符 ⇒ 谓词为假 ⇒ 会被拒。**但那一整条臂 mode 0 根本到不了**，所以它不是我们的 13。

**mode 0（我们）的路线。** 本地标志 `orr w8,w26,#0x20; strh w8,[sp,#0xb8]`，`args+4 |= 1`；
然后 `bl 0x576859c`（`current_proc`）与 kernel_task 全局 `0xfffffff005f25978` 比 —— 不等就 `w26=1`（EPERM，log `0x9a4`）。
这是**一颗还没被排除、但我们应当能过的门**：探针跑在内核线程上。再往后是 `cbz x22,0x56c6d0c`（x22 = devvp），
然后 `bl 0x567fa0c` 且 `cbz w0,0x56c6ef0`，否则打 log `0x9af` 并**把被调者的 errno 原样返回**。
`0x567fa0c` 是 `container_rootmount`（串：`AppleAPFSContainer`、`AppleAPFSContainerScheme`、`AppleAPFSMedia`、
`IOMedia`、`BSD Name`、`BSD Major`、`/dev/`、`%s%s`）。它里面**没有 `#0xd`**（自己的常量只有 2、15、79），
但它同样在普通（非 RAMDisk）那侧用 `0x567fa84` 重测同一个 `md?` 谓词。**⇒ 我们的 13 是从更深处传上来的。**

**顺带把 round131 的另一件事查实了。** apfs 自己的标志掩码在 `0x56c6130`：`mov w24,#0xf7fe; movk w24,#0xdff0,lsl #16`
（=`0xdff0f7fe`）→ `orr w8,w24,#1`（=**`0xdff0f7ff`**）→ `and w24,w23,w8`，`~0xdff0f7ff = 0x200f0800` ⇒
**bit `0x4000`（`MNT_ROOTFS`）是被保留的**。也就是说 round131 清掉的那一位确实送到了函数面前 —— 这条不能反过来发。

**宿主把卷的数量与身份也读了。** 就是上面那段挂载的 OS 窗口：`diskutil apfs list` 只有**一颗**卷、`Sealed` 是 `No`。
于是 round132 的帧有了具体期望（FNV-1a 32 哈希 + 短串小端打包）：
`vt=1`（卷数）、`un=1`（`Sealed=No` 的颗数）、`vs=0x6f4e`（"No"）、`vo=0x696c6e4f`（"Online"）、
`vu=0x01d874fd`、`vc=0xa7dd292a`（容器 UUID）、`vn=0x6e776144/0x61647055/0x31326574/0x31363448`
（`Dawn` / `Upda` / `te21` / `H461`）。

## v0.3.5（round132）：把挂的那颗卷画出来，并优先没密封的那颗

**这一轮只改两件事，按原话：把扫到的每颗 `AppleAPFSVolume` 的身份画出来，并把挂载对象换成那颗 `Sealed=No` 的 OS 卷，其余一律不动。**

- `drivers/CloverBlock/CloverBlock.cpp`（现在 1652 行）新增 `kFullNameKey`/`kVolumeUuidKey`/`kSealedKey`/`kStatusKey`
  四把键（注释里点明出处是 `check-original-apfs-root-checkpoints.py:50-56`），以及 `stringProperty()`、`packedHead()`、
  `fnv1a()` 三个小工具。只读**字符串**属性 —— 驱动那份 include 树里没有 `libkern/c++/OSBoolean.h`，而上游自己就把
  `Sealed`/`Status` 当字符串读（它的 `registry_string_events` 证明了这一点）。
- 新的 `VolumeIdentity{name[4], uuid, sealed, status}` 与 `readVolumeIdentity()`：`FullName` 头 15 个字符按四个
  `le32At` 字取，`UUID`/`Sealed`/`Status` 走 FNV-1a；缺串 ⇒ `0xffff`（`kNoProperty`）。
- `measureApfsDepth()` 里 `volumeCount` 逐颗累加（封顶 255），`unsealedCount` 数 `Sealed=="No"`，容器的 `UUID` 哈希一次；
  选卷规则从「第一颗 IOMedia 赢」改成「**手里是密封卷、来了颗没密封的就换**」，换掉的旧 media 立即 `release()`。
- **没动的部分**：`VFS_MOUNT(mp, devvp, 0, ctx)` 一个字节没改，flags、`mnt_devvp`、`data` 全部照 round131；读块预算不变，
  身份是注册表属性，**零次新读**，`r=` 应当仍是 464。

**产物**（`~/clover-out/round132-*`，`round132-SHA256SUMS`）：payload `round132-payload.img` 58,720,256 B
`353770b70331e7b48d833cd4ba81ff5f0f5c43498c6d3154a7e4b317c6c1bd54`；内核 `round132-kernel.macho`
`147d5090bd972240da401116f79ff65a03eba31c188b6d46e21dd1c76b642690`；卷内内核
`round132-in-volume-kernel.macho` 51,994,624 B `c3788ad230843b1cbf4044b1c83f5b877119a62d0388d07d0da0848646facd17`；
ADT 13,268 B `427b6b1579f2…`；trustcache 与 scratch-data 沿用 round128（`c3ed2152…` / `614b8356…`）。
`scripts/check-clover.py` 与 `scripts/verify-clover-payload.py` 都过（后者的 found 集加了 `CloverBlock r132: apfs`、
`vt=%u un=%u vs=0x%x vo=0x%x vu=0x%x vc=0x%x`、`FullName`、`Sealed`）。

**这一轮只写 `cache` 一个分区，没有新的 eMMC 分区。** `userdata` 保持 round128 的镜像（`be354a4a…`）不动，
`logfs`、`boot`、GPT 一概不碰 —— 身份是 IORegistry 里的属性，读它不需要改盘。

```
fastboot flash cache /Users/zhouzhou/clover-out/round132-payload.img
fastboot reboot
```

（冷刷，绝不用 `fastboot boot`。）

**屏幕上应当读到**：菜单首行（`board.toml` 的 `payload.title`）`clover r132: draw which volume the mount was aimed
at, and prefer the unsealed one`，帧尾部 `@CloverBlock.cpp:1369`，新七栏的期望就是上一节那串（`vt=1 un=1 vs=0x6f4e vo=0x696c6e4f vu=0x01d874fd
vc=0xa7dd292a vn=0x6e776144/0x61647055/0x31326574/0x31363448`），`mt=` 与 `r=` 沿用 round131（13 与 464）。

**三种读法**：

1. **身份与宿主一致（`vt=1 un=1 vs=0x6f4e`），`mt` 仍 13** —— 选卷这一格在实机上关闭，13 与「挂错卷」无关。
   下一格是候选 2：`/chosen { root-snapshot-name = …; }`，并在宿主先把容器里的快照名解析出来。
2. **`vt` 或 `un` 不是 1，或者 `vs=` 读回 `0x736559`（"Yes"）/`0xffff`（没有这个属性）** —— 这台板子的 IOKit 枚举和宿主
   看到的不是一回事，`vn=` 前两字会直接告诉我们是哪颗；那时先修枚举，别急着改挂载参数。
3. **`mt=0`** —— 那就跳过了这一格，直接进 `rootvp not authenticated after mounting` 那颗钉子（候选 3）。

## round132 读数（v0.3.5 实机，2026-10-26）

一帧，逐字对上了宿主的预测：

```
f=15797536 n=11870275 m=1 l=0 d=10 s=1 c=1 v=1
vt=1 un=1 vs=0x6f4e vo=0x696c6e4f vu=0x1d874fd vc=0xa7dd292a
vn=0xbe776144/0x61647055/0x31326574/0x363448
dv=1:3 bd=0 al=0 t=0x73667061/0x0/0x0/0x0 on=0x2f fm=0x6b736964
dp=0xffffffff:0x107bd5b0 fl=0x4005001 fc=0x4001001 fk=0x0
op=0xfffffff0:0x5f2b3c8 mm=0xfffffff0:0x56c5fac mt=13
st=65535 rt=65535 k=65535/65535/65535/65535 rd=65535 g=0 x=0x0 y=0x0 r=464 w=0
@CloverBlock.cpp:1369
```

- **新七栏全部命中**：`vt=1`（实机只扫到一颗卷）、`un=1`（它就是一颗 `Sealed=No`）、`vs=0x6f4e`（"No"）、
  `vo=0x696c6e4f`（"Online"）、`vu=0x1d874fd`（宿主的 `0x01d874fd`，前导零是打印/照片的事）、
  `vc=0xa7dd292a`（容器 UUID）、`vn=` 四字的期望是 `0x6e776144/0x61647055/0x31326574/0x31363448`
  （`Dawn`/`Upda`/`te21`/`H461`），照片里首字读成 `0xbe776144`、末字读成 `0x363448` 少一位 —— 都是拍照糊掉的位，
  不是字段的差别（`0x6e776144` 与 `0xbe776144` 只差最高字节，`w`/`a`/`n` 三字节完全一致）。
  **⇒ 这台板子的 IOKit 枚举和宿主 `diskutil apfs list` 看到的是同一颗卷：`DawnUpdate21H461`，没密封、在线。**
- **旧栏一个没动**：`dv=1:3`、`bd=0`、`al=0`、`t=` 拼出 "apfs"、`on=0x2f`、`fl=0x4005001 → fc=0x4001001`
  （`MNT_ROOTFS` 0x4000 被清掉了，且只清了它）、`mm=0xfffffff0056c5fac`、**`mt=13`、`r=464`（连续第四轮一字不差）**。
- **帧尾 `@CloverBlock.cpp:1369` 命中** —— `panic()` 那串格式符跨到 1369 行的预测成立，行的映射可信。

**这一格关闭了。** 挂错卷这个假设到此为止：我们挂的就是那颗 `Sealed=No` 的 OS 卷，身份和宿主一致，
`VFS_MOUNT` 还是回 13。`r=464` 不变还多tell一件事：**拒绝发生在读任何媒体块之前**（464 块全是自己扫描时读的），
13 不是「容器超级块读不出来」。

## round132 读数之后：13 追到了一句 `vnode_open`

### 新仪器：apfs 的每一处外呼现在都有名字

apfs 调内核走的是 12 字节桩：`adrp x16, #page; ldr x16, [x16, #off]; br x16`。关键是**那些槽位（段
`__PLK_DATA_CONST`）在文件里就烤着内核 vmaddr**，而 `round132-kernel.macho` 带着 276,972 条 nlist。
`/tmp/namestubs.py` 于是能把每条桩解析成符号（`/tmp/stubs.json`，**2813 条**），`/tmp/dfun2.py` 在反汇编里
同时内联符号和 `adrp/add` 指向的 C 串。这条路上一口气有了名字的是：`current_task`、`copyin`、**`bdevvp`**、
`vnode_specrdev`、`vnode_open`、`vnode_mount`、`vfs_context_current`、`vfs_context_kernel`、`vfs_context_suser`、
`proc_selfpid`、`proc_selfname`、`OSMetaClassBase::metaCast`/`safeMetaCast`、`snprintf`、`strlcpy`、`strncpy_chk`、
`VNOP_IOCTL`、`lck_mtx_unlock`、`IOBSDNameMatching`、`IOService::copyMatchingService`。

两条踩坑值得记：`LC_SEGMENT_64` 是 `<4Q4I>`，不是 `<4Q6I>`；`ldr` 的位移要信 capstone，别自己抠 imm10 ——
拿槽 `0x5f25440` 的值验的，它正好等于 `_bdevvp` 的地址。

### `mt=` 就是 `container_rootmount` 的 errno 出参

`apfs_vfsop_mount` 的 mode 0 那条臂 `bl 0x567fa0c` 之后 `str w0,[sp,#0xec]; mov x26,x0` ⇒ 被调者的非零返回值**原样**
变成挂载的返回值。`container_rootmount`（`0x567fa0c..0x5680344`，帧 0x160）的 `stp xzr,xzr,[sp,#0x50]` 就是那颗 errno 槽，
而它的出口有两条 `ldr w19,[sp,#0x50]`（`0x567fcdc`、`0x5680214`）—— **函数返回的数就是槽里的数**。
整段里没有 `#0xd`；往槽里写硬编码常量的站点只有 79、16、2、12、15。能往槽里写「别人的返回值」的只有四处：
三处 `bdevvp`（`0x567fc3c`、`0x567fea4`、`0x56800b4`，每处后面都跟着 `str w0,[sp,#0x50]`）和一处
`container_load_with_callback`（`0x5670890`，`x4 = sp+0x50`）。

### `bd=0` 当场把 `bdevvp` 排除掉了

round129 那一档自己就在驱动里调 `bdevvp((major<<24)|minor, &devvp)`，画的就是 `bd=`。这帧 `dv=1:3 bd=0` ⇒
内核给我们这颗卷的 dev_t 造得出 vnode、也开得起（`bdevvp` 内部是 `VNOP_OPEN(vp, FREAD)`；源码里 NODEV 之外唯一的
静默非零返回来自 `vnode_create_internal`，而它的区间 `0x7472018..0x7472a00` 里也没有 `#0xd`）。

顺手把 dev 的拼法和真机对齐了：`ls -l /dev/disk0s3` = `0x1000003`、`/dev/disk1s1` = `0x1000006`
（major 恒为 1，minor 是全局分片计数），而 apfs 在 `0x567fc34` 拼的是 `orr w0, w0, w25, lsl #24`
（w25 取自 `BSD Major`、低字取自 `BSD Minor`）—— **就是 XNU 的 `major<<24|minor`，我们报的和它算的是同一个 dev_t，
没有串位。**

### 剩下那处：加载器里有一句按路径的 `vnode_open`，它的返回值直接进 errno 槽

`container_load_with_callback`（`0x5670890`，帧 0x4d0）自己也没有 `#0xd`，但它把入参 `x4` 存成 `[sp,#0x58]` 当 errno
出参用，并且在「手上没有现成 vnode」（`cbz x24, 0x5670bb0`）这条路上：

```
0x5670bb0  cmp   w19, #0
0x5670bb4  mov   w8, #0x13
0x5670bb8  csinc w1, w8, wzr, ne      ; w19!=0 → 0x13 = FREAD|FWRITE；w19==0 → 1 = FREAD
0x5670bc0  mov   x0, x27              ; = 入参 x0，那颗名字缓冲
0x5670bc4  mov   w2, #0
0x5670bc8  mov   w3, #0
0x5670bbc  add   x4, sp, #0xb0
0x5670bcc  ldr   x5, [sp, #0x50]      ; 上下文：0x56708e0 bl vfs_context_current
0x5670bd0  bl    vnode_open           ; 桩 0x5769394
0x5670bd4  ldr   x8, [sp, #0x58]
0x5670bd8  str   w0, [x8]             ; *** 返回值原样写进那颗 errno 出参 ***
0x5670bdc  cbnz  w0, 0x567135c
```

XNU 导出的 `vnode_open` 是**按路径**开文件的（`bsd/sys/vnode.h:2143`：
`vnode_open(const char *path, int fmode, int cmode, int flags, vnode_t *vpp, vfs_context_t ctx)`），六个槽位与这里一一对上；
而 `container_rootmount` 的另一半正好用 `snprintf("/dev/", …)` 造过同类名字 ⇒ apfs 是在对一颗 `/dev/` 路径
做一次按路径的打开，并把它的 errno 交回 `container_rootmount` 的返回值，也就是我们画了四轮的那个数。
（`w19` 的来源我没追到底；若它是 0，这次就是 FREAD-only、轮不到写授权 —— 这恰好就是 `vw=`/`vr=` 要一刀切开的东西。）

> **round133 宿主更正（2026-10-26）**：上一版在这里写的是「`/dev/<那颗媒体的 BSD Name>`」，那句是错的，
> 而且错在决定探针开哪颗节点的地方。`container_rootmount+0x78`（`0x567fa84`）调的 helper `0x56eb540`
> 先 `PE_parse_boot_argn("rd", buf, 0x20)`，再用 `cmp #0x6d` / `cmp #0x64` / `ccmp w9,#0`（`0x56eb5a4-0x56eb5c0`）
> 判 `buf[0]=='m' && buf[1]=='d' && buf[3]==0`；成立就 `snprintf("/dev/%s", buf)` 并返回 1，
> `cbz w0,0x567fb0c` **不跳**，`0x567faac` 直接拿这颗串去调 `container_load_with_callback`。
> 本板 boot-arg 是 **`rd=md0`**（三位，`md0s39` 只是媒体的 `BSD Name` 属性，是 step 9 为 `bsd_init` 的
> RAMDEV 前缀测试写的别名），谓词成立 ⇒ **apfs 实际打开的是 `/dev/md0`，即 RAMDisk，而不是 eMMC 卷的节点。**
> 走匹配器 `0x5671c78` 的那条媒体臂（`0x567fb0c`）在本板根本不会进。这一更正同时解释了
> round129–132 为什么连着四轮改「卷的挂载状态」都纹丝不动：那个调用方压根没在开那颗节点。
> `w19` 也已定位：`0x56708f4-0x5670900` 对入参 `x2` 指向的字节做 `tst #1` 后 `cset w19,eq`，
> 即 **bit0 为 0 ⇒ w19=1 ⇒ fmode=0x13（带写）**。

### 13 从哪来：`vnode_authorize` 把 EPERM 翻成 EACCES

`vn_open` → `vn_authorize_open_existing`（`bsd/vfs/vfs_subr.c:8160`）：`fmode & (FWRITE | O_TRUNC)` 时请求
`KAUTH_VNODE_WRITE_DATA`（:8228-8240），再 `vnode_authorize`（:8753）：

```c
	result = kauth_authorize_action(vnode_scope, vfs_context_ucred(ctx), action, ...);
	if (result == EPERM) {          /* traditional behaviour */
		result = EACCES;
	}
```

全内核 `__TEXT_EXEC` 的 `mov wN,#0xd` 普查（`/tmp/eacces2.py`，336 处）里，`vnode_authorize+0x74` 那一处对上的就是
这句 EPERM→EACCES（:8768-8770）。而 `bdevvp` 走的是 `VNOP_OPEN`，**根本不经过 kauth** —— 这就是 `bd=0` 和 `mt=13`
能同时成立的机制：同一颗设备，绕过授权开得了，带着写按路径开就被拒。

### 同时排除掉的两处诱人文案

- `container_load_with_callback` 里那句 `"refusing exclusive access to pid <%d (%s)> as other clients <%d> are
  actively accessing the container device = %s"`（`0x5670c38`）之后写的是 `mov w20, #0x10` ⇒ **EBUSY(16)**，不是 13。
  同一颗函数里另一处 `"container already loaded by pid <%d> in exclusive mode…"` 同理。**「我们已经把 devvp 开着，
  占住了设备」这条路排除了**（真要说，它许的是 16）。
- **round131/132 心里挂着的「Image4 根认证那道门」正式作废。** 那道门在 `0x56c6a7c` 的第二次分发之后，而那次分发
  只放 **mode 3 与 mode 8** 过去（mode 0/1/2/4..7 一律从 `0x56c6ab4` 离开），`validate_payload_and_manifest`
  （`0x56809a0`）和它前面两处 `copyin`（`0x56c6bdc`、`0x56c6bf0`）都在门后 ⇒ **mode 0 到不了**。
  `handle_crypto_mount` 那句 `#0xd` 也一样，只在 mode 4..7 那条臂上。

## v0.3.6（round133）：在驱动里复现 apfs 那句 `vnode_open`，开的必须是它真正开的那颗节点

这一格只做一件事：把 `container_load_with_callback` 在 `0x5670bd0` 那句 `vnode_open` 在我们自己的线程上
照原样跑两遍（`FREAD` 一次、`FREAD|FWRITE` 一次），把两个 errno 画出来。`VFS_MOUNT` 调用本身第五轮保持
逐字节不变，`mt=`/`r=` 应当仍是 `13`/`464`，这样新读数和旧读数可以放在同一帧里对照。

**但开哪颗节点，是这一轮在宿主上改掉的关键。** 上一节末尾的更正已经写了推导，这里只给结论：
`rd=md0` 让 `0x56eb540` 的 `md?` 谓词成立，`container_rootmount` 走的是「直接用 `rd=` 拼 `/dev/%s`」那条臂，
所以 apfs 开的是 **`/dev/md0`（RAMDisk）**，不是 eMMC 卷的 `/dev/md0s39`。round132 及之前记的「媒体臂」作废。
驱动不再替它猜：自己 `PE_parse_boot_argn("rd", buf, 0x20)`，套同一个三字节谓词，成立就用 `rd=` 的值、
不成立才退回媒体的 `BSD Name`，再拼 `/dev/`。因此 `nd=` 既是路径、也是「走了哪条臂」的自证。

新增的六个字段：`rdb=`（`PE_parse_boot_argn` 的返回）、`rda=`（`rd=` 值的前 16 字节，四个 `le32`）、
`nd=`（真正被打开的路径前 16 字节）、`vr=`/`vw=`（两次 `vnode_open` 的返回）、
`cr=`（`kauth_cred_getuid(vfs_context_ucred(vfs_context_current()))`）。上下文用 `vfs_context_current()`，
因为 `container_load_with_callback` 序言里存的正是它（`0x56708e0`），不是 `VFS_MOUNT` 传下来的那个；
两次打开的节点都在 `VFS_MOUNT` 之前用 `vnode_close` 关掉了，不给 apfs 留下「设备被我们占着」的借口。

预期帧（末行 `@CloverBlock.cpp:1508`）里新段的形状：

```
rdb=1 rda=0x30646d/0x0/0x0/0x0 nd=0x7665642f/0x30646d2f/0x0/0x0 vr=<a> vw=<b> cr=0
```

`rda=` 解出来是 `md0`，`nd=` 解出来是 `/dev/md0`。若 `rda=` 拼出的不是 `md0`，说明 boot-arg 没按预期传下去，
后面所有关于路径的推导都要先停下来重看。

读法（`vr`/`vw` 是 errno，2=ENOENT、6=ENXIO、1=EPERM、16=EBUSY、13=EACCES）：

- **`vr=0 vw=13`** —— 最想要的一帧：这颗节点只读开得了、带写被拒。apfs 的 `w19` 在其标志字节 bit0 为 0 时
  取 1，对应 fmode `0x13`，正好落在被拒的那一侧 ⇒ `mt=13` 的生产者当场抓获，而且问题被改写成
  「`rd=` 把 apfs 指到了 RAMDisk 上」，与卷的密封、快照、`MNT_ROOTFS` 全都无关。
- **`vr=13 vw=13`** —— 连只读都被拒，那是 kauth/MACF 一层的拒绝（`vn_authorize_open_existing` 在
  `VNOP_OPEN` 之前，`CONFIG_MACF` 下 `mac_vnode_check_open` 也能拒读）。`cr=` 非 0 会先解释它。
- **`vr=2`** —— `/dev` 里没有这颗节点。v0.2.36 起 EL0 的 `open("/dev/console")` 是成功的，devfs 确实挂着，
  所以 2 只说明「这颗设备没被 devfs 造成节点」。若此时 `mt=` 仍是 13，则本格的路径推导与实测矛盾，
  先查推导，不要急着改 `rd=`。
- **`vr=vw=0`** —— 同一个调用在我们线程上两次都过，13 就是 apfs 自己决定的（它的 `w19`/mode/另一处写槽），
  下一格回到 `container_rootmount` 内部，而不是内核授权。
- **`vw=1` / `vw=16`** —— 分别指向 `spec_open` 的 `securelevel>=2 && FWRITE && D_DISK` 那一条
  （`bsd/miscfs/specfs/spec_vnops.c:436-457`）和 `vfs_mountedon` 的 `SI_MOUNTEDON`
  （`bsd/vfs/vfs_subr.c:4125`）。后者还可能睡在 `vnode_waitformounting` 上：那时这一格不出新行，
  由 tick 看门狗报点，同样是有效读数。

归档在 `~/clover-out/round133-*`：`payload.img` 58,720,256 B（`bc6a0f7f…`）、本地 `kernel.macho`
`cf0ecb40…`、卷内 `kernel.macho` 51,994,624 B（`8d0951be…`）、ADT 13,268 B（`019e04b5…`）、
trustcache 56 B（`c3ed2152…`），另附 `round133-SHA256SUMS`。`check-clover.py` 与
`verify-clover-payload.py` 全绿（probe patch 6678 B / 35 runs，CloverBlock 的 r133 身份串、
`rdb=/rda=`、`vr=/vw=/cr=`、`/dev/` 与 `_vnode_open`、`_vnode_close`、`_kauth_cred_getuid`、
`_vfs_context_current`、`_PE_parse_boot_argn` 均在）。

**刷写范围只有 `cache` 一个分区，没有新的 eMMC 分区写入**：`fastboot flash cache
/Users/zhouzhou/clover-out/round133-payload.img`，然后 `fastboot reboot` 冷启动。

下一格候选（要等这一帧，排序按当前证据）：

1. **若 `vw=13`：把 `rd=` 不再指向 RAMDisk 这件事拆开做** —— 它是 round121 为绕 `rootvp not authenticated`
   而撒的谎，现在证明这句谎同时被 apfs 的 `container_rootmount` 当真了。要先决定的是「`rd=` 写真名之后
   用什么代替 RAMDEV 前缀来跳过 rootauth」，而不是直接改字符串。
2. **ADT 加 `/chosen { root-snapshot-name = …; }`** —— 只有在 `mt=` 过了 13 之后才轮得到快照这一层。
3. **`/filesystems { DoNotUseSEP; }` 与 `allow-root-hash-mismatch`** —— 同属认证类，同样往后排。

## round133 读数（v0.3.6 实机，2026-10-26）：两次 open 都是 ENOENT —— 这一格问的根本不是本次挂载会走的路

先把仪器自己的毛病交代掉：这一帧的**末段标签比参数表快四格**。格式串在 `k=…` 之后排的是
`rd=%u g=%u x=0x%x y=0x%x r=%u w=%u rdb=%u …`，而参数表在这个位置给的是
`read, bytes, word0, word1`（阶梯那四格，占了 `rd=/g=/x=/y=` 的标签），新探针的
`rdFound, rdArg[0..3], node[0..3], openRead, openWrite, uid` 与两个块计数器只能从 `r=` 那格开始顺延。
所以屏上那几行**不能按标签读**，必须按参数序还原。屏上打出的是：

```
r=1 w=3171437 rdb=0
rda=0x0/0x0/0x7665642f/0x30646d2f
nd=0x0/0x0/0x2/0x2 vr=0 vw=464 cr=0
```

按参数序无损还原之后的真实字段：

| 字段 | 值 | 解出来 |
| --- | --- | --- |
| `rdFound` | 1 | `PE_parse_boot_argn("rd", …)` 命中 |
| `rdArg[0..3]` | 0x0030646d / 0 / 0 / 0 | `"md0"` |
| `node[0..3]` | 0x7665642f / 0x30646d2f / 0 / 0 | `"/dev/md0"` |
| `openRead` | **2 = ENOENT** | FREAD 开不到 |
| `openWrite` | **2 = ENOENT** | FREAD\|FWRITE 同样开不到 |
| `uid` | 0 | root |
| `gBlocksRead` | 464 | 屏上错显成 `vw=464` |
| `gBlocksWritten` | 0 | |

而 `mt=13 r=464` 与 round129–132 一样纹丝不动。这一格读法里写「`vr=2` ⇒ `/dev` 里没有这颗节点；
若此时 `mt=` 仍是 13，则本格的路径推导与实测矛盾，先查推导，不要急着改 `rd=`」—— 现在正是这一条。

**于是 round132 末尾那条「13 追到了一句 `vnode_open`」的推导正式撤回，两处都不成立：**

1. **那句 `vnode_open` 本次根本不可达。** `container_load_with_callback` 里按路径 open 的那一臂，
   前提是调用方没给它 devvp；我们走的是 `VFS_MOUNT(mp, devvp, 0, ctx)`，`bd=0` 意味着 `bdevvp()`
   已经交出一颗真 vnode，加载器拿着它就走设备臂，不会再按 `/dev/%s` 去 open。所以 round133 复现的
   那次 open 是**本次挂载不会执行的路径**，它的 `ENOENT` 与 `mt=13` 没有因果关系 ——
   也说明 `vr=2` 不是「devfs 没造节点」，是根本没人需要这颗节点。
   ⚠️ 顺带把上一节末尾候选 ①（「若 `vw=13` 就处理 `rd=` 那句谎」）一并作废：`vw` 真实值是 2，
   不是 13，`rd=md0` 不需要为这 13 负责，`kernel_args` 保持原样。
2. **「errno 槽只有一个外来返回值写手」也撤回。** 上一轮只查了 `bdevvp` 与
   `container_load_with_callback`；这次把 `container_rootmount` 到 `nx_mount` 整条链上写 `[sp, #0x58]`
   （保存下来的 errno 出参）的点全部列出来，`nx_mount` 返回之后就是其中之一。

`bdevvp` 那条反证仍然有效，而且比之前更关键：`bsd/vfs/vfs_subr.c` 的 `bdevvp()` 自己就做了
`VNOP_OPEN(nvp, FREAD, &context)`，并且失败是 panic 而不是静默 13 —— 所以「设备侧压根没打开」不成立，
但**打开之后**的读取是另一回事。

## round133 读数之后：errno 槽的另一个写手是 `nx_mount` 自己，13 的新候选是设备读路径

宿主反汇编（`/tmp/cst/bin/python` + `/tmp/kdisasm.py`，`__PLK_TEXT_EXEC` vmaddr `0xFFFFFFF004920000`）
落在这一处：

```
0xfffffff005670e74  bl  #0xfffffff005738928   ; nx_mount
0xfffffff005670e78  mov x28, x0
0xfffffff005670e7c  ldr x8, [sp, #0x58]
0xfffffff005670e80  str w0, [x8]
```

`x8` 就是 `container_rootmount` 那个 errno 出参槽 —— **`nx_mount` 的返回值原样变成 `mt=`**。
于是问题从「谁在授权层拒绝」变成「`nx_mount` 为什么要 13」，而它进门第一件事就是读块 0：

```
nx_mount
  -> 默认设备操作表 +0x18
  -> _vnode_dev_read
  -> buf_getblk / VNOP_STRATEGY
  -> spec_strategy
  -> dkstrategy / dkreadwrite
  -> IOMedia::read
  -> provider / IOBlockStorageDriver
  -> CloverBlockDevice::doAsyncReadWrite
```

这条链上每一环都不会自己发明权限错误，但有一处会把 IOKit 的返回码翻成 13。
`spec_strategy`（`bsd/miscfs/specfs/spec_vnops.c`）没有权限门；`buf_biowait` 只回 buffer 里已有的
`b_error`；而 `iokit/Kernel/IOService.cpp` 的 `errnoFromReturn` 里：

```cpp
case kIOReturnNotPermitted:   return EPERM;
case kIOReturnNotPrivileged:  return EACCES;
case kIOReturnExclusiveAccess:
case kIOReturnLockedRead:
case kIOReturnLockedWrite:
case kIOReturnNotOpen:
case kIOReturnNotReadable:    return EACCES;
```

`IOMediaBSDClient` 的 completion 正是用它把 IOReturn 写成 errno 再 `buf_seterror`，
`IOMedia::read()` 里还有一条**根本不下到 provider** 的早退：

```cpp
if (_openLevel == kIOStorageAccessNone) { complete(completion, kIOReturnNotOpen); return; }
```

`NotOpen → EACCES(13) → dkreadwritecompletion → buf_biowait → _vnode_dev_read → nx_mount → mt=13`，
这是目前唯一一条**既能产出 13、又能解释 `r=464` 四轮不涨**的链：`CloverBlockDevice::doAsyncReadWrite`
只有真读了块才加 `gBlocksRead`，如果 `IOMedia` 在到达它之前就早退，块计数当然一动不动。

同时把不成立的候选钉死：`0x56c69c8` 那句 `#0xd` 属于 `handle_crypto_mount` 的 mode≥4 臂，
mode 0 的跳转表进不去；`nx_dev_init` 相关的直接错误值只有 12 与 22；`0x566b968` 只回 0 或 12。

⚠️ 这条链**目前只是候选**，不是结论：`bdevvp` 已经 `VNOP_OPEN(FREAD)` 过，
所以「`_openLevel` 为 None」需要有别的解释（比如它开的是媒体客户端的另一种 level，或者 apfs 在
`nx_mount` 里用了另一个上下文）。这一点必须由实机读数决定，不能靠推导收工。

## v0.3.7（round134）：撤掉路径仪器，挂载失败后用同一个 devvp 读 4 KiB

**这一轮只问一个问题：`mt=` 失败之后，那颗已经交到 apfs 手上的 devvp 还能不能读出块。**

做法上刻意保持「不改变被测量」：新增的读**只在 `VFS_MOUNT` 返回失败之后**执行，用的就是刚才传给
`VFS_MOUNT` 的那一颗 `devvp`、同一次 `ctx` 的 `vfs_context_ucred(ctx)`，读 4 KiB 到
`IOBufferMemoryDescriptor`。挂载调用本身一字未改（`flags=0`、同一个 devvp、同一个 vfsops）。

```cpp
report->mount = static_cast<uint32_t>(VFS_MOUNT(mp, devvp, 0, ctx));
if (report->mount) {
  OSSharedPtr<IOBufferMemoryDescriptor> device =
      IOBufferMemoryDescriptor::withCapacity(kDeviceReadBytes, kIODirectionIn, true);
  if (!device.get()) { fail("device-read-buffer", 0); }
  int deviceResid = 0;
  report->deviceRead = static_cast<uint32_t>(
      vn_rdwr(kUIORead, devvp, device->getBytesNoCopy(),
              static_cast<int>(kDeviceReadBytes), 0,
              kUIOSysSpace, kIOUnit, vfs_context_ucred(ctx), &deviceResid, nullptr));
  ...
}
```

删掉的是 round133 那套已证伪的仪器：`PE_parse_boot_argn("rd", …)` 解析、`/dev/` 拼路径、
两次 `vnode_open`/`vnode_close`、`kauth_cred_getuid`，以及随之不再需要的
`vfs_context_current`/`vnode_open`/`vnode_close`/`kauth_cred_getuid` 声明与 `kFRead`/`kFWrite` 常量
（`PE_parse_boot_argn` 保留 —— `clover-apfs` 这些真实功能还在用它）。
**顺手修掉本轮开头那张帧暴露的错位**：`r=`/`w=` 两个块计数器现在排在新增字段之后，
参数表与格式串一一对应，`dr=` 之后每格都能直接读。

新字段：`dr=`（`vn_rdwr` 的返回，`65535` = 没走到这一步，即挂载成功或更早的档没到）、
`db=`（实际读到的字节数，`4096 - resid`，夹在 0..4096）、
`dx=`/`dy=`（读出内容的前两个 `le32`）。末行当时写的是 **`@CloverBlock.cpp:1403`**，实机读回来是
**`@CloverBlock.cpp:1425`** —— 差的那 22 行不是内容变了，是我把行号规则猜错了：`panic()` 是跨行调用，
屏上打的是**这个调用的收尾 `);` 那一行**，不是 `panic(` 起手的行（1403 是 `panic(`，1425 正是 `);`）。
以后预告末行号一律按「`);` 的行」算。

`dx=`/`dy=` **本轮不预设常量**：`dv=1:3` 是那颗 `AppleAPFSVolume` 的媒体，它的块 0 是卷的超级块，
而容器的 `NXSB` magic 在偏移 `0x20` 而不是 0（`scripts/make-clover-apfs-container.py:45` 钉过这件事），
所以前 8 个字节是 ablock 头一类的对象号/事务号，具体值取决于卷的状态。这两个字段的用途只有一个：
**分辨「真读到了内容」与「拿到一块全零缓冲」** —— 任何非零都算读通了，`dr=0` 配全零才可疑。

读法（`dr=` 是 errno）：

- **`dr=13`** —— 同一个设备 vnode 在挂载之后仍然被持续拒绝：`mt=13` 是**设备侧**的，与授权层、
  密封、快照无关。`db=0` 且 `dx=dy=0`（内容全空）＋ `r=` 不涨 ⇒ 支持「`IOMedia` 在到达
  `CloverBlockDevice` 之前就早退」；`db=4096` 却 `dr=13` 则是另一回事（读成功但被改写）。
- **`dr=0`（`db=4096`，`dx`/`dy` 非零）** —— 这条链当场断掉：设备读得通，
  `13` 不是从块读取路径来的，得回到 `nx_mount` 内部或容器复用（`nx_mount` 会先找已存在的容器，
  那里另有写槽）。`r=` 这一轮才可能第一次真正涨。
- **`dr=6`（ENXIO）/ `dr=16`（EBUSY）/ `dr=2`（ENOENT）** —— 分别指向设备节点已失效、
  媒体被别的客户端占着（`kIOReturnExclusiveAccess` 也映射 13，但 `dkreadwrite` 自己会先回 16）、
  vnode 已回收。任何一种都比 13 好读，因为它们是明确的对象状态而不是权限。
- **`dr=5`（EIO）** —— 卡/驱动真的答不上来，此时 `r=` 应当有变化，`CloverSdhci` 那侧接手。
- 注意 `r=` 是在新读**之后**采样的，所以它包含这次 4 KiB 造成的实际媒体 I/O；
  但**`r=` 不涨不能反过来证明没有读** —— buffer cache 可能命中（块 0 之前已被 `bdevvp`/挂载路径读过）。
  本轮不预读，就是为了不把这笔账搅浑。

**预算**：一次 4 KiB 读最多一块；`rd=md0`、`clover-apfs=2`、`clover-emmc=21`、
`armcpu-clock-panic=180000` 全部原样。如果这轮死在定时器上而不是死在这行上，`elr=` 是那笔慢读，不是 bug。

构建命令与上一轮相同（两个驱动都在 `[kernel] archives` 里，`kernel` 那格照旧不能省），本轮 `userdata`
不重建 —— 镜像一个字节都没变：

```
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-root/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

产物归档在 `~/clover-out/round134-*`，`shasum -a 256 -c round134-SHA256SUMS` 七项全部 OK：
`payload.img` 58,720,256 B（`d84480e6…`）、本地 `kernel.macho` `f60360af…`、卷内 `kernel.macho`
51,994,624 B（`8ddbd3e0…`）、卷内 `unixnu.toml` 606 B（`6de8b37d…`）、ADT 13,268 B（`e1c29f6a…`）、
`scratch-data.img` 35,888 B（`614b8356…`）、trustcache 56 B（`c3ed2152…`）。两个 checker 全绿：
probe patch  localize 成 6680 B / 35 runs，CloverBlock 的 `r134` 身份串、
`dr=%u db=%u dx=0x%x dy=0x%x r=%u w=%u`、`_vn_rdwr`、`_vfs_context_kernel` 全部在位，
旧的 `rdb=`/`vr=`/`_vnode_open` 已从 checker 里删掉（防止仪器被静默换掉）。

**刷写范围只有 `cache` 一个分区，没有新的 eMMC 分区写入**：`fastboot flash cache
/Users/zhouzhou/clover-out/round134-payload.img`，然后 `fastboot reboot` 冷启动。

下一格候选（等这一帧，排序按当前证据）：

1. **`dr=13` ⇒ 把 `_openLevel` 量出来**：在驱动里对这颗媒体的 `IOMedia`/客户端对象问出它的
   open level 与 owner，决定是「我们没开够」还是「apfs 用了另一个客户端」。
2. **`dr=0` ⇒ 回到 `nx_mount` 内部**：它先查已存在的容器（`nx_mount` 的复用臂），那里的写槽是新的嫌疑；
   此时才轮到 mode 1 对照（fixture 的普通 APFS 数据挂载用 mode 1 是成功的）。
3. **ADT 加 `/chosen { root-snapshot-name = …; }`** 与
   **`/filesystems { DoNotUseSEP; }` / `allow-root-hash-mismatch`** —— 依旧只在 `mt=` 过了之后才排队。

## round134 读数（v0.3.7 实机，2026-10-26）：那颗 devvp 独立读也回 13，一格字节都没动

屏上末帧原文（`@CloverBlock.cpp:1425`）：

```
CloverBlock r134: apfs f=15797536 n=11870275 m=1 l=0 d=10 s=1 c=1 v=1 vt=1 un=1 vs=0x6f4e vo=0x696c6e4f
vu=0x1d874fd vc=0xa7dd292a vn=0x6e776144/0x61647055/0x31326574/0x363448 dv=1:3 bd=0 al=0
t=0x73667061/0x0/0x0/0x0 on=0x2f fm=0x6b736964 dp=0xffffffff2:0x352524c0 fl=0x4005001 fc=0x4001001 fk=0x0
op=0xfffffff0:0x5f2b3c8 mm=0xfffffff0:0x56c65fac mt=13 st=65535 rt=65535 k=65535/65535/65535/65535
rd=65535 g=0 x=0x0 y=0x0 dr=13 db=0 dx=0x0 dy=0x0 r=464 w=0
```

本轮的新格只有四格，其余全部与 round132 逐格相同（身份、选卷、`dv=1:3`、`bd=0`、`mt=13`）：

| 字段 | 读数 | 含义 |
|---|---|---|
| `dr=` | 13 | 挂载失败之后，对**同一个 devvp** 做一次独立 4 KiB `vn_rdwr`，errno 还是 13 |
| `db=` | 0 | 一个字节都没搬（`resid` 等于请求长度） |
| `dx=` / `dy=` | 0x0 / 0x0 | 缓冲没被填 |
| `r=` / `w=` | 464 / 0 | 与 round132 完全相同，**这次读没有让块计数器涨一格** |

`t=0x73667061` 是 "apfs"、`fm=0x6b736964` 是 "disk"、`vn=` 拼起来是 "DawnUpdate12H46…"、
`vs=0x6f4e` 是 "No" —— 这些都还是 round132 那一颗未封存卷，六轮没换过目标。

**这一格确认的事**：13 不是挂载调用的参数状态造出来的。清掉 `MNT_ROOTFS`（round131）、挑未封存卷
（round132）、去掉路径 open 仪器（round133）都动不了它；现在连「根本不调 apfs，只对这个 vnode 做一次
普通读」也是同一个 13。而且它发生在**我们的块驱动之上**：`r=` 不涨说明请求没落到
`CloverBlockDevice` 手里，而 apfs 在拒绝挂载**之前**明明已经用同一颗驱动读过 464 块（计数器是在
`measureApfsDepth` 前才清零的），所以「这台设备的读通路是坏的」不成立 —— 坏的只有这条 vnode 读。

**这一格没确认的事，以及要撤回的一句**：落盘时我写过「`mt=13` 与 `dr=13` 是同一种失败，因此拒绝发生在
`CloverBlockDevice` 之上」。前半句成立，后半句是我又往前走了一步 —— 因为 `dr=` 是 `vn_rdwr` 的返回值，
而 `vn_rdwr` 在做真正的读之前有一道授权门，这道门和设备读都会打出 13。宿主四条证据：

1. `vn_rdwr_64()`（`xnu-0b4b910d/bsd/vfs/vfs_vnops.c:995-998`）：`if ((ioflg & IO_NOAUTH) == 0)` 就先
   `error = mac_vnode_check_read(&context, cred, vp)`，只有它返回 0 才轮到 `VNOP_READ`。`IO_NOAUTH`
   是 `0x8000`（`bsd/sys/vnode.h:174`），而我们传的 `kIOUnit` 是 `IO_UNIT = 0x0001`（`:154`）——
   **授权门是开着的**。
2. `nm ~/clover-out/round134-kernel.macho` 里有 `_mac_vnode_check_read`，`CONFIG_MACF` 是 1，
   那道门在我们这颗内核里确实编译进去了。
3. `bdevvp()`（`bsd/vfs/vfs_subr.c:2106-2170`）没有 alias 短路：`vnode_create` → `VNOP_FSYNC` →
   `buf_invalidateblks` → `VNOP_OPEN(nvp, FREAD)` 是无条件顺序，且 open 失败直接 `panic("bdevvp failed:
   open")`。所以 `bd=0` 且没 panic ⇒ `spec_open` 走到 `dkopen(dev, FREAD, S_IFBLK)` 并返回 0；
   `dkopen` 里唯一的 EACCES 需要 `flags & FWRITE` 且 `media->isWritable()==false`，FREAD 开不到那一臂。
4. `dkreadwrite()`（`IOStorageFamily-ad2e13e0/IOMediaBSDClient.cpp:2900-3126`）的全部早退是：缺 minor 或
   orphaned → `ENXIO`；`isFormatted()==false` → `kIOReturnUnformattedMedia`（映射也是 ENXIO）；
   `byteStart >= mediaSize` 的读 → **Success 且不搬数据**；raw 未对齐 → `NotAligned`(EINVAL)；
   无缓冲 → `NoMemory`(ENOMEM)；`prepare()` 失败 → `VMError`(EFAULT)。最后统一
   `return minor->media->errnoFromReturn(status)`。**它自己写不出 13。**

把 4 和 `errnoFromReturn`（`xnu-0b4b910d/iokit/Kernel/IOService.cpp:8160-8232`）合起来看，13 只可能来自
两处：`media->read()` 的返回，或 `VNOP_READ` 之上那层授权。而 `IOMedia::read()` 的早退清单里
（`IOMedia.cpp` 的异步 `read`，逐条读过）只有 `_openLevel == kIOStorageAccessNone → kIOReturnNotOpen
(0xe00002cd)` 会映射成 13，其余 `isInactive → NoMedia`、`_mediaSize==0 || _preferredBlockSize==0 →
UnformattedMedia`、四个几何/方向检查 → `BadArgument` 分别映射 ENXIO/ENXIO/EINVAL。
`IOMedia::read()` **完全不看调用者的 `client` 参数**，也不验证调用者开没开过。
`IOMedia::handleIsOpen()`（`IOMedia.cpp:486-505`）查的是它自己的 `_openClients` 字典，
所以 `isOpen()` 是同一个问题的第二种问法，不需要任何结构体偏移。

## v0.3.8（round135）：把这 13 的层切开 —— 去掉授权门再读一次，然后直接读卷的 IOMedia

一句话的问题：**这个 13 是哪一层写的**。挂载调用依旧逐字节不变，本轮所有测量都在 `VFS_MOUNT`
返回之后，全程只读、不改任何状态、不开也不关任何媒体。

```cpp
  report->mount = static_cast<uint32_t>(VFS_MOUNT(mp, devvp, 0, ctx));
  if (report->mount) {
    ... round134 那次 vn_rdwr(kUIORead, devvp, ..., kIOUnit, ...) 原样保留 ...

    // (1) 同一把 4 KiB 读再来一次，只多加 IO_NOAUTH
    report->noAuthRead = static_cast<uint32_t>(
        vn_rdwr(kUIORead, devvp, noAuth->getBytesNoCopy(), static_cast<int>(kDeviceReadBytes), 0,
                kUIOSysSpace, kIOUnit | kIOFlagNoAuth, vfs_context_ucred(ctx), &noAuthResid, nullptr));

    // (2) 绕过 vnode、绕过 buffer cache、绕过 BSD，直接读这颗卷自己的 IOMedia
    gVolumeOp.done = 0;
    gVolumeOp.status = static_cast<uint32_t>(kIOReturnNotReady);
    gVolumeOp.actual = 0;
    IOStorageCompletion completion;
    completion.target = &gVolumeOp;
    completion.action = mediaOpDone;
    completion.parameter = nullptr;
    volume->read(client, 0, raw.get(), nullptr, &completion);
    report->mediaRead = gVolumeOp.done ? gVolumeOp.status : static_cast<uint32_t>(kIOReturnTimeout);
    report->mediaActual = static_cast<uint32_t>(gVolumeOp.actual);
    report->openAny  = volume->isOpen(nullptr) ? 1u : 0u;
    report->openUs   = volume->isOpen(client)  ? 1u : 0u;
    report->writable = volume->isWritable()    ? 1u : 0u;
    return;
  }
```

新字段（`65535` = 没走到；`volumeMountLadder()` 现在多收一个 `IOService *client`，就是 CloverBlock
自己在整盘媒体上开合 GPT 时用的那个客户端）：

| 字段 | 是什么 |
|---|---|
| `na=` | 同样的 4 KiB 读、同样 offset 0、同样 cred，**只多 `IO_NOAUTH`**；这个 flag 在整个 `vn_rdwr` 里只跳过 `mac_vnode_check_read` 那一处 |
| `ir=` | 直接 `IOMedia::read()` 的**原始 IOReturn**（十六进制），不是 errno |
| `ia=` | 它自己的 completion 报回来的字节数（请求 512） |
| `mo=` | `isOpen(nullptr)` —— 这颗媒体此刻被**任何人**开着吗 |
| `mc=` | `isOpen(client)` —— 是不是我们开着 |
| `mw=` | `isWritable()` —— 与 `dkopen` 的 FWRITE+EACCES 那一臂直接相关 |

读法（`ir=` 是决定性的那格，因为 `dkreadwrite` 写不出 13）：

- `ir=0x0 ia=512` ⇒ 这颗媒体读得通，13 是在它**上面**造的。此时看 `na=`：`na=0` 而 `dr=13` ⇒
  作者是 `vn_rdwr` 那道授权门，round134 的「设备侧持续拒绝」整条撤回，下一步改 apfs 之前先改我们
  自己的读标志；`na=13` ⇒ 门不是作者，去 `spec_read` / `buf_bread` 那一层找（`_vnode_dev_read` 与
  `vn_rdwr` 走的是不同臂，这一支还留着一格余地）。
- `ir=0xe00002cd`（NotOpen）配 `mo=0` ⇒ 这颗卷媒体根本没人开着，问题变成「`bdevvp` 那句无条件
  `VNOP_OPEN(FREAD)` 为什么没把它的 `_openLevel` 抬起来」—— 而 `dv=1:3` 的 minor 3 与 `volume` 是不是
  同一个 `MinorSlot` 也第一次成了可问的问题。
- `ir=0xe00002e4`（NoMedia）⇒ `isInactive()`，这颗媒体在挂载失败时已被注销；
  `ir=0xe00002e5`（UnformattedMedia）⇒ 它的 `_mediaSize` 或 `_preferredBlockSize` 是 0；
  `ir=0xe00002c2`（BadArgument）⇒ 几何不匹配（`_mediaSize < 512` 或方向不对）。
- `ir=0xe00002bf`（`kIOReturnTimeout`）⇒ 请求被排出去且没在本线程内联完成；自 round111 起这块板的
  `IOBlockStorageDriver` 都是内联完成，所以这本身是一条新信息，不是失败。
- `mc=1` 且 `mo=1` 且 `ir=0` ⇒ 我们没开错客户端，媒体也开着，那 13 与 open 状态无关。

`dx=`/`dy=`/`na=` 一律**不预设常量**，理由同 round134：容器 `NXSB` 在偏移 `0x20`，块 0 前 8 字节没有
可预测的东西；`na=` 的两种可能（0 或 13）都是答案。

**预算**：一次 512 B + 两次 4 KiB 读，全部在挂载失败之后；`rd=md0`、`clover-apfs=2`、`clover-emmc=21`、
`armcpu-clock-panic=180000` 一个都没动。

构建命令与上一轮相同（两个驱动都在 `[kernel] archives` 里，`kernel` 那格照旧不能省），`userdata`
不重建 —— 镜像一个字节都没变：

```
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-root/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

checker 增量：`verify-clover-payload.py` 的 `check_block()` 把身份串升到 `CloverBlock r135: apfs`，尾段
断言换成完整的 `dr=%u db=%u dx=0x%x dy=0x%x na=%u ir=0x%x ia=%u mo=%u mc=%u mw=%u r=%u w=%u`（这是编译器
拼接后的**一段连续字面量**，所以一次断言就覆盖了六个新字段全在位）；`check-clover.py` 的 `rd=md0`
那条只把轮次号从 round134 抬到 round135，理由一字未改。

产物归档在 `~/clover-out/round135-*`，`shasum -a 256 -c round135-SHA256SUMS`。

**刷写范围只有 `cache` 一个分区，没有新的 eMMC 分区写入**：`fastboot flash cache
/Users/zhouzhou/clover-out/round135-payload.img`，然后 `fastboot reboot` 冷启动。末行预告
**`@CloverBlock.cpp:1519`**（按 round134 学到的规则：`panic()` 调用的收尾 `);` 那一行）。

下一格候选（要等这一帧，排序按当前证据）：

1. **`na=0` 且 `dr=13`**：13 的作者是我们自己那两次 `vn_rdwr` 都带着的授权门，`mt=13` 是不是同一处写的
   要重新问一次 —— apfs 的读走 `_vnode_dev_read`/`vn_rdwr`，它传不传 `IO_NOAUTH` 得回宿主对着
   `mm=0xfffffff0056c65fac` 那段反汇编数一遍。
2. **`ir=NotOpen` 且 `mo=0`**：把 `dv=1:3` 的 minor 与这颗 `IOMedia` 的对应关系量出来（`_openClients`
   里是谁），并第一次认真问 `rd=md0` 那句谎是不是把 root 的 open 落在了另一颗媒体上。
3. **`ir=0 ia=512` 且 `na=13`**：换 `spec_read` 层 —— 同一个 vnode 改用 `VNOP_STRATEGY` 直接下一块
   buffer，把 `buf_bread` 的缓存与 `dkstrategy` 的错误通道切开。
4. **ADT `/chosen { root-snapshot-name }` / `/filesystems { DoNotUseSEP }` / `allow-root-hash-mismatch`**
   —— 仍然只在 `mt=` 过了 13 之后才排队。

## round135 读数（v0.3.8 实机，2026-10-26）：授权门不是作者，512 B 的探针死在几何上

屏上末帧原文（`@CloverBlock.cpp:1519`）：

```
CloverBlock r135: apfs f=15797536 n=11870275 m=1 l=0 d=10 s=1 c=1 v=1 vt=1 un=1 vs=0x6f4e vo=0x696c6e4f
vu=0x1d874fd vc=0xa7dd292a vn=0x6e776144/0x61647055/0x31326574/0x363448 dv=1:3 bd=0 al=0
t=0x73667061/0x0/0x0/0x0 on=0x2f fm=0x6b736964 dp=0xfffffffa:0xcbe833d0 fl=0x4005001 fc=0x4001001 fk=0x0
op=0xfffffff0:0x5f2b3c8 mm=0xfffffff0:0x56c5fac mt=13 st=65535 rt=65535 k=65535/65535/65535/65535
rd=65535 g=0 x=0x0 y=0x0 dr=13 db=0 dx=0x0 dy=0x0 na=13 ir=0xe00002c2 ia=0 mo=1 mc=0 mw=1 r=464 w=0
```

身份与选择依旧逐格不变（`t=0x73667061` = "apfs"、`fm=0x6b736964` = "disk"、`vs=0x6f4e` = "No"、
`dv=1:3`、`mt=13`、`r=464`），新格只有七格：

| 字段 | 读数 | 含义 |
|---|---|---|
| `dr=` / `db=` / `dx=` / `dy=` | 13 / 0 / 0x0 / 0x0 | 与 round134 完全相同：挂载失败后同一 devvp 的 4 KiB 读仍是 13，一个字节没搬 |
| `na=` | 13 | **同一把读、只多 `IO_NOAUTH`，结果不变** ⇒ `mac_vnode_check_read` 那道门不是作者 |
| `ir=` | `0xe00002c2` | 卷的 `IOMedia::read()` 返回 `kIOReturnBadArgument`，不是 13 |
| `ia=` | 0 | 它自己的 completion 报回 0 字节 |
| `mo=` | 1 | `isOpen(nullptr)` —— 这颗媒体**有人开着** |
| `mc=` | 0 | `isOpen(client)` —— 不是我们开的 |
| `mw=` | 1 | 可写 |

**这一格确认的事**：13 不是授权层写的。`IO_NOAUTH` 在整条 `vn_rdwr` 里只跳过
`mac_vnode_check_read` 一处，门关了读数一点没变，所以作者在 `VNOP_READ` 之下。
同时 `ir=BadArgument` 说明这颗卷媒体的读**根本没到 provider**：按 `IOMedia::read()` 的早退顺序
（inactive → openLevel → 零尺寸 → 空缓冲 → 方向 → 两个几何臂），在非空、`kIODirectionIn`、`byteStart=0`
的前提下，只剩 `_mediaSize - byteStart < buffer->getLength()` 一臂 —— 也就是那次请求 512 B 的探针
**自己**证明了 `_mediaSize < 512`。这不是「读坏了」，是「问得太长」。

**这一格推翻的一句，以及由此改的口径**：round135 的 `ir=` 描述的是探针，不是挂载。`dkreadwrite()`
在调 `media->read()` 之前会把请求裁到 `mediaSize - byteStart`（`IOMediaBSDClient.cpp` 的
`dkreadwrite`），所以挂载自己的读**不可能**死在 `_mediaSize < length` 那一臂：它会带着合法长度
一路走到 provider。能对挂载回 EACCES 的 `NotReadable` 也正好在 provider 那一侧。
换句话说，round135 只证明了「我们直接读这颗媒体时，请求长度太大」，还没证明这颗媒体读不通；
round136 把请求改成 `min(ms, 512)`，让它走过同一道几何门，去拿 provider 的答复。

`mo=1 mc=0` 另有一层信息：这颗媒体是被 BSD 的 `dkopen` 开的（`dkopen` 传的是 `minor->client`，
不是我们），所以 `NotOpen` 这一支在挂载路径上已经被排除；`mw=1` 也让 `dkopen` 里
FWRITE+EACCES 那一臂不可能命中。`r=464` 不涨与几何早退完全一致 —— 请求没出 `IOMedia`。

（顺带修掉 round134 读数里的一处落盘笔误：那一帧的 `dp=` 高半截多打了一位，写成
`0xffffffff2`；低半截 `0x352524c0` 与判读都不受影响。以后 `dp=` 一律按 `0x%x:0x%x` 的
两段原样抄。）

## v0.3.9（round136）：向卷的媒体要它自己的几何，再顺着 dev_t 找到它真正指的那颗媒体

一句话的问题：**那颗卷媒体的真实几何是多少，以及挂载真正读的是不是它。** 挂载调用依旧逐字节不变，
所有测量仍在 `VFS_MOUNT` 返回失败之后，全程只读。

```cpp
  // 请求长度改成 min(mediaSize, 512)，与 dkreadwrite 裁剪后的挂载请求走同一道几何门
  uint32_t requestBytes = 0;
  if (report->mediaSize) {
    requestBytes = report->mediaSize < kBlockBytes
        ? static_cast<uint32_t>(report->mediaSize) : kBlockBytes;
  }
  ...
  volume->read(client, 0, raw.get(), nullptr, &completion);
  report->mediaRead = gVolumeOp.done ? gVolumeOp.status : kIOReturnTimeout;
  report->mediaActual = gVolumeOp.actual;

  // 顺着 dv= 的 minor 走一遍我们发布的 disk 子树，看它落到哪颗 IOMedia
  IOMedia *minorMedia = findMediaByMinor(whole, report->major, report->minor, 0);
  if (minorMedia) {
    report->minorFound  = 1;
    report->minorSame   = minorMedia == volume;
    report->minorSize   = minorMedia->getSize();
    report->minorOpen   = minorMedia->isOpen(nullptr);
    report->minorWritable = minorMedia->isWritable();
    if (!report->minorSame && report->minorSize) {
      ... 同一把 min(minorSize, 512) 读 ...
    }
  }
  report->wholeOpen = whole->isOpen(nullptr);
```

`bs=`/`mb=` 是 `getPreferredBlockSize()`/`getBase()`：`IOMedia::read()` 在调 provider 之前会把
`byteStart` 加上 `_mediaBase`，所以 provider 看到的是「卷内偏移 + 基址」；`mb=` 是这一跳的账。

新字段：

| 字段 | 是什么 |
|---|---|
| `ms=` | 卷媒体的 `getSize()` —— round135 只是推出它 <512，这一格直接画出来 |
| `bs=` | 卷媒体的 `getPreferredBlockSize()` |
| `mb=` | 卷媒体的 `getBase()`（provider 看到的偏移 = `mb= + byteStart`） |
| `ml=` / `md=` | 这次请求的缓冲长度 / 方向，从 descriptor 读回，证明请求就是 `min(ms,512)`、方向是 In |
| `xm=` | 我们发布的 disk 子树里，有没有一颗媒体的 BSD Major/Minor 等于 `dv=` |
| `xe=` | 找到的那颗是不是 `volume` 本身 |
| `xs=` | 找到的那颗的尺寸（`xm=0` 时停在 `65535`） |
| `xo=` / `xw=` | 它的 `isOpen(nullptr)` / `isWritable()` |
| `xr=` / `xa=` | 对它的同一把自适应读的原始 IOReturn / 字节数；`xe=1` 时不重复读，保持 `65535` |
| `wo=` | 整盘媒体的 `isOpen(nullptr)`（GPT 与 APFS 扫描都从它读） |

读法（按证据分叉）：

- **`ms>=512` 且 `ir=0 ia=ml`** ⇒ 这颗卷媒体读得通。13 在它**上面**（`dkreadwrite` 的
  `prepare()`/对齐、或 `buf_bread` 的 16 KiB 请求在 512 B 媒体上失败）。
  此时再看 `r=`：这次读在采样之前，若 `r=464` 仍不涨，说明 provider 没落到
  `CloverBlockDevice`（走了缓存或别的对象），下一格改 `VNOP_STRATEGY`。
- **`ms<512`** ⇒ kernel 眼里的这颗「卷媒体」不是容器声明的 5.3 GiB 卷。`xe=1` 表示
  `dv=1:3` 就是它，那么整条挂载链从 bdevvp 起就指错了对象；`xe=0` 时 `xs=`/`xo=`/`xw=`/`xr=`
  才描述挂载真正读的那颗媒体，`xr=` 是它自己的答复。
- **`ir=0xe00002cd`（NotOpen）** ⇒ 现在请求长度已合法，NotOpen 才第一次可信；与
  `mo=`/`mc=`/`xo=` 一起判断是谁没开够。round135 的 `mo=1` 已先把这一支压得很小。
- **`ir=` 是 EACCES 家族里其余那几个**（NotReadable / NotPrivileged / ExclusiveAccess /
  LockedRead / LockedWrite）⇒ provider 明确拒绝，且挂载会拿到同一个 13；这时「为什么拒绝」变成
  provider 的问题，而不是几何的问题。
- **`ms=0`** ⇒ `ir=` 停在 `65535`（没发请求）；feed 的 `IOMedia::read()` 会先回
  UnformattedMedia，所以这个零是媒体的状态，不是请求的错。
- **`xm=0`** ⇒ `dv=1:3` 在我们的 disk 子树里找不到任何媒体：BSD 号是客户端登记的，而挂载走的是
  另一个 minor 槽；下一格量注册表里 `dv=` 的属主。

**预算**：最多两次 512 B 读（卷一次、若 `xe=0` 则 minor 一次），都在挂载失败之后；`rd=md0`、
`clover-apfs=2`、`clover-emmc=21`、`armcpu-clock-panic=180000` 全部原样。没有新的 eMMC 写入，
`userdata` 不重建。

构建命令与上一轮相同：

```
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-root/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

checker 增量：`verify-clover-payload.py` 的 `check_block()` 身份串升到 `CloverBlock r136: apfs`，
尾段断言换成连续的 `dr=%u … ms=0x%x:0x%x bs=%u mb=0x%x:0x%x ml=%u md=0x%x xm=%u xe=%u
xs=0x%x:0x%x xo=%u xw=%u xr=0x%x xa=%u wo=%u r=%u w=%u`；`check-clover.py` 的 `rd=md0`
那条只把轮次号抬到 round136。

产物归档在 `~/clover-out/round136-*`，`shasum -a 256 -c round136-SHA256SUMS` 七项全部 OK：
`payload.img` 58,720,256 B（`5835bc6f…`）、本地 `kernel.macho` `88a47806…`、卷内 `kernel.macho`
51,994,624 B（`bfed3887…`，probe patch localized 成 6,678 B / 35 runs）、卷内 `unixnu.toml`
629 B（`ab5b4d30…`）、ADT 13,268 B（`7d77ddd1…`）、`scratch-data.img` 35,888 B（`614b8356…`）、
trustcache 56 B（`c3ed2152…`）。两个 checker 全绿。

**刷写范围只有 `cache` 一个分区，没有新的 eMMC 分区写入**：`fastboot flash cache
/Users/zhouzhou/clover-out/round136-payload.img`，然后 `fastboot reboot` 冷启动。末行预告
**`@CloverBlock.cpp:1666`**（`panic()` 调用的收尾 `);` 那一行）。

下一格候选（要等这一帧，排序按当前证据）：

1. **`ms>=512` 且 `ir=0 ia=ml`**：媒体读得通，13 在 `dkreadwrite`/`buf_bread` 这一层。改问
   `VNOP_STRATEGY`，把 buffer cache 的 16 KiB 请求与 `dkstrategy` 的错误通道切开；同时看 `r=`
   这次为什么没涨。
2. **`ms<512` 且 `xe=0`**：挂载读的媒体不是我们留着的那颗。下一轮直接以 `xs=/xo=/xw=` 那颗为对象，
   量它的开合与读（`xr=` 本轮已经先给了一次答复）。
3. **`ir=NotOpen`（`0xe00002cd`）**：请求长度合法之后第一次可信，回到 open 账本：`mo=/mc=/xo=`
   谁开着、`dkopen` 给的是哪个 client。
4. **ADT `/chosen { root-snapshot-name }` / `/filesystems { DoNotUseSEP }` /
   `allow-root-hash-mismatch`** —— 仍然只在 `mt=` 过了 13 之后才排队。

## round136 读数（v0.3.9 实机，2026-10-26）：几何不是理由 —— 6.08 GB 的卷读 512 B 仍回 BadArgument

末行 `@CloverBlock.cpp:1666`，逐字段：

- **卷身份没动**：`v=1 vt=1 un=1`，`vs=0x6f4e`（`packedHead("No")`，未封存），
  `vo=0x696c6e4f`（`"Onli"`，Status 是 Online），`vu=0x01d874fd`、`vc=0xa7dd292a`
  （卷 UUID 与容器 UUID 的 FNV-1a），`vn=0x6e776144/0x61647055/0x31326574/0x00363448`
  拼出 `"Dawn"/"Upda"/"te21"/"H46"` —— FullName 头 15 字节。挂载指向的还是那颗唯一的
  未封存 OS 卷。
- **挂载阶梯**：`mt=13`，`st=rt=k=65535` —— `VFS_MOUNT` 仍是 EACCES，后面三级从未执行。
  挂载失败后的两刀：`dr=13 db=0 dx=0 dy=0`（对同一 devvp 的独立 `vn_rdwr` 仍是 13），
  `na=13`（同一个读加上 `IO_NOAUTH`，一位没变）。round135 的结论在这一帧上复现：
  **授权门不是作者**。
- **媒体账**：`ms=xs=0x1:0x6a400000` = 6,077,546,496 B（1,483,776 × 4096），`bs=4096`，
  `mb=0x0:0x0`；`ml=512 md=0x1` —— 请求确实是 512 B、方向 In。`xm=1 xe=1`：`dv=1:3`
  找到的那颗媒体就是被采样的这颗卷媒体本身。`xo=1 xw=1 mo=1 mc=0 mw=1` —— 它开着、可写，
  开它的不是本驱动。
- **直接读的答复**：`ir=0xe00002c2 ia=0` —— 绕过 vnode、buffer cache 和 BSD，对这颗
  6.08 GB 媒体读 512 B，得到的仍是 `kIOReturnBadArgument`、0 字节。`r=464 w=0` ——
  块驱动自己的读计数在采样前后没涨。

这一帧推翻了 round135 唯一还站着的几何推断：**`ms<512` 不成立**，媒体远大于请求，
`min(ms,512)` 的请求合法到不能再合法，`IOMedia::read()` 还是回 BadArgument。也就是说
`ir=` 描述的是这颗卷媒体自己的状态，而不是挂载失败的原因；`dr=`/`na=` 又证明 13 不在
vnode 授权门；round133 的两次 `vnode_open` 是 ENOENT、round134 的 devvp 读也是 13 ——
**13 的七个候选层全部被孤立复现排除，测量已经不再产生新信息**。

## v0.4.0（round137）：不再自己挂 —— 把根交回 `/chosen`，让 `vfs_mountroot()` 去挂

七轮 `mt=13 r=464` 一字未动之后，这一轮不再加字段，改的是**谁去挂**。两条依据都来自
内核源码，不是猜测：

- `IOFindBSDRoot()`（`iokit/bsddev/IOKitBSDInit.cpp:958`）等的就是 `/chosen` 的
  `root-matching` 字典指向的那颗 `IOMedia`；`ROOTDEVICETIMEOUT` 在 DEBUG 构建下是
  120 秒，匹配成功后从 service 读 `BSD Name`/`major`/`minor` 拼出 `makedev`（:940-975）。
- `vfs_mountroot()`（`bsd/vfs/vfs_subr.c:1210+`）先 `bdevvp(rootdev)`，再遍历 `vfsconf`：
  **`vfsp->vfc_mountroot` 非空就调用它**（:1247-1255），只有为空才落到
  `VFS_MOUNT(mp, rootvp, 0, ctx)`。round129-136 手工调的一直是后者；标准路径至少多出
  一条 apfs 自己注册的 mountroot 入口。

所以本轮的动作是：**驱动只负责让卷出现，挂载交给内核。**

```cpp
// clover-apfs=3：加载 apfs.kext，等卷节点和 BSD 名，然后返回
static void prepareApfsBoot(IOMedia *whole)
{
  kern_return_t loaded = OSKextLoadKextWithIdentifier(kApfsKext);
  ...
  for (; passes < 12000 && !found.volumeMedia; passes++) {   // 最多 120 s
    ApfsDepth now{};
    measureApfsDepth(whole, &now, 0);
    ...
    IOSleep(10);
  }
  for (uint32_t attempt = 0; attempt < 1000; attempt++) {     // 最多 10 s
    if (volumeBsd(found.volumeMedia, &devname, &major, &minor)) { named = true; break; }
    IOSleep(10);
  }
  IOLog("CloverBlock r137: boot v=%u un=%u vu=%s bsd=%s %u:%u p=%u\n", ...);
}
```

- 等 BSD 名不是仪式：`IOFindBSDRoot()` 在匹配成功后立刻读同一颗 service 的
  `BSD Name`/`major`/`minor`，卷节点出现而命名未完成时返回，会抢出一个 `makedev(0,0)`。
- `clover-apfs=2` 原样保留为探针（`apfsProbe()`，仍然只测量、最后 panic）；
  `clover-apfs=3` 走启动分支，**不 mount、不 panic**。
- `board.toml`：删除 `rd=md0`，加回
  `root_match_volume_uuid = "04769D26-2CA0-4825-B443-E2E5F88300F2"`（未封存 OS 卷，
  role System），`clover-apfs=3`；`userdata` 窗口与 `clover-emmc=21` 不变。
- `scripts/check-clover.py`：`/chosen/root-matching` 必须是 `IOProviderClass=IOMedia` +
  `IOPropertyMatch/UUID=<上面的 UUID>`，任何 `rd=` 都报错，`clover-apfs` 必须是 3。
- `scripts/verify-clover-payload.py`：新增 `ADT: root-matching names the OS volume`
  （在卷内 ADT 里直接找 `root-matching` + UUID + `IOMedia`），`check_block()` 身份串
  升到 `CloverBlock r137: apfs` / `CloverBlock r137: boot`。

内核侧接下来的顺序（同一轮启动内，不再有驱动介入）：`IOFindBSDRoot` 匹配 → `makedev`
→ `vfs_mountroot()` → 成功则 `mount_list_add`/`VFS_START` → `bsd_init.c:880-960` 做
`FSIOC_KERNEL_ROOTAUTH` → `_bsd_utaskbootstrap` exec `/sbin/launchd`。ADT 与 uint 参数
一切不变（`armcpu-clock-panic=180000` 仍是兜底），没有新的 eMMC 写入，全程只读。

构建命令不变：

```
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-root/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

产物归档在 `~/clover-out/round137-*`，`shasum -a 256 -c round137-SHA256SUMS` 七项全部 OK：
`payload.img` 58,720,256 B（`ba5fa957…`）、本地 `kernel.macho` `41892154…`、卷内
`kernel.macho` 51,994,624 B（`ab0290db…`，probe patch localized 成 6,680 B / 35 runs）、
卷内 `unixnu.toml` 588 B（`7d563204…`，args 里已无 `rd=md0`）、ADT 13,476 B
（`f9af6316…`，含 root-matching）、`scratch-data.img` 35,888 B（`614b8356…`）、
trustcache 56 B（`c3ed2152…`）。两个 checker 全绿。

**刷写范围只有 `cache` 一个分区，没有新的 eMMC 分区写入**：`fastboot flash cache
/Users/zhouzhou/clover-out/round137-payload.img`，然后 `fastboot reboot` 冷启动。

这一帧要看的不是一条末行，而是**控制台停在哪**（console 已由探针挂上，内核文字会留在
屏上）：

1. **`CloverBlock r137: boot … bsd=disk0sN M:N`** —— 驱动侧完成，卷与 BSD 名都在。
   下一行应是内核自己的 `Got boot device = …` / `BSD root: disk0sN, major …, minor …`。
2. **`panic("Failed to mount root device")`（`IOKitBSDInit.cpp:984`）** —— 120 秒内没有
   任何 `IOMedia` 携带 `root-matching` 的 UUID。此时回看 `vu=` 的哈希与宿主对
   `04769D26-…` 算出的 FNV-1a 是否一致（大小写/字符串差异会直接表现为匹配不到）。
3. **`cannot mount root, errno = 13`（`bsd_init` 的循环）** —— 匹配到了，但标准路径仍然
   拿到 13。这会把「13 在 apfs 的 `vfs_mount` 里」这一格钉死；同帧若有
   `[clover] vfs_mountroot returned` 标记，说明 apfs 没有注册 `vfc_mountroot`，标准路径
   落回了 `VFS_MOUNT`。
4. **`rootvp not authenticated after mounting`** —— 挂上了，墙变成
   `FSIOC_KERNEL_ROOTAUTH`（根清单/封存那一层），下一格转向根授权而不是挂载。
5. **exec 阶段的 AMFI 拒绝** —— 根挂载与 `/sbin/launchd` 的查找都过了，墙是
   owner trust cache 里没有它的 cdhash；下一格补信任，不再是挂载问题。

## round137 读数（v0.4.0 实机，2026-10-26）：APFS 根挂上了，墙是 ROOTAUTH

第一帧末行是 `rootvp not authenticated after mounting`。这一句不是挂载失败，恰好相反：
它只出现在 `bsd_init.c` 的挂载成功分支里——`vfs_mountroot()` 返回 0、`VFS_ROOT` 拿到
根 vnode、`set_rootvnode()` 完成之后，非 macOS 构建把 `require_rootauth` 置为真，
对根 vnode 发 `FSIOC_KERNEL_ROOTAUTH`（`bsd/sys/fsctl.h:476`），ioctl 返回错误且没有
owner 授权时才在 `bsd_init.c:1098` panic。也就是说：

- `/chosen/root-matching` 的 UUID 匹配到了本驱动发布的 OS 卷；
- `IOFindBSDRoot()` 从 service 读到了 BSD 名与 major/minor，`makedev` 成立；
- 内核自己的 `vfs_mountroot()` 把 APFS 卷挂上了，round129-136 手工 `VFS_MOUNT`
  的 13 没有出现；
- 墙是根授权：这颗卷是未封存的复制卷，`FSIOC_KERNEL_ROOTAUTH` 不会给它认证。

round137 同时证明了 `v0.4.0` 把挂载交回内核是正确方向：驱动只需让卷出现，根发现、
`bdevvp`、`vfs_mountroot` 都由 XNU 自己完成。

## v0.4.1（round138）：用四个字符的 `rd=` 跳过根授权门

`bsd_rooted_ramdisk()`（`bsd_init.c:450-466`）就是那道门的开关：

```c
#define RAMDEV "md0"                                   /* :447 */
...
if (PE_parse_boot_argn("rd", dev_path, MAXPATHLEN)) {
    if (strncmp(dev_path, RAMDEV, strlen(RAMDEV)) == 0) {
        is_ramdisk = true;
    }
}
```

它只读 boot-arg `rd`，只比前三个字符，从不解析设备名；`bsd_init.c:1057` 的
`if (!bsd_rooted_ramdisk())` 是它在整个内核里唯一的使用点。所以 `rd=md0s39`
让整段授权块不执行，同时不会夺走根：

- root-matching 先赢：`IOFindBSDRoot()` 在 `IOKitBSDInit.cpp:790-797` 把字典装进
  `matching` 就 `continue`，`:888` 的 BSD 名支路要求 `!matching`；
- md 内存设备捷径要求 `mdX` 正好三字符（`rdBootVar[3] == 0`，`:849`/`:1080`），
  `md0s39` 四字符不满足；
- 这个名字在本轮设备树里也不再存在：CloverBlock 发布的是 APFS 卷自己的 BSD 名。

round121 已在实机量过同一形状：`BSD root: md0s39, major 1, minor 39`，根授权块被跳过。

改动只有启动参数与 checker，驱动与内核二进制不动：

- `board.toml`：`kernel_args` 加回 `"rd=md0s39"`，标题升到 r138。
- `scripts/check-clover.py`：根契约从「任何 `rd=` 都报错」改为「必须且只能有一个
  `rd=md0s39`」，其他 rd 值继续报错。
- `scripts/verify-clover-payload.py`：无需改动（驱动身份串仍是 `CloverBlock r137`）。

内核侧接下来的顺序（同一轮启动内）：`IOFindBSDRoot` 匹配 → `makedev` →
`vfs_mountroot()` → 跳过 ROOTAUTH → `/sbin/launchd` 的 path 查找与 exec。
ADT 不变（13,476 B），uint 参数不变（`armcpu-clock-panic=180000` 仍是兜底），
没有新的 eMMC 写入，全程只读。

构建命令不变：

```
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-root/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

**刷写范围只有 `cache` 一个分区，没有新的 eMMC 分区写入**：`fastboot flash cache
/Users/zhouzhou/clover-out/round138-payload.img`，然后 `fastboot reboot` 冷启动。

产物归档在 `~/clover-out/round138-*`，`shasum -a 256 -c round138-SHA256SUMS` 七项全部 OK：
`payload.img` 58,720,256 B（`d5f0d63d…`）、本地 `kernel.macho` `a8e50254…`、卷内
`kernel.macho` 51,994,624 B（`73ec05ba…`，probe patch localized 成 6,680 B / 35 runs）、
卷内 `unixnu.toml` 614 B（`155aaa73…`，args 末尾是
`… rd=md0s39 armcpu-clock-panic=180000`）、ADT 13,476 B（`f258f237…`，含 root-matching）、
`scratch-data.img` 35,888 B（`614b8356…`）、trustcache 56 B（`c3ed2152…`）。两个 checker 全绿。

这一帧只问一个问题：**跳过 ROOTAUTH 之后，`bsd_init` 会不会继续走到
`_bsd_utaskbootstrap` exec `/sbin/launchd`，以及第一颗被拒的 exec 是不是它。**
控制台仍由探针挂在前面，内核文字会留在屏上：

1. **不再出现 `rootvp not authenticated after mounting`，末行走过
   `siginit returned, execing init` 之后停在 launchd 的某处** —— 根授权门已跳过，
   下一格是 launchd 的加载与 AMFI。若停住时屏上没有任何 panic，说明内核在
   exec 前/中挂住，下一轮把标记移到 exec 前后。
2. **再次出现 `rootvp not authenticated after mounting`** —— `rd=md0s39` 没进内核
   命令行（先看 `v0.2.80` 起 PMGR 打出的完整 args 行），或 `PE_parse_boot_argn`
   没读到它；这不是机制失败，是参数没送达。
3. **`panic("Failed to mount root device")`（`IOKitBSDInit.cpp:984`）** —— 根声明被
   `rd=` 抢走了，属回归；检查 `rd=` 是否被拼成正好三字符的 `md?`。
4. **`[clover] ... returned` 标记停在 exec 前** —— 内核走到了 exec，墙在
   Mach-O/AMFI 一侧，下一轮补 launchd 的 cdhash 或检查 `OSKextLoadKextWithIdentifier`
   之后的 kext 状态。
5. **`cannot mount root, errno = N`** —— 根挂载又失败了，回看 `vu=` 与
   `root-matching` 的 UUID 是否仍一致；这一格与 round137 的成功读数相矛盾，
   优先怀疑 payload 里混进了旧参数。

## round138 读数（v0.4.1 实机，2026-10-26）：根授权过了，墙是 AMFI 的 exec 策略

第一帧末行是 `unexpected SIGKILL of launchd with reason -- namespace 9 code 0x8`，
下一行 `description none`。这一帧同时回答了两件事。

**`rd=md0s39` 生效了。** 屏上没有 `rootvp not authenticated after mounting`，也没有
`cannot mount root`：根挂载保持成功，`FSIOC_KERNEL_ROOTAUTH` 整段没有执行，
`bsd_init` 一路走到 `_bsd_utaskbootstrap` 的 exec。这一格从此不再是问题。

**墙在 exec 的安全策略上，且不是签名校验。** 拆开那行 reason：

- `namespace 9` 是 `OS_REASON_EXEC`（`bsd/sys/reason.h:87-127`），
  `code 0x8` 是 `EXEC_EXIT_REASON_SECURITY_POLICY`（`:255`）；
- initproc 收到 SIGKILL 后由 `psignal_internal()` 直接 panic
  （`bsd/kern/kern_sig.c:2072-2078`），所以这就是 launchd 第一次 exec 被拒；
- `description none` 是关键判别位。`process_signature()` 只在
  `ip_mac_return != 0` 的分支（`kern_exec.c:8541-8546`）创建不带描述的
  `os_reason_create(OS_REASON_EXEC, EXEC_EXIT_REASON_SECURITY_POLICY)`；
  带描述的两条支路分别是 `proc_process_signature()`（`:8476`）和
  `ip_cs_error`（`:8549-8553`）。这一帧落在前者，说明拒绝来自 MAC 策略的
  label-update 钩子，而不是签名校验本身；
- `ip_mac_return` 由 `kauth_proc_label_update_execve()` 的 `label_update_return`
  赋值（`kern_exec.c:6751`），值由每个 MAC 策略的 `mpo_cred_label_update_execve`
  合成（`security/mac_vfs.c:840-870`）。这里已加载、且会在这个钩子里拒绝的只有 AMFI；
- AMFI 是 Apple 自己的 kext，构建日志第 31 行的
  `--root com.apple.driver.AppleMobileFileIntegrity` 与 `--early-security-init`
  证明它来自 stock 21H461 kernelcache；同一帧上
  `Security policy loaded: Apple Mobile File Integrity (AMFI)` 与
  `AppleMobileFileIntegrity: Starting` 说明它已经起来；
- `attempting to load 1 external trust cache modules` / `loaded external trust cache
  module: 0` 是我们那 56 B 的 mockfs 信任模块（`c3ed2152…`），里面没有 launchd 的
  cdhash。卷上的 launchd 对这颗 AMFI 来说既不是 platform binary，也不在信任表里。

同一帧上的 `AppleImage4: root power domain not yet available: 19`、
`RPI: ignored model name from device tree: iPad7,1`、
`AppleMobileFileIntegrity: Error: Profile handler? region not found (?)` 都发生在
`Starting` 之前或之外，是 profile 数据缺失的旁证，不是这次拒绝的直接原因：
拒绝点是由 `description none` 钉住的 label-update 钩子，而不是某个签名 verdict。

## v0.4.2（round139）：关掉 AMFI 的执行策略，只加一个 boot-arg

这一轮的假设只有一句：**如果 r138 的拒绝来自 AMFI 的 label-update 钩子，那么
`cs_enforcement_disable=1` 应该让同一次 exec 通过。** 依据是两处独立读同一名字的代码：

```c
/* bsd/kern/kern_codesigning.c:242-289（#if DEVELOPMENT || DEBUG） */
PE_parse_boot_argn("cs_enforcement_disable", &cs_enforcement_disabled, ...);
...
else if (cs_enforcement_disabled) config |= CS_CONFIG_ALLOW_ANY_SIGNATURE;
if (cs_enforcement_disabled) config |= CS_CONFIG_ENFORCEMENT_DISABLED;
else if (cs_enforcement_disabled) config |= CS_CONFIG_GET_OUT_OF_MY_WAY;
```

XNU 侧这三位置是 `amfi_get_out_of_my_way` 的严格超集；AMFI 的 kext 在 `start()` 里
另读一次同名参数，并打印 `%s: cs_enforcement disabled by boot-arg`。预链接镜像里
`amfi_get_out_of_my_way` 只有 XNU 解析器那一份字符串，没有 AMFI 的副本，所以那个
经典越狱参数够不到真正拒绝 launchd 的钩子；这一轮只加 `cs_enforcement_disable=1`，
把「一次刷机只回答一个问题」保持在单变量上。

改动只有一处参数与 checker，内核与驱动二进制不动：

- `board.toml`：`kernel_args` 在 `rd=md0s39` 之后加 `"cs_enforcement_disable=1"`，
  标题升到 r139。
- `scripts/check-clover.py`：新增断言，`kernel_args` 必须包含
  `cs_enforcement_disable=1`；`rd=md0s39` 的唯一性断言原样保留。
- `scripts/verify-clover-payload.py`：无需改动（驱动身份串仍是 `CloverBlock r137`）。

构建命令不变：

```
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-root/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

**刷写范围只有 `cache` 一个分区，没有新的 eMMC 分区写入**：`fastboot flash cache
/Users/zhouzhou/clover-out/round139-payload.img`，然后 `fastboot reboot` 冷启动。

产物归档在 `~/clover-out/round139-*`，`shasum -a 256 -c round139-SHA256SUMS` 七项全部 OK：
`payload.img` 58,720,256 B（`59c10e5a…`）、本地 `kernel.macho` `e6683d30…`、卷内
`kernel.macho` 51,994,624 B（`7272a92b…`，probe patch localized 成 6,680 B / 35 runs）、
卷内 `unixnu.toml` 646 B（`7fa57db2…`，args 末尾是
`… rd=md0s39 cs_enforcement_disable=1 armcpu-clock-panic=180000`）、ADT 13,476 B
（`959cb293…`，含 root-matching）、`scratch-data.img` 35,888 B（`614b8356…`）、
trustcache 56 B（`c3ed2152…`）。两个 checker 全绿。

这一帧只问一个问题：**`cs_enforcement_disable=1` 送达后，AMFI 会不会放过 launchd 的
第一次 exec。** 读帧顺序：

1. **屏上出现 `cs_enforcement disabled by boot-arg`** —— AMFI 自己的 `start()` 认了
   这个参数，这是参数送达的最强证据；紧随其后若没有同一条 SIGKILL 行，说明钩子放行，
   启动进入 launchd 的 dyld/库加载。
2. **同一行 `namespace 9 code 0x8` 再次出现，且没有 `cs_enforcement disabled` 行**
   —— 参数没有送到 kext。先看 PMGR 打出的完整 args 行是否含
   `cs_enforcement_disable=1`；若含而 kext 没认，下一格是 AMFI 的 profile region
   缺失（同一帧已有 `Profile handler? region not found`），转向补 profile 而不是再换参数。
3. **reason 变成带描述的一种** —— 签名校验路径开始说话（`ip_cs_error` 支路），
   下一格是补 launchd 的 cdhash 到 trust cache 或继续放开签名检查，不再是 MAC 钩子。
4. **不再有 SIGKILL，但停在 launchd 之后某处** —— 根、授权、AMFI 三层已过；
   下一轮把探针的 `[clover] ... returned` 标记移到 exec 前后，定位新墙。
5. **`armcpu-clock-panic=180000` 到点** —— 启动整体被兜底，按前四条的日志尾段判读。

## round139 读数（v0.4.2 实机，2026-10-26）：参数送到了，但 AMFI 自己先 panic

实机第一帧没有出现 `cs_enforcement disabled by boot-arg`，也没有第二次 launchd 的
SIGKILL。屏上第一行是：

```
panic: "can't has cs_enforcement_disable" @AppleMobileFileIntegrity.cpp:5261
```

`ticks=0`，`BC=0xfffffff007a4a3b4`，整帧的时间戳落在启动约 0.32 s，比 r138 的 exec
拒绝更早。这条 panic 不是 XNU 的 CS 核心打的，是 AMFI kext 自己的初始化路径打的。
把预链接镜像里那个字符串的引用点找出来，整段逻辑只有六条指令：

```asm
0x4ab4ba8  adrp x0, 0x42df000 ; add x0,x0,#0xe5a   ; "cs_enforcement_disable"
0x4ab4bb8  bl   PE_parse_boot_argn
0x4ab4bbc  cbz  w0, 0x4ab4bec                       ; 没传 → 跳过
0x4ab4bc0  ldr  w8,[sp,#0x20] ; cbz w8, 0x4ab4bec   ; 传了 0 → 跳过
0x4ab4bdc  bl   IOLog("%s: cs_enforcement disabled by boot-arg\n", "AMFI")
0x4ab4be4  bl   0x4ac9bd4                            ; 单参数守卫
0x4ab4be8  cbz  w0, 0x4ab4fa4                        ; 返回 0 → panic thunk
```

`0x4ac9bd4` 是一个 import stub，GOT 槽在 `0x5cfaba8`。读出来的绑定目标是
`0xfffffff007944964`，dSYM 里正是 `PE_i_can_has_debugger`：

```
PE_i_can_has_debugger(uint32_t *flags)
  return debug_enabled;              // pexpert/arm/pe_init.c:598-611
```

也就是说 AMFI 的条件是：**只要传了 `cs_enforcement_disable`，板子就必须是“可调试”的，
否则宁可 panic 也不许关执行策略。** 这与参数有没有送达是两个问题。r139 的帧同时证明
了两件事：boot-arg 确实进了 kext（否则不会走到这条分支），以及我们从来没有满足它的
前置条件。

`debug_enabled` 不是 boot-arg，也没有第二个写手：`pe_arm_init()` 在
`SecureDTLookupEntry(NULL, "/chosen")` 之后读 4 字节的 `debug-enabled`，原样拷进这个
变量（`pexpert/arm/pe_init.c:467-481`）。开发熔断的 iBoot 会发布它；clover 的 ADT
从 `v0.2.x` 起就没有这个属性，所以它一直是 `FALSE`。这一轮的问题因此从“AMFI 的钩子
认不认参数”改成了“板子有没有告诉内核自己可调试”。

## v0.4.3（round140）：发布 `/chosen/debug-enabled`，让 AMFI 接受那个参数

改动只有一个 ADT 属性，`kernel_args` 一个字符不动：

```python
# scripts/make-clover-adt.py，/chosen 里
("debug-enabled", u32(1)),
```

`PE_i_can_has_debugger()` 是 AMFI 在 `start()` 里唯一读取的状态，而它的唯一写手就是
这个属性。发布 4 字节的 1 等价于告诉内核“这台设备是开发熔断的”，也正是 r139 缺的那一格。
`scripts/check-clover.py` 新增断言：`/chosen/debug-enabled` 必须存在、必须是 4 字节、
必须等于 1；ADT 因此从 13,476 B 增到 13,516 B（32 字节名字 + 4 字节长度 + 4 字节值）。
board 标题升到 r140。

构建命令不变：

```
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-root/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

**刷写范围只有 `cache` 一个分区，没有新的 eMMC 分区写入**：`fastboot flash cache
/Users/zhouzhou/clover-out/round140-payload.img`，然后 `fastboot reboot` 冷启动。

产物归档在 `~/clover-out/round140-*`，`shasum -a 256 -c round140-SHA256SUMS` 七项全部 OK：
`payload.img` 58,720,256 B（`dd5b5b46…`）、本地 `kernel.macho` `446fe0d7…`、卷内
`kernel.macho` 51,994,624 B（`7b702371…`，probe patch localized 成 6,680 B / 35 runs）、
卷内 `unixnu.toml` 650 B（`aedb1c3f…`，args 末尾是
`… rd=md0s39 cs_enforcement_disable=1 armcpu-clock-panic=180000`）、ADT 13,516 B
（`ba88bcd6…`，比 r139 多一条 4 字节的 `debug-enabled`）、`scratch-data.img` 35,888 B
（`614b8356…`）、trustcache 56 B（`c3ed2152…`）。两个 checker 全绿。

这一帧只问一个问题：**`PE_i_can_has_debugger()` 为真之后，AMFI 会不会放过 launchd
的第一次 exec。** 读帧顺序：

1. **屏上出现 `AMFI: cs_enforcement disabled by boot-arg` 且没有 panic** —— 前置条件
   过了，参数被接受；接着看有没有 launchd 的 SIGKILL 行。
2. **同一条 `can't has cs_enforcement_disable` 再次出现** —— `debug-enabled` 没有到达
   `debug_enabled`。先看 PMGR 的 args 行（确认 `cs_enforcement_disable=1` 还在），
   再核对 ADT 里属性是否被别处覆盖；下一格是 kext 读取的 `/chosen` 与我们的 ADT 不是
   同一个节点。
3. **`namespace 9 code 0x8` 再次出现，且已有 `cs_enforcement disabled` 行** —— 参数与
   前置条件都过了，拒绝仍在 MAC 钩子。下一格转去量 AMFI 的 profile region 与
   `amfi-only-platform-code`（我们的 ADT 给的是 1，kext 把它变成
   “不允许非平台代码”，这可能是第二道门），不再是换 boot-arg。
4. **reason 变成带描述的一种** —— 签名校验路径开始说话（`ip_cs_error` 支路），
   下一格是补 launchd 的 cdhash 到 trust cache。
5. **不再有 SIGKILL，但停在 launchd 之后某处** —— 根、授权、AMFI 三层已过；
   下一轮把探针标记移到 exec 前后，定位新墙。

## v0.4.4（round141）：让 MAC 层自己说出是谁拒绝了 launchd

r140 的帧只证明了一件事：AMFI 不再 panic 在 `can't has cs_enforcement_disable` 上，
并且 `Guarded vnode policy (vnguard)` 第一次出现——`debug-enabled` 让 vnguard 完成了
注册。帧上那两行 `AppleARMBootPerf: … Error` 与签名无关：它是 `/chosen/iboot-profile-data`
缺失时打出的性能配置读取失败，属于背景噪声。真正的死因仍是
`namespace 9 code 0x8`，即 `OS_REASON_EXEC` 的 `EXEC_EXIT_REASON_SECURITY_POLICY`。

这个理由只有一个产者。`kern_exec.c:8541` 的 `if (imgp->ip_mac_return != 0)` 是唯一
kill 点，而 `ip_mac_return` 只在 `kauth_proc_label_update_execve()` 返回非零时被写入，
即 `security/mac_vfs.c` 的 `mac_cred_label_update_execve()`。它把每个策略的结果用
`mac_error_select(hookret, error)` 合并；同一文件里还有 `mac_cred_check_label_update_execve()`
（`result = result || hookret`），它在标签更新之前跑，同样能把 exec 挡下。

这一轮不改任何行为，只把合并前的 `hookret` 打出来。补丁是
`patches/xnu-clover/zzz-clover-mac-exec-probe.patch`（工作副本在
`~/UniXNU/patches/xnu/`，构建脚本按文件名顺序应用），只动 `security/mac_vfs.c`：

```c
hookret = hook(cred, new, vfs_context_proc(ctx), vp, offset, scriptvp,
    mac_vnode_label(vp), scriptvnodelabel, execl, csflags, spawnattr, spawnattrlen, disjoint);
if (hookret != 0 && proc_getpid(vfs_context_proc(ctx)) == 1) {
    printf("clover-mac: label-update %s (%s) -> %d disjoint=%d\n",
        mpc->mpc_name ? mpc->mpc_name : "?",
        mpc->mpc_fullname ? mpc->mpc_fullname : "?", hookret, *disjoint);
}
error = mac_error_select(hookret, error);
```

静态与动态两段列表（`staticmax` 和 `mac_policy_list_conditional_busy()` 之后）各一遍；
check 函数的两个循环同理，行首是 `clover-mac: label-check`。三个约束：只对 pid 1、
只在非零时打印、返回值原样交回 `mac_error_select`/`||`。所以探针不会改变拒绝者，也
不会改变拒绝码；它只在同一帧里多出一行策略名。

为什么不去改 `kern_exec.c`：那段代码的上下文被 `zzz-boot-progress.patch` 的 hunk
覆盖，任何插入都会让 boot-progress 的反向重放失败，进而拖垮后面依赖它的 root-binding
补丁。MAC 层已经能给出策略名，`kern_exec` 只能重复一次“有人拒绝了”，不值得冒这个险。

静态枚举的结论是：这份内核里真正安装 exec 标签钩子的只剩 **Sandbox**。
`mpo_cred_check_label_update_execve` 在它的 kext 里是 `mov w0,#1; ret`
（`0x5a0d204`），`mpo_cred_label_update_execve` 在 `0x5a0d20c`；两者的拒绝都走
`os_log`，所以 r139/r140 的 panic 帧上看不到它说话。AMFI 的每个拒绝分支都会上屏
（unsigned code、Legacy VPN、dyld 签名、`AMFI: hook..execve() killing pid %u: %s`），
帧上没有这些行，因此 AMFI 已放行。候选只剩 Sandbox，这一轮就是让它在拒绝时留下名字。

构建命令不变：

```
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-root/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

`verify-clover-payload.py` 新增一条断言：卷内内核必须同时含
`clover-mac: label-update %s (%s) -> %d disjoint=%d\n` 与
`clover-mac: label-check %s (%s) -> %d\n` 两条格式串。少了任一条，就说明这个镜像
没有带探针，帧拍回来也无法归因。board 标题升到 r141。

**刷写范围只有 `cache`，没有新的 eMMC 分区写入**：`fastboot flash cache
/Users/zhouzhou/clover-out/round141-payload.img`，然后 `fastboot reboot` 冷启动。

产物归档在 `~/clover-out/round141-*`，`shasum -a 256 -c round141-SHA256SUMS` 七项全部 OK：
`payload.img` 58,720,256 B（`a98f6a92…`）、本地 `kernel.macho` `c241d778…`、卷内
`kernel.macho` 51,994,624 B（`908018e8…`，probe patch localized 成 6,680 B / 35 runs）、
卷内 `unixnu.toml` 624 B（`274e3be2…`，args 末尾仍是
`… rd=md0s39 cs_enforcement_disable=1 armcpu-clock-panic=180000`）、ADT 13,516 B
（`b1510f88…`；与 r140 的 64 字节差异全部落在 `/chosen/random-seed`，每次构建重新生成，
其余节点不变）、`scratch-data.img` 35,888 B（`614b8356…`，与 r140 相同）、trustcache
56 B（`c3ed2152…`，与 r140 相同）。两个 checker 全绿。

这一帧只问一个问题：**launchd 第一次 exec 时，是哪一个 MAC 策略返回了非零。**
读帧顺序：

1. **出现 `clover-mac: label-update Sandbox (…) -> 1 disjoint=…`** —— 命中。
   拒绝者是 Sandbox 的标签更新；下一轮去量它的 profile 与 launchd 的 entitlements
   （`com.apple.private.*`、`platform-application`），或者给根卷上的 launchd 补一个
   可被 Sandbox 接受的标签，不再动 boot-arg。
2. **同一行里是别的策略名** —— 按名字定点。`mpc_fullname` 在括号里，能区分同名实例；
   下一轮直接读那个策略的 `mpo_cred_label_update_execve`。
3. **只有 `clover-mac: label-check …`，没有 `label-update`** —— 拒绝发生在标签更新
   之前：check 钩子先返回了 1，`kauth_proc_label_update_execve()` 根本不会走到 update。
   按 check 行里的名字查它的 `mpo_cred_check_label_update_execve`。
4. **仍是 `namespace 9 code 0x8`，但一条 `clover-mac` 都没有** —— `printf` 没到帧上。
   先确认这是内核行而不是用户态输出：`printf` 走 kernel console，和之前 PMGR/SDHCI
   那些行同一条路；若确实被吞，下一格改用 panic 前追加一条 logfs 记录或直接
   `panic()` 带策略名，不再靠 console。
5. **reason 不再是 `namespace 9 code 0x8`** —— MAC 层已经放行，墙换了位置。若变成
   带描述的签名失败，看 `ip_cs_error` 支路；若没有 panic 而是停在 launchd 之后，
   下一轮把探针标记移到 exec 完成之后。

## v0.4.5（round142）：照 sheng 的信任缓存布局，把原始 Apple 模块接上

r138–r141 一直把 `namespace 9 code 0x8` 当成一个"策略问题"去追：先跳过 ROOTAUTH，
再关 `cs_enforcement_disable`，再补 `/chosen/debug-enabled`，最后在 MAC 层装探针。
sheng（Xiaomi Pad 6S Pro，同一个 `ipados-17.7.11-21H461` 目标）在同一段路上没有遇到
这个 panic，差别不在策略，而在**启动信任缓存本身**：

它的启动卷里是**一个三模块 bundle**——先放自己的 owner 模块，再把目标 IPSW 的两份
原始 Apple 模块逐字节接在后面：

```
<count=3, offsets[3]> <owner 模块> <StaticTrustCache 模块> <Cryptex1,SystemTrustCache 模块>
模块 = <version=2, uuid[16], n> <n × (cdhash[20], type, 3 pad)>
```

clover 从 round123 起只有 56 字节的**单模块** bundle（`scripts/make-clover-root.py`
签出的 mockfs init，cdhash `4f63be63…`）。可 round137 起根已经换成了 APFS 上的 OS 卷
（`rd=md0s39`），内核第一次 exec 的是根卷上那颗真正的 `/sbin/launchd`。宿主侧已经量过：

```
$ codesign -dvvv /Volumes/DawnUpdate21H461…/sbin/launchd
CDHash=1d52c69747dffc4b1937d241bdbc9cee3b22df18
Signature=adhoc
```

这颗 cdhash 在 r141 的 56 字节 bundle 里不存在，在 IPSW 的 `StaticTrustCache` 里是
**第 279 条**（2478 条中的一条）。也就是说 r141 的 launchd 在 AMFI 眼里没有任何离线
信任，`amfi-only-platform-code=1` 又要求它只能信平台码——标签更新钩子返回非零、
`ip_mac_return != 0`、无描述的 `namespace 9 code 0x8`，整条链完全吻合。探针没打出来
是因为帧拍在 r140 的内核上（r141 的 payload 只换了内核文本，未确认帧里的构建号）。

这一轮**只改一个变量：信任缓存**。内核与 r141 逐字节相同（本地 `kernel.macho`
`c241d778…`），boot-args、ADT 结构、根镜像、SDHCI/CloverBlock 全部不动。

新增 `scripts/make-clover-trust-bundle.py`，模型就是 sheng 的
`scripts/make-owner-trust-bundle.py`：

- owner 模块取自 `build/clover-root/trustcache.bundle`（必须恰好 1 个模块）；
- `StaticTrustCache` 从 IPSW 成员 `Firmware/044-76040-038.dmg.trustcache` 解 IM4P，
  解开后的模块 sha256 必须是 `da88460e…`，59,496 字节 / 2478 条；
- `Cryptex1,SystemTrustCache` 从 `Firmware/044-76034-038.dmg.trustcache` 解出，
  sha256 必须是 `d121ba89…`，1,848 字节 / 76 条；
- 两份原始模块**逐字节不改**，并按 DarwinBoot `ValidTrustCaches` 的同一套结构规则
  （模块数 ≤128、offset 对齐递增、version=2、`24 + n*24` 精确、type 1..4、
  模块内 cdhash 严格升序）自检后才写盘。

产物：`build/clover-trust/trustcache.bundle`，61,408 字节（16 头 + 48 + 59,496 + 1,848），
sha256 `7c369ce0…`。`board.toml [payload] trust_cache_sha256` 钉住它，
`verify-clover-payload.py` 改成对这颗 bundle 做逐字节比对，`check-clover.py` 增加
三条断言（模块数=3、两份原始模块 sha256、版本 2 与长度），在产物存在时执行。

IPSW 成员不用整包下载：用框架 `scripts/unixnu/ipsw.py` 的 HTTP Range 读取器，
只取这两个成员（59,519 B 与 1,871 B），落在 `out/clover/ipsw/21H461/`，并各自带
sha256 记录。

### 构建、校验与本轮该刷哪一格

```
# 只重打 payload；内核不重建
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

产物归档在 `~/clover-out/round142-*`，`shasum -a 256 -c round142-SHA256SUMS` 七项全 OK：
`payload.img` 58,720,256 B（`0b03fbaa…`）、本地 `kernel.macho` `c241d778…`（与 r141 相同）、
卷内 `kernel.macho` 51,994,624 B（`908018e8…`，探针补丁仍在）、卷内 `unixnu.toml`
626 B（`72f78109…`，args 与 r141 逐字相同）、ADT 13,516 B（`ded422cc…`；差异只来自
每次构建新生成的 `/chosen/random-seed`）、`scratch-data.img` 35,888 B（`614b8356…`）、
**trustcache 61,408 B（`7c369ce0…`，r141 是 56 B 的 `c3ed2152…`）**。两个 checker 全绿。

**刷写范围只有 `cache`，没有新的 eMMC 分区写入**：`fastboot flash cache
/Users/zhouzhou/clover-out/round142-payload.img`，然后 `fastboot reboot` 冷启动。
菜单第一行应当是 `clover r142: boot with the three-module trust cache sheng boots with`。

这一帧只问一个问题：**launchd 的 cdhash 现在在启动信任缓存里了，AMFI 还挡不挡。**

1. **不再有 `namespace 9 code 0x8`，launchd 之后的内核日志继续往下走** —— 墙从
   exec 信任挪到了用户态启动（缺 Data 卷、缺服务、缺文件系统上的配置）。下一轮去读
   launchd 停在哪一条，不再动信任缓存。
2. **仍是 `namespace 9 code 0x8`，且出现 `clover-mac: label-update AMFI (…) -> 非零`** ——
   探针这次把拒绝者点名了；但 cdhash 已经确认在 bundle 里，下一刀要查的是**挂载的根卷
   是不是这颗 launchd 所在的卷**（卷 UUID/角色/Named 对不上时会 exec 到别的副本），
   以及 AMFI 是否因为 `cs_enforcement_disable=1` 走了另一条判定支路。
3. **`clover-mac` 里是别的策略名（Sandbox 等）** —— 信任这一关已经过了，拒绝发生在
   策略标签上，按名字去读那个钩子；不再怀疑 bundle。
4. **仍是 `namespace 9 code 0x8`，一条 `clover-mac` 都没有** —— 要么 `printf` 被吞
   （r141 未解释的那件事），要么拒绝不经这两个钩子。先核对菜单标题确实是 `r142`，
   确认刷进 `cache` 的是这一格；然后按 r141 第 4 条的办法改走 logfs 或直接 `panic()`
   带策略名，不再靠 console。

## v0.4.6（round143）：照 sheng 的做法把 SystemOS 卷 union 挂到根上，给 dyld 找回共享缓存

r142 的帧换了名字：`namespace 6 subcode 0xd` 是 `OS_REASON_DYLD`，正文是

```
Library not loaded: /usr/lib/libSystem.B.dylib
  Referenced from: <…> /sbin/launchd
  Reason: tried: '/usr/lib/libSystem.B.dylib' (no such file, no dylib cache)
```

AMFI/MAC 这一关已经过了（r142 里没有任何 `namespace 9` 帧）。dyld 不是被策略挡住，
而是**在根卷上找不到共享缓存**。宿主侧对两个卷的核对：

```
OS      04769D26-2CA0-4825-B443-E2E5F88300F2   /usr/lib/libSystem.B.dylib     不存在
                                             System/Library/Caches/com.apple.dyld/  不存在
SystemOS 8846FE78-2910-4297-AF97-D59BB6F8FC16  usr/ 与 com.apple.dyld/ 都在
```

OS 卷是封印快照，`usr/` 与 dyld 缓存都在旁边的 `Cryptex1,SystemOS` 卷上。sheng 在同一段
路上就是这么过的：`patches/xnu/offline-owner-systemos-mount.patch` 在
`load_init_program()` 里、exec init 之前调 `native_owner_cache_prepare()`，把 SystemOS
卷以 `MNT_RDONLY|MNT_UNION` 挂到 `/`：

1. 先验证根：`vfs_flags(root_mount) & MNT_RDONLY` 且
   `native_verified_block_root_authorized(root_device)`（UUID `8846FE78…` 也写死在这份补丁里）；
2. 用 `native_verified_block_volume_device(uuid, &selected, path, size)` 取卷的 `dev_t` 与
   `/dev/…` 路径，`mount()` 之后复查 `actual == selected`、`f_fstypename == "apfs"`、
   `vnode_mountedhere(original_os_root) == cache_mount && rootvnode == mounted_root`；
3. 成功后 `dyld_flags |= 8; commpage_update_dyld_flags(dyld_flags)`，让 dyld 认这份共享缓存。

失败一律 `panic("offline owner SystemOS mount failed: %d")`。

基础树里这份补丁**已经应用但被编译掉**：`scripts/build-xnu-4k-baseline.sh` 给
`kern_exec.o` 传 `-DXNU_OWNER_CACHE_MOUNT=0 -DXNU_OWNER_CACHE_STARTUP=0
-DXNU_OWNER_UNION_SYSTEMOS=0`，并且 OWNER_* 的开关门会连带要求 VERIFIED_ROOT_PROBE 和
`NativeVirtioBlock`（clover 没有 virtio 块设备，会重复定义
`native_verified_block_root_authorized`）。所以这一轮**不动 defconfig**，板级补丁
`patches/xnu-clover/zzy-clover-systemos-mount.patch` 在 `kern_exec.c` 顶部
`#undef` 再 `#define` 这三个宏为 1——`kern_exec.c` 是唯一读它们的编译单元，命令行的
`=0` 因此被覆盖。补丁按名字排序落在 `zzz-boot-progress.patch` 之前。

`drivers/CloverBlock` 这一轮把「两个窗口」做出来（r142 只发布 OS 窗口）：

- 从 SHNGROOT 清单里一次读出 `OS` 与 `SystemOS` 两个窗口，`clover-apfs=3` 时缺任何一个都在
  清单校验处 panic；
- 先 `publishWindow(OS)` → 等 apfs 卷命名、闩住 `gOsVolumeDev`，再发布 `SystemOS` 窗口并
  闩住 `gSystemVolumeDev`/`gSystemVolumeMedia`。顺序固定，是因为 `CloverSdhci` 的 PIO
  单调用者卫兵不允许两个窗口的请求重叠；
- 导出 `native_verified_block_root_authorized(dev_t)` 与
  `native_verified_block_volume_device(uuid, dev_t *, char *, size_t)`，后者只认
  `8846FE78…`，与内核侧补丁的契约逐字段对齐。两个符号在 `kernel.macho` 里都已解析
  （`native_verified_block_root_authorized` @ `fffffff007994a48`）。

两个 checker 都加了 r143 契约：`check-clover.py` 要求板级补丁存在且三个宏都是
`#undef` + `#define … 1`，`[userdata] windows` 必须恰好是 `OS` / `SystemOS`（后者
`component = Cryptex1,SystemOS`）；`verify-clover-payload.py` 要求卷内内核同时带
`CloverBlock r143: os|system vu=…`、`windows os=… system=…`、两个 owner 导出，以及
补丁独有的 `offline owner SystemOS mount failed: %d`——最后这条是「补丁真的被编进来」
的唯一直接证据。

### 构建、校验与本轮该刷哪一格

```
# 内核必须重建：kern_exec.c 被板级补丁改过，libCloverBlock.a 多了两个导出
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

产物归档在 `~/clover-out/round143-*`，`shasum -a 256 -c round143-SHA256SUMS` 八项全 OK：
`payload.img` 58,720,256 B（`6ec7f421…`）、本地 `kernel.macho` `64ffdf08…`（r142 是
`c241d778…`，本轮重建）、卷内 `kernel.macho` 51,994,624 B（`cbebc08e…`）、卷内
`unixnu.toml` 626 B（`72f78109…`，args 与 r142 逐字相同）、ADT 13,516 B（`15d0436e…`）、
`scratch-data.img` 35,888 B（`614b8356…`）、trustcache 61,408 B（`7c369ce0…`，未变）。
两个 checker 全绿；`userdata` 清单里两个窗口早已写好，**本轮没有新的 eMMC 分区写入**。

**刷写范围仍然只有 `cache`**：`fastboot flash cache
/Users/zhouzhou/clover-out/round143-payload.img`，然后 `fastboot reboot` 冷启动。
菜单第一行应当是 `clover r143: union-mount SystemOS over the OS root for dyld`。

这一帧只问一个问题：**共享缓存挂上之后，launchd 能不能被 dyld 装起来。**

1. **`/sbin/launchd` 起来并继续往下走** —— dyld 这一关过了；下一轮读 launchd 在用户态
   停在哪（缺 Data 卷、缺服务、缺文件系统配置），不再动缓存与挂载。
2. **`offline owner SystemOS mount failed: N`** —— 挂载本身失败，N 是 `mount()` 的 errno
   或补丁内部的 EAUTH。先对 N 去 `kern_return`/errno 表里定位；若为 EAUTH（`native_verified_*`
   返回 0），说明 `CloverBlock` 闩住的卷 `dev_t` 与内核侧取到的不一致（窗口顺序或卷命名）。
3. **还是一条 `namespace 6 subcode 0xd`，但这次看得到 `CloverBlock r143: os/system vu=…`**
   —— 两个窗口都发布了，挂载却没改变 dyld 的结果；查 union 是否真的生效
   （`mnt_flag` 里 `MNT_UNION`、`vnode_mountedhere(root)`），以及 `dyld_flags` 是否被
   commpage 更新。
4. **没有 r143 行、仍是旧名字** —— 先核对菜单标题确实是 `r143`，确认刷进 `cache`
   的是这一格，再往补丁是否被 `kern_exec.o` 的编译命令覆盖方向查。

## v0.4.7（round144）：先问清楚根挂载表为什么是空的

r143 的帧不是 dyld，也不是挂载失败，而是 `_bsd_init+0x79c` 的 data abort：

```
ESR 0x96000004 (translation fault level 0, read)   FAR 0x70
pc  fffffff0076ac00c   ldr w8,[x20,#0x70]
x20 0                  <-- mountlist.tqh_first
```

那一行就是 `mountlist.tqh_first->mnt_flag |= MNT_ROOTFS;`，紧跟在
`IOSecureBSDRoot(rootdevice)` 之后；而 `vfs_mountroot()` 刚刚返回 0（否则
`bsd_init()` 的 `while (TRUE)` 不会 `break`）。静态检查把这个矛盾夹死在一个很窄的范围里：

- `vfs_mountroot()` 的 `return 0` 只有三处（`mountroot` 钩子路径、
  MACF 的 `MNT_MULTILABEL` 提前返回、正常收尾），三处都在内联的
  `mount_list_add()` 之后；唯一的跳过条件是 `get_system_inshutdown()`；
- 这个标志只由 `reboot_kernel()` 置 1，而它唯一的非 panic 调用者是 `reboot(2)`；
- 从挂载返回到 `bsd_init()` 解引用之间，没有 `dounmount()` / `vfs_unmountall()` 的调用者；
  `IOSecureBSDRoot` 的失败分支只调 `mdevremoveall()`，动的只是 devfs 的 md 节点；
- r142 和 r143 的 `_bsd_init` / `_vfs_mountroot` 字节相同，不是编译差异。

所以剩下的是运行时事实，而不是再读一遍源码能回答的。这一轮不单独刷探针，
把读数直接嵌进这次启动：

1. 内核侧 `bsd/vfs/vfs_subr.c` 在四个出口各记一次快照
   （tag 0/1/2 = 三处 return 0，tag 3 = `return ENODEV`），内容为
   `mountroot` 指针、`mountlist.tqh_first`、`nummounts`、`get_system_inshutdown()`、`rootdev`；
2. `bsd/kern/bsd_init.c` 在同样的位置先看表：若为空，把快照取走，再调一次
   `vfs_mountroot()`；仍然为空才 `panic`，并把两次的 tag/快照一起打出来；
   重试成功则打一行 `clover r144: root mount recovered on retry …` 继续走
   `MNT_ROOTFS` 与 `VFS_ROOT`。

改动落在基础仓的 `patches/xnu/zzy-clover-root-mount-why.patch`（契约与 r143 的板级补丁
同形：`check-clover.py` 要求四个 tag 与两条 r144 字符串都在补丁里，
`verify-clover-payload.py` 要求卷内内核确实带着这两条字符串——后者是「补丁真的编进来」的
唯一直接证据）。两个 checker 全绿。

### 构建、校验与本轮该刷哪一格

```
# 内核必须重建：vfs_subr.c 与 bsd_init.c 都被新补丁改过（本轮 drivers 未动）
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

产物归档在 `~/clover-out/round144-*`，`shasum -a 256 -c round144-SHA256SUMS` 七项全 OK：
`payload.img` 58,720,256 B（`cecaf4e5…`）、本地 `kernel.macho` 51,998,720 B（`afefcb3d…`）、
卷内 `kernel.macho`（`9ad76c4b…`，与本地只差 entry 探针补丁）、卷内 `unixnu.toml` 608 B
（`36ded5b3…`，args 与 r143 逐字相同，只有 title 换成 r144）、ADT 13,516 B（`d471ed6b…`，未变）、
`scratch-data.img` 35,888 B（`614b8356…`，未变）、trustcache 61,408 B（`7c369ce0…`，未变）。
**本轮没有新的 eMMC 分区写入**，刷写范围仍然只有 `cache`。

```
fastboot flash cache /Users/zhouzhou/clover-out/round144-payload.img
fastboot reboot
```

菜单第一行应当是 `clover r144: ask why the root mount list was empty`，冷启动
（不要 `fastboot boot`）。

这一帧只问一个问题：**空表现在有没有第二次机会，以及第一次是哪个出口留下的。**

1. **`clover r144: root mount recovered on retry p1tag=… p2err=0 first=…`** ——
   重试把卷挂上了，启动继续过 `MNT_ROOTFS`/`VFS_ROOT`。`p1tag` 就是第一次成功返回
   的那个出口，下一轮按它把「第一次为什么没进表」修掉（而不是靠重试兜底）。
2. **仍然是 `_bsd_init+0x79c` 的 data abort** —— 补丁没进内核。先核对菜单标题是 `r144`，
   再跑 `verify-clover-payload.py` 看两条 r144 字符串是否在卷内内核里。
3. **`clover r144: mountlist empty after vfs_mountroot()==0 p1 tag=… mr=… first=… n=… inshut=… dev=… p2 err=… tag=… first=… n=…`** ——
   按字段读：
   - `tag=0`：走的是预选 `mountroot` 钩子（`mr=` 是它的地址；全 0 表示没有钩子，那就不该是 0）；
   - `tag=1`：MACF 的 `MNT_MULTILABEL` 提前返回；`tag=2`：正常收尾。两者都说明
     `vfs_mountroot()` 自己认为挂上了；
   - `first=0 n=0 inshut=0` 而 `tag=1/2`：入表那一步被跳过或表被清空，`inshut` 为 0
     排除了关机标志，下一轮在 `mount_list_add()` 前后各加一个记录点；
   - `first` 非 0 而第二次仍空：有东西在返回后把节点摘了，按 `dev` 去查 `dounmount` 路径；
   - `p2 err` 非 0：重试自己失败（`err` 是 errno），空表只是它的后果；
   - `inshut=1`：`system_inshutdown` 被置上了，回头查是谁在根挂载前调了 `reboot(2)`。

## v0.4.8（round145）：把 SystemOS 窗口挪到 sheng 的时刻——exec 时再发布

r143 和 r144 是同一个回归的两次复现：只要启动时（`clover_block_run()` 的
`clover-apfs=3` 分支）把 userdata 的第二个窗口也发布出去，`bsd_init()` 就会在
`mountlist.tqh_first->mnt_flag |= MNT_ROOTFS` 上以空表 fault；而 r137–r142 只发布
OS 那一个窗口，同一颗根卷每次都挂上了。这已经不是「哪一行代码写错」，而是
**发布时刻**的问题：r143 让第二颗 APFS 在根挂载前就 attach 完成（它的卷节点、
BSD 名和 devfs 节点都先于根出现），根挂载紧接着在这套已经变化过的 IOKit 树上跑。

r144 读数把这件事进一步夹窄，也留下一个矛盾：帧里 `p1 tag=3`、`mr=0`、`first=0`、
`n=0`、`inshut=0`、`p2 err=19`（`ENODEV`）——第二次调用自己就没挂上，可第一次
那一轮却带着 `tag=3`（`vfstable` 循环走完、`return ENODEV`），和「返回 0 后表却是
空的」对不上。这说明 r144 的单张快照抓到的可能不是 `bsd_init()` 里那一次调用，
而是同一轮启动里另一次（`imageboot` 的两处调用也在 `bsd_init()` 里）。单张快照
无法区分，所以 round145 顺便把探针升级成**每次调用一行历史**，而不是再猜一遍源码。

sheng 的顺序不是这样。sheng 的 `NativeVirtioBlock::native_verified_block_volume_device`
在 **exec 时**才 `IOService::serviceMatching("IOMedia")` + `IOPropertyMatch/UUID`
去遍历注册表，把 SystemOS 卷找出来交给 `bsd/kern/kern_exec.c` 的 owner mount；
启动阶段只挂根。clover 的镜像把 SystemOS 放在 userdata 的第二个窗口而不是启动盘的
分区里，所以「发布窗口」这一步仍然只能是板级代码，但**可以放在 sheng 的同一时刻**：
根挂载之后、launchd exec 时，由 owner 契约的查找顺手发布。

这一轮因此不新造流程，只把 r143 那一步从 boot 移到 sheng 本来就在的地方：

1. `drivers/CloverBlock/CloverBlock.cpp`
   - `publishSystemOs()`：从 `gSystemWindowFirst/gSystemWindowSectors` 读出窗口，
     调 `publishWindow(first, sectors, /*writeProtected=*/true, …)`，用
     `findVolumeByUuid(whole, kSystemOsVolumeUuid, 0)` 等卷出现（12000 × 10 ms），
     再等 `volumeBsd()` 给出 BSD 名，记下 `gSystemVolumeDev` / `gSystemVolumeMedia`。
     媒体被 retain，因为 owner mount 会在这之后很久还读它。
   - `native_verified_block_volume_device()`：进入后先 `publishSystemOs()`；
     之后仍是 sheng 的形状——按 UUID 校验、`candidate != gSystemVolumeDev` 拒绝、
     `native_verified_block_root_authorized(candidate)` 校验、给出 `/dev/<名>`。
   - `clover-apfs=3` 的启动分支只发布 OS 窗口（`publishWindow(osFirst, osSectors, true, …)`），
     读完两个窗口的几何并做上界检查后就返回，**不再调用 `prepareSystemOs()`**；
     日志改为 `CloverBlock r145: boot os=…+… system=…+… fires=… deferred=1`。
     清单里没有 SystemOS 时仍会在根挂载前 `fail("systemwin")`。
2. 板级内核补丁 `zzy-clover-root-mount-why.patch`（r144 的探针升级，不是新机制）
   - `vfs_subr.c` 每次进 `clover_root_probe()` 写一格 8 项环形历史：
     `clover_root_phase`（1 循环、2 重试、3/4 `imageboot`、10 快照点）、tag、`rootdev`、
     `nummounts`、`mountlist.tqh_first`；并给 `mount_list_add()` / `mount_list_remove()`
     各加一个计数（`clover_mount_adds/removes`）。
   - `bsd_init.c` 在快照点先 `clover_root_dump()` 把整圈历史打出来，再重试、再 dump，
     仍为空才 panic；两条 panic/恢复串与 r144 同形，但多了 `hist`/`call` 两组行。
     `imageboot.c` 两处 `vfs_mountroot()` 前各写一个 phase（3/4），让历史能指出是谁调的。

行为上的预期：启动日志里 `boot os=… system=… deferred=1` 之后**不应**再看到
`system vu=`；根挂上、`launchd` exec 时才会出现 `CloverBlock r145: system vu=… bsd=…`
与 `CloverBlock r145: owner system … dev=…`，随后是 sheng 的
`offline owner SystemOS mount`（`MNT_RDONLY|MNT_UNION` 叠到 `/`）与
`dyld_flags |= 8`。

### 构建、校验与本轮该刷哪一格

```
# CloverBlock 与内核补丁都变了，kernel + payload 一起重建
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

产物归档在 `~/clover-out/round145-*`，`shasum -a 256 -c round145-SHA256SUMS` 七项全 OK：
`payload.img` 58,720,256 B（`435db7f1…`）、本地 `kernel.macho` 51,998,720 B（`5e3a1027…`）、
卷内 `kernel.macho`（`fde13a88…`，与本地只差 entry 探针补丁）、卷内 `unixnu.toml` 617 B
（`8ff23b3d…`，args 与 r144 逐字相同，只有 title 换成 r145）、ADT 13,516 B
（`388a7f7b…`；其中 `chosen/random-seed` 每次构建随机，所以与 r144 的 `d471ed6b…`
不同，其余属性未变）、`scratch-data.img` 35,888 B（`614b8356…`，未变）、
trustcache 61,408 B（`7c369ce0…`，未变）。**本轮没有新的 eMMC 分区写入**，
刷写范围仍然只有 `cache`。

```
fastboot flash cache /Users/zhouzhou/clover-out/round145-payload.img
fastboot reboot
```

菜单第一行应当是 `clover r145: SystemOS window moves to exec time, like sheng`，
冷启动（不要 `fastboot boot`）。

这一帧只问一个问题：**去掉启动时的第二窗口之后，根挂载是否回到 r137–r142 的样子，
以及 exec 时的 owner 查找能否把 SystemOS 挂上。**

1. **根挂上、日志出现 `system vu=…` 与 `owner system … dev=…`，然后停在 dyld/共享缓存** ——
   发布时刻就是回归原因，上一轮的空表只是 r143 的副作用；下一轮按 sheng 的
   `dyld_flags`/共享缓存路径继续。
2. **根挂上，但 exec 时没有 `system vu=`（或 `systemvol`/`systembsd` 的 fail 行）** ——
   窗口几何或 UUID 没进 manifest，按 fail 的 stage 名查 SHNGROOT 里的 `SystemOS`。
3. **仍然是空表 panic（`clover r145: mountlist empty after vfs_mountroot()==0 …`）** ——
   这次历史行会指出调用者：`ph=1` 是 `bsd_init()` 的循环、`ph=2` 是重试、`ph=3/4`
   是 `imageboot`。同时看 `add=`/`del=`：`add=0` 说明根本没入表（`tag` 指向那条出口），
   `add>0 del>0` 说明入表后又被摘掉，`n`/`first` 与 `add` 之差就是被摘掉的节点数。
4. **仍然是 `_bsd_init+0x79c` 的 data abort 且没有任何 `clover r145` 行** ——
   补丁没进内核；先核对菜单标题是 `r145`，再跑 `verify-clover-payload.py`。

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
  根设备从 `v0.4.0` 起交回内核：`/chosen/root-matching` 指向 eMMC 上那颗未封存 OS 卷的
  UUID，`CloverBlock` 只把卷发布出来并等 BSD 名，`IOFindBSDRoot()` + `vfs_mountroot()`
  走标准路径去挂；round120 删过 `root-matching`，round127 把根改回 `rd=md0`，都是为了让
  探针能跑。`v0.4.1` 起 `kernel_args` 里保留一个 `rd=md0s39`，但它不是根声明：四字符
  进不了 `IOFindBSDRoot` 的根选择（`matching` 优先、md 捷径要求三字符），只让
  `bsd_rooted_ramdisk()` 读到 `md0` 前缀而跳过 ROOTAUTH。`v0.4.2` 起再加
  `cs_enforcement_disable=1`：r138 的 launchd SIGKILL 是 AMFI 的 label-update 钩子
  （`EXEC_EXIT_REASON_SECURITY_POLICY`，无描述），而这个参数是 XNU 的 CS 核心与
  AMFI kext 自己都会读的唯一开关。`v0.4.3` 起 ADT 再发布
  `/chosen/debug-enabled = 1`：r139 的帧证明 AMFI 在 `start()` 里先认这个参数、随后
  用 `PE_i_can_has_debugger()` 做前置检查，返回假就 panic
  （`can't has cs_enforcement_disable` @AppleMobileFileIntegrity.cpp:5261），而这个
  函数的值只由这个属性写入（`pexpert/arm/pe_init.c:467-481`）。payload 里仍带
  `scratch-data.img` 作为 `initrd`，内核会把它挂成 `md0`，但它不再是根。
  round119-121 试过把根指向 eMMC 上的 `disk0s39`/`md0s39`，round127-136 的探针证明
  「卷发布、身份、几何、读路径都正常，唯独本驱动手工 `VFS_MOUNT` 一律回 13」，
  round137 因此改用标准挂载入口。
- **没有 GPU。** `NativeAdreno` 面向 Adreno 740；这块板子是 Adreno 512。
  所以显示栈缺席，`scripts/build-clover-display-stub.py` 只提供
  DEVELOPMENT-only XNU 补丁无条件调用的那一个空符号。首次启动的目标是 UEFI
  菜单和 XNU 控制台，不是 framebuffer。
- **没有触摸。** `edt-ft5406` 没有 UniXNU 驱动，USB 又只有 device 模式，
  所以在有人补上之前，菜单输入只能走调试串口（`blsp1_uart2`，115200）。
- **没有 Data 卷。** `[payload.adt]` 现在是 `packed_data_ramdisk = false`：本机没有
  `NativeRamBlock`，`--data-ramdisk` 后面那颗镜像不是内核要写的 Data 种子，而是
  `md0`/mockfs 的根（`scripts/make-clover-root.py`）。`/chosen` 因此保持普通
  main boot，不加 `ephemeral-storage`，也不加 `PackedRAMDisk` 槽。启动信任缓存是
  `scripts/make-clover-trust-bundle.py` 拼的三模块 bundle（clover owner 模块 +
  IPSW 的 `StaticTrustCache` + `Cryptex1,SystemTrustCache` 原始模块），
  `trust_cache_sha256` 钉住合并后的整包。

## v0.4.9（round146）：共享缓存那一问——可见吗、谁来挂的、内核怎么答

r145 的帧把根的问题结掉了：根挂上了（`disk1s1`，UUID
`04769D26-2CA0-4825-B443-E2E5F88300F2`），SystemOS 在 exec 时发布
（`CloverBlock r145: system vu=8D4FE78D-… bsd=disk3s1`），owner 契约把它找了出来
（`owner system disk3s1 dev=16777223`），`SystemCryptex` 由 init 挂上，launchd 的
exec 过了两道 MAC（`label-check AMFI -> 1`、`Sandbox -> 1`）。然后帧里只剩两行

```
shared_region: … [1(launchd)] check_np(0x…) vm_shared_region_start_address() failed
```

没有 panic，也没有 `offline owner SystemOS mount failed`。这两行不是错误：共享区此刻
是空的，`shared_region_check_np()` 就是在这条路上把 `ENOMEM` 交给 dyld 的——**空区是
dyld 期待的第一种答案**，它接下来应该调 `_shared_region_map_and_slide()` 去挂共享缓存。
帧到这里就没了，于是有三个可能挤在一起：缓存文件在 union 过的 `/` 下面根本看不见；
dyld 看见了却没走到 map；或者走到了，内核在某个门里回了错，而那一行没被看见。
这一轮只把这三个可能拆开，不加任何新机制。

### 探针放在 `vm_unix.c`，不放在 `kern_exec.c`

原本想在 owner mount 之后顺手问一次缓存路径，但那是 `kern_exec.c` 里
`offline-owner-systemos-mount.patch` **新增块**的内部。构建脚本会先反向检查、
再按名字顺序应用 `patches/xnu/*.patch`；往一个更早补丁的新增行里再插探针，
那个补丁的 `git apply --reverse --check` 就不再原子，第一次 r146 构建正是这样
在 `kern_exec.c:183` 失败的（详见提交信息）。所以这一轮的五个探针全部落在
`bsd/vm/vm_unix.c`：这个文件只被本轮的新板级补丁
`zzy-clover-shared-region-why.patch` 碰，不破坏任何既有补丁的反向检查。
`zzy-clover-systemos-mount.patch` 因此保持 r143 的 25 行原样，一行未动。

### 五个打印各自回答什么

1. `clover r146: check err=%d start=%llx` —— `check_np` 的返回值。12（`ENOMEM`）
   是「有共享区但空」，22（`EINVAL`）是「连共享区都没有」，0 且 `start` 非零则是
   「已经挂好了」。r145 的两行对应 12。
2. `clover r146: cache err=%d size=%lld mnt=%p rmnt=%p fl=0x%llx rfl=0x%llx` ——
   在 dyld 做决定的**同一时刻**，用它将用的同一条绝对路径
   `/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e` 做一次
   `vnode_lookup()`，再取文件大小和它背后的挂载点。
   - `err=0`、`size` 是几十 MB 量级 → 缓存透过 union 可见。
   - `err=2`（`ENOENT`）→ 上层（OS 快照）没有这个文件，union 没把 SystemOS 的内容
     叠上来，或者在错的位置上。
   - `mnt == rmnt` 且 `fl & 0x21 == 0x21` → 文件确实来自叠在 `/` 上的那次
     `MNT_RDONLY|MNT_UNION`（`MNT_RDONLY=0x1`、`MNT_UNION=0x20`，
     `bsd/sys/mount.h`），也就是 sheng 想要的形状。
   - `mnt != rmnt` → 名字解析落在了 SystemOS 自己的挂载点上，不是 `/` 的那一层；
     非 CSR 的 `srfmp->vp->v_mount != rdir_vp->v_mount` 门随后会拒绝它
     （`bsd/vm/vm_unix.c` 里那句 trace 是 `not on process' root volume nor preboot
     volume`），除非 owner carrier 判定通过。
3. `clover r146: map enter files=%u maps=%u` —— dyld 是否真的跟进了 map 调用。
   它不出现，说明 dyld 在 `check_np` 之后改走了 `mmap()` 路径或直接放弃，
   下一轮该查 dyld 的分支选择，而不是内核的门。
4. `clover r146: map setup err=%d` —— setup 的 errno。此时它上面那行
   `SHARED_REGION_TRACE_ERROR` 会指出是哪道门（`shared_region_trace_level`
   默认就是 error 级，`osfmk/vm/vm_shared_region.c:140`），常见的是 1（`EPERM`，
   「不在进程根卷也不在 preboot 卷」或 `scdir` 目录门）和 22（`EINVAL`，参数形状
   不对）。本打印只把 errno 钉在行上，省得从 trace 的措辞倒推。
5. `clover r146: map exit err=%d kr=0x%x` —— map 的最终结果。
   `err=0 kr=0` 表示缓存已映射、共享区不再为空，墙已经移到这次调用之后
   （下一个要问的就是 dyld 在那之后停在哪）；非零时 `kr` 是 `vm_shared_region_map_file()`
   的原始 `kern_return_t`，和 errno 的换算关系在同一函数里（`KERN_NO_SPACE` → 12 等）。

### 构建、校验与本轮该刷哪一格

```
# 只有内核补丁变了；payload 仍要重建，因为它在卷内放一份改过的内核
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

两个 checker 都增加了本轮契约：`check-clover.py` 要求板级补丁包含五个
`clover r146:` 串，`verify-clover-payload.py` 在卷内 `kernel.macho` 里逐条找五个
格式串（`kernel.macho: shared-region probe linked OK`），少一条就 FAIL——否则那一半
问题会以「没有输出」的形式回来。

产物归档在 `~/clover-out/round146-*`，`shasum -a 256 -c round146-SHA256SUMS` 七项全 OK：
`payload.img` 58,720,256 B（`dc0285b5…`）、本地 `kernel.macho` 51,998,720 B（`a9197a0e…`）、
卷内 `kernel.macho`（`e39ec24a…`，与本地只差 entry 探针补丁）、卷内 `unixnu.toml` 608 B
（`3c43e756…`，args 与 r145 逐字相同，只有 title 换成 r146）、ADT 13,516 B
（`ce429eb9…`；其中 `chosen/random-seed` 每次构建随机，其余属性未变）、
`scratch-data.img` 35,888 B（`614b8356…`，未变）、trustcache 61,408 B
（`7c369ce0…`，未变）。**本轮没有新的 eMMC 分区写入**，刷写范围仍然只有 `cache`。

```
fastboot flash cache /Users/zhouzhou/clover-out/round146-payload.img
fastboot reboot
```

菜单第一行应当是 `clover r146: ask the shared cache what it answered`，
冷启动（不要 `fastboot boot`）。

这一帧只问一个问题：**空共享区之后，dyld 看到的缓存文件是谁挂的，它有没有去 map，
内核怎么答。**

1. **`cache err=0`、`mnt == rmnt`、`fl` 带 `0x21`，且出现 `map enter`** ——
   union 是对的，问题在 map 的门或参数；按 `map setup err` / `map exit err` 的数值
   对照上面的表，下一轮直接修那一处。
2. **`cache err=2` 或 `mnt != rmnt`** —— 共享缓存在 `/` 下面不可见（或来自错误的挂载点），
   SystemOS 的 union 还没覆盖到 `System/Library/Caches`；下一轮查 union 的层次与
   目录门的授权，不再碰 dyld。
3. **`cache err=0` 但只有 `check` 行、没有 `map enter`** —— 内核这边缓存是好的，
   是 dyld 在空区之后没走 map；下一轮转到 dyld 的共享缓存选择逻辑。
4. **`map exit err=0 kr=0`** —— 缓存已映射，r145 的墙过去了；下一帧要问的是 dyld
   在拿到共享区之后停在哪，这一轮的五个打印就不再是主线。

## v0.5.0（round147）：无人值守——墙上的死线、IMEM 里的原因、复位进 fastboot、宿主读卡

r146 之前，每一轮都要有人守着：等屏、拍照、长按电源、再来一次。这一轮把「守」
拆成三段可以各自检查的代码：

1. **内核里的死线**（`drivers/ArmCpu`）。boot-arg `clover-unattended=<秒>`，
   从第一个 timer tick 起按 CNTVCT/CNTFRQ 计墙钟；到点就 `panic("clover r147:
   unattended deadline …")`。它替换了 r128 起用的 `armcpu-clock-panic=180000`：
   那个数的是**中断次数**，只有在这颗板子的时钟周期刚好和预期一致时才是时间；
   而无人值守要的承诺是「N 秒没有进展」，所以这一轮改数寄存器。
   180 秒是本轮给的值：比 r145 那帧从 exec 到共享区报错长得多，又短到一帧
   冷启动加拍照来得及。
2. **panic 屏记因并复位**（`scripts/xnu-panic-screen.c`）。以前 panic 屏画完就
   停在那里等人；现在它先把文本写进 logfs（kind 20，r123 起就有），然后向
   IMEM 的 restart-reason 单元写 `0x77665501`（”bootloader”），读回确认，
   等 20 秒走 PSCI `SYSTEM_RESET`。XBL 在下次启动时看到这个值，直接进 fastboot。
   屏上多一行 `fb=1`（或 `0`）——这就是「原因到底写没写进去」的读数。
3. **宿主脚本**（`scripts/run-clover-unattended.sh` + `scripts/read-clover-log.py`）。
   等 fastboot、按需冷刷 cache、请求 recovery、等 TWRP 的 adb、从
   `/dev/block/by-name/logfs`（带两个后备路径）读前 64 个扇区，按 16 个槽解码。

### 为什么记因用 IMEM，不用 `misc`

restart reason 在 clover.dtb 里是 `qcom,msm-imem@146bf000` 下的
`restart_reason@65c`：物理 `0x146bf65c`，一个 32 位单元。IMEM 是电源域内的
SRAM，**复位不清、掉电清**。写它既不是 eMMC 写入，也不需要 `misc` 的 BCB；
电源键长按断电后就自然消失，没有副作用要善后。

`drivers/CloverSdhci` 在这一轮多做一件事：`clover_unattended_begin()` 在
CloverPMGR 发布完 nub 之后、eMMC 探测之前调用一次，把 IMEM 映射进来，先写
`0x77665501` 再读回，**只有读回一致才把单元地址发布给 panic 屏**
（`_clover_fastboot_reason`）。这样 panic 屏不必自己再去 map 一块它不认识的
物理内存，也顺手在正常启动的 IOLog 里留下一行
`clover r147: unattended mark 77665501 -> 77665501 deadline=180s`。

代价要说清楚：**开了 `clover-unattended` 之后，原因在启动早期就写上了**，
所以这一次启动如果成功，之后任何一次重启（包括正常 `reboot`）也会落进
fastboot。这本就是无人值守想要的——下一轮还要读卡——但如果这轮要正常用机，
把 boot-arg 去掉即可；不带这个参数的 panic 屏仍然只画屏、写 logfs，20 秒后
照常复位（原因没写，XBL 走正常启动）。

### 死线为什么放在 tick 里，而不是单独一个线程

clover 没有 Gunyah，也没有可用的 SError/NMI 通道；能定时执行的内核上下文只有
timer 中断。把死线挂在 tick 上意味着它和现有的 clock watchdog 用同一个观察点：
只要这行 panic 出现了，至少有一个核还在收中断、内核还能 panic。反过来，
**屏蔽了 IRQ 的上下文同时逃过两者**——没有 tick，也没有死线。所以主路径不是
这个定时器，而是 panic 屏 + 宿主端的 fastboot 监视：真的完全静默时，脚本的
`--wait` 到点退出，人再断电重启，IMEM 里的原因已经没了，走正常启动；下一次
`panic` 仍会被屏抓住。

### 宿主脚本怎么走

```
# 平板自己从 panic 复位进 fastboot 后：
cd ~/UniXNU-clover
scripts/run-clover-unattended.sh --allow-misc-bcb
```

它按顺序做这些事，每一步都在 stderr 说话：

1. `fastboot devices` 轮询（默认 900 秒）；已有一个 adb 设备时按「已经在 TWRP」
   处理，跳到第 4 步。
2. `--flash-cache IMG` 可选：冷刷 cache 分区。这是**一次新的 eMMC 分区写入**，
   脚本会在执行前把目标分区、镜像和大小打印出来；不加这个选项就不写。
3. `fastboot reboot recovery`。这条命令可能把启动命令写进 `misc` 分区的 BCB，
   所以要有 `--allow-misc-bcb`；没有这个参数时脚本只打印说明并退出 3，
   不发命令。bootloader 拒绝时给出两条手动后备：断电清 IMEM 后按音量下进
   fastboot，或者先进 TWRP 再用 `--skip-recovery`。
4. 等 adb（默认 180 秒），确认 `ro.twrp.version` / `ro.product.device`。
5. 依次尝试 `/dev/block/bootdevice/by-name/logfs`、`/dev/block/by-name/logfs`、
   `/dev/block/mmcblk0p39`，用 `adb exec-out dd bs=512 count=64` 取 32 KiB，
   校验长度和 `UXNU`/`LOG1` 魔数，落盘到 `~/clover-out/clover-logfs.bin`。
6. 调 `read-clover-log.py --count` 解码；每个槽打印 kind、长度、校验和是否
   对得上，正文缩进展开。退出码 0 = 读到了记录，1 = 存了 dump 但没有记录，
   2 = 没等到 fastboot，3 = 缺 BCB 同意，4 = bootloader 拒绝 recovery，
   5 = recovery 里没有 adb。

解码器本身不依赖设备：给一个 32 KiB 的 dump 就能在宿主上读，半写的记录
（头对、正文和不符）也会打出来——半张 panic 也是读数。整个脚本**没有一处
`fastboot boot`**，每次启动镜像都是冷启动，`check-clover.py` 逐行断言这一点。

### 构建、校验与本轮该刷哪一格

```
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

两个 checker 都增加了本轮契约：`check-clover.py` 断言 boot-arg 是
`clover-unattended=180`、旧参数不在了、标题是 r147，并逐条检查驱动里的 IMEM
常量、`PE_parse_boot_argn("clover-unattended")`、三行 IOLog、panic 屏的
`FASTBOOT_WORD`/`fastboot_mark`/`fb=`/`reset_after`、补丁的
`-DFASTBOOT_REASON`，以及宿主脚本的 BCB 门和两个日志路径；
`verify-clover-payload.py` 在卷内 `kernel.macho` 里找五条格式串
（`kernel.macho: unattended watchdog linked OK`）。这一条不是形式：patcher 现在把
`_clover_fastboot_reason` 当成**必需符号**，缺失就构建失败（r147 之前是可选，
静默编进 `FASTBOOT_REASON=0`，`fb=` 永远 0，和「板子拒绝记因」同形——多花一次刷机
才分得开）。所以这份归档案存在，就说明指针在链接期解析到了。

产物归档在 `~/clover-out/round147-*`，`shasum -a 256 -c round147-SHA256SUMS`
七项全 OK：`payload.img` 58,720,256 B（`3cf89255…`）、本地 `kernel.macho`
51,998,720 B（`a8a062c7…`）、卷内 `kernel.macho`（`5bc366ce…`，与本地只差
entry 探针补丁，6,838 字节、35 段）、卷内 `unixnu.toml` 611 B（`6abbda6c…`，
args 里 `armcpu-clock-panic` 已换成 `clover-unattended=180`）、ADT 13,516 B
（`503f690a…`；`chosen/random-seed` 每次构建随机，其余未变）、
`scratch-data.img` 35,888 B（`614b8356…`，未变）、trustcache 61,408 B
（`7c369ce0…`，未变）。

**本轮有两处 eMMC 写入，各自需要同意：**

1. 刷 payload 到 `cache`（与之前每一轮相同，手动执行，只覆盖 `cache` 这一格）：

```
fastboot flash cache /Users/zhouzhou/clover-out/round147-payload.img
fastboot reboot
```

2. 宿主脚本里的 `fastboot reboot recovery` 可能写 `misc` 的 BCB，只有带上
`--allow-misc-bcb` 才会发；不带就退出 3，一个字节都不写。

菜单第一行应当是 `clover r147: mark the restart reason, reset into fastboot`，
冷启动（不要 `fastboot boot`）。

这一帧只问一个问题：**这块板子能不能在没人碰它的时候把 panic 变成一次
可读的复位，并让宿主在 fastboot 里接住它。**

1. **屏上出现 `clover r147: unattended deadline …`，`fb=1`，20 秒后黑屏复位，
   宿主 `fastboot devices` 里出现设备** —— 三段都通了；之后 `run-clover-unattended.sh`
   读出的 kind 20 记录就是这一帧的全文，下一轮起可以整夜跑。
2. **出现死线 panic 但 `fb=0`** —— 屏在了，原因没写进去：看 IOLog 里
   `unattended mark … read back …`/`imem map failed` 那行的地址，说明是 map
   还是写回读的问题；这一帧不会自己进 fastboot，但仍然存了 logfs 记录。
3. **没有死线 panic，只有 r146 那种静默** —— 当前上下文屏蔽了 IRQ（或时钟早
   就停了）：定时器这条路走不到，按上面说的断电重来；这本身就把「静默发生在
   屏蔽中断的上下文」钉死了。
4. **panic 屏出现但 20 秒内没复位** —— PSCI 这台固件没接受 `SYSTEM_RESET`；
   下一轮该查 UEFI 侧的 PSCI conduit（`psci-method smc` 已在 ADT 里），
   而不是内核。

## v0.5.1（round148）：分号不是分隔符——汇编器把每条 asm 的后半句当注释吃掉了

r147 刷进去之后，第一帧就是死线 panic，但它出现在**第一个** timer tick 上：

```
clover r147: unattended deadline 180 s (elr=fffffff007968c84 spsr=fffffff208a69d50
 ticks=1 cnt0=124f800 cnt1=fffffff5eeea86f8 freq=124f800) @ArmCpu.cpp:544
```

数字自己就打架：`cnt0` 恰好等于 `freq`（`0x124f800` = 19,200,000 = 1 秒的
计数），`cnt1 - cnt0` 按无符号是天文数字、按有符号读是 −15.9 秒，`spsr` 也
不是一个合法的 PSTATE（`0x600003c5` 那一类）。算术没有错——**读寄存器的那几条
指令根本不在二进制里**。

### 反汇编怎么说

`round147-kernel.macho` 的 `ArmCpu::interrupt`（`0xfffffff007969718`）：

```
969828: mrs  x8, CNTFRQ_EL0
969834: str  x8, [gTickCounterFrequency]
969838: isb
96983c: str  x8, [gTick0Counter]        ; 这里本该是 mrs x8, CNTVCT_EL0
...
969888: isb
96988c: ldp  x10, x8, [gTick0Counter]   ; freq 与 cnt0
969890: sub  x8, x2, x8                 ; now 用的是 x2：函数入口的第三个参数
```

函数入口也少一条：`mrs x9, ELR_EL1` 在，紧跟的 `mrs x8, SPSR_EL1` 不在，
`entrySpsr` 存进去的是未初始化的 x8。整个 `ArmCpu::interrupt` 里
`MRS <Xt>, SPSR_EL1` 的编码一条都没有（镜像里另有一条在 xnu 自己的异常码
里，位置 0x3e64e0，与本函数无关，也是整镜像计数不可用的原因）。

### 根因：Darwin arm64 的 `;` 是注释

clang 把内联汇编的字符串原样交给汇编器，而 **Darwin 的 arm64 汇编器用 `;`
作注释起始符**（GNU/ELF 用 `//`）。于是

```c
__asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r" (value));
```

只汇编了 `isb`：`mrs` 连同它的输出约束一起落进注释，编译器不报错，`value`
只是一个没被写过的寄存器。本机最小复现（Xcode clang，`-O2`）：

| 写法 | 反汇编 |
| --- | --- |
| `"isb; mrs %0, cntvct_el0"` | `isb` |
| `"isb\n\tmrs %0, cntvct_el0"` | `isb`，`mrs x8, CNTVCT_EL0` |
| `"mrs %0, elr_el1; mrs %1, spsr_el1"` | `mrs x8, ELR_EL1` |
| `"mrs %0, elr_el1\n\tmrs %1, spsr_el1"` | `mrs x8, ELR_EL1`，`mrs x9, SPSR_EL1` |

### 影响面：不止看门狗

内核（Darwin 目标）里一共 20 条这样的字符串，全在 `ArmCpu.cpp`。按后果排：

| 位置 | 原意 | 实际执行 |
| --- | --- | --- |
| `counter()` | 读 CNTVCT | 只有 `isb` |
| `interrupt()` 入口 | 读 ELR + SPSR | 只有 `mrs elr` |
| `signalCPU()` | `dsb ishst; msr icc_sgi1r_el1` | 只有 `dsb ishst`——**SGI 从未发出** |
| `armcpu_sgi_selfprobe()` | `mrs daif; msr daifset, #2` | 只有 `mrs`——中断没关 |
| `waitClear()`、GIC 的三处屏障 | 操作后 `isb`/`dsb sy` | 顺序不完整 |

panic 屏里也有 3 处同样的半角分号写法，但它不是内核的一部分：`assemble_c()`
用 `--target=aarch64-none-elf` 编译它，ELF 汇编器把 `;` 当**语句分隔符**，
所以那 3 处（`reset_after` 的等待、`mapped()`/`readable()` 的 PAR 读取）一直
是好的，r147 的 panic 屏也确实正常画出帧、正常等满 20 秒。这一轮仍把它们统一
成 `\n\t`（两种汇编器都接受），但**不算故障**。同一条源码换一个目标就换一套
注释规则，这正是这轮最值得记住的一点。

「`fb=0`、复位后还是回到 payload」也在这里：死线在第一个 tick 就到了，
CloverPMGR 还没跑，`_clover_fastboot_reason` 尚未发布，panic 屏走「没有单元」
的路径，做的是普通复位，XBL 于是又启动了同一个 payload。屏本身工作正常，
缺的是它要写的那个单元。

`@ArmCpu.cpp:544` 这个行号与源码里的 642 不符，但反汇编与当前源码逐条吻合
（ruler、比较、panic 的七个参数），所以编译的就是这份源码；行号怎么来的另说，
不该从它推结论。

### 这一轮改了什么

- `ArmCpu.cpp` 的 20 处全部改成 `\n\t` 分隔；panic 屏 3 处顺手统一（本就没坏）；
  身份串、菜单标题、panic 文案升到 `r148`。
- `scripts/check-clover.py` 新增 `check_inline_asm`：扫 `drivers/**` 的每条内联
  汇编，字符串里只要还有 `;` 就报错并写明原因。只扫 `drivers/` 是因为
  `scripts/xnu-panic-screen.c` 走 ELF 目标，不该被这条规则误伤。现在输出
  `75 inline asm statements separated`。
- `scripts/verify-clover-payload.py` 新增 `check_inline_asm_tails`：从 Mach-O
  符号表定位 `ArmCpu::interrupt` 与 `ArmCpu::signalCPU`，在各自函数体内数编码：
  `MRS <Xt>, SPSR_EL1`（`0xd5384000｜Rt`）、`MRS <Xt>, CNTVCT_EL0`
  （`0xd53be040｜Rt`）、`MSR ICC_SGI1R_EL1, <Xt>`（`0xd518cba0｜Rt`）。字符串
  证明不了指令真的在，编码能；整镜像计数又会被 xnu 自带的同类指令凑够，所以
  必须按符号限定范围。r147 的旧镜像是 0/0/0，FAIL；这一轮是 1/2/1，OK。
- 旁支也一并修好：IPI 现在会真的发出去、DAIF 会真的关、`waitClear` 的超时
  真的按 CNTFRQ 的 10% 计。这些过去都是「写了但没执行」——这一帧如果出现
  从未见过的现象，先查这里。

### 下一帧该看到什么

死线仍然只在「180 秒没有进展」时出现，但这一次数字要能自证：

- `ticks=` 是这 180 秒里收到的 timer 中断次数；
- `cnt1 - cnt0` ≈ 180 × `0x124f800` = `0xCDFE6000` 左右，且 `cnt1 > cnt0`；
- `spsr` 是 `0x600003c5` 一类合法的 PSTATE，不再像内核指针；
- `fb=1`（IMEM 在启动早期已由 CloverSdhci 写好），20 秒后复位进 fastboot；
- 宿主 `run-clover-unattended.sh` 接住设备、请求 recovery、读 logfs。

如果 180 秒内没有 hang，那就什么都不会发生——这本身也是答案：误触发没了。
`elr=` 在那种情况下不出现，比出现一个假死线更有信息量。

### 本轮产物与自检

构建 `~/clover-out/round148-build.log`，退出码 0。两个 checker：

```
[check-clover] clover ok: ADT 13516 bytes, 8 cores; splash 0x9d400000-0x9f800000,
FD 0x9fc00000-0x9ff00000; board clover_defconfig, CLOVERESP;
75 inline asm statements separated; modules visible
kernel.macho: asm tails survived the assembler OK
(mrs SPSR_EL1 in interrupt 1, mrs CNTVCT_EL0 in interrupt 2, msr ICC_SGI1R_EL1 in signalCPU 1)
```

反向对照（同一命令跑 r147 的旧卷）是 `FAIL (… in interrupt 0, … in interrupt 0,
… in signalCPU 0)`，说明这条检查真的会抓住这个 bug，而不是恒绿。

产物在 `~/clover-out/round148-*`，`shasum -a 256 -c round148-SHA256SUMS`
七项全 OK：`payload.img` 58,720,256 B（`b6864724…`）、本地 `kernel.macho`
51,998,720 B（`98571fab…`）、卷内 `kernel.macho`（`2db8a3fd…`，与本地只差
entry 探针补丁，6,838 字节、35 段）、卷内 `unixnu.toml` 605 B（`1fbb3ef8…`，
args 仍是 `clover-unattended=180`）、ADT 13,516 B（`9f64dbb1…`；
`chosen/random-seed` 每次构建随机，其余未变）、`scratch-data.img` 35,888 B
（`614b8356…`，未变）、trustcache 61,408 B（`7c369ce0…`，未变）。

**本轮有一处 eMMC 写入，需要同意：**

刷 payload 到 `cache`（与之前每一轮相同，手动执行，只覆盖 `cache` 这一格）：

```
fastboot flash cache /Users/zhouzhou/clover-out/round148-payload.img
fastboot reboot
```

宿主脚本里的 `fastboot reboot recovery` 可能写 `misc` 的 BCB，只有带上
`--allow-misc-bcb` 才会发；不带就退出 3，一个字节都不写。

菜单第一行应当是 `clover r148: the deadline waits 180 s, not one tick`，
冷启动（不要 `fastboot boot`）。

这一帧仍然只问一个问题：**这块板子能不能在没人碰它的时候把 panic 变成一次
可读的复位，并让宿主在 fastboot 里接住它。** 与 r147 不同的是，这次的窗口
不是第一个 tick，而是真的 180 秒；若屏上再次出现死线 panic，`cnt1-cnt0`
和 `spsr` 必须自洽。

## v0.5.2（round149）：去掉死线——iOS 能自己走多远，由屏回答

r148 的死线是修好了的，但修好的死线仍然是死线：`clover-unattended=180` 让每
一次启动都在 180 秒处结束，不管 iOS 当时正走到哪。这一轮的问题不是「panic 能
不能被接住」，而是**没人打断时它能走到哪**，所以只从 shipped boot 里摘掉这
一个参数。

r148 刷入后的观察（2026-10-26）：22:03–22:09 的几轮 `fastboot devices` /
`adb devices` 都是空的，22:10:48 板子自己出现在 fastboot（`81ffcaaa  fastboot`，
`product: clover`），到 22:11:06 仍稳定。这与 r147 设计的「panic → 写 IMEM
原因 → PSCI 复位 → XBL 落 fastboot」一致；要把它确认为死线 panic 而不是别的
复位，需要进 recovery 读 kind 20 记录，本轮没有做。

### 改了什么：一个参数，一条反向断言

`board.toml` 的 `kernel_args` 删掉 `"clover-unattended=180"`，标题换成
`clover r149: no deadline; the screen is the only stop`。驱动和内核一行没动：
`ArmCpu` 仍然解析这个参数，`CloverSdhci::clover_unattended_begin()` 仍然按它
映射 IMEM，宿主脚本 `run-clover-unattended.sh` 也保持原样——把参数传回去就
恢复 r148 的全部行为。

`check-clover.py` 的断言随之反向：`kernel_args` **不得**含
`clover-unattended=`，也不得含 `armcpu-clock-panic=`，标题必须是
`clover r149: ...`。源码侧那些「链路还在」的断言（IMEM 常量、三行 IOLog、
panic 屏的 `fastboot_mark`、`clover_fastboot_reason`）一条没删，因为代码确实
还在，只是没有参数去武装它。

### 行为上差在哪

- ArmCpu：`gUnattendedSeconds = 0`，每个 tick 的那次比较不再进入；真正的
  hang 不会被任何计时器打断。
- CloverSdhci：没有参数就立即返回，IMEM 不映射，`clover_fastboot_reason`
  保持空。panic 屏照旧打印、照旧在 20 秒后走 PSCI `SYSTEM_RESET`，但
  `fastboot_mark()` 没有单元可写，所以复位回到普通启动路径，**不再自动停在
  fastboot**。
- 两个计数看门狗都不在（`armcpu-clock-panic` 本来就没传）。所以「停住」只剩
  一种表现：屏上最后一帧不动；要再来一次，得手动进 fastboot。

### 构建、校验与本轮该刷哪一格

```
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

构建 `~/clover-out/round149-build.log`，退出码 0。两个 checker：

```
[check-clover] clover ok: ADT 13516 bytes, 8 cores; splash 0x9d400000-0x9f800000,
FD 0x9fc00000-0x9ff00000; board clover_defconfig, CLOVERESP;
75 inline asm statements separated; modules visible
kernel.macho: unattended watchdog linked   OK (unattended deadline %llu s ( present,
unattended mark %x -> %x dea present, unattended mark %x read back present,
unattended imem map failed % present, fb= present)
kernel.macho: asm tails survived the assembler OK
(mrs SPSR_EL1 in interrupt 1, mrs CNTVCT_EL0 in interrupt 2, msr ICC_SGI1R_EL1 in signalCPU 1)
```

死线代码仍被链进来（那些格式串还在），所以 `unattended watchdog linked OK` 不
矛盾；它证明的是「把参数传回去就能用」，而不是「这一轮用了它」。卷内
`unixnu.toml` 的 args 已确认同时不含 `clover-unattended=` 与
`armcpu-clock-panic=`。

产物在 `~/clover-out/round149-*`，`shasum -a 256 -c round149-SHA256SUMS`
七项全 OK：`payload.img` 58,720,256 B（`b6a4a076…`）、本地 `kernel.macho`
51,998,720 B（`6f7faa78…`）、卷内 `kernel.macho`（`799cba51…`，与本地只差
entry 探针补丁，6,838 字节、35 段）、卷内 `unixnu.toml` 585 B（`324676c8…`）、
ADT 13,516 B（`88355906…`；`chosen/random-seed` 每次构建随机，其余未变）、
`scratch-data.img` 35,888 B（`614b8356…`，未变）、trustcache 61,408 B
（`7c369ce0…`，未变）。

**本轮有一处 eMMC 写入：** payload 刷到 `cache`（与之前每一轮相同，只覆盖
`cache` 这一格）：

```
fastboot flash cache /Users/zhouzhou/clover-out/round149-payload.img
fastboot reboot
```

菜单第一行应当是 `clover r149: no deadline; the screen is the only stop`，冷
启动（不要 `fastboot boot`）。

这一帧只问一个问题：**没人打断时，iOS 从菜单一路能走到哪一步、最后停在哪一帧。**
屏上的最后一帧就是答案；若 20 秒后自己重启，那次是 panic，而不是 hang。没有
自动回 fastboot 的兜底。

## v0.5.3（round150）：根是授权来的，不是改名骗来的

r146 的墙（`shared_region` 空、`check_np` ENOMEM 之后无声）这一轮没有继续拆。
按「照搬 sheng」的要求，这一轮搬的是 sheng 与 clover 之间**唯一一处还没搬的
启动关键差异**：sheng 从不给自己根改假名，它让 `bsd_init.c` 的
`FSIOC_KERNEL_ROOTAUTH` 真正执行，然后在 owner 信任下接受这个根。

### 改了什么

1. `board.toml` 删掉 `rd=md0s39`，补上 sheng 的 `-no_slto_panic`；标题换成
   `clover r150: the root is authorized, not aliased`。
2. 新板级补丁 `patches/xnu-clover/zzy-clover-owner-root-trust.patch`：在
   `bsd_init.c` 里 `#undef` 再 `#define XNU_OWNER_ROOT_AUDIT 1` 与
   `XNU_OWNER_ROOT_TRUST 1`。上游 `offline-owner-root-binding-audit.patch`
   本来就在树里，但 `build-xnu-4k-baseline.sh` 把这两个开关关着——它们被
   `XNU_NATIVE_VIRTIO_BLOCK` 门控，而那个归档会和 CloverBlock 重复定义
   `native_verified_block_*`。所以还是老办法：只在读它们的 translation unit
   里覆盖命令行，和 r143 的 `zzy-clover-systemos-mount.patch` 同一形状。
3. `check-clover.py` 的根契约整体反向：`kernel_args` **不得**含 `rd` /
   `rd=`；标题必须是 `clover r150:`；新补丁必须存在且四个
   `#undef`/`#define` 都在；必须有 `-no_slto_panic`。
4. `verify-clover-payload.py` 新增 `owner root trust compiled in`：查
   `native_owner_root_checkpoint`、`native_owner_root_accepted`、
   `native_owner_root_selected_device` 三个符号（审计块引用它们，链接器丢
   不掉），并确认 `rootvp not authenticated after mounting` 这句还在——它
   证明的是「如果没编译进去，窗口会读到什么」。

补丁插入点有一个实况：第一版把它放在文件第一行，和
`zzz-boot-progress.patch` 的首行 `#include` 插入撞在一起。`git apply` 对
「声明在第 1 行、没有前置上下文」的 hunk 不做向下的偏移搜索，`zzz` 的
`bsd_init.c` hunk 因此失败，内核构建在补丁阶段就退出（`line 44: apply
--check failed`）。改到 license 头之后、`#include <sys/param.h>` 之前后，
两个补丁互不相干，先应用哪个都能过 `--check`。

### ROOTAUTH 现在会做什么

r138 起，`rd=md0s39` 让 `bsd_rooted_ramdisk()` 的前缀判断为真，整个
`FSIOC_KERNEL_ROOTAUTH` 块被跳过。现在它会被执行：取
`vfs_devvp(root_mount)` 的 `vnode_specrdev()`，检查根挂载带 `MNT_RDONLY`，
再问 `drivers/CloverBlock` 的 `native_verified_block_root_authorized(device)`。
CloverBlock 对 `gOsVolumeDev` 与 `gSystemVolumeDev` 答 yes——r145 的 exec
路径 `native_owner_cache_prepare()` 已经在实机上走过同一个函数。两问都真，
`owner_authorized = 1`，ioctl 的失败被接受；否则原来的
`panic("rootvp not authenticated after mounting")` 照旧。

`-no_slto_panic` 是 sheng 的启动参数，`osfmk/kern/startup.c:476` 把它变成
`spinlock_timeout_panic = FALSE`。r146 的静默里没有自旋锁报告，但计时器还
没到点；这一轮先按 sheng 的方式把它摘掉，免得下一帧被一个无关的 panic
打断。

### 屏上三种读数

1. `rootvp not authenticated after mounting`：审计拒绝了（根不是只读，或
   `device` 不是 CloverBlock 的两个卷之一）。这说明别名之外还有一层没对齐，
   下一轮就查 `native_owner_root_decision` / `native_owner_root_readonly`
   这两个全局值。
2. 过了 `bsd_init`、继续 exec `/sbin/launchd`：说明假名本来就不是 r146 墙
   的原因，墙还在更上面，`shared_region` 那条线重新成为唯一嫌疑。
3. 其它新帧：按原话记录。

### 构建、校验与本轮该刷哪一格

```
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

构建日志 `~/clover-out/round150-build.log`，退出码 0。两个 checker：

```
[check-clover] clover ok: ADT 13516 bytes, 8 cores; splash 0x9d400000-0x9f800000,
FD 0x9fc00000-0x9ff00000; board clover_defconfig, CLOVERESP;
75 inline asm statements separated; modules visible
kernel.macho: owner root trust compiled in OK (native_owner_root_checkpoint present,
native_owner_root_accepted present, native_owner_root_selected_device present,
rootvp not authenticated after mounting present)
kernel.macho: asm tails survived the assembler OK
(mrs SPSR_EL1 in interrupt 1, mrs CNTVCT_EL0 in interrupt 2, msr ICC_SGI1R_EL1 in signalCPU 1)
```

卷内 `unixnu.toml` 的 args 已确认没有任何 `rd=`，同时带着 `-no_slto_panic`
与 `cs_enforcement_disable=1`。

产物在 `~/clover-out/round150-*`，`shasum -a 256 -c round150-SHA256SUMS`
七项全 OK：`payload.img` 58,720,256 B（`7047d320…`）、本地 `kernel.macho`
51,998,720 B（`611f6a17…`）、卷内 `kernel.macho`（`cb14b125…`，与本地只差
entry 探针补丁，6,838 字节、35 段）、卷内 `unixnu.toml` 585 B（`a1398d2b…`）、
ADT 13,516 B（`5a4e70ef…`；`chosen/random-seed` 每次构建随机，其余未变）、
`scratch-data.img` 35,888 B（`614b8356…`，未变）、trustcache 61,408 B
（`7c369ce0…`，未变）。

**本轮有一处 eMMC 写入：** payload 刷到 `cache`（与之前每一轮相同，只覆盖
`cache` 这一格）：

```
fastboot flash cache /Users/zhouzhou/clover-out/round150-payload.img
fastboot reboot
```

菜单第一行应当是 `clover r150: the root is authorized, not aliased`，冷启动
（不要 `fastboot boot`）。

这一帧只问一个问题：**把假名拿掉、让根授权真正执行之后，屏上读到的还是不是
那句 “rootvp not authenticated after mounting”。**

### 读数（2026-10-26 冷启动）

答案是第 2 种：**审计接受了根，假名本来就不必要。** 屏上依次是

```
[clover] vfs_mountroot returned
[clover] root_mount_authorization_request:d753: disk1s1 Root Volume, Root hash authorization is required
[clover] devfs_kernel_mount returned
[clover] siginit returned, exec'ing init
load_init_program: attempting to load /usr/appleinternal/sbin/launchd.development
load_init_program: attempting to load /sbin/launchd
clover-mac: label-check AMFI (Apple Mobile File Integrity) -> 1
clover-mac: label-check Sandbox (Seatbelt sandbox policy) -> 1
```

`root_mount_authorization_request` 这句说明 `FSIOC_KERNEL_ROOTAUTH` 这次真的进了
`bsd_init.c` 的审计块（r138–r149 的 `rd=md0s39` 会整块跳过它），而后面没有
`rootvp not authenticated after mounting`：`vfs_devvp(root_mount)` 的
`vnode_specrdev()` 落在 `gOsVolumeDev` 上，只读检查也通过。SIGINIT 之后
launchd 被 exec，两个 MAC 标签检查都返回 1。

墙没有后退，但这一帧第一次把墙量了出来（同一帧里 launchd 重试，一模一样的
两遍）：

```
shared_region: 0xfffffe2da3f40fdf [1(launchd)] check_np(0x16f3ddb0) va_shared_region_start_address() failed
clover r146: check err=12 start=0
clover r146: cache err=2 size=0 mnt=0x0 rmnt=0x8c8ba2d95933a3e7 fl=0x0 rfl=0x14089821
```

- `check err=12` 是 `shared_region_check_np` 的 ENOMEM：共享区不存在，起点为 0。
  dyld 在拿到空答案后就停了，下一行的 `map enter` 从未出现。
- `cache err=2` 是 `vnode_lookup("/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e", …)`
  的 ENOENT：`size=0`、`mnt=0x0`，路径没有落到任何挂载点上。
- `rfl=0x14089821` 是根挂载的 `vfs_flags`，包含 `MNT_RDONLY`（0x1）和
  `MNT_UNION`（0x20）——union 层在根挂载上，不是没挂上。`rmnt` 是
  `VM_KERNEL_ADDRPERM` 过的指针（没开 `-show_pointers`），只有比较意义，不
  是地址。

同一帧还确认了 SystemOS 这一侧的形状：卷名
`DawnUpdate21H461.J120J121J207J200SSystemCryptex`，UUID `8846FE78-2910-4297-AF97-0D9BB6FC16`，
`disk3s1`、`1:6`、`p=96`，由 `init (pid 1)` 请求挂载并 `mount-complete`，
`884608` 个 4 KiB 块、`unencrypted`、`features: 28.0`；OS 侧的卷名是
`DawnUpdate21H461.J120J121J207J200S`（`disk1s1`）。也就是说 exec 路径、
owner 查找、两窗口闩锁都按 r145 的设计走通了，剩下的唯一缺口是**名字查找**
：`/System/Library/Caches/com.apple.dyld/…` 在这个 union 里看不见。

这给了下一轮一个不用刷机的问题：**SystemOS 窗口里的那份 APFS 卷，自己带不带
这条路径？** 宿主上直接读 `~/UniXNU/out/clover/userdata/userdata-root.img`
第 6,079,643,648 字节起的 3,303,014,400 字节就能回答；带，说明 union 没把下层
露出来（方向/时机）；不带，说明 dyld 缓存根本不在这个卷里，得去 Data 侧找。

## v0.5.4（round151）：看 dyld 自己去开那个文件

### 宿主先回答的那半个问题

把 SystemOS 窗口从 `userdata-root.img` 里切出来挂上，`/System/Library/Caches/
com.apple.dyld/` 里躺着 `dyld_shared_cache_arm64`（491,520 字节的主桩）、
`.01`…`.52` 子缓存和一份 `.symbols`。文件头是 `dyld_v1   arm64\0`。

**名字是 `arm64`，不是 `arm64e`。** r146 的 `cache err=2` 查的是
`dyld_shared_cache_arm64e`，那是一份不存在的文件——它回答的不是 dyld 的问题，
是探针自己的拼写错误。这一轮不能再用同一句话下结论。

### dyld 在这条路上的真实顺序

用 OS 卷里的 `/usr/lib/dyld`（21H461）反汇编对出来：

1. `dyld::CacheFinder`（0x32c58）读 `dyld_flags`，`tbnz w8,#0x3` 跳到 0x32e28；
   `dyld_flags` 第 3 位正是 r150 已经置上的那一位，所以这里不再 ignite cryptex，
   而是 `SyscallDelegate::open("/System/Library/Caches/com.apple.dyld/")`。
2. `mapSplitCacheSystemWide` → `preflightMainCacheFile`（0x4e578）：对这个 fd
   `fstatat("dyld_shared_cache_arm64")`、再 `stat("enable-dylibs-to-override-cache")`、
   再 `openat(dirfd, "dyld_shared_cache_arm64")`。
3. `preflightCacheFile`（0x4d…）对这个新 fd：`fstat64`、`pread` 16 字节比 magic，
   然后校验 mappings / platform / linkedit / 签名长度。
4. 全部过了才分配 dynamic config 的地址，并在 0x4e488 调 syscall 536。

r150 的 `map enter` 从未出现，说明卡在第 2–3 步。第 2 步的第一步是 dyld 自己的
`open`：目录开不开、文件开不开，r146 一个字都没看到。

### 改了什么：一个包装，两行读数

新补丁 `zzy-clover-shared-region-why2.patch` 只碰 `bsd/vfs/vfs_vnops.c`：

- `vn_open_auth` 是 `open(2)`、`openat(2)` 和 `vn_open()` 的唯一汇合点，把它改名
  成 `vn_open_auth_internal`，外面套一层同名包装。打开结束后 `cn_pnbuf` 里就是
  内核已拷好的路径，只对两个前缀出声（`/System/Library/Caches/com.apple.dyld`
  和 `dyld_shared_cache`），其余 open 原样返回。
- 每次命中打印 `clover r151: open <path> fmode=… err=…`；成功时加上
  `fl=/vt=/fs=`（vnode 所在挂载的 flags、vnode 类型、文件系统名）。
- 若是普通文件，再用同一个 vnode `vn_rdwr` 读 16 字节，打印
  `clover r151: read err=… resid=… hdr=…`——这一行是 dyld 的 `pread` 在内核侧
  的等价物，头部读不出来就不必再猜 preflight 的校验。

包装不改任何已有补丁的新增行，r146 的反向检查仍然成立。

### 屏上三种读数

- 目录 open 报错（例如 `err=2`/`13`）：union 这一层根本没把 SystemOS 的
  `/System/Library` 露出来，问题回到挂载方向，而不是缓存文件本身。
- 目录 open 成功、`dyld_shared_cache_arm64` 的 open 报错：目录里的文件对内核
  不可见，下一刀查目录项或权限。
- 两个 open 都成功：看 `read` 行。`hdr=dyld_v1   arm64` 说明内核读得到，
  卡点在 16 字节之后的头部校验或 dynamic config 的 `vm_allocate`。

### 构建、校验与本轮该刷哪一格

```sh
cd ~/UniXNU-clover && python3 scripts/check-clover.py
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" ./unixnu build kernel
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH ./unixnu tree
cd ~/UniXNU-clover && python3 scripts/verify-clover-payload.py
```

刷写仍然只写 cache，冷启动：

```sh
fastboot flash cache /Users/zhouzhou/clover-out/round151-payload.img
fastboot reboot
```

菜单第一行应当是 `clover r151: dyld opens its own cache, the frame says which step`。

这一帧只问一个问题：**dyld 打开目录和打开 `dyld_shared_cache_arm64` 时，
内核分别给了什么 errno；打开成功的那一份，16 字节头读不读得出来。**

产物归档在 `~/clover-out/round151-*`，`shasum -a 256 -c round151-SHA256SUMS` 七项全 OK：
`payload.img` 58,720,256 B（`a6ef5930…`）、本地 `kernel.macho` 51,998,720 B
（`0e79fb40…`）、卷内 `kernel.macho`（`a975a97f…`，入口探针仍在）、卷内
`unixnu.toml` 601 B（`f775e0ce…`）、ADT 13,516 B（`4f0ea293…`）、`scratch-data.img`
35,888 B（`614b8356…`，即 launchd 数据盘）、`trustcache` 61,408 B（`7c369ce0…`）。

## v0.5.5（round152）：把 r146 撤掉，让这一帧只剩 dyld 自己

### 上一帧的两个事实

r151 的帧上，r146 的五行都在（两次 `clover r146: check err=12` 和
`cache err=2 …`），`clover r151:` 一行都没有。镜像本身没有问题：卷内内核里
`vn_open_auth_internal` 出现 9 次，两条 r151 格式串各 1 次——探针确实编进去了。

所以这一轮只改一个变量：**把 r146 的探针全部撤掉**。

### 为什么先撤 r146

r146 的 `vnode_lookup` 跑在 `shared_region_check_np` 里面——那正是 dyld 在找缓存
之前调用的那个 syscall。从 union 挂好到 dyld 打开缓存之间，它是唯一一处我们插进
VFS 的调用。而 r151 的包装只在 `vn_open_auth` 返回之后打印：如果 dyld 的 open 在
调用里停住，屏上同样什么都不会出现。把 r146 拿掉，下一帧就是干净的对照——
那个时刻只剩 dyld 自己的 open。

### 改了什么

- 删除 `zzy-clover-shared-region-why.patch`（vm_unix.c 的五个读数：
  check / cache / map enter / map setup / map exit），构建树已反向撤净。
- `zzy-clover-shared-region-why2.patch`（r151 的 `vn_open_auth` 包装）原样保留。
- `check-clover.py` 的契约从“五针必须在”改成“r146 补丁必须不在”；
  `verify-clover-payload.py` 断言镜像里不再有 `clover r146:`，也不再有
  `dyld_shared_cache_arm64e` 这个名字。
- 标题 `clover r152: r146 comes out, and dyld gets the frame to itself`。

### 构建与刷写

```sh
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

只写 `cache`，冷启动：

```sh
fastboot flash cache /Users/zhouzhou/clover-out/round152-payload.img
fastboot reboot
```

### 这一帧怎么读

- 出现 `clover r151: open …`：r146 的探针确实压住了 dyld 的路径，按那几行
  继续分目录、文件、头三步。
- 仍然没有任何 `clover r151:`，但 `shared_region` 那两行还在：dyld 在
  `vn_open_auth` 返回前就停了，或根本没有调 open。下一轮把打印挪到进入 open
  之前，用 enter/exit 两行把它切开。
- 连 `shared_region` 都没有：对照没有成立，先查刷入的是不是这一格。

这一帧只问一个问题：**撤掉 r146 之后，dyld 会不会自己走到 open，屏上出现
`clover r151:`。**

产物归档在 `~/clover-out/round152-*`，`shasum -a 256 -c round152-SHA256SUMS` 七项全 OK：
`payload.img` 58,720,256 B（`f9a6ee2d…`）、本地 `kernel.macho` 51,998,720 B
（`83ec1fd6…`）、卷内 `kernel.macho`（`a48ecd2e…`，入口探针仍在）、卷内
`unixnu.toml` 599 B（`88dc8758…`）、ADT 13,516 B（`9b5b8659…`）、`scratch-data.img`
35,888 B（`614b8356…`，即 launchd 数据盘）、`trustcache` 61,408 B（`7c369ce0…`）。
ADT 每轮都会变一次 64 字节：`/chosen/random-seed` 是 `os.urandom(64)`，其余字节
与上一轮逐字节相同。

## v0.5.6（round153）：探针先看再比，顺带问 dyld_flags

### r152 的帧说了什么

r146 的五行走干净了（镜像刷对了），可 `clover r151:` 还是一行都没有，屏上仍是
两遍：

```
shared_region: … [1(launchd)] check_np(0x16ed25150) vm_shared_region_start_address() failed
```

上一轮据此准备“把打印挪到进入 open 之前”。核对源码后，这个方向错了：**问题在
探针自己身上**。

### 根因：`NDINIT` 从不初始化 `cn_pnbuf`

`NDINIT`（`bsd/sys/namei.h:202`）只设 `ni_dirp`、`cn_flags` 等字段，`cn_pnbuf`
留给 `namei()` 第一次 copyin 时填。`openat_internal` 用
`kalloc_type(..., Z_WAITOK)` 分配 `nameidata`，不置零。r151 的包装在调用
`vn_open_auth_internal` **之前**就读 `ndp->ni_cnd.cn_pnbuf`，拿到的是未初始化的
垃圾（大概率是 NULL），于是每次都在前缀比较处提前 `return error`。

也就是说：r151 和 r152 的沉默只证明探针没跑，**完全不能**用来判断 dyld 有没有
调 open。上一轮把“dyld 没 open”当成结论，是错的。

`nameidone()`（`vfs_lookup.c:1949`）只在路径超过 `ni_pathbuf`（`HASBUF`）时
`zfree` 并把指针置 NULL；dyld 的目录名和文件名都远短于 1024 字节，**调用内部函数
之后**再读这个字段是有效的。

### 改了什么

- `zzy-clover-shared-region-why2.patch`（只剩两个文件）：
  - `bsd/vfs/vfs_vnops.c`：包装改为先 `error = vn_open_auth_internal(...)`，
    再 `path = ndp->ni_cnd.cn_pnbuf;` 做前缀匹配；三条读数（open / 挂载 / 头）不变，
    前缀仍是 `/System/Library/Caches/com.apple.dyld` 与 `dyld_shared_cache`。
  - `bsd/vm/vm_unix.c`：在 `shared_region_check_np` 开头打印一行
    `clover r153: check_np dyld_flags=0x%llx`。板级 owner chain 在 exec 时置
    `dyld_flags |= 8`（bit 3 = 从磁盘找缓存），这行直接回答“发问的进程拿到的是
    什么值”。
- `check-clover.py`：断言补丁必须是“调用后读路径”的形状（`error = …internal`
  出现在 `path = ndp->ni_cnd.cn_pnbuf` 之前），且补丁与镜像都不许再有
  `clover r151:`；标题 `clover r153:`。
- `verify-clover-payload.py`：镜像里必须有四条 r153 串（open / mount / read /
  flags）和两个前缀，且 `clover r146:`、`dyld_shared_cache_arm64e`、
  `clover r151:` 都不在。

构建坑：`./unixnu build` 读的是 `~/UniXNU/patches/xnu/*.patch`，不是
`~/UniXNU-clover/patches/xnu-clover/`。改完补丁必须把新文件同步过去，否则构建
会拿旧副本去 apply 并失败（本轮第一次构建就死在这里）。

### 构建与刷写

```sh
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

只写 `cache`，冷启动：

```sh
fastboot flash cache /Users/zhouzhou/clover-out/round153-payload.img
fastboot reboot
```

### 这一帧怎么读

1. `clover r153: check_np dyld_flags=0x0`：launchd 没拿到 bit 3，dyld 不会去磁盘
   缓存目录。先修 owner chain 的 exec 路径 / `dyld_flags=0x…` 字符串注入，别碰
   VFS。
2. `dyld_flags=0x8` 且出现 `clover r153: open …`：探针终于说话，按目录 → 文件 →
   头三步读。目录 err=0、文件 err=2 说明缓存不在预期卷；文件 err=0 但头读失败，
   说明 vnode 读通路有问题；头以 `dyld_v1` 开头则缓存可达，墙在别处。
3. `dyld_flags=0x8` 且仍无任何 `clover r153: open`：dyld 在
   `shared_region_check_np` 失败（ENOMEM）后就放弃了找缓存。下一轮攻
   `shared_region_check_np` 的 ENOMEM，而不是文件系统。

这一帧只问一个问题：**把探针的“先读后调”改成“先调后读”之后，dyld 会不会在屏上
留下它自己的 open，以及它拿到的 dyld_flags 是多少。**

产物归档在 `~/clover-out/round153-*`，`round153-SHA256SUMS` 八项：
`payload.img` 58,720,256 B（`deeadfff…`）、本地 `kernel.macho` 51,998,720 B
（`ccb3dbd3…`）、卷内 `kernel.macho`（`2554dbbc…`，入口探针仍在）、卷内
`unixnu.toml` 610 B（`c6584c01…`）、ADT 13,516 B（`a8eb00da…`）、`scratch-data.img`
35,888 B（`614b8356…`）、`trustcache` 61,408 B（`7c369ce0…`）、构建日志
（`804c2d79…`）。两个 checker 的结果：`r146 probe removed OK`、`cache open probe
linked OK (open/mount/read/flags/prefixes present, r151 form absent)`。

## v0.5.7（round154）：读完头之后，dyld 到底有没有进 syscall 536

### r153 的帧说了什么（2026-10-27 冷启动）

探针修好了，屏上第一次出现 dyld 自己的两条路径：

```
clover r153: check_np dyld_flags=0x0
shared_region: … [1(launchd)] check_np(0x16fac5150) vm_shared_region_start_address() failed
clover r153: open /System/Library/Caches/com.apple.dyld fmode=0x1 err=0 fl=0x14089821 vt=2 fs=apfs
clover r153: open dyld_shared_cache_arm64 fmode=0x1 err=0 fl=0x14089821 vt=1 fs=apfs
clover r153: read err=0 resid=0 hdr=dyld_v1   arm64
clover r153: open dyld_shared_cache_arm64.01 fmode=0x1 err=0 fl=0x14089821 vt=1 fs=apfs
clover r153: read err=0 resid=0 hdr=dyld_v1   arm64
```

三条结论：

1. **缓存路径完全可达**：目录、主文件、`.01` 都是 `err=0`，两个头都读回
   `dyld_v1   arm64`。文件系统、APFS、只读挂载这一层没有墙。
2. **`dyld_flags=0x0`，dyld 仍然自己去磁盘找了缓存**。这说明板级 owner chain 的
   bit 3 注入没有发生，但不影响 dyld 走 `open`；它只是不把“磁盘上有缓存”当既定
   事实而已。这一项先记账，不是当前的墙。
3. **`check_np` 的 ENOMEM 不是文件问题**：只有任务已有共享区、而
   `vm_shared_region_start_address()` 又失败（`sr_first_mapping == -1`，空区）时才
   打印这句。空区是每次 exec 的 `vm_map_exec` → `vm_shared_region_enter` 建出来
   的，首进程本来就是这个状态。dyld 的本职就是随后用 syscall 536 把它填满。

屏上的输出停在 `.01` 的头读之后。**不能**据此断言 dyld 没进 syscall 536：控制台
输出可能止于下一个探针之前，也可能 dyld 真的停在映射之前。r153 的问句到此为止，
r154 接着问下一句。

### 改了什么

只加 `patches/xnu-clover/zzy-clover-shared-region-why3.patch`（**只动
`bsd/vm/vm_unix.c`**，why2 原样保留）：

- `shared_region_map_and_slide_2_np`（syscall 536 入口）三行：
  `map2 enter files=/mappings=`、`map2 fd0=/m0=/slide0=`（copyin 之后才读，文件数
  与映射数都已校验非零）、`map2 <- err=`（`_shared_region_map_and_slide` 在这条
  路径上返回的是 errno）。
- `shared_region_map_and_slide_setup` 的阶梯：`clover_stage = 1 + n;` 每过一道闸
  就加一（1 文件数、2 分配与填充、3 共享区与 root_dir、4 fd、5 可读、6 vnode 类型、
  7 MAC、8 root uid、9 跨卷/scdir、10 文件大小、11 memory object、12 codesign），
  循环顶部记 `clover_at`；`done:` 处失败时打印
  `setup err=%d stage=%d at=%u files=%u mappings=%u`。
- `_shared_region_map_and_slide` 里：setup 失败打印 `map setup <- err=`；`map_file`
  之后打印 `mapfile kr=0x%x`。源码里另有一条带 `carrier=<accepts>/<rejects>` 的
  形式，但本板构建没有导出 `XNU_OWNER_CACHE_CARRIER`（默认为 0，
  `zzy-clover-systemos-mount.patch` 只强开 `XNU_OWNER_CACHE_MOUNT` /
  `XNU_OWNER_UNION_SYSTEMOS`），所以镜像里只有不带计数的短行；计数器要等真的
  打开 carrier 才存在。这几行刻意**不**插进 carrier 补丁自己的新增块：那样会让它
  的 `--reverse --check` 失效，构建脚本会以为补丁没打过并再次正向 apply（本轮
  第一次构建就死在这里）。

- `check-clover.py`：why3 必须存在、只含 `bsd/vm/vm_unix.c`、含六条 r154 串与
  阶梯的关键档位；不许出现 `clover r146:` / `clover r151:`；标题 `clover r154:`；
  why2 的存在性断言不变。
- `verify-clover-payload.py`：新增 `check_map_slide_probe`，镜像里必须有五条 r154
  串（entry / fd0 / return / setup / mapfile 短行），且 r153 的 open 串仍在（这一
  轮是加问句，不是替换上一句）。

### 构建与刷写

```sh
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

只写 `cache`，冷启动：

```sh
fastboot flash cache /Users/zhouzhou/clover-out/round154-payload.img
fastboot reboot
```

### 这一帧怎么读

1. 有 `map2 enter`、没有 `map2 <-`：dyld 进来了，卡在映射内部。看有没有
   `setup err=… stage=… at=…`；有 stage 就按档位查，没 stage 说明卡在
   `vm_shared_region_map_file` 里（那时只有 `mapfile kr=` 之前的部分）。
2. `map2 <- err=0`：共享区填好了，墙在 dyld 的下一段（后续 `mmap`/绑定）。
3. `map2 <- err≠0` 或 `setup err=…`：按 errno 与 stage 定位。常见组合：
   stage=3 → 共享区/root_dir 不匹配；stage=9 → 跨卷/scdir 被拒（本板 carrier
   宏为 0，跨卷时只能靠 preboot 卷命中；若这里报 EPERM，下一轮就得打开
   `XNU_OWNER_CACHE_CARRIER`）；stage=12 → codesign 覆盖。
4. 一条 `clover r154:` 都没有：dyld 在 `.01` 头读之后、syscall 536 之前就停了，
   下一轮转查 dyld 侧（缓存结构校验、映射尺寸），不再碰 VFS。

这一帧只问一个问题：**dyld 读完两个缓存头之后，有没有进入
`shared_region_map_and_slide_np`；若进了，内核在哪一道关卡回答它。**

产物归档在 `~/clover-out/round154-*`，`round154-SHA256SUMS` 八项：
`payload.img` 58,720,256 B（`d3e58c00…`）、本地 `kernel.macho` 51,998,720 B
（`0ad2579e…`）、卷内 `kernel.macho`（`3da6b675…`，入口探针仍在）、卷内
`unixnu.toml` 618 B（`778d27ef…`）、ADT 13,516 B（`27ff8bc1…`）、`scratch-data.img`
35,888 B（`614b8356…`）、`trustcache` 61,408 B（`7c369ce0…`）、构建日志
（`59845e62…`）。两个 checker 的结果：`r146 probe removed OK`、`cache open probe
linked OK`、`map-and-slide probe linked OK (enter/fd0/return/setup/mapfile present,
r153 form kept)`。

### r154 的帧说了什么（2026-10-27 冷启动）

屏上依次是 `load_init_program` → `/usr/appleinternal/sbin/launchd.development` →
`/sbin/launchd`、两条 `clover-mac: label-check …-> 1`，然后是：

```
clover r153: check_np dyld_flags=0x0
shared_region: … [1(launchd)] check_np(0x16ea0bbb0) vm_shared_region_start_address() failed
clover r153: open /System/Library/Caches/com.apple.dyld fmode=0x1 err=0 fl=0x14089821 vt=2 fs=apfs
clover r153: check_np dyld_flags=0x0
shared_region: … [1(launchd)] check_np(0x16ea0b150) vm_shared_region_start_address() failed
clover r153: open dyld_shared_cache_arm64 fmode=0x1 err=0 fl=0x14089821 vt=1 fs=apfs
clover r153: read err=0 resid=0 hdr=dyld_v1   arm64
clover r153: open dyld_shared_cache_arm64.01 fmode=0x1 err=0 fl=0x14089821 vt=1 fs=apfs
clover r153: read err=0 resid=0 hdr=dyld_v1   arm64
```

之后屏幕结束，**没有一条 `clover r154:`，也没有 panic**。三条结论：

1. **syscall 536 没有进来**：`map2 enter` 是 536 入口的第一句，缺它即未进。
   r154 的四种读法落到第 4 种，墙在 dyld 侧，不在 VFS 也不在映射阶梯。
2. **`.01` 的 open 反证主缓存 preflight 全过**。按 dyld 源码，
   `openSubSharedCacheFile(".01")` 只在 `preflightMainCacheFile` 返回真之后发生；
   而主缓存的 preflight 已经包含 `fstat`、`pread(0x4000)`、magic、
   `validPlatform`、mappingCount、`mappings[0].fileOffset==0`、
   `codeSignatureOffset+codeSignatureSize==st_size`、maxProt、映射不重叠，以及
   **`F_ADDFILESIGS_RETURN` 的整段签名块注册**。也就是说：签名块从 APFS 读进
   UBC、并挂到 vnode 上，这条链在 r154 已经走通；`.01` 只是复用了同一份文件系统
   与同一套 preflight 代码。
3. **`dyld_flags=0x0` 与映射无关**。它只说明板级 owner chain 的 exec 成功块没跑到
   （bit 3 没置），dyld 因此不把“磁盘上有缓存”当既定事实、自己去 open；这条路径
   在 r153 已证明可达。r154 再次确认：flags 为 0 时 dyld 照样走到了 `.01`。

剩下两种形态，这一帧无法区分：

- **`.01` 的头字段被 dyld 静默拒绝**（`preflightCacheFile(…, nullptr)` 对子缓存
  不做签名注册，但同样校验 platform/mapping/`st_size`；任一失败只是 `return
  false`，没有日志）。
- **卡在 `.01` 的签名块读**（`fcntl` 里 `vn_rdwr` 一次拉满整段 blob；若这段 blob
  有几十 MB，APFS 冷读在 25 KB/s 量级下就是分钟级，屏幕在此期间停住，看起来和
  “静默拒绝”一样）。

r155 就是为了把这两种形态分开，并顺带回答“如果都过了，会不会直接走进 536”。

## v0.5.8（round155）：`.01` 打开之后，dyld 停在哪道门

### 改了什么

两个补丁，各管一面，互不越界：

- **`zzy-clover-shared-region-why2.patch` 扩写**（`bsd/vfs/vfs_vnops.c`）。在 r153
  的 `read` 之后调用新的静态辅助函数
  `clover_cache_header(vnode_t vp, const char *path)`：用 `vn_rdwr` 读 512 字节头
  和 `map_count*32` 字节映射数组，把 dyld 自己马上要校验的字段原样打到屏上——
  `len`/`sig`（`codeSignatureOffset+Size` 与 `st_size` 的对照）、
  `off`/`n`/`ty`/`plat`/`fl`、`m0`/`ml`（首末 mapping 的 fileOffset 与 maxProt）、
  `subs`（子缓存数组位置）。tag 取路径末三字符（`.01`）或 `"main"`，行首统一
  `clover r155: hdr …`，**每行自带本轮身份**，避免再出现 r154 那种“帧无法自证镜子
  是哪一版”的情况。只用 `IO_NODELOCKED | IO_NOAUTH`，不触发签名路径。
- **`zzy-clover-shared-region-why4.patch`**（新建，**只动
  `bsd/kern/kern_descrip.c`**）。在 `F_ADDFILESIGS_RETURN` 的内联分支里打四针：
  `fsig vp=… start=… blob=… size=…`（copyin 之后，dyld 实际要求注册的范围）、
  `fsig hit=1`（`ubc_cs_blob_get` 命中已有 blob，说明没重读）、
  `fsig mem=…/…`（`ubc_cs_blob_allocate` 成功，给出内核 blob 地址与大小）、
  `fsig rd off=… len=…` 与 `fsig rd <- err=… resid=… ms=…`
  （`vn_rdwr` 前置时间戳 `mach_absolute_time()`，回来后换算成毫秒）。这样“签名块
  读慢”就有了直接证据：`len` 是这段 blob 的真实尺寸，`ms` 是它花的时间。

另外三处配套：

- `check-clover.py`：why2 必须含 `clover_cache_header(vnode_t vp, const char *path)`
  与三条 hdr 格式串，并**反向断言不许含 `clover r154:`**；why4 必须存在、只含
  `bsd/kern/kern_descrip.c`、含五条 fsig 串与 `mach_absolute_time()`、断言
  `rd off` 出现在 `mach_absolute_time() - clover_start` 之前；标题以
  `clover r155:` 开头。
- `verify-clover-payload.py`：新增 `check_cache_gate_probe`，镜像里必须同时有八条
  r155 串（`hdr_size` / `hdr_layout` / `hdr_maps` / `fsig` / `fsig_hit` /
  `fsig_mem` / `fsig_read` / `fsig_done`），且 r153 的 open 串仍在。
- `board.toml` 的 `title`。

一个必须记住的构建陷阱：`scripts/save-xnu-patch.py` 以“HEAD + 名字排序在目标之前
的补丁”为基线，而工作区里**同时**存在 why3 的 r154 改动。直接重新生成 why2 会把
r154 的行吸进 why2。正确顺序是先 `git apply --reverse` why3 → 生成 why2 → 再
`git apply` why3；checker 里那条“why2 不许含 r154”就是防这个回归的。

### 构建与刷写

```sh
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

只写 `cache`，冷启动：

```sh
fastboot flash cache /Users/zhouzhou/clover-out/round155-payload.img
fastboot reboot
```

### 这一帧怎么读

先看 `hdr .01`（或 `hdr main`）那三行，再看 `fsig`：

1. `hdr .01` 的 `plat` 不等于进程平台（arm64 / 非 simulator），或 `m0`/`ml` 的
   `maxProt` 不含读执行，或 `sig` 加起来不等于 `len`：dyld 会在该字段静默
   `return false`。这就是“头字段被拒”的形态，墙在缓存文件本身。
2. `hdr .01` 全部合规，随后出现 `fsig vp=… rd off=… len=…` 但**没有**
   `fsig rd <-`：签名块读卡住了。`len` 就是卡住的字节数。
3. `fsig rd <- … ms=` 的 `ms` 很大（几十秒级）而同一条后面还有输出：只是慢，
   不是卡；下一帧应该在 `ms` 之后继续走。
4. `fsig hit=1` 且没有 `fsig rd`：这份 blob 已经注册过（主缓存注册的同一段），
   签名注册不是墙。
5. 头合规、签名也过了，紧接着出现 `map2 enter`：`.01` 整段 preflight 通过，
   dyld 往下走到 536，墙在映射阶梯（回到 r154 的读法 1–3）。
6. 头合规、签名过了、但既没有 `map2 enter` 也没有更多 `fsig`：dyld 在子缓存的
   后续校验（mapping 与主缓存重叠、subcache 数组一致性）里停下，下一轮转查
   dyld 的 `mapSplitCacheSystemWide`。

这一帧只问一个问题：**`.01` 打开之后，dyld 是被头字段拒、被签名块读拖住，还是
已经跨进 syscall 536。**

产物归档在 `~/clover-out/round155-*`，`round155-SHA256SUMS` 八项：
`payload.img` 58,720,256 B（`6b5a15f4…`）、本地 `kernel.macho` 51,998,720 B
（`0ee70622…`）、卷内 `kernel.macho`（`4bb7dcf2…`，r153/r154/r155 三类探针都
在）、卷内 `unixnu.toml` 620 B（`22c9b271…`）、ADT 13,516 B（`6d1acfdb…`）、
`scratch-data.img` 35,888 B（`614b8356…`）、`trustcache` 61,408 B
（`7c369ce0…`）、构建日志（`03cba9db…`）。两个 checker 的结果：
`clover ok: ADT 13516 bytes, 8 cores; … 75 inline asm statements separated;
modules visible`、`kernel.macho: probe patch localized OK (6836 bytes in 35 runs)`、
`cache gate probe linked OK (hdr_size/hdr_layout/hdr_maps/fsig/fsig_hit/fsig_mem/
fsig_read/fsig_done present, r153/r154 forms kept)`、`map-and-slide probe linked
OK`、`cache open probe linked OK`。

## v0.5.9（round156）：让 FIFO 按卡的速度跑

### 为什么这一轮先修速度

r155 的探针还没上机，用户要求不再一轮一个探针地等。宿主上能确认的事实已经足够回答
上半截问题：把 21H461 的 `Cryptex1,SystemOS` 卷挂起来，`/System/Library/Caches/
com.apple.dyld/` 里是 **54 个文件、3,987,406,848 B（3.71 GiB）**，`sharedRegionStart
= 0x180000000`、`sharedRegionSize = 3,428,777,984`、`maxSlide = 0x20000000`；
`cacheType = 2`（universal）、`cacheSubType = 1`（production）、`platform = 2`（iOS）、
`altPlatform = 0`、`simulator = 0`。主缓存 `dyld_shared_cache_arm64` 只有 **480 KiB**
（一个 mapping、475,136 B 的桩），`.01` 是 **114,720,768 B**，签名块
**224 KiB**（`sig=114,491,392+229,376`）。逐字段核对过头与 SuperBlob：CD
（`0xfade0c02`）的 `codeLimit == sigOffset`、hashType 2（SHA-256）、ident
`com.apple.dyld.cache.arm64.universal`，全部合规——**dyld 的头部检查没有理由拒绝
`.01`**。

于是“`.01` 打开之后停住”只剩一个解释，而且不在 dyld 里：在我们自己的块设备。
`drivers/CloverSdhci/CloverSdhci.cpp` 的 `readDataBlock` 每读一个 FIFO 字等
`frequency / 6250`（160 µs）。这个节奏来自 round98 的 mode 12——当时卡还在
400 kHz 的单比特识别时钟上，一个字确实要 80 µs；可实机参数是 `clover-emmc=21`，
不做识别、不改时钟，固件把卡留在 **HS200、8-bit、200 MHz**。在这个时钟上继续
每字等 160 µs，一个 512 B 块就是 **20.5 ms**，折算 **25 KB/s**；`.01` 的 224 KiB
签名块要读 **9 秒**，54 个文件的签名注册是分钟级。面板看起来“停了”，其实就是
数据在一字一字地爬。

### 改了什么

读路径（写路径不动，见下）：

- **`readDataBlock` 去掉 counter gap**：只在 `SDHCI_PRESENT_STATE` 的
  `DATA_AVAILABLE` 置位时从 `SDHCI_BUFFER` 取字，行尾再按同一个 level 位补扫一遍。
  SDHCI 在 FIFO 满时会对卡反压，所以按 level 位读永远不会跑过卡；200 ms 的
  counter deadline 保留，超时仍然返回失败。
- **新增 `sendStopCommand()`（CMD12）与 `readMultiBlock()`（CMD18）**：
  `BLOCK_COUNT=blocks`、`TRANSFER_MODE = READ | BLK_CNT_EN | MULTI`，数据相位
  的 deadline 是 200 ms + 10 ms/块，收尾**总是**发 CMD12。没有用 AUTO_CMD12：
  这块卡的 vendor 状态从来没被要求自动发过它，而 CMD12 的命令形状是前面每一轮
  都验证过的。
- **`clover_emmc_block_read` 改为分块**：每次最多 `kReadChunkBlocks = 32` 块
  （16 KiB）；`chunk >= 4` 先走 CMD18，失败就退回单块 CMD17。最坏情况与改动前
  完全一样，设备只会更能读。
- **`CloverBlock::doAsyncReadWrite` 批量下发**：静态 16 KiB `gBatchWords`，一次
  32 块；`gBatchBusy` 用原子 compare-exchange 保证两个调用者不共用缓冲区，抢不到
  的调用者继续走原来的逐块路径（栈上 512 B），所以并发时不会退化成失败。
  读、写、`gBlocksRead/Written` 的计数都保留。

诊断（不增加新的问题，只把 r155 的帧缩到屏幕装得下）：

- **why2 的 hdr dump 只留 main 与 `.01`**：54 个文件各六行会把帧尾的
  `map2 enter` / `fsig add` 顶出屏幕。新增 `clover_cache_focus()`，非重点文件的
  open 行只在**失败**时打印——第一个失败的子缓存仍会被点名。r153 的
  `open`/`read` 格式串原样保留。
- **why4 增加一行**：`ubc_cs_blob_add` 失败时打印
  `clover r156: fsig add err=%d`。以前 blob 被拒只会让帧静默，与“dyld 没问”
  无法区分。
- `check-clover.py` 新增两组形状断言：SDHCI 的读函数里不许再出现
  `const uint64_t gap` / `lastRead`、必须有 `SDHCI_DATA_AVAILABLE`、
  `readMultiBlock`/`sendStopCommand`/`kReadChunkBlocks`；CloverBlock 必须有
  批量缓冲、busy flag 和逐块回退。标题改为 `clover r156:`。
- `verify-clover-payload.py` 的 `check_cache_gate_probe` 增加 `fsig_add` 一条。

写路径（`writePioBlock`、mode 11 的识别 drain）**保留** 160 µs 节奏：写只发生在
logfs，量小，且单块写序列是前几轮逐一验证过的；这一轮只动读。

### 构建与刷写

```sh
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

只写 `cache`，冷启动：

```sh
fastboot flash cache /Users/zhouzhou/clover-out/round156-payload.img
fastboot reboot
```

### 这一帧怎么读

先看**速度**，再看门：

1. `open …dyld_shared_cache_arm64.01 … err=0`、`hdr .01` 三行、`fsig` 行在几秒内
   连续出现：节流已经去掉，缓存 preflight 在正常前进。
2. 出现 `fsig vp=… rd off=… len=…` 却迟迟没有 `fsig rd <-`：签名块读仍然卡。
   现在 `len` 与 `ms` 会给出一手读数；如果 `ms` 仍是几十秒，瓶颈就不在我们的
   PIO 循环，下一轮转查卡侧或签名 blob 的尺寸分布。
3. `clover r156: fsig add err=N`：blob 被内核拒，墙在签名结构，`N` 就是 errno。
4. `fsig hit=1`：这段 blob 已经注册过，签名不是墙。
5. `clover r154: map2 enter files=55 mappings=…`：`.01` 整段 preflight 通过，
   dyld 走进了 syscall 536；`map2 <- err=0` 表示映射成功，墙在 dyld 之后。
6. 既没有 `fsig` 也没有 `map2`，但有一条 `open … err≠0`：就是那个子缓存打不开；
   非重点文件只有失败时才打印，所以这条失败行不会被刷掉。

这一帧同时回答两件事：**读得有多快**，以及 **`.01` 之后是签名、blob 还是 536**。

产物归档在 `~/clover-out/round156-*`，`round156-SHA256SUMS` 八项：
`payload.img` 58,720,256 B（`dbe05f34…`）、本地 `kernel.macho` 51,998,720 B
（`b60664a1…`）、卷内 `kernel.macho`（`a4eb4758…`，r153/r154/r155/r156 四类探针
都在）、卷内 `unixnu.toml` 622 B（`f7b160f2…`）、ADT 13,516 B（`53328fa7…`）、
`scratch-data.img` 35,888 B（`614b8356…`）、`trustcache` 61,408 B
（`7c369ce0…`）、构建日志（`024631bb…`）。两个 checker 的结果：
`clover ok: ADT 13516 bytes, 8 cores; … 75 inline asm statements separated;
modules visible`、`kernel.macho: probe patch localized OK (6838 bytes in 35 runs)`、
`cache gate probe linked OK (… fsig_add present, r153/r154 forms kept)`、
`map-and-slide probe linked OK`、`cache open probe linked OK`。

## v0.5.10（round157）：让 fast path 的每句话都是真的

### 为什么这一轮只修 bug

r156 把 25 KB/s 的读节奏拿掉了，但那条新路有几处只在**出错时**才暴露的“读数
说谎”。面板上的帧就是这些路径写的；如果它们把失败报成成功，下一帧会把判断带到
错误的地方。这一轮不加探针，只把四个判断改成真的。

1. **CMD12 的失败不可见**：r156 的 `sendStopCommand()` 发完 CMD12 就清中断，
   不看响应。停止没完成时卡会留在 data mode，之后每次块读都超时，而帧上没有任何
   一行解释。现在 CMD12 按规范用 R1b（busy-check）发，等
   `SDHCI_INT_CMD_MASK`、检查 `SDHCI_INT_RESPONSE` 与超时/CRC/INDEX 位，
   200 ms counter 死线，失败重试一次，返回值交给调用者。
2. **`DATA_END` 旁边的错误位被忽略**：r156 的返回式只看 `DATA_END` 与字数。
   同一个 `INT_STATUS` 里若同时有 `DATA_CRC` / `DATA_TIMEOUT` / `DATA_END_BIT`
   / `ADMA_ERROR`，损坏的 512 B 会被当成干净数据交给上层。新增
   `kDataErrorMask`，任何错误位都否决这次传输；`readDataBlock`、
   `readMultiBlock`、`writePioBlock` 三处都过它。
3. **块偏移在 32 位里回绕**：`done * kBlockWords` 是 uint32 乘法，合法请求一旦
   超过 16 GiB，指针会折回缓冲区开头。改成
   `static_cast<uint64_t>(done) * kBlockWords`。当前调用者最多 32 块，但导出
   函数自己的契约允许整段卡。
4. **CloverBlock 的完成长度多报**：批量分支在读命令只读到一半、或
   `writeBytes` 失败时执行 `done += read` / `done += chunk`，把从未交给调用者的
   块算进 completion 的字节数。现在只有真正交付的读块才计入；写方向仍保留
   已落卡的 `written` 块，那些确实到了介质。

写路径（`writePioBlock`、mode 11 的识别 drain）仍保留 160 µs 节奏：写只发生在
logfs，量小，且单块写序列是前几轮逐一验证过的。

### 契约

- `check-clover.py` 把新的形状写死：`sendStopCommand` 的返回类型、
  `SDHCI_CMD_RESP_SHORT_BUSY`、`kDataErrorMask` 的使用、64 位偏移；反向断言
  CloverBlock 里不许再出现 `done += read;`。标题改为 `clover r157:`。
- `verify-clover-payload.py` 不需要新串：内核探针与 r153–r156 完全一致，这一轮
  只改驱动；`cache gate probe linked` 仍要求八条 r155 串与 r156 的
  `fsig add` 在场。

### 构建与刷写

```sh
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build kernel payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

只写 `cache`，冷启动：

```sh
fastboot flash cache /Users/zhouzhou/clover-out/round157-payload.img
fastboot reboot
```

### 这一帧怎么读

打印与 r156 完全相同，问题也不变：**`.01` 打开之后，dyld 是被头字段拒、被签名块
读拖住，还是已经跨进 syscall 536。** 这一轮保证的是：帧上的每一行现在都能自证。

1. `open .01 err=0` → `hdr .01` 三行 → `fsig rd off/len` → `fsig rd <- ms=`：
   缓存 preflight 正常前进；`ms` 是这段 blob 的真实耗时。
2. 有 `fsig rd off/len` 而没有 `fsig rd <-`：读仍卡住；`len` 是卡住的字节数。
   如果在 r156 帧里这里很快、而这一帧反而出现长时间停顿，先怀疑这块控制器对
   R1b 的完成时机；下一步把 CMD12 降回 R1 并只保留响应检查（`waitCommandSlot`
   本来就会等 DAT inhibit）。
3. `fsig add err=N`：blob 被内核拒，`N` 是 errno。
4. `fsig hit=1`：这段 blob 已注册，签名不是墙。
5. `map2 enter files=… mappings=…`：`.01` 整段 preflight 通过，dyld 进入了
   syscall 536；`map2 <- err=0` 表示映射成功。
6. 既没有 `fsig` 也没有 `map2`，但有 `open … err≠0`：失败的那个子缓存被点名。

产物归档在 `~/clover-out/round157-*`，`round157-SHA256SUMS` 八项：
`payload.img` 58,720,256 B（`263fa94c…`）、本地 `kernel.macho` 51,998,720 B
（`ec6149bc…`）、卷内 `kernel.macho`（`670ed61a…`）、卷内 `unixnu.toml` 615 B
（`aee4d4c5…`）、ADT 13,516 B（`b739a7ce…`）、`scratch-data.img` 35,888 B
（`614b8356…`）、`trustcache` 61,408 B（`7c369ce0…`）、构建日志 4,641 B
（`1f47b6f7…`）。两个 checker 的结果：
`clover ok: ADT 13516 bytes, 8 cores; … 75 inline asm statements separated;
modules visible`、`probe patch localized OK (6838 bytes in 35 runs)`、
`cache gate probe linked OK (… fsig_add present, r153/r154 forms kept)`、
`map-and-slide probe linked OK`、`cache open probe linked OK`。

## v0.5.11（round158）：照 sheng 对齐仓库契约，补一条锁超时安全网

### 这一轮改的是仓库，不是启动路径

把 `~/UniXNU-sheng` 近四十个提交逐条看过：除了仓库契约，其余几乎全是 SM8550
专属（Adreno 740、ath12k、ADSP、Novatek、sheng-* 启动参数、Data 卷）。真正
与 SoC 无关、clover 也该有的只有四份文件和一项配置：

1. **`porting.toml`**：上游 `scripts/porting-record.py`（CI 实际执行的那份）要求
   `schema_version = 1`、固定 13 个 area 全部存在、`device.name/soc/os_profile`
   与 `board.toml [board]` 对应键相等、每个 area 的七组字段都是非空字符串列表；
   `implemented` 必须有 evidence，`host-verified`/`device-verified` 必须有带
   40 位 revision 与 command 的同类 evidence，非 https 的 reference 必须是仓库
   内存在的文件。clover 的 13 个 area 里 configuration、firmware、cpu、memory、
   storage、power、packaging、diagnostics 记为 `implemented`；display、input、
   gpu、network、audio 记为 `planned`（sheng 的 SM8550 组件不能选，clover 还没
   有对应驱动）。planned area 也要显式写 `evidence = []`：校验器直接下标
   `area['evidence']`，缺键会抛 `porting record: 'evidence'`。
2. **`AGENTS.md`**：仓库所有权（`board.toml`、eMMC 存储、固件选择、ADT、
   payload 归本仓；共享 XNU 与组件目录归 UniXNU；固件仓是 `uefi-clover`）、
   必读文档、构建与双 checker、trust cache 只有三模块 bundle 被接受、只冷刷
   cache、绝不 `fastboot boot`、以及「storage 是当前活跃 area、墙是 dyld 共享
   缓存 preflight」。
3. **`.github/workflows/porting-record.yml`**：与 sheng 相同的四行 job，调用
   `UniXNU/UniXNU/.github/workflows/porting-record.yml@main`。
4. **`.gitattributes`**：照 sheng 的 LF 与二进制清单，补丁文件
   `-whitespace`，避免 CI 或编辑器改写 hunk。
5. **`[board] os_profile = "ipados-17.7.11-21H461"`**：校验器要求它与
   `porting.toml [device]` 相等，这不是新的构建输入，只是把两处已有的 IPSW
   标识写进板级描述。`check-clover.py` 现在断言这个值。

### `ml-timeout-lock-panic-scale=40` 不是 sheng 专属

`osfmk/arm/locks_arm.c:106` 声明
`MACHINE_TIMEOUT(lock_panic_timeout, "lock-panic", 0xc00000, …)`，`0xc00000`
个 24 MHz timebase tick 约 **524 ms**；`osfmk/kern/machine.c:1331-1345` 用
`"ml-timeout-" + name + suffix + "-scale"` 组 boot-arg 名，所以 40 把死线拉到
约 **21 s**。理由是本板 r156 自己的测量：CloverBlock 一次把 16 KiB（32 块）
交给控制器，在旧的逐字等待节奏（25 KB/s）下这次调用持缓冲区锁 **655 ms**，
已经超过默认死线——慢读会被报成死锁。sheng 出于同样原因传同一个值，内核其他
东西不变。`check-clover.py` 反向要求 shipped boot 必须带这一项。

### 不适用的 sheng 差异

- `sheng-*`（WiFi/GPU/CPU governor/touch/AKS/banner/vsync）：由 sheng 的板级
  驱动消费，clover 没有这些驱动。
- `offline-legacy-*`、`offline-hid-*`、`offline-key-events`：sheng 的
  `LegacyDisplayProbe.cpp`、`HidInputProbe.cpp`、`KeyStoreProbe.cpp` 消费。
- `-apfs_shared_datavolume`、`[payload.install]`：都需要 Data seed / Data 卷，
  clover 现在只有 OS + SystemOS。
- `CONFIG_XNU_NATIVE_UFS`、`VIRTIO_BLOCK`、`VERIFIED_ROOT`、
  `OWNER_DATA_MOUNT`、`BOOT_MANIFEST`、`DT_PANIC_REGION`、`ZONE_PGZ`：
  clover 走 `CloverSdhci` + `CloverBlock` 与自己的 owner 契约。
- `[components] include`：clover 用 archives + prelink roots。

### 构建与刷写

内核没有变，只重建 payload：

```sh
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

只写 `cache`，冷启动：

```sh
fastboot flash cache /Users/zhouzhou/clover-out/round158-payload.img
fastboot reboot
```

### 这一帧怎么读

与 r157 相同，这一轮不动探针，问题仍是：**`.01` 打开之后，dyld 是被头字段
拒、被签名块读拖住，还是已经跨进 syscall 536。** 与 r157 唯一的差别是锁死线
从 524 ms 变成 21 s，慢读不会再以 panic 的形式污染帧。

产物归档在 `~/clover-out/round158-*`，`round158-SHA256SUMS` 八项：
`payload.img` 58,720,256 B（`0fbb7e67…`）、本地 `kernel.macho` 51,998,720 B
（`ec6149bc…`）、卷内 `kernel.macho`（`670ed61a…`）、卷内 `unixnu.toml` 635 B
（`6514717a…`）、ADT 13,516 B（`049c8dbd…`）、`scratch-data.img` 35,888 B
（`614b8356…`）、`trustcache` 61,408 B（`7c369ce0…`）、构建日志 885 B
（`d6521036…`）。两个 checker 的结果：
`clover ok: ADT 13516 bytes, 8 cores; … 75 inline asm statements separated;
modules visible`、`probe patch localized OK (6838 bytes in 35 runs)`、
`cache gate probe linked OK (… fsig_add present, r153/r154 forms kept)`、
`owner SystemOS mount compiled in OK`、`owner root trust compiled in OK`；
`porting.toml` 另经上游校验器：`clover: 13 structured areas checked`。

## v0.5.12（round159）：卡跑在 50 MHz，并且带上 Data 卷

### 这一轮为什么是两件事

要求是先把 eMMC 驱动写完，再把 Data 卷一起解决。前几轮已经能读能写，但整场
启动都停在识别时钟、1 条数据线上——从来没有发过 CMD6；Data 卷则完全缺一块
可写的底层设备。两件事都必须在同一个内核里成立，因为 Data 的挂载要经过
eMMC 的批量读。

### eMMC：先让卡按它自己的速度跑

`CloverSdhci` 在 EXT_CSD 与几何读出之后、`gBlockReady` 抬起之前调用
`fastMode()`，按标准顺序做三步，每步都以卡的回答为门：

1. CMD6 SWITCH `HS_TIMING = 1`（EXT_CSD 字节 185），进入高速时序；
2. CMD6 SWITCH `BUS_WIDTH = 2`（字节 183），切到 8 条数据线；
3. `HOST_CONTROL` 置 `HISPD | 8BITBUS`，并把 GCC 的 `sdcc1_apps_clk_src`
   写成 50 MHz 那一行（`src 6, divider 11`，`kGccCfg50m = 0x0000060b`）。

最后读 LBA 1，要求两个 32 位字是 `0x20494645` / `0x54524150`（"EFI PART"），
才认为链路成立。任何一步被拒就 `programRootClock(true, kGccCfg400k, …)` 并
清掉 HISPD，退回前几轮验证过的 400 kHz 1 线——慢卡不能变成死启动。读数在
`CloverSdhci r159: link hs=… bw=… clk=… hc=… hdr=…/%x ok=…`。

批量路径同样是这一轮补的：读走 CMD18、写走 CMD25，一次 32 块（16 KiB），
收尾 CMD12 用 R1b 的忙线、200 ms 或 10 ms/块的死线；`kDataErrorMask` 把
`DATA_END` 同字里的 CRC/TIMEOUT/END_BIT/ADMA 与成功区分开。任何批量失败都
退回前几轮的单块 CMD17/CMD24，并且**每个方向只报一次**
`CloverSdhci r159: multi read|write fail lba=… blocks=… st=… w=…/…`，不再像
r156 那样静默降速。

顺带修了一处构建期问题：`drivers/CloverPMGR` 的 `clover_pmgr_observation`
标成 `alignas(128)`。panic 屏的探针脚本要求这个数组 128 字节对齐，本轮新增
代码改变了链接顺序，把它从对齐位置上挤开，`patch-xnu-entry-probe.py` 因此
拒绝内核。把对齐写进声明，就不再依赖链接器的运气。

### Data：第三扇窗，交给 owner 去挂

boot 用的 userdata GPT 里原本只有本仓自己声明的两颗分区（OS、SystemOS）。
`scripts/append-data-window.py` 在镜像尾部追加 32 MiB 种子
（`owner-data-32.cdr`，sha256 `ef00c083…`，卷 UUID
`FC1A580E-2D67-48FC-B8CF-87422A319340`），作为 SHNGROOT 的第三条 `Data`，
`flags = 0`（可写；打包器自带的条目都是 1）。它落在两份 GPT 之间；`[userdata]
windows` 仍只列 OS 与 SystemOS，因为那是上游打包器的声明，不是本板自己的。

`CloverBlock` 识别第三条目后，以 `windowsFromManifest(…, apfs == 3, apfs == 3)`
发布可写窗口，按 UUID 找到卷、等 BSD 名，导出
`native_ram_block_volume_seed()`：设备路径 `/dev/<bsd>`、字节数等于窗口大小、
digest 取条目里的 sha256；同时导出 manifest sha384。日志依次是
`CloverBlock r159: boot os=…+… system=…+… data=…+…`、`data vu=… bsd=…`、
`manifest …`、`seed data …`。

owner 侧保持 sheng 的原代码：`native_owner_data_prepare()` 校验尺寸、4096
对齐，以匿名 owner 99 把 apfs 挂到 `/private/var`；失败则
`offline scratch Data mount failed: %d` panic。`kern_exec.c` 的 GPU addon 是
弱引用，静态内核链接仍要求定义，因此 `CloverBlock` 同时提供
`native_ram_block_gpu_addon()`；没有 `sheng-gpu-addon=1` 时它立即回答“无
addon”，不碰 Data 镜像。

32 MiB 是 scratch，不是完整 `/data`；原用户数据分区不在这轮范围。

### 构建与刷写

内核变了，`build payload` 不会重编内核，必须先跑 `build kernel`：

```sh
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" ./unixnu build kernel
PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  ./unixnu build payload --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
cd ~/UniXNU-clover && scripts/check-clover.py && python3 scripts/verify-clover-payload.py
```

这一轮要动两个分区。`userdata` 是破坏性写入：整段替换为稀疏镜像，Data 的
写入只会落在追加的 32 MiB 窗口内，但原分区的其余内容会按种子重建。命令先
记录在这里，**尚未执行**；按长期约定，写入前需要明确同意：

```sh
fastboot flash cache /Users/zhouzhou/clover-out/round159-payload.img
fastboot flash userdata /Users/zhouzhou/clover-out/round159-userdata-root.sparse.img
fastboot reboot
```

### 这一帧怎么读

主问题是：50 MHz / 8 线是否真的成立，以及 Data 卷能不能挂上。

- `link hs=1/… bw=1/… hc=… hdr=20494645/54524150 ok=1`：快链路成立
  （`hs`/`bw` 的第二个数是那条 CMD6 的 INT_STATUS，非零表示命令确实回来过）。
- `ok=0`：已回退 400 kHz，启动继续，只是速度仍是旧值；此时 `hdr` 不再有
  意义。
- `CloverBlock r159: boot … data=<非零>+<非零>`：第三扇窗已被识别；
  `data vu=… bsd=…`：卷已找到并拿到设备名；`seed data … dev=N`：owner 拿到
  的种子；随后若挂载失败会 panic 并打印
  `offline scratch Data mount failed: <errno>`。
- `multi read fail` / `multi write fail` 每个方向最多一行；出现即表示批量被
  拒、已退单块，数据仍然正确。
- dyld 共享缓存那堵墙不在本轮解决；若仍走到同一条 `shared_region` 墙，属
  预期。

### 内核构建不是逐字节可复现

两次 `build kernel` 会得到不同的哈希：`emit_prelink` 每次生成新的 prelink
UUID，落在 `LC_UUID`、`__PRELINK_INFO` 与少量 `__LINKEDIT` 记录里。本轮两次
构建相差 81 字节，所有符号地址一致。因此
`verify-clover-payload.py` 的“探针补丁只落在本地”改为只在
`__TEXT_EXEC` 与 `__PLK_TEXT_EXEC` 两个可执行段里比较字节（补丁实测 6838
字节、35 段），可执行段之外的差异计数后忽略。归档里的本地 `kernel.macho`
是 payload 的精确输入（`8d24b98c…`）；重编得到 `948ecc1f…` 属正常。

产物归档在 `~/clover-out/round159-*`，`round159-SHA256SUMS` 十一项：
`payload.img` 58,720,256 B（`b77e7368…`）、本地 `kernel.macho` 51,998,720 B
（`8d24b98c…`）、卷内 `kernel.macho`（`82c35e51…`）、卷内 `unixnu.toml`
634 B（`8e5d6481…`）、ADT 13,516 B（`2555023c…`）、`scratch-data.img`
35,888 B（`614b8356…`）、`trustcache` 61,408 B（`7c369ce0…`）、userdata
稀疏镜像 9,416,215,880 B（`60a86a38…`）、构建日志 3,840 B
（`9659c2f3…`）、payload 补丁日志 777 B（`d4275ce6…`）、双 checker 日志
5,346 B（`277fcf0a…`）。两个 checker 的结果：`clover ok: ADT 13516 bytes,
8 cores; … 75 inline asm statements separated; modules visible`、
`probe patch localized OK (6838 bytes in 35 runs, 81 outside executable text
ignored)`、`CloverSdhci linked OK`、`CloverBlock linked OK`、
`owner SystemOS mount compiled in OK`（含新的
`offline scratch Data mount failed: %d` 与 `native_ram_block_volume_seed`）、
`owner root trust compiled in OK`。

## v0.5.13（round160）：刷 userdata 之前，先在宿主上把它读一遍

### 为什么这一轮只动宿主

`fastboot flash userdata` 是这台板子上唯一会整段覆盖 54.4 GB 分区的动作，而
round159 的镜像是 9.4 GB 的 all-RAW 稀疏文件：三扇窗（OS、SystemOS、Data）写在
一个 SHNGROOT 清单里，Data 的 32 MiB 种子是事后追加的，清单也是事后重写的。
清单少一条、`flags` 不是 0、种子被改过、镜像超出分区——这几种错误 fastboot 都
会照写不误，要等内核 panic 时才知道。所以这一轮不刷机、不重编内核，也不重打
镜像，只补一个「写之前先读一遍」的宿主闸门。

### 它检查什么

`scripts/verify-userdata-window.py` 按 flasher 的方式展开稀疏容器（28 字节文件
头 + 每段 12 字节块头，只接受 `RAW`），然后检查：

- 容器：magic、版本 1、头尺寸 28/12、4096 字节块、各段展开后与头部声明的
  `total_blks` 一致、文件长度恰好等于各段之和；
- 清单：`SHNGROOT`、版本 1、条目恰好是 `OS` + `SystemOS` + `Data`、清单自己在
  第 4064 字节的 sha256；每条 512 字节块、扇区对齐、落在镜像内；
- Data 这一条：`flags = 0`（可写，另外两条是 1）、32 MiB、sha256 等于钉住的
  种子，并且必须是最后一扇窗（后面只剩备份 GPT 与卡尾）；
- 种子本身：偏移处字节的 sha256、块 0 偏移 32 的 `NXSB`、页对齐的 APFS 块大小；
- `--crc`：对展开后的 9.4 GB 重算 CRC-32，与稀疏头里的值比对；
- `--apfs`：把种子落到临时文件，`hdiutil` 只读挂上，再向 `diskutil` 要
  `VolumeUUID`，证明内核要搜的那个 UUID 真在这串字节里，而不是只在 seed.json 里。

`PARTITION_BYTES` 由 `board.toml` 的 `[userdata]` 推出：
`(0x747bfde - 0xf10520 + 1) * 512 = 54,449,765,888`，镜像超过它就拒绝。

### 实跑结果

对 round159 的归档跑 `--crc --apfs`：

```
verify-userdata-window: crc32 0x6c29b693 over 9416212480 bytes
verify-userdata-window: APFS volume UUID FC1A580E-2D67-48FC-B8CF-87422A319340
verify-userdata-window: PASS: 9416212480 bytes, 3 windows, Data 33554432 bytes at 0x22f400000, seed sha256 ef00c083fc54453c...
```

也就是：稀疏文件 9,416,215,880 B、281 个 RAW 段，展开后 9,416,212,480 B；三扇
窗；Data 33,554,432 B 起于 `0x22f400000`；种子 sha256 `ef00c083…`；容器
CRC-32 `0x6c29b693` 与头部一致；只读挂载得到卷 UUID `FC1A580E-…`。四项都与
round159 构建时的记录相符。

### 怎么跑

```sh
cd ~/UniXNU-clover
scripts/verify-userdata-window.py \
  --image ~/clover-out/round159-userdata-root.sparse.img --crc --apfs
```

`--crc` 要顺序读 9.4 GB（几十秒），`--apfs` 需要宿主允许 `hdiutil` 挂载；不加
这两个开关时只做清单与种子的原地检查，秒级完成。`--json <path>` 可以把验证过的
事实写成文件。

### 这一轮不动镜像

内核、payload、根镜像、trustcache、userdata 稀疏镜像、ADT 都没有重编，
`~/clover-out/round159-*` 的十一项哈希与 v0.5.12 相同。`scripts/check-clover.py`
新增一条 Round160 检查：闸门脚本必须在，且必须钉住 32 MiB、种子 sha256、卷
UUID，必须拒绝非 RAW 段，必须要求 `Data/OS/SystemOS` 三窗，必须要求 Data 是
最后一扇窗（`offset + size` 必须正好落在镜像末尾，备份 GPT 的区域不许被覆
盖）。`porting.toml` 的 storage 区把脚本列进 `paths` 与 `procedure`，并在
`findings` 里记下本轮的宿主读数。

### 下一帧仍然只问一件事

冷刷 round159 的 cache payload，读三种行：`CloverSdhci r159: link …` 证明 50 MHz
8 线是否被卡接受，`CloverBlock r159: data vu=… bsd=…` / `seed data …` 证明 Data
窗与 owner 种子是否到位，以及 dyld 是否越过 `.01` 预检。userdata 的写入是破坏性
动作，命令仍只记录、未执行，需要单独同意：

```sh
fastboot flash cache /Users/zhouzhou/clover-out/round159-payload.img
fastboot flash userdata /Users/zhouzhou/clover-out/round159-userdata-root.sparse.img
fastboot reboot
```

## v0.5.14（round160 修订）：命中的每个 area 都要记录

上游 `scripts/porting-record.py check --base` 有一条容易漏的规则：一个改动的
文件如果同时匹配多个 area 的 `paths`，**每个** 命中的 area 的 `findings` 或
`evidence` 都必须相对 base 有变化。`scripts/check-clover.py` 同时属于
`firmware` 与 `diagnostics`——v0.5.13 只在 `diagnostics` 里写了 Round160 的
句子，于是 main 上的 `Device documentation` 报
`porting record: scripts/check-clover.py: update firmware findings or evidence
with this change`。这一版只补 `firmware` 的 `findings`：闸门脚本丢了钉住的
种子或三窗规则，应当让固件构建失败，而不是等到破坏性的刷写之后才发现。

这一版没有重编镜像。它发布时 round155–round160 都还没上机；此后 round159 的
镜像已经冷刷过（见下面 v0.5.15 那一帧），round155–round158、round160 仍然没有
独立的真机帧。

## v0.5.15（round159 的第一帧）：Data 挂上了，dyld 在读整组缓存

这一帧来自 round159（v0.5.12）的 cache payload 与 userdata 镜像。屏幕从 APFS
挂载开始，关键行按顺序是（部分数字被照片边缘切掉，用 `…` 表示）：

```
CloverBlock r159: manifest os vp=… uuid=… off=1048576 size=…
CloverBlock r159: manifest system vp=… uuid=… off=8077… size=…
apfs_vfsop_mount:3091: disk5t mount-complete volume DawnData, requested by: init (pid 1)
load_init_program: attempting to load /sbin/launchd
clover_mac: label-check 881 (Apple Mobile File Integrity) -> 1
clover_mac: label-check Sanctum (Sandbox, sandbox policy) -> 1
clover r153: check_np dyld_flags=0x0
shared_region: … check_np(…) vm_shared_region_start_address() failed
clover r153: open /System/Library/Caches/com.apple.dyld mode=0x1 err=0 … fs=apfs
clover r153: open dyld_shared_cache_arm64 mode=0x1 err=0 … fs=apfs
clover r153: read err=0 resid=0 hdr=dyld_v1   arm64
clover r155: hdr main … sig=475136+16384 err=0/0
clover r155: fsig vp=… start=0 blob=475136 size=16384
clover r155: fsig rd <- err=0 resid=0 ms=7
clover r153: open dyld_shared_cache_arm64.01 mode=0x1 err=0 … fs=apfs
clover r155: hdr .01 … len≈1.07 GiB sig=11491332+229376 err=0/0
clover r155: fsig rd off=11491332 len=229376
clover r155: fsig rd <- err=0 resid=0 ms=189
（之后每个子缓存一组：fsig vp= / mem= / rd off=… len=… / rd <- err=0 resid=0）
```

读法：

- `CloverBlock r159: seed data … bsd=disk5s1 … bytes=33554432` 加
  `mount-complete volume DawnData`，再对卷 UUID
  `FC1A580E-2D67-48FC-B8CF-87422A319340`，三件事一起证明 Data 窗、种子与
  owner `/private/var` 挂载全对；`offline scratch Data mount failed` 没有出现。
- 主缓存和 `.01` 的 open 都是 `err=0`，两个头都是 `dyld_v1   arm64`；`.01`
  的签名偏移 `11491332` 与 224 KiB 长度，和宿主上的量法一致。
- 之后每四行一组是一个缓存文件的 `F_ADDFILESIGS_RETURN`。`err=0 resid=0`
  表示这一组的签名 blob 读成功；`vp=` 与 `blob=` 每组都在变，说明在换文件，
  不是同一行重印或重试。
- 16 KiB 读 7 ms、224 KiB 读 105–189 ms，约 1–2 MB/s，比 round156 之前的
  25 KB/s 快 40–80 倍。round159 的高速链路、8 线与 CMD18 批量就是被这组数
  证明的。

还没看到的是 syscall 536 与映射结果。屏上两行
`check_np … vm_shared_region_start_address() failed` 是 exec 建的空区
（`sr_first_mapping=-1`），r153 就解释过，不是新错误。

怎么判断「在走」还是「卡住」：看最新一行的 `vp=` 与 `blob=`。变了就是在处理
下一个缓存文件；同一组 `fsig` 连续几分钟不动，才是卡在某次读或锁上。这一帧
的 fsig 组数上限就是缓存文件数（宿主上 21H461 的缓存目录是 54 个文件），按上面
的速率应是秒到几十秒级；控制台逐行打印会让它看起来比实际慢。

这一帧没有重编任何东西，`~/clover-out/round159-*` 的十一项哈希不变。

## v0.5.16（round159 的第二帧）：launchd 起来了，守护进程在 AMFI 上循环

同一个 round159 镜像继续跑了一个多小时之后的屏幕（照片是竖着拍的，行按阅读顺序
转正；部分数字被边缘切掉）：

```
unable to create a validated cs_blob object: 85
proc 13675: load code signature error 4 for file "logd"
AMFI: hook..execve() killing pid 13675: dyld signature cannot be verified. You either have a corrupt system image or are trying to run an unsigned application outside of a supported development configuration.
…
proc 13662: load code signature error 4 for file "dasd"
AMFI: hook..execve() killing pid 13662: dyld signature cannot be verified. …
```

照片里那个数字的末两位是 `05`，斜着看不排除首位被挡住。这条 printf 只有一个
来源（`ubc_subr.c:4687` 打印 `cs_blob_init_validated()` 的返回值），而它的两条
校验路径（`cs_validate_csblob` / `cs_validate_codedirectory`）只可能返回
`EBADEXEC`（85）。所以读数按 85 记。

读法：

- `launchd` 不再停在 `load_init_program`：它在跑，而且会拉子进程（`logd`、
  `dasd`）。这是第一帧没有的东西。
- 每个子进程的 exec 都在同一个地方断：主二进制的 `LC_CODE_SIGNATURE` blob 读进
  来后结构校验失败（85），`ubc_cs_blob_add` 返回，`load_code_signature` 报
  `LOAD_FAILURE`（4）；进程因此没有 `CS_PLATFORM_BINARY`，AMFI 的 exec 钩子在
  后面按「dyld signature cannot be verified」杀掉它，`launchd` 再拉，pid 一路
  上涨（13621 → 13662 → 13673 → 13675）。**这是循环，不是慢，不会自己结束。**
- 这一轮在宿主上把四个 cdhash 对了一遍（都从 `044-76040-038.dmg` 的 OS 卷算）：
  `logd 4882a798…`、`dasd db318f4f…`、`dyld b479f90e…`、`launchd 1d52c697…`。
  四个全在 2478 条的 static 信任缓存里，也都在
  `build/clover-trust/trustcache.bundle` 里。所以**不是信任决策的问题**：
  `ubc_subr.c:4685` 的结构校验在 `mac_vnode_check_signature`（4756）之前，
  信任缓存还没轮到。
- 同一个宿主上这些 blob 都合法：logd 的签名在文件偏移 224912、长 21120，头是
  `fade0cc0`、内部长度 3108、5 个索引；dasd、dyld、launchd 形状相同。内核读回来
  却过不了同一段校验，说明**读到的字节，或者 load command 给的偏移/长度**不对，
  不是签名本身不对。
- 每环都很贵：exec 会先启动 dyld、把整组共享缓存签名注册一遍（就是第一帧那串
  `fsig`），然后才轮到 AMFI 杀。所以一小时只涨了几十个 pid。

下一轮（round163）只问一件事：在 `load_code_signature` 的 `vn_rdwr` 之后打印
`v_name`、`macho_offset`、`dataoff`、`datasize`、`error/resid` 和缓冲区头 16 字节，
看内核到底读到了什么。一个镜像，一个问题。

这一帧同样没有重编任何东西，`~/clover-out/round159-*` 的十一项哈希不变。

## v0.5.17（round163）：把 AMFI 的 exec 钩子改成永远放行

上一帧把问题切得很干净：内核自己的签名校验失败（85）之后，exec 仍然走到了
AMFI，是 AMFI 的 exec 钩子按「dyld signature cannot be verified」把进程杀掉。
既然 `cs_enforcement_disable=1` 已经让内核不计较签名，剩下的唯一闸门就是
kext 里那一个分支。`amfi_get_out_of_my_way` 到不了它——AMFI 是 stock kext，
没有解析这个参数的代码（round139 的注释）；所以这一轮直接改它的指令。

钩子的形状（`__PLK_TEXT_EXEC`，VA `0xfffffff004ab40d0` 起）：

```
fffffff004ab40d0  ldr  x24, [x29, #0x18]      ; cs flags
fffffff004ab40d4  bl   <check>                ; w0 = 结果
fffffff004ab40d8  ldr  w8, [x24]
fffffff004ab40dc  and  w9, w8, #0x2000        ; CS_REQUIRE_LV
fffffff004ab40e0  orr  w9, w0, w9
fffffff004ab40e4  cbz  w9, 0xfffffff004ab4128 ; ← 放行
fffffff004ab40e8  tbnz w8, #0x19, allow       ; CS_NO_UNTRUSTED_HELPERS
…                  打印 kill 原因，返回
fffffff004ab4128  …                            ; 放行后的普通路径
```

把 `cbz`（`34 00 02 29`）换成 `b`（`11 00 00 14`），等于把 `check` 的结果和
`CS_REQUIRE_LV` 一起忽略，永远跳到 `0xfffffff004ab4128`。映射关系：
`__PLK_TEXT_EXEC` 的 VA `0xfffffff004920000` 对应文件 `0x18ef000`，所以
VA `0xfffffff004ab40e4` 是 `kernel.macho` 的文件偏移 **`0x1a830e4`**（改前
字节 `29 02 00 34 e8 00 c8 37 e0 03 14 aa bd 58 00 94`，改后
`11 00 00 14 …`）。

只改 payload，不重编内核：

```sh
cd ~/UniXNU && PATH=$HOME/UniXNU-clover/build/shim:$PATH \
  SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" \
  /opt/homebrew/bin/python3.13 ./unixnu build payload --unique-chip-id 0x81ffcaaa \
  --uefi ../mu-clover --kernel /Users/zhouzhou/clover-out/round163-kernel-amfi.macho \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
```

`--kernel` 指向的是 prelink 输出的一份拷贝（`out/clover/kernel/kernel.macho`
→ 改 4 字节），payload 步骤仍照常在它的副本上打入口探针，所以两道校验都能
过：`probe patch localized` 报 `6841 bytes in 36 runs`（比上一轮多一个 run，
就是这 4 个字节），其余契约全 OK。菜单标题同时升到
`clover r163: the AMFI exec hook always takes the allow path`，产物：
`~/clover-out/round163-payload.img`（56 MiB，sha256 `c6efe71a…`）。

写入只有一个分区，cache：

```sh
fastboot flash cache /Users/zhouzhou/clover-out/round163-payload.img
fastboot reboot
```

`userdata` 不动——round159 的 Data 窗口仍在。这是冷刷，不用 `fastboot boot`。

这一轮预期看到的是：同样的 85 / `load code signature error 4` 还会印（根因没
修），但 AMFI 的 kill 行应该消失，`launchd` 的子进程不再被立刻杀掉。如果之后
出现 dyld 自己的错误或直接回到 fastboot，那就说明墙已经从 AMFI 移到共享区
映射；如果屏幕停在某个 daemon 的日志上，那 exec 这一层就通了。

## v0.5.18（round164）：16 KiB 签名的缓存不再整文件哈希，AMFI 第二条拒绝分支 NOP

r163 的冷刷读数是：85 和 `load code signature error 4` 照旧，AMFI 的 kill
行也照旧，pid 继续涨。回看那段钩子才发现改错了分支：`0xab40e4` 的 `cbz`
只合并 `check` 的结果与 `CS_REQUIRE_LV`；当 `check` 失败且没有
`CS_REQUIRE_LV` 时 `w9 = check | 0` 并不为零，`cbz` 不会跳。真正决定生死
的是后面 `0xab416c` 的 `tbz w0, #0, allow`——`w0` 就是签名检查的结果，
0 表示「没有有效签名」，于是它反而落到 `0xab4210` 的 kill 路径。

这一轮改两处：

1. AMFI 钩子 `0xab416c`：`tbz w0,#0`（`20 05 00 36`）→ `nop`
   （`1f 20 03 d5`），文件偏移 `0x1a8316c`。两条拒绝分支现在都不成立，
   剩下的 `cbz x20` 出口（`0xab4254`/`0xab428c`）都走到
   `0xab43d4`/`0xab43f0` 并把 `w0` 置 0，也就是放行。
2. 内核 `patches/xnu-clover/zzy-clover-dyld-cache-skip.patch`：在
   `cs_native4k_prepare` 里，当 `PAGE_SHIFT == 12` 且 blob 的 hash
   pageshift 为 14 时置 `csb_native4k_skipped` 并返回 0，不再用 `vn_rdwr`
   把整个 `codeLimit` 读一遍；`cs_validate_hash` 对标记过的 blob 把缺页
   直接当作已处理（`bytes_processed = PAGE_SIZE`、`tainted = 0`）。共享缓存
   是 3.7 GiB，在这块板 1–2 MB/s 的读速下，整文件哈希是分钟级，而且每次
   exec 都可能重来。

两处改动的源码都在板级仓库里：补丁同时抄到 `~/UniXNU/patches/xnu/`，否则
内核构建读不到；AMFI 的两条分支由 `scripts/patch-clover-amfi.py` 按唯一
签名定位，`--check` 可以只读复验。编译后的镜像里能读到三段证据（
`prepare` 的 `cmp/ccmp/b.eq` + `strb [x1,#0xd0]`、`validate_hash` 的
`ldrb [x0,#0xd0]` + `tbz` + 接受体），`verify-clover-payload.py` 现在把
这三段一起断言，`check-clover.py` 也反向断言补丁、patcher、verifier 三份
源码都在。

构建链（先内核、再 payload）：

```sh
cd ~/UniXNU && PATH=/tmp/clover-bin:$PATH \
  /opt/homebrew/bin/python3.13 ./unixnu build kernel --device ../UniXNU-clover -j 8
PATH=/tmp/clover-bin:$PATH /opt/homebrew/bin/python3.13 \
  ~/UniXNU-clover/scripts/patch-clover-amfi.py \
  ~/UniXNU/out/clover/kernel/kernel.macho \
  ~/clover-out/round164-kernel-amfi.macho
PATH=/tmp/clover-bin:$PATH \
  /opt/homebrew/bin/python3.13 ./unixnu build payload --unique-chip-id 0x81ffcaaa \
  --uefi ../mu-clover --kernel /Users/zhouzhou/clover-out/round164-kernel-amfi.macho \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
```

产物与读数：prelink 内核 `0f28d236…`；打过 AMFI 的拷贝 `bd7ce2dc…`
（`0x1a830e4`：`29 02 00 34` → `11 00 00 14`；`0x1a8316c`：
`20 05 00 36` → `1f 20 03 d5`）；payload `2ec8a37d…`（56 MiB），卷内
kernel `aa53af16…`、`unixnu.toml` `3b57cedc…`、ADT `89f425cb…`、
`scratch-data.img` `614b8356…`、trustcache `7c369ce0…`。入口探针仍然是
`6845 bytes in 37 runs`；菜单标题 `clover r164: the dyld caches skip the
whole-file hash, AMFI allows exec`。

写入只有一个分区，cache：

```sh
fastboot flash cache /Users/zhouzhou/clover-out/round164-payload.img
fastboot reboot
```

`userdata` 不动，round159 的 Data 窗口仍在；这是冷刷，不用 `fastboot boot`。

预期读数：AMFI 的 kill 行消失，85 仍在（根因未修）。之后要么是 dyld 自己
的签名错误，要么是 536/共享区映射的读数，要么用户态日志继续往下走。若
dyld 仍以签名失败拒绝，下一轮直接量 `ubc_cs_blob_add` 读回的 blob 长度
（`logd` 期望 21120；只到 16384 就是 16 KiB 批量读的尾块问题）。

## v0.5.19（round165）：每次读都从空 FIFO 开始，批量读必须等于单块

r164 的冷刷读数把两件事同时说清了：

1. AMFI 的 kill 行没了，第二条拒绝分支的 NOP 生效；85 和
   `load code signature error 4` 对每个子进程（locationd、bluetoothd、
   searchpartyd、PowerUIAgent、sysdiagnosed、mediaserverd）照旧重复，
   pid 继续涨，最后 `mediaserverd` 以 `Too many corpses being created` 崩。
2. APFS 自己报完整性错误，这是第一次有会自我校验的读证据：
   `apfs_jhash.c:146: disk0s1 Data hash mismatch for 16384 bytes at
   offset 12128256 in compressed data stream 102 of inode 162: expected
   jhash 2e6c78f0…, got 0d13…`，随后 `Verification failed with error 94`、
   `apfs_decomp.c:1642: decompress_pagein_compressed: err 94`、
   `apfs_pagein_with_verification: … Corrupt read data for compressed
   inode 162/162`、`apfs_pagein: … I/O 162/162 … err 94`。帧里那个 102 是
   **数据流号**，不是字节数；块层交给 APFS 的 16 KiB 与盘上的不一致。

这一轮不动签名、不动 AMFI，只修读路径：

1. 每条读命令之前 `drainReadFifo()`：`PRESENT_STATE` 里
   `DATA_AVAILABLE` 还在就先把 `BUFFER` 读空；排不干净就
   `SDHCI_RESET_DATA`（`SOFTWARE_RESET` 0x04），重置后再放行。理由是
   一次提前结束的传输会把字留在 FIFO 里，下一条命令先吃掉旧字，数据整体
   错位而 CRC 仍然正确。
2. 任何一次 `readDataBlock` / `readMultiBlock` 失败（含响应错、尾块、错误
   位）都在返回前 `resetReadPath()`，成功路径在收尾后也排空一次。
3. 第一次满批读做一次性 A/B 对照：对同一个 16 KiB 先读两次 CMD18 批量，
   再用 32 次 CMD17 单块读一遍；三者逐字比较，任一不一致就
   `gReadBatchTrusted = false`，本轮启动剩下的读全部走单块。屏上打印
   `CloverSdhci r165: readcheck lba=… batch=…/… single=… diff=…/…
   first=…/… m=… s=…`；不一致时再加一行
   `CloverSdhci r165: batch reads disabled for this boot`。

源码与契约：`drivers/CloverSdhci/CloverSdhci.cpp`（`drainReadFifo`、
`resetReadPath`、`prepareReadPath`、`checkReadPath`，读循环条件
`chunk >= 4 && gReadBatchTrusted`）；`scripts/verify-clover-payload.py`
新增 `check_read_integrity`，断言镜像里两行读数串都在；
`scripts/check-clover.py` 的 Round165 段逐条断言这些函数与串，并断言标题是
`clover r165:`。菜单标题：`clover r165: every read starts from an empty
FIFO, and the batch must match single`。

构建链（先内核、再 payload）：

```sh
cd ~/UniXNU && PATH=/tmp/clover-bin:$PATH \
  /opt/homebrew/bin/python3.13 ./unixnu build kernel --device ../UniXNU-clover -j 8
PATH=/tmp/clover-bin:$PATH /opt/homebrew/bin/python3.13 \
  ~/UniXNU-clover/scripts/patch-clover-amfi.py \
  ~/UniXNU/out/clover/kernel/kernel.macho \
  ~/clover-out/round165-kernel-amfi.macho
PATH=/tmp/clover-bin:$PATH \
  /opt/homebrew/bin/python3.13 ./unixnu build payload --unique-chip-id 0x81ffcaaa \
  --uefi ../mu-clover --kernel /Users/zhouzhou/clover-out/round165-kernel-amfi.macho \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
```

构建坑：`out/clover/tree/.cache/xnu-work` 会保留上一轮「全部补丁已打」的
状态，而基线脚本要求逐条反向校验；当最新补丁与 `native-4k-signature-hash-
cache.patch` 压在同一段上下文时，两个方向都失败。做法是先全部反向撤掉，
再按名字顺序打到最新一条之前，让构建自己补上最后一条。清理时不要连
`EXTERNAL_HEADERS/TrustCache/` 和 `iokit/DriverKit/*.h` 一起删——归档步骤比
基线脚本先跑，它们需要提前存在。

产物与读数：prelink 内核 `8a2d2bbf…`；打过 AMFI 的拷贝 `54875b4b…`
（仍是 `0x1a830e4`：`29 02 00 34` → `11 00 00 14`；`0x1a8316c`：
`20 05 00 36` → `1f 20 03 d5`）；payload `31f2572d…`（56 MiB），卷内
kernel `12d0d834…`、`unixnu.toml` `aa31b36a…`、ADT `99afed39…`、
`scratch-data.img` `614b8356…`、trustcache `7c369ce0…`。入口探针仍是
`6845 bytes in 37 runs`；`verify-clover-payload.py` 的
`kernel.macho: eMMC read integrity` 为 OK。

写入只有一个分区，cache：

```sh
fastboot flash cache /Users/zhouzhou/clover-out/round165-payload.img
fastboot reboot
```

`userdata` 不动，round159 的 Data 窗口仍在；这是冷刷，不用 `fastboot boot`。

预期读数：先找 `CloverSdhci r165: readcheck`。`diff=0/0` 说明两次批量与
32 次单块一致（读路径本身没在错，怀疑转向 blob 偏移/长度）；`diff≠0` 或
出现 `batch reads disabled for this boot` 则说明批量读确实在交坏字节，
本轮后面的读已经全部退回单块，APFS 的 jhash 错应当随之改变。无论哪种，
`unable to create a validated cs_blob object: 85` 是否消失就是这一刷的答案。

## v0.5.20（round166）：整轮单块读，exec 侧直接打印签名 blob 的头四字

第二帧把读路径的嫌疑收窄到了一个尺寸：APFS 的 jhash 错这次落在
`disk3s1` inode 182/182、压缩流 103、偏移 `11747328`，同样是 16 KiB，
`11747328 = 717 × 16384`，正好是一条 32 块 CMD18 的边界。两次错都
出现在 16 KiB 整数倍上，而不是任意偏移，说明坏字节更可能来自批量读，
而不是签名层。宿主复查已经把镜像这一侧排除：OS 窗口能挂载，六个卷
读出约 6 GB，`launchd`（536128+25728）、dyld（911680+25376）、trustd
（708384+25936）的 `LC_CODE_SIGNATURE` 都以 `fa de 0c c0` 开头且长度
自洽；所以这一轮只做两件事：

1. **整轮不再用 CMD18**：`gReadBatchTrusted` 初值改为 `false`，boot
   全程走 r156 之前的单块 CMD17 序列。r165 的 A/B 对照保留，但只作为
   一次性读数：`CloverSdhci r166: readcheck lba=… batch=…/… single=…`，
   随后打印 `CloverSdhci r166: single-block reads for this boot`，不再
   因为对照一致就把批量读打开。
2. **补上最后一个漏掉的复位**：`readDataBlock` 的命令响应被拒
   （TIMEOUT/CRC/END_BIT/INDEX 或没有 RESPONSE）时也调用
   `resetReadPath()`；之前这条拒绝路径直接返回，下一条命令可能落在
   半截数据通路上。
3. **exec 侧读数**：新补丁 `zzy-clover-exec-sig-bytes.patch` 在
   `load_code_signature` 的 `vn_rdwr` 成功之后、`ubc_cs_blob_add` 之前，
   打印前八个签名 blob 的 `macho_offset`、`dataoff`、`datasize` 和头四
   个 32 位字：`clover r166: sig n=%u mo=%lld off=%u size=%u
   w=%08x/%08x/%08x/%08x`。若头字是 `fade0cc0` 且长度自洽而 85 仍在，
   墙就在 `cs_validate_csblob` 的判定上，下一轮改 `ubc_subr.c`；若头字
   不是 `fade0cc0` 或长度对不上，读路径仍然是根因。

构建链（先内核、再 payload）：

```sh
cd ~/UniXNU && PATH=/tmp/clover-bin:$PATH \
  /opt/homebrew/bin/python3.13 ./unixnu build kernel --device ../UniXNU-clover -j 8
PATH=/tmp/clover-bin:$PATH /opt/homebrew/bin/python3.13 \
  ~/UniXNU-clover/scripts/patch-clover-amfi.py \
  ~/UniXNU/out/clover/kernel/kernel.macho \
  ~/clover-out/round166-kernel-amfi.macho
PATH=/tmp/clover-bin:$PATH \
  /opt/homebrew/bin/python3.13 ./unixnu build payload --unique-chip-id 0x81ffcaaa \
  --uefi ../mu-clover --kernel /Users/zhouzhou/clover-out/round166-kernel-amfi.macho \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
```

构建坑与 round165 相同：归档步骤要求整棵 `xnu-work` 已打全补丁，而内核
步骤要求 `native-4k-signature-hash-cache.patch` 可反向校验，所以先把
`zzy-clover-dyld-cache-skip.patch` 反向撤掉再跑内核；payload 的
`--trust-cache` 必须用 `build/clover-trust/trustcache.bundle`
（`7c369ce0…`），`build/clover-root/trustcache.bundle` 是 56 字节单模块，
会被 board.toml 的哈希闸门拒绝。`--kernel` 同理指向打过 AMFI 的拷贝，
否则新 prelink 会把 r163/r164 的两处分支恢复成 stock。

产物与读数：prelink 内核 `e8b6570c…`；打过 AMFI 的拷贝 `986d6ab2…`
（`0x1a830e4`：`29 02 00 34` → `11 00 00 14`；`0x1a8316c`：
`20 05 00 36` → `1f 20 03 d5`）；payload `9092782b…`（56 MiB），卷内
kernel `b897038d…`、`unixnu.toml` `dac1ddfc…`、ADT `7645659b…`、
`scratch-data.img` `614b8356…`、trustcache `7c369ce0…`。入口探针
`6844 bytes in 37 runs`；`verify-clover-payload.py` 21 项全 OK，包括
`kernel.macho: AMFI exec hook allows`、`eMMC read integrity` 和新的
`exec signature dump`。归档在 `~/clover-out/round166-*`，清单
`round166-SHA256SUMS`。

写入只有一个分区，cache：

```sh
fastboot flash cache /Users/zhouzhou/clover-out/round166-payload.img
fastboot reboot
```

`userdata` 不动，round159 的 Data 窗口仍在；这是冷刷，不用 `fastboot boot`。

这一刷回答一个问题：`clover r166: sig` 的头四字是不是 `fade0cc0`、
`size` 是否等于 blob 自己的长度字段。APFS 的 16 KiB jhash 错应当随
CMD18 一起消失；如果它还在，而且 `sig` 行也不对，那读路径里还有第二个
坏源（PIO 单块本身或块层拷贝），下一轮就量单块读自己的 A/B。若 jhash
消失、`sig` 行正确，85 仍在，则签名结构校验的输入没问题，下一刀落在
`ubc_subr.c` 的判定，而不是存储。

## v0.5.21（round167）：写要等卡说完成，写缓存关掉，批量读必须三份样本一致

r166 的冷刷把故障面从「读回错字节」挪到了「可写容器的 I/O」。这一帧里
`launchd_cache_loader[22]` 以 `namespace 2 subcode 0xa`（SIGBUS）退出，
initproc 一死就 panic；屏上 APFS 反复报
`apfs_scan_verify:2467: disk0s1 failed to read …`，随后
`Container corruption detected by apfs_scan_verify`，中间是 persona 1000
的加载行。与此前的 jhash 错不同，这次 APFS 说的是**读失败**，不是校验和
不符；而 r166 已经整轮走单块读，dyld 读 SystemOS 缓存依旧 `err=0`。
r161 与 r166 用的是同一份驱动，唯一的差别是 Data 卷在 persona 阶段被写过。

写路径上有四处缺陷，本轮一并修掉：

1. **写完从不确认**。`writePioBlock`/`writeMultiBlock` 只要命令响应、
   DATA_END 和字数对就返回成功，从不发 CMD13 看卡是否已
   `READY_FOR_DATA` 且处于 TRAN。新增 `writeSettled()`：先等
   `SDHCI_DATA_INHIBIT` 落掉（200 ms 上限），再轮询 CMD13，只有
   `R1_READY_FOR_DATA` 且 `R1_CURRENT_STATE == TRAN` 才算这块写完；
   未就绪的批次不计入 `done`，并打印一次
   `CloverSdhci r167: write not ready lba=%u blocks=%u rsp=%08x st=%08x polls=%u`。
2. **停命令失败被忽略**。`writeMultiBlock` 现在检查 `sendStopCommand()`
   的返回值；数据相位不干净或停命令失败，一律 `SDHCI_RESET_DATA`，
   不让卡留在 data 模式去污染后续读。
3. **写节奏还是 400 kHz 的旧账**。单块写删掉每字 160 µs 的间隔，改成与读
   相同的 level-gated：`SPACE_AVAILABLE` 就交一个字。SDHCI 在缓冲满时
   自己背压，不会跑过卡。两条整请求死线也改为随块数缩放：
   `frequency * 2 + blocks * (frequency / 200)`。
4. **写缓存从未关过**。每次开机都是硬断电，而 eMMC 的写缓存里可能还躺着
   没落盘的块。mode 21 读完 EXT_CSD 之后、`fastMode` 之前，按
   `CACHE_CTRL`（字节 33）把写缓存关掉（已经是 0 就跳过），并打印
   `CloverSdhci r167: write cache size=%u ctrl=%u off=%d rsp=%08x st=%08x`。

读这一侧把批量读放回来，但不再无条件信任：`checkReadPath` 对首个 ≥32 块
的请求读三份 16 KiB——两次 CMD18 批量、一次 32 块逐块——三份逐字相同
（`diff=0/0`、长度都等于请求）才 `gReadBatchTrusted = true`，否则整轮退回
单块。无论哪种都先打
`CloverSdhci r167: readcheck lba=… batch=…/… single=… diff=…/… trusted=…`，
再打 `batch reads trusted for this boot` 或
`single-block reads for this boot`。理由是 3.7 GiB 的 dyld 缓存按单块读要
按小时计，而 r166 已证明批量读不是坏读的来源；三份对照保留为一道闸。

构建链与 round166 相同（先内核、再 payload）：

```sh
cd ~/UniXNU && /opt/homebrew/bin/python3.13 ./unixnu build kernel --device ../UniXNU-clover
/opt/homebrew/bin/python3.13 ~/UniXNU-clover/scripts/patch-clover-amfi.py \
  ~/UniXNU/out/clover/kernel/kernel.macho ~/clover-out/round167-kernel-amfi.macho
/opt/homebrew/bin/python3.13 ./unixnu build payload --device ../UniXNU-clover \
  --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --kernel /Users/zhouzhou/clover-out/round167-kernel-amfi.macho \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
```

内核构建前同样要把 `out/clover/tree/.cache/xnu-work` 里的
`zzy-clover-dyld-cache-skip.patch` 反向撤掉，否则
`native-4k-signature-hash-cache.patch` 正反都校验不过。`--trust-cache`
仍必须是 `build/clover-trust/trustcache.bundle`（`7c369ce0…`）。

产物：prelink 内核 `248e7c58…`；打过 AMFI 的拷贝 `f15625be…`
（`0x1a830e4` 与 `0x1a8316c` 两处补丁照旧）；payload `2017f236…`（56 MiB），
卷内 kernel `b87480e5…`、`unixnu.toml` `4cdbd9c7…`、ADT `5b328b26…`、
`scratch-data.img` `614b8356…`、trustcache `7c369ce0…`。入口探针
`6845 bytes in 37 runs`；`verify-clover-payload.py` 全项 OK，新增
`kernel.macho: eMMC read/write integrity`（readcheck、trusted/single 两行、
write cache 与 write not ready）。归档在 `~/clover-out/round167-*`，清单
`round167-SHA256SUMS`。

写入只有一个分区，cache：

```sh
fastboot flash cache /Users/zhouzhou/clover-out/round167-payload.img
fastboot reboot
```

`userdata` 不动，round159 的 Data 窗口仍在；这是冷刷，不用 `fastboot boot`。

这一刷回答一个问题：可写的 Data 容器是否还会在读回时报 I/O 失败。
读数顺序：`CloverSdhci r167: write cache … off=1`（缓存确实关了）、
`CloverSdhci r167: readcheck … trusted=1` 与 `batch reads trusted`（批量读
恢复且三份一致）、persona 阶段之后 `apfs_scan_verify` 的
`failed to read` 与 `Container corruption detected` 是否消失、
`launchd_cache_loader` 的 SIGBUS 与那条 85 是否随之前进。若写缓存关掉后
APFS 仍然判容器损坏，下一次启动前应先在宿主上对 Data 种子做一次
fsck 级检查，再决定是否需要重建那 32 MiB 种子；那属于对 userdata 窗口的
破坏性写入，会单独声明。

## v0.5.22（round168）：卡忙的时候要等，判死的块要重试

r167 的冷刷第一次没有 panic：`launchd` 的 exec 循环照旧，但块层开始自己报错。
`disk5: general error.` 反复出现，随后
`tx_flush:1173: disk5 tx xid 531 took 22708252 us to flush`——一次 APFS 事务
落盘用了 22.7 秒；紧接着 `disk1: general error.`、
`apfs: buf_bread(0x20): disk1 buf_bread() failed, error = 5`、
`ctx->dev_read(507): disk1 dev_get err 5 reading blkno 31134`（还有 `91811`）、
`apfs_fs.c:1033: apfs_fs_read_tree … (gr=1074)`。`error = 5` 是块设备交给
APFS 的 EIO；两块盘在同一时刻、在固定块号上报 EIO，介质不可能这样坏，APFS
只是在重问同一个块。

这三处 EIO 都出自本驱动自己的时限：

1. **写落盘闸门只有 200 ms**。r167 的 `writeSettled` 等卡把一块真正写进介质，
   但一次 checkpoint 的落盘时间按秒计。批次在 200 ms 后被丢弃、不计入 `done`，
   APFS 看到短写就是 `disk5: general error`，然后重发同一批。现在上限 4 秒，
   并发一次 `CloverSdhci r168: write settle took %llu ms polls=%u`（超过
   500 ms 才打），把卡自己的编程时间变成读数。
2. **块层 guard 只等 2 秒**。一个调用者持锁正在写，另一个等 2 秒就拿不到、
   返回 0，块设备把它变成 `dev_get err 5`。现在等 30 秒（持有者的请求死线
   本身有界），并在等待超过 250 ms 时打一次
   `CloverSdhci r168: block guard waited %llu ms`。
3. **失败一次就返回 EIO**。读的批量失败后只补一个单块，单块再失败就整请求
   截断；写的批次落盘超时后直接放弃。现在：批量读失败先复位数据通路重试一次
   再退回单块；单块读每个块最多 3 次；写批次落盘超时后复位并重发同一批一次；
   单块写同样两次。全部失败才放弃，并各打一次
   `CloverSdhci r168: read gave up lba=%u blocks=%u done=%u st=%08x` /
   `write gave up lba=%u blocks=%u done=%u`。

请求死线随之放宽：读 `5 s + blocks × 20 ms`，写 `10 s + blocks × 200 ms`；
批量传输的每块份额从 10 ms 提到 50 ms。r167 的三份样本闸门与
`CACHE_CTRL` 清零不变。

构建链与 round167 相同（先内核、再 payload）：

```sh
cd ~/UniXNU && /opt/homebrew/bin/python3.13 ./unixnu build kernel --device ../UniXNU-clover
/opt/homebrew/bin/python3.13 ~/UniXNU-clover/scripts/patch-clover-amfi.py \
  ~/UniXNU/out/clover/kernel/kernel.macho ~/clover-out/round168-kernel-amfi.macho
/opt/homebrew/bin/python3.13 ./unixnu build payload --device ../UniXNU-clover \
  --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --kernel /Users/zhouzhou/clover-out/round168-kernel-amfi.macho \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
```

内核构建前同样要把 `out/clover/tree/.cache/xnu-work` 里的
`zzy-clover-dyld-cache-skip.patch` 反向撤掉。`--trust-cache` 仍必须是
`build/clover-trust/trustcache.bundle`（`7c369ce0…`）。

产物：prelink 内核 `1be9cc03…`；打过 AMFI 的拷贝 `86f99fc0…`
（`0x1a830e4` 与 `0x1a8316c` 两处补丁照旧）；payload `5870d848…`（56 MiB），
卷内 kernel `cb0a20cf…`、`unixnu.toml` `6f1b4889…`、ADT `056815d2…`、
`scratch-data.img` `614b8356…`、trustcache `7c369ce0…`。入口探针
`6845 bytes in 37 runs`；`verify-clover-payload.py` 全项 OK，
`kernel.macho: eMMC read/write integrity` 现在同时要求 r167 与 r168 的
读数行。归档在 `~/clover-out/round168-*`，清单 `round168-SHA256SUMS`。

写入只有一个分区，cache：

```sh
fastboot flash cache /Users/zhouzhou/clover-out/round168-payload.img
fastboot reboot
```

`userdata` 不动，round159 的 Data 窗口仍在；这是冷刷，不用 `fastboot boot`。

这一刷回答一个问题：**r167 帧里两块盘上的 EIO 是不是我们自己的超时判死的**。
读数顺序：`CloverSdhci r168: block guard waited …`（是否真有长时间占用）、
`CloverSdhci r168: write settle took …`（卡自己的编程时间），然后看
`disk5/disk1: general error.`、`dev_get err 5`、`buf_bread … failed`
是否消失，`tx_flush` 的耗时是否降到秒级以下。若同两个块号仍失败，那行
`read gave up … st=…` 会带上块号和 INT_STATUS，把问题挪回卡或总线；若它
一次都不出现而同处仍报 EIO，剩下的就是 `CloverBlock` 窗口映射而不是 SDHCI。

## v0.5.23（round169）：读失败要停卡，失败要说清阶段

r168 的冷刷回答了 r167 的问题：写落盘闸门放宽之后，`disk5` 不再
`general error`，22.7 秒的 `tx_flush` 也不再出现。但同一帧里 `disk1` 从第一个
16 KiB 请求起就连续报错，块号互不相关：

```
apfs: buf_bread(0x20): disk1 buf_bread() failed, error = 5
ctx->dev_read(507): disk1 dev_get err 5 reading blkno 580 (108169, 91610, 102029)
apfs_ntree_read(805): disk1:1 readdesc(1) err … retval 5
decmpfs_read_compressed: decmpfs_fetch_uncompressed_data err 5
ChunkCompression.cpp:298: /usr/libexec/logd: WEP_GETNTRC(264,0): errno 5
```

同一时刻 SystemOS 的缓存读取仍是 `err=0`，所以介质与高速链路没有坏；坏的是
读失败之后的状态。写批次失败从 r157 起就发 CMD12，把卡送回 TRAN；单块读失败
只做 `resetReadPath()`（主机 `SDHCI_RESET_DATA`）。卡停在数据态时主机复位对它
没有意义，之后发给它的每个 CMD17 都被忽略，块设备就把一次失败放大成一串 EIO。

本轮改四处：

1. `abortReadTransfer()`：先 `sendStopCommand()`（CMD12 R1b）再复位主机。
   `readDataBlock` 的两条失败分支（命令被拒、数据相位不干净）都改走它。
2. `waitCardIdle()`：每个读请求在第一条命令前等主机 `DATA_INHIBIT` 落下，再用
   CMD13 确认 `R1_READY_FOR_DATA` 且状态为 `TRAN`，上限 30 秒；超时先发一次
   CMD12 再报失败。等卡时间不计入请求死线（拿到 guard 后重新起算）。
   超过 250 ms 打一行 `CloverSdhci r169: card busy %llu ms`，每次启动最多一行。
3. 写路径：`writeSettled` 的上限 4 秒 → 30 秒；写请求死线基数 10 秒 → 30 秒；
   批次或单块落盘失败后、重发前也调用 `waitCardIdle()`，不再把同一批写进一张
   还在编程的卡。
4. `reportReadFailure()`：读失败按 `guard` / `idle` / `deadline` / `gave up`
   四个阶段打点，最多八行；每行都带 `lba`、`blocks`、`done` 与 `st`。
   r168 那条每启动一次的 `read gave up` 被它取代（`block guard waited` 与
   `write gave up` 保留）。

构建链与 round168 相同（先内核、再 payload；内核构建前在
`out/clover/tree/.cache/xnu-work` 里反向撤掉 `zzy-clover-dyld-cache-skip.patch`）：

```sh
cd ~/UniXNU && /opt/homebrew/bin/python3.13 ./unixnu build kernel --device ../UniXNU-clover
/opt/homebrew/bin/python3.13 ~/UniXNU-clover/scripts/patch-clover-amfi.py \
  ~/UniXNU/out/clover/kernel/kernel.macho ~/clover-out/round169-kernel-amfi.macho
/opt/homebrew/bin/python3.13 ./unixnu build payload --device ../UniXNU-clover \
  --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --kernel /Users/zhouzhou/clover-out/round169-kernel-amfi.macho \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
```

产物：prelink 内核 `c5e70a3b…`；打过 AMFI 的拷贝 `f5f1db9e…`
（`0x1a830e4` 与 `0x1a8316c` 两处补丁照旧）；payload `db15478e…`（56 MiB，
58,720,256 字节），卷内 kernel `52ef7cf4…`、`unixnu.toml` `76ca3839…`、
ADT `34b08b2b…`、`scratch-data.img` `614b8356…`、trustcache `7c369ce0…`。
入口探针 `6845 bytes in 37 runs`；`verify-clover-payload.py` 全项 OK，
`kernel.macho: eMMC read/write integrity` 现在要求 r169 的 `card busy`、
`read <stage>` 与 `write settle took` 串。归档在 `~/clover-out/round169-*`，
清单 `round169-SHA256SUMS`。

写入只有一个分区，cache：

```sh
fastboot flash cache /Users/zhouzhou/clover-out/round169-payload.img
fastboot reboot
```

`userdata` 不动，round159 的 Data 窗口仍在；这是冷刷，不用 `fastboot boot`。

这一刷回答一个问题：**r168 帧里成串的 16 KiB EIO 是不是「读失败没停卡」造成的**。
读数顺序：先看 `CloverSdhci r169: card busy … ms`（是否真有一次长时间忙卡），
再看 `CloverSdhci r169: read <stage> … st=…`（第一处失败发生在等卡、死线还是
传输），然后看 `disk1: general error`、`dev_get err 5`、`buf_bread … failed`
是否从「每个请求都失败」变为「不再出现」或「只在特定块出现」。若 `read idle`
出现且 `st=00000000`，说明卡在第一条命令前就没准备好，问题在写后恢复；若
`read gave up … st=` 带 CRC/超时位，问题回到传输本身；若 EIO 消失而守护进程
仍报 `errno 5`，剩下的就是 CloverBlock 的窗口与 APFS 的一致性问题。

## v0.5.24（round170）：缓存探针封顶，exec 与 exit 各出一声

r169 的冷刷答掉了 r168 提的问题。**这一帧上没有 EIO**：`disk1: general error.`
不再出现，`apfs: buf_bread(0x20): disk1 buf_bread() failed, error = 5`、
`ctx->dev_read(507): disk1 dev_get err 5 reading blkno …`、
`apfs_ntree_read(805): … retval 5`、每个守护进程解压自身二进制时的
`decmpfs_fetch_uncompressed_data err 5` 与
`ChunkCompression.cpp:298: … WEP_GETNTRC(264,0): errno 5` 全部消失，r164/r166
两轮的 APFS jhash 自校验错（`apfs_jhash.c:146: Data hash mismatch for 16384
bytes at offset …`）也没有再来一次，而且没有 panic。驱动唯一的读数是

```
CloverSdhci r169: block guard waited 31 ms
```

31 毫秒，不是 30 秒；`card busy` 一行都没打，`read guard/idle/deadline/gave up`
一条都没有——读路径这一次根本没进失败分支。所以级联的成因确认了：不是卡坏，
是 r168 之前「读失败不停卡」，卡留在数据态之后把每个 CMD17 都当成噪声。r169
补上 CMD12 与读前等卡，这条就断了。

同一帧也暴露了仪器自己的毛病。照片上能读的行几乎只有一条

```
clover r153: check_np dyld_flags=0x0
```

在重复，另有一条 `clover r153: open /System/Library/Caches/com.apple.dyld …
err=0` 和 `launchd [Event: finish-restore] during boot task`。
`shared_region_check_np` 每颗进程都会调好几次（dyld 的 `reuseExistingCache()`
先调它，后面映射时还要调），r153 那版一次都不节流，于是这一行把整块面板刷成
同字复读。两个后果：

1. 这一行不带 pid。「重复很多次」到底是几颗进程各调几次，还是一百颗进程各调
   一次，照片分不开；
2. XNU 只对 critical process 打印退出原因（`proc_handle_critical_exit`），普通
   子进程的死在控制台上完全静默。r162/r164 那种「launchd 反复拉、pid 一路涨」
   的循环，在这一帧里没有可判读的形状。

按 dyld 源码，「只有目录 open、没有主缓存与 `.01` 的 open」与 `reuseExistingCache()`
命中已有共享区、不再去开文件是一致的；但面板只留尾部，复读行会把别的字挤出去，
所以这只能算待验的形状，不能写进已解决。

本轮不动存储栈，只改可观测性，共三处：

1. `check_np` 节流（`zzy-clover-shared-region-why2.patch` 里的 `bsd/vm/vm_unix.c`）：
   只在 `dyld_flags` 变化时、或 pid ≤ 8 时打印，整启动上限 48 行，标签升到
   `clover r170: check_np pid=%d comm=%s dyld_flags=0x%llx`。pid 与 comm 是这一行
   本来就缺的东西。
2. 缓存目录 open 封顶（同一补丁的 `bsd/vfs/vfs_vnops.c`）：`err=0` 的
   `/System/Library/Caches/com.apple.dyld` 只报前 16 次；失败的 open 永不封顶，
   主缓存与 `.01` 的 open 也不封顶，因为那两条是这道题的正文。r153 的
   `read err=… resid=… hdr=…` 与 r155 的 `hdr`/`fsig` 系列标签保持不变。
3. 新增 `zzy-clover-shared-region-why5.patch`（只碰 `bsd/kern/kern_exec.c` 与
   `bsd/kern/kern_exit.c`，两颗都不在 why2 的文件里）：`set_proc_name(imgp, p)`
   之后打一行 `clover r170: exec pid=%d ppid=%d t=%llus comm=%s`，
   `exit_with_reason` 之后打一行
   `clover r170: exit pid=%d ppid=%d t=%llus rv=0x%x ns=%d code=0x%llx comm=%s`，
   `t=` 是 `clock_get_uptime()` 的整秒，`ns`/`code` 取自 `exit_reason` 的
   `osr_namespace`/`osr_code`（无 reason 时 `ns=-1`），各 256 行封顶。

补丁层这件事值得记下来：探针**不能插进 why2 已经新增的块里**，因为
`build-xnu-4k-baseline.sh` 按文件名顺序重放 `patches/xnu/*.patch`，靠
`git apply --reverse --check` 判断某颗补丁是否已在树上；改了前面补丁的新增行，
它会逆撤失败、正打也失败，构建直接中止。所以 why2 的节流是**重生成 why2 自身**
（`cd ~/UniXNU/out/clover/tree && python3 scripts/save-xnu-patch.py …`，生成前先把
排序在后的 why3 逆撤），why5 只碰 why2 未涉及的两个文件。`check-clover.py` 与
`verify-clover-payload.py` 都对此加了断言：why5 里不得出现 `bsd/vm/vm_unix.c` 或
`vfs_vnops.c`，且 r153 的两条复读串必须从镜像里消失。

构建链与 round169 相同（先内核、再 payload；内核构建前在
`out/clover/tree/.cache/xnu-work` 里反向撤掉 `zzy-clover-dyld-cache-skip.patch`，
它会盖住上游 `native-4k-signature-hash-cache.patch` 的行）：

```sh
cd ~/UniXNU && /opt/homebrew/bin/python3.13 ./unixnu build kernel --device ../UniXNU-clover
/opt/homebrew/bin/python3.13 ~/UniXNU-clover/scripts/patch-clover-amfi.py \
  ~/UniXNU/out/clover/kernel/kernel.macho ~/clover-out/round170-kernel-amfi.macho
/opt/homebrew/bin/python3.13 ./unixnu build payload --device ../UniXNU-clover \
  --unique-chip-id 0x81ffcaaa --uefi ../mu-clover \
  --kernel /Users/zhouzhou/clover-out/round170-kernel-amfi.macho \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
```

产物：prelink 内核 `98f2e5e1…`；打过 AMFI 的拷贝 `a100d479…`
（`0x1a830e4` 与 `0x1a8316c` 两处补丁照旧）；payload `00510286…`（56 MiB，
58,720,256 字节），卷内 kernel `27e95ca6…`、`unixnu.toml` `6df3a99e…`、
ADT `d8de9a71…`、`scratch-data.img` `614b8356…`、trustcache `7c369ce0…`。
入口探针 `6844 bytes in 37 runs`；`verify-clover-payload.py` 全项 OK，新增的
`kernel.macho: exec/exit probe linked` 要求 r170 的 exec/exit 两串，
`cache open probe linked` 现在要求 r170 的节流串、并反向断言 r153 的两条复读串
不在镜像里。归档在 `~/clover-out/round170-*`，清单 `round170-SHA256SUMS`。

写入只有一个分区，cache：

```sh
fastboot flash cache /Users/zhouzhou/clover-out/round170-payload.img
fastboot reboot
```

`userdata` 不动，round159 的 Data 窗口仍在；这是冷刷，不用 `fastboot boot`。

这一刷回答一个问题：**launchd 是在反复拉同一批死掉的子进程，还是在往前走**。
读数顺序：先看 `clover r170: exec` 的 comm 集合——`/sbin/launchd` 之后有没有
铺开出一串 `/usr/libexec/…`；再看 `clover r170: exit` 有没有与之配对，`ns=` 是
哪个域（`OS_REASON_DYLD` 是 6）、`code=` 是什么、`rv=` 折出哪个信号；最后看
`t=` 有没有在推进。只有 exec 没有 exit 且 `t=` 一路拉长 = 有东西堵在里面；
exec/exit 成对但 comm 总是同一颗 = 还在循环；exec 铺开、exit 缺席 = 守护进程
活下来了，下一格才轮到 536 与 `/data`。附带读数：`check_np` 封顶之后，屏上应该
重新看得见 `clover r153: read`、`clover r155: hdr`、任何一条 APFS 或签名错误，
以及 `clover r154: map2` 到底有没有出现。

## 许可证

BSD-2-Clause-Patent，与 UniXNU 一致。`scripts/build-arm-driver.py` 移植自
UniXNU-gta4l（NoNameBuilder），采用同一许可证。
