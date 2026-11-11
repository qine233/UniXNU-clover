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
