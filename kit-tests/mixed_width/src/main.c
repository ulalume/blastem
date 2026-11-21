// Test ROM for frames that mix H32 and H40 lines (README.fork.md, "Mixed H32/H40 frames").
// H40 above line 112, H32 below it: the H interrupt switches the mode, the V interrupt restores H40.
// Build: sgdkx make release (Makefile next to this directory), ROM: out/release/rom.bin.
#include <genesis.h>

static HINTERRUPT_CALLBACK hint(void)
{
    *(vu16 *)VDP_CTRL_PORT = 0x8C00;   // register 12: H32
}

static void vint(void)
{
    *(vu16 *)VDP_CTRL_PORT = 0x8C81;   // register 12: H40
}

int main(bool hard)
{
    (void)hard;
    char row[41];
    VDP_setScreenWidth320();
    for (u16 x = 0; x < 40; x++) row[x] = '0' + (x % 10);
    row[40] = 0;
    for (u16 y = 0; y < 28; y++) VDP_drawText(row, 0, y);
    VDP_drawText(" H40 above line 112 ", 10, 6);
    VDP_drawText(" H32 below ", 10, 20);
    SYS_disableInts();
    SYS_setVIntCallback(vint);
    SYS_setHIntCallback(hint);
    VDP_setHIntCounter(111);
    VDP_setHInterrupt(TRUE);
    SYS_enableInts();
    kprintf("MIXED READY");
    while (TRUE) SYS_doVBlankProcess();
    return 0;
}
