#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Boot ROM USB input driver, end to end on the emulated board (docs/usb_input.md, "Boot ROM driver").

    input_test.py [--reboot] [--out DIR]

Starts the isolated Amiberry (~/amiberry-zorro2, as run_test.py) with Z2RTG_BOOTROM=sw/bootrom/build/bootrom.bin
and Z2RTG_INPUT on a named pipe, waits for the remote shell (z2amiga.py), then checks:
  - Workbench is on the card (the RTG driver still takes the board with the boot ROM in it);
  - text typed through the card (z2sendkeys.py, the frames the RP2040 sends) runs in a Shell window;
  - with --reboot: Ctrl+LAmiga+RAmiga reboots the machine (RAM: is empty afterwards).
Screenshots and the emulator log go to --out. Build the ROM (sw/bootrom/build.sh) and Amiberry
(sw/emu/amiberry/build.sh) first.
"""
import argparse
import os
import pathlib
import struct
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
INST = pathlib.Path.home() / "amiberry-zorro2"
PIPE = INST / ".runtime/z2rtg_input"
Z2AMIGA = [sys.executable, str(HERE / "z2amiga.py")]
SEND = [sys.executable, str(ROOT / "sw/tools/host/z2sendkeys.py"), "--pipe", str(PIPE)]
errors = 0


def check(what, ok, detail=""):
    global errors
    print(f"{'ok  ' if ok else 'FAIL'} {what}{(': ' + detail) if detail else ''}")
    errors += not ok


def sh(script, timeout=60):
    r = subprocess.run(Z2AMIGA + ["sh", script], capture_output=True, text=True, timeout=timeout + 30)
    return r.returncode, r.stdout.strip()


def wait_agent(seconds):
    t0 = time.monotonic()
    while time.monotonic() - t0 < seconds:
        try:
            rc, out = sh("Echo z2-agent-up", timeout=20)
            if "z2-agent-up" in out:
                return True
        except subprocess.TimeoutExpired:
            pass
    return False


def screenshot(path):
    subprocess.run(Z2AMIGA + ["ipc", "SCREENSHOT", str(path)], capture_output=True, timeout=30)
    with open(path, "rb") as f:
        head = f.read(24)
    return struct.unpack(">II", head[16:24])           # PNG IHDR width, height


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=".")
    ap.add_argument("--reboot", action="store_true")
    ap.add_argument("--config", default="A2000 Zorro2RTG.uae")
    o = ap.parse_args()
    out = pathlib.Path(o.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    rom = ROOT / "sw/bootrom/build/bootrom.bin"
    if not rom.exists():
        sys.exit("input_test: build the boot ROM first (sw/bootrom/build.sh)")
    subprocess.run(SEND + ["mkfifo"], check=True, capture_output=True)
    drive = INST / "Amiberry/HardDrives/A2000-Zorro2RTG"
    for flag in (drive / "Prefs/Env-Archive").glob("Prism.booting*"):
        flag.unlink()
    env = dict(os.environ, SDL_VIDEODRIVER="offscreen", SDL_AUDIODRIVER="dummy",
               AMIBERRY_BIN=str(INST / "src/build/amiberry"), Z2RTG_BOOTROM=str(rom), Z2RTG_INPUT=str(PIPE))
    with open(out / "amiberry.log", "w") as log:
        p = subprocess.Popen([str(INST / "run.sh"), "--config", str(INST / "Amiberry/Configurations" / o.config),
                              "-G", "--log"], env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            check("Amiga booted, remote shell answers", wait_agent(180))
            time.sleep(10)                              # PrismRTG switches Workbench to the card after the agent
            w, h = screenshot(out / "boot.png")
            check("Workbench on the card with the boot ROM present", (w, h) == (1280, 720), f"{w}x{h}")
            sh("Delete RAM:z2usb QUIET\nRun >NIL: NewShell \"CON:20/40/600/150/USB input test\"")
            time.sleep(3)
            subprocess.run(SEND + ["text", r"echo Typed-through-USB >RAM:z2usb\n"], check=True, timeout=60)
            time.sleep(2)
            rc, txt = sh("Type RAM:z2usb")
            check("typed command ran in the Shell", txt == "Typed-through-USB", repr(txt))
            screenshot(out / "typed.png")
            if o.reboot:
                try:
                    subprocess.run(SEND + ["key", "Ctrl+LAmiga+RAmiga"], timeout=30, capture_output=True)
                except subprocess.TimeoutExpired:
                    pass
                time.sleep(5)
                ok = wait_agent(180)
                rc, txt = sh("Type RAM:z2usb") if ok else (1, "")
                check("Ctrl+LAmiga+RAmiga rebooted the Amiga", ok and "Typed-through-USB" not in txt)
        finally:
            try:
                subprocess.run(Z2AMIGA + ["ipc", "QUIT"], capture_output=True, timeout=10)
                p.wait(timeout=20)
            except (subprocess.TimeoutExpired, OSError):
                p.kill()
    print("PASS input_test" if errors == 0 else f"FAIL input_test: {errors} errors")
    sys.exit(errors != 0)


if __name__ == "__main__":
    main()
