#!/bin/sh
# Build Amiberry v8.5.0 with the Zorro2RTG board (docs/driver.md), the way the AUR PKGBUILD builds it.
#   sw/emu/amiberry/build.sh [SRC]   default SRC: ~/amiberry-zorro2/src, the project's isolated Amiberry
# Output: SRC/build/amiberry. Run it with AMIBERRY_BIN=SRC/build/amiberry ~/amiberry-zorro2/run.sh ...
# The system Amiberry (/usr/bin/amiberry) and ~/Amiberry are not touched.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${1:-$HOME/amiberry-zorro2/src}
if [ ! -d "$SRC/.git" ]; then
    git clone --branch v8.5.0 https://github.com/BlitterStudio/amiberry.git "$SRC"
fi
(cd "$SRC" && git submodule update --init --recursive)
python3 "$HERE/integrate.py" "$SRC"
cmake -S "$SRC" -B "$SRC/build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$SRC/build"
ls -l "$SRC/build/amiberry"
