#!/bin/sh
# Builds the Fit640 extension and Info app, and packs both into one
# 1.4 MB Mac disk image, build/Fit640.dsk, and into a StuffIt 1.5.1
# archive, build/Fit640.sit
set -e
TOOLCHAIN="${RETRO68:-$HOME/Retro68/build/toolchain}"
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$HERE/build"
cd "$HERE/build"
cmake -S "$HERE" -B . \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN/m68k-apple-macos/cmake/retro68.toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release > cmake.log
cmake --build .

rm -f Fit640.dsk
dd if=/dev/zero of=Fit640.dsk bs=1024 count=1440 2>/dev/null
"$TOOLCHAIN/bin/hformat" -l Fit640 Fit640.dsk > /dev/null
"$TOOLCHAIN/bin/hcopy" -m Fit640.bin :
"$TOOLCHAIN/bin/hcopy" -m Fit640Info.bin :
"$TOOLCHAIN/bin/hcopy" -m Fit640Strip.bin :
"$TOOLCHAIN/bin/hls" -l
"$TOOLCHAIN/bin/humount"
python3 "$HERE/tools/mksit.py" Fit640.sit Fit640.bin Fit640Strip.bin Fit640Info.bin
echo "Built: $HERE/build/Fit640.dsk"
echo "Built: $HERE/build/Fit640.sit"
