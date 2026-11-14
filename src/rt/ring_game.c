/* Ring-specific hooks and tweaks (natives listed in tools/native_ring.txt). */
#include "rt.h"

/* window.c: WASD -> arrow keys while this says so (none for Ring yet) */
int game_wasd_active(void) { return 0; }
