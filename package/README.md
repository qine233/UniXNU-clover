# package/

The contents of this directory appear as `kits/clover` in the assembled build tree, and
`[firmware] packages` puts `{tree}/package` on the EDK II `PACKAGES_PATH`, so the directory
has to exist even while it is empty.

What belongs here, in the order the board is likely to need it:

1. **A kit directory with a `Kit.toml`** — driver `.efi` files, device tree overlays or kext
   bundles that UniXNU merges when `[match]` is satisfied. The first candidate is the debug
   UART or the ft5406 touch panel as a payload-side UEFI driver, because the tablet's USB is
   peripheral-only and menu input otherwise needs a serial cable.

   ```toml
   schema = 1

   [kit]
   name = "clover"
   version = "1.0.0"
   description = "Mi Pad 4 8\" LTE bring-up kit"

   [match]
   arch = "AARCH64"
   fdt_compatible = ["xiaomi,clover-lte", "qcom,sdm660"]
   firmware_vendor = ["..."]          # the string Mu-Silicium reports; read it from a real boot
   requires_protocol = ["..."]        # the protocol the driver needs; fill from the driver
   ```

   `firmware_vendor` and `requires_protocol` are left as placeholders on purpose: both must be
   taken from the built firmware and the driver INF, not guessed here.

2. **XNU driver sources** — if a component is board-specific it lives in `drivers/` as its own
   repository and is listed in `feeds.toml`; this directory holds the built kit contents.

3. **A `unixnu.toml`** only if the board needs a payload configuration that the `[payload]`
   section of `board.toml` cannot express. The payload step normally writes that file itself.

Nothing here is required for the first console boot, which is why the directory is empty.
