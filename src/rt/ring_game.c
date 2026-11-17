/* Ring-specific hooks and tweaks (natives listed in tools/native_ring.txt). */
#include <stdlib.h>
#include "rt.h"

/* window.c: WASD -> arrow keys while this says so (none for Ring yet) */
int game_wasd_active(void) { return 0; }

/* 439b60 (aApplication::GameZoneOnCD): which CD a game zone is on.  The game
   compares it with the number in D:\data\cd.ini before it enters a zone
   (402392, 43b93d) and asks for another disc when they differ.  The port's
   D:\ holds all four CDs (cd.ini says 1), so at those two places the answer
   is "the disc in the drive"; everywhere else the real number. */
/* 40b070 (key handler, wndproc WM_CHAR / Delete): for Esc it waits for the
   key's release and then throws away every queued keyboard message,
   including that WM_KEYUP.  The wndproc's "key held" flag 0x4a04f0 (set on
   WM_KEYDOWN, cleared on WM_KEYUP) then stays set, and F12 (main menu) is
   ignored until some other key is pressed - in the original game too, after
   skipping a cutscene with Esc.  The key is up by then: clear the flag. */
void N_key_char(void)
{
    u32 key = RD32(cpu.r[R_ESP] + 4);
    rt_wrap_orig("N_key_char", NULL)();
    if (key == 0x1b) WR32(0x4a04f0u, 0);
}

void N_zone_cd(void)
{
    u32 ra = RD32(cpu.r[R_ESP]);
    rt_wrap_orig("N_zone_cd", NULL)();
    if ((ra == 0x402392u || ra == 0x43b93du) && cpu.r[R_EAX] != 0) {
        static int n;
        if (n++ < 20) rt_log("disc check: zone wants CD %u, all discs present\n", cpu.r[R_EAX]);
        cpu.r[R_EAX] = 1;
    }
}
