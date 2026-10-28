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
开机必问的那一读，答案不论 ACK 还是 NACK 都从卡上的日志环里读回来。

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
| 诊断通道 | panic 屏直写 framebuffer（不经 tty）、无人值守的 IMEM `0x77665501` + PSCI 复位、`read-clover-log.py` 取 logfs、`decode-clover-address.py` 把地址翻成函数 |
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
| round181（v0.5.38） | 排期第 4 格第一次有代码：`drivers/CloverInput` 在 QUP v2 的 BLSP1 I2C-3（`0xc177000`，DT 声明 `0x600`，映射整页）上问两个候选——Goodix `gt9xx@5d` 的 `0x8040`、EDT/FocalTech `@38` 的寄存器 `0x00` 与 `0xBB` 名字——各问它们**自己的上游驱动开机必问的那一读**，因为 NACK 只有对着真实寄存器才有意思。寄存器与位全部从三个具名来源抄（本板 DT、mainline 7.2.y `i2c-qup.c`/`gcc-sdm660.c`/`pinctrl-sdm660.c`、小米 4.19 CAF 的 `i2c-msm-v2.c`/`gt9xx_zql6019/`），唯一真分歧是位时钟字段打包，取可手推的 mainline；Goodix 复位用厂商时序与 `clover.dts` 里 `ts_int_defalut` 声明的 16 mA，身份解码按 `gtp_get_fw_info()` 的字节布局。三档 `clover-input=1/2/3`（纯读对照 / bring-up 不出 SCL 边沿 / 问地址），每趟传输头部一次 SW_RESET、FIFO 模式零 MX 计数 + `REPACK_EN`、rx tag 写进输出 FIFO，完成判据比上游更严（done 位与字节数必须同时到位，否则算超时而不是短数据）。排在 PMGR 尾部、eMMC 之后，因为读数唯一的出路是卡上的内核日志环；任何路径都不 panic | **已构建**，待冷刷；内核 `09fea490…` → 归一化交付 `2f13ca62…`（AMFI 命中点随链接布局挪到 `0x1aa80e4`/`0x1aa816c`，object 时间戳 962→966）→ payload `887043d3…`（探针 6857 字节 / 37 段）；`verify-clover-payload.py` 新增 `touch instrument linked`，显示档案闭环重审通过（193 绑定、两个 object 哈希与上一轮逐字相同） |

round171 + round172 + round173 一帧问九件事：`read guard` 是否整批消失（守卫只等、
不拒），
`tx_flush … took … us to flush` 是否从 102/120 秒掉回毫秒量级，
`multi read fail … st=0x21 w=1024/1024` 是否不再出现且 `readcheck … trusted=1`
上屏；再加上 `CloverSdhci r172: dma up data=…` 有没有出现、
`dmacheck … trusted=` 与 `kbs=…/…` 那两个速率数、`first dma read … ms=` 有没有
落地；最后 `CloverSdhci r173: wrcheck … control=… dma=… diff=…/… trusted=` 与
`first dma write … ms=`。前五件成立，launchd 的页才继续映射得进来；后四件给出
536 与 `/data` 之前是否要先重排吞吐这一格的答案。

round174 之后，这九件事不再取决于照片：`run-clover-unattended.sh` 一次拉 logfs 的
1216 个扇区（round176 起两圈相接要这么多），`read-clover-log.py --verdict` 按先后
打印驱动环里的判据。同一次冷刷里，panic 尾巴（前 32 扇区）、驱动判据（第 64 扇区
起）和内核日志（第 192 扇区起）来自同一条启动，读的是同一张卡。

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

### 排期（未开始 · 已知限制）

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
   ACK 还是 `no address ACKed`。
5. **网络、音频、USB** — WCN3990 WiFi/蓝牙、WCD 音频、USB device 模式都没有。
6. **电源管理** — 除 AMFI 需要的那颗 nub 外，充电、电池、PMIC 都没有真驱动。
7. **eMMC 数据通路已上 DMA，速度还没量到** — 读的那条 ADMA2 在 round172 镜像里，
   写的那条在 round173 镜像里（描述符抄自本板 CAF 树，数据走自有连续 bounce；
   读要批量 PIO 逐字相同才准用，写要 logfs 那 16 块读回原样才准用），
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
drivers/                      板级 XNU 驱动（ArmCpu、CloverPMGR、CloverSdhci、CloverBlock、NativeMobileDisplay …）
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

五条必须照办，否则构建会静默产出错的镜像：

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

补丁层的硬规则：`build-xnu-4k-baseline.sh` 按文件名排序逐个
`git apply --reverse --check`（已在树上就跳过），否则 `--check` + `apply`，
`set -e`。所以**新探针绝不能插进排序在前的补丁自己新增的块**——那会让逆撤失败、
正打也失败、构建中止；要改前面的补丁就重生成它自身
（`scripts/save-xnu-patch.py`，先逆撤同文件排序在后的补丁）。另外内核构建前必须在
`out/clover/tree/.cache/xnu-work` 逆撤
`zzy-clover-dyld-cache-skip.patch`（它盖住上游的
`native-4k-signature-hash-cache.patch`）。

`tree/patches/xnu` 是指向基仓 `~/UniXNU/patches/xnu` 的软链，构建只读那一份；
改完 `~/UniXNU-clover/patches/xnu-clover/` 必须同步过去，否则 forward apply 失败。

Xcode 的 clang 在重建归档时不带 `-isysroot` 会拿 macOS SDK 编 iOS 目标并把
`-Wincompatible-sysroot` 当错误，上面显式指定 iPhoneOS SDK 就是为了绕开它；
只重打 payload 不需要这一步。

## 校验

```sh
python3 scripts/check-clover.py            # 板级预检（= [firmware] preflight）
python3 scripts/verify-clover-payload.py   # 归档对镜像：24 项形状与探针断言
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
- **logfs**：`scripts/read-clover-log.py` 把内核环形缓冲拉回宿主。
- **verdict 环**（round174 起）：同一张卡的 logfs 第 64 扇区起是 32 槽 kind-21 环，
  驱动每句判据都落一份；头字段第 8 个字的序号让转圈的环仍按先后读，`--verdict`
  打印，环满覆盖最旧一条并计数。屏会滚，卡不会。
- **环里的内核日志**（round175 起）：同一个环有两种来源。驱动在每个握着块守卫的
  尾巴上按字节游标读 `msgbufp`，白名单命中的行（dyld 探针、`exec`/`exit`、APFS 错、
  `load code signature error`、launchd、panic）照 kind-21 落卡，**不回声给 `IOLog`**。
  `read-clover-log.py` 按行首是不是 `CloverSdhci ` 判来源并打印 `said by driver/kernel`，
  `--tee` 只看内核行、`--driver` 只看驱动行。
- **地址翻译**：`scripts/decode-clover-address.py --suspects` 出本镜像的
  tty/console 区间表。
- **无人值守**：`clover-unattended=N` 的墙钟死线，把 panic 写进 IMEM
  `0x77665501` 再 PSCI 复位（当前 shipped 的 boot-arg 不带它）。

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
