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

## 5. gfx3d 的父源/频率表与 GX 的完整账单（2026-11-03 取回，全部有源）

r288/r289 期间把这条线一直缺的那张表补齐了，r290 当天又补上 GFX LDO 那一格（§5.6）。原文九份已进仓，
下面的每一条都能在本仓 `notes/sources/sdm660/` 里按行号核对，不再依赖 `/tmp`：

| 文件 | sha256 前 12 | 上游 |
| --- | --- | --- |
| `gpucc-sdm660.c` | `2305d741b7da` | `LineageOS/android_kernel_xiaomi_sdm660` @ `lineage-20`（下游 CAF，这块板子真跑过的驱动） |
| `sdm660.dtsi` | `4d84f5169e88` | 同上 `arch/arm64/boot/dts/vendor/qcom/` |
| `sdm660-gpu.dtsi` | `3c3abcba2bbf` | 同上 |
| `msm-gdsc-660.dtsi` | `e31d934fbb42` | 同上 |
| `sdm660-regulator.dtsi` | `4dc3788265a9` | 同上（§5.4 用它更正「三颗轨」的读法） |
| `adreno-gpulist.h` | `e998295aa216` | 同上 `drivers/gpu/msm/` |
| `msm_gfx_ldo.c` | `929bc99f86f1` | 同上 `drivers/regulator/`（§5.6：GFX LDO 的寄存器表与位名） |
| `cpr4-mmss-ldo-regulator.c` | `045af7dc9c8c` | 同上（§5.6：MMSS CPR4 的六档参考电压） |
| `cpr3-regulator.h` | `2cac843f9978` | 同上（CPR3/CPR4 共用的内部结构声明，本轮未据它下任何读数） |

### 5.1 `gfx3d_clk_src` 的父源表（`gpucc-sdm660.c:57`，`P_XO` 等枚举在 `:34`）

`cmd_rcgr = 0x1070`、`hid_width = 5`、`mnd_width = 0`、`flags = FORCE_ENABLE_RCG`（`:199`）。
父源把「硬件 mux 值」和「时钟名」成对给出（索引即 `cfg` 里读回的 src 值）：

| 索引 | 硬件 mux 值 | 名字 |
| --- | --- | --- |
| 0 | 0 | `xo`（19.2 MHz） |
| 1 | 1 | `gpu_pll0_pll_out_main` |
| 2 | 3 | `gpu_pll1_pll_out_main` |
| 3 | 5 | `gcc_gpu_gpll0_clk` |
| 4 | 7 | `core_bi_pll_test_se` |

`ftbl_gfx3d_clk_src`（`:171`）只有 `F_GFX(f, s, h, m, n, sf)` 一种形状，`s` 恒为 0、`h` 恒为 2
（除 19.2 MHz 那档 `h=1`），真正决定频率的是最后一列 PLL 频率：160/266/370/430/465/585/588/647/700/750 MHz
= 640/532/740/860/930/1170/1176/1294/1400/1500 MHz ÷ 2。SDM630 换另一张表（`:186`）。
设备侧的 `ro.vendor.gpu.available_frequencies = 647000000 588000000 465000000 370000000 266000000 160000000`
正是这张表的子集，两边对上。

**boot 74 的 `rcg=80000000` 说明什么、不至于说明什么。** 整行原文（`~/clover-out/round287-log-window.bin`，
t=1338）：`gpucc post cx=a0282000 cxhw=80001fe0 cx_on=1 cx_collapse=0 gx=08282000 gx_on=0
gx_collapse=0 gfx3d_en=1 cxo_en=1 rcg=80000000 dom=00140001 sw=00000000`。按上表，CMD 的低 2 位 = 0
就是「已经选在 `xo`」，bits[15:8]=0 就是分频 1——也就是说这一行**不能**读成「gfx3d 没有时钟源」。
bit31 不在这两份源（下游 `gpucc-sdm660.c` 与 mainline 同名文件）对 CMD_RCGR 的任何字段里；同理
`cx=a0282000` 与 `gx=08282000` 都带 bit27，而 `gdsc-regulator.c:29-39` 从 BIT(31) 到 BIT(0) 一个都
认不出它——**两个没名字的位，别拿它们做判决**。所以 r288 流水日志里那句「没给它一个跑起来的时钟源，
所以喂给 GX 的 gfx3d 时钟实际不转」是**过度论断**，本轮撤回；boot 74 真正说的是 `gfx3d_en=1`、
`cxo_en=1` 而 `gx_on=0`，仅此而已。

### 5.2 GX 上电前，厂商的账单是六根时钟、三颗轨

`msm_gpu` 的 `clocks`/`clock-names`（`sdm660-gpu.dtsi:68-76`）逐条列出，括号里是我们在 boot 74
读到的状态：

1. `GPUCC_GFX3D_CLK` = GPUCC `0x1098` BIT(0)（`gpucc-sdm660.c:266`）——**已开**（`gfx3d_en=1`）；
2. `GCC_GPU_CFG_AHB_CLK` = GCC `0x71004` BIT(0)——**已开**（`gpu_ahb=1`）；
3. `GPUCC_RBBMTIMER_CLK` = GPUCC `0x10d0` BIT(0)，父 `rbbmtimer_clk_src`（`cmd_rcgr 0x10b0`，
   父源表 `map_0`：`cxo_a`=0 / `gcc_gpu_gpll0_clk`=5 / `gcc_gpu_gpll0_div_clk`=6 / `core_bi`=7，
   频率只有 19.2 MHz 一档）——**从没碰过**；
4. `GCC_GPU_BIMC_GFX_CLK` = GCC `0x71010`——**已开**（`gpu_bimc=1`）；
5. `GCC_BIMC_GFX_CLK` = GCC `0x7106c`——**已开**（`bimc=1`）；
6. `GPUCC_RBCPR_CLK` = GPUCC `0x1054` BIT(0)，父 `rbcpr_clk_src`（`cmd_rcgr 0x1030`，同一张
   `map_0`，19.2 MHz / 50 MHz 两档）——**从没碰过**。

还有第 7 项是这两颗 RCG 的公共父：`gpucc_cxo_clk` = GPUCC `0x1020` BIT(0)（`:249`），`map_0` 名字
`cxo_a` 就是它产出的。boot 74 已经把它读回来了——`cxo_en=1`，所以这条父也在。
真正从没碰过的是 `0x10d0 / 0x10b0`（rbbmtimer）与 `0x1054 / 0x1030`（rbcpr）两对，加上 CONFIG
寄存器 `0x1074`（我们只读过 CMD_RCGR `0x1070`，`clk_rcg2` 的 CONFIG 在 `cmd_rcgr + 4`，两份源都是这个形状）。

轨（`clock_gfx: gfx@5065000`，`sdm660.dtsi:1081-1097`；**它们的实现见 §5.4——都不是 SPMI 直读**）：

* `vdd_dig_gfx-supply = <&pm660l_s3_level>` → **CX 数字轨是 PM660L 的 S3**（RPM SMD 持有）；
* `vdd_mx_gfx-supply = <&pm660l_s5_level>` → **GPU PLL 吃 PM660L S5**（RPM SMD 持有）；
* `vdd_gfx-supply = <&gfx_vreg_corner>` → GX 的 corner（CPR4，MMIO `0x05061000`；`sdm660.dtsi:2836`
  的 parent 也是它）；
* `qcom,gfxfreq-corner`：0 / 160→1 / 266→2 / 370→3 / 465→4 / 588→5 / 647→6 / 700→7 / 750→7。

`gpucc_660_probe()`（`:401-468`）在注册任何时钟之前先 `devm_regulator_get` 这三颗轨，拿不到就
`return`（`:424-448`），随后才对两颗 GPU PLL 做 `clk_alpha_pll_configure`（`:465-468`，
`l=0x29`、`alpha=0xaaaaab00`、`config_ctl=0x4001055b`、VCO 档 `0x2<<20`）。**换句话说厂商自己也是
「先把轨拿到，再谈 PLL 和时钟」**——这正是下一步该问的形状。

同一窗口有两个提供者节点：`clock_gpu: clock-controller@5065000`（`qcom,gpu-sdm660`，`:1074`，
`msm_gpu` 用它取 `GPUCC_RBCPR_CLK`）与 `clock_gfx: gfx@5065000`（`qcom,gpucc-sdm660`，`:1081`）。
地址相同、驱动不同，别把它当成两处硬件。

### 5.3 下一步（可反驳的读数，不是「再猜一次」；§5.4 已把第 1 条改过一次）

1. **读「角」，不读轨**：轨本身读不到（见 §5.4，S3/S5 是 RPM SMD 持有的），能读到的是 GX 的角
   ——CPR `0x05061000` 与 GFX LDO `0x0506e000` 都在 MMIO 上，和 boot 74 已有的 `cx_on=1` 放在一起。
   若角读回 0，那 `gfx3d` 只能停在 XO 档就是**结果**而不是原因；若角已经在，这一支当场结掉。
2. **补齐账单**：把 `0x1020 / 0x10b0+0x10d0 / 0x1030+0x1054` 三票按厂商表在 XO 档使能并回读，
   与 `gfx3d`、GX GDSCR 同排打印；`rcg` 那行改印**拆解后的 src/div/mux 字段**而不是整字，并同时读
   CONFIG `0x1074`（本轮从没读过它），免得再拿一个带无名位的裸字做判决
   （仪器课：一行读数的解释宽度就是它的上限）。
3. 只有这两步之后 `gx_on` 仍为 0，才轮到「GX 的塌陷由 GPMU/RBCCU 掌管、GDSCR 写不动」这条支——
   它的正对照是 `a5xx_regulator_enable()` 用 `A5XX_GPMU_RBCCU_PWR_CLK_STATUS`（`a5xx_reg.h:766`）
   轮询状态，而不是读 GDSCR；`adreno-gpulist.h:592` 的 a512 条目里带 `zap_name = "a512_zap"`，
   说明本板 a512 确实走 GPMU/zap 那条路。

### 5.4 更正 §5.2 与 §5.3 第 1 条：那三颗「轨」不是 SPMI 读得到的（2026-11-03，同一天）

`sdm660-regulator.dtsi`（同一棵树、同一分支；`sha256 4dc3788265a9…`，已进仓
`notes/sources/sdm660/`）把这三颗 regulator 的实现写清楚了：

* `pm660l_s3_level`（`:57`）与 `pm660l_s5_level`（`:104`）都是 `compatible =
  "qcom,rpm-smd-regulator"`——**RPM 通过 SMD 持有的轨**（文件注释 `:53` 写 S3+S4 = VDD_CX、`:99`
  写 S5 = VDD_MX）。它们没有 SPMI 外设地址，所以 r182 的 SPMI 通路读不到；§5.3 原先那句
  「用 r182 的 SPMI 通路读 PM660L S3/S5」是错的，照做会去读一个不存在的地址。这一支要么走
  RPM SMD（那是 GLINK 的活，而 ADSP/GLINK 自己还压在排期 4），要么放弃。
* `vdd_gfx = &gfx_vreg_corner` 也不是 PMIC 轨，而是 **CPR4 角调节器**：`gfx_cpr: cpr4-ctrl@05061000`
  （`qcom,cpr4-sdm660-mmss-ldo-regulator`，`:534-542`），`gfx_vreg_corner` 在它 `thread@0` 里
  （`:569`，`regulator-min/max-microvolt = <1>/<7>` = 角 1..7，正对 `qcom,gfxfreq-corner` 的 7 档）。
  **它是 MMIO**：`cpr_ctrl = 0x05061000 + 0x4000`，另有一段 fuse `0x00784000 + 0x1000`。注意这块
  不在 GPUCC 窗口内（GPUCC 是 `0x5065000..0x5075000`），要单独映射。
* 而 CPR 节点自己 `clocks = <&clock_gpu GPUCC_RBCPR_CLK>, <&clock_rpmcc RPM_SMD_CNOC_CLK>`
  （`:538-540`）——**rbcpr 正是我们从没使能过的那根**（§5.2 第 6 条）。于是这条链是自洽的、
  且读得出真假：`rbcpr_clk` 关 ⇒ CPR 不转 ⇒ `gfx_corner` 停在 0 角 ⇒ `vdd_gfx` 不上角 ⇒ GX 上不了电。
  测法：在 XO 档打开 `0x1030`（RCG，`map_0` 的 `cxo_a`=0，19.2 MHz 一档）与 `0x1054`（分支），
  再读 CPR `0x05061000`、GFX LDO `ldo@0506e000 + 0x34`（`qcom,sdm660-gfx-ldo`，400..925 mV，`:523-529`）
  与 mem-acc 角 `regulator@01fcf004`（`qcom,mem-acc-regulator`，`acc-sel-l1` 第 0 位，`:510-520`）。
  `gx_on` 跟着变就是这条链；不跟着变就退回 §5.3 第 3 条。

r290 的 GPU 那一格因此定为：**开 rbcpr + rbbmtimer 两票 → 读 CONFIG `0x1074`、CPR `0x05061000`、
GFX LDO `0x0506e000`、mem-acc `0x01fcf004` → 再看 `gx_on` 与 `chipid`**。S3/S5 两轨不在这一格。

### 5.5 r290 实际发出去的那一格，比 §5.4 结尾那句窄（2026-11-03，发版前自己核对）

§5.4 那句把「读 CPR / LDO / mem-acc」也写进了 r290，但本轮落地时**没有**照做，理由不是省事：

* 三颗都**不在 GPUCC 窗口**里（CPR4 `0x05061000+0x4000`、GFX LDO `0x0506e000+0x34`、mem-acc
  `0x01fcf004`），要各自新建 `IODeviceMemory::withRange` 映射——这是新代码，不是一行读数；
* 更要紧的是**偏移没有出处**。盘上的 `notes/sources/sdm660/` 六份文件给的是 DT 里的**基址与
  compatible**，没给「角状态在 +0x几」。在没有一张有出处的寄存器表之前打这些字，读回来的数字
  没人能解释——正是本项目对「没有名字的位不许读数」那条规矩要挡的事。

所以 r290 只做两件事，且都建立已经钉死的位上：

1. **把「问了」和「在跑」分开**：四支 GPUCC branch（gfx3d `0x1098`、cxo `0x1020`、rbcpr `0x1054`、
   rbbmtimer `0x10d0`）各读一次，同时报 BIT(0)（enable 请求）与 BIT(31)（`CBCR_CLK_OFF`，
   `clk-branch.h:59`；`BRANCH_HALT` 的极性 `:31` 说 1 = 停）。boot 74/76 那句 `gfx3d_en=1` 一直只说了
   前半句。
2. **补上账单里从没碰过的两票**：rbcpr 与 rbbmtimer（§5.2 六根时钟里的第 5、6 根）。
   三颗 RCG 命令字（`0x1070`/`0x1030`/`0x10b0`）保持**原字**输出，因为本轮一个频率都没设，解码一份
   自己没设过的速率就是猜。

**为什么值得单独一轮**：boot 76（v0.6.86）上 t=1338/1582/1707 三次 fire 的 gpucc 行逐字段与 boot 74
一字不差（`cx=a0282000 gx=08282000 rcg=80000000 cx_on=1 gx_on=0 gfx3d_en=1 cxo_en=1`）——`gx_on=0` 是
稳定硬件状态，不是竞态、不是时机。既然稳定，下一步能推翻的假设就只有两类：**父/票没到位**（本轮的
两票 + halt 位能证）与**角/轨没到位**（要 CPR/LDO，见下）。

**r291 的门槛（写在这里，免得又空跑一轮）**：先取一份**有出处的** CPR4 寄存器表——候选是 mainline
`drivers/misc/qcom-cpr.c`（`cpr4-ctrl@05061000` 的 `qcom,cpr4-*` 正是它的绑定）里物理角/目标角的
偏移宏，GFX LDO 若上游无对应驱动则退回 vendor 头文件并注明版本——取到之后才建映射、才打读数。
mem-acc `0x01fcf004` 是 vendor-only（`qcom,mem-acc-regulator`），上游没有，暂时排在后面。
桌面仍 CPU-first。

### 5.6 门槛已在 2026-11-03 当天跨过：GFX LDO 有名字齐全的寄存器表（vendor，本板 SoC 对应）

§5.5 说「偏移没有出处，所以不建映射」——这句现在只对了半天。用 GitHub 代码检索按 DT 里那颗
compatible 原文（`qcom,sdm660-gfx-ldo`）找到的正是它的厂商驱动：

* 顺带更正 §5.5 给的候选路径：mainline 的 CPR 驱动是 `drivers/pmdomain/qcom/cpr.c`（MAINTAINERS
  `F:` 两行之一，另一行是 binding yaml），兼容表里**只有** `qcom,qcs404-cpr`，全文没有 `cpr4`；
  §5.5 写的 `drivers/misc/qcom-cpr.c` 在 master 上不存在（404）。所以 CPR4 这一支上游取不到，
  下面的厂商源码是唯一有名字的出处。

* 来源：`LineageOS/android_kernel_xiaomi_sdm660` @ `lineage-20`（与本文件上面那张表同一仓、同一分支，
  也就是这块板子真跑过的 CAF 内核），路径 `drivers/regulator/`。三份已归档：
  `notes/sources/sdm660/msm_gfx_ldo.c`（sha256 `929bc99f86f1`）、`cpr3-regulator.h`（`2cac843f9978`）、
  `cpr4-mmss-ldo-regulator.c`（`045af7dc9c8c`）。
* `msm_gfx_ldo.c:1504` 的 match 表里就有 `.compatible = "qcom,sdm660-gfx-ldo"`（另一颗是 msm8953），
  而本板 DT 的 `gfx_ldo_vreg: ldo@0506e000`（`sdm660-regulator.dtsi:523-530`）写的正是这个
  compatible、`reg = <0x0506e000 0x34>` —— **窗口大小 0x34 和驱动的寄存器表正好对上**，
  所以偏移 0x00..0x30 可用，`ADC_CTL_REG 0x34`（`:63`）落在窗口外，不许碰。
* 名字齐全的读数（都在这份文件里）：
  `LDO_VREF_SET_REG 0x18`，值在 `VREF_VAL_MASK = GENMASK(6, 0)`（`:37`），解码
  **µV = val × 5000 + 375000**（`:1371`，`MIN_LDO_VOLTAGE 375000`/`LDO_STEP_VOLATGE 5000` 在 `:65/:67`；
  写方向的反函数 `GET_VREF` 在 `:266`）；
  `PWRSWITCH_CTRL_REG 0x1C`，位名齐全：`CPR_BYPASS_IN_LDO_MODE BIT(30)`、`EN_LDOAP_CTRL_CPR BIT(29)`、
  `PWR_SRC_SEL BIT(9)`、`LDO_BYPASS BIT(6)`、`LDO_PDN BIT(5)`、`LDO_UNDER_SW_CTRL BIT(4)`（`:39-52`）；
  `PWRSWITCH_STATUS_REG 0x28`：`LDO_VREF_SETTLED BIT(4)`、`LDO_READY BIT(2)`（`:57-58`）、
  `BHS_EN_REST_ACK BIT(1)`，而驱动自己判定「稳定」用的就是 `reg & (SETTLED|READY)` 非零
  （`:307`、`:1336`）。`LDO_STATUS1_REG 0x24`（`:54`）在这份源码里**定义了但没用**，
  所以本轮不读它、也不给它任何解释。
* `cpr4-mmss-ldo-regulator.c` 补上另一头：`SDM660_MMSS_FUSE_CORNERS = 6`（`:31`），六档参考电压
  **585/645/725/790/870/925 mV**（`:116-123`），偏移步进 10 mV（`:126`）。这里有一处**没对齐**：
  DT 的 `gfx_vreg_corner` 写 `regulator-min/max-microvolt = <1>/<7>`（角 1..7），厂商 fuse 表只有
  6 档。这个差我暂时解释不了，所以 r291 不靠「角号」下结论，只读 LDO 自己的电压与状态位。

**r291 因此定为**：读 `0x0506e000 + 0x34` 这三枚寄存器（原写「第三次用同一套 `IODeviceMemory::withRange`
路径」——发版前核对 DT 发现该窗口已在 GPUCC 的 `0x10000` 范围内，改为走已有映射，见 §5.8），
读 `0x18`/`0x1C`/`0x28` 三个带名字的寄存器，把 µV 按上面的公式解出来，连同当拍的 `gx_on` 一起打。
判据事先写死：`LDO_READY=0` 或 µV 落在 375000（=val 0，即从没人写过） ⇒ GX 的供电确实没上角，
这条链成立，下一步才谈 CPR/RCG 编程；`LDO_READY=1` 且 µV 已经在角上而 `gx_on` 仍 0 ⇒ 债不在轨上，
回到 GPUCC 的分支/父源那一支（也就是 r290 的 halt 位读数负责区分的那一支）。mem-acc
`0x01fcf004` 仍然排在后面：它是 vendor-only，暂无本板对应源码。桌面仍 CPU-first。

### 5.7 boot 77 带回 §5.5 那道门槛的读数：票到了、钟没起，而「没起」和 gx 没合上是一件事

行本身（`~/clover-out/round290-log-window.bin.txt`，t=1097 与 t=1219 两拍逐字段相同）：

```
clover r290: gpu t=1097 branches beat gfx3d=1/1 cxo=1/0 rbcpr=1/0 rbbm=1/1 en/halt
             rcg3d=80000000 rcg_rbcpr=00000000 rcg_rbbm=80000000
```

三支结论要分开写，因为 §5.5 的门槛把这一轮当成了二选一，真读数比那个形状更窄：

1. **`cxo=1/0` 是这行的地基。** 这一支本轮没有写任何东西（固件开着它），它给出「halt=0 长什么样」。
   没有它，`halt=1` 可以被读成「BIT(31) 在这个窗口里恒为 1」。
2. **`rbcpr=1/0` 证明写生效。** 0x1054 的 BIT(0) 是本轮新按的一票，它从 halt 走到不 halt——
   所以「GPUCC 的写在 `gfx3d_en=1` 那种地方到底有没有落进硬件」这个疑问一并结掉。
3. **`gfx3d=1/1`、`rbbm=1/1` 不等于「父源/速率缺失」。** GX 电源域从头到尾 `gx=08282000`、
   `gx_collapse=0`、`gx_on=0`：SW_COLLAPSE 已经放开，PWR_ON 就是不来。GDSC 没合上时它下游的根时钟
   没有理由离开 HALT，所以**按 §5.1 的表给 RCG 编程速率不能排到下一轮**——那一支的前提（gx 域有电）
   还没成立。rbbmtimer 属于 RB 侧、同样在 gx 之后，形状一致是旁证。
4. **`rcg3d=80000000` / `rcg_rbbm=80000000` 对 `rcg_rbcpr=00000000`：只记相关性。** halt 的两支命令字都是
   0x80000000，在跑的那支是全 0。要把 BIT(31) 说成 CG hidden/reset，需要一份能归档的
   `drivers/clk/qcom/clk-rcg.c`——本树的 `clk-rcg.h` 只有 191 行、不含这些偏移名，所以这里不下判语。

于是战场从「时钟区」挪回「电」：§5.6 那张 GFX LDO 表就是为这一步取的，r291 的三寄存器读数与判据照 §5.6 末
执行。另记两条与本节相关的仪器事实：`branches post` 那拍被环转出去了（窗从 796 s 起，首次上电更早），留下
`beat` 副本顶上来——r249 那条「一次性判决行要有周期副本」在这轮真的被用到；`dom=00140001`、`sw=00000000`
继续原样报，不编故事。

### 5.8 r291 发出去的那一格：走 GPUCC 已有的映射读，而不是第三张窗口（§5.6 结尾那句要更正）

§5.6 把 r291 写成「第三次用同一套 `IODeviceMemory::withRange` 路径映射 `0x0506e000 + 0x34`」。发版前自己核对
DT 时发现那一刀是多余的，而且方向不对：

* `sdm660.dtsi:1074/:1076` 的 `clock_gpu: clock-controller@5065000` 与 `:1081/:1083` 的
  `clock_gfx: gfx@5065000` **两个节点都写 `reg = <0x5065000 0x10000>`**，而 `sdm660-regulator.dtsi:523/:525`
  把 `gfx_ldo_vreg` 放在 `0x0506e000 + 0x34`——`0x0506e000 - 0x05065000 = 0x9000`，正好落在 r286 已经映射的
  GPUCC 范围内。
* 再要一张重叠窗口不是「更保守」，是让 IOKit 对同一页给出第二个映射，还会让「LDO 在不在 GPUCC 里」这件事
  变成两条互不相干的读数。所以 r291 用 `kGpuccLdoVref 0x9018` / `kGpuccLdoCtrl 0x901c` /
  `kGpuccLdoStatus 0x9028` 三个 GPUCC 内偏移读，`check-clover.py` 把「本驱动只开三张窗口」钉成
  `IODeviceMemory::withRange(` 计数 = 3（GCC、GPUCC、GPU core）——将来谁缩小 `kGpuccSize` 把 LDO 挤出映射，
  这枚针会立刻报错而不是读出一个漂亮的 0。

行（`post`/`beat` 两支，字段完全一致，与 r290 同规矩）：

```
clover r291: gpu t=%u ldo %s vref=%08x val=%u uv=%u ctrl=%08x byp=%u pdn=%u cpr_byp=%u
             sts=%08x ready=%u settled=%u cx_on=%u gx_on=%u
```

`uv` 是厂商自己的算式（`msm_gfx_ldo.c:1371`：`(reg & GENMASK(6,0)) * 5000 + 375000`），`ready`/`settled` 是
它判稳用的那两个位（`:307` 的 `reg & (LDO_VREF_SETTLED_BIT | LDO_READY_BIT)`）；`byp`/`pdn`/`cpr_byp` 取
`PWRSWITCH_CTRL` 的 BIT(6)/BIT(5)/BIT(30)。**同拍带上 `cx_on`/`gx_on`**：这两票本轮已连三靴稳定读到，
它们在这行里的作用是正对照——若 `vref`/`ctrl`/`sts` 全 `0xffffffff` 而 `cx_on=1`，那是 0x9000 这页没应答，
不能读成「电压为 0」；反过来三词有值而 `cx_on` 也照常，才是轨真的在说话。

不读的两处也写死：`LDO_STATUS1_REG 0x24` 在厂商驱动里定义了但从不读，`ADC_CTL_REG 0x34` 在 DT 给的窗口之外。

**判据（刷前写死）**：
* `ready=0`（或 `settled=0` 而 `ready=0`）⇒ 从没判稳；配合 `uv=375000`（val=0）⇒ 角根本没抬起来，债在
  CPR4/`vdd_gfx` 那侧，下一轮问的是 CPR4 的角寄存器（要有出处）。
* `uv` 落在 400000…925000 之间（DT `regulator-min/max-microvolt`，`sdm660-regulator.dtsi:528/:529`，
  即 val=5…110）且 `ready=1`，而 `gx_on` 仍 0 ⇒ **轨是清白的**，回头看 GDSC 自己的三个等待字段与
  `gpu_gx_domain_addr`（`dom=00140001` 至今原样报，没名字过）。
* `byp=1` 或 `pdn=1` ⇒ LDO 被旁路/掉电——这一支读出来就先去对 CPR_BYPASS 的语义，不先猜是谁写的。
* 三个词全 `0xffffffff` ⇒ 这一页没应答，整行作废（照 r282 的规矩，不当 0 票用）。

角数（厂商 `SDM660_MMSS_FUSE_CORNERS=6` 对 DT 的 1..7）本轮仍不下结论：这一行不印角号，只印电压与判稳位。

### 5.9 boot 78 把 §5.8 的两条判据一起点亮：vdd_gfx 在掉电位，参考电压从没编程过（2026-11-03）

读到的唯一一行（`beat`；`ldo post` 那拍在这靴的 537.496418 → 718.840606 s 环窗之外，r249 的周期副本第二次
顶上来，见 development-log 的 boot 78 块 ②）：

```
clover r291: gpu t=617 ldo beat vref=00000000 val=0 uv=375000 ctrl=80000038 byp=0 pdn=1 cpr_byp=0
             sts=00000000 ready=0 settled=0 cx_on=1 gx_on=0
```

1. **§5.8 的第一条与第三条同时成立。** `ready=0` 且 `uv=375000`（`val=0`）⇒ 角根本没抬；`pdn=1` ⇒ LDO 在
   掉电位。合起来的读法是：**这颗 LDO 自这次上电起既没被使能也没被编程**，`vdd_gfx` 侧从没动过。
   **「轨是清白的」这一支关闭**——注意关闭的方向：不是「轨挡住了 gx」被证实，而是「轨无罪」被否掉；
   gx 拿不到 PWR_ON 与「vdd_gfx 是 375 mV 且掉着」是同一件事的两个读数。
2. **`ctrl=0x80000038` 只命名归档里有的位。** BIT(31) LDO_CLAMP_IO（`msm_gfx_ldo.c:40`）、BIT(5) LDO_PDN
   （`:48`）、BIT(4) LDO_UNDER_SW_CTRL（`:49`），剩下 BIT(3)=0x8 **不在 `:39-49` 那十枚位里，不命名**。
   `byp=0`（BIT(6)）与 `cpr_byp=0`（BIT(30)）分别说：LDO 没被旁路、CPR 也没被旁路——所以「谁把电放掉的」
   这个问题本轮没有读数，只有 `pdn=1` 这个状态。按 §5.8 的规矩到此为止，不猜写手。
3. **同拍正对照按预期工作了。** `cx_on=1` 与 `gcc gpu_ahb=1 gpu_bimc=1` 在 `vref/ctrl/sts` 全 0 的那一行里
   照常说「这一页在应答」，所以那串 0 不是「没读到」（r282 的规矩：0 需要正对照）。三词也没出现
   `0xffffffff`，那条作废判据没触发。
4. **下一手是「对轨编程」，而它缺的不是寄存器是序列。** 寄存器三处都在 `msm_gfx_ldo.c` 里点得名：
   `PWRSWITCH_CTRL` 抬 `LDO_PDN`、判稳读 `PWRSWITCH_STATUS`（`:307`），`LDO_VREF_SET_REG` 写 `val` 之后
   抬 `UPDATE_VREF_BIT`（BIT(31)，`:35`）再落回去——厂商自己就是 `reg |= UPDATE_VREF_BIT` 后跟一句
   `reg &= ~UPDATE_VREF_BIT`（`:282/:288`，即 `configure_ldo_voltage()` `:268` / `ldo_update_voltage()` `:297`）。
   缺的是**顺序与角值来源**：`enable_ldo_mode()`（`:325`）与 `msm_gfx_ldo_voltage_enable()`（`:819`）
   这条使能全序，以及角→电压那张表来自 CPR4 fuse（`gfx_cpr` 的 `qcom,cpr-voltage-ceiling`，
   `sdm660-regulator.dtsi:579-581`，已在盘上）——而 fuse 本身在 `0x00784000`（`:536`）另一张窗口里。
   没有 sha256 出处的序列不写；这是 GPU 这一格从「读」转「写」的真正门槛。
5. **不动形状。** §5.7 那三条结论（票到了、钟没起、起不动与 gx 没合上同源）没有被本轮推翻或修改；
   本轮只把「债在轨那侧」从推断变成读数。主线（桌面）现在的瓶颈不在这格：boot 77/78 两连「静默无 panic
   复位」占了主线，见 development-log boot 78 ③④⑧。

### 5.10 更正 §5.9 第 4 条：使能全序**有出处**，缺的是厂商自己的模式判据（2026-11-03，宿主，未花开机）

§5.9 写「没有 sha256 出处的序列不写」，然后把门槛安在「顺序与角值来源」上。回到盘上那份
`notes/sources/sdm660/msm_gfx_ldo.c`（sha256 `929bc99f86f1…`，与 §5.6 同一份）逐条核对之后，这句话**两头都要改**：

1. **顺序有出处，而且名字齐全。** `enable_ldo_mode()`（`:325`）与 `switch_mode_to_ldo()`（`:498`）把每一步
   的位与注释一起给出：`BHS_UNDER_SW_CTL` BIT(0)、`LDO_UNDER_SW_CTRL_BIT` BIT(4)、`LDO_PREON_SW_OVR_BIT`
   BIT(7)、`LDO_BYPASS_BIT` BIT(6)、`PWR_SRC_SEL_BIT` BIT(9)、`ACK_SW_OVR_BIT` BIT(8)、
   `CPR_BYPASS_IN_LDO_MODE_BIT` BIT(30)（`:40-52`），抬压是 `ldo_update_voltage()`（`:297`）——
   写 `LDO_VREF_SET_REG 0x18` 的 `val` 后 `reg |= UPDATE_VREF_BIT`（BIT(31)）再落回去。
   boot 78 那行 `ctrl=80000038`（BIT(31) CLAMP_IO｜BIT(5) PDN｜BIT(4) UNDER_SW_CTRL｜BIT(3) 未命名）
   与 `pwr_src=0`，正好是厂商会走 `switch_mode_to_ldo()` 的起始形状——**清 `LDO_PDN` 那一步在
   `:537`，不在 `enable_ldo_mode()` 里**，先前把两函数混作一谈是错的。
2. **但本板不走 CORNER 那一支，所以缺的不是角→电压表。** `msm_gfx_ldo_probe()` 按 SoC 分派：
   `SDM660_SOC_ID` → `ops_type = VOLTAGE`（`:1561-1562`，`sdm660_ldo_config`），只有 MSM8953 才是 CORNER
   （`:1550-1552`）。于是 `:360/:387/:504/:645` 那四处 `ops_type == CORNER` 的 `CPR_BYPASS` 写在本板**不该照抄**，
   `gfx_vreg_corner` 的 `qcom,cpr-voltage-ceiling`（`sdm660-regulator.dtsi:579-581`）也不是编程这颗轨的入口。
3. **真正的门槛是厂商自己的模式判据。** `get_operating_mode()`（`:230-237`）不看寄存器，看三个软件量：
   `!ldo_mode_disable && ldo_fuse_enable && ldo_corner_en_map[corner]` 才返回 `LDO_MODE`，否则 `BHS_MODE`；
   `ldo_fuse_enable` 来自 `msm_gfx_ldo_efuse_init()`（`:1030`）读的那张 fuse 窗口
   （`cpr4-ctrl@05061000` 的第二段 `reg = <0x00784000 0x1000>`，`sdm660-regulator.dtsi:536/:537`）。
   **所以「要不要抬这颗 LDO」这个问题在厂商那里由 fuse 决定，而 fuse 在第四张窗口里**——这一点 §5.9 说对了，
   只是给它的理由（角值表）不对。
4. **r293 的候选因此还是读，不是写，而且比 §5.9 的「下一手」更靠前。** 厂商每次进出模式都调
   `dump_registers()`（`:253-262`），它按 `i*4` 读 **0x00…0x28 共 11 个字**（`MAX_LDO_REGS 11`，`:69`）并用 `register_str[]`（`:240-251`）
   点名——其中 **0x20 那格的名字是 `LDO_STATUS0`，本板从没印过**（r291 只读 0x18/0x1C/0x28），
   而 `LDO_STATUS1 0x24` 厂商定义了也从不判。这 11 个字全落在 DT 声明的
   `gfx_ldo_vreg reg = <0x0506e000 0x34>` 里（`:525`，11 个字由 `MAX_LDO_REGS`（`msm_gfx_ldo.c:69`）定），也就是**现有 GPUCC 映射内**（偏移 0x9000），
   不新增窗口、不动 `IODeviceMemory::withRange` 计数=3 那枚针。把它们与 `sdm660_ldo_config[]`
   （`:147-158`：ATEST 0x80、CFG0 0x0100A600、CFG1 0xA0、CFG2 0xC3FE、LD_DATA 0、VREF_TEST_CFG 0x00401100、
   REF_X1 0x230、REF_X2 0x48）并排读回来，问的是**这颗 LDO 到底被谁配置过**：
   对上 ⇒ XBL/固件编过程序，缺的只剩 fuse 的模式判据；全 0 或全 `0xffffffff` ⇒ 要么没人编，要么这页没应答
   （正对照仍按 r282：同拍 `cx_on`）。**没有这格读数之前不写任何位。**
5. **不动主线。** 桌面仍 CPU-first；这一格是并行取证线，本轮不改 `drivers/CloverGpu` 的字节。

### 5.11 更正 §5.10 第 3 条：本板根本不跑那段 fuse 代码；决定模式的是 CPR4 那三枚 fuse 行（2026-11-03，宿主，未开花机）

同一份 `msm_gfx_ldo.c`（sha256 `929bc99f86f1…`）逐层回到调用点之后，§5.10 第 3 条那句「要不要抬这颗 LDO 由
`get_operating_mode()` 里的 `ldo_fuse_enable` 决定」**在本板不成立**——不是判据选错了，是那段代码在 SDM660 上
压根不会被执行。顺带把 boot 81 那行的读法改掉，并给「下一手」换了一个更靠前、而且能一次读完的问题。

1. **fuse 那条链挂在 MSM8953 分支上。** `msm_gfx_ldo_probe()` 的分派里，`case MSM8953_SOC_ID`（`:1550-1558`）除了
   `ops_type = CORNER` 还调 `msm_gfx_ldo_corner_config_init()`（`:1553`）；`case SDM660_SOC_ID`（`:1560-1565`）只做三件事：
   挂 `sdm660_ldo_config`、置 `ops_type = VOLTAGE`、给 constraints 补 `REGULATOR_CHANGE_BYPASS`。而
   `msm_gfx_ldo_corner_config_init()`（`:1459-1494`）才是 `target_init`（`:1253`，把 `ref_volt`/`ldo_enable_param`/`init_volt_param`
   全指到 `msm8953_*` 那几张表）、`efuse_init`（`:1030`，映射 fuse 窗口）与 `voltage_init`（`:940`，`:1017` 读 fuse、
   `:1023` 写 `ldo_fuse_enable`）的**唯一调用者**。`rdesc->ops` 也在 `:1582-1585` 分岔：CORNER 用 `corner_ops`，
   本板用 `voltage_ops`——而 `get_operating_mode()` 只被 `:436`（`corner_enable`）与 `:708`（`set_corner`）读到。
   ⇒ **本板上 `ldo_fuse_enable` 恒为 0（`kzalloc` 的字段），`efuse_base` 从没映射，那张 `0x00784000` 窗口
   `msm_gfx_ldo.c` 自己一次也没读。** §5.10 给「门槛在 fuse」找的理由要收回；`msm_gfx_ldo.c` 里那段 fuse 代码不是本板的门槛。
2. **本板 mode 的真正生产者只有消费者一句。** `msm_gfx_ldo_voltage_enable()`（`:819-846`）不问 fuse，
   直接 `enable_mode = ldo_vreg->mode`；`mode == LDO_MODE` 才 `enable_ldo_mode()`（`:325`），否则 `enable_bhs_mode()`（`:373`）。
   而写 `mode` 的地方全文件只有一处：`msm_gfx_ldo_set_bypass()`（`:786-808`），即消费者调 `regulator_set_bypass()`。
   这个消费者在 DT 里点名了：`gfx_cpr` 的 `vdd-thread0-ldo-supply = <&gfx_ldo_vreg>`（`sdm660-regulator.dtsi:559`），
   而 CPR4 那份驱动把自己的初始值写成 `vreg->ldo_regulator_bypass = BHS_MODE`（`cpr4-mmss-ldo-regulator.c:513`，
   sha256 `045af7dc9c8c…`）。⇒ **出厂默认是 BHS 供电，LDO 侧那九个词全 0 与「本板走 BHS」不矛盾**，boot 81 的普查
   不能单独读成「有人本该按那张表配而没配」。**但要留一条诚实的缺口**：`enum msm_ldo_supply_mode` 的声明在
   `linux/regulator/msm-ldo-regulator.h`，这份头文件没归档 ⇒ 不能说 `BHS_MODE == 0`，只能说 CPR4 显式写成 BHS_MODE。
3. **那么 `ctrl=0x80000038` 现在能命名的比 boot 81 那句更多，也更朴素。** 按 `:39-52` 那位表拆开：BIT(31) CLAMP_IO=1、
   BIT(5) LDO_PDN=1、BIT(4) LDO_UNDER_SW_CTRL=1、BIT(3) 无名；**BIT(9) PWR_SRC_SEL=0 ⇒ 电源源选的是 BHS**，
   **BIT(1) BHS_EN_FEW 与 BIT(2) BHS_EN_REST 都=0 ⇒ BHS 这一路也没被抬**。合起来是一句更硬的读法：
   **这颗轨的两个供电级（LDO 与 BHS）在这次引导链里都停在复位状态**——这解释了 `gx_on=0`，而且不依赖那 11 个 0。
   同一枚 `0x1C` 还反过来证伪「固件配过」：`msm_gfx_ldo_init()`（`:1102`）在 probe 时**无条件**先清 CLAMP_IO、
   置 EN_LDOAP_CTRL_CPR（BIT(29)，`:1129-1132`）再写那九词（`:1135-1140`）；读到 CLAMP_IO 仍为 1、BIT(29) 仍为 0
   ⇒ **本板从 fastboot 直接进 XNU 这条链上，没有任何人跑过 gfx-ldo 的 init**（XBL/AOP 没跑，Linux 更没跑）。
   这一条把「下一手是读别人的表」换成了「下一手是我们自己按 `:1135`+`:325` 那两段配」，出处仍是同一份 sha256。
4. **决定「要不要抬 LDO」的 fuse 在 CPR4 那份文件里，而且三枚行号全点得名。** `cpr4_sdm660_mmss_read_fuse_data()`
   （`:150-215`）从 `ctrl->fuse_base` 读：`sdm660_mmss_ldo_enable_param`（`:99-106`）= **row 73 的 bit 62/61/60/59/58/57
   分别对应 6 个 fuse-corner 的「这颗角允许 LDO 模式」**；`sdm660_ldo_cpr_cl_enable_param`（`:108-111`）= **row 71 bit 38**
   （LDO 模式下允许 CPR 闭环）；`sdm660_cpr_fusing_rev_param`（`:82-85`）= **row 71 bit 34..36**（fuse combo，`CPR4_SDM660_MMSS_FUSE_COMBO_COUNT = 8`，`:59`）；
   初压与偏移在 **row 65**（`init_voltage_param :73-80` 的 bit 39..24、`offset_voltage_param :87-94` 的 bit 52..44），
   参考电压表 `sdm660_mmss_fuse_ref_volt`（`:116-123`：585000/645000/725000/790000/870000/925000）配步压 10000（`:125`）。
   「允许」的最终判据是 `ldo_mode_allowed = DT 的 qcom,cpr-corner-allow-ldo-mode && fuse->ldo_enable[i]`（`:297-326`，属性名在 `:302`），
   闭环那一路在 `:370-371`。三枚行全落在 DT 已声明的 `reg = <0x00784000 0x1000>`（名字 `fuse_base`，`sdm660-regulator.dtsi:536-537`）里。
5. **下一手仍是读，而且比 §5.10 的候选小得多：一行 8 字节 × 3 枚 row。** 读 row 65/71/73（各 8 字节，`row * BYTES_PER_FUSE_ROW`）
   并把上面那些 bit 段解出来打印，问的是**「这颗芯片出厂时被允许走 LDO 吗、允许走闭环吗、六个角的初压是多少」**——
   这正是 §5.10 想要而找不到出处的那道判据，出处现在在同一块 `0x1000` 窗口里，且 CPR4 与 gfx_ldo 两份文件都点得到名。
   **两个前提**：(a) 这需要一个新映射（`0x00784000` 不在现有 GPUCC 窗口内），`IODeviceMemory::withRange` 计数=3
   那枚针要按「DT 声明的第二段窗口（`reg-names = "fuse_base"`）」这一条理由**主动**抬到 4，不是顺手加；
   (b) 行距的问题**当天就结掉了**——`cpr3_read_fuse_param()` 的本体不在 `cpr3-regulator.c`（那份 6472 行里没有它，
   `cpr3-regulator.h:959` 那个是 `#else` 桩，直接返回 `-EPERM`），而在 **`cpr3-util.c:195`**（同仓同分支，已归档
   `notes/sources/sdm660/cpr3-util.c`，sha256 `2f3c672d0142…`）：`BYTES_PER_FUSE_ROW 8`（`:24`）、
   `readq_relaxed(fuse_base_addr + param->row * BYTES_PER_FUSE_ROW)`（`:219-220`）、取段 `(fuse_val >> bit_start) & ((1<<bits)-1)`
   并从低位往高拼接、`{0,0,0}` 结束（`:210-226`）——与 `msm_gfx_ldo.c:219` 那份逐字一致，所以**两个文件互为佐证**。
   换算也在同一份里：`cpr3_convert_open_loop_voltage_fuse()`（`:244-251`）取 MSB 作符号位，
   `uV = ref_volt + sign * steps * step_volt`，本板 `fuse_len = SDM660_MMSS_VOLTAGE_FUSE_SIZE = 5`、`step = 10000`（`:125`/`:127`）。
   于是三枚行的字节偏移是确定的：**row 65 → +0x208、row 71 → +0x238、row 73 → +0x248**（各 8 字节，全在
   `<0x00784000 0x1000>` 内）。仍**一个位也不写**。
6. **两处驱动注释比这份笔记更严，已改口。** `drivers/CloverGpu/GpuRegs.h` 与 `CloverGpu.cpp` 都写过「0x20 在那份文件里没有名字」，
   而 `register_str[]`（`:240-251`）第 9 项——索引 8 ⇒ 偏移 `8*4 = 0x20`——就叫 **`LDO_STATUS0`**（`:249`）；
   准确的说法是「**有 dump 名字、没有 `#define`**」。`0x24` 反过来：有 `#define LDO_STATUS1_REG`（`:54`）而**没有位名**，
   所以 boot 81 的 `w24=0x0000000d` 仍然只印不解释。两条都是注释/文档修正，本轮不改任何字节，不发负载。

### 5.12 更正 §5.11 第 4 条：本板 DTB 自己把 LDO 模式关了，fuse 那三枚行答的是已经答了的题（2026-11-03，宿主，未开花机）

1. **证据来自我们烧的那颗 DTB，不是厂商 dtsi 的类比。** `dtc -I dtb -O dts ~/mu-clover/Resources/DTBs/clover.dtb`
   （sha256 前缀 `815b8255`，2026-10-23 由真机 xbl 填进检出，任务 #18 那一步）里
   `cpr4-ctrl@05061000`（扁平化后第 7658 行）的 corners 子节点写着：
   `qcom,cpr-fuse-corners = <0x06>`（:7692）、
   **`qcom,cpr-corner-allow-ldo-mode = <0x00 0x00 0x00 0x00 0x00 0x00 0x00>`（:7704）**、
   **`qcom,cpr-corner-allow-closed-loop = <0x00 0x00 0x00 0x00 0x00 0x00 0x00>`（:7705）**，
   与 `sdm660-regulator.dtsi:634-637` 逐字相同——这颗板子的出厂 DTB 就是这么带的。
2. **于是 §5.11 第 4 条作废。** `cpr4_mmss_parse_ldo_mode_data()`（`cpr4-mmss-ldo-regulator.c:297-326`）做的是
   `corner[i].ldo_mode_allowed = (ldo_allowed[i] && fuse->ldo_enable[i])`（:321-322）——
   与门的 DT 那头是 0，**fuse 行 73 无论读回什么都得 false**。同一文件 :369-371 的
   `corner[i].use_open_loop = !(fuse->ldo_cpr_cl_enable && use_closed_loop[i] && corner[i].ldo_mode_allowed)`
   因此**恒真**：厂商给这颗 SoC 的 MMSS 轨既不进 LDO 模式、也不进闭环。
   另有一枚**节点级**开关别混：`:511 vreg->ldo_mode_allowed = !of_property_read_bool(of_node, "qcom,ldo-disable")`
   是整节点的一个布尔，与上面那枚 per-corner 数组不是同一个量，本板 DTB 里没有 `qcom,ldo-disable`。
3. **r298 原计划（读 row 65/71/73、为此新增第四枚 `IODeviceMemory::withRange`）因此取消**：它问的那道题
   已经由出厂 DT 答了，而且读回什么都不改变结论。任务 #549 按这条改向，`drivers/CloverGpu/GpuRegs.h`
   里为那一支预备的 `kCpr4Fuse*` 常量已写又已撤（不留死常量替一个错前提作证）。**一个位也没写过。**
4. **boot 81 的 LDO 普查从「故障」改判成「符合配置」。** `ctrl=0x80000038` 两级都在复位值、
   `:513 vreg->ldo_regulator_bypass = BHS_MODE` 是 CPR4 的初值——在厂商自己的 SDM660 配置下这颗 LDO
   本来就不该被抬进 LDO 模式。boot 78 的 `uv=375000 sts=0 pdn=1`（§5.9 那格）现在说的是
   **「vdd_gfx 这颗 LDO 不是这块板的供电路径」**，不是「供电路径没抬起来」。
   §5.9 的结论「轨清白」因此更站得住了，但理由换了：不是我们没读到，是它不该有读数。
5. **真正喂 GPU 的是哪一路，DT 写得直白**（`sdm660-regulator.dtsi:555-559`，同一份内容要按第 1 条的办法
   在 `clover.dtb` 里复核一遍再进结论）：`vdd-supply = <&gfx_stub_vreg>` 是 **stub regulator**
   （`:500-505`，`compatible = "qcom,stub-regulator"`，min 400000/max 1070000，没有硬件寄存器）、
   `mem-acc-supply = <&gfx_mem_acc_vreg>`、`system-supply = <&pm660l_s3_level> /* vdd_cx */`。
   所以电压这一格真正的下一枚读数是 **pm660l 的 S3**，而它是
   `compatible = "qcom,rpm-smd-regulator"` + `qcom,use-voltage-level` + `qcom,set = <3>`（`:55-63`）——
   **电平是发给 RPM 的 SMD 请求**，不是 CloverPower 现在这套 SPMI 直读 LDO 寄存器问得出来的东西
   （那两条是 pm660 pm8998 侧的 `ldo-a11-vcc_i2c`、pm660l 的 `ldo-b3-vdd`，`CloverPower.cpp:624-626`）。
6. **下一支按「不需要 RPM 也能读」排。** 先读 `gfx_mem_acc_vreg` 那颗**单字 MEM_ACC 寄存器**：
   `reg = <0x01fcf004 0x4>`、`reg-names = "acc-sel-l1"`、`qcom,corner-acc-map = <0x1 0x0>`
   （`sdm660-regulator.dtsi:510-518`）——纯 MMIO、4 字节、与 GCC/GPUCC 同法映射，读回来的是
   「L1 电源域的 mem-acc 角选择停在哪儿」，与 GPU 是否拿得到 vdd_cx 是同一张问题的另一头。
   RPM-SMD 那一格（S3 电平）排在它后面，并且要写明：**我们的引导链从未发过这种请求**。仍只读。

### 5.13 r311 发出去的那一格：第四扇窗口、一个位也没写（2026-11-04，宿主，未花开机）

1. **「与 GCC/GPUCC 同法映射」这句要按算术读。** §5.12⑥ 说的是同一个 `IODeviceMemory::withRange`
   写法，不是同一扇窗口：`0x01fcf004` 在 GCC 的 `0x00100000+0x94000`（到 `0x0194000`）之上，
   也在 GPUCC（`0x05065000`）与核（`0x05000000`）之下，**r286 起那三扇没有任何一扇够得着它**，
   所以 r311 是本线第一次向 IOKit 要第四张映射。`check-clover.py` 里那条「只许三扇」的断言因此
   改成「只许四扇」，并把理由换掉——旧文案说的「第四扇＝同一页重复映射」只对 LDO 那一支成立
   （`0x0506e000` 在 GPUCC 的 `0x10000` 里，r291 才走已有映射）。
2. **这颗节点在出厂 DTB 里是逐字这样带的**（`dtc -I dtb -O dts ~/mu-clover/Resources/DTBs/clover.dtb`，
   sha256 前缀 `815b8255`，第 7633-7645 行；§5.12 第 1 条要求的复核办法）：
   `regulator@01fcf004` → `compatible = "qcom,mem-acc-regulator"`、`reg = <0x1fcf004 0x4>`、
   `reg-names = "acc-sel-l1"`、`regulator-name = "gfx_mem_acc_corner"`、
   `regulator-min-microvolt = <0x01>`、`regulator-max-microvolt = <0x02>`、
   `qcom,corner-acc-map = <0x01 0x00>`、`qcom,acc-sel-l1-bit-pos = <0x00>`、
   `qcom,acc-sel-l1-bit-size = <0x01>`、`phandle = <0x163>`；消费方是同一棵树里的
   `cpr4-ctrl@05061000` 的 `mem-acc-supply = <0x163>`。**全树只有这一颗 `qcom,mem-acc-regulator`**。
   节点**没有 `clocks` 也没有 `vdd-supply`**——这就是为什么读它可以不做 r282 那一赌：
   没有任何一条记录说这页需要谁的钟先起；而核窗口（`0x05000000`）的同一句话是有前提的。
3. **这一格读的不是电压。** `min/max-microvolt` 在这颗 compatible 上是**角号**（所以是 1 和 2，
   不是 400000/925000），`corner-acc-map` 是两个角各自要的字段值。所以行里给的是
   `raw=`（整字）、`l1=`（按 DT 自己的 pos/size 切出的字段）、`corner=`（1 或 2，用树自己的映射对回去），
   **不声明这两个角在伏特上是多少**——那颗节点没有电压表，能说出电平的那条路是 §5.12⑤ 的 RPM。
   判读之前有一枚必须先过的闸：`answered=`。`0xffffffff` 是本板「页没答」的形状（r282/r286 拿它当
   GPUCC 的门禁用的就是这一条），而 **`bit-size=1` 将 `0xffffffff` 切出来正是 1，也就是角 1 自己要的值**——
   先 mask 后判断会把一扇死页报成「角 1 已就位」。这就是第十五层那枚「一个字不许两义」在这一格的样子，
   代码里钉成 `if (raw != kMemAccNoAnswer)`，两个 checker 各钉一处。
4. **行有三种形状，缺行与形状不同是一回事还是两回事，要说死。** `map=0 phys= size=`＝IOKit 拒了这段范围；
   `… answered= raw= l1= corner= gpucc=0`＝读到了，但对照位所属的那扇 GPUCC 这一拍还没开（首拍必是这个形状，
   因为 GPUCC 是在 `clover_gpu_power_on()` 里、GCC 那票验过之后才映射的）；完整形状才带 `cx_on= gx_on=`。
   三种都要出声，因为「这靴没有 mem_acc 行」只能说明 GPU 定时器没拍，说明不了这页死不活——这是第十三层
   （镜像里有这行≠本靴有读数）在这一格的落法，也是它进 `CloverSdhci` 独立 tee 类 `r311-gpu`（32）并被
   免掉每拍上限的理由。
5. **本格的仪器地平线（写明，别留给下一轮猜）。** `bit-size=1` 时字段只能是 0 或 1，两个值都在映射里，
   所以 `corner=` 永远不会落在「映射外」；剩下的分辨力只有 `raw` 整字。若某靴读到 `raw=00000000`，
   这一格**分不开**「角 2 的真值」与「整页读零」：本板已知的非应答形状是全 1，而同一 4 KiB 页里另一个
   DT 节点 `qpdi@1fc1000`（`qcom,coresight-qpdi`，`clocks` 指两颗没查过的源）**没被任何记录证明带钟**，
   拿它当对照就是 r282 拒掉的那种赌。因此 `raw=00000000` 的下一步不是再读一遍，而是找一个有出处的同页字——
   在本板目前不存在。
6. **r311 之后剩下的两格，按「不需要 RPM」排到「需要 RPM」。** 已发出的这一格答完，MEM_ACC 这条腿就没有
   可离线推进的了；下一格是 §5.12⑤ 的 **pm660l_s3_level 电平**，它要的是 RPM SMD 请求的读法，
   而我们的引导链从未发过这种请求——**这一格要先在宿主上钉住「读一个已发的 level」到底读哪儿**
   （`qcom,set = <3>` 的语义与 RPM 侧的镜像），不是先写驱动。GPU 主线仍按 CPU-first：本线只读不写。

7. **这一格开花了（boot 104，2026-11-04 21:40 手动回 TWRP 读回）。** 卡上原文两拍同形：
   `clover r311: gpu t=621 fire=5 mem_acc map=1 answered=1 raw=00000000 l1=0 corner=2 gpucc=1 cx_on=1 gx_on=0`
   与 `t=744 fire=6`。三件同时成立：`map=1`（第四扇窗口映射成）、`answered=1`（`0x01fcf004` **应读**，本板已知的
   非应答全 1 形状在这里没出现）、`raw=00000000`。第 5 条预告的那一支正是这一支：**这一格分不开
   「角 2 的真值是零」与「整页读零」**，因为 `bit-size=1` 裁出来的 `l1=0` 与整字 `raw=0` 是同一枚数的两种裁法，
   而本板至今没有一枚有出处的同页字当对照。所以读数到此为止，不许写成「MEM_ACC 在零档」。
   产物在盘：`/Users/zhouzhou/clover-out/boot104-logdump.bin` `87001412521657ec…`（623,104 B）、
   解码 `boot104-tee.txt` `13b0ef0f6b9063c3…`；同帧的 r317 判形 `shape=list-never-written comparable=8 agree=1
   differ=7 dead=0 map_ok=1 control=1` 见 dev-log §Round318 ⑬。下一格仍按第 6 条：先钉「读一个已发的 level 读哪儿」，
   不是先写驱动。


## 6. r389：GFX LDO 上电序列已实现（2026-11-11，等 boot 173 读数）

按 §5.9 的结论（LDO 自上次上电起未编程、`pdn=1`、`vref=0`），`drivers/CloverGpu/CloverGpu.cpp` 新增
`clover_gpu_ldo_bringup()`：按 `notes/sources/sdm660/msm_gfx_ldo.c` 的厂商顺序走 `configure_ldo_voltage()`
→ `enable_ldo_mode()`（:325）→ `switch_mode_to_ldo()`（:498），电压取 DT 下限 400 mV（`val=5`），
`PWRSWITCH_STATUS` 的 ready/settled 各等 50×10 µs。行：

```
clover r389: gpu t=%u ldo prog uv=400000 val=5 ready=%u vref=%08x ctrl=%08x sts=%08x
```

随后照旧开 CX/GX GDSC。**预登记判据**：`gx_on` 从 0 翻 1 且同拍 `cx_on=1`（正对照）⇒ 轨这一格结掉；
若 `ready=0` 或 `gx_on` 仍 0 ⇒ 序列/角值来源有问题，回到 CPR4 fuse（`0x00784000`）那条线；
另记「这一靴是否活到 1800 s 死线」——r290 的 rbcpr/rbbmtimer 两票曾与连续早死相关，LDO 写也按同一
怀疑标准记录。


## 7. r408/r409：电压能写、CTRL 不接受 AP 写（2026-11-12 上机）

- **boot 181（只写 VREF）**：`r291 … vref=00000005 val=5 uv=400000 … ran=1 vfa=00000005 cfa=80000038`，
  板子**自己落回 TWRP**（不挂死）⇒ 电压寄存器可写、写后回读生效；
- **boot 182（逐位写 CTRL，每步先打 `r409 ctl step=`）**：`cfa=80000038` 与写前**完全相同**，
  也不挂死 ⇒ **CTRL（PWRSWITCH_CTRL）不接受 AP 写**；
- 因此「之前整段序列挂死」不是 CTRL 位的问题，而是那一版的顺序/等待；而这颗 GFX 轨的
  `PDN` 状态由 **CPR/固件侧**持有，AP 翻不动。
- 下一步只能从固件侧（`uefi-clover` 交接前把 GPU 轨/GX 打开）或 CPR4 角编程（需要新源码）进入。


## 8. sheng（SM8550）的 GPU 电源与 clover 的结构差异（2026-11-12，读 UniXNU-sm8550-common）

`UniXNU-sm8550-common` 已拉到本地（`~/UniXNU-sm8550-common`）。它的 GPU 相关代码只有：

- `kits/Sm8550Platform/ShengGmu.c`、`ShengGmuCommandDb.c`、`ShengGmuVotes.h` —— **GMU 固件与票**；
- `kits/Sm8550Platform/ShengGpuSmmu.c` —— GPU 的 SMMU 上下文。

**没有**任何 GFX 轨 / CPR / LDO 的编程代码。原因不是遗漏，是结构：SM8550 的 GPU 有 **GMU**，
轨与频率由 GMU/RPMh 侧管，AP 只负责把 GMU 固件装起来并投票（`sheng-gpu-power=9` 就是这个策略的开关）。

clover 的 Adreno 512 **没有 GMU**（`notes/gpu-bringup.md` §5 早已记录），所以 sheng 这条路在 clover 上
没有对应物：AP 必须自己开轨，而我们的实测（§7）说 CTRL/PDN 不接受 AP 写。结论因此是：

- sheng 的做法**不适用于** clover；
- clover 的轨只能由**固件侧**（`uefi-clover` 交接前）或 CPR4 角编程打开。


## 9. 下一步实验（已从现有读数缩小到一次写入）

§7 说 CTRL 写被忽略。r294 行本来就把 LDO 的十三个字打上卡了，boot 173/180/181 的都一样：
除 `w1c=80000038`（PWRSWITCH_CTRL）与 `w24=0000000d`（LDO_STATUS1）外全为 0 —— 于是
`VREF_TEST_CFG`（w14，偏移 0x9014）里的 `ENABLE_LDO_STATUS_BIT`（BIT8|BIT12）与 `AUTOBYPASS`
（BIT20）**当前都是 0**。

一次写入实验足以判定：把 `w14` 的 BIT8|BIT12 置位（只写这一个字，不碰 CTRL），回读确认后
再写一次 CTRL：

- 若 CTRL 这次能写进去（`cfa` 变化）⇒ 「CTRL 被忽略」的原因是状态使能未开，轨可由此路径打开；
- 若 CTRL 仍不变 ⇒ 被忽略与 w14 无关，回到 RPM/CPR 那条线。

写 w14 是这颗块里唯一还没试过的「使能」位，且 r294 的基线已证明它当前为 0，所以这一次写有明确的前后对照。
