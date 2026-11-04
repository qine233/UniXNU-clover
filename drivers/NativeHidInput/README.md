# NativeHidInput (board-local copy)

HID input driver. Publishes keyboard, pointer and touch events to IOHIDFamily.

This is a copy of the base repository's `drivers/NativeHidInput` module (an upstream submodule of its
own), kept here because `unixnu` resolves a device repository's own module directory before the base
repository's (`scripts/unixnu/feeds.py`, candidate 5 before candidate 6; `drivers/ArmCpu` and
`drivers/NativeMobileDisplay` use the same pattern). It arrived in round255, when this board needed its
owned `IOHIDDevice` to hang under the original `IOHIDResource` rather than under a touch controller that
does not answer.

Nothing differs from the base module: `NativeHidInput.cpp` is byte for byte the base copy, sha256
`0445e5f451ffcc827bdd603e822180444d998427fa3a6d63559b1a7b8f850998` as measured in round314, and
`LICENSE` is carried with it. The README is this file -- the base module ships one, round255 did not
copy it, and `docs/drivers/xnu-drivers.md:7-14` treats the three companions as one layout.

Current state: used by the clover HID path -- `package/CloverHidInputProbe.cpp` publishes the device
behind the `offline-hid-input` gate (its `PE_parse_boot_argn("offline-hid-input", ...)` early return)
and under the separate `offline-hid-home` switch, and the board ships `sheng-touch-hid=1` for the
device shape (`board.toml:2337`). `docs/drivers/native-novatek-touch.md:120-128` describes the sequence
counter this driver reads; the contacts on this board come from the button/HID probe, not a digitizer
matrix, so the counter shape is the reference rather than the source.

## How this module meets the UniXNU driver documentation

This is the only driver in the clover tree that holds the class shape of
`docs/drivers/xnu-drivers.md:19-64` with the plain pair of macros rather than a hand-written
metaclass: `class NativeHidInput : public IOHIDDevice`, with `OSDeclareDefaultStructors` and
`OSDefineMetaClassAndStructors`. It can use them because
`scripts/build-clover-hid-input.py` derives the family's real ABI from the original kernelcache rather
than guessing it: it synthesizes `build/native-hid-input/OriginalHidDeviceABI.h` (opaque private
storage sized to the original instance, `static_assert` on `sizeof(IOHIDDevice)`, and slots 168--206
declared but deprecated-so-uncallable), then binds the four symbols the macros need
(`IOHIDDevice::gMetaClass`, `IOHIDDevice`'s vtable, the constructor and destructor thunks) to addresses
read out of that image through a generated `original-bindings.S`. The same script refuses the build
unless the driver's own 207-slot vtable overrides exactly the ten slots the audit names, which is the
strictest product-layer gate any clover driver has. The two neighbouring shapes are worth naming
because they are three answers to the same clause: `drivers/CloverPMGR` stays on the macro path with
`OSDefineMetaClassWithInit` and resolves its superclass metaclass at runtime by name, while
`drivers/CloverBlock` hand-writes its `MetaClass` because it has neither the derivation nor a symbol to
look up -- `notes/unixnu-doc-conformance.md` §5 now lists this script as the proven way to close that gap.

Layer, wiring and object ABI are the documented ones: a built-in XNU driver
(`docs/drivers/overview.md:11-16`), compiled by `build-arm-driver.py NativeHidInput` -- called from
`scripts/build-clover-hid-input.py`, which `scripts/build-clover-gop-legacy-display.py` runs, which is
the `[kernel] archives` row at `board.toml:67` (`docs/drivers/xnu-drivers.md:118-124`) -- and audited
against `docs/drivers/xnu-drivers.md:106-116` by `build-arm-driver.py:120,129`. The runtime-stage
table of `:219-228` is met for real: `init`/`attach`/`start`/`stop`/`free` are all overridden here, and
the object goes through them from the probe's `OSMetaClass::preModLoad` / `postModLoad` /
`metaCast("IOHIDDevice")` sequence -- the builder deliberately strips `__mod_init_func` and globalizes
`__GLOBAL__sub_I_NativeHidInput.cpp` so the metaclass registers when the probe says so, not at image
load.

What it still does not have is a personality of its own (`docs/drivers/xnu-drivers.md:147-168`), and for
this module the reason is different from every other clover driver's: no `Info.plist` is correct here
because the matching record is already Apple's. The report descriptor at the top of the file is written
so that the original IOHIDFamily's own generic-keyboard personality matches the device, and the object
is attached to the `IOHIDResource` that the shipped family publishes -- so `registerService()` is not
this file's call to make and the probe order of
`docs/drivers/device-tree-matching.md:108-128` is not the path it enters by. The scheduling
divergence and its prerequisites are `notes/unixnu-doc-conformance.md` §5; the shape difference between
this driver's report path and that note's sixth rule is recorded in its §6.
