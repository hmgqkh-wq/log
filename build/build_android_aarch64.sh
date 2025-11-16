#!/usr/bin/env bash
# Build libxeno_wrapper.so for Android aarch64 using NDK
NDK_PATH=${NDK_PATH:-"$HOME/Android/Sdk/ndk-bundle"}
if [ ! -d "$NDK_PATH" ]; then echo "Set NDK_PATH to Android NDK root"; exit 1; fi
API=24
TOOLCHAIN=$NDK_PATH/toolchains/llvm/prebuilt/linux-x86_64
export PATH=$TOOLCHAIN/bin:$PATH
export CC=aarch64-linux-android$API-clang
mkdir -p build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=$NDK_PATH/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API ..
make -j$(nproc)
cp libxeno_wrapper.so ../usr/lib/ || true
