#!/bin/sh
# Build the PicoZorro firmware for the card in the loop (uart-backplane: the register window served on UART0,
# GPIO0 TX / GPIO1 RX, 1 Mbaud) and flash it over SWD with probe-rs. NOT for a card in an Amiga: GPIO0/1 are
# Zorro data lines there. A bare RP2350B module (Core2350B) on the bench, or the card out of its slot.
#
#   PICOZORRO_SRC=~/source/repos/PicoZorro flash.sh [--build-only]
#
# Environment: PICOZORRO_SRC (the PicoZorro checkout, required), PICOZORRO_NIC (w5500, the default, or
# enc28j60: the Ethernet chip on SPI1, GPIO40 MISO / 41 CS / 42 SCK / 43 MOSI), PICOZORRO_PROBE (probe-rs --probe
# VID:PID[:SERIAL] when several probes are attached), PICOZORRO_SWD (probe-rs, the default when it is
# installed, or openocd), PICOZORRO_CMSIS_IF and PICOZORRO_CMSIS_SERIAL (openocd: the CMSIS-DAP USB interface
# number and the adapter serial, for a probe with several SWD ports), OPENOCD_SCRIPTS (default
# /usr/share/openocd/scripts), FEATURES (default below; mpeg is added when
# arm-none-eabi-gcc is there). A boot ROM is embedded when PICOZORRO_SRC/firmware/boot.img exists (`make boot`
# there); without one, run Amiberry with PICOZORRO_BOOTROM=0.
# The ELF is copied to PICOZORRO_SRC/firmware/target/backplane/picozorro.elf: give it to the peer as
# PICOZORRO_RTT_ELF for the defmt log over SWD.
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
: "${PICOZORRO_SRC:?set PICOZORRO_SRC to the PicoZorro checkout}"
FW="$PICOZORRO_SRC/firmware"
if [ -z "$FEATURES" ]; then
    FEATURES=nic-${PICOZORRO_NIC:-w5500},xrdy-direct,int-nfet,usb-host,uart-backplane
    command -v arm-none-eabi-gcc >/dev/null && FEATURES="$FEATURES,mpeg"
fi
[ -f "$FW/boot.img" ] && echo "boot ROM: $FW/boot.img" || echo "boot ROM: none (Amiberry: PICOZORRO_BOOTROM=0)"
(cd "$FW" && cargo build --release --bin picozorro --no-default-features --features "$FEATURES")
OUT="$FW/target/backplane"
mkdir -p "$OUT"
cp "$FW/target/thumbv8m.main-none-eabihf/release/picozorro" "$OUT/picozorro.elf"
echo "ELF: $OUT/picozorro.elf ($FEATURES)"
[ "$1" = "--build-only" ] && exit 0
SWD=${PICOZORRO_SWD:-$(command -v probe-rs >/dev/null && echo probe-rs || echo openocd)}
if [ "$SWD" = openocd ]; then
    ADAPTER=""
    [ -n "$PICOZORRO_CMSIS_SERIAL" ] && ADAPTER="adapter serial $PICOZORRO_CMSIS_SERIAL; "
    [ -n "$PICOZORRO_CMSIS_IF" ] && ADAPTER="${ADAPTER}cmsis-dap usb interface $PICOZORRO_CMSIS_IF; "
    openocd -s "${OPENOCD_SCRIPTS:-/usr/share/openocd/scripts}" -f interface/cmsis-dap.cfg \
        -c "${ADAPTER}adapter speed 5000" -f target/rp2350.cfg \
        -c "gdb port disabled; telnet port disabled; tcl port disabled" \
        -c "program $OUT/picozorro.elf verify reset exit"
else
    set -- --chip RP235x
    [ -n "$PICOZORRO_PROBE" ] && set -- "$@" --probe "$PICOZORRO_PROBE"
    probe-rs download "$@" "$OUT/picozorro.elf"
    probe-rs reset "$@"
fi
echo "flashed and reset ($SWD)"
