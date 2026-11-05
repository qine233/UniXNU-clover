# NativeMobileDisplay (board-local copy)

Mobile display driver and GPU scanout hand-off. Validates swaps, retires frames and drives the firmware framebuffer.

This is a copy of the base repository's `drivers/NativeMobileDisplay` module, kept here because "unixnu" resolves a device repository's own module directory before the base repository's (`scripts/unixnu/feeds.py`, candidate 5 before candidate 6; `drivers/ArmCpu` uses the same pattern). Only one line differs from the base module:

* `NativeLegacyDisplay::startHardware()` claims the main display for any nonzero `PE_Video.v_display` instead of the literal `1`, because on this ARM port that field is a Text/Graphics flag and not a mode enumeration (see the comment at the call site).

Everything else is the base module, byte for byte, at the commit this device was built against. `scripts/build-clover-gop-legacy-display.py` records the digest of every source it compiles in `build/native-legacy-display/build-report.json`, and the kernel link gate rejects the archive if any of them changed.

Current state: the CPU and GPU scanout paths work. The GOP scanout has no vblank interrupt; with `sheng-vsync=<Hz>` a work-loop timer sends the original family's vsync message to the compositor.

Linked into XNU by `unixnu build kernel`.
## How this module meets the UniXNU driver documentation

The class is the documented shape: `NativeMobileDisplay : public IOMobileFramebuffer` with the standard
`OSDeclareDefaultStructors`/`OSDefineMetaClassAndStructors` macros is `docs/drivers/xnu-drivers.md:19-64`,
and the frame lifecycle it implements is the `probe()/start()/registerService()` stage table at
`:219-228`.

Layer and wiring are also documented, with one board-specific state worth saying plainly: on *this* board
the family class itself is not linked — `configs/clover_defconfig:102` sets
`CONFIG_XNU_NATIVE_MOBILE_DISPLAY=n` — while the two objects the display round actually ships,
`NativeLegacyDisplay` and `NativeGOPPresenter`, are built from this same directory by
`scripts/build-clover-gop-legacy-display.py`, which delegates each of them to
`scripts/build-arm-driver.py` (`:136`, `:322`). That is the route carrying the `-Wall -Wextra -Werror`
replay of the recorded flags (`docs/drivers/xnu-drivers.md:71-104`) and the five-field object ABI of
`:106-116`; the builder additionally pins the `.late.a` link contract and records the digest of every
source it compiles in `build/native-legacy-display/build-report.json`.

Wiring follows `docs/drivers/xnu-drivers.md:118-124` and `docs/porting/device-repository.md:113`
(`[kernel] archives`), with `CONFIG_XNU_NATIVE_LEGACY_DISPLAY=y` in the defconfig. No `Info.plist` is a
consequence of the same rule as above: `docs/drivers/xnu-drivers.md:14` records a personality, the base
repository ships this module without a plist, and this copy inherits that
(`notes/unixnu-doc-conformance.md` §3). The framebuffer address is not a provider property and is not
invented here either — `docs/drivers/xnu-drivers.md:215` is the clause for a device below the `arm-io`
range, and the splash base this port uses is the device-verified `0x9D400000`, whose two competing
readings and the round that settled them are `notes/clover-device-tree-sources.md` §5, pinned again for
every build as `SPLASH_BASE` in `scripts/check-clover.py:43`. The instruments
in this directory are opt-in through their own arguments (`sheng-vsync=<Hz>`, `sheng-fb-gpu=1`,
`offline-legacy-*`), which is rule 6 of `docs/drivers/sheng-novatek-touch.md:141-148`.
