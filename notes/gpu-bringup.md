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
