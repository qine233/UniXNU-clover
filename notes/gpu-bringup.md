# Adreno 512（a5xx）上电与提交路线（并行线底稿）

这份文件的用途只有一个：把 GPU 线从「计划」变成一组可以按顺序验证的硬件事实。所有地址都从
clover 自己的厂商设备树里取，不从 sheng 的 sm8550 偏移里猜；sheng 能借的是**分段方式**，不是寄存器。

## 1. 已钉死的设备事实

来源：`E7G/clover-sdm660-ksb`，`arch/arm64/boot/dts/vendor/qcom/sdm660-gpu.dtsi` 与
`msm-arm-smmu-660.dtsi`（2026-11-02 取回，`/tmp/sdm660-gpu.dtsi`、`/tmp/smmu660.dtsi`）。

| 事实 | 值 | 来源行 |
| --- | --- | --- |
| GPU 寄存器块 | `0x05000000` + `0x40000` | `msm_gpu` `reg` |
| QFPROM 块 | `0x07800000` + `0x6220` | `reg` 第二段 |
| chipid | `0x05010200`（a512） | `qcom,chipid` |
| GPU 中断 | `GIC_SPI 300`，level high | `interrupts` |
| GPUCC | `0x05065000`，`qcom,gpucc-sdm660` | `clock_gpu: clock-controller@5065000` |
| GCC | `0x01830000`（时钟控制器） | `sdm660.dtsi` GCC 节点 |
| GPU SMMU | `0x05040000` + `0x10000`，SMMUv2，3 级页表 | `kgsl_smmu` |
| SMMU 上下文 | bank 0 = user（`gpu-offset 0x48000`），bank 2 = secure | `gfx3d_user` / `gfx3d_secure` |
| SMMU 中断 | `GIC_SPI 329/330/331` | `kgsl_smmu interrupts` |
| micro-MMU 控制 | `0x6000`；protect 区 `0x40000 + 0x10000` | `kgsl_msm_iommu` |
| 电源域 | `gdsc_gpu_cx`、`gdsc_gpu_gx`（GX 要 `GFX3D_CLK_SRC`） | `sdm660.dtsi` 尾部 |
| GPU 时钟 | `GPUCC_GFX3D_CLK`、`GCC_GPU_CFG_AHB_CLK`、`GPUCC_RBBMTIMER_CLK`、`GCC_GPU_BIMC_GFX_CLK`、`GCC_BIMC_GFX_CLK`、`GPUCC_RBCPR_CLK` | `msm_gpu clocks` |
| zap | `qcom,kgsl-hyp`，PAS 13，firmware `a512_zap`，区域 `0xf0b00000 + 0x2000` | `pil_gpu` / `notes/clover-device-tree-sources.md` |
| 初始档位 | `qcom,initial-pwrlevel = <6>`；最高 750 MHz，最低 XO 19.2 MHz | `msm_gpu` / `pwrlevels-0` |
| idle 超时 | `64`（注释 `<HZ/12>`） | `qcom,idle-timeout` |

**a5xx 与 sheng 的 a740 的结构差异（不是偏移差异）：** a5xx 没有 GMU、没有 HFI/RSC/PDC，也没有
SMMU 上下文里的 GMU 固件；命令处理器在 RB 侧，微码是 `a530_pfp.fw` / `a530_pm4.fw`（Android 侧命名）。
因此 sheng 的 `NativeAdrenoGmu*`、`kits/AdrenoFirmware` 的 HFI 阶段整段不适用。可借的是
`NativeAdrenoPublish.cpp` 的分段：**先发布一个能被 Metal 发现的设备壳，再谈真提交**。

## 2. 为什么第一轮只读

sheng 的 `GpuPowerProbe.cpp` 把「读电源/时钟状态」和「第一次写 GDSCR/时钟」分成 stage 1–3 与
stage 5 之后。本板连 GDSC 的当前状态都没读过；在没有读数的情况下写任何一位都可能把整机带进
不可恢复的电源状态（而且现在还有 726/1324/1499 s 的间歇早停没定性）。所以 r282 只做**只读**：

1. 映射 `0x05000000`、`0x05065000`、`0x01830000`、`0x05040000`；
2. 每 120 s 与 keystore/WDT 行一起印：chipid 回读、GPUCC 的 GDSCR/RBBM 状态、GCC 的 GPU 分支
   enable/div、SMMU 的 sACR/context bank 有效位；
3. 不做任何写、不申请 IRQ、不碰 SMMU 页表。

判据先写死：

- `chipid != 0x05010200` ⇒ 地址或时钟没到，后面所有读数都不可信；
- GDSC 显示 already-on ⇒ 固件/ABL 把 GPU 留在了上电状态，先查谁开的，再决定是否接手；
- 所有 GPU 寄存器读回全 `0xffffffff` ⇒ AHB 时钟没开（只读也会挂总线），下一步先开 `GCC_GPU_CFG_AHB_CLK` 再读；
- 全 0 ⇒ 寄存器块没供电，读到的是未供电的总线默认值。

## 3. 后续顺序（每步一个可反驳的读数）

1. **只读取证**（上面）；
2. **电源/时钟真开**：GDSC cx → gx、AHB/CFG、GFX3D root，写后回读 GDSCR；
3. **SMMU**：bank 0 的页表、bypass 与 fault 中断；先让 SMMU 能翻译一段 host 内存；
4. **RB/CP + 微码**：从本机 firmware 分区取 `a530_pfp.fw`/`a530_pm4.fw`（先证明文件在），ring、fence、
   一次 NOP 提交；
5. **设备壳**：照 sheng 的 `AGXAccelerator : IOAcceleratorES` + `AGXDeviceUserClient` 命名发布，
   `newTextureWithDescriptor:` 的最小语义先只做分配，不做渲染；
6. **显示**：只有前面都通了才谈 MDP5/DSI；当前桌面仍由 CPU presenter 顶。

## 4. 与主线的关系

这条线**不阻塞** r281 的 keystore/WDT 读数，也不改这一轮的 payload。GPU 探针在 r282 才进
`board.toml`，且第一版只读、只走 r281 已经验证过的周期行通道。
