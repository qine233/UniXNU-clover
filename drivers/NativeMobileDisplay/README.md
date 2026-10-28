# NativeMobileDisplay (board-local copy)

Mobile display driver and GPU scanout hand-off. Validates swaps, retires frames and drives the firmware framebuffer.

This is a copy of the base repository's `drivers/NativeMobileDisplay` module, kept here because "unixnu" resolves a device repository's own module directory before the base repository's (`scripts/unixnu/feeds.py`, candidate 5 before candidate 6; `drivers/ArmCpu` uses the same pattern). Only one line differs from the base module:

* `NativeLegacyDisplay::startHardware()` claims the main display for any nonzero `PE_Video.v_display` instead of the literal `1`, because on this ARM port that field is a Text/Graphics flag and not a mode enumeration (see the comment at the call site).

Everything else is the base module, byte for byte, at the commit this device was built against. `scripts/build-clover-gop-legacy-display.py` records the digest of every source it compiles in `build/native-legacy-display/build-report.json`, and the kernel link gate rejects the archive if any of them changed.

Current state: the CPU and GPU scanout paths work. The GOP scanout has no vblank interrupt; with `sheng-vsync=<Hz>` a work-loop timer sends the original family's vsync message to the compositor.

Linked into XNU by `unixnu build kernel`.
