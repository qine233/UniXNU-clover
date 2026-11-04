# NativeKeyStoreEvents

AppleKeyStore state client. Handles keybag and lock-state events for the sheng port.

Current state: used with `sheng-aks=1`.

Linked into XNU by `unixnu build kernel`.
## How this module meets the UniXNU driver documentation

Class shape is the documented one: `AppleKeyStore : public IOService` and
`AppleKeyStoreUserClient : public IOUserClient` are `docs/drivers/xnu-drivers.md:19-64`, built with the
standard `OSDeclareDefaultStructors`/`OSDefineMetaClassAndStructors` macros, and the service is published
by `registerService()` from a successful start, which is the stage order of `:219-228`.

Layer and wiring: `docs/drivers/overview.md:11-16` (built-in XNU), and on this board the object is
compiled by `scripts/build-arm-driver.py NativeKeyStoreEvents`, which is invoked from
`scripts/build-clover-gop-legacy-display.py:306` next to the two display objects it also builds. That
route carries the five-field object ABI of `docs/drivers/xnu-drivers.md:106-116` and the
`-Wall -Wextra -Werror` replay of the recorded `.cpo.json` flags (`docs/drivers/xnu-drivers.md:71-104`).

No `Info.plist`, and that is not a gap: `docs/drivers/xnu-drivers.md:14` treats the plist as the record of
a personality, this module declares none, and the base repository does the same for it
(`ls /Users/zhouzhou/UniXNU/drivers/NativeKeyStoreEvents` has no plist) — see
`notes/unixnu-doc-conformance.md` §3 for the measured rule. On this board the class is armed by
`offline-key-events=1` (`board.toml:2372`) through `package/CloverKeyStoreProbe.cpp`, and the argument's
name is sheng's, kept verbatim because the probe reads it.
