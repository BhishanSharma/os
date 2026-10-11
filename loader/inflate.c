// inflate.c - gzip decompression (RFC 1951 DEFLATE inside RFC 1952 gzip)
//
// A small, careful decoder in the style of zlib's reference decoder "puff":
// canonical Huffman codes decoded a bit at a time. Slower than zlib, but the
// RAM disk compresses well (long runs of zeros), so it is quick enough.
#include "inflate.h"

typedef struct {
    const uint8_t *in, *in_end;
    uint8_t *out, *out_start, *out_end;
    uint32_t bitbuf;
    int bitcnt;
    int error;
} state_t;

typedef struct {
    int16_t count[16];          /* how many codes of each length */
    int16_t symbol[320];        /* symbols ordered by code */
} huffman_t;

static uint32_t bits(state_t *s, int need) {
    uint32_t v = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->in >= s->in_end) {
            s->error = 1;
            return 0;
        }
        v |= (uint32_t)*s->in++ << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = (uint32_t)((uint64_t)v >> need);
    s->bitcnt -= need;
    return v & ((1u << need) - 1);
}

static int stored(state_t *s) {
    s->bitbuf = 0;                                          /* to a byte boundary */
    s->bitcnt = 0;
    if (s->in + 4 > s->in_end) return -1;
    uint32_t len = s->in[0] | s->in[1] << 8, nlen = s->in[2] | s->in[3] << 8;
    s->in += 4;
    if (len != (~nlen & 0xFFFF)) return -1;
    if (s->in + len > s->in_end || s->out + len > s->out_end) return -1;
    for (uint32_t i = 0; i < len; i++) *s->out++ = *s->in++;
    return 0;
}

static int decode(state_t *s, const huffman_t *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= (int)bits(s, 1);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* Build the decoding table; returns 0 if complete, > 0 if incomplete, < 0 if over-subscribed. */
static int construct(huffman_t *h, const int16_t *length, int n) {
    for (int len = 0; len < 16; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    int16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (int16_t)(offs[len] + h->count[len]);
    for (int sym = 0; sym < n; sym++)
        if (length[sym]) h->symbol[offs[length[sym]]++] = (int16_t)sym;
    return left;
}

static const int16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const int16_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                   3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const int16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                    8193, 12289, 16385, 24577 };
static const int16_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                   7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(state_t *s, const huffman_t *lencode, const huffman_t *distcode) {
    for (;;) {
        int sym = decode(s, lencode);
        if (sym < 0 || s->error) return -1;
        if (sym < 256) {
            if (s->out >= s->out_end) return -1;
            *s->out++ = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            uint32_t len = (uint32_t)lbase[sym] + bits(s, lext[sym]);
            int dsym = decode(s, distcode);
            if (dsym < 0 || dsym >= 30 || s->error) return -1;
            uint32_t dist = (uint32_t)dbase[dsym] + bits(s, dext[dsym]);
            if (s->error || dist > (uint32_t)(s->out - s->out_start) || s->out + len > s->out_end) return -1;
            const uint8_t *from = s->out - dist;
            while (len--) *s->out++ = *from++;
        }
    }
}

static int fixed(state_t *s) {
    static huffman_t lencode, distcode;
    static int built;
    if (!built) {
        int16_t lengths[288];
        int sym = 0;
        for (; sym < 144; sym++) lengths[sym] = 8;
        for (; sym < 256; sym++) lengths[sym] = 9;
        for (; sym < 280; sym++) lengths[sym] = 7;
        for (; sym < 288; sym++) lengths[sym] = 8;
        construct(&lencode, lengths, 288);
        for (sym = 0; sym < 30; sym++) lengths[sym] = 5;
        construct(&distcode, lengths, 30);
        built = 1;
    }
    return codes(s, &lencode, &distcode);
}

static int dynamic(state_t *s) {
    static const int16_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int16_t lengths[320];
    huffman_t lencode, distcode;
    int nlen = (int)bits(s, 5) + 257, ndist = (int)bits(s, 5) + 1, ncode = (int)bits(s, 4) + 4;
    if (s->error || nlen > 286 || ndist > 30) return -1;
    int index = 0;
    for (; index < ncode; index++) lengths[order[index]] = (int16_t)bits(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;
    if (s->error || construct(&lencode, lengths, 19) != 0) return -1;
    index = 0;
    while (index < nlen + ndist) {
        int sym = decode(s, &lencode);
        if (sym < 0 || s->error) return -1;
        if (sym < 16) {
            lengths[index++] = (int16_t)sym;
        } else {
            int16_t len = 0;
            if (sym == 16) {
                if (index == 0) return -1;
                len = lengths[index - 1];
                sym = 3 + (int)bits(s, 2);
            } else if (sym == 17) {
                sym = 3 + (int)bits(s, 3);
            } else {
                sym = 11 + (int)bits(s, 7);
            }
            if (s->error || index + sym > nlen + ndist) return -1;
            while (sym--) lengths[index++] = len;
        }
    }
    if (lengths[256] == 0) return -1;
    int err = construct(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return -1;
    err = construct(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return -1;
    return codes(s, &lencode, &distcode);
}

static uint32_t crc32(const uint8_t *p, size_t n) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t gzip_size(const uint8_t *gz, size_t len) {
    if (len < 18 || gz[0] != 0x1F || gz[1] != 0x8B) return 0;
    const uint8_t *t = gz + len - 4;
    return t[0] | t[1] << 8 | t[2] << 16 | (uint32_t)t[3] << 24;
}

int gunzip(const uint8_t *gz, size_t len, uint8_t *out, size_t out_len) {
    if (len < 18 || gz[0] != 0x1F || gz[1] != 0x8B || gz[2] != 8) return -1;
    uint8_t flags = gz[3];
    const uint8_t *p = gz + 10, *end = gz + len - 8;
    if (flags & 4) {                                        /* FEXTRA */
        if (p + 2 > end) return -1;
        p += 2 + (p[0] | p[1] << 8);
    }
    if (flags & 8) while (p < end && *p++) {}               /* FNAME */
    if (flags & 16) while (p < end && *p++) {}              /* FCOMMENT */
    if (flags & 2) p += 2;                                  /* FHCRC */
    if (p >= end) return -1;

    state_t s = { p, end, out, out, out + out_len, 0, 0, 0 };
    int last;
    do {
        last = (int)bits(&s, 1);
        int type = (int)bits(&s, 2);
        if (s.error) return -1;
        int r = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : -1;
        if (r != 0) return -1;
    } while (!last);

    size_t produced = (size_t)(s.out - out);
    const uint8_t *t = gz + len - 8;
    uint32_t want_crc = t[0] | t[1] << 8 | t[2] << 16 | (uint32_t)t[3] << 24;
    if (produced != gzip_size(gz, len) || crc32(out, produced) != want_crc) return -1;
    return 0;
}
