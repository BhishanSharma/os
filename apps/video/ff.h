/* ff.h - the video player's FFmpeg decoder (ff.c): decodes on a thread of
 * its own, a few pictures ahead, already at the window's size. */
#ifndef FF_H
#define FF_H
extern int ff_w, ff_h, ff_rate;            /* the picture's size; the sound's sample rate */
extern double ff_duration;                 /* seconds, 0 if not known */
int ff_open(const char *path, const char **why);   /* 0, or -1 and why */
const char *ff_codecs(void);               /* "h264 + aac" */
int ff_has_audio(void);
int ff_sound(void);                        /* send the sound to the 48 kHz stream (sound_open first) */
int ff_start(int w, int h, const int *xmap);   /* pictures at w x h (xmap: source column per x); start decoding */
int ff_next(unsigned int *dst);            /* the picture due now into dst: 1, 0 none yet, -1 over */
double ff_time(void);
void ff_pause(int on);
void ff_seek(double t);
void ff_stats(long *decoded, long *late);  /* pictures made ready; skipped as too late */
void ff_close(void);
#endif
