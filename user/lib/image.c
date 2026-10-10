// image.c - PNG (with a small inflate), BMP and PPM decoders
#include <image.h>
#include <stdlib.h>
#include <string.h>
#include <os.h>

#define MAX_PIXELS (32L * 1024 * 1024)      /* 32 megapixels: 128 MiB decoded */

/* Transparent pixels: blended over 8x8 grey squares, like most viewers. */
static unsigned int blend(int x, int y, unsigned r, unsigned g, unsigned b, unsigned a) {
    unsigned bg = ((x >> 3) ^ (y >> 3)) & 1 ? 0x66 : 0x44;
    if (a != 255) {
        r = (r * a + bg * (255 - a)) / 255;
        g = (g * a + bg * (255 - a)) / 255;
        b = (b * a + bg * (255 - a)) / 255;
    }
    return (r << 16) | (g << 8) | b;
}

/* ===================== inflate (RFC 1951) ===================== */

typedef struct {
    const unsigned char *src;
    size_t src_size, pos;
    unsigned bitbuf;
    int bitcnt;
    unsigned char *out;
    size_t out_size, out_pos;
    int error;
} inflate_t;

typedef struct {
    unsigned short counts[16];      /* codes of each length */
    unsigned short symbols[320];    /* symbols in canonical order */
} huffman_t;

static int bits(inflate_t *s, int need) {
    while (s->bitcnt < need) {
        if (s->pos >= s->src_size) { s->error = 1; return 0; }
        s->bitbuf |= (unsigned)s->src[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    int v = (int)(s->bitbuf & ((1u << need) - 1));
    s->bitbuf >>= need;
    s->bitcnt -= need;
    return v;
}

static int build(huffman_t *h, const unsigned char *lengths, int n) {
    unsigned short offs[16];
    memset(h->counts, 0, sizeof(h->counts));
    for (int i = 0; i < n; i++) h->counts[lengths[i]]++;
    h->counts[0] = 0;
    int left = 1;                                   // reject over-subscribed codes
    for (int len = 1; len < 16; len++) {
        left = left * 2 - h->counts[len];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h->counts[len];
    for (int i = 0; i < n; i++)
        if (lengths[i]) h->symbols[offs[lengths[i]]++] = (unsigned short)i;
    return 0;
}

/* One symbol, reading the code a bit at a time (canonical Huffman). */
static int decode(inflate_t *s, const huffman_t *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= bits(s, 1);
        int count = h->counts[len];
        if (code - count < first) return h->symbols[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    s->error = 1;
    return -1;
}

static const unsigned short len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const unsigned char len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const unsigned short dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const unsigned char dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(inflate_t *s, const huffman_t *lit, const huffman_t *dist) {
    while (!s->error) {
        int sym = decode(s, lit);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->out_pos >= s->out_size) return -1;
            s->out[s->out_pos++] = (unsigned char)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            size_t len = len_base[sym] + (size_t)bits(s, len_extra[sym]);
            int d = decode(s, dist);
            if (d < 0 || d >= 30) return -1;
            size_t back = dist_base[d] + (size_t)bits(s, dist_extra[d]);
            if (back > s->out_pos || len > s->out_size - s->out_pos) return -1;
            unsigned char *o = s->out + s->out_pos;
            for (size_t i = 0; i < len; i++) o[i] = o[(long)i - (long)back];   // may overlap: byte by byte
            s->out_pos += len;
        }
    }
    return -1;
}

static int block_stored(inflate_t *s) {
    s->bitbuf = 0;                                 // to a byte boundary
    s->bitcnt = 0;
    if (s->pos + 4 > s->src_size) return -1;
    unsigned len = s->src[s->pos] | (unsigned)s->src[s->pos + 1] << 8;
    unsigned nlen = s->src[s->pos + 2] | (unsigned)s->src[s->pos + 3] << 8;
    s->pos += 4;
    if ((len ^ 0xFFFF) != nlen || s->pos + len > s->src_size || len > s->out_size - s->out_pos) return -1;
    memcpy(s->out + s->out_pos, s->src + s->pos, len);
    s->pos += len;
    s->out_pos += len;
    return 0;
}

static int block_fixed(inflate_t *s) {
    static huffman_t lit, dist;
    static int ready;
    if (!ready) {
        unsigned char lengths[288];
        int i = 0;
        for (; i < 144; i++) lengths[i] = 8;
        for (; i < 256; i++) lengths[i] = 9;
        for (; i < 280; i++) lengths[i] = 7;
        for (; i < 288; i++) lengths[i] = 8;
        build(&lit, lengths, 288);
        for (i = 0; i < 30; i++) lengths[i] = 5;
        build(&dist, lengths, 30);
        ready = 1;
    }
    return codes(s, &lit, &dist);
}

static int block_dynamic(inflate_t *s) {
    static const unsigned char order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    unsigned char lengths[320];
    huffman_t lencode, lit, dist;
    int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
    if (nlen > 286 || ndist > 30) return -1;
    memset(lengths, 0, sizeof(lengths));
    for (int i = 0; i < ncode; i++) lengths[order[i]] = (unsigned char)bits(s, 3);
    if (build(&lencode, lengths, 19) != 0) return -1;

    for (int i = 0; i < nlen + ndist; ) {
        int sym = decode(s, &lencode);
        if (sym < 0 || s->error) return -1;
        if (sym < 16) {
            lengths[i++] = (unsigned char)sym;
            continue;
        }
        int value = 0, repeat;
        if (sym == 16) {
            if (i == 0) return -1;
            value = lengths[i - 1];
            repeat = 3 + bits(s, 2);
        } else if (sym == 17) {
            repeat = 3 + bits(s, 3);
        } else {
            repeat = 11 + bits(s, 7);
        }
        if (i + repeat > nlen + ndist) return -1;
        while (repeat--) lengths[i++] = (unsigned char)value;
    }
    if (lengths[256] == 0) return -1;              // no end-of-block code
    if (build(&lit, lengths, nlen) != 0 || build(&dist, lengths + nlen, ndist) != 0) return -1;
    return codes(s, &lit, &dist);
}

long zlib_inflate(const unsigned char *src, size_t src_size, unsigned char *out, size_t out_size) {
    if (src_size < 2 || (src[0] & 0x0F) != 8 || ((src[0] << 8) | src[1]) % 31 != 0 || (src[1] & 0x20))
        return -1;                                 // not deflate, bad check, or a preset dictionary
    inflate_t s = { src, src_size, 2, 0, 0, out, out_size, 0, 0 };
    int last;
    do {
        last = bits(&s, 1);
        int type = bits(&s, 2);
        int r = type == 0 ? block_stored(&s) : type == 1 ? block_fixed(&s) : type == 2 ? block_dynamic(&s) : -1;
        if (r != 0 || s.error) return -1;
    } while (!last);
    return (long)s.out_pos;
}

/* ===================== PNG ===================== */

static unsigned be32(const unsigned char *p) {
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* Undo the per-row filters of a w x h (sub)image in place. Returns 0. */
static int unfilter(unsigned char *data, int rows, size_t rowbytes, int bpp) {
    unsigned char *prev = 0;
    for (int y = 0; y < rows; y++) {
        unsigned char *row = data + (size_t)y * (rowbytes + 1);
        int type = row[0];
        unsigned char *p = row + 1;
        for (size_t i = 0; i < rowbytes; i++) {
            int a = i >= (size_t)bpp ? p[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = prev && i >= (size_t)bpp ? prev[i - bpp] : 0;
            switch (type) {
                case 0: break;
                case 1: p[i] = (unsigned char)(p[i] + a); break;
                case 2: p[i] = (unsigned char)(p[i] + b); break;
                case 3: p[i] = (unsigned char)(p[i] + ((a + b) >> 1)); break;
                case 4: p[i] = (unsigned char)(p[i] + paeth(a, b, c)); break;
                default: return -1;
            }
        }
        prev = p;
    }
    return 0;
}

typedef struct {
    int width, height, depth, type, channels;
    unsigned char palette[256][4];
    int palette_size;
    int has_key;
    unsigned key[3];             /* tRNS colour key for grey / RGB, at sample depth */
} png_t;

/* Sample number `i` of a row (i = x * channels + channel). */
static unsigned sample(const unsigned char *row, size_t i, int depth) {
    if (depth == 8) return row[i];
    if (depth == 16) return (unsigned)row[i * 2] << 8 | row[i * 2 + 1];
    size_t bit = i * (size_t)depth;
    return (row[bit / 8] >> (8 - depth - (int)(bit % 8))) & ((1u << depth) - 1);
}

static unsigned to8(unsigned v, int depth) {
    if (depth == 8) return v;
    if (depth == 16) return v >> 8;
    return v * 255 / ((1u << depth) - 1);
}

/* Put one row of a (sub)image into the picture at x0 + i * dx, y. */
static void png_row(const png_t *png, const unsigned char *row, int count, int x0, int dx, int y,
                    unsigned int *pixels) {
    int ch = png->channels, d = png->depth;
    for (int i = 0; i < count; i++) {
        int x = x0 + i * dx;
        unsigned r, g, b, a = 255;
        size_t s = (size_t)i * (size_t)ch;
        switch (png->type) {
            case 0: {                                // grey
                unsigned v = sample(row, s, d);
                if (png->has_key && v == png->key[0]) a = 0;
                r = g = b = to8(v, d);
                break;
            }
            case 2: {                                // RGB
                unsigned vr = sample(row, s, d), vg = sample(row, s + 1, d), vb = sample(row, s + 2, d);
                if (png->has_key && vr == png->key[0] && vg == png->key[1] && vb == png->key[2]) a = 0;
                r = to8(vr, d); g = to8(vg, d); b = to8(vb, d);
                break;
            }
            case 3: {                                // palette
                unsigned v = sample(row, s, d);
                if ((int)v >= png->palette_size) v = 0;
                r = png->palette[v][0]; g = png->palette[v][1]; b = png->palette[v][2]; a = png->palette[v][3];
                break;
            }
            case 4:                                  // grey + alpha
                r = g = b = to8(sample(row, s, d), d);
                a = to8(sample(row, s + 1, d), d);
                break;
            default:                                 // RGBA
                r = to8(sample(row, s, d), d); g = to8(sample(row, s + 1, d), d);
                b = to8(sample(row, s + 2, d), d); a = to8(sample(row, s + 3, d), d);
                break;
        }
        pixels[(size_t)y * (size_t)png->width + (size_t)x] = blend(x, y, r, g, b, a);
    }
}

static size_t row_bytes(const png_t *png, int width) {
    return ((size_t)width * (size_t)png->channels * (size_t)png->depth + 7) / 8;
}

static int decode_png(const unsigned char *data, size_t size, image_t *img, const char **error) {
    static const unsigned char signature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    static const int ax[7] = { 0, 4, 0, 2, 0, 1, 0 }, ay[7] = { 0, 0, 4, 0, 2, 0, 1 };
    static const int sx[7] = { 8, 8, 4, 4, 2, 2, 1 }, sy[7] = { 8, 8, 8, 4, 4, 2, 2 };
    png_t png;
    memset(&png, 0, sizeof(png));
    int interlace = 0, have_header = 0;
    size_t idat_size = 0;
    *error = "damaged PNG file";

    // Pass 1: header, palette, transparency, total compressed size.
    for (size_t pos = 8; pos + 12 <= size; ) {
        unsigned len = be32(data + pos);
        const unsigned char *type = data + pos + 4, *body = data + pos + 8;
        if (len > size - pos - 12) return -1;
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            png.width = (int)be32(body);
            png.height = (int)be32(body + 4);
            png.depth = body[8];
            png.type = body[9];
            interlace = body[12];
            have_header = 1;
        } else if (!memcmp(type, "PLTE", 4)) {
            png.palette_size = (int)(len / 3 > 256 ? 256 : len / 3);
            for (int i = 0; i < png.palette_size; i++) {
                png.palette[i][0] = body[i * 3];
                png.palette[i][1] = body[i * 3 + 1];
                png.palette[i][2] = body[i * 3 + 2];
                png.palette[i][3] = 255;
            }
        } else if (!memcmp(type, "tRNS", 4)) {
            if (png.type == 3) {
                for (unsigned i = 0; i < len && i < 256; i++) png.palette[i][3] = body[i];
            } else if (png.type == 0 && len >= 2) {
                png.has_key = 1;
                png.key[0] = (unsigned)body[0] << 8 | body[1];
            } else if (png.type == 2 && len >= 6) {
                png.has_key = 1;
                for (int i = 0; i < 3; i++) png.key[i] = (unsigned)body[i * 2] << 8 | body[i * 2 + 1];
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            idat_size += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12 + (size_t)len;
    }
    (void)signature;
    if (!have_header || !idat_size) return -1;

    switch (png.type) {
        case 0: png.channels = 1; break;
        case 2: png.channels = 3; break;
        case 3: png.channels = 1; break;
        case 4: png.channels = 2; break;
        case 6: png.channels = 4; break;
        default: *error = "unknown PNG colour type"; return -1;
    }
    int d = png.depth;
    int depth_ok = (png.type == 0 && (d == 1 || d == 2 || d == 4 || d == 8 || d == 16)) ||
                   (png.type == 3 && (d == 1 || d == 2 || d == 4 || d == 8)) ||
                   ((png.type == 2 || png.type == 4 || png.type == 6) && (d == 8 || d == 16));
    if (!depth_ok || interlace > 1) { *error = "unsupported PNG bit depth"; return -1; }
    if (png.width <= 0 || png.height <= 0 || png.width > 16384 || png.height > 16384 ||
        (long)png.width * png.height > MAX_PIXELS) {
        *error = "image too large";
        return -1;
    }
    if (png.type == 3 && png.palette_size == 0) return -1;
    if (png.has_key && d < 16) {                    // keys are stored at 16 bits
        for (int i = 0; i < 3; i++) png.key[i] &= (1u << d) - 1;
    }

    // Pass 2: the compressed data, joined.
    unsigned char *z = malloc(idat_size);
    if (!z) { *error = "out of memory"; return -1; }
    size_t zpos = 0;
    for (size_t pos = 8; pos + 12 <= size; ) {
        unsigned len = be32(data + pos);
        if (!memcmp(data + pos + 4, "IDAT", 4)) {
            memcpy(z + zpos, data + pos + 8, len);
            zpos += len;
        }
        if (!memcmp(data + pos + 4, "IEND", 4)) break;
        pos += 12 + (size_t)len;
    }

    // Size of the filtered data: one filter byte per row, per pass.
    int passes = interlace ? 7 : 1;
    size_t raw_size = 0;
    for (int p = 0; p < passes; p++) {
        int pw = interlace ? (png.width - ax[p] + sx[p] - 1) / sx[p] : png.width;
        int ph = interlace ? (png.height - ay[p] + sy[p] - 1) / sy[p] : png.height;
        if (pw > 0 && ph > 0) raw_size += (size_t)ph * (row_bytes(&png, pw) + 1);
    }
    unsigned char *raw = malloc(raw_size);
    unsigned int *pixels = malloc((size_t)png.width * (size_t)png.height * 4);
    if (!raw || !pixels) {
        free(z); free(raw); free(pixels);
        *error = "out of memory";
        return -1;
    }
    long got = zlib_inflate(z, idat_size, raw, raw_size);
    free(z);
    if (got != (long)raw_size) {
        free(raw); free(pixels);
        *error = "damaged PNG data (inflate failed)";
        return -1;
    }

    int bpp = (png.channels * png.depth + 7) / 8;
    size_t offset = 0;
    for (int p = 0; p < passes; p++) {
        int pw = interlace ? (png.width - ax[p] + sx[p] - 1) / sx[p] : png.width;
        int ph = interlace ? (png.height - ay[p] + sy[p] - 1) / sy[p] : png.height;
        if (pw <= 0 || ph <= 0) continue;
        size_t rb = row_bytes(&png, pw);
        unsigned char *pass = raw + offset;
        if (unfilter(pass, ph, rb, bpp) != 0) {
            free(raw); free(pixels);
            *error = "damaged PNG data (bad filter)";
            return -1;
        }
        for (int y = 0; y < ph; y++) {
            const unsigned char *row = pass + (size_t)y * (rb + 1) + 1;
            if (interlace) png_row(&png, row, pw, ax[p], sx[p], ay[p] + y * sy[p], pixels);
            else png_row(&png, row, pw, 0, 1, y, pixels);
        }
        offset += (size_t)ph * (rb + 1);
    }
    free(raw);
    img->width = png.width;
    img->height = png.height;
    img->pixels = pixels;
    img->format = "PNG";
    return 0;
}

/* ===================== BMP ===================== */

static unsigned le16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static unsigned le32(const unsigned char *p) { return le16(p) | le16(p + 2) << 16; }

/* Scale a masked field to 0-255. */
static unsigned field(unsigned v, unsigned mask) {
    if (!mask) return 0;
    int shift = 0, bits = 0;
    while (!((mask >> shift) & 1)) shift++;
    while (((mask >> (shift + bits)) & 1) && shift + bits < 32) bits++;
    unsigned x = (v & mask) >> shift, max = bits >= 32 ? 0xFFFFFFFFu : (1u << bits) - 1;
    return max ? (unsigned)((unsigned long)x * 255 / max) : 0;
}

static int decode_bmp(const unsigned char *data, size_t size, image_t *img, const char **error) {
    *error = "damaged BMP file";
    if (size < 54) return -1;
    unsigned offset = le32(data + 10), header = le32(data + 14);
    if (header < 40 || 14 + header > size) { *error = "old OS/2 BMP files are not supported"; return -1; }
    int width = (int)le32(data + 18), height = (int)le32(data + 22);
    unsigned bpp = le16(data + 28), compression = le32(data + 30), used = le32(data + 46);
    int bottom_up = height > 0;
    if (height < 0) height = -height;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || (long)width * height > MAX_PIXELS) {
        *error = "image too large";
        return -1;
    }
    if (compression != 0 && compression != 3) { *error = "compressed (RLE) BMP files are not supported"; return -1; }

    unsigned rmask = 0xFF0000, gmask = 0xFF00, bmask = 0xFF;
    if (bpp == 16) { rmask = 0x7C00; gmask = 0x3E0; bmask = 0x1F; }
    if (compression == 3) {
        if (14 + 40 + 12 > size) return -1;
        rmask = le32(data + 54); gmask = le32(data + 58); bmask = le32(data + 62);
    }
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) {
        *error = "unsupported BMP bit depth";
        return -1;
    }
    const unsigned char *palette = data + 14 + header;
    unsigned colors = bpp <= 8 ? (used ? used : 1u << bpp) : 0;
    if (colors > 256 || 14 + header + colors * 4 > size) return -1;

    size_t stride = (((size_t)width * bpp + 31) / 32) * 4;
    if (offset > size || stride * (size_t)height > size - offset) return -1;
    unsigned int *pixels = malloc((size_t)width * (size_t)height * 4);
    if (!pixels) { *error = "out of memory"; return -1; }

    for (int y = 0; y < height; y++) {
        const unsigned char *row = data + offset + (size_t)(bottom_up ? height - 1 - y : y) * stride;
        for (int x = 0; x < width; x++) {
            unsigned r, g, b;
            if (bpp <= 8) {
                unsigned i = (row[(size_t)x * bpp / 8] >> (8 - bpp - (x * bpp) % 8)) & ((1u << bpp) - 1);
                if (i >= colors) i = 0;
                b = palette[i * 4]; g = palette[i * 4 + 1]; r = palette[i * 4 + 2];
            } else if (bpp == 24) {
                b = row[x * 3]; g = row[x * 3 + 1]; r = row[x * 3 + 2];
            } else {
                unsigned v = bpp == 16 ? le16(row + x * 2) : le32(row + x * 4);
                r = field(v, rmask); g = field(v, gmask); b = field(v, bmask);
            }
            pixels[(size_t)y * (size_t)width + (size_t)x] = r << 16 | g << 8 | b;
        }
    }
    img->width = width;
    img->height = height;
    img->pixels = pixels;
    img->format = "BMP";
    return 0;
}

/* ===================== PPM / PGM (binary) ===================== */

static int ppm_number(const unsigned char *data, size_t size, size_t *pos) {
    while (*pos < size) {
        unsigned char c = data[*pos];
        if (c == '#') { while (*pos < size && data[*pos] != '\n') (*pos)++; }
        else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') (*pos)++;
        else break;
    }
    int v = 0, digits = 0;
    while (*pos < size && data[*pos] >= '0' && data[*pos] <= '9' && v < 100000) {
        v = v * 10 + (data[(*pos)++] - '0');
        digits++;
    }
    return digits ? v : -1;
}

static int decode_ppm(const unsigned char *data, size_t size, image_t *img, const char **error) {
    *error = "damaged PPM file";
    int grey = data[1] == '5';
    size_t pos = 2;
    int width = ppm_number(data, size, &pos), height = ppm_number(data, size, &pos);
    int maxval = ppm_number(data, size, &pos);
    if (width <= 0 || height <= 0 || maxval <= 0 || maxval > 65535 || pos >= size) return -1;
    if ((long)width * height > MAX_PIXELS) { *error = "image too large"; return -1; }
    pos++;                                          // the single whitespace after maxval
    int bytes = maxval > 255 ? 2 : 1, ch = grey ? 1 : 3;
    if ((size_t)width * (size_t)height * (size_t)(ch * bytes) > size - pos) return -1;
    unsigned int *pixels = malloc((size_t)width * (size_t)height * 4);
    if (!pixels) { *error = "out of memory"; return -1; }
    const unsigned char *p = data + pos;
    for (long i = 0; i < (long)width * height; i++) {
        unsigned v[3];
        for (int c = 0; c < ch; c++) {
            unsigned s = bytes == 2 ? (unsigned)p[0] << 8 | p[1] : p[0];
            p += bytes;
            v[c] = s * 255 / (unsigned)maxval;
        }
        if (grey) v[1] = v[2] = v[0];
        pixels[i] = v[0] << 16 | v[1] << 8 | v[2];
    }
    img->width = width;
    img->height = height;
    img->pixels = pixels;
    img->format = grey ? "PGM" : "PPM";
    return 0;
}

/* ===================== Entry points ===================== */

int image_decode(const unsigned char *data, size_t size, image_t *img, const char **error) {
    static const unsigned char png_sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    const char *dummy;
    if (!error) error = &dummy;
    memset(img, 0, sizeof(*img));
    if (size >= 8 && !memcmp(data, png_sig, 8)) return decode_png(data, size, img, error);
    if (size >= 2 && data[0] == 'B' && data[1] == 'M') return decode_bmp(data, size, img, error);
    if (size >= 3 && data[0] == 'P' && (data[1] == '6' || data[1] == '5')) return decode_ppm(data, size, img, error);
    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8) { *error = "JPEG is not supported yet (use PNG or BMP)"; return -1; }
    if (size >= 4 && !memcmp(data, "GIF8", 4)) { *error = "GIF is not supported yet (use PNG or BMP)"; return -1; }
    *error = "not an image this OS can read (PNG, BMP, PPM)";
    return -1;
}

int image_load(const char *path, image_t *img, const char **error) {
    const char *dummy;
    if (!error) error = &dummy;
    int fd = open(path, OPEN_READ);
    if (fd < 0) { *error = os_strerror(fd); return -1; }
    size_t cap = 64 * 1024, len = 0;
    unsigned char *buf = malloc(cap);
    while (buf) {
        if (len == cap) {
            unsigned char *bigger = realloc(buf, cap * 2);
            if (!bigger) { free(buf); buf = 0; break; }
            buf = bigger;
            cap *= 2;
        }
        long n = read(fd, buf + len, cap - len);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(fd);
    if (!buf) { *error = "out of memory"; return -1; }
    int r = image_decode(buf, len, img, error);
    free(buf);
    return r;
}

void image_free(image_t *img) {
    free(img->pixels);
    img->pixels = 0;
}
