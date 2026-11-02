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
| GCC | `0x00100000` + `0x94000`（时钟控制器） | `sdm660.dtsi` `clock-controller@100000`；`CloverInput/InputRegs.h` 已按同一 DT 钉过 |
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

1. 映射 `0x00100000`（GCC）、`0x05065000`（GPUCC）；**不映射 GPU 核与 SMMU**——这两块要 GPU 的
   GDSC/GX 时钟在才能安全读，r282 不碰；
2. 每 120 s（首行 240 s）与 keystore/WDT 行一起印：GCC 三根 GPU 分支的 enable 位 + 两条 eMMC 分支
   作正对照；只有 `GCC_GPU_CFG_AHB_CLK` 的 enable 位为 1 时才读 GPUCC 的 `gdsc_gpu_cx`（GDSCR
   0x1004、状态在 hw-ctrl 0x1008 的 bit31）、`gdsc_gpu_gx`（GDSCR 0x1094 的 bit31）与 gfx3d 分支；
3. 不做任何写、不申请 IRQ、不碰 SMMU 页表、不读 GPU 核 chipid（留到能安全开 GX 的那一轮）。

**GDSC 偏移的来源**（2026-11-02 取回，均在下游仓库
`LineageOS/android_kernel_xiaomi_sdm660`、分支 `lineage-17.1`）：

- `arch/arm/boot/dts/qcom/msm-gdsc-660.dtsi`：`gdsc_gpu_cx: qcom,gdsc@5066004`，`hw-ctrl-addr = <&gpu_cx_hw_ctrl>`，
  该 syscon 是 `0x5066008`；`gdsc_gpu_gx: qcom,gdsc@5066094`，`domain-addr = <&gpu_gx_domain_addr>`
  （`0x5065130`）、`sw-reset = <&gpu_gx_sw_reset>`（`0x5066090`）；
- `drivers/clk/qcom/gdsc-regulator.c`：`PWR_ON_MASK = BIT(31)`；有 `hw-ctrl-addr` 时状态从 hw-ctrl 读，
  否则从 GDSCR 读（`poll_gdsc_status()`）；`qcom,gds-timeout = <2000>`；
- `drivers/clk/qcom/gdsc.c`：`SW_COLLAPSE_MASK = BIT(0)`，写 0 表示上电、写 1 表示塌陷——所以只读时
  「bit0=0 && bit31=1」才是「已上电且没有人在请求关」。
- `drivers/clk/qcom/gcc-sdm660.c`：`gcc_gpu_cfg_ahb_clk` 的 `enable_reg = 0x71004`、`BIT(0)`、
  `CLK_IS_CRITICAL`；`gcc_gpu_bimc_gfx_clk` 0x71010、`gcc_bimc_gfx_clk` 0x7106c，都是 `BIT(0)`。
  这些偏移相对 DT 的 GCC 基址，所以物理地址分别是 `0x081004/0x081010/0x08106c`。正对照是
  `gcc_sdcc1_ahb_clk`（0x116008）与 `gcc_sdcc1_apps_clk`（0x116004）：eMMC 在 `sdhc_1`（`0xc0c4000`），
  每一轮块读写都证明这两根分支是活的。
- 下游 `gcc-sdm660.c` 的 probe 在碰 GPUCC 之前先 `clk_prepare_enable(gcc_gpu_cfg_ahb_clk)`，注释是
  “This clock is used for all GPUCC register access”；r282 把这句当作读 GPUCC 的门。

判据先写死：

- GCC 的 `gcc_gpu_cfg_ahb_clk` bit0 = 0 而两条 eMMC 分支在同一行 = 1 ⇒ GCC 窗口是活的、GPU 的 AHB
  投票确实没人开；这一支是本轮**唯一**允许下「GPU 时钟没开」结论的形状；
- `gcc_gpu_cfg_ahb_clk` 的整字读回 `0xffffffff` ⇒ 总线没应答，不是「时钟开着」；同样不读 GPUCC；
- AHB 开着但 `gdsc_gpu_cx` 的 hw-ctrl bit31 = 0 ⇒ CX 没上电，GPUCC 里的 GDSCR 读数不可信；
- CX 上电、`gdsc_gpu_gx` bit31 = 0 ⇒ 固件只留了 CX，GX 归我们开；
- 两颗 bit31 都是 1 ⇒ 固件/ABL 已经把 GPU 留在上电状态，先查谁开的，再决定是否接手；
- 正对照反了（eMMC 分支读 0）⇒ 本轮所有 GPU 读数作废，先修窗口。

（chipid 不再作为 r282 的判据：GPU 核寄存器要 GX 与 gfx3d 时钟，读它等于在没有控制的块里冒挂总线的险。
等 r283 能写 GDSC/时钟后再读，并用 a512 的值 `0x05010200` 验收。）

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
