#!/usr/bin/env python3
"""Adds the Zorro2RTG board to an Amiberry v8.5.0 source tree (README.md in the repo root).
    integrate.py AMIBERRY_SRC
Copies the board model in and makes a few small edits, each skipped if already present, so it can be
run again. Fails if an anchor line is missing (a different Amiberry version)."""
import pathlib
import shutil
import sys

HERE = pathlib.Path(__file__).resolve().parent   # hosts/amiberry
REPO = HERE.parents[1]
PLUGIN = REPO / "plugins" / "zorro2rtg"


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


def replace(path, old, new):
    text = path.read_text()
    if new in text:
        return
    if old not in text:
        sys.exit(f"integrate.py: text not found in {path}: {old!r}")
    path.write_text(text.replace(old, new, 1))
    print(f"edited {path.relative_to(path.parents[1])}")


def migrate(path, old, new):
    """Renames text left by an older integrate.py (the adapter was src/zorro2rtg.cpp, zorro2rtg_func)."""
    text = path.read_text()
    if old in text and new not in text:
        path.write_text(text.replace(old, new))
        print(f"migrated {path.relative_to(path.parents[1])}")


def main():
    src = pathlib.Path(sys.argv[1]).resolve()
    old = src / "src" / "zorro2rtg.cpp"            # the adapter's name before 2026-11-20
    if old.exists():
        old.unlink()
    shutil.copy(HERE / "zboard_amiberry.cpp", src / "src" / "zboard_amiberry.cpp")
    shutil.copy(REPO / "hosts" / "common" / "zboard_channel.cpp", src / "src" / "zboard_channel.cpp")
    for f in ("z2rtg_plugin.cpp", "z2rtg_core.cpp", "z2rtg_link.cpp"):
        shutil.copy(PLUGIN / f, src / "src" / f)
    for f in (PLUGIN / "z2rtg_core.h", PLUGIN / "z2rtg_link.h", PLUGIN / "zorro2rtg_regs.h",
              REPO / "include" / "zboard_plugin.h", REPO / "hosts" / "common" / "zboard_channel.h"):
        shutil.copy(f, src / "src" / "include" / f.name)
    h = src / "src/include/gfxboard.h"
    g = src / "src/gfxboard.cpp"
    c = src / "cmake/SourceFiles.cmake"
    migrate(h, "zorro2rtg_func", "zboard_func")
    migrate(g, "&zorro2rtg_func", "&zboard_func")
    migrate(c, "        src/zorro2rtg.cpp\n", "        src/zboard_amiberry.cpp\n")
    edit(h, "#define GFXBOARD_ID_ZZ9000_Z3 44\n", "#define GFXBOARD_ID_ZORRO2RTG 45\n")
    edit(h, "extern struct gfxboard_func zz9000_func;\n", "extern struct gfxboard_func zboard_func;\n")
    edit(g, """		0, 0x83, &zz9000_func, {}, 0x20
	},
""", """	{
		GFXBOARD_ID_ZORRO2RTG,
		_T("Zorro2RTG [Zorro II]"), _T("Zorro2-Tang25k"), _T("Zorro2RTG"),
		0x5a32, 1, 0, 0,
		0x00000001, 0x00400000, 0x00400000, 0x00400000, 0, 2, 2, false, true,
		0, 0xd7, &zboard_func
	},
""", marker="GFXBOARD_ID_ZORRO2RTG,")
    # boot ROM (docs/registers.md section 3): er_Type $D7 sets ERTF_DIAGVALID, er_InitDiagVec $8000 its DiagArea
    replace(g, "		0, 0xc7, &zboard_func\n", "		0, 0xd7, &zboard_func\n")
    edit(g, "	ew(gb, 0x24, (gfxb->serial >>  0) & 0xff);\n",
         "	if (gfxb->id == GFXBOARD_ID_ZORRO2RTG) {\n		ew(gb, 0x28, 0x80);\n		ew(gb, 0x2c, 0x00);\n	}\n")
    e = src / "src/expansion.cpp"
    edit(e, "rbc->rtgmem_type != GFXBOARD_ID_ZZ9000_Z3 &&",
         " rbc->rtgmem_type != GFXBOARD_ID_ZORRO2RTG &&", marker="GFXBOARD_ID_ZORRO2RTG")
    edit(c, "        src/zz9000.cpp\n", "        src/zboard_amiberry.cpp\n        src/z2rtg_core.cpp\n")
    edit(c, "        src/z2rtg_core.cpp\n", "        src/z2rtg_link.cpp\n")
    edit(c, "        src/z2rtg_link.cpp\n", "        src/z2rtg_plugin.cpp\n")
    edit(c, "        src/z2rtg_plugin.cpp\n", "        src/zboard_channel.cpp\n")


if __name__ == "__main__":
    main()
