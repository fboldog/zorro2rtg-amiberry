#!/usr/bin/env python3
"""Run the Zorro2RTG Amiberry build headless and take screenshots over its IPC socket (docs/driver.md).
    run_test.py [--config NAME] [--shots 40,70] [--out DIR] [--seconds 80]
Starts ~/amiberry-zorro2/run.sh with AMIBERRY_BIN=~/amiberry-zorro2/src/build/amiberry, offscreen video and
no audio, takes a PNG at each time in --shots (seconds after start), then stops it. The emulator log is
written next to the screenshots.
Stopping the emulator while PrismRTG starts leaves ENVARC:Prism.booting, and PrismRTG then skips one boot;
the flag is removed from --drive before every run."""
import argparse
import os
import pathlib
import socket
import subprocess
import time

HOME = pathlib.Path.home()
INST = HOME / "amiberry-zorro2"


def ipc(cmd, *args, timeout=10.0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(str(INST / ".runtime" / "amiberry.sock"))
    s.sendall(("\t".join((cmd,) + args) + "\n").encode())
    data = b""
    while not data.endswith(b"\n"):
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
    s.close()
    return data.decode(errors="replace").strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", default="A2000 Zorro2RTG.uae")
    ap.add_argument("--shots", default="40,70")
    ap.add_argument("--seconds", type=float, default=80)
    ap.add_argument("--out", default=".")
    ap.add_argument("--drive", default=str(INST / "Amiberry/HardDrives/A2000-Zorro2RTG"))
    o = ap.parse_args()
    out = pathlib.Path(o.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, SDL_VIDEODRIVER="offscreen", SDL_AUDIODRIVER="dummy",
               AMIBERRY_BIN=str(INST / "src/build/amiberry"))
    cfg = INST / "Amiberry/Configurations" / o.config
    for flag in pathlib.Path(o.drive, "Prefs/Env-Archive").glob("Prism.booting*"):
        flag.unlink()
    with open(out / "amiberry.log", "w") as log:
        p = subprocess.Popen([str(INST / "run.sh"), "--config", str(cfg), "-G", "--log"], env=env,
                             stdout=log, stderr=subprocess.STDOUT)
        t0 = time.monotonic()
        try:
            for t in sorted(float(x) for x in o.shots.split(",") if x):
                while time.monotonic() - t0 < t:
                    if p.poll() is not None:
                        raise SystemExit("amiberry exited early, see amiberry.log")
                    time.sleep(0.5)
                png = out / f"shot_{int(t):03d}s.png"
                print(f"{t:5.0f} s: {ipc('SCREENSHOT', str(png))}")
            while time.monotonic() - t0 < o.seconds and p.poll() is None:
                time.sleep(0.5)
        finally:
            try:
                ipc("QUIT", timeout=3)
            except OSError:
                pass
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()


if __name__ == "__main__":
    main()
