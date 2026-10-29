# UniXNU-clover

**小米平板 4 8 英寸 LTE 版（2018）**的设备仓库——代号 `clover`，LTE 版本，
4 GB 内存，高通 **SDM660**（骁龙 660，Kryo 260：4× Cortex-A53 @1.8 GHz
+ 4× Kryo 385 Gold @2.2 GHz）。

它向 [UniXNU](https://github.com/UniXNU/UniXNU) 构建驱动描述这块板子：
目标 IPSW、内核配置、prelink 根、固件检出、ESP payload 和 Apple 格式设备树。
配套的固件仓库是 `uefi-clover`，同一块板子的 Mu-Silicium 移植。

`clover` 既是主线给这个家族的名字（`xiaomi,clover-lte`），也是小米给这个平台
的名字（`BoardPlatform: clover`），所以板级标识沿用它。

目标系统标识是原封不动的 **iPad7,1 / iPadOS 17.7.11 / 21H461** 镜像
（`j120ap`，`ApChipID 0x8011`，`ApBoardID 12`）。Kryo 260 是 ARMv8.0，
没有指针认证，所以只有这一档 4 KiB 页的镜像能跑，内核按
`DEVELOPMENT_ARM64_BCM2837` 构建。

逐轮的开发过程、每一张照片读到什么、由它推出什么，全部搬到了
[`notes/development-log.md`](notes/development-log.md)。这份 README 只留关键
事实和进度。

## 开发进度

现在的形态：整条内核引导链、根挂载、EL0 与多进程、eMMC 读写都已经有真机读数
并且通了；launchd 已经在拉守护进程（`zproxy`、`accessoryupdater`、
`identityservicesd` 都被 exec 起来过），卡住它的是驱动自己——一次 102 秒的
checkpoint flush 握着块守卫，把另一颗卷上的每一次读判成 EIO，pid 1 最终在
映射页时 SIGBUS。round171 修那一次 flush，round172 修「每个字节都过 CPU」这件
事本身：读通路现在有一条 ADMA2，靠逐字比较挣到信任。round173 把同一条通路交给写，
但它只准重写自己刚读到的字节——先用 PIO 读回、再用 ADMA 写回同一批、再用 PIO 读回
比对，而且只在 logfs 那 8 MiB 里做。round174 不改通路，只改记录：驱动的每一句
判据现在同时上屏和写进卡上的 verdict 环，读数不再依赖拍照的时机。round175 把这条
通道交给别人的话：驱动直接读内核自己那份 `msgbuf`，把 dyld、exec/exit、APFS、AMFI
那些已经打印出来的行滤进同一个环——一轮新探针都不用加，屏幕滚没滚过都不再重要。
round176 拆掉那一轮自己的代价：两个说话人不再抢同一圈 32 槽，内核的话有 kind-22 的
512 槽，驱动的判断有 kind-21 的 64 槽，一次 `dd` 还是全拿。round177 不看机器，只看
构建本身：那三处「重建一定不一样」的字段被改写成文件自己算出来的值，于是同一棵树的
两次构建交付同一个内核镜像。round178 回到那张卡：pid 1 的死因已经读成一次页调入读不
到字节——round171 让抢不到守卫的读去等，但等只发生在请求开头，于是一个 128 块的写
占住守卫多久，页就等多久。这轮把守卫的持有单位从「一个请求」改成「一个块段」，
读写两条循环每交付满一段就放一次手再排队。round179 不动 shipped 字节，改的是显示
那一格的起点：原始 `IOMobileFramebufferLegacy` 在**本板自己的内核**里被逐槽资格化，
类身份从注册指令推出来，不再借用别人在虚拟机里观测的那四个数。round180 接着把那一格
从 stub 换成真代码：真的原始 Legacy 档案链接进内核，phase 1 第一次在硬件上问「这个类
还认不认得自己、console 交到内核手里是什么几何」。round181 开的是排期第 4 格：触摸总线上
第一次有事务——QUP v2 I2C-3 被按上游的形状带上电，两个候选控制器各被问一次它们自己驱动
开机必问的那一读，答案不论 ACK 还是 NACK 都从卡上的日志环里读回来。round182 问的是那条
答案的下半场：面板没上电，就得先知道 PMIC 归谁写，于是 SPMI 仲裁器第一次被读。round183 问
排期第 5 格的入口问题——ADSP 到底起没起——共享堆第一次被人照着厂商布局读出来。round184
不再问硬件：三件仪器停在原地，把「引导能自己回到 fastboot、panic 前把日志落到卡上」这条路
接上，因为 round183 那一帧证明了一件事——屏是一台画满就停的打印机，读它等于没读。

### 已完成（都有真机读数）

| 事项 | 证据 |
| --- | --- |
| 固件与菜单 | `uefi-clover` v0.3.10 渲染 UniXNU 菜单、读到 payload，3 秒后默认进 XNU（Stage 1） |
| XNU 引导链 | 入口色带 + panic 屏证明内核经 `arm_init`、`machine_startup`、`vm_mem_bootstrap` 走到 `_bsd_init` |
| 时钟与抢占 | INTID 19 真的来 tick（round82 `ticks=0x14e5`）；`select` 期限、`wait4` 唤醒、抢占第一次有依据 |
| AMFI | `amfi-only-platform-code` + `CloverPMGR` 发布的 `ApplePMGRNub` 消掉 `No PMGR?` panic，`PMGRAON` 走完读写（round83） |
| EL0 与 syscall | `rd=md0` mockfs 根上 pid 1 进用户态；`getpid`、`open("/dev/console")`、`write`、`fork`、`wait4`、`execve` 都有读数 |
| APFS 根 | round137 挂上 OS 卷；round150 owner 审计接受未封印的根，三模块信任缓存放行 |
| SystemOS | round143/145 在 exec 时把 SystemOS 卷 union 到 `/`；round153 dyld 打开缓存目录、主缓存、`.01` 三处 err=0，两个头都是 `dyld_v1   arm64` |
| eMMC 高速读 | round159 帧：16 KiB 读 7 ms、224 KiB 读 189 ms（约 1–2 MB/s，比 round156 前的 25 KB/s 快 40–80 倍）；CMD6 高速链路、8 线、CMD18 批量成立 |
| eMMC 读不再级联失败 | round169 帧：卡停在数据态那一类级联消失——`buf_bread … error = 5`、`dev_get err 5`、APFS jhash 校验错在同一次冷刷里全部消失，唯一驱动读数 `block guard waited 31 ms`。**但 round170 帧暴露出另一类级联**：`read guard … st=00000000`，读不是被介质拒绝，是没排到块守卫；由 round171 修 |
| eMMC 写 | 写完成需 CMD13 确认、批量读须三份样本一致才信任（round167）；`disk5: general error` 自 round168 帧起未再出现。代价被 round170 帧量出来：`CACHE_CTRL` 清零之后 `tx_flush` 从 22.7 秒涨到 **102.0 / 120.0 秒**，握死块守卫；round171 改为开缓存 + 显式提交 |
| launchd 与守护进程是死是活可读 | round170 帧：`clover r170: exec pid=38 ppid=1 t=66 comm=accessoryupdater`、`exit pid=54 ppid=1 t=29 rv=0x4000 ns=-1 code=0x0 comm=zproxy`——comm 在换、pid 在铺、`t=` 在推进，`Early boot complete` 出现两次；结论是**在往前走而不是在循环**，直到 pid 1 自己 `initproc exited -- exit reason namespace 2 subcode 0xa`（SIGBUS） |
| Data 卷 | round159 帧：`seed data … bytes=33554432`、`bsd=disk5s1`、APFS `mount-complete volume DawnData`，UUID `FC1A580E-2D67-48FC-B8CF-87422A319340` 与钉住的种子一致，owner 的 `/private/var` 挂载失败行没有出现 |
| dyld 缓存预检 | round159 帧：目录 / 主缓存 / `.01` 全 err=0，`.01` 头 `len≈1.07 GiB sig=11491332+229376`，随后每颗子缓存的 `F_ADDFILESIGS` 都 `err=0 resid=0` |
| 诊断通道 | panic 屏直写 framebuffer（不经 tty）、无人值守的 IMEM `0x77665501` + PSCI 复位、`read-clover-log.py` 取 logdump（round185 起；此前是 logfs）、`decode-clover-address.py` 把地址翻成函数 |
| 仓库契约 | `porting.toml`（`schema_version = 1`，上游 13 area 校验通过）、`AGENTS.md`、`.github/workflows/porting-record.yml`；CI Device documentation 在 main 与 tag 双绿 |

### 进行中（代码已在镜像里，等实机读数）

| 轮次 | 内容 | 状态 |
| --- | --- | --- |
| round154（v0.5.7） | syscall 536 入口/返回、setup 阶梯与 `mapfile` 探针 | 探针仍在镜像里，至今没有 `map2` 读数 |
| round166（v0.5.20） | exec 侧打印前八个签名 blob 的 `mo/off/size` 与头四字 | `clover r166: sig` 尚未上屏 |
| round171（v0.5.27） | 批量读按「交付字数 + 内容错误位」判定（`st=0x21 w=1024/1024` 那种全量交付不再被丢回单块）；卡的写缓存打开并显式提交（CMD6 关→提交→开）；块守卫与批量缓冲从有界自旋改成 `IOLock`；批量 32 → 128 块，可信性闸门改为按请求取样 | 改动全部在 round172 镜像里，与下一行同一帧读 |
| round172（v0.5.28） | 读通路加一条 ADMA2：描述符形状逐字节抄自本板 4.19 CAF 树（12 字节项、`cmd` 在前 `len` 在后、末项带 END 不加 NOP），数据落在本驱动自有的 64 KiB 连续 bounce（照 sheng `NativeUfs` 的四条物理地址检查与 `dc civac`/`dc ivac` 节奏）；同一个请求先走已信任的批量 PIO、再走 ADMA，**逐字相同才准用**，失败八次整轮退役 | 改动全部在 round173 镜像里，与下一行同一帧读 |
| round173（v0.5.29） | 写通路也上 ADMA2，但只准重写自己刚读到的字节：闸门取 logfs 窗内第 4096 个扇区起的 16 块（离 panic 槽占的前 32 块很远），PIO 读回原字节 → 同样字节走 PIO 写一遍当对照 → ADMA 写回同一批 → PIO 再读回逐字比对，三份都对才 `trusted=1`；此后每个 ≥4 块的写请求先走 DMA，被拒就落回 r171 的 PIO 批量，八次退役。panic 屏与 bring-up 报告的两块写仍走 `writePioBlock`，不依赖本轮任何标志 | 改动全部在 round174 镜像里；r173 那些上不了屏的判据，这一轮起会写进卡上的环 |
| round174（v0.5.30） | 判据写进卡上，不再只留在屏上：驱动里每一句 `CloverSdhci r…` 改走 `sdhciSay`——照常 `IOLog`，同时存进 RAM 环（round176 起 128 槽暂存、卡上 64 槽）；`flushVerdictRing` 在每个已经握着块守卫的传输尾巴上最多落 4 条（每条两个 512 字节 LOG1 块），写进 logfs 第 64 扇区起、离 panic 槽很远的 kind-21 环，头字段第 8 个字带序号，所以环转完一圈宿主仍能按先后读出。环满就覆盖最旧一条并计数，绝不阻塞；写用不加守卫的单块原语，因此不可能在失败的传输里再套一层等待。宿主侧 `read-clover-log.py` 解两个窗口、`--selftest` 与驱动逐字节往返，`check-clover.py` 把它当构建闸门 | 改动全部在 round176 镜像里，与下两行同一帧读 |
| round175（v0.5.31） | 内核自己已经说过的那句话，也搬到卡上：**一个探针都不加**。驱动在每个已经握着块守卫的尾巴上调用 `teeKernelLog`，按字节游标读内核的 `msgbuf`（`subr_prf.c:388` 把每条 `TOLOG` 字符交给 `log_putc`，`subr_log.c:554` 就是往 `msgbufp->msg_bufc` 环形写），四个字段每趟重读因为那块缓冲在 `subr_log.c:622` 之后可能被换掉；一次最多扫 4096 字节、最多落 8 条（round176 起的额度，那时是两个说话人共用一圈）。只有命中白名单的行才留（`clover r`、`CloverBlock`、`apfs`、`decmpfs`、`load code signature`、`AMFI`、`launchd`、`vm_fault`、`shared_region`、`panic`，round176 又补 `cs_blob`、`dyld`、`mountroot`、`rootvp`——四条都在 XNU 源码里逐字核过），驱动自己的 `CloverSdhci ` 行直接跳过。抄进来的行走 `ringPush(kRingKernel)`，驱动自己的状态行走 `ringPush(kRingDriver)`，**都不过 `IOLog`**：守卫里再拿一次 `bsd_log_lock` 是给自己造死锁机会，而且抄进来的话再打印出去会被自己再抄一遍。校验（magic/size/bufx/必须落在内核 VA 之上）不过就只报一次 `tee refuses reason=`，缓冲搬家报 `tee buffer moved`，每 256 行报一次 `tee seen= kept= throttled= lap=`（round176 起最多 12 次）。宿主侧 `read-clover-log.py` 按行首前缀分 `driver`/`kernel` 两种来源，`--tee` 与 `--driver` 各取一边 | 改动全部在 round176 镜像里，与下一行同一帧读 |
| round176（v0.5.32） | 拆掉 round175 自己记下的那笔代价：**一圈 32 槽、两个说话人**——`load code signature error 4` 每次 exec 一行，一个忙着起不来的守护进程能在卡看见之前把驱动的判据挤出环。改的是几何而不是指针：驱动环 kind 21 从 logfs 第 64 扇区起 64 槽、每趟最多落 4 条，内核日志环 kind 22 从第 192 扇区接上、512 槽、每趟最多落 8 条，RAM 侧一圈 128 槽暂存、序号各自从 0 走。tee 自己的状态行（`tee armed`、`tee seen=`、`tee refuses`）故意留在驱动环，于是内核环的意思唯一：里面只有 kernel 说的话，宿主可以反问 `--tee --driver` 必须为 0，非 0 就是 tee 在抄自己。两圈首尾相接（64 + 2×64 = 192、192 + 2×512 = 1216），`run-clover-unattended.sh` 一次 `dd` 1216 个扇区仍然只拉一次 | **已构建**，待冷刷 |
| round177（v0.5.33） | 结掉排期里的「内核构建不可逐字节复现」：镜像现在是这棵树的纯函数。`scripts/normalize-kernel-macho.py` 把三处链接不可复现的字段——ld 每次链接生成的 `LC_UUID`、962 个 object-path 调试符号 `n_value` 里那个 `.o` mtime、`emit_prelink.py` 用前两者哈希出来的 `_PrelinkKCID`——改写成「文件其余内容 sha256 的派生值」，长度一字不差、幂等。同一棵树两次独立构建（`a307b5b0…` 与 round176 的 `d274a15e…`）归一化后是同一个 `86273887…`，两份真正交付的**打过 AMFI** 的拷贝同样收敛到 `f9966b72…`。本轮不动驱动、不动 XNU 补丁，所以帧上读的还是round171–176 那一整套判据；`check-cs-blob-structure.py` 的 `cs_validate_csblob` 宿主复刻也进了闸门（14 条拒绝规则 + 真机 `launchd.macho` 必须被接受），round162 那条 `cs_blob … 85` 至此在主机侧结掉 | **已构建**，待冷刷 |
| round178（v0.5.34） | 块守卫的持有单位从「一个请求」改成「一个块段」：round171 把「抢不到就拒」变成「抢不到就等」，但等仍然只发生在请求的**开头**，一个 128 块的写在卡上要占住守卫多久，页故障就最多等多久——round170 帧里 pid 1 正是这样死于一次页调入（`namespace 2 subcode 0xa` = SIGBUS），旁边摆着 8 条 `read guard … done=0 st=00000000`（主机一个错误位都没 raise，是这些读根本没拿到守卫）和两条 `tx_flush … took 102001275/120000602 us`。本轮在读写两条循环里，每交付满一段（`kReadChunkBlocks`）就 `handOffBlockGuard`：记下已占用时长、放行、重新排队，抢到才继续，队没排上就按 `guard` 报失败并结束本请求。死线随之改成 `owned + readCounter () - start`，**换手期间不计时**，所以卡的忙不再被算成驱动的死线（r167/r168 的 `write not ready`/`write gave up` 与 30 s 基数一字未动）。新增 `cuts=`/`waits=`/`worst=` 三个数与最多四行 `CloverSdhci r178: %s handoff …`；flush 那段不加换手——它等的是卡自己的缓存编程，任何锁策略都缩不了 | **已构建**，待冷刷；本轮不改 shipped 字节，屏上读的就是这一行 |
| round179（v0.5.35） | 显示排期第一格：本板自己的内核被资格化，不再借别人的结论。`scripts/audit-clover-mobile-graphics-legacy.py` 在 `out/clover/kernel/kernel.macho` 里逐槽审计原始 `IOMobileFramebufferLegacy`——family `__TEXT_EXEC` 的 sha256 与 sheng 钉住的 `2e044ee6…` 一致（所以那六个构造函数站点是同一批指令）、vtable 262/262/358、抽象叶 `[211,213,245,246,306,307,337,338,345]`、service 相对 base 只加 `[0,1,7]`、103 处覆盖、`newUserClient` 落在 141（四参数原生 ABI）。**类身份是新的**：meta class 记录在文件里是零（装载时才填），所以身份不能读、只能推——family 文本里 `mov w3,#0x18a0` 只有一处，它前后的 ADRP/ADD 给出名字与父记录，紧挨着注册壳的那个 getter 物化记录本身。推出来 `record=0xfffffff006b35940`、`parent=0xfffffff006b31400`、`size=0x18a0`、`name=IOMobileFramebufferLegacy`，与 sheng 只能从 QEMU 里观测的四个数逐字相同，而这里一次运行都没有 | 本轮不改 shipped 字节：内核与负载哈希仍是 round178 的 `28324379…`，屏上读的仍是 r178 那三个数；`check-clover.py` 把脚本的常量与「不许把静态推断说成运行观测」当闸门，产出的 `build/display-analysis/clover-mobile-graphics-legacy-abi.json` 是 round180 生成 ABI 头与原始绑定表的输入 |
| round180（v0.5.36） | 显示那一格不再是 stub。`scripts/build-clover-gop-legacy-display.py` 把 round179 的 ABI JSON 翻成本板自己的 `OriginalLegacyFramebufferABI.h` 与 `original-bindings.S`，编 `drivers/NativeMobileDisplay/`（板内检出，不是上游那份仍宣称 `v_display == 1` 的驱动）出真的 `NativeLegacyDisplay`/`NativeGOPPresenter` 档案；`[kernel].archives` 里 `build-clover-display-stub.py` 换成它，`CONFIG_XNU_NATIVE_LEGACY_DISPLAY=y` 让基座构建 `-force_load` 那个 `.late.a`。档案的每条原始绑定都是 Apple family 文本里的一个地址，所以出档案前逐条重读 `out/clover/kernel/kernel.macho` 那一条指令，**一条不同就拒绝**；构造/析构走 `base + 0x1A5CC`/`base + 0x1A618`，`__mod_init_func` 被剥掉以免两个模块初始化器跑两遍。板级探针 `package/CloverLegacyDisplayProbe.cpp` 在 `_bsd_init` 里登记类、构造一个实例、`metaCast`、`init()`，说出它看到的 console 几何就收手——phase 1 不 attach 也不 start，七个仍未资格化的抽象叶一个不碰 | **已构建**，待冷刷；本轮没有一帧来自这份档案，`original_instructions_modified`/`driver_initialized`/`display_presented`/`device_observed` 四个标志都仍是 False |
| round180 补（v0.5.37） | shipped 字节一个没动，补的是记录。上游文档闸门对每个改动文件找**所有** `paths` 认领它的区，`board.toml` 被九个区认领，而它一次只报第一个——v0.5.36 因此红在 `audio`。本地复刻同一条闸门跑通后才补满九区（`configuration` 写新开关与三条 boot arg、`packaging` 写本轮三份哈希与「`ar` 的 mtime 让 `archive_sha256` 每跑必动，对象哈希才是复现证据」、`memory` 写 boot 时多出的那个 0x18a0 实例、`power`/`network`/`firmware` 各自写清本轮没碰什么）；`AGENTS.md` 的 Porting record 一节留下这条闸门的本地跑法，并改掉「display 是 planned」这句 round180 之后就不再成立的话 | `porting-record.py check` 本地 exit 0（13 个区），`check-clover.py` 仍绿；镜像仍是 payload `b453884d…`，屏上读的仍是 r178 那三个数 |
| round181（v0.5.38） | 排期第 4 格第一次有代码：`drivers/CloverInput` 在 QUP v2 的 BLSP1 I2C-3（`0xc177000`，DT 声明 `0x600`，映射整页）上问两个候选——Goodix `gt9xx@5d` 的 `0x8040`、EDT/FocalTech `@38` 的寄存器 `0x00` 与 `0xBB` 名字——各问它们**自己的上游驱动开机必问的那一读**，因为 NACK 只有对着真实寄存器才有意思。寄存器与位全部从三个具名来源抄（本板 DT、mainline 7.2.y `i2c-qup.c`/`gcc-sdm660.c`/`pinctrl-sdm660.c`、小米 4.19 CAF 的 `i2c-msm-v2.c`/`gt9xx_zql6019/`），唯一真分歧是位时钟字段打包，取可手推的 mainline；Goodix 复位用厂商时序与 `clover.dts` 里 `ts_int_defalut` 声明的 16 mA，身份解码按 `gtp_get_fw_info()` 的字节布局。三档 `clover-input=1/2/3`（纯读对照 / bring-up 不出 SCL 边沿 / 问地址），每趟传输头部一次 SW_RESET、FIFO 模式零 MX 计数 + `REPACK_EN`、rx tag 写进输出 FIFO，完成判据比上游更严（done 位与字节数必须同时到位，否则算超时而不是短数据）。排在 PMGR 尾部、eMMC 之后，因为读数唯一的出路是卡上的内核日志环；任何路径都不 panic | **已上机**（v0.5.40 那一帧）：`no address ACKed` + `done xfers=4 nack=4 timeout=0` —— 电气上是通的（有 NACK 就说明总线被驱动回了话），但 `gt9xx@5d` 与 `@38` 两个地址都没醒，`rails unproven` 从推论变成读数。构建侧：内核 `09fea490…` → 归一化交付 `2f13ca62…`（AMFI 命中点随链接布局挪到 `0x1aa80e4`/`0x1aa816c`，object 时间戳 962→966）→ payload `887043d3…`（探针 6857 字节 / 37 段）；`verify-clover-payload.py` 新增 `touch instrument linked`，显示档案闭环重审通过（193 绑定、两个 object 哈希与上一轮逐字相同） |
| round182（v0.5.39） | 排期第 6 格第一步，也是 round181 最后一行 `rails unproven` 的回答：`drivers/CloverPower` 打开内核通往 PMIC 的唯一一扇门——SPMI PMIC Arbiter v5。五个窗口逐格钉在 `qcom,spmi@800f000/reg`（core `0x0800f000+0x1000`、chnls `0x08400000+0x1000000`、obsrvr `0x09400000+0x1000000`、intr `0x0a400000+0x220000`、cnfg `0x0800a000+0x3000`），读版本号并按 v5 带宽拒绝（低于 `0x50000000` 是 v2/v3 的 `0x800` 地图基址，达到 `0x60000000` 是 sheng v7 的 `0x2000/0x20` 布局），走 `core+0x900` 的 448 项 APID→PPID 地图（平局取第一条、但被本 EE 拥有的后来者顶替，IRQ 另按「最后一个带 bit24 的」），把 `cnfg+0x700+4*apid` 的写归属**在任何人写之前**量出来——这就是排期 4 第二道闸的数；再问 PM660/PM660L 的 revid 身份（`0x51` 不对就不报身份），最后读触摸面板声明的两条轨 `ldo-a11`（PPID `0x04a`）/`ldo-b3`（`0x242`）的 `0x40/0x41` 电压原始字节与 `0x46` enable。**只读**：每笔事务都是 *observer* 通道上的 `EXT_READL`，chnls 窗只按页读自己那页的 `ACC_ENABLE`/`IRQ_STATUS`，一次 CMD 都不往里写，这条由 checker 反向钉死（源码里不许出现 `windowWrite`/`kPmicArbWriteOp`）。三档 `clover-power=1/2/3`；事务逻辑拆在 `PowerArb.h`，宿主模型 `tests/clover_power_host.cpp` + `scripts/test-clover-power.py` 用 pthread 假设备逐条重解码命令并断言它是读，超时那一档故意不带服务线程跑，证明轮询真有界。排在 PMGR 尾部、CloverInput 之后，任何路径都不 panic | **已上机**（v0.5.40 那一帧）：仲裁器核心版本字不在 v5 带内，仪器按自己定的规矩 `not a v5 arbiter` 收手——`obs00=00000100` 证明映射与读通路是好的，`cnfg700=00000000`、`apid=-1 candidates=0` 证明它一个 APID 都没敢认，两条轨因此没读。下一步不是读轨，是先回答那个版本字到底是什么（照片首位零的个数分不清，`0x03000000` 与 `0x30000000` 都在「低于 V5_MIN」这一侧）。构建侧：内核 `7305d191…` → 归一化交付 `37c3f260…`（AMFI 两站点照旧 `0x1aa80e4`/`0x1aa816c`，object 时间戳 966→967）→ payload `0ee08570…`（卷内内核 `3efd9089…`，入口探针 6857 字节 / 37 段）；`verify-clover-payload.py` 新增 `pmic arbiter instrument linked`（七条串全在），仲裁器宿主模型接进 `check-clover.py` 闸门；显示档案闭环重审通过（`kernel_sha256` 是本轮内核，两个 object 哈希与上一轮逐字相同、193 绑定未动） |
| round183（v0.5.40） | 排期第 5 格的第一步问的不是「怎么讲 APR」，是「对面是不是已经有人」：`drivers/CloverDsp` 只读两块区域——`qcom,smem@86000000` 那 2 MiB 共享堆（头字段含 `reserved` 必须为 0 这条厂商约束、512 项全局 TOC 的有界走查、离区域末尾一页的 `$TOC` 分区表，且每行再对着它自己的 `$PRT` 头逐项校验两个 host id 与声明尺寸）和 `0x17911008` 那个 doorbell 字（按本机 DT 声明的七个掩码解码），然后按 `427 + remote_pid` **推导**出的编号找 adsp/modem/cdsp 的出、入 SMP2P 项（`$SMP` v1 + 十六项 + 两个 pid 全部相对上才算一条边），最后扫 ADSP 的 `removed-dma-pool` carve-out `0x92a00000` 与 Wi-Fi MSA `0x85700000` 各 16 页：见到几页、非零几页、第一页非零在哪、有没有字读起来像 ELF 魔数。三档 `clover-dsp=1/2/3` 逐档累加；**一个字节都不写**，因为在 APCS 上写就是中断本身，这条由 checker 反向钉死（不许出现 `windowWrite`/`ds_write`/`kSmp2pSet`/`IOMemoryDescriptor::withAddress`）。解码全在 `DspCore.h`，宿主模型 `tests/clover_dsp_host.cpp`（合成 128 KiB 堆、189 条断言）被 `check-clover.py` 当闸门跑，专挡「魔数按厂商 `u8 magic[4]` 的字面顺序抄」这一类在板子上看不见的字节序错——它会让一块活堆报成「没有分区表」，而报告里没有任何线索说原因是字节序。结尾那句 `verdict adsp_edge=… adsp_loaded=…` 就是排期 5 的分叉：有边又有镜像，第 5 格是协议轮（开 GLINK、讲 APR）；空堆加空 carve-out，第 5 格的第一步其实是排期 6 的——`qcom,pas-id = <1>` 得先压在 round182 已经量到归属的 `vdd_cx` 上 | **已上机**（v0.5.40 那一帧）：共享堆读通了——`initialized=00000001 free=4096 reserved=00000000`、全局 TOC `scanned=512 allocated=24`、离末尾一页的分区表认出来 `magic=$TOC version=1`，`part(1..3)` 都对着自己的 `$PRT` 头校验通过，其中 `part(3) hosts=0:1` 里那个 `1` 就是 ADSP，是排期 5 判决行等的候选边；`part(0) hosts=65535:65535 hdr=43476424` 被正确拒掉（头不是 `$PRT`）。判决行 `verdict adsp_edge=… adsp_loaded=…` 没上屏，原因不在引导而在屏——那一帧第一行仍是内核自己的版本横幅、最后一行正是 `part(3)`，屏在它之后画满了。构建侧：内核 `1c48bac7…` → 归一化交付 `acc94982…`（AMFI 两处命中随 0x4000 的文本增长挪到 `0x1aac0e4`/`0x1aac16c`，虚拟地址一字未动；object 时间戳 967→**968**）→ payload `839543a8…`（入口探针仍是 6857 字节 / 37 段）；`verify-clover-payload.py` 新增 `smem and adsp instrument linked`（九条串全在），DSP 宿主模型接进 `check-clover.py` 闸门，DT 常量审计从 26 个地址涨到 **30** 个；本轮另外修掉两处只在「新增档案后第一次全量重建内核」才暴露的构建基础设施缺陷——`.config` 里那份 `CONFIG_XNU_EXTRA_LD_ARCHIVES` 缓存，和不带 `-isysroot` 的共享编译记录（详见流水日志） |
| round184（v0.5.41） | 优先级的轮，不是硬件的轮：驱动开发停在原地，把「引导能自己回到 fastboot、panic 前把日志落到卡上」这条路接上——因为 round183 那一帧证明屏是一台画满就停的打印机，读它等于没读。三件事：① `clover-input`/`clover-power`/`clover-dsp` 三个 boot arg 从 `board.toml` 注释掉，**档案照旧构建、`SHENG_PMGR` 门照旧开**，只是这一轮不跑——是「停」不是「删」，checker 把「档案在、开关在、参数不在」写成一种一等状态而不是失败；② `clover-unattended=300` 回到 `boot_args` 末尾，round149 当年砍掉它换的是「什么都不打断引导」，代价是走得到的引导永远不会自己回 fastboot；③ panic 出口接上卡——导出的 sink `clover_emmc_panic_append` 在追加 panic 记录之后调用 `panicDrainRings()`，把 RAM 侧两个环（kind 21 驱动判据、kind 22 内核 tee）排干进 logfs，`kPanicTeePasses=8`、`kPanicDrainCap=48`，并报一行 `CloverSdhci r184: panic drain wrote=… dropped=… tee_passes=…`。写用不加守卫的单块原语，因此不可能在 panic 里再造一层等待 | **已构建**，待冷刷；内核 `c523b080…` → 归一化交付 → AMFI 命中 `166b827d…` → payload `954ac76a…`（入口探针仍 6857 字节 / 37 段），`boot.img` 与 round183 逐字节相同（本轮没动固件）；`check-clover.py` 绿（含死线存在/数字/60–1800 区间、sink 签名、体内 `panicDrainRings ()` 调用），`verify-clover-payload.py` 28 条断言含新增 `panic ring drain linked`；显示档案闭环重审通过（`kernel_sha256` 是本轮内核、两个 object 哈希与上一轮逐字相同、193 绑定未动）
| round185（v0.5.44） | 还是优先级的轮，这一轮补的是 round174 自己记下的那把锁：日志窗口从 logfs（p39，shipped 引导如今把它挂成根 APFS 容器）挪到 **logdump**（p56，`13320216..13451287`，131072 个扇区 / 64 MiB，量自卡自己的 GPT），三个就绪位 `gPanicLogReady`/`gVerdictReady`/`gTeeReady` 从此只看一件事——GPT 里有没有 `logdump` 这条 entry——不再要求「这一条启动已经从报告窗里读到过一条既有记录」，环因此不可能再把自己锁死。上膛前加一道尺寸闸门：`needed = 192 + 2*512 = 1216` 扇区，entry 不够长就整批不上膛并报 `logdump entry too small`，找不到 entry 报 `no logdump entry at …, rings stay disarmed`。记录格式、两环几何（kind 21 第 64 扇区 64 槽、kind 22 第 192 扇区 512 槽）、`panicDrainRings()` 与 round184 那条排干一个字没动，搬的只有窗口起点；唯一跟着搬家的是 r173 写闸门的取样，它现在以 logdump 窗口为界，拒写时报 `write gate refuses, log window first=%u blocks=%u sectors=%u`。宿主两条路同步：`read-clover-log.py --first-lba` 默认换成 13320216（旧卡要显式传 12320768），`run-clover-unattended.sh` 先试 `by-name/logdump` 再落回 logfs，拉的仍是那 1216 个扇区；三件仪器照旧停在 boot args 之外 | **已上机（logdump 读回 622592 字节），环上膛了**：kind-21 环 slot 0 就是 `log window armed from GPT first=13320216 … kernel=13320408` 原行，数值逐项一致——卡上第一次持有驱动判据（verdict 环 17 条、内核环 352 条连续）；panic 报告窗是空的（userspace panic 不走 `clover_emmc_panic_append` 那条出口，盲区如实记下）；launchd 走到了 34.28s：三个卷全挂、共享缓存 `map2 <- err=0`、fsck/mount/seputil/mobile_obliterator/apfs_boot_util/MSUEarlyBootTask/cc_fips_test 全部 rv=0，**keybagd `rv=0x4700` 是唯一非零**，日志停在 usermanagerd 的 exec 中途——下一轮第一格是把卷里 staged 的 boot-task 可执行文件与 launchd 清单对出缺的那个；内核 `dae8a8c2…` → 归一化交付 → AMFI 命中 `51a12944…`（两处站点仍是 `0x1aac0e4`/`0x1aac16c`、object 时间戳仍是 968，本轮驱动文本没长过一页）→ payload `6b73e99b…`（入口探针 6857 字节 / 37 段，卷内内核 `d9634f42…`），`boot.img` `196b284f…` 与 round184 逐字节相同（本轮没动固件）。构建侧修掉一条真缺陷：补丁回放不幂等——`zzy-clover-dyld-cache-skip.patch` 一进树，`native-4k-signature-hash-cache.patch` 就既不可正向应用也不可反向撤销，同一棵树的第二次构建必然停在 `apply --check`；按脚本已有的约定把它加进「先反向」名单（`../UniXNU/scripts/build-xnu-4k-baseline.sh:38`），同一份树连跑三遍幂等。顺带查出一件更要紧的事：工作树里 `kern_exec.c`/`kern_exit.c`/`vfs_vnops.c`/`vm_unix.c` 缺 `zzy-clover-shared-region-why5.patch` 的全部内容，而上一轮的镜像里那些串都在——它们是靠从没重编过的 object 进镜像的；本轮是第一份「源码状态与补丁集相符」的内核。`check-clover.py` 绿（含写闸门不得再以 logfs 为界的反向断言、`gVerdictFirst = entryLdFirst;` 的位置断言、旧的 mode-20 上膛块必须消失），`verify-clover-payload.py` 新增并读到 `logdump rings armed from GPT OK`；显示档案闭环重审通过（`kernel_sha256` 是本轮内核、两个 object 哈希 `1b6c134e…`/`a061787e…` 与 round183/184 逐字相同、193 绑定未动）
| round186（v0.5.46） | 还是优先级的轮，本轮一行驱动代码没写：屏上那句 `required boot task executable not found` 与卡上内核环里 `exec pid=12 comm=usermanagerd` 之间，没有任何一处说过 launchd 要 spawn 的到底是哪个路径、那个路径查到第几段断掉——内核返回用户态的只有一个 errno。前置调查先把三件事钉死：launchd 的清单是**嵌在 `/sbin/launchd` 里的 XML plist**（偏移 426879、9237 字节、`Boot` 表 34 条），required 任务失败值是和 `#0x2` 比出来的，而清单里既 required 又无 gate 且卷上不存在的那一条是 `/usr/libexec/darwinos-boot-task`。插桩因此有确定的落点：这条路上能产生 ENOENT 的只有 `exec_activate_image()` 对程序路径的 `namei()` 和 `get_macho_vnode()` 对 dyld 路径的 `namei()`，`load_return_to_errno()` 把其它一切映射成 EBADARCH/EBADMACHO/EACCES/EAUTH/EIO/EBADEXEC，不会伪装成 2。`patches/xnu-clover/zzzz-clover-spawn-why.patch` 因此只做三件事：对 pid 1（每处上限 24 条）把路径按 `/` 切段逐段 `namei()`，报 `err=/n=/path=/stop=/good=/fs=/on=/mflags=/retry=`；在 dyld 那一侧的失败 namei 里单独报一行 `loader-why`，免得「缺 dyld」被读成「缺 boot task」；同一整条路径立刻再查一次，粘住的失败和偶发的失败在 `retry=` 上分家。**只加日志**：返回给用户态的 errno 一字未动，checker 把这条写成硬断言（补丁里除 `---` 头不许出现任何以 `-` 开头的行）——135 行、99 条新增、0 条删除。三个实现细节各能坑掉一次冷刷：本 XNU 的 `nameidone()` 只放 pnbuf 不放 `ni_vp`，逐段走查每段都得自己 `vnode_put()`（且先取 `vnode_mount()`）；NDINIT 的路径参数是 `user_addr_t`，`char[]` 直传是 `-Wint-conversion` 硬错误，必须 `CAST_USER_ADDR_T()`；板级补丁的名字就是排序契约——第一次生成的那份 878 行里同时出现 `boot_progress` 与 `XNU_OWNER_*`，因为 `save-xnu-patch.py` 会把「名字排在它之前且碰同一批文件的补丁」的已进树编辑算成它自己的内容，改名 `zzzz-` 之后才是这 135 行。构建侧留下一条**没修**的死结：`unixnu` 先跑 `[kernel].archives` 再跑 XNU，而档案要用 `nm` 读 `link.filelist` 上每个 object，身份戳删掉的 `kern_exec.o` 因此让构建停在 `nm: error: … kern_exec.o: No such file or directory`，本轮用一次性 object 预 pass 绕过 | 内核两次独立 pass `55ec10fe…`/`dfe149c3…` → 归一化收敛到同一个 `b4f898c9…`（这同时是补丁链在同一棵树上第二次干净重放的证据）；AMFI 两处命中随 0x4000 的文本增长挪到 file `0x1aad0e4`/`0x1aad16c`、虚拟地址一字未动、object 时间戳仍 968；payload `f7c130ea…`（入口探针 6857 字节 / 37 段，卷内内核 `426201d7…`、`unixnu.toml` `c5272a2b…`、ADT `1ecd4345…` 13516 字节），`boot.img` `196b284f…` 与 round183–185 逐字节相同（本轮没动固件）。`check-clover.py` 绿（新增一节：补丁在场 + 九条针 + 只加日志的反向断言 + 不许出现 `clover r185:`/`clover r170:` 的旧串；标题升到 `clover r186:` 而驱动戳照旧 r185，因为本轮没动驱动），`verify-clover-payload.py` 新增并读到 `spawn ENOENT probe linked OK`（32 条 OK），五道宿主自测全 PASS；显示档案闭环重审通过（`kernel_sha256` 是本轮内核、两个 object 哈希与 round183–185 逐字相同、193 绑定未动）。**已构建**，待冷刷（只写 cache）：这一帧要么看见 `spawn-why` 那行并读出 `stop=` 的边界，要么用它的缺席否掉「生产者就是 namei」这个前提 |
| round187（v0.5.47） | 还是优先级的轮，本轮同样一行驱动代码没写。round186 那一帧把墙换了位置：spawn 探针只在 init 的正常回退上响了一次（`path=/usr/appleinternal/sbin/launchd.development stop=/usr/appleinternal good=/ retry=2`），没有任何 boot task 的 ENOENT，而 `usermanagerd`（pid 12）在 37.193360 被 exec、37.233612 打开 `/System/Library/Caches/com.apple.dyld` 之后，到抓帧为止 34 秒整个内核环一行新字都没有——launchd 是在**等**它，不是找不到它。所以这一轮问的是「最后进去没出来的那个调用是几号」，`patches/xnu-clover/zzzzz-clover-syswhy.patch` 因此只做三件事：在 `bsd/dev/arm/systemcalls.c:unix_syscall()` 里对 `comm` 为 `usermanagerd`/`keybagd` 的调用打印 `unix-in n= pid= tid= comm= code= a0= a1=`；同一条路上的非 `EJUSTRETURN`/`ERESTART` 返回打印 `unix-fail … err=`（一次**返回了 errno** 的调用和一次**永不返回**的调用是两种完全不同的墙，只有 errno 那一行能把它们分家）；mach trap 走的是 `osfmk/arm64/sleh.c:handle_svc()` 里 `trap_no < 0` 分支、**根本到不了** BSD，所以在 `mach_syscall(state)` 之前再挂一个 `clover_sys_why_mach()`，由 systemcalls.c 里的同一个过滤器打印 `mach-in … trap= a0= a1=`。按 `comm` 而不是按 pid 过滤，是因为 pid 每次开机都会漂、而 `fsck`/`mount`/`seputil` 会在同一秒里把环刷满；按 `comm` 过滤，环里留下的每一行都属于同一个可复现的角色。三条线各有预算：entry 512 条、fail 128 条，之后每 128 条打一次心跳——**预算用尽后的沉默**和**进程挂死**必须在帧上可区分，否则下一位读帧的人会把探针自己的上限误读成新的墙。**只加日志**：两处插入都不改任何返回值、不删任何已有行，checker 仍把「补丁里不许出现以 `-` 开头的行」写成硬断言（115 行、新增 72、删除 0）；驱动戳不动（display r180 / input r181 / power r182 / dsp r183 全留），`board.toml` 标题升到 `clover r187:`。构建侧还是那条一次性 object 预 pass（round186 已记录的 archives-vs-XNU 顺序死结没修，不在本轮范围） | 内核 `5c07c1e1…`（三条 r187 格式串在 Mach-O 里逐字可读），AMFI 两刀落在 file `0x1aad0e4`/`0x1aad16c`（与 round186 同址，虚拟地址未动），归一化 `e4a929b8…`（968 object 戳全清）；payload `fc04f57e…`（56 MiB，卷内内核入口探针 6857 字节 / 37 段仍成立），`boot.img` `196b284f…` 与 round183–186 逐字节相同（本轮没动固件）。`check-clover.py` 绿（标题 + 补丁在场 + 十条针 + 只加日志反断言 + 旧串缺席），`verify-clover-payload.py` 新增 `syscall-in probe linked OK`（unix-in/unix-fail/mach-in/comm-filter 四项全 present），五道宿主自测全 PASS。**已构建**，待冷刷（只写 cache）：这一帧要读出 usermanagerd 停住前最后一条 `unix-in`/`mach-in` 与它前面的 `unix-fail`，或者用「mach-in 那侧一条都没有」把墙完全划到 BSD 那半 |
| round188（v0.5.48） | 还是优先级的轮，本轮同样一行驱动代码没写，但 round187 那一帧把两个 stuck boot task 都读出来了。`keybagd`（pid 11）走完的是它自己写死的 fatal 路径：`code=55` 是 `reboot(2)`、Protobox 回 `deny(1) syscall-unix 55`（`err=1` EPERM），下一行 `code=1 a0=47` 就是 `exit(71)`——与宿主反汇编里那条唯一的 `bl _reboot`（file `0x100015b24`，前面 `fprintf("FATAL KEYBAG ERROR: %s")` / `fwrite("REBOOTING INTO RECOVERY MODE.")`，结尾 `reboot(0)` / `exit(71)`）逐字对上，所以 `rv=0x4700` 不是崩溃；`usermanagerd`（pid 12）最后一条是 `code=397`（`write_nocancel`）`a0=1`，前一条是 `trap=-47`（`mach_msg2_trap`），此后到 300 秒死线复位环里一行新字都没有。于是本轮只问三件 r187 没抄回来的事：write 进内核时带着的 fd/长度/前 64 字节文本、每次失败的 open/stat/access 背后那条用户态路径（openat 一族取 `x[1]`）、以及每条 write 的返回值。`patches/xnu-clover/zzzzz-clover-syswhy2.patch` 因此只加三条打印（125 行、100 新增、0 删除，只碰 `bsd/dev/arm/systemcalls.c`），预算 path 320 / write 96 / 文本 64 字节，**只加日志**的硬断言照旧，checker 的「旧轮串缺席」改成只看新增行（r187 的打印是补丁的上下文）。构建侧还是那条一次性 object 预 pass | 内核 `007e0122…`（归一化 968/968）→ AMFI 两刀在 file `0x1aad0e4`/`0x1aad16c` 得 `bf50c592…` → payload `bf61bdcc…`（56 MiB，卷内内核 `c58d218b…`、`unixnu.toml` `683c4b4a…`、ADT `dcc620b6…` 13516 字节、trustcache `7c369ce0…`、data 种子 `614b8356…`；入口探针 6854 字节 / 37 段）；`boot.img` `196b284f…` 与 round183–187 逐字节相同。`check-clover.py` 绿（标题 + 补丁在场 + 12 条针 + 只加日志 + 旧轮串只查新增行），`verify-clover-payload.py` 32 条 OK 含新增 `write/path contents probe linked OK`，五道宿主自测全 PASS。**已构建**，待冷刷（只写 cache）：先看有没有 `unix-wrote … fd=1` 把「write 没回来」和「回来了但之后没调用」分开，再看 `unix-write … txt=` 那句最后的话，最后看 19 条 `unix-path … err=2 path=` 是不是同一组路径 |
| round189（v0.5.49） | 还是优先级的轮，驱动代码只动一个常量。round188 那一帧把「write 没回来」彻底排除了：`unix-wrote … code=4 fd=3 ret=1020 err=0` 与 `unix-write … len=1020 txt=bplist00…VNUMENTTBLOB\DEVICECONFIGZBOOTEDUSER` 成对，usermanagerd 把那 1020 字节的会话表完整写进了 fd 3，然后到死线 219 秒里整个内核环再无新行——墙不是挂住的 write，是写完之后什么都不再发生。它同时留下两处空白：`code=216`（`open_dprotected_np`）12 条 `err=2 path=` 全空，因为 r188 把这条调用归进了 openat 一族、读了 `x[1]`（那是 flags `0x701`）；以及成功的 open 从来没有行，所以「fd 3 是哪个文件」无从回答。本轮三件事：`clover_sys_why_path_arg()` 按调用分寄存器（只有 `openat`/`openat_nocancel` 取 `x[1]`，`open`/`open_nocancel`/`open_dprotected_np` 取 `x[0]`）、路径缓冲 128→256 字节；新增成功 open 的 `unix-open … fd= path=`（fd 取 `uu_rval[0]`，预算 128）；`sleh.c` 在 `mach_syscall(state)` 之后调 `clover_sys_why_mach_out()` 打印 `mach-out … trap= ret=`（预算 512、心跳 64）。预算 entry 512→4096、fail 128→256、心跳 128→64，把阅读窗口拉过死线。驱动侧 `kTeePushPerPass` 8→16：r188 的 `throttled=694` 对 `kept=580`，每趟放行量丢的正是 fd 3 前后的突发。构建侧记下两条：LATE 反向名单必须把 syswhy3 **和** syswhy2 一起反向（r189 改的是 r187 新增的行，r187 因此不再可逆），以及内核编译的是 `out/clover/tree/.cache/xnu-work` 而不是 `~/UniXNU/.cache/xnu-work`。`patches/xnu-clover/zzzzz-clover-syswhy3.patch` 仍是只加日志（170 行、新增 100+、删除 0，两条文件），checker 新增「open_dprotected_np 不得再读 `x[1]`」「mach-out 不得打在 `mach_syscall` 之前」两反向断言 | 内核 `45e591da…` → 归一化 `6bc257d1…`（968 object 戳全清）→ AMFI 两刀仍在 file `0x1aad0e4`/`0x1aad16c` 得 `6b94eb85…` → payload `78cfff67…`（56 MiB，卷内内核 `8ff9b930…`、`unixnu.toml` `2b585316…`、ADT `616f48bf…` 13516 字节、trustcache `7c369ce0…`、data 种子 `614b8356…`；入口探针 6856 字节 / 37 段）；`boot.img` `196b284f…` 与 round183–188 逐字节相同（本轮没动固件，冷刷只写 cache）。**payload 容器不是逐字节可复现的**：同一内核连做两次构建差 66 字节，其中 64 字节是 ADT `/chosen/random-seed` 每次重抽，另 2 字节在 GPT 区与它同源，卷内内核两次相同——r177 的复现性只覆盖内核。`check-clover.py` 绿（标题 + 补丁在场 + 新针 + 两条反向断言 + 旧轮串只查新增行 + tee 预算 16），`verify-clover-payload.py` 35 条 OK 含新增 `open/mach-return probe linked OK`，五道宿主自测全 PASS。**已构建**，待冷刷（只写 cache）：先看 `unix-wrote … fd=3` 之前最近一条 `unix-open … fd=3` 的路径，再看那 12 条 `code=216` 现在打出的路径，最后看最后一条 `mach-in trap=-47` 后面有没有 `mach-out` |
| round190（v0.5.50） | 还是优先级的轮。round189 那一帧把墙挪到了最后一条调用上：512 条内核环条目、tee 普查收在 `seen=2447 kept=1258 throttled=838 lap=1`，12 条 `code=216` 现在都点出了路径 `/var/logs/usermanagerd.log.0`（一个不存在的目录），随后 `/private/var//keybags/usersession.kb.writing` 以 `fd=3` 打开，1020 字节的会话表 `ret=1020 err=0` 写完，`fsync`(95) 与 `renameatx_np`(488) 都返回了，最后一条是 `fcntl(3, 0x55)`（`F_BARRIERFSYNC`）在 `[112.438859]`，此后到 300 秒死线复位整整 187 秒环里一行新字都没有，也没有 panic 或看门狗行。问题在于 tee 只在块请求的守卫尾部泵 msgbuf：barrier 挂住时用户态不会再发块请求，所以内核侧再打一行也运不下来。本轮因此做成两侧括号。内核侧 `patches/xnu-clover/zzzzz-clover-syswhy4.patch` 只对能等存储的五条调用（`fcntl`/`fcntl_nocancel`/`fsync`/`fsync_nocancel`/`renameatx_np`）打 `clover r190: unix-out … code= ret= err=`，预算 128、心跳沿用 64，仍然只加日志（2060 字节、新增 23 行、删除 0）。驱动侧把「开始」写在卡上**先于**第一次 CMD6：`clover_emmc_flush_cache()` 拿到块守卫后、`switchExtCsd (kExtCsdCacheCtrl, 0)` 之前 `sdhciSay` 一行 `flush-in flushes= on= size= wait= ms`（`wait=` 取自新导出的 `gGuardLastWaitMs`，回答「是不是我们自己那把锁挡住的」），并立刻 `teeKernelLog(); flushVerdictRing();` 先把 msgbuf 与判据环排进 logdump，提交后再打 `flush-out flushed= back= rsp= st= polls= rsp2= st2= polls2= ms= flushes=`；`CloverBlock::commitCache()` 加 `sync-in`/`sync-out ok=0\|1` 与一次性的 `sync pending=0`，把「根本无事可提交」「提交了」「提交失败」分成三种可读状态。读法：有 flush-in 无 flush-out 是我们自己的 CMD6/守卫路径；一条 flush-in 都没有是 apfs 在块层之上就挡住了；有 flush-out 说明卡收下了这次提交、墙在驱动之上 | 内核 `e7d55abb…` → 归一化 `b89f7616…`（968 object 戳全清）→ AMFI 两刀仍在 file `0x1aad0e4`/`0x1aad16c` 得 `23b89e9a…` → payload `674c8383…`（56 MiB，卷内内核 `aef349ec…`、`unixnu.toml` `2050da52…`、ADT `41b3fe98…` 13516 字节、trustcache `7c369ce0…`、data 种子 `614b8356…`；入口探针 6856 字节 / 37 段）；`boot.img` `196b284f…` 与 round183–189 逐字节相同（本轮没动固件，冷刷只写 cache）。构建侧抓到一条真缺陷并修进流程：设备仓库的补丁必须先复制到 `out/clover/tree/patches/xnu/`，否则内核会**静默地**不带它出厂——第一次构建就是这样，`verify-clover-payload.py` 报 `barrier bracket linked FAIL (unix-out ABSENT)` 才暴露。`check-clover.py` 绿（标题 + why4 补丁在场 + 新针 + 只加日志 + 旧轮串只查新增行 + 驱动侧 flush-in 早于第一次 CMD6 且 `teeKernelLog ()` 在它之前 + 四条 `CloverBlock r190:` 串），`verify-clover-payload.py` 36 条 OK 含新增 `barrier bracket linked OK`，五道宿主自测全 PASS。**已构建**，待冷刷（只写 cache）：先看 usermanagerd 那条 `fcntl(3, 0x55)` 之后有没有 `unix-out … code=92`，再看有没有一条 `CloverSdhci r190: flush-in`，最后看它后面有没有 `flush-out` 与 `CloverBlock r190: sync-out` |
| round191（v0.5.51） | 还是优先级的轮。round190 那一帧把墙从「一次 fcntl 调用」缩到「一次 CACHE_CTRL 提交」：驱动环最后一条是 `flush-in flushes=0 on=1 size=65536 wait=0 ms`（`wait=0` 说明不是我们那把块锁挡住的，`flushes=0` 说明这是本次启动第一次提交，前面压着 144 个脏块），此后 187 秒没有 `flush-out`，内核环停在 `CloverBlock r190: sync-in pending=144`。`flush-in` 到了、`flush-out` 没到，问题就落在两次 `switchExtCsd` 与它们之间的等待；而按当时的写法，`switchExtCsd` 之后的 `waitCardReady()` 最多 100 次 CMD13，每次又要自旋 4 M 次读 `SDHCI_INT_STATUS`，这正好是「几分钟的静默」的形状。本轮把这段拆成五条记录，每条写在下一条之前并立刻排到卡上：`flush-go cf= bound= ps= inh=`（`cf` 是卡自己的 `CACHE_FLUSH_TIMEOUT`，`bound` 是据此算出的毫秒上限，`inh` 是第一次 CMD6 前主机是否已经忙）、`flush-c1`（提交命令本身，带 `spins=` 与 `ms=`）、`flush-w1`（提交后的等待，带 `busy=`/`polls=`/`smp=`/`mk=`/`to=`）、`flush-c2`、`flush-w2`；等待本身也从 CMD13 风暴换成主机的 `SDHCI_PRESENT_STATE & DATA_INHIBIT`——r169 的 `waitCardIdle()` 早就证明这个忙位不必问卡，于是卡往介质里写 cache 的时候主机不再往它身上打命令。`switchExtCsd` 多一个 `spins` 出参，好把「命令根本没发出去（`st=0 spins=4M`）」和超时、CRC 分开。读法：停在 `flush-c1` 是提交命令自己没回来；停在 `flush-w1` 是卡在写介质；五条都在而无 `flush-out` 说明墙已经不在这个函数里 | 内核 `b3896c46…` → 归一化 `d621279f…`（968 object 戳全清）→ AMFI 两刀仍在 file `0x1aad0e4`/`0x1aad16c` 得 `d3a2903a…` → payload `9c334c4c…`（56 MiB，卷内内核 `a696a73a…`、`unixnu.toml` `b0e77509…`、ADT `4ea0533b…` 13516 字节、trustcache `7c369ce0…`、data 种子 `614b8356…`；入口探针 6856 字节 / 37 段）；`boot.img` `196b284f…` 与 round183–190 逐字节相同（本轮没动固件，冷刷只写 cache）。`check-clover.py` 绿（标题 + 五条 r191 串 + `flushSettle` 签名 + 忙位判据 + `cf` 取自 EXT_CSD byte 246 + 每段之后都排一次环 + 反向断言 flush 尾段不再出现 `cardInTransfer (`），`verify-clover-payload.py` 37 条 OK 含新增 `cache commit stages linked OK`，五道宿主自测全 PASS。**已构建**，待冷刷（只写 cache）：先看 `flush-go/c1/w1/c2/w2` 停在哪一条，再看 `busy=`/`mk=`/`to=` 的毫秒数，最后看有没有 `flush-out` 与 `CloverBlock r190: sync-out` |
| round192（v0.5.52） | 还是优先级的轮，但这次是把手从卡上拿开。round191 那一帧只多了一条就把问题问穿了：驱动环的最后一条是 `CloverSdhci r191: flush-go cf=0 bound=30000 ps=01f800f0 inh=0`，**其后再无一条**，`flush-c1` 没有、`flush-out` 没有、内核环停在 `CloverBlock r190: sync-in pending=144`，连 r184 的 `panic drain` 也没上岸。`cf=0` 是卡自己说 `CACHE_FLUSH_TIMEOUT` 没声明，所以上限退到 30 s；`inh=0` 是第一次 CMD6 之前主机并不忙；`ps=01f800f0` 里 `DATA_INHIBIT` 也没立——也就是说卡在 CMD6 之前一切正常，问题正好发生在提交命令本身。而这一帧的沉默还有第二层原因，比「卡不回话」更要紧：r191 的每一条分段记录都经 `flushVerdictRing()` → `writePioBlock()` **写进同一张卡**，卡一旦为提交握住 DAT0，这些记录就连发都发不出去，panic 环的排干也同样卡在这张卡上。于是这次提交不只是没有上限，而是**不可观测**——再往里加读数只会多一条运不下来的行。本轮因此不再提交：bring-up 里那两行 `switchExtCsd (kExtCsdCacheCtrl, 1)` 拿掉，缓存交还给卡（r159–r170 跑的就是这条写穿路径，每写一块已经在 TRAN 里等 `READY_FOR_DATA`，r167 的量法、r170 量到的 102 s / 122 s 这笔账本来就在付），`F_BARRIERFSYNC` 因此立刻返回、不再把一个启动任务堵在barrier 上。r171 那行照旧打印（它会读出 `ctrl=0 on=0`，就是「缓存交还」的证据），r191 的五段 flush 代码整段留着——卡若出厂就开着缓存，`cacheOn` 仍为真、flush 路径仍是活的。另外早退改成一次性出声：`CloverSdhci r192: commit skipped size= on= (once)`，这样「跳过」不会和「路径丢了」混为一谈。读法：`r171` 行 `on=0` + 一条 `commit skipped` + `sync-out ok=1` 三者同在，就说明 barrier 这次是真的过了 | 内核 `42d531c1…` → 归一化 `4d550364…`（968 object 戳全清，LC_UUID 与 `_PrelinkKCID` 由文件派生）→ AMFI 两刀仍在 file `0x1aad0e4`/`0x1aad16c` → payload `08135ca4…`（56 MiB，卷内内核 `4ceaf22a…`、`unixnu.toml` `e53abf25…`、ADT `0803d093…` 13516 字节、trustcache `7c369ce0…`、data 种子 `614b8356…`）；`boot.img` `196b284f…` 与 round183–191 逐字节相同（本轮没动固件，冷刷只写 cache）。`check-clover.py` 绿（标题 + `commit skipped` 串 + `const bool cacheOn = cacheSize != 0 && cacheCtrlBefore != 0;` + 反向断言 bring-up 里不再有 `if (cacheSize && cacheCtrlBefore == 0) {` + 跳过记录早于 `acquireBlockGuard`，r171/r175/r190/r191 的针全数保留），`verify-clover-payload.py` 38 条 OK 含新增 `cache ownership reading linked OK`，五道宿主自测全 PASS。**已构建**，待冷刷（只写 cache）：先看 `r171` 行是不是 `ctrl=0 on=0`，再看有没有 `commit skipped`，最后看 usermanagerd 那条 `fcntl(3, 0x55)` 之后有没有 `sync-out ok=1` 与新的内核记录 |
| round193（v0.5.53） | 这一轮不碰驱动，碰的是「谁有权结束启动」。r192 那一帧已经越过了卡：`r171: write cache … ctrl=0 on=0`、`r192: commit skipped`、成串 `sync-out ok=1` 之后启动走到 207.93 s，`usermanagerd`、`runningboardd`、`powerd`、`containermanagerd_system`、`dyld` 共享缓存都起来了；停点是 `watchdogd` 连续三次以 `rv=0x5 ns=2 code=0x5` 退出——`ns=2` 是 `OS_REASON_SIGNAL`、`rv=0x5` 是 `W_EXITCODE(0, SIGTRAP)`，而卡上 job plist 的 `LaunchEvents` 按 `IOProviderClass = IOWatchdog` 匹配一个本板不存在的服务（上游 `check-watchdogd-branches.py` 记录过物理硬件上这个查找失败会 `brk`），同一份 plist 又设了 `PanicOnCrash`/`PanicOnConsecutiveCrash`，第三次死亡于是走到 `proc_handle_critical_exit()` 并把机器打崩。桌面优先、驱动暂停，所以这一轮不写 IOWatchdog，而是把这类崩溃从「致命」里摘出来：boot arg 加 `panic_on_crash_disabled=1`（`proc_should_trigger_panic()` 里排在 `initproc` 判定之后、crash 行为判定之前；launchd 真死仍然 panic），并新增 `patches/xnu-clover/zzzzzz-clover-crash-why.patch`，在 `proc_handle_critical_exit()` 入口打印 `clover r193: crit pid= comm= rv= sig= fatal= sigcrash= crashbits= spawnfail= deadline= now=`——探针在早退之前，被抑制的那几次也留记录，`crashbits`/`sigcrash`/`deadline` 正是判定用到的输入 | 契约：`check-clover.py` 认 `clover r193:` 标题、`panic_on_crash_disabled=1`、补丁与探针顺序；`verify-clover-payload.py` 新增 `critical exit reading linked`（r193 串 + r170 串）。读法：下一次 watchdogd 死亡应看到 r193 行后紧跟 `panic-on-crash disabled via boot-arg`，启动越过 208 s |

round171 + round172 + round173 一帧问九件事：`read guard` 是否整批消失（守卫只等、
不拒），
`tx_flush … took … us to flush` 是否从 102/120 秒掉回毫秒量级，
`multi read fail … st=0x21 w=1024/1024` 是否不再出现且 `readcheck … trusted=1`
上屏；再加上 `CloverSdhci r172: dma up data=…` 有没有出现、
`dmacheck … trusted=` 与 `kbs=…/…` 那两个速率数、`first dma read … ms=` 有没有
落地；最后 `CloverSdhci r173: wrcheck … control=… dma=… diff=…/… trusted=` 与
`first dma write … ms=`。前五件成立，launchd 的页才继续映射得进来；后四件给出
536 与 `/data` 之前是否要先重排吞吐这一格的答案。

round174 之后，这九件事**可以**不再取决于照片：`run-clover-unattended.sh` 一次拉 logfs 的
1216 个扇区（round176 起两圈相接要这么多），`read-clover-log.py --verdict` 按先后
打印驱动环里的判据。同一次冷刷里，panic 尾巴（前 32 扇区）、驱动判据（第 64 扇区
起）和内核日志（第 192 扇区起）来自同一条启动，读的是同一张卡。**读这一侧在 2026-10-29
被证过了**：板子在 TWRP 里、宿主一条 `dd ... 2>/dev/null` 稳定回整段分区（脚本现在跑的就是这条
命令；脚本自己那一趟端到端还没重跑过，因为板子几分钟后就离开了 USB 总线）。也正是在那一次通读里，**写这一侧被推翻了**——整个 16384 扇区的 logfs 分区与
round127 刷进去的那 8 MiB 裸 APFS 容器逐字节相同，一个 `UXNU` 都没有。原因在驱动自己的
上膛条件里：`gVerdictReady`/`gTeeReady`/`gPanicLogReady` 只在「这一条启动已经从报告窗里读到过
一条既有记录」的分支里置位，而 round127 那次刷写把唯一的既有记录抹掉了，于是此后每一条启动
都读不到记录、于是不上膛、于是不落卡、于是下一条启动照样读不到——环把自己锁死了。
所以现在的任务不是「读得再快一点」，而是先把锁解开：让环在 GPT 认出 logfs 分区时就上膛，
或者把窗口挪出根容器所在的那块分区。（这一句 round185 兑现了，两半都做：见下面 round185 那段。）

round175 之后，环里不再只有驱动自己的话。dyld 的 `shared_region` 探针、r170 那对
`exec`/`exit`、APFS 的 jhash 与 ntree 错、`load code signature error 4`、launchd
的抱怨——这些从 round146 起就只能在「恰好滚到面板上那一秒」被拍到的行，现在和驱动
的判据排在同一个环里，按序号先后给宿主。于是排期第 1 条（536 与空共享区）第一次变成
一个不需要人在机器旁边蹲着的问题：刷完、回 recovery、拉一次 logfs 就是一整条启动的
文本。

那一轮记下过一笔代价，写在驱动的形状里：一圈 32 槽，两个说话人。`load code
signature error 4` 每次 exec 一行，一个起不来的守护进程能在卡落笔之前把某条判据挤出
环——回答这一轮的探针顺手抹掉下一轮的读数。round176 把它拆成两圈：kind 21 归驱动，
64 槽、每趟最多落 4 条；kind 22 归内核日志，512 槽、每趟 8 条；序号各走各的。分开
之后白名单才敢加，加了 `cs_blob`、`dyld`、`mountroot`、`rootvp` 四条，每条都在 XNU
源码里对着 `printf` 逐字核过。内核环的意思因此唯一：里面只有 kernel 说的话——宿主
反问 `--tee --driver` 必须数到 0，数到别的就说明 tee 在把抄来的东西再抄一遍。至于
「tee 没读到」还是「读到了但白名单没命中」，由驱动环里那行 `tee seen=/kept=/
throttled=` 回答，它自己不再被挤。

round178 要读的是三个新数，都在驱动环里：`cuts=` 说明守卫真的在请求中途放过手，
`waits=` 与 `worst=` 给出这一条启动里最长的一次排队是多少毫秒。判据是一句话——如果
pid 1 的 SIGBUS 消失而 `tx_flush … took … us` 仍在，那页调入不是被写请求挡死的，
下一格该改的是写段的长度而不是放手的时机；如果 `worst=` 自己就涨到秒级，那是卡的忙
在排队，本轮的换手只是把它摊平了。

round180 要读的是三行，都在卡上的内核日志环里（kind 22；round176 的白名单本来就含
`clover r`，本轮没为它加任何东西）。`probe: console 1200x1920 row=… depth=… display=…
rotate=… scale=… base=9d400000 length=…` 是固件把屏交出去之后**内核自己**看到的形状，
也是板内驱动那句 `primary = video.v_display != 0` 唯一能依据的数；
`probe: object=… size=6304 parent=1 init=<IOReturn>` 里的 6304 就是 round179 从本板注册
指令推出来的 `0x18a0`，`parent=1` 是三级 `metaCast` 打中了原类；否则回来的是一条拒绝行
（`preModLoad refused`、`postModLoad=<n>`、`no console descriptor`、`console rejected`、
`allocation failed`），它直接说这个类死在哪一步。判据是顺序的：phase 1 三行齐了才准开
phase 2（等 IOSurface 根）和 phase 3（GOP nub、attach/start），因为那两格会真的落到本板
还没资格化的 backend 槽位上；`offline-legacy-validate/transfer/retire/pm` 与
`sheng-fb-gpu` 是换手通路，屏还没开始呈现就没有换手，本轮一概没传。

round181 也只问一件事：**这条总线上有没有人在应答**。读数同样全在卡上的内核日志环里，
一次 `run-clover-unattended.sh` 就够。第一件是 `declared qup=c17700 size=1000 dt_size=600
gcc=100000+94000 intid=129`——让读数对着 DT 的声明比，而不是对着记忆比；第二件是
`bring-up ok fifo out=… in=… clk=…`，它出现才说明 AHB 投票、RCG 换源、分支使能和 core 出复位
这四步都过了，`out` 为 0 则说明那次 `IO_MODE` 读回来的不是硬件自己的尺寸描述；第三件把
「地址上没人」和「总线被拉住」切开——`xfer … NACK` 是有答案，`xfer … timeout` 才是问题。
若两个候选都不 ACK，本轮预期如此（两个 DT 触摸节点都声明了 supplies，而驱动一次都没碰 SPMI），
要说清这一点靠 `no address ACKed; rails unproven, sda=… scl=… status=… op=…` 那四个字：
线被压在低电平说明有芯片卡在传输中途，那是下一格（电源轨）的证据，不是这一格（总线）的失败。
顺序仍然是闸：bring-up 齐了才准问地址，地址 ACK 了才准谈配置下发，配置下发才准谈多点上报。

round185 做两件 round174 那段欠下的事，都只动窗口的位置和上膛的条件，不动卡的格式。第一件：
日志窗口从 logfs 挪到 **logdump**——卡上的 p56，`13320216..13451287`，131072 个扇区、64 MiB，
量自 `facts/81ffcaaa-20261022-210928/mmc_partitions.txt`。logfs（p39）如今是 shipped 引导挂上去的
根 APFS 容器，驱动继续往里写就是往别人的文件系统里涂字节：报告槽 0 落在超级块那一段，两个环
落在已挂载的空间里，round173 那道写闸门取样的 +4096 扇区那 16 块也在根里——三处都得搬家，
连 r173 的取样窗一起搬。第二件：上膛不再要求「本板已从报告窗里读到过一条既有记录」，改成
**GPT 里认得出 logdump 这一条、且这条的跨度装得下三个环**（`64 + 2×64` 到 `192 + 2×512` = 1216 扇区）；
两个拒绝分支各自往卡上说一句（`logdump entry too small` / `no logdump entry at %u..%u`），
所以下一帧要么看见 `CloverSdhci r185: log window armed from GPT first=… sectors=…`，
要么看见它为什么没上膛，不会有第三种沉默。格式一字未改：报告/panic 槽 0..31 在窗口开头、
kind-21 驱动环在第 64 扇区、kind-22 内核环在第 192 扇区，宿主仍是一次 `dd bs=512 count=1216`。
`run-clover-unattended.sh` 先试 `/dev/block/bootdevice/by-name/logdump`（再 `logfs`、`mmcblk0p39`
作为旧卡的退路），`read-clover-log.py` 的 `--first-lba` 默认改成 13320216。
**环到底上没上膛，要等这一刷自己回答**——本轮只做到「镜像里那两句话在、闸门挪了家」。

同一轮里冒出来的是构建侧的缺陷，值得单独记：`build-xnu-4k-baseline.sh` 的补丁循环只保证
**从干净树**跑通，第二次构建会断在 `native-4k-signature-hash-cache.patch`——`zzy-clover-dyld-cache-skip.patch`
往这块补丁自己新加的结构体字段里又插了一行，于是前面那条既 `--reverse --check` 不过（上下文已经不是它自己写的样子）
也正向 `--check` 不过（字段已经在了），脚本按规矩停在「宁可半途不装」的那一步。修法用的是脚本自己
为这类重叠准备的名单：把 `zzy-clover-dyld-cache-skip.patch` 加进开头那段「先反掉」的 late 补丁里
（UniXNU/scripts/build-xnu-4k-baseline.sh:38），同一棵工作树连跑三遍幂等。顺手把 `.cache/xnu-work`
复位重放时还查出一件更要紧的：`kern_exec.c`、`kern_exit.c`、`vfs_vnops.c`、`vm_unix.c` 四个文件里
`zzy-clover-shared-region-why5.patch`（round170 那对 `exec`/`exit` 行、缓存目录复读的限流）**根本没在树上**，
而镜像里那些 `clover r170:` 字符串是靠没有重编的旧目标文件混进链接的——也就是说上一轮跑在板子上的
内核，源码状态和补丁集合并不一致。本轮的内核是先把 `.cache/xnu-work` 复位到 `d8b8029` 的干净树、
再按名序重放全部补丁编出来的（72 个文件被改，另有两个由补丁新建），
读回来的 `exec`/`exit` 行第一次真的对应一份可复现的源码。

### 排期（未开始 · 已知限制）

0. **当前优先级（round184 起）** — 先把 iOS 引导推到桌面，驱动开发排在它后面。
   round181/182/183 那三件仪器（触摸、SPMI、SMEM/ADSP）的档案仍在镜像里、开关仍开着，
   但三个 boot arg 已从 `board.toml` 注释掉：它们已经各自回答完自己那一问，继续跑只会被
   那台画满就停的屏截掉后面的话。等桌面到了，再按下面第 4、6、5 格读回来的答案接上。

1. **共享区映射（syscall 536）** — 头字段与签名注册两段已被 round159 帧关掉，
   映射这一格还空着；排在守护进程起来之后。round176 起这一格的读法变了：
   `clover r154:` 的 536 三行、`mapfile kr=`、`clover r166: sig` 与 r170 的
   `exec`/`exit` 都从卡上的**内核日志环**里读（kind 22，512 槽），不再和驱动的
   判据抢同一圈位置。
2. **完整 `/data`** — 现在只是 32 MiB 的 APFS scratch 种子，追加在 userdata 的
   两个 GPT 副本之间、不进 GPT；原 54.4 GB userdata 内容没有迁移。
3. **显示与 GPU** — 原始 `IOMobileFramebufferLegacy` 已在板内内核逐槽资格化
   （round179，`build/display-analysis/clover-mobile-graphics-legacy-abi.json`），
   round180 起链接进来的已经是真档案（`build/native-legacy-display/libNativeLegacyDisplay.late.a`）
   而不是 `libCloverDisplayStub.a`，phase 1 的登记与构造已在镜像里等读数。剩下的依次是
   phase 2（IOSurface 根）、phase 3（GOP nub、attach/start）、phase 4（presenter 换手），
   每一格都要上一格的读数才准放开。MDP5/DSI 的硬件通路仍未开始；Adreno 512 无驱动，
   没有 iOS 显示栈。
4. **输入** — 触摸（`edt-ft5406` / focaltech `fts` / `gt9xx`）没有能上报的驱动。
   round181 起 `drivers/CloverInput` 已经在总线上问地址（QUP v2 I2C-3 的 bring-up、
   Goodix 复位时序、两个候选各自的第一读），但它止步于此：电源轨（SPMI/PMI632）未证、
   触摸中断（gpio67，DT 声明 INTID 129）未接、上报协议未解。下一格取决于这一帧的答案是
   ACK 还是 `no address ACKed`——v0.5.40 那帧答的是后者（`done xfers=4 nack=4 timeout=0`：
   总线被驱动回了话，两个地址都没人醒），所以这一格的第一件事是电源轨，而轨归第 6 格。
5. **网络、音频、USB** — WCN3990 WiFi/蓝牙、WCD 音频、USB device 模式都没有驱动。
   这三件事的答案都跑在同一颗处理器上（ADSP），所以 round183 起
   `drivers/CloverDsp` 先读那两块共用区域——SMEM `0x86000000` 与 APCS doorbell
   `0x17911008`——并扫 ADSP 的 carve-out `0x92a00000` 与 Wi-Fi MSA `0x85700000`，
   专门用来分叉这一格：`verdict adsp_edge=yes adsp_loaded=yes` 就是协议轮
   （开 GLINK、讲 APR、走 QRTR），`no/no` 就是这一格的第一步其实是第 6 格的
   `qcom,pas-id = <1>`。v0.5.40 那帧读到了堆是活的（`initialized=1`、TOC
   `scanned=512 allocated=24`、`$TOC` 分区表认出来、`part(3) hosts=0:1` 那个
   `1` 就是 ADSP），判决行本身没上屏——屏在它之前画满了；GLINK/APR/QRTR 一个包都没发，
   `qcom,icnss@18800000` 的寄存器一次都没读。
6. **电源管理** — 除 AMFI 需要的那颗 nub 外，充电、电池、PMIC 都没有真驱动。
   round182 起 `drivers/CloverPower` 能**问**了：SPMI PMIC Arbiter v5 的 APID 地图、
   每条轨的写归属 EE、PM660/PM660L 的 revid、`ldo-a11`/`ldo-b3` 的 enable 原始字节，
   全走 *observer* 通道的 `EXT_READL`，一笔写事务都不发起。写一条轨是这一格的下一格，
   而它等的是那帧的 `write-owner` 读数——v0.5.40 那帧没有：核心版本字落在 v5 带宽之外，
   仪器按自己定的规矩 `not a v5 arbiter` 收手，一个 APID 都没敢认（`cnfg700=00000000`、
   `apid=-1 candidates=0`），两条轨因此没读；能确定的是映射与读通路本身是好的
   （`obs00=00000100`）。所以这一格先回答「那个版本字到底是什么」，再谈写。
   第 5 格要是落在「ADSP 从没被装载」那一侧，
   它压的 `vdd_cx`（pm660l L9，RPM 级 TURBO）也归这一格管。
7. **eMMC 数据通路已上 DMA，速度还没量到** — 读的那条 ADMA2 在 round172 镜像里，
   写的那条在 round173 镜像里（描述符抄自本板 CAF 树，数据走自有连续 bounce；
   读要批量 PIO 逐字相同才准用，写要日志窗那 16 块读回原样才准用——窗在 round185 起是
   logdump，此前是 logfs），
   `cap=742dc8b2` / `hcl=38` 证明这本来就是固件自己在用的通路。剩下的事实只有帧
   能给：`dmacheck … kbs=` 与 `wrcheck … kbs=` 那两对速率、
   以及至今没被读到过的 `write settle took … ms`——round174 起这几行会从卡上的
   verdict 环里读出来，而不是等一张恰好拍到它的照片。HS400/CQE 等这几数真的落地再定。
   被闸门挡在 DMA 之外的还有 panic 屏与 bring-up 报告的两块写：最后那句话不该
   依赖本轮任何标志。
8. **存储栈形态** — 没有 UFS、virtio-block 或 native verified-root；sheng 的存储栈
   是 SM8550 专有的，不能照搬。

## 关键硬件事实

大部分常量来自公开的设备树，在 `feeds.toml` 里钉住版本，`unixnu fetch` 会保留
一份副本；平板自己能回答的那些来自 `facts/<serial>-<stamp>/` 下的 dump
（现有的一份是 2026-10-22 采集的 `facts/81ffcaaa-20261022-210928/`）。

| 项目 | 值 | 来源 |
| --- | --- | --- |
| SoC / msm-id | SDM660，`qcom,msm-id = <317 0x0>`（`0x13d`） | 下游 `sdm660.dtsi` |
| 下游 board-id | MTP 基座上的 `qcom,board-id = <11 0>`；merged dtb 报 `<0x0001000b 0x0>` | `sdm660-mtp-clover.dts`、dump |
| 核心 | Silver `0x0-0x3`（A53，1804800 kHz），Gold `0x100-0x103`（2208000 kHz） | `sdm660.dtsi` → `sdm630.dtsi` |
| 时基 / tick | 19.2 MHz；虚拟定时器走 **INTID 19**（timer 节点四个 PPI 1、2、3、0 ⇒ 17/18/19/16） | 两套树一致、`mu-clover/Resources/DTBs/clover.dts:2233` |
| DRAM | base `0x80000000` + `0x80000000`，第二个 bank `0x100000000` + `0x7e4c0000` | `dt/memory.raw.txt`、`proc/meminfo.txt`（MemTotal 3825460 kB） |
| GIC | distributor `0x17a00000`/`0x10000`，redistributor `0x17b00000`/`0x100000`，stride `0x20000` | 下游 `sdm660.dtsi` |
| Splash / framebuffer | `0x9d400000` + `0x02400000`，即固件内存图里的 "Display Reserved" 行；overlay 的 `0x9d500000` 不作数 | `fw/xbl-uefiplat.txt` |
| 屏幕 | 1200×1920，`boe,tv080wum-nx2` / `novatek,nt51021`，107×172 mm，即 8 英寸 | 主线 + 下游 |
| 触摸 / 背光 | `edt,edt-ft5406` @ `blsp_i2c3` 0x38（下游用 focaltech `fts`）；背光 `pm660l_wled`，是 LED 驱动而非 PWM | `sdm660-xiaomi-clover-regular-common.dtsi` |
| 存储 | eMMC `sdhc_1: sdhci@c0c4000`（hc `0xc0c4000`/`0x1000`，cqhci `0xc0c5000`/`0x1000`，ICE `0xc0c8000`/`0x8000`），CQE + HS400ES，8-bit | 下游 `sdm660.dtsi` |
| userdata | mmcblk0p64，`first_lba 0xf10520` / `last_lba 0x747bfde`，即 54,449,765,888 字节 | dump |
| 保留区 | adsp `0x92a00000`、mba `0x94900000`、cdsp `0x94b00000`、venus `0x9cc00000`、ramoops `0x9fe00000`、gpu zap `0xf0b00000` | 下游设备树 |
| 串口 / USB | `blsp1_uart2` 115200 是 `stdout-path`；`dr_mode = "peripheral"`，只有 device 模式 | 两套树一致 |
| boot 镜像参数 | header version `0`、page size `0x1000`、kernel offset `0x8000`、ramdisk `0x1000000`、tags `0x100`、DTB 追加；无 AVB 尾，boot 镜像自身不带签名 | dump 的 stock `boot`/`recovery` 两个镜像互相印证 |
| 固件参照 | Mu-Silicium 的 `lavender`（Redmi Note 7）同颗 SoC：DDR `0x80000000`、FD `0x9FC00000`/`0x300000`、`append_dtb`、psci `smc` | `Resources/Configs/lavender.toml` |

## 目录

```
board.toml                    板级描述
configs/clover_defconfig      内核配置
feeds.toml                    每个常量所依据的两棵设备树
drivers/                      板级 XNU 驱动（ArmCpu、CloverPMGR、CloverSdhci、CloverBlock、CloverInput、CloverPower、CloverDsp、NativeMobileDisplay …）
package/                      链接进内核但不属于任何驱动的板级代码（显示探针）
patches/xnu-clover/           板级 XNU 补丁（探针与板级行为）
scripts/                      ADT 生成器、预检与校验、事实采集、归档构建、md0/mockfs 根镜像
notes/                        调研笔记 + development-log.md（逐轮流水）
facts/                        按机器存放的 dump，Git 忽略
```

## 构建

内核这一步只能在 macOS 上做；固件在 Linux 上构建。宿主机先补两件事：
`sudo xcodebuild -license accept`，以及给 `./unixnu` 准备一个不带版本号的
`python3`（它的 shebang 是 `env python3`，macOS 自带的是 3.9、没有 `tomllib`）：

```sh
mkdir -p ~/UniXNU-clover/build/shim
ln -sf /opt/homebrew/bin/python3.13 ~/UniXNU-clover/build/shim/python3
```

下面所有 `./unixnu` 都以 `PATH=$HOME/UniXNU-clover/build/shim:$PATH` 开头。
`scripts/check-clover.py` 没有这个要求，系统 Python 3.9 就能跑。

`--uefi` 指向的不是 `uefi-clover`，而是**应用了它 overlay 的那份 Mu-Silicium
检出**（本机 `../mu-clover`）：payload 的 panic 屏要复用固件的 8×16 点阵，
`patch-xnu-entry-probe.py` 因此去那份检出的 `FrameBufferSerialPortLib/Font.h`
取字模。

```sh
git clone --recurse-submodules https://github.com/UniXNU/UniXNU
git clone <this repo> UniXNU-clover

cd UniXNU
./unixnu defconfig clover_defconfig -D ../UniXNU-clover
./unixnu fetch
./unixnu tree
python3 ../UniXNU-clover/scripts/check-clover.py      # 预检，任何时候都可以跑
./unixnu build firmware --uefi ../mu-clover
SDKROOT="$(xcrun --sdk iphoneos --show-sdk-path)" ./unixnu build kernel
./unixnu build package
cd ../UniXNU-clover && python3 scripts/make-clover-root.py   # md0/mockfs 根 + trust cache
cd ../UniXNU
python3 ../UniXNU-clover/scripts/patch-clover-amfi.py \
  out/clover/kernel/kernel.macho ~/clover-out/roundNNN-kernel-amfi.macho
python3 ../UniXNU-clover/scripts/normalize-kernel-macho.py \
  ~/clover-out/roundNNN-kernel-amfi.macho
./unixnu build payload --unique-chip-id 0x<device seed> --uefi ../mu-clover \
  --kernel ~/clover-out/roundNNN-kernel-amfi.macho \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
./unixnu bootimg --uefi ../mu-clover --target RELEASE \
  --dtb ../mu-clover/Resources/DTBs/clover.dtb --ramdisk none
```

七条必须照办，否则构建会静默产出错的镜像：

1. **先 kernel，再 payload。** payload 里那两半都从内核产物取。
2. **payload 必须带 `--kernel …-kernel-amfi.macho`。** 每次重建内核，AMFI 的两处
   补丁都会被 prelink 恢复成 stock；那份 copy 是 `scripts/patch-clover-amfi.py`
   重打出来的，才是实机上真正在跑的镜像。
3. **`--trust-cache` 用 `build/clover-trust/trustcache.bundle`（三模块）。**
   根镜像旁边那颗 56 字节单模块 bundle 会被 `board.toml` 的 `trust_cache_sha256`
   直接拒绝。
4. **`bootimg --ramdisk none` 是必须，不是省略。** 否则它把 payload 卷当 ramdisk
   塞进 boot 镜像，而这块板子的固件根本看不见 ramdisk 里的卷。产物是
   `out/clover/boot/boot.img`，它是 boot 分区唯一需要的命令（`unixnu` 没有
   `build boot`）。
5. **打完 AMFI 补丁、进 payload 之前，归一化一次。** round177 起
   `scripts/normalize-kernel-macho.py` 把链接产物里那三处「重建不复现」的字段——
   ld 每次链接生成的 `LC_UUID`、每个 object-path 调试符号 `n_value` 里那个 `.o`
   mtime、以及 `emit_prelink.py` 用前两者哈希出来的 `_PrelinkKCID`——改写成「文件
   其余内容的纯函数」。顺序是有意义的：先归一化再打 AMFI，那 8 个指令字节就落在
   身份字段已经哈希过的范围之外。它不改任何可执行段，也不动长度。
6. **改过 `configs/clover_defconfig` 就要重新选一次 defconfig。** `unixnu status` 里那行
   `config …/out/clover/.config` 是真正的读取来源：`cmd_defconfig` 只是把
   `configs/<name>_defconfig` **复制**过去，`build kernel` 不会替你再复制。所以往
   `CONFIG_XNU_EXTRA_LD_ARCHIVES` 里加一份新档案之后，如果跳过这步，档案会照常编出来、
   链接却仍按上一轮那份 `.config` 走，报错是 `Undefined symbols "_clover_dsp_run"`
   这种「档案明明存在」的样子。参数要写全名：`./unixnu defconfig clover_defconfig
   --device ../UniXNU-clover`，写 `clover` 会报 `configs/clover not found`。
7. **object 缺席时，构建死在档案步骤而不是编译步骤。** `unixnu` 的 kernel 步骤顺序是
   「档案 → XNU」，而 `build-corecrypto-14.py` 用 `nm` 读 `link.filelist` 上的**每一个**
   object。身份戳 `.boot-progress-mode` 会删掉 `kern_exec.o` 等一批 object，所以一次编译
   失败之后，下一次构建必然停在 `nm: error: …/kern_exec.o: No such file or directory`，
   而源码看起来毫无问题。round186 的处理是一次性的 object 预 pass（先跑
   `scripts/build-xnu-4k-baseline.sh` 把 object 编出来，再走驱动），**这是绕过不是修复**——
   驱动里那两步的顺序仍然反着。

补丁层的硬规则：`build-xnu-4k-baseline.sh` 按文件名排序逐个
`git apply --reverse --check`（已在树上就跳过），否则 `--check` + `apply`，
`set -e`。所以**新探针绝不能插进排序在前的补丁自己新增的块**——那会让逆撤失败、
正打也失败、构建中止；要改前面的补丁就重生成它自身
（`scripts/save-xnu-patch.py`，先逆撤同文件排序在后的补丁）。同一条排序契约还有第二面，
round186 踩过：`save-xnu-patch.py` 生成的 diff 是「pinned HEAD + 所有名字排在它之前且碰
同一批文件的补丁」与工作树之间的差，所以**新补丁的名字必须晚于每一个碰同一批文件的旧
补丁**，否则别人已经进树的编辑会被算成这份补丁自己的内容——第一次生成的 878 行里同时
出现 `boot_progress` 和 `XNU_OWNER_*` 就是这个原因，那不是什么工作树漂移。
另外内核构建前必须在
`out/clover/tree/.cache/xnu-work` 逆撤
`zzy-clover-dyld-cache-skip.patch`（它盖住上游的
`native-4k-signature-hash-cache.patch`）。

`tree/patches/xnu` 是指向基仓 `~/UniXNU/patches/xnu` 的软链，构建只读那一份；
改完 `~/UniXNU-clover/patches/xnu-clover/` 必须同步过去，否则 forward apply 失败。

Xcode 的 clang 在重建归档时不带 `-isysroot` 会拿 macOS SDK 编 iOS 目标并把
`-Wincompatible-sysroot` 当错误，上面显式指定 iPhoneOS SDK 就是为了绕开它；
只重打 payload 不需要这一步。

round183 起这一步不再靠人记：`build-xnu-4k-baseline.sh` 是一次**全量** pass，它把
iokit 那份共享编译记录 `build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/
IOPlatformExpert.cpo.json` 重写成了不带 `-isysroot` 的样子（内核自己无所谓，因为它带
`-nostdlibinc` 且 `BUILD_WERROR=0`），而板上那四个「按记录复编驱动」的脚本
（`build-arm-driver.py`、`build-clover-block.py`、`build-clover-pmgr.py`、
`build-clover-gop-legacy-display.py`）会照抄那份记录再补 `-Wall -Wextra -Werror`，
于是新档案第一次构建就全体停在一条与驱动无关的报错上。四个脚本现在都做同一件条件
判断：记录里已经有 `-isysroot`/`--sysroot` 就原样用（保住它当初真的编过的那个 SDK），
一个都没有才补 `xcrun --sdk iphoneos --show-sdk-path`。
`scripts` 目录是按大小和 mtime **复制**进 `out/clover/tree` 的（`drivers`/`kits`/
`patches`/`refer` 才是软链），所以改了设备仓的脚本要等下一次 `unixnu tree` 才生效。

## 校验

```sh
python3 scripts/check-clover.py            # 板级预检（= [firmware] preflight）
python3 scripts/verify-clover-payload.py   # 归档对镜像：卷内形状 + 逐轮新增的探针断言（本轮 29 条）
python3 scripts/verify-userdata-window.py  # 刷 userdata 前的宿主闸门（展开 sparse、SHNGROOT、APFS 只读挂载）
```

`check-clover.py` 生成一份临时 ADT 再把 Apple 的编码解回来，断言 `/chosen`
（64 字节 `random-seed`、`nvram-proxy-data` 与 `nvram-bank-size`、`dram-base`、
`dram-size`、`crypto-hash-method`、
`root-matching`、`display-base`、4 字节的 `amfi-only-platform-code`、4 字节为 1 的
`debug-enabled`）、`/chosen/memory-map` 里恰好只有 `RAMDisk` 与 `TrustCache`
两个占位、带八个 MPIDR 的 `/cpus`、`/psci`、带 `ranges` 与 GICv3 的 `/arm-io`、
`/defaults`、`/options`、`/product`；board 一段钉住根契约：根只由
`/chosen/root-matching` 对 OS 卷 UUID 的 `IOMedia` 属性匹配声明一次，`kernel_args`
里**不许出现任何 `rd=`**（round150 起，`rd=` 会在 owner 审计之前造出第二个根声明），
`cs_enforcement_disable=1` 与 `debug-enabled = <1>` 必须在（AMFI 只在
`PE_i_can_has_debugger()` 为真时接受那个参数），`clover-apfs = 3`，
`packed_data_ramdisk` 为假，`trust_cache_sha256` 非空并与 bundle 文件一致；
最后断言当前轮次的 `clover rNNN:` 探针形状与旧轮次复读形态的缺席，并真的执行一遍
`read-clover-log.py --selftest`——驱动写环的形状和宿主解环的形状必须在同一道闸门
上互相咬住，否则「卡上有记录」和「宿主读得出」会是两套说法。自测里两圈各装各的
行——驱动环只有 `CloverSdhci ` 开头的，内核环只有不是的——并按序号还原转完一圈的
先后，`--tee`/`--driver` 的分流与「读错圈要报错」也在这一道上验一次。

round177 起同一道闸门还咬住两件宿主侧的事。`check-clover.py` 现在会真的执行
`normalize-kernel-macho.py --selftest`（同内容的两份合成构建必须收敛、三处字段之外
一个字节都不许动、形状变了的内核必须被拒绝而不是被顺手改写）和
`check-cs-blob-structure.py --selftest`（宿主复刻的那条 `cs_validate_csblob` 必须按
`ubc_subr.c` 的行号顺序拒绝，并且真机要交给 launchd 的那份 `launchd.macho` 必须被
它接受）；`verify-clover-payload.py` 的新项则用同一个解析器把**卷内**内核的字段表
再走一遍，要求 962 个 object-path 时间戳全为零、`LC_UUID` 与 `_PrelinkKCID` 各只
有一处——于是一个没归一化就进 payload 的构建会在这里失败，而不是等下一轮才发现两
次构建对不上。

round182 起同一道闸门还跑**驱动自己的宿主模型**：凡是能在宿主上判对错的东西都不许等到
板子上才发现。`check-clover.py` 依次执行 `scripts/test-clover-power.py`（SPMI 仲裁器：
pthread 假设备逐条重解码命令并断言它是读）与 `scripts/test-clover-dsp.py`（SMEM/SMP2P：
合成 128 KiB 堆，189 条断言，含「四个魔数按小端字读」这条只有宿主能挡的字节序检查），
两者非零返回都算本轮失败。被闸门跑的那些逻辑因此必须留在 `PowerArb.h`/`DspCore.h` 这种
纯 inline 头里，驱动的 `.cpp` 只留映射、解码、打印和观测数组——这条同样有正反钉子。

## 上机

payload 卷不是 boot 镜像里的 ramdisk。固件卷有 `Fat`/`PartitionDxe`/`DiskIoDxe`
但没有 ramdisk 文件系统驱动，UniXNU 只能靠扫描 `SimpleFileSystem` 找
`\EFI\UniXNU\unixnu.toml`；只存在于 boot 镜像 ramdisk 字段的卷它看不见。所以卷
写到 eMMC 分区上，就像上游 fajita 写到 `boot_a` 那样——用 `cache`
（`mmcblk0p49`，256 MiB），可以随便牺牲，也装得下 56 MiB 的卷。

```sh
fastboot flash cache ~/clover-out/roundNNN-payload.img
fastboot reboot
```

**永远冷刷**：设备先关机，再用「音量减 + 电源」进 fastboot，绝不用
`fastboot boot`。只写 `cache`，不碰 `userdata`；要写别的分区，动手前必须说清
哪个分区、为什么，并取得同意。每一刷要问的问题写在轮次日志末尾，排期攒够时
一刷可以同时回答几件。

## 读数通道

- **入口色带 0–15**：下半屏的带在 panic 屏清屏后仍在，用于确认引导阶梯走到哪。
- **panic 屏**：直写 framebuffer，不经过 `vcputc`，是这台板子上唯一不受被测控制台
  影响的通道；读数在前、寄存器证据在后。
- **`clover rNNN:` 内核 printf 探针**：每轮换号，`verify-clover-payload.py`
  断言新号在、旧号的复读形态不在。
- **日志窗（logdump）**：`scripts/read-clover-log.py` 把整段窗口拉回宿主并解码。窗口
  在 round185 从 logfs 挪到 logdump（p56，起始 LBA `13320216`）；旧卡上的记录仍在
  logfs，读它要显式 `--first-lba 12320768`。
- **verdict 环**（round174 起）：窗口第 64 扇区起是 kind-21 环（round176
  起 64 槽，每槽两个 512 字节块），驱动每句判据都落一份——**但这一句在 2026-10-29 之前只是
  代码里的意图**：环只在「本次启动从报告窗里读到过一条既有记录」时才上膛，而 round127 把
  那个窗口刷成了裸 APFS 容器，所以这台板子上的卡里从来没有过一帧；上膛条件是第一件要改的事
  （round185 改了：只看 GPT 里有没有 logdump 这条 entry）。
  头字段第 8 个字的序号让转圈的
  环仍按先后读，`--verdict` 打印，环满覆盖最旧一条并计数。屏会滚，卡不会——而且 round183
  那一帧证明屏连滚都不会：它是一台画满就停的打印机。
- **环里的内核日志**（round175 起）：同一个环有两种来源。驱动在每个握着块守卫的
  尾巴上按字节游标读 `msgbufp`，白名单命中的行（dyld 探针、`exec`/`exit`、APFS 错、
  `load code signature error`、launchd、panic）照 kind-21 落卡，**不回声给 `IOLog`**。
  `read-clover-log.py` 按行首是不是 `CloverSdhci ` 判来源并打印 `said by driver/kernel`，
  `--tee` 只看内核行、`--driver` 只看驱动行。
- **地址翻译**：`scripts/decode-clover-address.py --suspects` 出本镜像的
  tty/console 区间表。
- **无人值守**：`clover-unattended=N` 的墙钟死线，把 panic 写进 IMEM
  `0x77665501` 再 PSCI 复位（round184 起 shipped 的 boot-arg 带它，`N = 300`）。
  宿主侧 `scripts/run-clover-unattended.sh` 接住回 fastboot 的板子、进 recovery、一次
  `dd` 拉回 1216 个扇区；刷完新 payload 要读它自己那一帧的话加 `--boot-after-flash`，
  否则拉回来的是刷之前那次引导的日志（不带这个开关时脚本会先 `warn` 一句，避免读到旧帧）。等不到板子自己回 fastboot 时以 exit 6 结束，
  话里说清两种可能（死线没响，或它触发的复位到这个主机不可见）。
  **诚实的限制**（2026-10-29 实测之后改了方向）：宿主侧那一半已经证过了——`adb_serial()`
  以前只认 `device`，而 TWRP 的 adbd 报的是 `recovery`，所以 round147 到 round184 之间每一次
  「等不到 recovery」都是假阴性；把状态放宽、再把 `dd` 的 stderr 在设备侧丢掉（TWRP 会把它
  和块数据挤在同一条流上，多出来的 85 字节让任何长度校验都不可信）之后，一次 `dd` 稳定回
  622592 字节，拉的源从 logfs 换成 logdump（round185 起，先 `by-name/logdump`，落回才试
  logfs）。反过来说，驱动侧那一半还没证：拉回来的窗口与 round127 的镜像逐字节相同，
  卡上从来没有过一帧 CloverSdhci 的记录，因为环的上膛要先看上一条启动留下的记录，而那条记录
  已经被 round127 的刷写抹了（见上面 round174 那段）。round185 改的就是这一条——上膛只看
  GPT 里的 `logdump` entry，窗口也不压在根容器上——**但它上没上膛要这一刷自己回答**。
  同一个晚上板子被死线打断后**自己重启了，
  没有回到 fastboot**——跑到我们内核里的板子在 USB 上是隐形的，所以 IMEM→fastboot 这一棒
  在这台机器上仍然只是「未证」，不是「被否」。

## 相关文档

- [`notes/development-log.md`](notes/development-log.md) — v0.1 起逐轮的改动、
  照片读数与推理，完整历史；新的轮次追加在文件末尾。
- [`notes/clover-device-tree-sources.md`](notes/clover-device-tree-sources.md) —
  每个常量的来源清单，含 splash 地址的推翻过程。
- [`porting.toml`](porting.toml) — 上游要求的 13 个 area 覆盖与证据。
- [`AGENTS.md`](AGENTS.md) — 仓库边界与当前活跃的 area。

## 许可证

BSD-2-Clause-Patent，与 UniXNU 一致。`scripts/build-arm-driver.py` 移植自
UniXNU-gta4l（NoNameBuilder），采用同一许可证。
