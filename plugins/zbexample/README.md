# zbexample

The example board of `docs/writing-a-plugin.md`: the smallest complete zboard plugin with a peer, to copy when
starting a new one. A 64 KB Zorro II I/O board with no display, a packet mailbox shaped like a network card.
Its peer owns its own device, a UDP socket, and needs no Nano 20K.

| File | What it is |
|---|---|
| `zbexample_plugin.cpp` | The board: registers, received packets kept in the plugin ("pushed state"), /INT2 |
| `peer.py` | The daemon half: packets out to a UDP socket, datagrams back as received packets |
| `plugin.json` | For `hosts/amiberry/integrate.py`: the expansion adapter, config name `zbexample` |
| `amiga/zbping.c` | Amiga tool: finds the board, sends a packet, prints the one that comes back |

## Registers (word access, board offsets)

| Offset | Name | Access | Function |
|---|---|---|---|
| `$00` | `ID` | R | `$5A45` |
| `$02` | `STATUS` | R | bit 0 `RX_AVAIL`: a received packet waits; bit 1 `LINK`: the peer's device is up |
| `$04` | `CTRL` | RW | bit 0 `IRQ_EN`: /INT2 while `RX_AVAIL` |
| `$06` | `TX_DATA` | W | the next byte of the packet being written (low byte) |
| `$08` | `TX_SEND` | W | any value: send the packet written so far |
| `$0A` | `RX_LEN` | R | the oldest received packet's length, 0 when none waits |
| `$0C` | `RX_DATA` | R | its next byte; reading moves on |
| `$0E` | `RX_DONE` | W | any value: drop the oldest packet |

Autoconfig: manufacturer `$5A32` (placeholder), product `$10`, `er_Type` `$C1` (Zorro II, 64 KB).

## Messages

| Direction | Record | Meaning |
|---|---|---|
| plugin to peer | `T` + bytes | a packet the Amiga sent (1 to 1514 bytes) |
| peer to plugin | `R` + bytes | a packet that arrived |
| peer to plugin | `L` + 1 byte | the device is up (1) or down (0) |

The peer sends each packet to `ZBEXAMPLE_DEST` (default: its own socket, a loopback) and binds
`ZBEXAMPLE_BIND` (default `127.0.0.1:0`); both are read from the daemon's environment.

## Running it

```
hosts/amiberry/build.sh ~/amiberry-zorro2/src zbexample     # Amiberry with this board compiled in
python3 sw/tools/host/z2bridged.py --port none                # the daemon, without a Nano (Zorro2-Tang25k project)
ZBOARD_DAEMON=default amiberry ...                            # a config with zbexample_rom_file=:ENABLED
```

In the emulated Amiga, `zbping hello` prints the board's address and `received 5 bytes ... "hello"`
(checked in Amiberry v8.5.0 on 2026-11-21: the board at `$E90000`, the packet back after 20 ms).

## Tests

`sw/tests/test_zboard_example.py` in the Zorro2-Tang25k project (`make -C sim host`): the plugin through a
ctypes host with a fake channel, and the peer through the daemon without a Nano, end to end over its UDP socket.
