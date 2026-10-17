// The pictures image.c does not read itself: JPEG, GIF (the first frame),
// TGA and PSD, decoded by stb_image (third_party/stb, public domain / MIT).
// Built with SSE allowed (see the Makefile): stb_image is not written for
// integer-only code.
#include <stdlib.h>
#include <string.h>
#include <image.h>

#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#define STBI_ONLY_PSD
#define STBI_ASSERT(x) ((void)0)
#define STBI_MALLOC(n) malloc(n)
#define STBI_REALLOC(p, n) realloc(p, n)
#define STBI_FREE(p) free(p)
#define STBI_MAX_DIMENSIONS 16384
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

int image_decode_stb(const unsigned char *data, size_t size, image_t *img, const char **error) {
    int w, h, n;
    if (size > 0x7FFFFFFF) { *error = "too big"; return -1; }
    unsigned char *rgb = stbi_load_from_memory(data, (int)size, &w, &h, &n, 3);
    if (!rgb) { *error = stbi_failure_reason(); return -1; }
    unsigned int *pixels = malloc((size_t)w * h * 4);
    if (!pixels) {
        stbi_image_free(rgb);
        *error = "out of memory";
        return -1;
    }
    for (long i = 0; i < (long)w * h; i++)
        pixels[i] = (unsigned)rgb[3 * i] << 16 | (unsigned)rgb[3 * i + 1] << 8 | rgb[3 * i + 2];
    stbi_image_free(rgb);
    img->width = w;
    img->height = h;
    img->pixels = pixels;
    img->format = data[0] == 0xFF ? "JPEG" : data[0] == 'G' ? "GIF" : data[0] == '8' ? "PSD" : "TGA";
    return 0;
}
