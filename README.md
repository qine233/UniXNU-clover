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
「在等一个永远不来的回音」——同形。r263 照 sheng 补这两处：普查行加 `dl=`——本板内核没有
`block_deadline` 这个字段（编译拒绝），定时等真记在 `thread->wait_timer`（`waitq.c:1391` 装填、
`timer_call.h:73` 存绝对期限），所以 `dl=` 报「还剩几秒」，0=没装定时器=无限等；对还停在 mach_msg2
陷阱里的线程（`x16==-47`）按 r230/r231 的老约定解出发送权名，用 r232/r233 的 owner 路报出
「它在等哪个服务」（sheng 的 `peer`）；上限抬到 20 并在表头打印 `hidden=`。
同一靴 SpringBoard 换了活法：`sbs t=1250 pid=272` 里也有一条线程睡在同一种 ulock 上，窗口内 0 条 `killsig`。 **boot 50（r263 实机）**：表头三拍全 `threads=15 shown=15 hidden=0`（cap 抬到 20 后不再吞线程）；`dl=` 四十三行全零 ⇒ 这一靴 backboardd 里**没有任何带期限的等待**，睡下去的都是无限等；`peer=-` **不携带信息**——我按 r230 的约定把 `x[6]` 当发送权名去解属主，而本板内核 `osfmk/ipc/mach_msg.c:934` 的解包是 x3 低 32=`msgh_remote_port`（真正的发送权）、x3 高 32=local、x5 高 32=`rcv_name`、x6=`rcv_size_and_priority`（不是端口）。按正确的字读：43 行里 27 行在陷阱中，**只有 tid 0x40c 一行持有发送权**（`remote=0xcb03`，等 `rcv=0xf007` 的回音，四拍连续都在），其余 `remote` 全是 `MACH_PORT_NULL`——不是「发给谁等回音」，而是**等别人给自己寄信**；0x40c 是全进程唯一一笔在外的请求。r264 = 按内核自己的字重解这一行，并对 `rcv=` 那颗端口读 `ip_messages.imq_msgcount`（队列里几条）与 `ip_srights`（还有几份发送权）——`msgcount=0 且 srights=0` 就是结构性死等：没有任何人再能给它发消息，缺的是发布那颗端口的提供者。**r264 的读数（boot 51）把这一格从「自家队列」扩到「跨进程一笔没人回的请求」**：唯一持有发送权的线程（tid 0x43d）等的是 pid 140 `com.apple.datamigrator` 的回音，`mc=0 sr=1` 且 `dl=0 to=0`——请求已被取走、回音没来、等待不会到期（`sr=1` 别读成「别人还能回」：带 `MACH_SEND_REPLY` 的在途请求，内核自己就持有一份回音端口的发送权）。r265 把同一把尺子挪到债主身上：对那颗端口的接收者任务跑同一份线程普查，并把 `mc=`/`sr=` 也读在 `remote=` 那颗端口上（队列里还排着消息=它没在收信；空=它取走了活儿没回）。**r265 已发（v0.6.41）**：补丁 `zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-clover-r265.patch`（40 个 z，坐 r264 上、最先摘除）加了 `clover_bb_peer_task`（要的是对象不是标签）与 `clover_bb_peer_census`（最多 12 行，`peer=` 用债主自己的 space 解，所以「下一跳」自动出来），普查行多两格 `rmc=`/`rsr=`，tee 新类 `r265-peer` 224 行；它不调用 `clover_bb_sample`（两者共享去重 statics，嵌套会吞第一份的行，checker 按字面禁止）。boot 52 的分岔预先写明：`rmc>0`=消息还排在 datamigrator 队列里没人取，去看它的等待者；**boot 52 落在这一支**：`rmc=0 rsr=1` —— 债主把请求取走了、回得来、但没回。对它的普查给出 8–10 条全睡的线程：**5 条睡在 `_filt_wlwait_continue`（kqueue 的 EVFILT_WORKLOOP 请求）、1 条 `_semaphore_wait_continue`、1 条空等收信、2 条空闲工人**，`tm=0`、`run=0`，而且**没有任何一条持有发送权**——所以「门→datamigrator→keybagd」与「backboardd↔datamigrator 互相等」两条假设同靴双双死掉，它等的是**内核事件**（五把 workloop note + 一把信号量），不是端口。读数缺口是我自己造的：`pth` 行没带 `we=`，而非端口等待只有 `we=` 能分组（五条睡的是同一把还是五把，现在读不出来）。r266 = `pth` 补 `we=`/`pc=` + 给债主那两条睡线程各抓一次用户栈，落点是 `wl_request` 还是 `DMWaitForExecutePluginsSignal`/`DMWatchdogPluginsAfterTimeout` 那一侧——若落在插件等待，桌面这一格的病根就是「迁移插件从没跑起来」，与 GPU 无关。 **r266 已发（v0.6.45，镜像 8ecee77c、内核 f88fba8c）**：债主行补回 `we=`/`pc=`，并按 continuation 去重抓三种不同的睡法各一次用户栈（`clover r266: pustk`，tee 类 r266-stack 64 行）；帧遍历放在解锁之后，checker 按次序钉住。 **r267 不再自己猜属主，改问内核**：`clover r267: own t= pid= tid= hint= ak= owner= ctx=` 对着 `thread->block_hint` 调 `kdp_sema/ulock/workloop_sync_wait_find_owner`（`stackshot_thread_wait_owner_info` 自己就是分派到这三个的，`block_hint.h:68-85` 全 extern）。 **boot 54（r267 实机）把债主那一格从五个收成了一**：pid 138 的 `0x34b/0x34c/0x3ee/0x5eb/0x784` 五行 `hint=10 ak=1` 全报 **同一个 `owner=tid 0x34d` 与同一个 `ctx=0x100f0c840`**（`ctx` 是 `kqwl_dynamicid`，相同=同一只 workloop）——boot 53 那五个不同 `we=` 是每笔同步调用各自的 knote，不是五把锁。而 0x34d 睡在**真正的机器号**上：`cont=_semaphore_wait_continue`、`we=_semaphore_event`（两者都对着本轮 noctf 的 `nm` 逐一验过），`dl=0`。 两处口径同时收紧：`hint=6` 的 `owner=` 是 `create_semaphore` 写的**创建者任务**（`sync_sema.c:128`，注释即「task that owns semaphore」），所以 `owner=pid 138` 只说明「这只机器号是 datamigrator 自己造的」，**不说明谁能叫醒它**；`hint=a` 的 `owner=0` 也分不开「没人抱锁」与「本来就是等值唤醒」，因为 `UL_COMPARE_AND_WAIT*` 恒写 0。另外 `VM_KERNEL_UNSLIDE_OR_PERM` 对非 slide 的内核地址加每靴随机的 `vm_kernel_addrperm`（`mach/vm_param.h:403-407`），所以 `hint=6` 的 `ctx=c00b39f54ef961b` 是本靴内稳定身份、不是可读地址——别当成坏指针。r268 就补这一格：从 `thr->waitq.wq_q` 反查 `struct semaphore`（`sync_sema.h:52-59`）报 `sc=`（count，负=有等待者）、`sp=`（`port->ip_srights` 还剩几份发送权）与接收者，`sp=0` 就是「结构上没人能叫醒它」，与 r264 那格 `sr=0` 同族。 **r268 把机器号本身读出来了（boot 55，v0.6.49）**：`clover r268: sem t= pid= tid= count= srights= otype= receiver=` 从 `thr->waitq.wq_q` 反查 `struct semaphore`（`sync_sema.h:52-59`，用 `offsetof` 不用硬编码偏移）再读端口字段。读数落在三分岔的**中间那一格**：`count=-1 srights=1 otype=0` —— 队列里挂着一个等待者，全系统**只剩一份发送权**，而 `otype=0` 就是 `IOT_PORT`（`ipc_object.h:185`），不是「类型未知」。所以**有人能 signal 而它没做**，既不是结构上叫不醒，也不是读错了时刻。`receiver=-` 是我自己的字段选错：机器号的端口是 `convert_semaphore_to_port` 以 `IPC_KOBJECT_ALLOC_NONE` 造的（`ipc_kobject.h:234`「just make the naked port」），**只有发送权的裸端口根本没有接收方空间**，`ip_receiver` 本就为 0——「换等待对象的种类就要换身份字段」这条我在 boot 52 之后写下、这是第四次照样犯。而且这棵树里早就有正确读法（`task.c:11200 clover_port_recv_task`：`is_read_lock`→`is_active`→`io_lock(obj)`→**在端口锁内**调 `ip_get_receiver`），我的注释还把断言名写成了 `io_lock_held`（真名 `ip_mq_lock_held`，`ipc_port.h:528-532`），checker 里那条「禁止调用形式」的禁令正是给这个错理由加的锁——它挡掉的恰是唯一安全的用法；裸读的风险也被证明是真的：不在空间里的端口那一格存的是 `ip_destination`（另一种对象）。r269 就问这个：拿 `sem->port` 去逐任务扫 `is_table`，报 `srhost port= pid= comm= name=`——名字在 pid 142 自己空间里 = 进程内自锁；在别人空间里 = 普查拿到新债主；哪里都没有 = 那份 srights 是内核捐给端口自己的引用，**叫不醒就从推断变成实证**。 另记两条同轮查实的仪器缺陷：宿主脚本在设备坐 TWRP 时，`an adb device is already up` 捷径会把 `--flash-cache` 整块跳过、读回**上一版 payload 的窗口**还 `exit 0`（我第一次跑 boot 55 拿到的正是 boot 54 的 16 份报告），现在声明了刷写就先 `adb reboot bootloader` 再刷，并把卡上的 boot 计数当硬闸门（刷前先读，读回必须前进，否则 exit 6；boot 55 打出 `the boot count moved: 54 -> 55`）；以及**上一轮我判死的「宿主解不出用户栈」是错的，已撤回**：缓存头里的 region 表是**未滑动**的，板子印的是滑动后的地址，我用 `pc-poff=0x180000000` 这个恒等式当成了「slide=0」的测量值。slide 其实能从卡上唯一钉死——把 r221 `sbm2x` 的八个指令字在子档里做字节搜索，每个命中给一个候选，**非页对齐的候选直接排除**（`.13` 的 `0x2f44ff80` 就这样被否，`.23` 的 `0xd100000` 四条行全同意）。再修掉两处我自己写错的小东西（镜像头按 32 字节打包，不是按页扫；mach-o 的 stab 掩码是 0xE0、`N_SECT` 才是 0x0E，写反会把所有定义符号跳光），`scripts/decode-clover-slide.py` 就把 boot 55 三条栈念出来了：runloop 那条是 `_mach_msg`←CoreFoundation←**`_CFRunLoopRunSpecific`←libxpc**（在 XPC 里等回信）；机器号那条**整条在 libdispatch**（dispatch_semaphore 被 datamigrator 自己调用）；workloop 等待者那条穿过 **DataMigration.framework (+0x1838)** 再进 libxpc——**这条栈真的走在迁移框架里，并卡在一次 XPC 往返上。** **r269 落地**：boot 55 把 r268 的分岔落在中间那一格——`count=-1 srights=1`，也就是说**有人能叫醒它而没叫**。端口只数发送权、不指回握着它的空间，所以这一轮改问内核自己的反查表 `ipc_hash_lookup()`（`osfmk/ipc/ipc_hash.c`：空间 → 名字），而它**只登记纯发送权**（`ipc_hash.c:139` 原话：接收 / send-once / port-set / dead-name 都不入表），于是「命中」就正好等于「这个进程有权利 signal」——我自己扫入口表反而会把接收者念成债主。行是 `clover r269: srhost t= pid= tid= port= hit= name= type= scanned= probed=`，每次普查至多一条（闩锁），逐 pid 1..400 在 `is_read_lock` + SMR 读临界区里查，调用点在**放开 peer 任务锁之后**。r268 那两行同时补 ` port=%llx`；receiver 改在 `io_lock()` 里走 `ip_in_a_space()`/`ip_get_receiver()`，并把 boot 55 那个空格的真正原因写清：`ipc_space_create_special()`（`ipc_init.c:159`）从不给内核专用空间设 `is_task`，**mach 信号量永远没有「接收进程」可报**，所以那格现在写 `kspace` 而不是留空。另外这一轮修了我自己的量具：`scripts/check-clover-xnu-syntax.py` 取代那个只打印前 40 条诊断的临时脚本（`task.c` 有上千条既有告警全排在前面，于是 `proc_rele` 未声明、`proc_find` 作用域外声明、`struct ipc_entry_table` 不完整类型取下标这五个真错误一个也没露出来），它照抄 `compile_commands.json` 的编译参数、错误不截断、退出码来自编译器。 **r269 读数（boot 56）结了这一格**：四次普查一字不差报 `clover r269: srhost … hit=139 name=2d23 type=10000 scanned=77 probed=77`——**全系统唯一那份发送权就攥在债主 pid 139 自己手里**（77 颗活着的进程每颗都查过，`scanned==probed` 自证没停在闸门上），`receiver=kspace` 也证实机器号端口的接收方是 `ipc_space_kernel`（没有 `is_task`，所以永远报不出进程名）。债主内部 `threads=8 wait=8 run=0`：一条等 mach 消息、五条睡在 libdispatch workloop（r267 说它们的属主就是 0x370）、一条 parked 工作队列、0x370 睡在这只机器号上——**环闭合在 datamigrator 进程内**；backboardd 的 `tid=49c` 发往 `remote=cb07`（139 的接收端口）等回信而对侧 `mc=0` 且全员睡着，**所以桌面卡的是一次永无回复的 XPC 往返，不是渲染**（CPU-first 继续成立）。下一刀问「0x370 等的 block 为什么不排出去」——libdispatch 破这种环 normally 靠再造一条工人线程，这里五条工人在睡、一条没新起，r270 先整表比对 sheng 的 `/defaults` 与 boot-args 里的线程类旋钮。 **r270 读数（boot 57）把自己的 IPI 假设证否，指到更基本的洞**：四次拍火全帧 `clover r270: ipi sent=0 recv=0 tick=224585→291290 sgi=0,0,0,0,0,0,0,0 tm=224585,0,0,0,0,0,0,0`——**没有任何一次跨核唤醒被请求过**（不是投递丢了，判定表第 2 格），而逐核 timer 只有 core 0 在走（约 170 Hz），另外七颗核一次都不取时中断。这与 boot 56 的「8 条线程全睡、0 条可运行、block 排出去没人跑」并排读，最像的解释是**调度器以为八核在线、线程被放到不会醒的核上**。而这一格我们仓库自己标过没量：`porting.toml` CPU 区原话「the online-CPU count has not been measured」。r271 只量一件事：每次拍火印逐核 XNU 处理器状态位（started/running/idle/pending），判据预先写死——只有 1 颗 started ⇒ 本板一直是单核；started=8 而 running 只有 1 颗 ⇒ 唤醒路径；都不成 ⇒ 回到 workloop 的 `wl_request` 事件。 **r271 已发（v0.6.58）**：补丁 `zz…z-clover-r271.patch`（46 个 z，坐 r270 上、最先摘除）。上一条里我打算用的四个名字在这棵树里**一个都没有**——`ml_get_active_cpus`/`ml_get_max_cpus`/`struct machine_cpu`/`machine_slot`/`cpu_signal_state` 在 `osfmk/` 全零命中，`MAX_CPUS` 也没有 `#define`（只有 i386 那份 64），它由构建配置带进来。树上真有的是：`cpu_data_internal.h:266` 的 `CpuDataEntries[MAX_CPUS]`（`cpu_data_register()` 每上一颗核填一格，没上过的槽是 `NULL`）、`cpu_flags & StartedState`（`processor_start()` 的路）、`cpu_running`、`cpu_stat.timer_cnt`，加 `cpu_internal.h:73` 的 `real_ncpus`。行是 `clover r271: cpu t= real= max= reg= start= run= s=xxxxxxxx r=xxxxxxxx tm=n,n,n,n,n,n,n,n`，每次普查一条，类 `r271-cpu`（64）。第一版又被自家闸门抓住一条真错：只 include `cpu_data_internal.h` 时 `real_ncpus` 是 `use of undeclared identifier`（`check-clover-xnu-syntax.py` 报 `hard=1` 并点名 `task.c:12834`），补 `arm/cpu_internal.h` 才 `exit=0`——这正是 r269 那一课的规矩在起作用。checker 另外钉住三件事：这一行**不许**去读 r270 已经读过的 `armcpu_` 全局（否则「数中断」会冒充「数处理器」）、它必须坐在 r270 行与逐线程行之间、`bb-cpu` 必须在出厂镜像里（round270-payload.img 报 ABSENT，所以这条针有鉴别力）。boot 58 三种读数各对应一处坏：`reg=1` ⇒ 副核从没上来过、八条线程一直挤一颗核；`reg=8 start=8 run=1` ⇒ 核在、唤醒路径坏；`reg=8 run=8` ⇒ 调度器有核可发，这一格问不出更多，回到 `wl_request`。 **r271 读数（boot 58）把这一格结掉了，而且结在比预想更硬的那格**：三次拍火一字不差 `clover r271: cpu t=1339 real=1 max=8 reg=1 start=0 run=1 s=00000000 r=10000000 tm=223730,0,0,0,0,0,0,0`（t=1479 时 247650、t=1733 时 292260）——本板**一直是单核**：`reg=1` 的语义是硬的（`CpuDataEntries[i].cpu_data_vaddr` 只由那颗核自己上线时 `cpu_data_register()` 写，这棵树没有按 DT 预分配槽位那一步），`real_ncpus=1`，`MAX_CPUS=8` 只是这颗内核的编译期上界，ADT 的八核到今天为止是声明不是在线数。`start=0` 别念成「没核在跑」——启动核自己不走 `processor_start()`，这颗 0 说的是「没有副核被 start 过」，同一行 `run=1`/`r=10000000`/`tm=223730` 才是那颗核活着的正面读数。两台仪器在这一格咬合：内核自己的 `cpu_stat.timer_cnt[0]` 与 r270 驱动侧 `armcpu_core_timer[0]` **同一次拍火同为 223730**，其余七格两边同为 0 ⇒ r270 的 `sent=0` 与这里的 `reg=1` 是同一件事的两面（单核机器不需要跨核唤醒）。**本格病情的结论要说小一点**：债主 pid 146 这次 `threads=8/9 wait=8/9 run=0`，而唯一发送权仍在它自己任务里（`hit=146 name=3903 scanned=78 probed=78`）——**再加七颗核也不会有人替它 signal**，所以 SMP/唤醒这条支线到此为止，桌面那一格回到 workloop 的 `wl_request`（五条工人睡在同一只 kqwl，没人往里放活儿）。SMP 这件事本身仍是移植保真度的洞，只是不是这一格的钥匙。r272 定为**零内核补丁**的一趟：`cpu_start()` 自己会 `kprintf("cpu_start() cpu: %d")`，而 tee 本来就按前缀从内核 msgbuf 挑行，加一条 `cpu_start()` 前缀类就能把「内核从没试着放副核」与「试了、`gPMGR->enableCPUCore()` 这条路在 SDM660 上不干活」分开——后一支才真的要往 PMGR/固件那边动刀。 **r272 已发（v0.6.60），是一趟不碰 XNU 源码的轮次**：`nm` 过本板 unstripped 内核，`AppleARMSMP` 符号**一个都没有**（`iokit/conf/files.arm64:7` 那行写的是 `optional iokitcpp`，这颗内核没开），`PE_cpu_start` 只剩 `IOCPU.cpp:90` 那份，而 `processor_start()` 的调用点全树只有 `AppleARMSMP.cpp:232/515` 与 `IOCPU.cpp:311`；`AppleARMSMP.cpp:499` 的原话「processor_start() never gets called for the boot CPU」正好把上一轮 `start=0 s=00000000` 读准——那是「没有副核被 start 过」，不是「没核在跑」。而本板自己就把 PMGR 的 CPU 电源钩子覆盖成会炸的版本——`drivers/CloverPMGR/CloverPMGR.cpp:104-105` 是 `unimplemented(168/169)`，`unimplemented()` 直接 `panic("CloverPMGR: unimplemented power operation %u")`，boot 58 跑满 1800 s 没有这条 ⇒ 钩子从没被叫过（这是「从缺席推出」；tee 那句 `cpu_start()` 才是对调用者的直接测量）。所以这一轮不加探针，只借内核自己那句话：`cpu_start()`（`cpu.c:909`）本来就 `kprintf`，`processor_should_kprintf`（`processor.c:1081`）只挡 CLPC 上下线——CloverSdhci 加白名单 token `"cpu_start()"` 与类 `r272-cpustart`（16）就够了。**这轮踩得到一个真坑，checker 现在钉着**：`teeKernelLog` 先跑白名单、再轮类表与配额，只加类不加 token 的那条行进不了环，读数会假成「内核没试过」；所以 Round272 块既断言 token 必须落在 `struct TeeProbeClass` 之前，也反向断言 `patches/xnu-clover/` 里不许出现 47 个 z 的 r272 补丁、`LATE_PATCH` 头仍是 r271。boot 59 三格：一条都没有 ⇒「从没试着放副核」坐实，缺口是 sheng 有而我们没有的 `zzyf-sheng-amp.patch`（`__ARM_AMP__`/`MAX_CPUS 8`/`MAX_CPU_CLUSTERS 2`）那一整块，不是 boot-arg；有几条无 panic ⇒ 调了 `cpu_start()` 却没走到我们那个会炸的钩子，路断在 `PE_cpu_start` 的 `IOCPU *` 转换；有几条并跟着 `unimplemented power operation 168` ⇒ 才谈得上把 `enableCPUCore` 做成 PSCI/寄存器操作。 **r272 读数（boot 59）没给出判决，而且缺的不是接线**：`cpu_start()` 0 条，但这一窗实测只覆盖 `t=1273…1814`（512 行 = 541 s），放核那句话只在 t<10 s 出声，落卡时早被挤出——**「仪器问不出它看得见的时刻」这一族第六次**，这次的限制是环的时间地平线，不是配额也不是掉字段。更该记的是**这轮连正对照都没有**：窗里那两条别人的行各走自己的通道（`CloverSdhci.cpp:3788` 的 nag token；另一条只是整行含 `apfs` 子串，`teeTokensMatch` 在行内任意位置找 token），不能证明 `cpu_start()` token 通不通——**测不出「没有」的仪器不能用来宣布「没有」**。所以「没试着放副核」目前仍靠 (a) 镜像里没有 `AppleARMSMP`（`processor_start()` 只剩 `IOCPU.cpp:311` 一个调用者）＋(b) 本板 `CloverPMGR.cpp:104-112` 的钩子是 panic 版而 boot 58/59 都没炸，(b) 是从缺席推出。真要量就得在 `cpu_start()` 现场留计数让普查晚点印（形状与自带对照的规矩已写进流水日志，是保真度那一格的账）；`r271`/`r270` 两行在 boot 59 第三次逐值复现单核与 `sent=0`。三格都不改桌面结论：债主仍 `threads=8/9 wait=8/9 run=0` 且唯一发送权在它自己任务里。 **r273 已发（v0.6.62，镜像 b35a50b9）**：把 workloop 读整个对象而不是塌成 `owner=` 的那一个数——`clover r273: wl t= pid= tid= kn= kq= wlo= ks= kc= kqpid= dyn= tr= tc= tt= ow=`，走内核自己的 `kn=(struct knote*)event`→`knote_get_kq()`→`KQ_WORKLOOP`→`kqwl_request` 那条路。**这一轮第一次编译被我自己的语法门打回**：osfmk 里 `#include <sys/eventvar.h>` 命中的是 EXPORT_HDRS 的导出副本，`workq_threadreq_s` 与 `WORKQ_TR_STATE_*` 都被剥掉了（十条 unknown type name + 一条 undeclared identifier），所以读数落在 `bsd/kern/kern_event.c:2661`、`task.c` 只传等待字取回整数数组——和它已经在调的 `kdp_*_find_owner` 同一条通道；两侧索引表由 checker 逐项钉住。peer 行同时补 `wl=`（这次有几个 workloop 等待者）当正对照——「没有 r273 行」只有配上 `wl=0` 才读得出「没人等在 loop 上」，这是把 r272 那条教训事前用上。**另记一条自我更正（v0.6.63）**：我原先还把「boot 60 里 `4f2/88a` 漂到 `hint=2`」写进过标题与文档，随后发现 flights 目录里只剩 boot 59 的产物、boot 60 那份文件已经不在，于是不管它当时是什么，一律撤回——**引用一次拍火就要引用还留在盘上的那个文件**。三分岔：`tr=0 ow=0` ⇒ 内核从没要过工人，回去找该往这只 loop 投活儿的那一侧；`tr=4/5` 且 `tt=` 是同一趟里也在睡的那条 ⇒ loop 内互等，切 `tt` 自己的等待；`tr=1/2` 且 `tt=0` ⇒ 要了不给，刀口在 `bsd/pthread/pthread_workqueue.c` 的加线程门槛。
| 2 | 触摸没有任何应答 | QUP v2 I2C-3 电气是通的（两个候选地址都 NACK：`done xfers=4 nack=4 timeout=0`），`rails unproven` | 先把面板轨真的上电（依赖 3），再问地址  r259 不等轨先把地址再问一遍（`clover-input=3` 回膛），理由是同一趟正在发合成手指，需要知道总线上到底有没有真坐标 || 3 | PMIC 轨读不出来 | SPMI 仲裁器读通路是好的（`obs00=00000100`），但核心版本字不在 v5 带内，`apid=-1` 一个都没认，`ldo-a11`/`ldo-b3` 未读 | 先弄清那个版本字，再认 APID、读轨、开轨  **boot 47 的教训不是「面板没答」，是「答案回不来」**：仪器跑了，42 行说在 t≈12 s，而环这次从 t=1194.04 起——r260 不再插第二次心跳，改抄 sheng 的机制：`drivers/CloverInput/CloverInput.cpp:241` 的 `clover_input_observation[16]`（自 r181 就为「不依赖日志环」而存在）由 beat 每拍重述成一条 `clover r260: bus mode=.. … verdict=..`，总线仍一靴只碰一次；sheng 的驱动根本不 print（`TouchProbe.cpp:29` 的行数组 + `BootLog.c:340-356` 每 2 秒 compose），所以它的早期读数活得下来，因为那是状态。`mode=0` 也打印——没跑过必须说出来。 **boot 48（r260 实机）把答案带回来了，而且每拍都有**：
`bus mode=3 … xfers=4 timeout=0 nack=4 verdict=102` 与 r183 一字不差——电气与协议栈都活着，
两个候选地址全 NACK。触摸这一格从「问不到」变成「已知不应答」，下一格在电源轨（3），不在 I2C 层。|
| 4 | ADSP 没起 | SMEM 共享堆读通（`$TOC`/`$PRT` 全部自校验通过，`part(3) hosts=0:1` 是候选边），判决行还没上屏 | 有边就上 GLINK/APR；没镜像则先压 `vadsp` 电源 |
| 5 | watchdogd 每次起就崩 | 自身可执行文件里 `brk #1`（`IOWatchdog` 服务在板上不存在）。r193 的 `panic_on_crash_disabled=1` 已把它从「致命」摘掉，所以它只是噪声 | 写一个最小 `IOWatchdog` 服务，或让 plist 不再要求它 |
| 6 | **GPU/Metal 一个都没有**（Adreno 512 无驱动、无固件、无 renderer） | `porting.toml` 的 `gpu` 区 `status = "planned"`、`limits = ["No GPU driver, firmware or renderer is present."]`；r261 的取证把 boot 48 那只死等的组的等待线程解进 `libVFXCore.dylib`/`VisualLogger.framework`（渲染/特效侧的框架），而 sheng 到桌面靠的正是它 `gpu` 区那一整套 Adreno 组件（`Adreno740`+`AdrenoMetal`+`AirNir`+`AdrenoPM4`+`AdrenoFirmware`+`ArmSmmu4K`；「空闲桌面每 250 ms 提交 6 个命令缓冲」「render server 预热 162 个 Metal 库」） | 先分岔，别急着写驱动（r262）。**用户 2026-11-02 定：不先写 Adreno 驱动，前期用 CPU 硬顶出桌面**——sheng 自己就把「发布一个 Metal 能打开的设备壳」和「CP/µcode 真提交」分成两段，CPU 路线借的是这个分段，不是它没有的东西（它没有任何软光栅：AirNir 是 turnip/Vulkan 桥，IR3 是 a6xx+ 指令集，a5xx 用不了）：等的是渲染完成（GPU 区），还是 backboardd 自家另一条卡住的队列。若落在 GPU，按它自己 `next_action` 那句先评 Adreno 512 能复用 sheng 的哪几块 **r262 的普查把这一格推进了一步但没有关掉**：pid 58 没有线程睡在内核/驱动路径上（所以「等一个内核侧完成」不成立），六个空闲 `workq` 工人在等活（所以「没有线程可跑那笔 leave」也不成立），剩下的形状是「那笔活儿从来没被排进来」——而它的来源要么是渲染侧的完成通知（本板没有 GPU/Metal 可用），要么是等一个永远不来的回音；`_ipc_mqueue_receive_continue` 这一层还分不清这两者，r263 的 `dl=` + port owner 就是为分辨它们而加的 **boot 50 对这一格的结果**：GPU 分支既没被证伪也没被支持——这一靴没有任何定时等待，也没有任何线程处在「已发出请求等回音」的形状（发送权全为 0），所以「等渲染完成通知」在这只进程里拿不到一条读数支持；下一步是 r264 的端口可读性（谁还能给它寄信），不是先写软光栅。 **r264 期间的镜像侧复查改了这条等待的名字**：它等的不是「渲染完成」，是**一张贴图加载完成**——`asyncGroupRequests`/`waitAsyncGroup`/`groupID`/`AsyncGroupRequestState` 全在 `dyld_shared_cache_arm64.07` 的 `_TtC7VFXCore12TextureCache` 与 `VFXCore/TextureCache.swift` 邻域里（配 `Can't wait cache entry in non loading state` 这类状态报错），原先我说的「async Metal library/shader-compile」是两处误读（`_renderPipelineStateForFramebuffer**Descriptor**:device:` 是管线状态缓存 ivar；`failed to create Metal library` 属于 ImGui 调试叠加层）。同时把「CPU 顶桌面」的边界钉死：窗口服务器没有软合成分支（`CoreAnimation: Software renderer! Simulating crash!` 就挨着 `Failed to initialize Metal or OpenGLES contexts!`），真有的软路线只有 CoreImage 的 `NUSoftwareRenderer initWithCIContext:priority:`；`swiftshader` 只是 libANGLE 里的 EGL 扩展名，不是设备。VisualLogger 这条杠杆**先被我判死、随即撤回**：开关实名是 `enableRecording`（VFXCore ECS Scheduler 的录制字段；`.31` 里那个 `enableRecord` 命中是别家框架的 `enableRecordButton:`），贴图组邻域里确实没有录制门槛，但 r261 把等待线程十帧解到底、**最底两帧就在 VisualLogger.framework**（上面才是 libVFXCore），也就是**记录器把 VFXCore 拉进这条触控路径的**——所以「关 VL」仍是 CPU-first 最便宜的一档，只是「谁打开 VL」还没找到（`.31` 里 `com.apple.visuallogger`/`VisualLoggerAgent`/`CFPreferences` 全 0，`MobileGestalt` 63 处）。 于是设备壳的最小要求变窄：只要能回答贴图加载那一串调用（设备枚举 + `newTextureWithDescriptor:` 与它返回的对象），不需要会画。 **boot 51 把这一格往后放了**：r264 点名成功——唯一那笔「已发出、等回音、永不到期」的请求，属主是 **pid 140 = `com.apple.datamigrator`**（`remote=6b0f`、`rcv=c903`、`mc=0`，四拍同形；镜像里唯一对得上号的是 `DataMigration.framework/XPCServices/com.apple.datamigrator.xpc`，523 个 LaunchDaemons 里没有 `datam*` 标签 ⇒ 按需拉起的 XPC 服务）。所以当前挡桌面的第一嫌疑人既不是渲染完成、也不是触摸传输，而是**一次没人回音的 XPC 往返**，设备壳仍排在它后面。 |

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
