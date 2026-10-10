/* image.h - decoding PNG, BMP and PPM files into pixels.
 * Pixels are 0x00RRGGBB, row by row; transparent parts are blended over a
 * grey checkerboard. Everything uses integer arithmetic (no floating point
 * in programs). */
#ifndef IMAGE_H
#define IMAGE_H

#include <stddef.h>

typedef struct {
    int width, height;
    unsigned int *pixels;      /* width * height, malloc'd */
    const char *format;        /* "PNG", "BMP", "PPM" */
} image_t;

/* Decode a whole file in memory. Returns 0, or -1 with *error set to a
 * short reason ("not an image", "interlaced ... not supported", ...). */
int image_decode(const unsigned char *data, size_t size, image_t *img, const char **error);

/* Read and decode a file. */
int image_load(const char *path, image_t *img, const char **error);

void image_free(image_t *img);

/* Inflate a zlib stream (RFC 1950/1951) into `out` (exactly `out_size`
 * bytes expected). Returns the number of bytes written, or -1. */
long zlib_inflate(const unsigned char *src, size_t src_size, unsigned char *out, size_t out_size);

#endif
