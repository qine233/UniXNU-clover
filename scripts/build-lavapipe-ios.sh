#!/bin/bash
# Cross-build the software Vulkan used by clover's Metal route on a macOS host.
#
# Inputs (override with environment variables):
#   LLVM_SRC   llvm-project-llvmorg-23.1.4 checkout
#   MESA_SRC   Mesa 27.0.0-devel checkout
#   OUT        output root (default ~/clover-out/lavapipe-ios-build)
#   SDK        iPhoneOS SDK (default: xcrun --sdk iphoneos)
#
# Outputs under OUT:
#   llvm-ios/                     installed LLVM static libraries for iOS
#   mesa-ios-build/libvulkan_lvp.dylib
#   lavapipe-ios/libvulkan_freedreno.dylib   signed, 4 KiB pages
#
# Two host traps are handled here:
#   * LLVM's CMake keys Darwin-only link flags on CMAKE_SYSTEM_NAME == "Darwin";
#     with iOS it takes the ELF branch and adds -Wl,-z,defs/--gc-sections.
#   * ninja runs commands with /bin/sh (bash 3.2 here), which cannot execve the
#     Homebrew python shebang; meson is reached through a zsh wrapper.
set -euo pipefail

LLVM_SRC=${LLVM_SRC:?set LLVM_SRC to the llvm-project checkout}
MESA_SRC=${MESA_SRC:?set MESA_SRC to the Mesa checkout}
OUT=${OUT:-$HOME/clover-out/lavapipe-ios-build}
SDK=${SDK:-$(xcrun --sdk iphoneos --show-sdk-path)}
JOBS=${JOBS:-6}
mkdir -p "$OUT/bin"

cat > "$OUT/bin/clang-ios" <<'EOF'
#!/bin/sh
exec /usr/bin/xcrun --sdk iphoneos clang "$@"
EOF
cat > "$OUT/bin/clang++-ios" <<'EOF'
#!/bin/sh
exec /usr/bin/xcrun --sdk iphoneos clang++ "$@"
EOF
cat > "$OUT/bin/meson" <<'EOF'
#!/bin/zsh
exec /opt/homebrew/bin/meson "$@"
EOF
chmod +x "$OUT/bin/"*

# --- LLVM for iOS ---------------------------------------------------------
python3.13 - "$LLVM_SRC" <<'EOF'
import sys
from pathlib import Path
root = Path(sys.argv[1])
p = root / 'llvm/cmake/modules/HandleLLVMOptions.cmake'
t = p.read_text()
t = t.replace('CMAKE_SYSTEM_NAME MATCHES "Darwin|FreeBSD', 'CMAKE_SYSTEM_NAME MATCHES "Darwin|iOS|FreeBSD')
p.write_text(t)
p = root / 'llvm/cmake/modules/AddLLVM.cmake'
t = p.read_text()
t = t.replace('if("${CMAKE_SYSTEM_NAME}" MATCHES "Darwin")', 'if("${CMAKE_SYSTEM_NAME}" MATCHES "Darwin|iOS")')
p.write_text(t)
EOF

cmake -G Ninja -S "$LLVM_SRC/llvm" -B "$OUT/llvm-ios-build" \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$SDK" -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_TARGETS_TO_BUILD=AArch64 -DLLVM_ENABLE_PROJECTS= \
  -DLLVM_BUILD_LLVM_DYLIB=OFF -DLLVM_LINK_LLVM_DYLIB=OFF \
  -DLLVM_ENABLE_RTTI=OFF -DLLVM_ENABLE_EH=OFF -DLLVM_ENABLE_THREADS=ON \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_DOCS=OFF -DLLVM_BUILD_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF \
  -DLLVM_BUILD_EXAMPLES=OFF -DLLVM_BUILD_TESTS=OFF -DLLVM_ENABLE_TERMINFO=OFF \
  -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF \
  -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_PIC=ON \
  -DLLVM_TABLEGEN=/opt/homebrew/opt/llvm@23/bin/llvm-tblgen \
  -DLLVM_NATIVE_TOOL_DIR=/opt/homebrew/opt/llvm@23/bin
ninja -C "$OUT/llvm-ios-build" -j"$JOBS"
cmake --install "$OUT/llvm-ios-build" --prefix "$OUT/llvm-ios"
( cd "$OUT/llvm-ios/lib" && /usr/bin/libtool -static -o libLLVMAll.a $(ls *.a | grep -v '^libLLVMAll.a$') )
printf '%s\n' all-targets aarch64 asmparser bitreader bitstreamreader bitwriter core coroutines \
  debuginfodwarf demangle engine executionengine instcombine interpreter irreader ipo linker lto \
  mcdisassembler mcjit native objcopy option orcjit remarks scalaropts support tablegen target \
  transformutils windowsdriver > "$OUT/llvm-ios/lib/components.txt"

cat > "$OUT/bin/llvm-config-ios" <<'EOF'
#!/bin/sh
prefix=${LLVM_IOS_PREFIX:?}
case "$1" in
  --version) echo 23.1.4 ;;
  --prefix) echo "$prefix" ;;
  --includedir) echo "$prefix/include" ;;
  --libdir) echo "$prefix/lib" ;;
  --cflags) echo "-I$prefix/include" ;;
  --libs) echo "-L$prefix/lib -lLLVMAll" ;;
  --system-libs) echo "" ;;
  --has-rtti) echo "NO" ;;
  --shared-mode) echo "static" ;;
  --link-shared) exit 1 ;;
  --link-static) echo "OK" ;;
  --targets-built) echo "AArch64" ;;
  --host-target) echo "arm64-apple-ios17.0" ;;
  --components) cat "$prefix/lib/components.txt" ;;
  --cmakedir) echo "$prefix/lib/cmake/llvm" ;;
  --cppflags) echo "-I$prefix/include" ;;
  *) echo "llvm-config-ios: unsupported query: $*" >&2; exit 1 ;;
esac
EOF
chmod +x "$OUT/bin/llvm-config-ios"

# --- Mesa (lavapipe) for iOS ---------------------------------------------
python3.13 - "$MESA_SRC" <<'EOF'
import sys
from pathlib import Path
root = Path(sys.argv[1])
# Loader entry point alias.
entry = root / 'src/gallium/targets/lavapipe/clover_loader_entry.c'
entry.write_text('''\
#include <stddef.h>
typedef void (*PFN_vkVoidFunction)(void);
extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(void *instance, const char *pName);
PFN_vkVoidFunction vkGetInstanceProcAddr(void *instance, const char *pName);
__attribute__((visibility("default")))
PFN_vkVoidFunction vkGetInstanceProcAddr(void *instance, const char *pName)
{
    return vk_icdGetInstanceProcAddr(instance, pName);
}
''')
p = root / 'src/gallium/targets/lavapipe/meson.build'
t = p.read_text()
if 'clover_loader_entry.c' not in t:
    p.write_text(t.replace("['lavapipe_target.c']", "['lavapipe_target.c', 'clover_loader_entry.c']"))
# W^X JIT probe: iOS here grants execution via RW then mprotect(RX), not MAP_JIT.
p = root / 'src/util/os_misc.c'
t = p.read_text()
old = '''   void *addr = mmap(NULL, page_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_ANONYMOUS | MAP_PRIVATE | MAP_JIT, -1, 0);
   if (addr == MAP_FAILED)
      return;

   munmap(addr, page_size);'''
new = '''   void *addr = mmap(NULL, page_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_ANONYMOUS | MAP_PRIVATE | MAP_JIT, -1, 0);
   if (addr == MAP_FAILED) {
      addr = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
      if (addr == MAP_FAILED)
         return;
      if (mprotect(addr, page_size, PROT_READ | PROT_EXEC) != 0) {
         munmap(addr, page_size);
         return;
      }
   }

   munmap(addr, page_size);'''
if old in t:
    p.write_text(t.replace(old, new))
EOF

cat > "$OUT/mesa-ios.ini" <<EOF
[binaries]
c = '$OUT/bin/clang-ios'
cpp = '$OUT/bin/clang++-ios'
objc = '$OUT/bin/clang-ios'
objcpp = '$OUT/bin/clang++-ios'
ar = '/usr/bin/ar'
strip = '/usr/bin/strip'
pkg-config = '/opt/homebrew/bin/pkg-config'
llvm-config = '$OUT/bin/llvm-config-ios'
[properties]
needs_exe_wrapper = true
[host_machine]
system = 'darwin'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
[built-in options]
c_args = ['-target', 'arm64-apple-ios17.0', '-isysroot', '$SDK', '-miphoneos-version-min=17.0']
cpp_args = ['-target', 'arm64-apple-ios17.0', '-isysroot', '$SDK', '-miphoneos-version-min=17.0']
c_link_args = ['-target', 'arm64-apple-ios17.0', '-isysroot', '$SDK', '-miphoneos-version-min=17.0']
cpp_link_args = ['-target', 'arm64-apple-ios17.0', '-isysroot', '$SDK', '-miphoneos-version-min=17.0']
EOF

LLVM_IOS_PREFIX="$OUT/llvm-ios" PATH="$OUT/bin:/opt/homebrew/bin:$PATH" \
LLVM_CONFIG="$OUT/bin/llvm-config-ios" \
meson setup "$OUT/mesa-ios-build" "$MESA_SRC" --cross-file "$OUT/mesa-ios.ini" \
  --prefix "$OUT/lavapipe-ios" -Dvulkan-drivers=swrast -Dgallium-drivers=llvmpipe \
  -Dplatforms= -Dglx=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
  -Degl=disabled -Dbuild-tests=false -Dllvm=enabled -Dvideo-codecs= \
  -Dshared-llvm=disabled -Dcpp_rtti=false
sed -i '' "s#/opt/homebrew/bin/meson#$OUT/bin/meson#g" "$OUT/mesa-ios-build/build.ninja"
ninja -C "$OUT/mesa-ios-build" -j"$JOBS" src/gallium/targets/lavapipe/libvulkan_lvp.dylib

cp "$OUT/mesa-ios-build/src/gallium/targets/lavapipe/libvulkan_lvp.dylib" \
   "$OUT/lavapipe-ios/libvulkan_freedreno.dylib"
strip -x "$OUT/lavapipe-ios/libvulkan_freedreno.dylib"
codesign --force --sign - --pagesize 4096 --identifier org.unixnu.clover.lavapipe \
  "$OUT/lavapipe-ios/libvulkan_freedreno.dylib"
codesign --verify --strict "$OUT/lavapipe-ios/libvulkan_freedreno.dylib"
echo "PASS: $OUT/lavapipe-ios/libvulkan_freedreno.dylib"
