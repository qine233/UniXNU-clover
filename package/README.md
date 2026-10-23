# package/

这个目录的内容在组装后的构建树里会以 `kits/clover` 出现，
`[firmware] packages` 把 `{tree}/package` 放进 EDK II 的 `PACKAGES_PATH`，
所以即使还是空的，目录本身也必须存在。

这里该放什么，按板子最可能需要它们的顺序排列：

1. **带 `Kit.toml` 的 kit 目录** —— UniXNU 在 `[match]` 满足时合并的驱动
   `.efi`、设备树 overlay 或 kext 包。第一个候选是把调试串口或 ft5406 触摸屏
   做成 payload 侧的 UEFI 驱动，因为平板的 USB 只有 device 模式，否则菜单输入
   只能拉串口线。

   ```toml
   schema = 1

   [kit]
   name = "clover"
   version = "1.0.0"
   description = "Mi Pad 4 8\" LTE bring-up kit"

   [match]
   arch = "AARCH64"
   fdt_compatible = ["xiaomi,clover-lte", "qcom,sdm660"]
   firmware_vendor = ["..."]          # Mu-Silicium 上报的字符串；从一次真实启动里读
   requires_protocol = ["..."]        # 驱动需要的协议；从驱动本身填
   ```

   `firmware_vendor` 和 `requires_protocol` 是故意留空的：两者都必须取自
   构建出的固件和驱动 INF，不能在这里猜。

2. **XNU 驱动源码** —— 板级组件如果存在，就作为独立仓库放在 `drivers/` 并在
   `feeds.toml` 里列出；这个目录只存构建好的 kit 内容。

3. **`unixnu.toml`** —— 只有当板子需要 `board.toml` 的 `[payload]` 段表达不了
   的 payload 配置时才放。通常 payload 步骤自己会写出这个文件。

首次控制台启动什么都不需要，所以这个目录是空的。
