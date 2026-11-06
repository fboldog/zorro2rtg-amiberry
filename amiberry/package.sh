#!/bin/sh
# Bundle the Zorro2RTG Amiberry build with one machine, to start by hand (docs/driver.md, "Board in the
# loop"): Amiberry's GUI opens with the config loaded, Start boots it, and with a Nano 20K on USB every
# write to the emulated board is mirrored to the real one, so Workbench shows on the module's HDMI too.
#   sw/emu/amiberry/package.sh [OUT.zip] [CONFIG]
#   default OUT: ~/Zorro2RTG-Amiberry.zip, CONFIG: "A2000 Zorro2RTG" (in ~/amiberry-zorro2)
# Takes the binary from sw/emu/amiberry/build.sh and the drive and Kickstart that CONFIG uses. Those are
# AmigaOS 3.1.4 files: the zip is for personal use, never commit or publish it.
# Needs the system Amiberry package for its data files (/usr/share/amiberry), as the build does.
set -e
OUT=${1:-$HOME/Zorro2RTG-Amiberry.zip}
CFG=${2:-A2000 Zorro2RTG}
ISO=$HOME/amiberry-zorro2
BIN=$ISO/src/build/amiberry
UAE="$ISO/Amiberry/Configurations/$CFG.uae"
[ -x "$BIN" ] || { echo "package.sh: no $BIN, run sw/emu/amiberry/build.sh first" >&2; exit 1; }
[ -f "$UAE" ] || { echo "package.sh: no config $UAE" >&2; exit 1; }
KICK=$(sed -n 's/^kickstart_rom_file=//p' "$UAE")
DRIVE=$(sed -n 's/^filesystem2=rw,DH0:DH0:\(.*\)\/,0$/\1/p' "$UAE")
[ -f "$KICK" ] && [ -d "$DRIVE" ] || { echo "package.sh: Kickstart or DH0 of $UAE not found" >&2; exit 1; }

STAGE=$(mktemp -d)
P=$STAGE/Zorro2RTG-Amiberry
mkdir -p "$P/Amiberry/Configurations" "$P/Amiberry/ROMs" "$P/Amiberry/HardDrives" "$P/.config/amiberry"
for d in CDROMs Floppies LHA RP9 SaveStates Screenshots; do mkdir -p "$P/Amiberry/$d"; done
cp "$BIN" "$P/amiberry"
# the bitstream of the current build, if there is one (fpga/zorro2rtg/build.sh)
FS=$(cd "$(dirname "$0")/../../.." && pwd)/fpga/zorro2rtg/impl/pnr/zorro2rtg.fs
[ -f "$FS" ] && cp "$FS" "$P/zorro2rtg.fs" || echo "package.sh: no bitstream in fpga/zorro2rtg/impl, none bundled" >&2
cp "$KICK" "$P/Amiberry/ROMs/"
cp -a "$DRIVE" "$P/Amiberry/HardDrives/DH0"
rm -f "$P/Amiberry/HardDrives/DH0/Prefs/Env-Archive/Prism.booting"   # left by a stopped PrismRTG start
cp -a "$ISO/Amiberry/Visuals" "$P/Amiberry/" 2>/dev/null || true
# @PKG@ is replaced with the bundle's folder by start.sh, so the folder can live anywhere
sed -e "s|^kickstart_rom_file=.*|kickstart_rom_file=@PKG@/Amiberry/ROMs/$(basename "$KICK")|" \
    -e "s|^filesystem2=rw,DH0:DH0:.*|filesystem2=rw,DH0:DH0:@PKG@/Amiberry/HardDrives/DH0/,0|" \
    -e "s|^uaehf0=dir,rw,DH0:DH0:.*|uaehf0=dir,rw,DH0:DH0:@PKG@/Amiberry/HardDrives/DH0/,0|" \
    -e "s|^amiberry\.\([a-z]*\)_path=.*/Amiberry/\(.*\)|amiberry.\1_path=@PKG@/Amiberry/\2|" \
    -e "/^diskimage[0-9]*=/s|=.*|=|" \
    "$UAE" > "$P/Amiberry/Configurations/Zorro2RTG Nano20K.uae.in"
# the recent disk and CD lists name files outside the bundle: left out
sed -e "s|$ISO/|@PKG@/|g" -e "/^Diskfile=/d" -e "/^CDfile=/d" "$ISO/.config/amiberry/amiberry.conf" \
    > "$P/.config/amiberry/amiberry.conf.in"
if grep -q "/home/" "$P/Amiberry/Configurations/Zorro2RTG Nano20K.uae.in" "$P/.config/amiberry/amiberry.conf.in"; then
    echo "package.sh: absolute paths left in the bundled configs:" >&2
    grep -n "/home/" "$P/Amiberry/Configurations/Zorro2RTG Nano20K.uae.in" "$P/.config/amiberry/amiberry.conf.in" >&2
    exit 1
fi

cat > "$P/start.sh" <<'EOF'
#!/bin/bash
# Starts this Amiberry with its own settings (nothing in ~/Amiberry or ~/.config/amiberry changes) and the
# Zorro2RTG config loaded: press Start. With a Nano 20K running the zorro2rtg bitstream on USB, every write
# to the emulated board goes to the real one too (Z2RTG_BRIDGE; Z2RTG_BRIDGE=none turns it off).
set -e
P=$(cd "$(dirname "$0")" && pwd)
for f in "$P/Amiberry/Configurations/Zorro2RTG Nano20K.uae" "$P/.config/amiberry/amiberry.conf"; do
    sed "s|@PKG@|$P|g" "$f.in" > "$f"
done
if [ -z "$Z2RTG_BRIDGE" ]; then
    Z2RTG_BRIDGE=$(ls /dev/ttyUSB* 2>/dev/null | tail -1)     # the Nano 20K's UART is its last ttyUSB
fi
if [ -n "$Z2RTG_BRIDGE" ] && [ "$Z2RTG_BRIDGE" != none ]; then
    export Z2RTG_BRIDGE
    echo "Zorro2RTG: mirroring to the Nano 20K on $Z2RTG_BRIDGE (link messages appear here)"
else
    unset Z2RTG_BRIDGE
    echo "Zorro2RTG: no Nano 20K link, emulated board only"
fi
REAL_RT=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
RT=$P/.runtime; mkdir -p "$RT"; chmod 700 "$RT"
[ -n "$WAYLAND_DISPLAY" ] && [ "${WAYLAND_DISPLAY#/}" = "$WAYLAND_DISPLAY" ] && export WAYLAND_DISPLAY=$REAL_RT/$WAYLAND_DISPLAY
[ -S "$REAL_RT/pulse/native" ] && export PULSE_SERVER=unix:$REAL_RT/pulse/native
[ -S "$REAL_RT/pipewire-0" ] && export PIPEWIRE_REMOTE=$REAL_RT/pipewire-0
export DBUS_SESSION_BUS_ADDRESS=${DBUS_SESSION_BUS_ADDRESS:-unix:path=$REAL_RT/bus}
export XAUTHORITY=${XAUTHORITY:-$HOME/.Xauthority}
export HOME=$P XDG_CONFIG_HOME=$P/.config XDG_DATA_HOME=$P/.local/share XDG_RUNTIME_DIR=$RT
mkdir -p "$P/.local/share/amiberry"
exec "$P/amiberry" --config "$P/Amiberry/Configurations/Zorro2RTG Nano20K.uae" "$@"
EOF
chmod +x "$P/start.sh"

cat > "$P/README.txt" <<EOF
Zorro2RTG in Amiberry, mirrored to a Tang Nano 20K
Built $(date +%F) from $(git -C "$(dirname "$0")" rev-parse --short HEAD 2>/dev/null || echo "?") with config "$CFG".

1. Load the zorro2rtg bitstream into the Nano 20K (SRAM):
     openFPGALoader -b tangnano20k zorro2rtg.fs
   and connect its HDMI output to a monitor or capture card.
2. ./start.sh   (Amiberry's GUI opens with "Zorro2RTG Nano20K" loaded; press Start)
3. The Amiga boots on the emulator's own screen. When the RTG screen opens (Workbench at 1280x720x16),
   the same picture appears on the Nano 20K's HDMI output.

- The Nano 20K is found as the last /dev/ttyUSB*. Z2RTG_BRIDGE=/dev/ttyUSBn ./start.sh picks another port,
  Z2RTG_BRIDGE=none ./start.sh runs without it. Link messages and statistics print in the terminal.
- The link sends what the emulated Amiga writes, at about 290 KB/s, so the board can lag behind on large
  redraws; it catches up.
- Reads are answered by the emulated board, never by the real one.
- Needs the system Amiberry package (its data files in /usr/share/amiberry).
- The Kickstart ROM and the DH0 contents are AmigaOS 3.1.4 files: personal use only, do not share.
EOF

rm -f "$OUT"
(cd "$STAGE" && zip -qr "$OUT" Zorro2RTG-Amiberry)
rm -rf "$STAGE"
ls -l "$OUT"
