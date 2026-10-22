# clover 设备树来源调研（2026-10-22）

结论先说：**能适配**。GitHub 上存在与这台机器完全对应的设备树，`clover` 的绝大部分硬件常量不必等真机 dump 就能确定。

## 1. 主线（Mainline）设备树 —— 精确到 8 吋 LTE 版

仓库 `sdm660-mainline/linux`，分支 `qcom-sdm660-7.2.y`，路径 `arch/arm64/boot/dts/qcom/`
（SDM660 尚未进 Linus 主线，这套 dts 活在这个 fork 里；postmarketOS 的 lavender 端口用的就是它。）

| 文件 | 内容 |
| --- | --- |
| `sdm660-xiaomi-clover.dts` | `model = "Xiaomi Mi Pad 4 LTE"`，`compatible = "xiaomi,clover-lte", "qcom,sdm660"` —— 社区把 8 吋家族叫 clover，`-plus` 才是 10.1" |
| `sdm660-xiaomi-clover-regular-common.dtsi` | 屏 `boe,tv080wum-nx2` / `novatek,nt51021`，4 data lane；触摸 `edt,edt-ft5406` @ `blsp_i2c3` 0x38，reset gpio66、int gpio67；背光是 `pm660l_wled`（LED 驱动，不是 PWM）；电池 6010 mAh |
| `sdm660-xiaomi-clover-common.dtsi` | framebuffer + reserved-memory + eMMC/USB/音频，数值见下 |
| `sdm660.dtsi` → `sdm630.dtsi` | Kryo 260 八核、`qcom,adreno-512.0`、`qcom,sdm660-mdp5`、`qcom,dsi-phy-14nm-660` |

## 2. 高通下游（Xiaomi CAF 4.4/4.19，一个镜像同时管 8 吋和 10.1"）

仓库 `E7G/clover-sdm660-ksb`（等价内容见 `PixelExperience-Devices/kernel_xiaomi_clover`），
路径 `arch/arm64/boot/dts/vendor/qcom/`

- `sdm660.dtsi`：`qcom,msm-id = <317 0x0>`（317 = 0x13D = SDM660），GICD `0x17a00000/0x10000`、GICR `0x17b00000/0x100000`、stride `0x20000`，`sdhc_1: sdhci@c0c4000`（hc_mem 0xc0c4000/0x1000、cqhci_mem 0xc0c5000/0x1000、cqhci_ice 0xc0c8000/0x8000）
- `sdm660-mtp-clover.dts`：`qcom,board-id = <11 0>`，PMIC 组合 pm660 + pm660l
- `xiaomi/mi/clover/clover-overlay.dtsi`：**`cont_splash_mem reg = <0x0 0x9d500000 0x0 0x02400000>`**
- `dsi-panel-nt51021-boe-video.dtsi`：1200x1920、物理 107x172 mm —— 确认是 8 吋屏

## 3. 提炼出来的 porting 数值

```
msm-id / SoC      317 (0x13D), SDM660
board-id          <11 0>  (MTP 基座；真机 DTBO 可能再叠 subtype，待 dump 确认)
DRAM base         0x80000000            (lavender 的 UEFI 也是这个)
splash / FB       0x9d500000 + 0x2400000 (36 MiB)   mainline 自用 fb 在 0x9d400000
分辨率            1200 x 1920, stride 4800, a8r8g8b8
GIC               dist 0x17a00000 / redist 0x17b00000, stride 0x20000
eMMC              sdhc_1 @ 0xc0c4000 (hc 0xc0c4000/0x1000, cqhci 0xc0c5000/0x1000, ICE 0xc0c8000/0x8000), CQE + HS400ES, 8-bit
pstore/ramoops    0x9fe00000 + 0x100000
adsp/mba/cdsp     0x92a00000+0x1f00000 / 0x94900000+0x200000 / 0x94b00000+0x600000
venus / gpu-zap   0x9cc00000+0x800000 / 0xf0b00000+0x2000
时钟              19.2 MHz；cpu reg 0x0-0x3、0x100-0x103
串口             blsp1_uart2 115200（stdout-path）
```

## 4. UEFI 侧的可复用证据

Mu-Silicium 的 `lavender`（Redmi Note 7，同 SDM660/StarlordPkg）已跑通，DXE 列表里同时有
`QcomPkg/Drivers/SdccDxe`（SDCC = 高通 eMMC 主机）和 `UFSDxe` —— 说明固件层驱动这台平板的 eMMC 是现成的。
内存图：DDR `0x80000000`、FD `0x9FC00000/0x300000`、boot header v1、append_dtb、psci smc。

## 5. 判断：能适配的部分 vs 仍然卡住的部分

能直接落地：ADT（dram-base/dram-size/display-base/reserved-memory/msm-id/board-id/cpu-regs）、
`clover.dts` 与 splash_region、MemoryMapLib、UEFI framebuffer 1920x1200、boot.img 打包参数（多数可由 header v1 + append_dtb 推定）。

仍需真机（`scripts/dump-clover-facts.sh`）：
- `qcom,board-id` 在真机 DTB 上的实际 subtype（要 `dtc` 解 dtb/dtbo）
- userdata GPT 的 first/last LBA 与 logical_block_size（board.toml `[userdata]` 必需，只能从机器读）
- 真机 DTB 里的 `/memory`、`/chosen`、ABL 实际使用的 display 区域
- 该机的 DDR 总量在 bootloader 视角的可用来跑 XNU 的量（4 GB 版）

结构性缺口（跟设备树无关，别抱期待）：
- XNU 没有原生 eMMC/SDHCI 驱动（`NativeUfs` 才有），所以拿 sheng 那种「userdata 里直接挂 APFS」的路子走不通，
  得走 gta4l（同为 eMMC）那条：内核 config 里没有块设备驱动，root 由固件/ramdisk 侧提供
- `NativeAdreno` 只支持 Adreno 740，Adreno 512 无驱动 → 目标是 UEFI 菜单 → XNU 控制台 / CPU 渲染
- 该平板 USB 只有 device 模式（`dr_mode = "peripheral"`），UniXNU 菜单的交互要靠串口或触摸
- 触摸是 ft5406，UniXNU 没有对应驱动
