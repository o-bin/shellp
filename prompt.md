# MISSION DIRECTIVE: Assemble, Integrate, and Compile `shellp` for Android (ARM64)

You are tasked with assembling, resolving missing dependencies, cross-compiling native subsystems, and building the final Android release APK for the **`shellp`** repository.

---

## 1. PROJECT OVERVIEW & ARCHITECTURAL CONTEXT

### What is `shellp`?
`shellp` is an all-in-one, 100% rootless Linux subsystem and terminal emulator for Android (`arm64-v8a`). It executes a full **Debian Bookworm** distribution under **PRoot** without requiring root privileges, Termux, or external dependencies.

### System Components & Architecture
1. **Host Android Shell (`app/android/`)**:
   - Packaged inside a Flutter project skeleton (`app/`), but the primary execution entrypoint is a custom Android `NativeActivity` (`com.shellp.app.MainActivity`).
   - `MainActivity.java` extends `android.app.NativeActivity` with overrides for Android 13+ (API 33+) Predictive Back gestures to prevent the Linux subsystem from terminating when users dismiss the soft keyboard or navigate backwards (`moveTaskToBack(true)`).
   - Provides JNI helper methods (`showSoftKeyboard()`, `hideSoftKeyboard()`, `isKeyboardVisible()`) called from native C.

2. **Native Terminal & Compositor (`native/shellp_main.c`)**:
   - Pure C event loop driven by `android_native_app_glue.c` handling direct drawing to the Android `ANativeWindow_Buffer` (RGBA_8888).
   - Direct bitmap glyph rendering using monospace fonts from `repos/yaft/glyph.h`.
   - On-screen touch keybar (`ESC`, `TAB`, `CTRL`, `ALT`, `|`, `/`, `^`, `v`, `<`, `>`).
   - Kinetic fling scrolling, 2,000-line circular scrollback history buffer, and PTY session management (`openpty`).

3. **In-App Bootstrap & Unpacker (`native/tar_extract/tar_xz.c` & `native/xz_embedded/`)**:
   - First launch checks whether Debian bash exists (`<filesDir>/debian/debian-bookworm-aarch64/bin/bash`).
   - If not installed, invokes Android's Java HTTPS stack via JNI (`java.net.URL`, `URLConnection`) to download `debian-bookworm-aarch64-pd-v4.17.3.tar.xz` (approx. 43 MB) from `https://github.com/termux/proot-distro/releases/download/v4.17.3/debian-bookworm-aarch64-pd-v4.17.3.tar.xz`.
   - Uses `extract_tar_xz()`: an in-process, pure-C decompressor combining `xz_embedded` (`xz_dec_catrun()`, LZMA2, BCJ filters, CRC32/CRC64) and a streaming tar parser that automatically handles POSIX and GNU long link/file names (`L` and `K` tar types), symlinks, hardlinks, permissions, and directory creation.
   - Sets up guest `/etc/resolv.conf` (nameservers `1.1.1.1` and `8.8.8.8`) and `/etc/hosts`.

4. **PRoot Sandboxed Execution**:
   - Executes `libproot.so` (with `PROOT_LOADER=libloader.so`) located in `ApplicationInfo.nativeLibraryDir`.
   - PRoot arguments:
     `proot --link2symlink --sysvipc -0 -r <rootfs_dir> -b /dev -b /proc -b /sys -b <internalDir>/resolv.conf:/etc/resolv.conf -w /root /bin/bash --login`
   - Sets environment variables: `PROOT_TMP_DIR`, `PROOT_LOADER`, `PROOT_IGNORE_MISSING_BINDINGS=1`, `HOME=/root`, `USER=root`, `TERM=xterm-256color`, `PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`, `SHELL=/bin/bash`.
   - Fallback: If PRoot fails to execute Debian bash, it executes `libbusybox.so` (`bin/busybox`) running `sh -i`.

---

## 2. REPOSITORY GAP ANALYSIS (WHAT IS MISSING)

1. **Missing `.gitmodules` configuration**:
   - The repository tree defines gitlink entries for `repos/proot`, `repos/proot-distro`, and `repos/yaft`, but no `.gitmodules` file is committed. Standard `git submodule update --init` fails.
2. **Missing `glyph.h`**:
   - `native/shellp_main.c` line 32 includes `../repos/yaft/glyph.h`. This header is not committed in `yaft` and must be generated using yaft's `mkfont_bdf` tool using its bundled BDF fonts.
3. **Missing Native Build Specification (`CMakeLists.txt`)**:
   - `native/` lacks a build script to compile `libshellp_main.so` together with `android_native_app_glue`, `tar_xz`, and `xz_embedded`.
4. **Missing Native Binaries in `jniLibs/`**:
   - At runtime, `shellp_main.c` expects the following binaries to reside in `ApplicationInfo.nativeLibraryDir`:
     - `libshellp_main.so` (the main NativeActivity)
     - `libproot.so` (the PRoot executable)
     - `libloader.so` (PRoot static aarch64 loader)
     - `libbusybox.so` (static aarch64 BusyBox)
     - `libtalloc.so` (dynamic talloc library required by PRoot)
   - These must all be placed in `app/android/app/src/main/jniLibs/arm64-v8a/`.
5. **Stale Host Build Cache in `repos/talloc-2.4.2`**:
   - Residual files in `repos/talloc-2.4.2/bin/` from the original author's system must be wiped before re-configuring for Android ARM64.

---

## 3. STEP-BY-STEP COMPILATION & INTEGRATION WORKFLOW

Execute the following steps in sequence in your environment.

### STEP 1: Set Up Toolchain & Environment Variables

Ensure you have:
- Linux x86_64 or aarch64 environment
- Host tools: `gcc`, `make`, `curl`, `ar`, `tar`, `xz-utils`, `python3`
- Android NDK (r25b+ or r26c/r27/r28)
- Android SDK (API 34+ platform, Build-Tools 34.0.0+)
- JDK 17 (`JAVA_HOME`)
- Flutter SDK (3.47.4+ stable) or Gradle

Set the cross-compilation environment:
```bash
# Set paths to your NDK and SDK installations
export ANDROID_NDK_ROOT=/path/to/android-ndk
export ANDROID_HOME=/path/to/android-sdk
export JAVA_HOME=/path/to/jdk-17

# Export cross-compilation toolchain for ARM64 (API 26+)
export TOOLCHAIN=$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64
export TARGET_TRIPLE=aarch64-linux-android
export API_LEVEL=26

export CC=$TOOLCHAIN/bin/${TARGET_TRIPLE}${API_LEVEL}-clang
export CXX=$TOOLCHAIN/bin/${TARGET_TRIPLE}${API_LEVEL}-clang++
export AR=$TOOLCHAIN/bin/llvm-ar
export STRIP=$TOOLCHAIN/bin/llvm-strip
export OBJCOPY=$TOOLCHAIN/bin/llvm-objcopy
export OBJDUMP=$TOOLCHAIN/bin/llvm-objdump

export PATH=$TOOLCHAIN/bin:$PATH
```

---

### STEP 2: Clone Missing Submodule Repositories

Clone the three missing dependencies at their exact pinned commit SHAs:

```bash
cd /path/to/shellp

# 1. PRoot
rm -rf repos/proot
git clone https://github.com/termux/proot repos/proot
cd repos/proot && git checkout 7266fb3e8516535682f5a9c8f3a7e70f6506eddb && cd ../..

# 2. PRoot-Distro
rm -rf repos/proot-distro
git clone https://github.com/termux/proot-distro repos/proot-distro
cd repos/proot-distro && git checkout 7049151937b562e5f446be7abb5040824cd86055 && cd ../..

# 3. Yaft
rm -rf repos/yaft
git clone https://github.com/uobikiemukot/yaft repos/yaft
cd repos/yaft && git checkout 59ef091187736200e07ee1d67d6249ad4c691542 && cd ../..
```

---

### STEP 3: Generate `repos/yaft/glyph.h`

Compile the font builder on the host machine and generate the header:

```bash
cd /path/to/shellp/repos/yaft
gcc -std=c99 -O2 tools/mkfont_bdf.c -o tools/mkfont_bdf

./tools/mkfont_bdf table/alias \
    fonts/milkjf/milkjf_k16.bdf \
    fonts/milkjf/milkjf_8x16r.bdf \
    fonts/milkjf/milkjf_8x16.bdf \
    fonts/terminus/ter-u16n.bdf > glyph.h

test -s glyph.h || { echo "ERROR: glyph.h is empty!"; exit 1; }
cd ../..
```

---

### STEP 4: Build Native Android Dependencies into `jniLibs/arm64-v8a`

Prepare the destination folder:
```bash
mkdir -p /path/to/shellp/app/android/app/src/main/jniLibs/arm64-v8a
JNILIBS=/path/to/shellp/app/android/app/src/main/jniLibs/arm64-v8a
```

#### A. Build `libtalloc.so`
```bash
cd /path/to/shellp/repos/talloc-2.4.2
# Clean stale host artifacts
rm -rf bin .lock-wscript

CC="$CC" AR="$AR" ./configure \
    --prefix=/usr/local \
    --disable-rpath \
    --disable-python \
    --cross-compile \
    --cross-answers=cross-answers.txt

./buildtools/bin/waf build
cp bin/default/libtalloc.so $JNILIBS/libtalloc.so
cd ../..
```

#### B. Build `libproot.so` and `libloader.so`
```bash
cd /path/to/shellp/repos/proot/src

# 1. Compile PRoot loader for ARM64 (libloader.so)
$CC -fPIC -ffreestanding -c loader/loader.c -o loader/loader.o
$CC -fPIC -ffreestanding -c loader/assembly.S -o loader/assembly.o
$CC -static -nostdlib -Wl,-Ttext=0x2000000000,-z,noexecstack -o loader/loader \
    loader/loader.o loader/assembly.o
$STRIP loader/loader
cp loader/loader $JNILIBS/libloader.so

# 2. Compile PRoot executable (packaged as libproot.so)
make clean
make CC="$CC" STRIP="$STRIP" OBJCOPY="$OBJCOPY" OBJDUMP="$OBJDUMP" \
    CFLAGS="-Wall -Wextra -O2 -I/path/to/shellp/repos/talloc-2.4.2 -I/path/to/shellp/repos/talloc-2.4.2/bin/default -DPROOT_UNBUNDLE_LOADER=\\\"/data/data/com.shellp.app/lib/libloader.so\\\"" \
    LDFLAGS="-L$JNILIBS -ltalloc -Wl,-z,noexecstack" \
    proot

$STRIP proot
cp proot $JNILIBS/libproot.so
cd ../../..
```

#### C. Stage `libbusybox.so`
Obtain or build a static ARM64 BusyBox binary, name it `libbusybox.so`, and place it in `$JNILIBS`:
```bash
curl -sL "http://deb.debian.org/debian/pool/main/b/busybox/busybox-static_1.35.0-4+deb12u1+b1_arm64.deb" -o /tmp/bb.deb
ar p /tmp/bb.deb data.tar.xz | tar -xJ -C /tmp/ ./bin/busybox
cp /tmp/bin/busybox $JNILIBS/libbusybox.so
chmod 755 $JNILIBS/libbusybox.so
```

---

### STEP 5: Build `libshellp_main.so`

Create `/path/to/shellp/native/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.22.1)
project(shellp_main C)

set(CMAKE_C_STANDARD 11)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/app_glue
    ${CMAKE_CURRENT_SOURCE_DIR}/tar_extract
    ${CMAKE_CURRENT_SOURCE_DIR}/xz_embedded/linux/include/linux
    ${CMAKE_CURRENT_SOURCE_DIR}/xz_embedded/userspace
    ${CMAKE_CURRENT_SOURCE_DIR}/../repos/yaft
)

add_definitions(
    -DXZ_USE_CRC64
    -DXZ_USE_SHA256
    -DXZ_DEC_ANY_CHECK
    -DXZ_DEC_CONCATENATED
    -DXZ_DEC_ARM64
    -DXZ_DEC_X86
    -DXZ_DEC_ARM
    -DXZ_DEC_ARMTHUMB
    -DXZ_DEC_RISCV
    -DXZ_DEC_POWERPC
)

set(SHELLP_SRCS
    shellp_main.c
    app_glue/android_native_app_glue.c
    tar_extract/tar_xz.c
    xz_embedded/linux/lib/xz/xz_crc32.c
    xz_embedded/linux/lib/xz/xz_crc64.c
    xz_embedded/linux/lib/xz/xz_sha256.c
    xz_embedded/linux/lib/xz/xz_dec_stream.c
    xz_embedded/linux/lib/xz/xz_dec_lzma2.c
    xz_embedded/linux/lib/xz/xz_dec_bcj.c
)

add_library(shellp_main SHARED ${SHELLP_SRCS})

target_link_libraries(shellp_main
    android
    log
    m
)
```

Compile `libshellp_main.so`:
```bash
mkdir -p /path/to/shellp/native/build && cd /path/to/shellp/native/build
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-26 \
    -DCMAKE_BUILD_TYPE=Release

cmake --build .
cp libshellp_main.so $JNILIBS/libshellp_main.so
cd ../..
```

Verify that all five shared objects are present:
```bash
ls -la /path/to/shellp/app/android/app/src/main/jniLibs/arm64-v8a/
# Expected:
# - libbusybox.so
# - libloader.so
# - libproot.so
# - libshellp_main.so
# - libtalloc.so
```

---

### STEP 6: Compile the Android APK

1. In `app/android/app/build.gradle.kts`, ensure `jniLibs` packaging is configured:
   ```kotlin
   android {
       ...
       sourceSets {
           getByName("main") {
               jniLibs.srcDirs("src/main/jniLibs")
           }
       }
       packaging {
           jniLibs {
               useLegacyPackaging = true
           }
       }
   }
   ```

2. Build the APK:
   ```bash
   cd /path/to/shellp/app

   # Via Flutter:
   flutter build apk --release

   # OR via Gradle wrapper:
   cd android
   ./gradlew assembleRelease
   ```

The compiled APK will be at:
`app/build/app/outputs/flutter-apk/app-release.apk` (or `app/android/app/build/outputs/apk/release/app-release-unsigned.apk`).

---

### STEP 7: Final Verification

Confirm that all native binaries are bundled inside the APK:
```bash
unzip -l app/build/app/outputs/flutter-apk/app-release.apk | grep "lib/arm64-v8a"
```
The output must show all 5 binaries:
- `lib/arm64-v8a/libshellp_main.so`
- `lib/arm64-v8a/libproot.so`
- `lib/arm64-v8a/libloader.so`
- `lib/arm64-v8a/libbusybox.so`
- `lib/arm64-v8a/libtalloc.so`

Completion of these steps produces a functional, standalone `shellp` Android package.
