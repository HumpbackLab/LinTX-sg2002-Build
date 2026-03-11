#!/bin/bash
set -e

cd /home/shimmer/LinTx/LicheeRV-Nano-Build

echo "=== Sourcing build environment ==="
source build/cvisetup.sh

echo "=== Configuring for sg2002_licheervnano_sd ==="
defconfig sg2002_licheervnano_sd

echo "=== Environment configured successfully ==="
echo "CROSS_COMPILE: $CROSS_COMPILE"
echo "PROJECT_FULLNAME: $PROJECT_FULLNAME"
echo "OUTPUT_DIR: $OUTPUT_DIR"

echo "=== Starting full build ==="
build_all

echo "=== Build completed ==="
ls -lh install/soc_sg2002_licheervnano_sd/
