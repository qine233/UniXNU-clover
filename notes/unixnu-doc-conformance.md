# UniXNU 文档 vs clover 的驱动 —— 逐条对照（Round314 底稿）

用途只有一件：把「驱动要照 UniXNU 文档做」这句话变成一组带行号的条款，和逐颗驱动对完之后的判定。
过去这条规矩没有落地：本仓 11 颗驱动里，只有 `drivers/CloverSdhci` 在源码和 README 里各引用了一次文档
（源码里是 `WHY IT RUNS FROM sheng_pmgr_run()` 那一段，README 里是同名小节；按 §8 的规矩不写它行号），
其余 10 颗写的时候没有把文档条款过一遍。本轮读尽
`~/UniXNU/docs` 里对设备仓驱动成立的部分，逐颗对照，并且把对照结论做成闸门。

## 1 取证办法

文档正文在 `/Users/zhouzhou/UniXNU/docs`（VitePress 树，`docs/zh/` 是同一套的中文版）。本轮读的是
`docs/drivers/overview.md`、`docs/drivers/xnu-drivers.md`、`docs/drivers/device-tree-matching.md`、
`docs/drivers/kexts.md`、`docs/drivers/native-novatek-touch.md`（范例）、`docs/drivers/uefi-drivers.md`、
`docs/porting/device-repository.md`、`docs/guide/build-kernel.md`。本文所有 `:NN` 行号都取自盘上当前文件，
可以逐条重放。判定所用的每一条实测命令和它的输出也都写在对应小节里。

## 2 文档对「设备仓里的 XNU 驱动」提出的八条

一、层级选择（`overview.md:11-16`）：内核内的平台、CPU、中断、存储、显示、GPU 走 built-in XNU driver。
clover 的板级驱动全在这一层，选对了。

二、源码布局（`xnu-drivers.md:7-14`）：`MyDriver.cpp` + `MyDriver.h` + `Info.plist`，其中 Info.plist 是
personality 的**参考记录**——内核真正用的是编译进 `iokit/KernelConfigTables.cpp` 的那一份。

三、类形状（`xnu-drivers.md:19-64`，运行期阶段表 `:219-228`）：IOService 子类，`probe()/start()/stop()`
逐级检查 `super::`，从 provider 读 `reg`，成功之后才 `registerService()`。

四、按内核 ABI 编译（`xnu-drivers.md:71-116`）：回放 `.cpo.json` 记录，产物必须命中五字段——
magic `0xfeedfacf`、CPU type `0x0100000c`、file type `1`(MH_OBJECT)、subtype `0`、
build version platform `2`(iOS) 且 minimum `0x110000`。

五、归档接线（`xnu-drivers.md:118-124`、`device-repository.md:90-113`、`build-kernel.md:131-134`）：
builder 脚本进 `[kernel] archives`，`CONFIG_XNU_<NAME>=y` 进 defconfig，归档进
`KERNEL_EXTRA_ARCHIVES` 与 `KERNEL_ARCHIVE_DEPENDENCIES`。

六、链接方式（`xnu-drivers.md:128-145`）：`-Wl,-force_load`，理由写在 `:139`——「force_load 保留每个目标
文件，包括链接器会丢弃的类」。

七、personality（`xnu-drivers.md:147-168`、`overview.md:40`、`build-kernel.md:135`）：通过补丁加进
`KernelConfigTables.cpp`，构建脚本再以 `KernelConfigTables.cpo_CFLAGS_ADD=-DXNU_<NAME>` 把宏递进去；
匹配键的含义与 probe 顺序见 `device-tree-matching.md:7-18` 与 `:151-159`。

八、测试五档（`xnu-drivers.md:230-239`）：编译、对象 ABI、宿主逻辑、内核启动、硬件。范例给的六条落地规矩
见 `native-novatek-touch.md:130-137`：纯逻辑与总线分离、常量全部带出处、纯函数先有宿主测试、分级
boot arg、序号计数器发布快照、默认关闭直到日志证明每一级。

## 3 逐颗判定

下表是**审计当时**（本轮动手之前）的判定；本轮补了什么、还欠什么，记在 §7。表里指向我仓可变
`.cpp` 的一格用符号锚点而不是行号，理由见 §8。

| 驱动 | 入口 | IOKit 类 | personality | README | 文档引用 | 宿主模型 | builder 是否校验第四档 | 判定 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| CloverSdhci | `clover_emmc_run()` | 无 | 无 | 有 | **有** | 无 | 有 | 已记录的偏差；本轮改正其中一句过时论断 |
| CloverBlock | `clover_block_run()` | 无（用现成类） | 无 | **缺** | **缺** | 无 | 有 | 补 README、补引用；链接在场由闸门代校验 |
| CloverInput | `clover_input_run()` | 无 | 无 | **缺** | **缺** | 无 | 有 | 补 README、补引用；范例第 1/3 条是真缺口 |
| CloverPower | `clover_power_run()` | 无 | 无 | 有 | **缺** | 有 | 有 | 补引用 |
| CloverDsp | `clover_dsp_run()` | 无 | 无 | 有 | **缺** | 有 | 有 | 补引用 |
| CloverGpu | `clover_gpu_run()` | 无 | 无 | **缺** | **缺** | 无 | 有 | 补 README、补引用 |
| CloverPMGR | `sheng_pmgr_run()` 本体 | 有（`class CloverPlatformPower : public ApplePMGR`） | 无 | 有 | **缺** | 无 | 有 | 补引用，链接在场由闸门代校验 |
| ArmCpu（覆写基仓 submodule） | `ArmCpu::start()` | 有 | 基仓的 patch | 有 | **缺**（本轮补） | — | 有 | **正对照**：覆写基仓模块时该带的四件套（cpp/Info.plist/README/LICENSE）它齐 |
| NativeHidInput（r255 拷入） | `IOHIDDevice` 子类 | 有 | 无 | **缺** | 无 | — | 有 | 基仓有 README+LICENSE，拷进来时掉了；本轮补回 |
| NativeKeyStoreEvents / NativeMobileDisplay | `AppleKeyStore::start()` / `NativeMobileDisplay::start()` | 有 | 无 | 有 | **缺**（本轮补） | — | 有 | 基仓自身也没有 Info.plist（实测 `ls /Users/zhouzhou/UniXNU/drivers/<name>`），无缺口 |

最后一列本轮重测过一次，并且**推翻了 §7 原来那条**：三颗看着像没有第四档校验的 builder 其实都有，
只是位置不同。`build-clover-block.py` 与 `build-clover-pmgr.py` 自己走
`require(info['kind'] == 1)` ＋ `require(build_version == [(2, 0x110000)])`，magic/CPU/subtype 三枚
由共用的 `kits/NativePrelink/native_prelink.py:14-19`（`macho()`）在解析头时一并拒掉；
`build-clover-gop-legacy-display.py` 则把三颗对象（`NativeLegacyDisplay`、`NativeKeyStoreEvents`、
`NativeGOPPresenter`）分别交给 `build-arm-driver.py` 编（`:136`、`:306`、`:322`），那张五字段表就在那
条路里。真正没人校验的是**链接之后**：没有哪个 builder 看得到自己的 `.o` 最后有没有进
`kernel.macho`——这正是 §4 那枚产物层闸门补的位置。

「Info.plist 只对 personality 存在」不是推测，是基仓自己的做法：`ArmCpu`、`NativeAdreno` 有，
`NativeHidInput`、`NativeKeyStoreEvents`、`NativeMobileDisplay` 没有。所以本轮**没有**给程序化启动的类
伪造 plist，而是把这条判据写进闸门：声明了 personality 的地方才必须有参考 plist。

## 4 实测：没有 force_load，今天丢没丢东西

现状：`configs/clover_defconfig:70` 把六枚归档交给 `CONFIG_XNU_EXTRA_LD_ARCHIVES`，基仓
`scripts/build-xnu-4k-baseline.sh:1002` 是这样接的——

```
KERNEL_EXTRA_ARCHIVES="$KERNEL_EXTRA_ARCHIVES ${XNU_EXTRA_LD_ARCHIVES:-}"
```

裸追加，没有 `-Wl,-force_load`，与文档 `:134` 的形状不同。这句话本身不构成结论，所以去量它有没有后果：
对每枚归档的每个成员 `.o`，取其导出定义符号，逐个在链好的 `out/clover/kernel/kernel.macho`
（69,296 个符号）里找。实测九枚成员——

```
libCloverSdhci.a    CloverSdhci.o              abi=ok defined=22 not_in_kernel=0
libCloverBlock.a    CloverBlockDevice.late.o   abi=ok defined=46 not_in_kernel=0
libCloverBlock.a    base-bindings.o            abi=ok defined= 2 not_in_kernel=2
libCloverInput.a    CloverInput.o              abi=ok defined= 2 not_in_kernel=0
libCloverPower.a    CloverPower.o              abi=ok defined= 2 not_in_kernel=0
libCloverDsp.a      CloverDsp.o                abi=ok defined= 2 not_in_kernel=0
libCloverGpu.a      CloverGpu.o                abi=ok defined= 1 not_in_kernel=0
libNativeGOPPresenter.a / libNativeHidInput.a  abi=ok not_in_kernel=0
```

两枚没跟上的符号是 `IOService` 的构造/析构 vague-linkage 绑定，而内核本来就有 IOKit 自己的同名定义
（`nm` 里 `__ZN9IOServiceC2EPK11OSMetaClass` 在）。也就是说：**今天没有丢任何属于我们的代码**，但这是偶然——
不带 force_load 时「谁的符号没被引用就丢」由链接器决定。文档 `:139` 那句保护是给 personality 启动、
没人显式调用的类准备的；我们的驱动全都靠显式调用链到达（`CloverPMGR` 串起
`clover_unattended_begin → clover_emmc_run → clover_input_run → clover_power_run → clover_dsp_run`），
所以暂时不在缺的那层保护底下。

因此闸门不抄文档那句话，它抄可证伪的等价检查：每枚成员 `.o` 必须命中第四档五字段；每颗驱动自己的入口
符号（`clover_*_run`）必须出现在链好的内核里。构建产物不在时是 notice，不许静默通过。

## 5 改正一句过时论断

`CloverSdhci.cpp` 的「WHY IT RUNS FROM sheng_pmgr_run()」一段（同文在
`drivers/CloverSdhci/README.md` 的「Why it runs from `sheng_pmgr_run()`」一节，里面引的
`docs/guide/build-kernel.md:135` 就是本段说的那条），过去写的是：设备仓加不了 `patches/xnu` 下的补丁，所以拿不到 personality。实测两处
把它推翻了：

* 基仓 `~/UniXNU/patches/xnu` 里 **73 枚 clover 补丁是 git 跟踪的**（`git ls-files patches/xnu | grep -c clover`
  = 73，`git status --short patches/xnu` 干净）。
* 组装树里 `out/clover/tree/patches/xnu` 符号链接到基仓 `patches/xnu`，
  另有 `out/clover/tree/patches/xnu-clover` 符号链接到本仓 `patches/xnu-clover`。

成立的说法是：**设备仓没有补丁槽位，补丁要提交进基仓的 `patches/xnu` 才会被应用，本仓留一份供评审**——
不是「加不了」。真正让我们还没有 personality 的是另外三条，本轮把它们写成前提：

1. provider 得先存在。`scripts/make-clover-adt.py:316-321` 的 `/arm-io` 只有一个 `intc` 孩子，
   spmi／sdhci／gpu／i2c／dsp 在本板 ADT 里没有节点，于是 `IOPlatformDevice` + `IONameMatch`
   谁都匹配不到（`device-tree-matching.md:20-28`、`:151-159`）。
2. 顺序表达不了。仪器要钉在开机链的固定位置上（Dsp 必须在块设备驱动把环备好之后），而
   `IOProbeScore` 只排候选，不表达「谁先把环备好」（`porting.toml` power.configuration）。
3. 没有 force_load（§4），personality 启动而没人引用的类会被链接器丢掉。

分歧本身站得住，写下的理由站不住——理由错了会让下一轮以为这条路被上游堵死，而它其实只欠三件具体的活。

本轮还量到一条能让这三件前提显得更清楚的事实：**「设备仓里的驱动拿不到文档化的类形状」也是错的**。
`drivers/NativeHidInput` 就用的是 `OSDeclareDefaultStructors` + `OSDefineMetaClassAndStructors` 那对标准宏
（`xnu-drivers.md:19-64` 的原样），之所以能过链接，靠的是 `scripts/build-clover-hid-input.py` 从原始
kernelcache 里把 `IOHIDDevice` 的真实 ABI 抄出来——生成 `OriginalHidDeviceABI.h`（私有存储按原实例尺寸
`static_assert`，168..206 槽声明为不可调用），再把 `gMetaClass`、vtable、构造/析构 thunk 四枚符号
用生成的 `original-bindings.S` 钉到读出来的地址上，并且要求驱动恰好覆盖审计点名的十槽。所以第 1、3 条
前提缺的不是「宏能不能用」这一环：类形状在宿主侧已被证明可达，缺的仍然只有 ADT 里的 provider 节点与
force_load 那两处。两颗要动开机顺序的活因此可以把风险写小一点，但**仍然要上机**——这一条抄的是符号地址，
地址属于本板那枚镜像，换镜像就得重跑审计。

顺带记一句，免得下一轮把对的东西「改正」掉：`board.toml:1020-1021` 与 `:1593-1594` 说的是同一件事的**准确**
版本——「`tree/patches/xnu` 是指向基仓的符号链接，设备仓里叫 `patches/xnu` 的目录会整片盖掉上游补丁而不是
接上去」。那句话没错，错只错在 `CloverSdhci` 把它推成了「所以加不了 personality」。

## 6 与范例六条对照（`native-novatek-touch.md:130-137`）

第一条（纯逻辑与总线分离）与第三条（纯函数先有宿主测试）：`CloverDsp`（`DspCore.h`）和 `CloverPower`
（`PowerArb.h`）做到了，而且各有宿主模型并被 `scripts/test-clover-dsp.py`、`scripts/test-clover-power.py`
当构建闸门；`CloverInput`、`CloverGpu`、`CloverBlock` 三颗既没分离也没有宿主模型——这是本文件里最重的一处
真缺口，等板子回来有读数打底之后再动。**r316 更新**：那一格不再等板子。`CloverBlock` 交出第三颗
（`drivers/CloverBlock/BlockCore.h` + `tests/clover_block_host.cpp` + `scripts/test-clover-block.py`），
剩下 `CloverInput`／`CloverGpu` 两颗。**r317 更新**：`CloverGpu` 交出第四颗
（`drivers/CloverGpu/GpuCore.h` + `tests/clover_gpu_host.cpp` + `scripts/test-clover-gpu.py`），只剩
`CloverInput` 一颗。

第二条（常量全部带出处）六颗都做到，且地址不止带出处还被钉在设备自己的 DT 上（`check-clover.py` 的
`check_device_tree_constants()`，38 枚地址对 230 个节点；`GpuRegs.h` 于 r311 收编）。第四条（分级 boot arg）
六颗全做到。第五条（序号计数器发布快照）`CloverSdhci` 的判据环/probe 环做到（单游标消费，r304），
`CloverInput` 的触摸出数走的是 r255 的 IOHIDDevice 报告路径而不是 seq 快照，记录为形状差异而非缺陷。
第六条（默认关闭直到日志证明每一级）：本轮实测 `board.toml` 的 kernel_args 里在的是
`clover-input=3`（`:2422`，r259 把它接回来的，检查器在 `# Round259's resumption, pinned` 那一段钉着这颗
参数不许掉——r316 按 §8 把它原先的 `check-clover.py:704-709` 换成锚点，那个行号在本轮之后就不在了）、
`clover-gpu=1`（`:2441`）、`clover-power=1`（`:2489`），`"clover-dsp=3"` 仍是注释状态（`:2534`）——
所以这一条不是「所有仪器都关着」，而是「每一颗的档位都停在只读或只映射的那一级，DSP 那台因为会读堆而整颗摘掉」。

## 7 本轮改动与未结缺口

本轮改的（全部宿主侧，未刷写）：三颗缺 README 的驱动补 README（`CloverBlock`、`CloverGpu`、
`CloverInput`）；`NativeHidInput` 从基仓补回 README 与 LICENSE，并按 `NativeMobileDisplay` 的
「board-local copy」写法把差异与 sha256 写进去；七颗驱动的源码头各加一段 DOCUMENTED CONTRACT，
引用它们遵循的 `docs/*.md:行号`；`CloverSdhci.cpp` 与它的 README 里那句过时论断按 §5 改正——
`board.toml` 那两处**没有改**，因为它们说的是准确的窄版本（见 §5 末段）；本轮又按同一条规矩改了
`CloverSdhci/README.md` 与 `NativeHidInput/README.md` 的四处不实：链接方式写成 `-Wl,-force_load`
（实际是 `build-xnu-4k-baseline.sh:1002` 原样追加）、shipped 参数写成 `clover-emmc=1`（`board.toml:1968`
是 21）、看门狗写成 `armcpu-clock-panic=60000`（本板 kernel_args 里 armcpu-* 只有 timer-intid、smp、
self-ipi 三枚，护住挂死的是 `clover-unattended=1800`，`:2597`）、以及把 CMD13／首块读／日志分区三段
写成「以后要做」——它们早在 round88、round96、round103 就做完了。
`check-clover.py` 新增两条闸门，都插在 `check_tree()` 之后、`main()` 之前，因此本轮被引用的那些
`check-clover.py:行号` 一个都没动：

* `check_driver_documentation()`（§2 的第二、三、八档 + §8 的引用规矩，纯结构、不需要内核）：
  有源码的 `drivers/*` 必须有 README；README 必须有一段点名「照哪份文档做」的章节并且**至少引一枚
  `docs/**.md:行号`**；每一枚引用都要落得下（基仓 `~/UniXNU/docs` 或本仓，且那一行真的在文件里）；
  没有 `Info.plist` 的驱动必须在 README 里说出为什么（`:14` 把 plist 当 personality 的记录，沉默会被
  读成漏了），有 plist 的必须被基仓 `patches/` 里的某枚补丁点名；以及拒绝「本仓 `.cpp`:数字」这种形状
  ——判定用的是「该 basename 在本仓存在」，所以 `ConfigurationSettings.cpp:235`、
  `iokit/Kernel/IOService.cpp:6582` 这类厂商引用照旧放行（用三段样例文本单独验过这两枚正则：带路径的
  厂商引用不匹配、本仓 basename 匹配、裸 `:147-168` 续引不匹配）。
* `check_driver_products()`（§4 那一格，产物层）：按 `!<arch>` 固定宽度头自己走 12 枚归档的成员
  （BSD 的 `#1/len` 长名与 `__.SYMDEF` 都处理了），逐枚对象核 `xnu-drivers.md:106-116` 那五个字段，
  再到 `out/clover/kernel/kernel.macho` 里验每颗驱动的导出符号在不在。负向测过：CPU type 改成
  `0x1000007`、filetype 改成 2、符号名换成不存在的，三种都会报错。产物不在就是 notice 加一句「跑
  `out/clover/tree` 来查」，**不会静默通过**——这一格要抓的正是没 force_load 时「归档安静消失」。

本轮实跑两遍：仓库里跑 = `drivers documented: 11` + 12 条 notice；组装树里跑 =
`drivers documented: 11; 12 archives, 22 objects ABI-checked, kernel read`。

未结，按顺序：

1. 第一颗真走 personality 的驱动需要 §5 三条前提各做一件（ADT 发布对应节点、想清楚顺序怎么表达、
   归档补 force_load）。这件事要改开机顺序，必须上机验证，排在桌面这一格之后。
2. ~~`build-clover-block.py`、`build-clover-pmgr.py`、`build-clover-gop-legacy-display.py` 三处没有第四档
   校验。~~ **本轮复查时撤回**：三处都有，只是位置不同（见 §3 末段——前两处自己
   `require(kind == 1)`＋`require(build_version == [(2, 0x110000)])`，第三处把三颗对象交给
   `build-arm-driver.py` 编）。这条撤回之后，真正没人覆盖的一格仍然在，但它不是「对象长什么样」而是
   「对象进没进最终内核」——那就是 §4 的产物闸门补的位置，也是本轮唯一新增的那一格。
3. ~~`CloverInput`／`CloverGpu`／`CloverBlock` 的纯层分离与宿主模型（范例第 1、3 条）。~~
   **r316 结掉三分之一**：`CloverBlock` 的清单走查与请求算术进了 `BlockCore.h`，15 组宿主模型由
   `scripts/test-clover-block.py` 编译并**运行**（`check-clover.py` 的 `# Round316.` 那一段既要求驱动
   真的调用它，也要求 `.cpp` 里不再留着第二份拷贝）。
   **r317 结掉三分之二**：`CloverGpu` 的每一枚「从字里读出一个判断」进了 `GpuCore.h`（LDO 普查、
   PWRSWITCH_CTRL／VREF／STATUS 解码、GDSC 写序列、分支 request/halt、AHB 门、chipid 门、L1 角选择），
   13 组宿主模型由 `scripts/test-clover-gpu.py` 编译并**运行**，同一面三式（存在／被调用／无第二副本）
   由 `check-clover.py` 的 `# Round317.` 那一段把住。剩 `CloverInput` 一颗。
4. `docs/drivers/uefi-drivers.md:7-13` 那一层的对照（`.inf` / DSC / FDF / `args` 载入选项）在固件仓
   `uefi-clover`，本轮未动，同一套办法搬过去即可。

r316 还改了这个闸门本身一处。组装树把基仓的驱动 symlink 进 `drivers/*`，所以从
`out/clover/tree` 跑的时候，`check_driver_documentation()` 开始判**本仓并不拥有**的代码——
它给 `drivers/NativeVirtioConsole` 报了三条错（没有点名文档的章节、没有 `docs/**.md:行号`、
没说为什么没有 plist），而那一颗驱动来自 `/Users/zhouzhou/UniXNU/drivers/`。现在按路径判定：
`base_repo_root()` 找到基仓检出（且它不是当前仓库）后，realpath 落在基仓里的 `drivers/*` 一律豁免，
并在 stderr 打印豁免了哪几颗、几颗（沿用 `check_tree()` 早已给 `drivers/DarwinBoot` 的那条 notice 的
规矩，而不是写一张名单——名单会在下一颗基仓驱动进来时静默失准）。实跑两侧都过：仓库里
`drivers documented: 11, base-repo drivers exempt: 0`，组装树里 `documented: 11, exempt: 11` 且
`12 archives, 22 objects ABI-checked, kernel read`。豁免是按「谁 author 这颗」切的，不是按名字，
所以 11 颗本仓驱动在树里照旧全部被判定。

## 8 引用规矩（本轮定的，因为本轮就踩到了）

本轮给七颗驱动的头部各插了 11–14 行注释，`CloverSdhci.cpp` 那段改写净增 20 行——于是**每一个指向本仓
`.cpp` 的行号都在同一轮里失效了一次**。这规矩就是从这次里长出来的：

* 引用 UniXNU 文档（`~/UniXNU/docs/**`）、引用冻结在仓里的厂商源（`notes/sources/sdm660/*.c` 的
  `VEND:`/`MAIN:`）、引用本轮不改的文件（`board.toml`、`configs/clover_defconfig`、头文件、
  `scripts/*.py`）：用行号，因为它们本轮不动，而且行号能表达「那一段」。
* 引用**本仓会被后续轮次编辑的 `.cpp`**：用符号锚点（函数名、常量名、那段注释的小标题），不用行号。
  一个会被编辑的文件里的 `:2279` 在下一轮就是 `:2290`，而写错行号的引用比没有引用更坏——它会让人去
  核对一段根本不存在的话。

按这条规矩，本轮三份新 README 里指向 `CloverBlock.cpp`／`CloverGpu.cpp`／`CloverInput.cpp`／
`CloverSdhci.cpp` 的位置全部改成符号锚点；`InputRegs.h`、`GpuRegs.h`、`PowerArb.h` 这类本轮未动的头文件
保留行号，等下一次编辑它们的那一轮再按本条转换。这条本身也在闸门里：`check-clover.py` 会拒绝驱动 README
出现「本仓 `.cpp`:数字」这种形状（见 §7 第一段的第二条闸门）。
