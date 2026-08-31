Nova — Cross-Platform CMake Build Instructions
===============================================

1. Linux x86_64

Install:
  - CMake >= 3.21
  - C++17 compiler (GCC or Clang)
  - LLVM development package containing LLVMConfig.cmake
  - pthreads (normally provided by the system)

Configure:
  cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release -DNOVA_ENABLE_LLVM=ON

If LLVM is installed in a non-standard prefix:
  cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm

Build:
  cmake --build build-linux -j

Optional install:
  cmake --install build-linux --prefix ./dist

2. Android / NDK

Use the Android NDK CMake toolchain instead of the old AArch64-only
prebuilt compiler.

Example (replace NDK path and ABI as required):
  cmake -S . -B build-android \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-26 \
    -DCMAKE_BUILD_TYPE=Release \
    -DNOVA_ENABLE_LLVM=OFF

Build:
  cmake --build build-android -j

For another Android ABI, change ANDROID_ABI, for example:
  armeabi-v7a
  arm64-v8a
  x86
  x86_64

3. LLVM on Android

Do not hardcode an NDK LLVM include path.

If you have a separately built LLVM for the target Android ABI and it provides
LLVMConfig.cmake, configure with:
  -DLLVM_DIR=/path/to/android-llvm/lib/cmake/llvm

If target LLVM is unavailable, leave NOVA_ENABLE_LLVM=OFF. The CMake file will
still build the rest of Nova without pretending that host LLVM libraries are
usable on Android.

4. Clean reproducible builds

Delete the build directory when changing toolchains:
  rm -rf build-linux build-android

Then configure from scratch with the desired toolchain.

The CMake file discovers the project C/C++ sources from the Nova source-tree
directories instead of depending on an AArch64 Android prebuilt layout.
