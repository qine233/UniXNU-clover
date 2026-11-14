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
| SpringBoard / 合成器 | 未到桌面。boot 188（r415，GPU 探针关）：显示通道第一帧 ~837 s、到死线仍在拷贝（428 次、0 拒绝），面板以不透明黑/全透明黑为主（`col=`）；boot 186/187 曾在 GPU 探针之后极早停 |
| 桌面 | 未到 |

## 闸门

| 闸门 | 最近一次 | 结果 |
| --- | --- | --- |
| `check-clover.py` | r415 | EXIT=0 |
| `verify-clover-payload.py` | r415 | EXIT=0，51 OK / 0 FAIL |
| 负测（比对盘上产物，不动活树） | r415 | 12/12 |

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
python3.13 scripts/check-clover.py                      # 全量门禁
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
