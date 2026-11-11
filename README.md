# UniXNU-clover

小米平板 4 8 英寸 LTE（2018，代号 `clover`）的 XNU 移植：4 GB / SDM660（Kryo 260，ARMv8.0，无指针认证），
跑原封不动的 **iPad7,1 / iPadOS 17.7.11 / 21H461** 4 KiB 页镜像。配套固件仓库 `uefi-clover`。

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
| `NativeMobileDisplay` + 包内探针 | GOP 显示与交换路径 | 已实现（合成器停摆待查） |
| `CloverGpu` | Adreno 512：上电、chipid、微码、CP 启动、SMMU 识别 | 进行中 |
| `CloverDsp` | ADSP 探针 | 仅探针 |
| `NativeHidInput` / `NativeKeyStoreEvents` | HID 与按键、钥匙串事件 | 已实现 |
| 网络（Wi-Fi/BT） | — | 未开始 |
| 音频（ADSP/LPASS/codec） | — | 未开始 |

## 进度

| 步骤 | 状态 |
| --- | --- |
| 固件（`uefi-clover`） | 通 |
| 内核入口与平台服务 | 通 |
| 存储（APFS 卷挂载） | 通 |
| 用户态（launchd 起的守护进程集） | 通 |
| 点亮屏幕（GOP 路径首帧） | 通 |
| SpringBoard / 合成器 | 卡住：backboardd 的 IOKit 交换在约 966 s 后停止 |
| 桌面 | 未到 |

## 构建

需要 UniXNU 检出（`../UniXNU`）与 `uefi-clover` 产物。

```sh
# 内核
python3.13 ./unixnu build kernel --device ../UniXNU-clover

# AMFI 补丁 + 归一化 + payload
python3 ../UniXNU-clover/scripts/patch-clover-amfi.py <kernel> <out>
python3 ../UniXNU-clover/scripts/normalize-kernel-macho.py <amfi>
python3.13 ./unixnu build payload --device ../UniXNU-clover --unique-chip-id 81ffcaaa \
  --uefi ../uefi-clover --kernel <normalized> \
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
