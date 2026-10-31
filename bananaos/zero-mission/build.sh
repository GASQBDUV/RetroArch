#!/bin/sh
# Cross-builds banana-zm.so for the Anbernic RG DS (aarch64, glibc 2.41 on bananaOS 0.5.0.1).
# Needs an aarch64 toolchain plus libdrm headers/libs for aarch64, e.g. on Debian/Ubuntu:
#   apt install gcc-aarch64-linux-gnu libdrm-dev:arm64
# Or build natively on the device with gcc and libdrm installed (set CC=gcc).
set -e
cd "$(dirname "$0")"
CC="${CC:-aarch64-linux-gnu-gcc}"
"$CC" -O2 -Wall -fPIC -shared \
    -I/usr/include/libdrm -I/usr/include/aarch64-linux-gnu \
    -o banana-zm.so banana-zm.c \
    -ldl -lpthread -ldrm -lm
echo "built $(pwd)/banana-zm.so"
