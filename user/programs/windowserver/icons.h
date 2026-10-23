// icons.h - the window server's icons: drawn from a few strokes on a 24 x 24
// grid, so an icon can be any colour (its item's state). No pictures.
#ifndef WS_ICONS_H
#define WS_ICONS_H

#include <ui.h>

enum { GL_NONE, GL_DOOR, GL_APPS, GL_FOLDER, GL_LAYERS, GL_COMPASS, GL_GEAR, GL_USER, GL_SEARCH, GL_BELL,
       GL_WIFI, GL_SOUND, GL_BATTERY, GL_MOON, GL_SUN, GL_POWER, GL_TERM, GL_DOC, GL_MUSIC, GL_GLOBE,
       GL_VIDEO, GL_PAINT, GL_PULSE, GL_INFO, GL_COMPUTER, GL_IMAGE, GL_MOUSE, GL_LOGO, GL_MIN, GL_MAX,
       GL_RESTORE, GL_CLOSE, GL_COUNT };

void glyphs_init(void);                   /* once, before the first gl_draw */
/* An icon, `size` pixels square (24 * S is the design size), in `color`.
 * `level` (GL_BATTERY): the charge in percent, drawn inside. */
void gl_draw(ui_canvas *cv, int x, int y, int size, int id, unsigned color, int alpha, int level);
/* The glyph for an app.cfg icon name; 0: none (a picture or a letter instead). */
int glyph_for(const char *icon);
/* The integer square root (also for the pointer and the wallpaper). */
int isq(long n);

#endif
