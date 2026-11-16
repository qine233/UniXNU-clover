# UniXNU-clover

小米平板 4 8 英寸 LTE（2018，代号 `clover`）的 XNU 移植：4 GB / SDM660（Kryo 260，ARMv8.0，无指针认证），
跑原封不动的 **iPad7,1 / iPadOS 17.7.11 / 21H461** 4 KiB 页镜像。配套固件检出 `mu-clover`。

过程记录不放在这里：逐轮的取证、推翻与判读在 [`notes/development-log.md`](notes/development-log.md)
（`§RoundNNN`），每轮摘要见 [`REPORT.md`](REPORT.md)，结构化记录见 [`porting.toml`](porting.toml)。

## 仓库结构

| 路径 | 内容 |
| --- | --- |
| `porting.toml` | 按子系统记录输入、发现、证据与完成判据 |
| `board.toml` | 设备配置、组件选择、启动参数、payload 标题 |
| `drivers/` | 板级驱动（见下表） |
| `patches/xnu/` | 板级 XNU 补丁序列（`series` 决定顺序） |
| `scripts/` | 构建、门禁、测试、上机脚本 |
| `notes/` | 记录、源码归档（`notes/sources/`）、施工图 |

## 驱动

| 驱动 | 作用 | 状态 |
| --- | --- | --- |
| `CloverSdhci` | eMMC（SDHCI/ADMA2）与日志 tee | 已实现 |
| `CloverPower` | PM660/PM660L、SPMI、时钟与电源域 | 已实现 |
| `CloverInput` | I2C/QUP 总线；Goodix GT9xx 触摸报表（`clover-input=4`） | 已实现（持续读取待补） |
| `NativeMobileDisplay` + 包内探针 | GOP 显示与交换路径 | 已实现（r414 前状态行尾列被 tee 切掉，队列判读待重取） |
| `CloverGpu` | Adreno 512：微码、PM4/ring/ME_INIT、CP 启动、SMMU 识别；LDO 电压可写、CTRL/PDN 不可写（轨归 CPR/固件侧）。r415 起主线靴以 `clover-gpu=0` 运行：该探针在 boot 186/187 于自己的 GDSC/LDO 行之后挂住（`clover-gpu-ldo` 那道门只写在注释里） | 进行中（主线暂停用） |
| `CloverDsp` | ADSP 探针 | 仅探针 |
| `NativeHidInput` / `NativeKeyStoreEvents` | HID 与按键、钥匙串事件 | 已实现 |
| 网络（Wi-Fi/BT） | — | 未开始 |
| 音频（ADSP/LPASS/codec） | — | 未开始 |

## 进度

| 步骤 | 状态 |
| --- | --- |
| 固件（`mu-clover`） | 通 |
| 内核入口与平台服务 | 通 |
| 存储（APFS 卷挂载） | 通 |
| 用户态（launchd 起的守护进程集） | 通 |
| 点亮屏幕（GOP 路径首帧） | 通 |
| SpringBoard / 合成器 | 未到桌面。boot 211：补按 Home 生效（presses 3→6），presentation 8→31 到 345 s，客户端 backboardd（calls=148 fails=19），但每帧仍全黑。**黑屏定因**：`AegirPoster` 崩溃栈落在 `CLKUIMetalQuadView → NUNICalliopeResourceManager`，断言 `pipelineState != nil` 失败（SIGABRT）——Metal render pipeline 创建返回 nil，锁屏时钟/星空渲染器自杀并被反复重启。路线改为 Metal 前端 → metal2vulkan → CPU 软件 Vulkan；r429 先测它能否成立的前提：设备上 RWX / RW→mprotect(RX) 映射与执行（见下） |
| JIT 前提（V2） | **通**（r430，boot 215）：RWX mmap 成功但执行 SIGBUS（W^X strip）；RW→`mprotect(RX)`→执行并返回，exit 0。宿主是 air-init 的 pid 1（ad-hoc 签名、无 entitlement；backboardd 的平台进程策略待对照） |
| V1 宿主自测 | **通（compute/render）**（r431）：metal2vulkan 的宿主桥 + Chrome 的 arm64 SwiftShader；`compute_bad 0`、`render_bad 0`；no-copy **import** 停在没有 `VK_EXT_external_memory_host`（V3 题面） |
| V3 宿主导入 | **通**（r432）：Mesa lavapipe（27.0.0-devel，LLVM 23.1.4）+ 40 行 `vkGetInstanceProcAddr` shim；`code 0 stage 0 compute_bad 0 render_bad 0 import_bad 0`。缓冲导入无拷贝；宿主页上的图像由驱动影子拷贝（`degrade-shadowed-texture`） |
| V3 设备（pid 1） | **通**（r433）：LLVM 23.1.4 与 lavapipe 交叉编到 iOS（arm64/17.0，LLVM 静态）；把 lavapipe 静态链进 metal2vulkan 自测、把它自己的路径当 loader，`code 0 stage 0 compute_bad 0 render_bad 0 import_bad 0`，`exit 0`。两个坑：沙箱拒绝 mmap Data 卷里的 dylib（只能加载已映射映像/系统卷），`os_jit_allowed()` 只认 MAP_JIT（改成 RW→mprotect RX 后通过） |
| 桌面 | 未到（卡在 Metal pipeline 创建）。V2、V1、V3（宿主+设备 pid 1）都已过；下一手：把 `libvulkan_freedreno.dylib`/插件放进 **OS 卷** `/usr/lib`（沙箱只放行系统卷），再量 backboardd 的平台沙箱；不行就走独立 helper + IPC |

## 闸门

| 闸门 | 最近一次 | 结果 |
| --- | --- | --- |
| `check-clover.py` | r433 | 未整跑（按用户要求）；标题与 trust pin 用定点 grep 核过 |
| `verify-clover-payload.py` | r433 | EXIT=0（0 FAIL；`air-init ok`，scratch-data == signed root，trustcache == owner+static+system bundle） |
| 源码针脚定点核对 | r433 | 板上 `v3.log` 6501–650f 全到、`code 0 stage 0 … import_bad 0`；`r433 stat/dlopen` 行给出沙箱与 JIT 判据 |

## 已知缺口

| 缺口 | 影响 | 处置 |
| --- | --- | --- |
| `patches.py prepare()` 信任自己的签名戳 | 签名写定后工作树漂移不再复核；r376–r414 丢了 `zzy-clover-dyld-cache-skip.patch` | 已补回；审计用**正向** `git apply --check` |
| tee 行界 320 含 msgbuf 戳 | 正文只活 304 字符，状态行尾列（`vs=`/`log=`/`logdrop=`）被切 | r414 四处天花板改 300 + 新增 `panel` 行 |
| 短行行首胶合 | 前一条无换行时粘住下一行行首 | 与切尾分开处置，未修 |
| GFX 轨归 CPR/固件侧 | AP 写不动 CTRL/PDN，a5xx 无 GMU 可照搬 | GPU 线停在此处 |

## 构建

需要 UniXNU 检出（`../UniXNU`）与 `mu-clover` 检出（`{uefi}/Common/Mu`、`Silicon/Qualcomm`、
`Silicium-ACPI` 三个包路径只有它有；旧的 `uefi-clover` 是半成品）。

```sh
# 内核
python3.13 ./unixnu build kernel --device ../UniXNU-clover

# r430 起的 air-init 内核：基仓 scripts/build-xnu-4k-baseline.sh 需要
# patches/xnu-clover/build-xnu-air-init-owner-patched.patch 的逃生键
# (XNU_NATIVE_AIR_INIT_OWNER_PATCHED=1)；本板的 owner 挂载在 board 补丁里按翻译单元强开。

# AMFI 补丁 + 归一化 + payload
python3 ../UniXNU-clover/scripts/patch-clover-amfi.py <kernel> <out>
python3 ../UniXNU-clover/scripts/normalize-kernel-macho.py <amfi>
python3.13 ./unixnu build payload --device ../UniXNU-clover --unique-chip-id 81ffcaaa \
  --uefi ../mu-clover --kernel <normalized> \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
```

## 门禁与测试

```sh
python3.13 scripts/verify-clover-payload.py <payload>   # payload 逐项校验
python3.13 scripts/test-clover-gpu.py                   # GPU 决策层 host 测试
python3.13 scripts/test-clover-input.py                 # 输入决策层 host 测试
```

## 上机

```sh
scripts/run-clover-unattended.sh --serial 81ffcaaa --flash-cache <payload.img> --boot-after-flash
```

脚本负责落回（TWRP / fastboot / EDL / Sahara 四类）与读数回传；启动参数以 `board.toml` 的
`[payload].kernel_args` 为准。
