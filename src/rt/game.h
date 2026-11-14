/* The game's names (runtime and installer) */
#pragma once
#define GAME_TITLE "Ring: The Legend of the Nibelungen"
#define GAME_TITLE_FS "Ring - The Legend of the Nibelungen"   /* for file / folder / shortcut names (no ':') */
#define GAME_SHORT "ring"
#define GAME_ID    "RingNibelungen"
#define GAME_ENV   "RING_"
/* widescreen 360-degree views (hires_pano.c draw_strips, present.c menu):
   kept in the code, switched off - 1 brings back the Video > Widescreen menu */
#define WIDESCREEN_ENABLED 0
/* the game's four CDs and files that identify each (installer) */
#define GAME_DISCS 4
#define GAME_DISC_FILES { \
    { "RING.EXE", "DATA\\SY.AT2", "DATA\\RO.AT2", NULL }, \
    { "DATA\\NI.AT2", "DATA\\AS.AT2", NULL }, \
    { "DATA\\RH.AT2", "DATA\\WA.AT2", NULL }, \
    { "DATA\\FO.AT2", "DATA\\N2.AT2", NULL } }
/* icon: the game's ring.ico on CD 1 */
#define GAME_ICON_ICO "ring.ico"
#define GAME_ICON_LBM NULL
