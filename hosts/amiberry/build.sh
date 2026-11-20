#!/bin/sh
# Build Amiberry v8.5.0 with one zboard plugin compiled in (README.md in the repo root), the way the AUR PKGBUILD
# builds it.
#   build.sh [SRC [PLUGIN]]   default SRC: ~/amiberry-zorro2/src, the Zorro2-Tang25k project's isolated Amiberry;
#                             PLUGIN: a directory of plugins/ (default zorro2rtg). Switching plugins in one SRC
#                             works: integrate.py starts again from v8.5.0 each time.
# Output: SRC/build/amiberry. Run it with AMIBERRY_BIN=SRC/build/amiberry ~/amiberry-zorro2/run.sh ...
# The system Amiberry (/usr/bin/amiberry) and ~/Amiberry are not touched.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${1:-$HOME/amiberry-zorro2/src}
PLUGIN=${2:-zorro2rtg}
if [ ! -d "$SRC/.git" ]; then
    git clone --branch v8.5.0 https://github.com/BlitterStudio/amiberry.git "$SRC"
fi
(cd "$SRC" && git submodule update --init --recursive)
python3 "$HERE/integrate.py" "$SRC" --plugin "$PLUGIN"
cmake -S "$SRC" -B "$SRC/build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$SRC/build"
ls -l "$SRC/build/amiberry"
