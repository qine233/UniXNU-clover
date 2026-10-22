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
board-id          <11 0>  (MTP 基座；真机 merged dtb 叠了 subtype 1，见第 5 节)
DRAM base         0x80000000            (lavender 的 UEFI 也是这个)
splash / FB       0x9d500000 + 0x2400000 (36 MiB)   mainline 自用 fb 在 0x9d400000；真机固件是 0x9d400000，port 用它（第 5 节）
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

## 5. 真机 dump 校正了设备树的三处

`scripts/dump-clover-facts.sh` 跑完后（`facts/<serial>-<stamp>/`），三处设备树推断被真机推翻或补齐：

- **splash 地址**：真机 xbl 里的 `uefiplat.cfg`（`fw/xbl-uefiplat.txt`）把 "Display Reserved" 定在
  **`0x9d400000 + 0x02400000`**，也就是说 stock xbl 开机时已经把 splash 画在这段窗口里，UEFI 接手时
  framebuffer 只有这个地址能用。`clover-overlay.dtsi` 的 `0x9d500000` 是给「内核自己重画 splash」
  预留的，非 overlay 的基座设备树本来就是 `0x9d400000`。结论：**固件赢，port 用 `0x9d400000`**。
- **DRAM 分 bank**：`dt/memory.raw.txt` 是两个 bank —— `0x80000000 + 0x80000000` 和
  `0x100000000 + 0x7e4c0000`（MemTotal 3825460 kB，确实是 4 GB 版）。之前笔记里「高 bank 在
  `0x1800000000`」是错的。`/chosen` 只带一组 dram-base/dram-size，所以 ADT 仍只描述低 2 GiB。
- **board-id**：真机 merged dtb 是 `qcom,board-id = <0x0001000b 0x0>`（低 16 位 0x000b = MTP 平台号
  11，高 16 位 subtype 1），`qcom,msm-id` 就是 `<317 0>`。

同一份 `uefiplat.cfg` 还给了 `[RegisterMap]` 33 行和 `[ConfigParameters]` 33 个键，
`Library/MemoryMapLib` 与 `Library/ConfigurationMapLib` 现在都是它的逐行照抄，不再是从
lavender 推断。`NumCpus = 4 / NumActiveCores = 8`（簇数/核数）与 `proc/cpuinfo.txt` 的
4×CPU part 0x801 + 4×0x800 一致。

**UEFI blob 的来源也不是 `uefi` 分区**：clover 的 GPT 里没有 `uefi`/`uefi_a`，SEC 直接 gunzip
`xbl`（mmcblk0p1）里偏移 `0xa89b0` 的 gzip 流，得到 `FvLength = 0x459040` 的固件卷，
83 个 FFS 文件、全是 AArch64 PE32，模块 GUID 与上游 lavender 的 Device-Binaries 逐个对得上。
`../uefi-clover/scripts/extract-clover-blobs.py --xbl <file>` 就是按这个流程把
`Binaries/clover` 填出来的。

## 6. 判断：能适配的部分 vs 仍然卡住的部分

能直接落地：ADT（dram-base/dram-size/display-base/reserved-memory/msm-id/board-id/cpu-regs）、
`clover.dts` 与 splash_region、MemoryMapLib、UEFI framebuffer 1200x1920、boot.img 打包参数（多数可由 header v1 + append_dtb 推定）。

仍需真机（`scripts/dump-clover-facts.sh`）：
- ~~`qcom,board-id` 在真机 DTB 上的实际 subtype~~ → 已确认，见第 5 节
- ~~userdata GPT 的 first/last LBA 与 logical_block_size~~ → `0xf10520` / `0x747bfde`，512 字节扇区，已写进 `board.toml`
- ~~真机 DTB 里的 `/memory`、`/chosen`、ABL 实际使用的 display 区域~~ → 见第 5 节
- ~~stock `boot` 分区头里 kernel/ramdisk 加载地址那一组字段的排布~~ → 见第 7 节，已经解出来并写进 `board.toml [bootimg.image]`

结构性缺口（跟设备树无关，别抱期待）：
- XNU 没有原生 eMMC/SDHCI 驱动（`NativeUfs` 才有），所以拿 sheng 那种「userdata 里直接挂 APFS」的路子走不通，
  得走 gta4l（同为 eMMC）那条：内核 config 里没有块设备驱动，root 由固件/ramdisk 侧提供
- `NativeAdreno` 只支持 Adreno 740，Adreno 512 无驱动 → 目标是 UEFI 菜单 → XNU 控制台 / CPU 渲染
- 该平板 USB 只有 device 模式（`dr_mode = "peripheral"`），UniXNU 菜单的交互要靠串口或触摸
- 触摸是 ft5406，UniXNU 没有对应驱动

## 7. stock boot 头：不需要外部镜像校准，两个分区自己就能证明（2026-10-22）

上次说「`0x0c/0x10/0x14` 与文档化的 v1 布局差一格」是我按 v1 去读 v0 的头。dump 里
`img/unixnu-boot.img`、`img/unixnu-recovery.img` 就是这台机器的 boot / recovery 整分区
（`unixnu-` 只是脚本的命名前缀），按 v0 布局读，两个镜像逐字段一致：

| 偏移 | 字段 | boot | recovery |
| --- | --- | --- | --- |
| 0x00 | magic | `ANDROID!` | 同 |
| 0x08 | kernel_size | `0x2fd1ac0` | `0xd02c1d` |
| 0x0c | kernel_addr | `0x8000` | `0x8000` |
| 0x10 | ramdisk_size | `0x140cb5` | `0x16cf38d` |
| 0x14 | ramdisk_addr | `0x1000000` | `0x1000000` |
| 0x18/0x1c | second | 0 / 0 | 0 / 0 |
| 0x20 | tags_addr | `0x100` | `0x100` |
| 0x24 | page_size | `0x1000` | `0x1000` |
| 0x28 | header_version | **0** | **0** |

三个「地址」其实是相对 DRAM 基址的偏移，`0x80000000` 由 ABL 自己加 —— 这正是它看着不像
物理地址的原因，也是 `board.toml` 里 `base` 必须保持 0 的原因（写进去就翻两倍）。

不靠读表、能独立自证的两条：

- **page size**：kernel 段从 `0x1000` 起，ramdisk 恰好落在 `0x1000 + align(kernel_size, 0x1000)`
  （boot 是 `0x2fd3000`，recovery 是 `0x0d04000`），两处实测都是 `1f 8b` gzip 头。
- **append_dtb**：内核段里 gzip 流结束的下一个字节就是 `d00dfeed`。boot 里是 **111 个 DTB 串联**
  （同一套镜像要覆盖 8 吋/10.1"、Wi-Fi/LTE），recovery 里只有 1 个 —— 后者就是我们打包的形状，
  ABL 用 `qcom,msm-id` + `qcom,board-id` 在这堆里挑。
- 两个镜像都没有 AVB 尾（分区末尾 8 KiB 内无 `AVB0`），头里的 `id` 全零，所以这条链上
  boot 镜像自身不带签名。

结论：`header_version` 从 lavender 的 1 改成本机的 0，`ramdisk_offset`/`tags_offset` 丢掉 gta4l
的 `0x20000000`/`0x01e00000`。`uefi-clover/scripts/check-clover-uefi.py` 现在拿这组实测值当基准，
board.toml 写错会直接报错。
