# Adreno 512 驱动实现计划（r390 起，源码已归档）

本文件是「写驱动」这条线的施工图。所有引用都指向 `notes/sources/sdm660/` 里已归档的厂商源码
（`LineageOS/android_kernel_xiaomi_sdm660` @ `lineage-20`，sha256 见下表），不再依赖外部链接。

| 文件 | sha256 前 12 | 本轮用途 |
| --- | --- | --- |
| `adreno_a5xx.c` | `81e8a5250273` | a5xx_start、ME_INIT、微码地址、IRQ |
| `adreno_a5xx.h` | `1bcc4a0df94a` | a5xx 设备结构、feature 位 |
| `adreno_a5xx_packets.h` | `81c484a5b45a` | PM4 包构造（CP_*） |
| `adreno_pm4types.h` | `ff15d0954443` | type4 包宏 |
| `adreno_ringbuffer.c/.h` | `9db0193b17ed` / `ab254c71e252` | ring、IB、fence |
| `adreno_iommu.c` | `35f75df7ce56` | SMMU 上下文 |
| `adreno.h` | `183c3c8d1d5c` | adreno 设备/固件模型 |
| `adreno_a5xx_preempt.c` | `637ca60c9021` | 抢占（暂不实现） |

## 里程碑

1. **上电（已做 r389 的 LDO；r390 待验）**：LDO → CX/GX GDSC → gfx3d 分支；`gx_on=1` 后读
   `kGpuPhys+0x10200`（`GpuRegs.h:210`）与 `0x05010200` 比对。
2. **微码**：`ADRENO_FW_PFP`/`ADRENO_FW_PM4` 两个 blob，`request_firmware` 的命名来自 a5xx core 描述
   （`adreno_a5xx.h` + `adreno-gpulist.h` 的 `a5xx_core`）。XNU 侧没有 firmware loader：blob 走 payload
   的 `--firmware` 输入（下一步给 UniXNU build payload 加一个 firmware 目录参数），由驱动在 start 时读入
   连续内存。
3. **ring**：`adreno_ringbuffer.c` 的 `adreno_ringbuffer_allocspace`/`submit`/`spin` 是模板；
   XNU 侧用 IOBufferMemoryDescriptor 分配 ring + IB，`CP_RB_BASE/BASE_HI/RPTR/WPTR` 写 GPU 核寄存器。
4. **ME_INIT**：`adreno_a5xx.c:1793-1930` 的序列（`CP_ME_INIT` 0x3b 包、PFB/PM4 地址、工作区）
   逐字段照抄；`a5xx_send_me_init()` 是入口。
5. **SMMU**：`adreno_iommu.c` + 已有的 `ArmSmmu4K` kit；GPU 页表用 4K 页 + TTBR0。
6. **提交**：IB + fence（`CP_EVENT_WRITE` 带 timestamp），IRQ 走 GPU 的 `A5XX_RBBM_INT_*`。
7. **用户态**：Metal 需要 `IOGPUDeviceUserClient`（`IOGPUFamily` kext）——最后一步，且依赖 1-6。

## 本轮的施工顺序（无板子也能做）

- 先把 2 的 blob 格式定下来：`adreno.c` 的 `adreno_load_fw()` 读 `struct firmware`（version/date/microcode），
  我们把它写成 host-testable 的解析器（`GpuCore.h` + `scripts/test-clover-gpu.py`）；
- 再把 3 的 ring 几何与 PM4 包头写成纯函数（type4/type7、包长、`CP_RB_*` 偏移），同样 host 测试；
- 4 的 ME_INIT 序列在一个函数里逐行对应 `adreno_a5xx.c` 的行号注释；
- 这些都不碰硬件；等板子回来只需要把它们接到 CloverGpu 的 MMIO 层。


## r390–r394 的落地情况

| 层 | 状态 | 位置 |
| --- | --- | --- |
| 微码 blob 解析 / PM4 头 / type4 拆分 | 已实现 + host 测试 | `GpuCore.h`、`tests/clover_gpu_host.cpp` |
| CP ring 几何 / RB_CNTL / 指针回绕 | 已实现 + host 测试 | `GpuCore.h`、`GpuRegs.h` |
| ME_INIT 9 dwords | 已实现 + host 测试 | `GpuCore.h` |
| 微码嵌入 | 生成器 + 空默认头 | `scripts/make-clover-gpu-ucode.py`、`drivers/CloverGpu/ucode_a530.h` |
| CP 启动（ring/微码基址/ME_HALT/ME_INIT/RPTR 轮询） | 已实现，编译通过；无 blob 时打 `cp start skipped ucode=absent` | `CloverGpu.cpp` |

**还缺（按依赖顺序）**：

1. **微码文件**：从设备 `/vendor/firmware/a530_pfp.fw`、`a530_pm4.fw`（TWRP adb 可 `cat`），跑
   `scripts/make-clover-gpu-ucode.py` 覆盖 `ucode_a530.h` —— 之后 CP 启动路径即完整；
2. **SMMU**：`adreno_iommu.c` + `ArmSmmu4K` kit；在 CP 起来之前用物理地址是可行的（当前实现），
   页表化是让 IOSurface/IB 能被 GPU 读的前提；
3. **提交/fence/IRQ**：`adreno_ringbuffer.c` 的 submit + `CP_EVENT_WRITE` timestamp + `A5XX_RBBM_INT_*`；
4. **Metal 用户态**：`IOGPUDeviceUserClient`（`IOGPUFamily` kext）——最后一步。


## SMMU 集成（下一步，依赖 `ArmSmmu4K` kit）

板上事实（notes/gpu-bringup.md §1）：SMMUv2 在 `0x05040000 + 0x10000`，3 级页表；上下文 bank 0 = user
（gpu-offset `0x48000`），bank 2 = secure；中断 `GIC_SPI 329/330/331`。

1. 在 `board.toml` 的组件里加 `ArmSmmu4K`（它与 `Adreno740` 同源，sheng 的 GPU 也依赖它）；
2. CloverGpu 分配一段连续页作页表池，用 `ArmSmmu4KBuild()`（`kits/ArmSmmu4K/ArmSmmu4K.h`）为
   ring / RPTR / ucode / IB / memstore 建 IOVA 映射；它的约束写在头文件里：只建 4K 叶子、排序不重叠、
   `mair` 由调用者用返回的 `result.mair`（NC 在 index 0、WB/WA 在 index 1）；
3. 用 MMIO 写 SMMU 全局（SCTLR/TTBR0/TCR/MAIR）与 bank 0 的 CBAR/SCTLR/TTBR0/TCR/MAIR，使能翻译，
   然后 `CP_RB_BASE` 等寄存器改用 IOVA；
4. IRQ：GIC_SPI 329（global）/330（context）/331（secure），先只要 330；
5. 验收：CP 启动时 `CP_RB_RPTR` 前进且 SMMU 无 fault（`SMMU_CB_FSR` 读 0），再把地址从物理换成 IOVA。


### SMMU 的具体接入点（`ArmSmmu4K` 的 API 已核对）

kit 把「建表」和「编上下文寄存器」都做成了纯函数，正好是我们缺的两块：

1. `ArmSmmu4KBuild()`（`ArmSmmu4K.h`）→ 建 4K 叶子页表，返回 `ArmSmmu4KResult`（含 `mair`、`start_level`）；
2. `ArmSmmu4KEncodeContext()`（`ArmSmmu4KContext.h`）→ 把 `ArmSmmu4KContextConfig`（lower/upper 表 +
   `id0/id2` + `asid` + `coherent_walk` + `stall_on_fault`）+ `ArmSmmu4KContextImage`
   （`cbar/cba2r/tcr/tcr2/mair0/1/sctlr/ttbr0/1`）编成寄存器值；
3. `ArmSmmu4KBindMmio()` + `ArmSmmu4KProgram*` 负责真正写 bank（含 poll budget）；
4. `ArmSmmu4KCompareContext` 可把读出与期望逐字段比对——**上机验收**就用它。

接入顺序：`[components].include` 加 `ArmSmmu4K` → 页表池（连续 4K）→ `Build` → `EncodeContext`
→ `Program`；然后 `CP_RB_BASE` 等改用 IOVA。验收判据仍是「`CP_RB_RPTR` 前进且 `CB_FSR` 为 0」。


## 与官方 GPU 文档对照（unixnu.com/zh/porting/gpu，2026-11-12）

官方页**通篇只讲 Adreno740**，运行路径是
`Metal → AdrenoMetal → metal2vulkan → Turnip → Darwin KMD → Adreno740 → A740`，并明确写：

> 其他 GPU 代际需要先实现**对应内核后端和编译器支持**，再选择前端。

对 clover（Adreno 512，a5xx）逐项核对：

| 官方步骤 | clover 的对应 | 结论 |
| --- | --- | --- |
| 1. 匹配代际与 OS ABI | A740 接口；a5xx 无此接口 | **不匹配** |
| 2. 电源与固件（GMU） | a5xx 无 GMU，靠 CPR/LDO；实测 CTRL 不可写 | **无对应物**（§7–§10 已测） |
| 3. 内存与提交（SMMU/ring/fence） | CloverGpu 已有 CP ring/ME_INIT/SMMU 识别，但缺轨 | 卡在轨 |
| 4. 构建 Metal（Turnip/AdrenoMetal） | Turnip 是 a6xx+；无 a5xx Vulkan 后端，更无 a5xx Metal 前端 | **不存在** |

结论：**官方 GPU/Metal 路径在 clover 上不可用**——它假设 GMU 与 A740 编译器栈。a5xx 既没有
GMU（轨开不了），也没有 Turnip/AdrenoMetal 前端。要桌面，只能回到显示/合成器那条线。
