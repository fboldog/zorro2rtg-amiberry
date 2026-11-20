/* zbping: the example board's Amiga side (docs/writing-a-plugin.md). Finds zbexample in the expansion list,
 * sends a packet through it and waits for one to come back: with the board's peer in the zboard daemon on its
 * default loopback, the packet travels Amiga -> plugin -> daemon -> peer -> UDP socket -> peer -> plugin -> Amiga.
 *   zbping [TEXT]        (default "zbping")
 * Prints the board's address, then the packet received, or that none came within 2 s.
 * Build: m68k-amigaos-gcc -m68000 -O2 -noixemul -o zbping zbping.c   (bebbo's amiga-gcc)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <exec/types.h>
#include <libraries/configvars.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#include <stdio.h>
#include <string.h>

#define MFR 0x5A32          /* placeholder manufacturer ID, as in zbexample_plugin.cpp */
#define PROD 0x10
#define R(o) (*(volatile UWORD *)(board + (o)))
enum { ID = 0x00, STATUS = 0x02, CTRL = 0x04, TX_DATA = 0x06, TX_SEND = 0x08, RX_LEN = 0x0A, RX_DATA = 0x0C,
       RX_DONE = 0x0E };

struct ExpansionBase *ExpansionBase;

int main(int argc, char **argv)
{
    const char *text = argc > 1 ? argv[1] : "zbping";
    struct ConfigDev *cd;
    volatile UBYTE *board;
    int i, n, t, rc = 0;

    if (!(ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library", 0)))
        return 20;
    if (!(cd = FindConfigDev(NULL, MFR, PROD))) {
        printf("zbping: no zbexample board ($%04x/$%02x) in the expansion list\n", MFR, PROD);
        rc = 20;
        goto done;
    }
    board = (volatile UBYTE *)cd->cd_BoardAddr;
    printf("zbping: zbexample at $%08lx, %lu KB, ID $%04x, STATUS $%04x\n", (ULONG)board, cd->cd_BoardSize >> 10,
           R(ID), R(STATUS));
    while (R(STATUS) & 1)                   /* drop what waited from before */
        R(RX_DONE) = 0;
    n = strlen(text);
    for (i = 0; i < n; i++)
        R(TX_DATA) = (UBYTE)text[i];
    R(TX_SEND) = 0;
    for (t = 0; t < 100 && !(R(STATUS) & 1); t++)
        Delay(1);                           /* 20 ms */
    if (!(R(STATUS) & 1)) {
        printf("zbping: sent %d bytes, nothing came back in 2 s (is the daemon running with its peer?)\n", n);
        rc = 5;
        goto done;
    }
    n = R(RX_LEN);
    printf("zbping: received %d bytes after %d ms: \"", n, t * 20);
    for (i = 0; i < n; i++)
        putchar(R(RX_DATA) & 0xFF);
    printf("\"\n");
    R(RX_DONE) = 0;
done:
    CloseLibrary((struct Library *)ExpansionBase);
    return rc;
}
