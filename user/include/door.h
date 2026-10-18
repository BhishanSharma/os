// door.h - DOOR OS design tokens that are plain numbers (see design/tokens.json).
// Integer only. Colours live in ui.h / ui.c (ui_color), because they depend on
// the appearance. Sizes are in points: multiply by the window's scale.
#ifndef DOOR_H
#define DOOR_H

/* Spacing scale */
#define DOOR_SP_1   4
#define DOOR_SP_2   8
#define DOOR_SP_3   12
#define DOOR_SP_4   16
#define DOOR_SP_5   20
#define DOOR_SP_6   24
#define DOOR_SP_8   32
#define DOOR_SP_10  40
#define DOOR_SP_12  48
#define DOOR_SP_16  64

/* Radii */
#define DOOR_R_MICRO    4
#define DOOR_R_CONTROL  8
#define DOOR_R_CARD     12
#define DOOR_R_WINDOW   16
#define DOOR_R_LARGE    20
#define DOOR_R_FULL     999     /* capsule: clamped to half the height */

/* Every clickable thing is at least this big (hit area, not drawn size) */
#define DOOR_HIT_MIN    40

/* Icon sizes (24 is the design grid) */
#define DOOR_ICON_S 16
#define DOOR_ICON_M 20
#define DOOR_ICON_L 24
#define DOOR_ICON_XL 32

/* Type sizes in px at 1x */
#define DOOR_TYPE_DISPLAY 30
#define DOOR_TYPE_TITLE   18
#define DOOR_TYPE_SECTION 16
#define DOOR_TYPE_BODY    14
#define DOOR_TYPE_LABEL   13
#define DOOR_TYPE_CAPTION 12

/* Motion, milliseconds. Reduce Motion: use DOOR_MS_MICRO fades only. */
#define DOOR_MS_MICRO     120
#define DOOR_MS_STANDARD  200
#define DOOR_MS_MAJOR     260
#define DOOR_MS_OUT(ms)   ((ms) * 3 / 4)    /* leaving is faster than arriving */

/* Elevation: dy, blur radius (px) and alpha (0-255) for ui_shadow */
#define DOOR_E1_DY 2
#define DOOR_E1_BLUR 8
#define DOOR_E1_ALPHA 64
#define DOOR_E2_DY 8
#define DOOR_E2_BLUR 24
#define DOOR_E2_ALPHA 90

#endif