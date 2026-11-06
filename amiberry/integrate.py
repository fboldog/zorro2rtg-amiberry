#!/usr/bin/env python3
"""Adds the Zorro2RTG board to an Amiberry v8.5.0 source tree (docs/driver.md).
    integrate.py AMIBERRY_SRC
Copies the board model in and makes six small edits, each skipped if already present, so it can be
run again. Fails if an anchor line is missing (a different Amiberry version)."""
import pathlib
import shutil
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def edit(path, anchor, insert, after=True, marker=None):
    text = path.read_text()
    marker = marker or insert.strip().splitlines()[0]
    if marker in text:
        return
    if anchor not in text:
        sys.exit(f"integrate.py: anchor not found in {path}: {anchor!r}")
    text = text.replace(anchor, anchor + insert if after else insert + anchor, 1)
    path.write_text(text)
    print(f"edited {path.relative_to(path.parents[1])}")


def main():
    src = pathlib.Path(sys.argv[1]).resolve()
    shutil.copy(HERE / "zorro2rtg.cpp", src / "src" / "zorro2rtg.cpp")
    shutil.copy(ROOT / "sw/emu/z2rtg_core.cpp", src / "src" / "z2rtg_core.cpp")
    shutil.copy(ROOT / "sw/emu/z2rtg_core.h", src / "src" / "include" / "z2rtg_core.h")
    shutil.copy(ROOT / "sw/emu/z2rtg_link.cpp", src / "src" / "z2rtg_link.cpp")
    shutil.copy(ROOT / "sw/emu/z2rtg_link.h", src / "src" / "include" / "z2rtg_link.h")
    shutil.copy(ROOT / "sw/p96/zorro2rtg_regs.h", src / "src" / "include" / "zorro2rtg_regs.h")
    h = src / "src/include/gfxboard.h"
    edit(h, "#define GFXBOARD_ID_ZZ9000_Z3 44\n", "#define GFXBOARD_ID_ZORRO2RTG 45\n")
    edit(h, "extern struct gfxboard_func zz9000_func;\n", "extern struct gfxboard_func zorro2rtg_func;\n")
    g = src / "src/gfxboard.cpp"
    edit(g, """		0, 0x83, &zz9000_func, {}, 0x20
	},
""", """	{
		GFXBOARD_ID_ZORRO2RTG,
		_T("Zorro2RTG [Zorro II]"), _T("Zorro2-Tang25k"), _T("Zorro2RTG"),
		0x5a32, 1, 0, 0,
		0x00000001, 0x00400000, 0x00400000, 0x00400000, 0, 2, 2, false, true,
		0, 0xc7, &zorro2rtg_func
	},
""", marker="GFXBOARD_ID_ZORRO2RTG,")
    e = src / "src/expansion.cpp"
    edit(e, "rbc->rtgmem_type != GFXBOARD_ID_ZZ9000_Z3 &&",
         " rbc->rtgmem_type != GFXBOARD_ID_ZORRO2RTG &&", marker="GFXBOARD_ID_ZORRO2RTG")
    c = src / "cmake/SourceFiles.cmake"
    edit(c, "        src/zz9000.cpp\n", "        src/zorro2rtg.cpp\n        src/z2rtg_core.cpp\n")
    edit(c, "        src/z2rtg_core.cpp\n", "        src/z2rtg_link.cpp\n")


if __name__ == "__main__":
    main()
