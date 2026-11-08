#!/usr/bin/env python3
"""Prepare the assembled format-2 tree for this device's kernel archives.

Two things format 2 does not place in out/<device>/<profile>/<name>/tree but the archive recipes
and the kernel link need:

1. The device drivers. Format 2 links only catalog-selected components into tree/drivers, and this
   port's sources (CloverSdhci, CloverBlock, CloverInput, CloverPower, CloverDsp, CloverGpu,
   CloverPMGR, NativeAES, NativeMobileDisplay, NativeHidInput, NativeKeyStoreEvents) are
   device-local and absent from the catalog. The archives run with cwd=tree, so every
   `build-arm-driver.py <clover name>` and every build-clover-*.py would otherwise look for a source
   schema 2 never placed there. This is the format-1 tree behaviour, made explicit: one step that
   says which sources the tree is missing, instead of a tree that silently differs from format 1.

2. The catalog output path for the AES archive. build-xnu-4k-baseline.sh links
   $ROOT/build/software-aes/libSoftwareAES.a whenever XNU_NATIVE_AES=1; format 1 got that path from
   sync_outputs' legacy_outputs link, which format 2 skips because SoftwareAES is not a selected
   component here. The archive recipe still writes build/native-aes/libSoftwareAES.a (that is the
   directory the device build-arm-driver.py has always used), so the link path is a symlink to it.
   It must point at libSoftwareAES.a, not libNativeAES.a: the bindings step rebuilds the former with
   NativeAES.o and original-bindings.o.

Runs as the first [kernel].archives entry, before any archive is compiled. Catalog modules already
linked by the assembler win; this only fills what is absent. Both the driver symlinks and this one
are rebuilt from scratch on the next `unixnu tree`.
"""
import os
import sys
from pathlib import Path

tree = Path(__file__).resolve().parents[1]
device = os.environ.get('UNIXNU_DEVICE')
if not device:
    sys.exit('UNIXNU_DEVICE is unset: the kernel archive step supplies the device repository')

linked, present = [], []
source = Path(device) / 'drivers'
if not source.is_dir():
    sys.exit(f'{source}: the device repository has no drivers/')
for entry in sorted(source.iterdir()):
    if not entry.is_dir() or entry.name.startswith('.'):
        continue
    target = tree / 'drivers' / entry.name
    if target.is_symlink() or target.exists():
        present.append(entry.name)
        continue
    target.parent.mkdir(parents=True, exist_ok=True)
    target.symlink_to(entry.resolve(), target_is_directory=True)
    linked.append(entry.name)

compat = tree / 'build/software-aes/libSoftwareAES.a'
compat.parent.mkdir(parents=True, exist_ok=True)
if compat.is_symlink() or compat.exists():
    compat.unlink()
compat.symlink_to(Path('..') / 'native-aes' / 'libSoftwareAES.a')

# build-arm-driver.py NativeAES resolves its source as drivers/NativeAES/NativeAES.cpp (the format-1
# directory name), and the driver itself is the base repository's SoftwareAES component. Link it
# from there rather than vendoring a second copy into this repository: a vendored driver would owe
# the driver-documentation gate a README clause citation, and it is not a driver this port authored.
aes = tree / 'drivers' / 'NativeAES'
if not (aes.is_symlink() or aes.exists()):
    # tree/patches/xnu is the base repository's patch directory, so two levels up is its root.
    base = (tree / 'patches' / 'xnu').resolve().parents[1]
    upstream = base / 'drivers' / 'SoftwareAES'
    if not upstream.is_dir():
        sys.exit(f'{upstream}: the base repository carries no SoftwareAES driver to link as NativeAES')
    aes.symlink_to(upstream, target_is_directory=True)
    linked.append('NativeAES')

print('prepare-clover-tree: linked ' + (', '.join(linked) or 'none')
      + '; already in the tree: ' + (', '.join(present) or 'none')
      + '; AES link ' + str(compat.relative_to(tree)))
