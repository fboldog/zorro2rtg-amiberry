# PrismNet with the PicoZorro card

Setting up the PrismNet TCP/IP stack (1.0.18, open source, lwIP based) on an Amiga whose PicoZorro card has
its boot ROM on. Kickstart then loads picozorro.device from the card at boot, so there is no driver file on
disk, and the PrismNet installer cannot see the driver.

## The driver: use the boot ROM's copy, not a disk copy

PrismNet's installer insists on a driver file in `DEVS:Networks`. If none is there, it asks for one and copies
it in. PrismNet itself opens a bare driver name from `DEVS:Networks` first and falls back to the name alone
(`port/sanaif.c`, `sanaif_open`).

With a copy in `DEVS:Networks`, PrismNet opens that file by its path, and AmigaOS can load it as a second
picozorro.device next to the boot ROM's. The driver does not check whether another copy already runs, so two
drivers would share one card. So:

1. Run `Install_PrismNet`. When it asks for the driver, give it any copy of picozorro.device (from the
   PicoZorro Amiga package, or `amiga/picozorro.device/picozorro.device` in the PicoZorro repository), unit 0.
2. After the installer finishes, delete the copy it made:

   ```
   Delete DEVS:Networks/picozorro.device
   ```

3. Keep the interface's `DEVICE=picozorro.device UNIT=0` (no path). PrismNet's try in `DEVS:Networks` fails,
   and the bare name finds the copy the boot ROM loaded.

Without the boot ROM (`pzflash BOOTROM OFF`, or a card without a boot image), keep the copy in
`DEVS:Networks`: it is then the only one.

## The interface

`ENVARC:PrismNet/interfaces/eth0`, with DHCP:

```
DEVICE=picozorro.device UNIT=0
CONFIG=DHCP
```

or a fixed address:

```
DEVICE=picozorro.device UNIT=0
CONFIG=STATIC ADDRESS=192.168.1.44 NETMASK=255.255.255.0 GATEWAY=192.168.1.1
```

plus `NAMESERVER 192.168.1.1` in `ENVARC:PrismNet/dns`. If DHCP gives an address outside the LAN's range,
another DHCP server is answering on the cable's segment (a router or extender in its factory setup, often
192.168.0.x). `PrismNet STATUS` shows which server answered.

The installer starts PrismNet from `S:User-Startup` (`C:PrismNet START`). `PrismNet STATUS`, `PrismNet LOG`
and `ping` check it.

## With the card in Amiberry (this plugin)

- PrismNet needs about 290 KB. Give the emulated Amiga enough fast RAM (8 MB on the bench). With 2 MB, Neptune
  and PrismNet together pushed allocations into chip RAM.
- Network traffic runs through the UART: about 57 KiB/s received and 50 KiB/s sent, timed on the PC. Tools
  that time themselves on the Amiga (AmiSpeedTest) report far higher figures, because emulated time stops
  while the Amiga waits for the card. Use ttcp mode against a peer on the PC to measure.
- AmiSpeedTest needs `mathieeesingbas.library` in `LIBS:`. The A2000 Kickstart 3.1.4 ROM does not have it.
