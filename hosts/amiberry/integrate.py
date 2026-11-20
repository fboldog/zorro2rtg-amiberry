#!/usr/bin/env python3
"""Adds one zboard plugin to an Amiberry v8.5.0 source tree (README.md in the repo root, docs/writing-a-plugin.md).
    integrate.py AMIBERRY_SRC [--plugin NAME]      NAME: a directory of plugins/ with a plugin.json (default zorro2rtg)

The tree must be a git checkout of v8.5.0 (build.sh clones one). Each run starts again from it, so plugins can be
switched in the same tree: the Amiberry files this script edits are restored from git, the files the last run
copied in (listed in AMIBERRY_SRC/.zboard_files) are removed, and then the chosen plugin goes in:
  - always: include/zboard_plugin.h, hosts/common/zboard_channel.{h,cpp}, hosts/amiberry/zboard_amiberry_common.h,
    the plugin's sources and headers (plugin.json "sources", "headers");
  - "adapter": "rtg" (an RTG card, ZBP_DESC_DISPLAY): hosts/amiberry/zboard_amiberry.cpp and the board's row in
    gfxboard.cpp from plugin.json "amiberry.rtg" (it must match the plugin's zbp_desc), its board ID, and a
    DiagArea offset when er_type has ERTF_DIAGVALID;
  - "adapter": "expansion" (any other Zorro II board): hosts/amiberry/zboard_expansion.cpp and one row in
    expansion.cpp's board table (ROMTYPE_ZBOARD, "amiberry.expansion.config_name"). Its Autoconfig bytes come
    from zbp_desc at run time. Switched on in a config with <config_name>_rom_file=:ENABLED.
Fails if an anchor line is missing (another Amiberry version) or the tree is not a git checkout.
"""
import argparse
import json
import pathlib
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent   # hosts/amiberry
REPO = HERE.parents[1]
EDITED = ["src/gfxboard.cpp", "src/include/gfxboard.h", "src/expansion.cpp", "src/include/rommgr.h",
          "cmake/SourceFiles.cmake"]
LIST = ".zboard_files"
ROMTYPE_ZBOARD = "0x00100099"        # free in v8.5.0 (after ROMTYPE_ATONCE 0x00100098); VERIFY upstream


def edit(path, anchor, insert, after=True):
    text = path.read_text()
    if anchor not in text:
        sys.exit(f"integrate.py: anchor not found in {path}: {anchor!r}")
    path.write_text(text.replace(anchor, anchor + insert if after else insert + anchor, 1))
    print(f"edited {path.relative_to(path.parents[1])}")


def c_str(s):
    return '_T("' + s.replace("\\", "\\\\").replace('"', '\\"') + '")'


def rtg(src, cfg, sources):
    r = cfg["amiberry"]["rtg"]
    h, g, e = src / "src/include/gfxboard.h", src / "src/gfxboard.cpp", src / "src/expansion.cpp"
    edit(h, "#define GFXBOARD_ID_ZZ9000_Z3 44\n", f"#define {r['id_name']} {r['id']}\n")
    edit(h, "extern struct gfxboard_func zz9000_func;\n", "extern struct gfxboard_func zboard_func;\n")
    b = lambda v: "true" if v else "false"   # noqa: E731
    edit(g, """		0, 0x83, &zz9000_func, {}, 0x20
	},
""", f"""	{{
		{r['id_name']},
		{c_str(r['name'])}, {c_str(r['manufacturer_name'])}, {c_str(r['config_name'])},
		{r['manufacturer']}, {r['model_memory']}, {r['model_registers']}, {r['model_extra']},
		{r['serial']}, {r['vram_min']}, {r['vram_max']}, {r['bank_size']}, {r['chiptype']}, {r['configtype']}, {r['irq']}, {b(r['swap'])}, {b(r['hasswitcher'])},
		{r['romtype']}, {r['er_type']}, &zboard_func
	}},
""")
    if int(str(r["er_type"]), 0) & 0x10:       # ERTF_DIAGVALID: er_InitDiagVec at $28/$2C
        dv = int(str(r["diag_vec"]), 0)
        edit(g, "	ew(gb, 0x24, (gfxb->serial >>  0) & 0xff);\n",
             f"	if (gfxb->id == {r['id_name']}) {{\n		ew(gb, 0x28, 0x{dv >> 8:02x});\n		ew(gb, 0x2c, 0x{dv & 0xff:02x});\n	}}\n")
    edit(e, "rbc->rtgmem_type != GFXBOARD_ID_ZZ9000_Z3 &&", f" rbc->rtgmem_type != {r['id_name']} &&")
    return ["zboard_amiberry.cpp"]


def expansion(src, cfg, sources):
    x = cfg["amiberry"]["expansion"]
    rm, e = src / "src/include/rommgr.h", src / "src/expansion.cpp"
    edit(rm, "#define ROMTYPE_ATONCE		0x00100098\n", f"#define ROMTYPE_ZBOARD		{ROMTYPE_ZBOARD}\n")
    edit(e, "const struct expansionromtype expansionroms[] = {\n",
         "bool zboard_exp_init(struct autoconfig_info *aci);   // zboard_expansion.cpp (zboard plugin)\n",
         after=False)
    edit(e, """

	{
		NULL
	}
};
""", f"""	{{
		{c_str(x['config_name'])}, {c_str(x['name'])}, {c_str(x['manufacturer_name'])},
		NULL, zboard_exp_init, NULL, NULL, ROMTYPE_ZBOARD | ROMTYPE_NOT, 0, 0, BOARD_AUTOCONFIG_Z2, true,
		NULL, 0,
		false, EXPANSIONTYPE_CUSTOM,
		0, 0, 0, false, NULL,
		false, 0, NULL,
		{{ 0 }}
	}},
""", after=False)
    return ["zboard_expansion.cpp"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("--plugin", default="zorro2rtg")
    o = ap.parse_args()
    src = pathlib.Path(o.src).resolve()
    plugin = REPO / "plugins" / o.plugin
    cfg = json.loads((plugin / "plugin.json").read_text())
    adapter = cfg["amiberry"]["adapter"]
    if adapter not in ("rtg", "expansion"):
        sys.exit(f"integrate.py: {plugin}/plugin.json: adapter must be rtg or expansion, not {adapter!r}")
    if subprocess.run(["git", "-C", str(src), "rev-parse", "--git-dir"], capture_output=True).returncode:
        sys.exit(f"integrate.py: {src} is not a git checkout of Amiberry v8.5.0 (build.sh clones one)")

    # start again from v8.5.0: the edited files from git, the files copied in last time removed
    subprocess.run(["git", "-C", str(src), "checkout", "--", *EDITED], check=True)
    old = src / LIST
    names = old.read_text().split() if old.exists() else []
    names += ["src/zorro2rtg.cpp"]               # the adapter's name before 2026-11-20
    for n in names:
        (src / n).unlink(missing_ok=True)

    copied = []

    def copy(f, dest):
        shutil.copy(f, src / dest / f.name)
        copied.append(f"{dest}/{f.name}")

    sources = [plugin / f for f in cfg["sources"]]
    adapter_files = rtg(src, cfg, sources) if adapter == "rtg" else expansion(src, cfg, sources)
    for f in adapter_files:
        copy(HERE / f, "src")
    copy(REPO / "hosts/common/zboard_channel.cpp", "src")
    for f in sources:
        copy(f, "src")
    for f in [REPO / "include/zboard_plugin.h", REPO / "hosts/common/zboard_channel.h",
              HERE / "zboard_amiberry_common.h"] + [plugin / f for f in cfg["headers"]]:
        copy(f, "src/include")
    cpp = [c for c in copied if c.endswith(".cpp")]
    edit(src / "cmake/SourceFiles.cmake", "        src/zz9000.cpp\n", "".join(f"        {c}\n" for c in cpp))
    old.write_text("\n".join(copied) + "\n")
    print(f"integrated plugin {cfg['name']} ({adapter} adapter): {', '.join(copied)}")


if __name__ == "__main__":
    main()
