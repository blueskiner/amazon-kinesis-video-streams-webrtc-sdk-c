#!/bin/bash

# T31
#if [ -d "/build" ]; then
#  rm -fr /build
#fi
mkdir -p build
cd build
export CC=/opt/mips-gcc540-glibc222-64bit-r3.3.0/bin/mips-linux-gnu-gcc
export CXX=/opt/mips-gcc540-glibc222-64bit-r3.3.0/bin/mips-linux-gnu-g++
export AR=/opt/mips-gcc540-glibc222-64bit-r3.3.0/bin/mips-linux-gnu-ar
#export CC="mips-linux-gnu-gcc -march=mips32r2 -std=gnu99" CXX=mips-linux-gnu-g++ AR=mips-linux-gnu-ar
cmake \
-DBUILD_STATIC_LIBS=ON \
-DBUILD_OPENSSL_PLATFORM=linux-generic32 \
-DBUILD_LIBSRTP_HOST_PLATFORM=x86_64 \
-DBUILD_LIBSRTP_DESTINATION_PLATFORM=mips-linux-gnu \
..
make
