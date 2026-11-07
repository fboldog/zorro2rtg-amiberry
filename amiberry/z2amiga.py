#!/usr/bin/env python3
"""Drive the emulated Amiga without its window (docs/driver.md, "Remote shell").

Two channels:
  - a shell: agent/Z2Agent.rexx runs inside the Amiga (started by S:User-Startup) and executes AmigaDOS
    scripts that this tool leaves in DH0:Z2Agent/ on the shared host directory, returning output and rc;
  - Amiberry's IPC socket ($XDG_RUNTIME_DIR/amiberry.sock of that instance): RESET, SCREENSHOT, ...

    z2amiga.py [--drive DH0DIR] [--socket SOCK] install          agent, User-Startup lines, mode presets
    z2amiga.py sh 'List SYS:Prefs'                               run AmigaDOS commands, print the output
    z2amiga.py mode 640x480 | 800x600 | 1024x768 | 1280x720      Workbench screen mode (used and saved)
    z2amiga.py ipc RESET HARD                                    one IPC command, tab-joined arguments

Defaults: DH0 of "A2000 Zorro2RTG" in ~/amiberry-zorro2, or $Z2AMIGA_DRIVE; the socket in that instance's
runtime dir, or $Z2AMIGA_SOCKET. Remote use: ssh to the host and run it there.
"""
import argparse
import os
import pathlib
import shutil
import socket
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
ISO = pathlib.Path.home() / "amiberry-zorro2"
DEF_DRIVE = ISO / "Amiberry/HardDrives/A2000-Zorro2RTG"
DEF_SOCKET = ISO / ".runtime/amiberry.sock"

# PrismRTG 16-bit mode IDs, the same for Zorro2RTG.card and the native driver (docs/driver.md)
MODES = {"640x480": 0x7A000101, "800x600": 0x7A000102, "1024x768": 0x7A000103, "1280x720": 0x7A000104}
ID_OFFSET = 0x32                      # DisplayID in ScreenMode.prefs (FORM PREF, SCRM chunk)

USER_STARTUP = """; Zorro2RTG remote shell (z2amiga.py install)
SYS:System/RexxMast >NIL:
SYS:Rexxc/WaitForPort REXX
Run >NIL: SYS:Rexxc/RX S:Z2Agent.rexx
"""


def drive_dir(a):
    return pathlib.Path(a.drive or os.environ.get("Z2AMIGA_DRIVE") or DEF_DRIVE)


def install(a):
    d = drive_dir(a)
    shutil.copy(HERE / "agent/Z2Agent.rexx", d / "S/Z2Agent.rexx")
    us = d / "S/User-Startup"
    text = us.read_text(errors="replace") if us.exists() else ""
    if "Z2Agent.rexx" not in text:
        us.write_text(text + ("\n" if text and not text.endswith("\n") else "") + USER_STARTUP)
    (d / "Z2Agent").mkdir(exist_ok=True)
    # one ScreenMode.prefs per mode, from the drive's own file with only the DisplayID changed
    src = (d / "Prefs/Env-Archive/Sys/ScreenMode.prefs").read_bytes()
    if src[:4] != b"FORM" or src[ID_OFFSET:ID_OFFSET + 2] != b"\x7a\x00":
        sys.exit(f"z2amiga: unexpected ScreenMode.prefs in {d}")
    pre = d / "Prefs/Presets"
    pre.mkdir(exist_ok=True)
    for name, mid in MODES.items():
        b = bytearray(src)
        b[ID_OFFSET:ID_OFFSET + 4] = mid.to_bytes(4, "big")
        (pre / f"Z2RTG-{name}.prefs").write_bytes(bytes(b))
    print(f"installed in {d}: S/Z2Agent.rexx, S/User-Startup, Prefs/Presets/Z2RTG-*.prefs (active from the next boot)")


def redirect(line):
    """Send a command line's output to the agent's file: >>RAM:z2a.out right after the command name, where
    AmigaDOS takes redirection. Comments, Execute directives and lines with their own redirection stay."""
    t = line.strip()
    if not t or t[0] in ";." or any(w.startswith(">") or w.startswith("<") for w in t.split()[1:2]):
        return line
    if t[0] == '"':
        end = t.find('"', 1)
        if end < 0:
            return line
        name, rest = t[:end + 1], t[end + 1:]
    else:
        name, _, rest = t.partition(" ")
        rest = " " + rest if rest else ""
    return f"{name} >>RAM:z2a.out{rest}"


def sh(a, script, timeout=None):
    box = drive_dir(a) / "Z2Agent"
    if not box.is_dir():
        sys.exit(f"z2amiga: no {box}: run install, then boot the Amiga")
    for f in ("out", "rc", "out.tmp"):
        (box / f).unlink(missing_ok=True)
    tmp = box / "cmd.tmp"
    # FailAt 21: no command stops the script; rc is the last command's
    tmp.write_text("FailAt 21\n" + "".join(redirect(l) + "\n" for l in script.splitlines()), encoding="latin-1")
    tmp.rename(box / "cmd")
    t_end = time.time() + (timeout or a.timeout)
    while not (box / "out").exists():
        if time.time() > t_end:
            (box / "cmd").unlink(missing_ok=True)
            sys.exit("z2amiga: no answer from the agent (is the Amiga booted, with the agent installed?)")
        time.sleep(0.1)
    time.sleep(0.05)
    out = (box / "out").read_bytes().decode("latin-1")
    rc_f = box / "rc"
    rc_s = rc_f.read_text().strip() if rc_f.exists() else "0"
    rc = int(rc_s) if rc_s.isdigit() else 255           # "timeout": the script ran more than 4 minutes
    for f in ("out", "rc"):
        (box / f).unlink(missing_ok=True)
    return out, rc


def ipc(a, *args):
    path = a.socket or os.environ.get("Z2AMIGA_SOCKET") or str(DEF_SOCKET)
    cwd = os.getcwd()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(10)
        # a socket path is limited to 107 bytes: connect from its directory, by name
        os.chdir(os.path.dirname(os.path.abspath(path)))
        try:
            s.connect(os.path.basename(path))
        finally:
            os.chdir(cwd)
        s.sendall(("\t".join(args) + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    return buf.decode(errors="replace").rstrip("\n")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--drive", help="the DH0 host directory")
    p.add_argument("--socket", help="Amiberry's IPC socket")
    p.add_argument("--timeout", type=float, default=30.0)
    p.add_argument("cmd", choices=["install", "sh", "mode", "ipc"])
    p.add_argument("args", nargs="*")
    a = p.parse_args()
    if a.cmd == "install":
        install(a)
    elif a.cmd == "sh":
        out, rc = sh(a, " ".join(a.args) if a.args else sys.stdin.read())
        sys.stdout.write(out)
        sys.exit(min(rc, 255))
    elif a.cmd == "mode":
        if len(a.args) != 1 or a.args[0] not in MODES:
            sys.exit("z2amiga: mode " + " | ".join(MODES))
        f = f"SYS:Prefs/Presets/Z2RTG-{a.args[0]}.prefs"
        # ENV: is what IPrefs watches (it reopens Workbench), ENVARC: keeps it over a reboot
        out, rc = sh(a, f"Copy {f} ENVARC:Sys/ScreenMode.prefs\nCopy {f} ENV:Sys/ScreenMode.prefs\n")
        sys.stdout.write(out)
        if rc:
            sys.exit(f"z2amiga: rc {rc}")
        print(f"Workbench: {a.args[0]} (IPrefs reopens it now)")
    else:
        if not a.args:
            sys.exit("z2amiga: ipc COMMAND [ARGS...] (HELP lists them)")
        print(ipc(a, *a.args).replace("\t", "\n"))


if __name__ == "__main__":
    main()
