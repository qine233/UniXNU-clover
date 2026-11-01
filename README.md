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
* boot 39（r251 实机，24 行时间序环 + 每进程计数）把最后一层窗户纸捅破了：
  `who pid=58 comm=backboardd path=3 key=3`——**backboardd 自己问了三次，两次把 `home-button-type`
  读回 `2`**。所以「用户态拿到的不是 2」这条从 r240 吊到现在的假设正式作废：`applyDefinitions:`
  的 `cmp x22,#2` 在 `==2` 一侧只打一行日志就返回，真正的无限 `dispatch_group_wait` 在
  `_configureAsync/SyncWithBlock:`，那两处一次都不问这个键；死等缺的是唯一的
  `dispatch_group_leave`（`0x100030ca8`，紧挨 `_BRController ready: device haptic asset slot count`）。
  同一帧还捞到计划外的东西：`function-button_halleffect` 回来 `len=0 val=0`——这块板子第一次
  「被问了一件没有的事」，而它能被看见纯属名字里正好有 `utton`。r252 因此把缺席单独记成一类读数：
  不按名字过滤，改按「谁在问」过滤（只记已经进过 button 计数表的那几个进程），16 行环加总数，
  同批豁免发出。下一轮干什么，直接由 `miss` 列表决定，不再猜。

* boot 38（r250 实机）第一次答了「问没问」：`asks path=9 key=6 notes=15`，SpringBoard 与 PreBoard
  都把 `IODeviceTree:/buttons` 解析成功、把 `home-button-type` 读回 `len=4 val=2`。于是「ADT 缺节点」
  「平面名字取错」「用户态压根没问」三条线一起结掉——值是对的，也被真的读走了。可这台仪器又犯了
  r249 那一类自身的错：8 格环从第 9 条起回头覆盖，而打印按槽序不按时间序，`notes=15` 里最早那 7 条
  （最可能是 pid 更小的 backboardd 问出来的）正好被吃掉；`comm=` 还被 `pid <n>, ` 前缀截成了
  `SpringBo`。r251 因此不加新仪器，只把这一台修到能答完它自己提的问题：24 行、时间序、取名字尾部、
  每进程一行计数。真正剩下来的一句话是：**用户态拿到 2，backboardd 却还在 `!=2` 那一支死等**——
  要么 MG 的值不来自这块注册表，要么 r242 那条分支语义又钉反了，先在共享缓存里查清再决定动谁。

* boot 41（r253 实机）是这台板子最短的一次开机，也是最贵的一次：内核日志一共 11 行，最后一行是
  `AppleM68Buttons:0x100000150 start`，接着 panic 槽写着
  `"AppleM68Buttons::handleStart failed to instantiate function-button_hold" @AppleM68Buttons.cpp:336`。
  驱动**确实** match 上了我们发布的 `/J120AP/buttons`——十六轮来的第一次——然后死在自己的
  `handleStart` 里，census、属性记录器、backboardd 采样一行都没来得及出。宿主侧从我们自己的内核镜像
  里把这枚 kext 的私有常量表倒出来，就看懂它要什么：`hold menu volup voldown ringerab ringeren
  halleffect halleffect1 hesattach halleffect2 skey help helpdouble app` 一张硬编码名字表，后面紧挨
  `home-button-type`、`function-button_%s` 和那句 `failed to instantiate %s`——它按名字逐条构造
  `function-button_<name>`，而 panic 点名的 `hold` 正是我们 ADT 里 `button-names` 的第一条。
  **一颗合成 ADT 的 `/buttons` 节点给不了它任何一条。**
* 对照组一句话把这条线结掉：`UniXNU-sheng/scripts/make-sheng-adt.py:185-186` 发布的是**同一个**
  `buttons` 节点（同样 `button-names = hold/voldown/volup`、同样 `home-button-type = 2`、一条
  `function-button_*` 都没有），它带着同一个 iPadOS 17.7.11 镜像到桌面；差别在
  `UniXNU-sheng/board.toml` 的 `[kernel]` 里**根本没有 `prelink.roots`**，按键由它自己的
  `ShengHidInput` 发出来。所以本板缺的不是 Apple 的驱动，是**任何一个把 HID 按键发布出来的驱动**。
  r254 把那行 root 摘回去（两个 checker 同时改成断言它**不在**镜像里——它再出现就意味着这台机器
  又回到 13 秒就死）。r255 把 `drivers/NativeHidInput` 接上了，boot 43 证明这条链在本板走通：
  census 的 `svc` 从点名 `IOHIDResource`+`buttons` 两类（`button-family=2`）变成
  `IOHIDResource`+`buttons`+**`IOHIDInterface`**+**`IOHIDEventDriver`**+**`IOHIDEventServiceUserClient`**
  （`button-family=5`，`svc total` 122→128）——本板唯一的 IOHIDDevice 就是我们那颗，家族匹配了它，
  用户态也真开到了事件服务上，全程不 panic。

> 仪器第四次复发，r255 撞上、r256 结：这次不是条数预算，是**时间窗口**。
> （第三次是 r254 结掉的：boot 40 上 `r252-mgq` 的 64 行预算被吃满，`misses total=153 shown=16`
> 那 16 行其实是四个名字重复、`soc-generation` 独占 8 行；live 打印改留给 miss、row 交给 census、
> 预算 64→128，缺席表改成**按名字去重**的计数表 `CLOVER_MG_MISSES 32`。）探针的状态行只在
> late entry（t≈25 s）打印一次，而卡上留下的 512 行内核窗口从 **t=1208 s** 才开始（`lap=24`），
> 于是本轮自己的 `clover r255:` 一行都读不到——机制成没成要靠 r243 census 的迟到点名反推，
> `published=`/`property_seen` 这两个直接答案全丢了。r256 = 把 verdict 改成慢心跳（每 120 s 重述，
> 仍在 `r255-hid` 的 32 行预算内），让任何一段晚窗口必然带着它。同一类毛病四次出现的地方都记着：
> r249 的推送上限、r251 的回卷环、r252/r254 的类预算、r255 的时间窗——一台仪器的输出通道就是它的上限。

* boot 48（r260 实机）把「答案回不来」这条结掉了：四条 beat（t=1338/1459/1578/1698）每拍都带回
  `clover r260: bus mode=3 map=0 hw=20070000 io=c0a5 op=90 err=0 status=10000c8 state=1d clk=315
  ack=0 id=0,0 xfers=4 timeout=0 nack=4 verdict=102`——就是 r183 那 42 行 burst 的压缩版：
  控制器驱动得起来，两个候选地址四笔子传输**全 NACK**，零超时。触摸的答案就此钉死：
  不在 I2C 层，在电源层。同一拍两处等待照旧。
* r261 不动板子，把 boot 48 的读数在宿主上解到底，四步都可复核：
  ① r238 的 hook 按 `strncmp(comm,"backboardd")` 匹配，所以那条 `ulock_wait2(COMPARE_AND_WAIT|NO_ERRNO,
  15bd8e6d4)` 是 backboardd 自己发起的（组基址 +0x34，`wr=0` 从未被写、`cw=0` 从未被唤醒，age=877）；
  ② `bbustk` 十帧用缓存解码器落到 `libVFXCore.dylib`（VFX.framework，子缓存 .07）与
  `VisualLogger.framework`（.31）——线程从 VisualLogger 进 libVFXCore，libVFXCore 回调进 backboardd
  自己的二进制，再从那里进 libVFXCore 睡下；③ backboardd 脱了本地符号，但 `__objc_stubs` 的桩、
  `__objc_selrefs`、`__objc_classrefs` 都带 chained fixups：lldb 反汇编桩取 selref 低 36 位落回
  `__objc_methname` 读到 `bs_filter:`/`count`/**`descriptionForRootObject:`**/`removeAllObjects`，
  类引用顺 class 结构解到 **`BKTouchHitTestContextMap`**，函数归属由 `dyld_info -objc` 的方法表按
  IMP 排序查得：调用它的 0xe7d4 在
  `-[BKHIDDirectTouchEventProcessor _queue_enumerateDigitizersForDisplay:usingBlock:]` 里，那段 C 辅助
  函数紧接 `-[BKTouchDeliveryObservationManager _queue_postPendingUpdates]`，它的 os_log 字面量是
  `canceling paths %{public}@ -- %{public}@ (ts:%{public}@)`，前面就是 `_BKLogTouchEvents`；
  ④ 还剩一笔账没结：那两帧与 `bbchain` 的 `inv` 在 slide=0x100b40000 下落在
  `mov x2, x27`/`bl _BKLogTouchEvents`（不是返回地址），在 0x100b44000 下正好是
  `-[BKHIDDirectTouchEventProcessor processEvent:sender:dispatcher:]` 里两处 `bl` 的返回地址——
  两种读法都在 backboardd 的触控事件管线内，结论不依赖它，`inv` 那条撞上的是 r257 记过的同一类错。
* 于是一条旧论断要更正：把这只组说成「触感初始化欠一次 leave」不再成立。等待线程的栈随靴变化
  （boot 45 = AssistantServices/Home.framework，boot 48 = libVFXCore/VisualLogger/backboardd 触控路径），
  堆地址也每靴不同（127d7a9e0→145d08b30→15bd8e6a0），**不变的只有账本形状**
  （`b=fffffffd cnt=1 wait=1 notif=0` 从开机早期一路到死线）。可钉的从来不是「谁的栈」，
  而是「谁该 leave」。
* 先查 sheng：它树里 `vfx`/`VisualLogger`/`descriptionForRootObject` 一个都没有，也没有任何跳过 VFX
  的开关；它到桌面靠的是 `gpu` 区那一整套（`Adreno740`+`AdrenoMetal`+`AirNir`+`Sm8550Platform`+
  `AdrenoPM4`+`AdrenoFirmware`+`ArmSmmu4K`，「空闲桌面每 250 ms 提交 6 个命令缓冲」，
  「render server 预热镜像里 162 个 Metal 库」）。本板 `gpu` 仍是 `planned`、
  `limits = ["No GPU driver, firmware or renderer is present."]`。所以剩下两条分支：**等的是
  Metal/IOGPU 的渲染完成**（GPU 区），还是**等的是 backboardd 自家另一条被卡住的队列**。
  r262 用一条读数分岔：照 sheng 的 `BootUiTraceProbe` 的形状（跨任务用原生 XNU VM API、
  热线程按上下文切换率与运行时间称出、报出 `peer`），每拍把 pid 58 的**全部**线程连同
  (tid, state, pc, lr, 内核栈前几帧, 睡在哪个字) 报出去。

每一轮的设计、构建与真机读数（含本节的证据出处）都在
[`notes/development-log.md`](notes/development-log.md) 里逐轮可查；
r236–r239 是把这条链读出来的四轮。

## 距离 iOS 桌面还差什么

| # | 缺口 | 现状（真机读数） | 下一步 |
| --- | --- | --- | --- |
| 1 | **backboardd 不回 SpringBoard 的信**（上节详述） | 十六轮的结论到 r254 为止是「缺一个发布 HID 按键的驱动」，r255–r257 把这一半**做完并测到了**：boot 43 的 census 多出 `IOHIDInterface`/`IOHIDEventDriver`/`IOHIDEventServiceUserClient`（`button-family` 2→5、`svc total` 122→128，全程不 panic）；boot 44 的 beat 是 `stage=5`、事件驱动已匹配、`transfer=8` 而 `published=0`，卡点是探针自己从 sheng 抄来的 Built-In 守卫（`NativeHidInput.cpp:296` 只在触摸设备上设 `Built-In`，按键设备永远没有，守卫永远为假）；r257 把守卫降级成读数（`deviceBuiltIn=0`）后，boot 45 读到 `Built-In=1 PrimaryUsagePage=1 PrimaryUsage=1 published=1 seen=1`，r252 缺席表从 288 条 32 个名字掉到 188 条 26 个，消失的七个全是 HID 形状的属性（`Built-In`/`DisplayIntegrated`/`HIDVirtualDevice`/`StandardType`/`CapsLockLanguageSwitch`/`KeyboardLanguage`/`AppleBluetoothRemote`），只多出更深层才会问的 `Manufacturer`——用户态确实在遍历我们那颗设备。桌面仍然没来，而且 boot 45 的栈推翻了一个旧论断：仍 `wait=1` 的主线程（tid 0x3fe，堆上的组 `base=127d7a9e0 b=fffffffd cnt=1 wait=1`）十帧用户栈全在 **AssistantServices**、等待点在 **Home.framework**，与按键无关；r239 那句「唯一 leave 在 `0x100030ca8`，挨着 `_BRController ready: device haptic asset slot count`」对应的是**另一个**栈上的组（`tid 0xec7 base=16f01bb4c b=1 g=27d22ec0 wait=1`），它也还在等。所以缺口是**两个等待者**，不是一个。（**这句在落盘时被自己撤回**：`isa=0x204e74bf0` 落进共享缓存是 ScreenTimeSettingsUI 的 **__TEXT**，不是类指针——那个「堆上的组」多半是 r239 把类型认错后按 dispatch_group 布局解出来的，所以不能算第二个等待者；能确认的只有「r257 之后 boot 45 仍 `wait=1`」。r258 先把那个地址的原样八个字与 ulock 持有者读出来。）一条候选已被 sheng 枪毙：它仓库里 `haptics`/`taptic`/`vibra` 与 `soc-generation`/`product-id`/`is-muse` 一个都没有，不发这些也到桌面 | r258 不猜名字，改读对象：对这两个组分别做 r239 对栈上那个组做过的取证（谁 alloc、`b=` 的 dq 链表、谁在等、谁该 leave），判 AssistantServices/Home 这条线程是不是第二个独立缺口。**不改 `home-button-type`，也不补 `/product/haptics` 或 ADT 身份表** r259 把这条线上最后一格形状补掉并同飞硬件问路：beat 现在通过驱动自己的通知发**一帧**合成数字化仪报告（中心点 15240/10160 = 驱动的轴上限 30480×20320，`NativeHidInput.cpp:167`；按下→50 ms→抬手，seqlock 与 `touchSend` 都是驱动自己的，探针只写它公开的六个符号；一次性，所以读法是 beat 上的 `touch=1 reports=2`——按下与抬手都进了 HID 栈），同一趟把 r184 停掉的 `clover-input=3` 放回 boot-args 并给它单独的 tee 类 `r181-i2c`（预算 24），因为合成坐标的可信度正好要靠「面板到底 ACK 不 ACK」来定：轨仍未证（r183 四帧全 NACK），NACK 在这里仍是有效答案。**r259 把这条差异填平了，并且证否了它**：boot 47 五条 beat 全读 `touch=1 reports=2`（`reports` 是驱动 `handleReport` 返回后才加一的自有计数，所以按下与抬手两笔都走完 IOHIDDevice 原实现），`presses=3` 上限仍在做事；而堆上那只组照旧 `base=145d08b30 b=fffffffd cnt=1 wait=1` 从 t=1342 到 t=1796，`bbchain` 每拍报同一只 block。**一帧数字化仪报告不是释放 backboardd 的钥匙。**新的形状读数：tid 0xeeb 与 0xe78 两只不同线程栈的 `g=45dfede0` 相同——两只线程在等同一只组；另有一只 `b=0 g=1 wait=0` 等的不是 group。输入侧已经没有可抄 sheng 的了。 **r261 的宿主取证再改一次口径**：boot 48 那条等待线程的十帧解到
`libVFXCore.dylib`+`VisualLogger.framework`，backboardd 那帧在选择器 `descriptionForRootObject:`
（receiver 类 `BKTouchHitTestContextMap`）的调用点上，位于
`-[BKHIDDirectTouchEventProcessor _queue_enumerateDigitizersForDisplay:usingBlock:]` 调用的
「canceling paths …」日志辅助函数里；r238 按进程名匹配证明 `ulock_wait2(…, 15bd8e6d4)` 就是
backboardd 自己发起、睡在组基址 +0x34、`wr=0 cw=0` age=877。所以「触感欠一次 leave」这条旧论断作废，
缺口改成一句可测的问题：**该 leave 的那笔完成来自哪里**——渲染侧（GPU/Metal，sheng 有整套
`Adreno740`/`AdrenoMetal`，本板 `gpu=planned`）还是 backboardd 自家另一条卡住的队列。
r262 = 照 sheng `BootUiTraceProbe` 的形状，每拍报出 pid 58 全部线程的
(tid, state, pc, lr, 内核栈, 睡在哪个字) 与等待对象，一次读数分岔这两条分支。
**boot 49（r262 实机）答了干脆的一半**：普查每拍报出 pid 58 的十五个线程，三个 continuation 对着
本轮内核的符号表解出来 —— `_ipc_mqueue_receive_continue`（十个，端口收消息）、
`_ulock_wait_continue`（tid 0x3b4，睡在 `101985a54` = 组基址 +0x34，`wr=0 cw=0` age 822→1087）、
`_workq_unpark_continue`（六个 dispatch 工作者停泊等活）。**「压在内核调用/触摸传输上」这条当场死了**：
pid 58 没有一个线程睡在 IOKit 或驱动路径上，而且有六个空闲工人在等活——缺的不是可跑的线程，
是那笔根本没被排进来的活儿。顺带钉住一处差点读错的地方：r238 的 `w/b/k/s` 是**累计**计数
（补丁 struct 里是 waits/blocked/woken/wakes），`a=2047ad580` 的 `w=14 b=14 k=14 s=15`
是一把热而无争的缓存内全局锁，不是十四个线程挤在同一把锁上。仪器自己的两处不足也写进读数旁边：
cap 每拍撞上（`threads=17 shown=14`），而 `_ipc_mqueue_receive_continue` 分不清「空闲等活」与
「在等一个永远不来的回音」——同形。r263 照 sheng 补这两处：普查行加 `dl=`
（`block_deadline`：0=无限等、有限=定时等，等回音是定时等），把 `wait_event` 走到 port 再走
r232/r234 的 owner 名字路报出「它在等哪个服务」（sheng 的 `peer`），上限抬到 20 并打印 `hidden=`。
同一靴 SpringBoard 换了活法：`sbs t=1250 pid=272` 里也有一条线程睡在同一种 ulock 上，窗口内 0 条 `killsig`。|
| 2 | 触摸没有任何应答 | QUP v2 I2C-3 电气是通的（两个候选地址都 NACK：`done xfers=4 nack=4 timeout=0`），`rails unproven` | 先把面板轨真的上电（依赖 3），再问地址  r259 不等轨先把地址再问一遍（`clover-input=3` 回膛），理由是同一趟正在发合成手指，需要知道总线上到底有没有真坐标 || 3 | PMIC 轨读不出来 | SPMI 仲裁器读通路是好的（`obs00=00000100`），但核心版本字不在 v5 带内，`apid=-1` 一个都没认，`ldo-a11`/`ldo-b3` 未读 | 先弄清那个版本字，再认 APID、读轨、开轨  **boot 47 的教训不是「面板没答」，是「答案回不来」**：仪器跑了，42 行说在 t≈12 s，而环这次从 t=1194.04 起——r260 不再插第二次心跳，改抄 sheng 的机制：`drivers/CloverInput/CloverInput.cpp:241` 的 `clover_input_observation[16]`（自 r181 就为「不依赖日志环」而存在）由 beat 每拍重述成一条 `clover r260: bus mode=.. … verdict=..`，总线仍一靴只碰一次；sheng 的驱动根本不 print（`TouchProbe.cpp:29` 的行数组 + `BootLog.c:340-356` 每 2 秒 compose），所以它的早期读数活得下来，因为那是状态。`mode=0` 也打印——没跑过必须说出来。 **boot 48（r260 实机）把答案带回来了，而且每拍都有**：
`bus mode=3 … xfers=4 timeout=0 nack=4 verdict=102` 与 r183 一字不差——电气与协议栈都活着，
两个候选地址全 NACK。触摸这一格从「问不到」变成「已知不应答」，下一格在电源轨（3），不在 I2C 层。|
| 4 | ADSP 没起 | SMEM 共享堆读通（`$TOC`/`$PRT` 全部自校验通过，`part(3) hosts=0:1` 是候选边），判决行还没上屏 | 有边就上 GLINK/APR；没镜像则先压 `vadsp` 电源 |
| 5 | watchdogd 每次起就崩 | 自身可执行文件里 `brk #1`（`IOWatchdog` 服务在板上不存在）。r193 的 `panic_on_crash_disabled=1` 已把它从「致命」摘掉，所以它只是噪声 | 写一个最小 `IOWatchdog` 服务，或让 plist 不再要求它 |
| 6 | **GPU/Metal 一个都没有**（Adreno 512 无驱动、无固件、无 renderer） | `porting.toml` 的 `gpu` 区 `status = "planned"`、`limits = ["No GPU driver, firmware or renderer is present."]`；r261 的取证把 boot 48 那只死等的组的等待线程解进 `libVFXCore.dylib`/`VisualLogger.framework`（渲染/特效侧的框架），而 sheng 到桌面靠的正是它 `gpu` 区那一整套 Adreno 组件（`Adreno740`+`AdrenoMetal`+`AirNir`+`AdrenoPM4`+`AdrenoFirmware`+`ArmSmmu4K`；「空闲桌面每 250 ms 提交 6 个命令缓冲」「render server 预热 162 个 Metal 库」） | 先分岔，别急着写驱动（r262）：等的是渲染完成（GPU 区），还是 backboardd 自家另一条卡住的队列。若落在 GPU，按它自己 `next_action` 那句先评 Adreno 512 能复用 sheng 的哪几块 **r262 的普查把这一格推进了一步但没有关掉**：pid 58 没有线程睡在内核/驱动路径上（所以「等一个内核侧完成」不成立），六个空闲 `workq` 工人在等活（所以「没有线程可跑那笔 leave」也不成立），剩下的形状是「那笔活儿从来没被排进来」——而它的来源要么是渲染侧的完成通知（本板没有 GPU/Metal 可用），要么是等一个永远不来的回音；`_ipc_mqueue_receive_continue` 这一层还分不清这两者，r263 的 `dl=` + port owner 就是为分辨它们而加的 |

1 挡住桌面；6 是 r261 之后 1 最可能的病根（还没证，r262 就是证它）；2–4 是之后要补的板级驱动，5 只是噪声。

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
