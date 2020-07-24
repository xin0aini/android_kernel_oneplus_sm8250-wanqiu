#!/bin/bash
export CC=clang
export PATH=/root/sm8475/build/build-tools/path/linux-x86:/root/sm8475/prebuilts-master/clang/host/linux-x86/clang-r416183b/bin:/root/sm8475/prebuilts-master/clang/host/linux-x86/llvm-binutils-stable:$PATH CLANG_TRIPLE=aarch64-linux-gnu- CROSS_COMPILE=aarch64-linux-gnu-     CROSS_COMPILE_COMPAT=arm-linux-gnueabi- 
args="-j$(nproc --all) \
O=out \
ARCH=arm64 \
CLANG_TRIPLE=aarch64-linux-gnu- \
CROSS_COMPILE=aarch64-linux-gnu- \
CC=clang \
CROSS_COMPILE_ARM32=arm-linux-gnueabi- 
LD=ld.lld"
make ${args} vendor/kona-perf_defconfig CC=clang #savedefconfig
make ${args} 