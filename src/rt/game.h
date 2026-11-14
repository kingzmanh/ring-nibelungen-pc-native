/* The game's names (runtime and installer) */
#pragma once
#define GAME_TITLE "Ring: The Legend of the Nibelungen"
#define GAME_SHORT "ring"
#define GAME_ID    "RingNibelungen"
#define GAME_ENV   "RING_"
/* widescreen 360-degree views (hires_pano.c draw_strips, present.c menu):
   kept in the code, switched off - 1 brings back the Video > Widescreen menu */
#define WIDESCREEN_ENABLED 0
/* files that identify the game's CDs */
#define GAME_CD_FILES  { "RING.EXE", "DATA\\SY.AT2", "DATA\\RO.AT2", NULL }
#define GAME_CD2_FILES { "DATA\\NI.AT2", NULL }
/* icon: taken from the game exe's own resources */
#define GAME_ICON_ICO NULL
#define GAME_ICON_LBM NULL
