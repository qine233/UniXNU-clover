# clover 的 UI 渲染路线：Metal → metal2vulkan → CPU Vulkan

决定（2026-11-16，用户选定）：GPU 区先走 **CPU 软件 Vulkan**，不先碰 Adreno 512 的硬件驱动。

## 依据

- 黑屏定因：锁屏时钟渲染器 `AegirPoster` 在 `CLKUIMetalQuadView → NUNICalliopeResourceManager`
  的 `pipelineState != nil` 断言处 SIGABRT；Metal render pipeline 创建返回 nil。
- 参考路线不可直接照搬：
  - sheng：`Adreno740 + AdrenoMetal`，Metal 前端是 **metal2vulkan → Turnip**；
  - santoni：`CA/GLES → EAGL → Mesa/IR3 → A505`，是 GLES 路径，覆盖不了 iOS 17 锁屏的 Metal。
  - Turnip 不支持 a5xx（Mesa 邮件列表 2025-11 明确：a5xx 上 Vulkan 1.2/descriptor indexing 不可能），
    而 clover 是 Adreno 512（a5xx）。
- CPU Vulkan 与用户既定策略「前期 CPU 强行解码顶出桌面」一致，且不需要 Adreno 驱动。

## 可用的接缝（已核对源码）

1. `drivers/AdrenoMetal/bridge/NativeMetalBridge.rs:88-90`：
   `open(loader, cache)` 里 `options.loader = Some(PathBuf::from(loader))`。
   注释（同文件 3–4 行）：Turnip 被当作 Vulkan **"loader"** 打开，只需要它导出
   `vkGetInstanceProcAddr`（iPadOS 没有 Khronos loader）。
   ⇒ Vulkan 实现是可替换的：任何一个导出 `vkGetInstanceProcAddr` 的 dylib 都能接。
2. `scripts/build-adreno-metal-plugin.py:95-102`：Vulkan dylib 放在插件 bundle 里，
   命名 `libvulkan_freedreno.dylib`，`strip -x`，`codesign --sign - --pagesize 4096`。
   ⇒ clover 侧同样只需把软件 Vulkan 作为这个 dylib 打包、签名、进 rootfs。
3. `NativeMetalBridge.rs` 的自测已覆盖 compute/render/import（IOSurface 无拷贝导入）四步，
   每步先写 marker，挂起时能报步骤号 —— 可以直接当第一步的判据。

## 必须先验证的三件事（按顺序）

| 顺序 | 问题 | 最小验证 | 失败后果 |
| --- | --- | --- | --- |
| V1 | 软件 Vulkan 能否在宿主（macOS arm64）跑通 metal2vulkan 自测 | 宿主构建 SwiftShader/lavapipe，把 `nmb_selftest` 的 loader 指向它 | 换实现（lavapipe/SwiftShader 二选一） |
| V2 | 设备上 backboardd 这类 Apple 进程能否做 JIT / `mmap(PROT_EXEC)` | 一个最小 dylib 在受控进程里 mmap RW→RX 并执行一段代码；用 `cs_enforcement_disable=1` 前提下测 | JIT 型软件 Vulkan 不可行，转解释器或独立 helper 进程 |
| V3 | IOSurface 导入路径能否由软件 Vulkan 提供 | metal2vulkan import 步骤 + 我们驱动的 `native_legacy_frame_observed` 采样 | 输出无法进扫描缓冲，需要自建 host-memory 导入 |

风险已知：SwiftShader/lavapipe 都靠 LLVM JIT；若 V2 不通过，唯一 CPU 路线是**解释执行**
（极慢，锁屏静帧也许可接受），或把 Vulkan 放进我们自己的 helper 进程（entitlement 可控），
在 backboardd 里只留 IPC stub —— 后者工程量大，但架构上可行（sheng 的
`HostMetalCommandServer` 就是同类形状的宿主测试替身）。

## 里程碑

1. **V1 宿主自测（r431：compute/render 通，import 待 V3）**：metal2vulkan 的宿主桥（`build-adreno-metal-bridge.py --target host`）+ Chrome 157 的 arm64 SwiftShader，在 macOS 上 `compute_bad 0`、`render_bad 0`；无拷贝 import 需要 `VK_EXT_external_memory_host`，SwiftShader 不提供 ⇒ 归入 V3。
   **r432 把 V3 的宿主半边补齐**：Mesa lavapipe（27.0.0-devel，LLVM 23.1.4）+ 一个导出
   `vkGetInstanceProcAddr` 的 40 行 shim，`bridge_smoke` 得 `code 0 stage 0 compute_bad 0
   render_bad 0 import_bad 0`。lavapipe 宣告 `EXT_external_memory_host`、4 KiB 对齐、
   `memoryTypeBits=1`，缓冲导入无拷贝；宿主页上的图像走驱动的影子拷贝
   （`metal2vulkan-degrade-shadowed-texture`）。软件 Vulkan 的选择定为 lavapipe。
2. **V2 设备 JIT 门**：一个最小 kext 侧/用户态探针，确认受控进程能有可执行映射；记录 errno。
   **r430 通过（boot 213–215）**：宿主是 `offline-air-init` 从 Data 卷 exec 的
   `/private/var/tmp/air-native`（`scripts/clover-jit-probe.S`，ad-hoc 签名、无 entitlement）。
   读数：RWX mmap 成功、写与刷缓存成功，但执行那条 `ret` 得 SIGBUS（内核剥掉执行权）；
   **RW → `mprotect(PROT_READ|EXEC)` → 调用正常返回**，进程 exit 0。⇒ JIT 用 W^X 写时切换即可，
   不必 MAP_JIT。待补：backboardd 这类平台进程的同一组读数（或按计划走独立 helper + IPC stub）。
3. **V3 导入门**：软件 Vulkan 的 host-memory 导入接上 IOSurface 语义，采样非黑。
   宿主半边 r432 已过（缓冲导入无拷贝）。**设备半边 r433 过**：LLVM 23.1.4 与 lavapipe
   交叉编到 iOS，把 lavapipe 静态链进 metal2vulkan 自测、以自身路径当 loader，板上得
   `code 0 stage 0 compute_bad 0 render_bad 0 import_bad 0`、`exit 0`。两个设备侧前提：
   `os_jit_allowed()` 要接受 W^X（RW→mprotect RX，LLVM 的 SectionMemoryManager 就用它）；
   沙箱只放行已加载映像与系统卷的 mmap ⇒ 插件/dylib 必须进 **OS 卷**，Data 卷不行。
4. **装配**：按 sheng 的插件 bundle 形状打包 `libvulkan_freedreno.dylib`（换成软件实现），
   ad-hoc 签名 4 KiB 页，放进 clover 的 **OS 卷** `/usr/lib` 或插件 bundle（沙箱要求）；
   OS 卷在 `round420-userdata-root.img` 的 APFS 容器里，宿主挂起编辑后只把改动的 1 MiB
   块 dd 回 `userdata`。backboardd 的平台沙箱还要单独量；若同样拦 mmap，就走独立
   helper + IPC stub。
5. **上机判据**：`AegirPoster` 不再 SIGABRT（无新 `AegirPoster-*.ips`）；`panelbox` 出现非黑
   `col=`/`lit>0`；presentation 数继续增长。

## 不做的事

- 不扩 Turnip 到 a5xx（上游结论：基本不可能）。
- 不在第一步就移植 Adreno 512 硬件驱动；那是 CPU 路线被证伪后的下一支。
