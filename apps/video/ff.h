/* ff.h - the video player's FFmpeg decoder (ff.c) */
#ifndef FF_H
#define FF_H
extern int ff_w, ff_h, ff_rate;            /* the picture's size; the sound's sample rate */
extern double ff_duration;                 /* seconds, 0 if not known */
int ff_open(const char *path, const char **why);   /* 0, or -1 and why */
const char *ff_codecs(void);               /* "h264 + aac" */
int ff_has_audio(void);
int ff_sound(void);                        /* send the sound to the 48 kHz stream (sound_open first) */
int ff_decode(double elapsed);             /* move the clock on, decode -> 0 when it is over */
int ff_picture_ready(void);                /* a new picture is due */
void ff_picture(unsigned int *dst, int dw, int dh, const int *xmap);   /* it, scaled */
double ff_time(void);
void ff_seek(double t);
void ff_close(void);
#endif
