# picozorro: the real PicoZorro card in Amiberry

The [PicoZorro](https://github.com/maximevince/PicoZorro) card (an RP2350 on the Zorro II bus) as a board in
the emulated Amiga, with the **real card in the loop**: every bus cycle to the board goes over a debug probe's
UART to the RP2350, which runs the firmware's own register window (Autoconfig slave, network, USB, MPEG,
update, boot ROM). The Amiga drivers, the boot ROM and `pzflash` run unchanged, against the real W5500 and USB
host. The probe's SWD port flashes the card and can stream its defmt log.

```
Amiberry                              z2bridged.py --port none            debug probe        RP2350
  zboard_expansion.cpp                  plugins/picozorro/peer.py          (Raspberry Pi     uart-backplane
  picozorro_plugin.cpp  -- 'D' -->      COBS + CRC16, stop and wait,  -->   Debug Probe)  --> firmware
  (batches, waits on reads) <-- 'D' --  retries, /INT notes, card log  <--  UART  /  SWD      GPIO0/1, SWD
```

## Why reads wait here

`docs/writing-a-plugin.md` asks that reads be answered from the plugin's own model. This board is the
exception on purpose: the card's registers *are* the firmware, and the point is to run that firmware. So:

- **Writes are posted**: batched into backplane ops (`WRITE`, `WRITE_N` for runs to one register such as
  TX_DATA, `WINDOW` for A16), sent at the next read, at each tick (64 us line) or when a batch is full. Word
  writes to the $C0-$FE data alias go as TX_DATA writes, which the register rules say they are. A frame the
  driver sends with `movem.l` through the alias is then one `WRITE_N` (2 bytes a word on the UART) instead of
  a 5-byte `WRITE` per word.
- **A read** ends its batch, which goes out, and waits for the card's reply: to the emulated CPU, a long XRDY
  wait state. Emulated time stands still; wall-clock time does not. Over a USB-serial probe expect about 1 ms a
  read (0.05 ms through a pty to a host stand-in), so polling loops and the boot stream are slow but exact.
- **Received frames** are the exception: the first RX_DATA (or $C0-$FE alias) read after RX_LEN fetches the
  whole frame with one `READ_N`, and the frame's other reads are answered from it. That is exact by the
  register rules (RX_LEN holds until RX_DONE, RX_DATA reads only advance, RX_DONE drops the rest). Without it a
  full frame costs about 760 round trips, and a LAN's broadcasts alone keep the emulated Amiga busy.
  `PICOZORRO_RX_PREFETCH=0` turns it off.
- **/INT** arrives as notification datagrams; `tick` returns /INT2 (default) or /INT6 while the card holds it.
- **A lost /INT note**: notes are not retried, so one can be lost on the UART. The card repeats an asserted level every
  100 ms but never a release, so if /INT is held and no note comes for 250 ms, the plugin releases it. Without
  that, one lost release keeps the Amiga in its interrupt servers, each one reading the card.
- **Autoconfig** is Amiberry's, built from the plugin's description (2011/$5A, 128 KB). At `configured()` the
  plugin resets the card, reads its Autoconfig ROM (logged, compared) and writes the base as
  expansion.library would; the card is then configured at the same address. An Amiga reset resets the card.
- No daemon, or the card stopped answering: reads give $FFFF at once (logged), until the peer reports the card
  is back; reset the Amiga then.

The board: A16 = 0 the register window (`docs/REGISTERS.md`), A16 = 1 the USB / MPEG / update window. The card
decodes A7-A1 and A16, so each 64 KB half aliases its 256 bytes.

## Messages

| Direction | Record | Meaning |
|---|---|---|
| plugin -> peer | `D` + request | a backplane request datagram (pz-core `backplane.rs`), seq in its header, always `F_REPLY` |
| peer -> plugin | `D` + datagram | the card's reply, or its /INT notification (flag $40) |
| peer -> plugin | `X` + seq (BE16) | no reply after the retries; the plugin stops waiting for that seq |
| peer -> plugin | `L` + 0/1 | the UART is open / the card answers again after a give-up |

The peer keeps one request on the line (stop and wait). No reply within 50 ms plus 20 us a byte of the request:
the same datagram again with the same seq, three times, then `X`; the firmware's reply cache answers a repeated seq without running it twice, so a
retried FIFO write does not land twice. The card's `LO` frames become daemon log lines `card: ...`.

## The card

A bare RP2350**B** module (a Core2350B, as on the One TH) on the bench, or a card out of its slot, with a Debug
Probe on SWD and its UART crossed to GPIO0 (card TX) and GPIO1 (card RX), common ground. Never in an Amiga:
GPIO0/1 are Zorro data lines. An Ethernet chip on SPI1 (GPIO40 MISO, 41 /CS, 42 SCK, 43 MOSI) and a USB
device on the RP2350's port are optional. The chip is a W5500 by default, or an ENC28J60 with
`PICOZORRO_NIC=enc28j60` for flash.sh. Without a W5500 the firmware keeps the link down. The ENC28J60 driver
cannot tell a missing chip, polls instead of using /INT, is 10BASE-T only and keeps its own receive filter
(the MAC filter switch does nothing).

```
PICOZORRO_SRC=~/source/repos/PicoZorro plugins/picozorro/flash.sh      # build (uart-backplane) and flash over SWD
```

The image has a boot ROM only when `firmware/boot.img` exists in the PicoZorro checkout (`make boot` there).
Without one, run Amiberry with `PICOZORRO_BOOTROM=0`; the plugin also notices at the first Autoconfig and
drops the DiagArea from the next reset.

## Running

```
hosts/amiberry/build.sh ~/amiberry-picozorro/src picozorro      # an Amiberry with this plugin compiled in

PICOZORRO_UART=/dev/serial/by-id/usb-Raspberry_Pi_Debugprobe_...-if01 \
  .../Zorro2-Tang25k/zorro2-tang25k/sw/tools/host/z2bridged.py --port none --peers $PWD/plugins

ZBOARD_DAEMON=default ~/amiberry-picozorro/src/build/amiberry -f a2000.uae   # with picozorro_rom_file=:ENABLED
```

Networking on the emulated Amiga: [PrismNet.md](PrismNet.md) (the boot ROM's driver and the installer's disk
copy, the interface, throughput).

| Variable | Where | Default | Meaning |
|---|---|---|---|
| `PICOZORRO_UART` | daemon | (required) | the probe's UART device |
| `PICOZORRO_BAUD` | daemon | 1000000 | the firmware's `pz_hal::backplane::BAUD` |
| `PICOZORRO_RTT_ELF` | daemon | unset | run `probe-rs attach` on this ELF and log the card's defmt lines (`rtt: ...`) |
| `PICOZORRO_PROBE` | daemon, flash.sh | unset | probe-rs `--probe` selector |
| `PICOZORRO_PROBE_RS` | daemon | probe-rs | the probe-rs binary |
| `PICOZORRO_BOOTROM` | Amiberry | 1 | 0: no DiagArea (a card without a boot image) |
| `PICOZORRO_INT` | Amiberry | 2 | 2 or 6: the level /INT raises (the card's jumper) |
| `PICOZORRO_TIMEOUT_MS` | Amiberry | 5000 | a read's backstop wait; the peer gives up sooner (four tries, a quarter of a second for a small request) |
| `PICOZORRO_RX_PREFETCH` | Amiberry | 1 | 0: read received frames word by word (a round trip each) |

**RTT is for debugging only.** Polling RTT over SWD disturbs the UART on the same probe. With a Raspberry Pi
Debugprobe, about 4% of frames are lost. A `probe-rs attach` during a live session also stalls the card long
enough that its UART receive buffer overflows (`WARN Overrun error` in the card log). That loses requests
outright, and each lost read gives the Amiga $FFFF. Leave `PICOZORRO_RTT_ELF` unset in normal runs, and read
the defmt log with the Amiga idle.

**Emulated time is slow.** Emulated time stands still during every wait for a read. A tool that measures by
the emulated clock (a timed test, a benchmark, a timeout) therefore runs about 100 times longer in
wall-clock time: `pztest 5` takes about 20 minutes. Use `pztest 1`.

## Tests

```
python3 plugins/picozorro/test_picozorro.py --daemon .../sw/tools/host/z2bridged.py --card CMD    # stand-in
python3 plugins/picozorro/test_picozorro.py --daemon .../sw/tools/host/z2bridged.py --uart PATH   # real card
```

The plugin through the interface (a ctypes host whose channel is a real zboard client of the daemon), the
daemon with `--port none`, the peer, and a card: the description; the card's Autoconfig ROM at `configured()`;
MAGIC in both windows ("PZ", "PU") and the 256-byte alias; SCRATCH by word and odd byte; a 1000-write
`WRITE_N` run; a write posted at a tick; reset and a second Autoconfig; received frames, read as
picozorro.device reads them, each in one request, with a read past the end giving $0000 and the IP and UDP
checksums of UDP frames verified; sent frames, UDP broadcasts sent through the alias as the driver sends them
and received on the PC by a socket (the kernel checks their checksums), compared byte for byte; $FFFF without a channel; the read latency. The RX checks need a cable and some
traffic and are skipped without (`--rx-frames`, `--rx-secs`). Large frames come from a broadcast sent from
the PC during the run, such as a 1400-byte UDP datagram to the LAN's broadcast address every 0.3 s. `--card` takes a command that speaks the backplane on stdin/stdout, put behind a pty, such as
pz-core's `Window` and `backplane::execute` built for the host; with `PZSIM_DROP=1` a stand-in that drops
first tries checks the peer's retries.
