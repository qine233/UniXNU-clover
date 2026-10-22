#!/usr/bin/env python3
"""Link target for the display probe that the DEVELOPMENT-only XNU patch calls unconditionally.

clover has no display bring-up: the real NativeLegacyDisplay archive needs the sheng ABI
generator, and the Adreno 512 in this tablet is not the A740 that NativeAdreno targets. The
probe only runs for the offline-legacy-register boot argument, which this board does not pass,
so one empty function keeps the link honest without pretending to drive the panel.

This mirrors UniXNU-gta4l's build-gta4l-display-stub.py, the same shape for the same reason:
an eMMC tablet with no display driver yet. Symbols are added here only when the kernel link
names them, never guessed in advance.
"""
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/clover-display-stub'

STUB = '''
void native_legacy_display_probe(void) {}
'''


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    source = OUT / 'clover-display-stub.c'
    obj = OUT / 'clover-display-stub.o'
    source.write_text(STUB)
    # The kernel step is macOS-only, so the iOS toolchain is the correct one here.
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    subprocess.run(['xcrun', '--sdk', 'iphoneos', 'clang', '-c', '-target', 'arm64-apple-ios17.0',
                    '-isysroot', sdk, '-mkernel', '-ffreestanding', '-O2',
                    '-o', str(obj), str(source)], check=True)
    archive = OUT / 'libCloverDisplayStub.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(obj)], check=True)
    print(f'display stub: {archive}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
