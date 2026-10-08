#include "tls/bearssl_client.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <bearssl.h>

#ifdef __cplusplus
extern "C" {
#endif
int net_tls_read(unsigned char *buf, size_t len);
int net_tls_write(const unsigned char *buf, size_t len);
#ifdef __cplusplus
}
#endif

static int low_read(void *, unsigned char *buf, size_t len) {
    return net_tls_read(buf, len);
}

static int low_write(void *, const unsigned char *buf, size_t len) {
    return net_tls_write(buf, len);
}

/* Build-time date is used because the OS currently has no RTC. BearSSL counts
 * days from 0000-01-01 in the proleptic Gregorian calendar. */
static uint32_t days_from_year_month_day(unsigned y, unsigned m, unsigned d) {
    uint32_t days = 365u * y + y / 4u - y / 100u + y / 400u;
    static const unsigned mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    for (unsigned i = 1; i < m; ++i) days += mdays[i - 1];
    if (m > 2 && ((y % 4u == 0 && y % 100u != 0) || (y % 400u == 0))) days++;
    return days + d - 1u;
}

static void build_date(unsigned *y, unsigned *m, unsigned *d) {
    const char *s = __DATE__;
    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    unsigned mi = 0;
    for (unsigned i = 0; i < 12; ++i) {
        if (s[0] == months[i * 3] && s[1] == months[i * 3 + 1] && s[2] == months[i * 3 + 2]) { mi = i + 1; break; }
    }
    *m = mi;
    *d = (unsigned)((s[4] == ' ' ? 0 : (s[4] - '0')) * 10 + (s[5] - '0'));
    *y = (unsigned)((s[7] - '0') * 1000 + (s[8] - '0') * 100 + (s[9] - '0') * 10 + (s[10] - '0'));
}

/* ISRG Root X2. This covers modern Let's Encrypt ECDSA chains. */
static unsigned char x2_dn[] = {
    0x30,0x4f,0x31,0x0b,0x30,0x09,0x06,0x03,0x55,0x04,0x06,0x13,0x02,0x55,0x53,
    0x31,0x29,0x30,0x27,0x06,0x03,0x55,0x04,0x0a,0x13,0x20,
    'I','n','t','e','r','n','e','t',' ','S','e','c','u','r','i','t','y',' ','R','e','s','e','a','r','c','h',' ','G','r','o','u','p',
    0x31,0x15,0x30,0x13,0x06,0x03,0x55,0x04,0x03,0x13,0x0c,
    'I','S','R','G',' ','R','o','o','t',' ','X','2'
};

static unsigned char x2_q[] = {
    0x04,
    0xcd,0x9b,0xd5,0x9f,0x80,0x83,0x0a,0xec,0x09,0x4a,0xf3,0x16,0x4a,0x3e,0x5c,0xcf,
    0x77,0xac,0xde,0x67,0x05,0x0d,0x1d,0x07,0xb6,0xdc,0x16,0xfb,0x5a,0x8b,0x14,0xdb,
    0xe2,0x71,0x60,0xc4,0xba,0x45,0x95,0x11,0x89,0x8e,0xea,0x06,0xdf,0xf7,0x2a,0x16,
    0x1c,0xa4,0xb9,0xc5,0xc5,0x32,0xe0,0x03,0xe0,0x1e,0x82,0x18,0x38,0x8b,0xd7,0x45,
    0xd8,0x0a,0x6a,0x6e,0xe6,0x00,0x77,0xfb,0x02,0x51,0x7d,0x22,0xd8,0x0a,0x6e,0x9a,
    0x5b,0x77,0xdf,0xf0,0xfa,0x41,0xec,0x39,0xdc,0x75,0xca,0x68,0x07,0x0c,0x1f,0xea
};

static br_x509_trust_anchor x2_ta;

static void init_trust_anchor(void) {
    x2_ta.dn.data = x2_dn;
    x2_ta.dn.len = sizeof(x2_dn);
    x2_ta.flags = BR_X509_TA_CA;
    x2_ta.pkey.key_type = BR_KEYTYPE_EC;
    x2_ta.pkey.key.ec.curve = BR_EC_secp384r1;
    x2_ta.pkey.key.ec.q = x2_q;
    x2_ta.pkey.key.ec.qlen = sizeof(x2_q);
}

static int fill_entropy(unsigned char out[64]) {
    unsigned eax = 1, ecx;
    __asm__ volatile("cpuid"
                     : "+a"(eax), "=c"(ecx)
                     :
                     : "ebx", "edx");
    if ((ecx & (1u << 30)) == 0) return 0;

    for (unsigned i = 0; i < 16; ++i) {
        unsigned long long v;
        unsigned char ok = 0;
        for (unsigned attempt = 0; attempt < 10 && !ok; ++attempt)
            __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (!ok) return 0;
        for (unsigned j = 0; j < 8; ++j) out[i * 8 + j] = (unsigned char)(v >> (j * 8));
    }
    return 1;
}

static int find_header_end(const unsigned char *p, uint32_t n) {
    if (n < 4) return -1;
    for (uint32_t i = 3; i < n; ++i)
        if (p[i-3]=='\r' && p[i-2]=='\n' && p[i-1]=='\r' && p[i]=='\n') return (int)(i + 1);
    return -1;
}

static int parse_status(const unsigned char *p, uint32_t n) {
    if (n < 12 || p[0]!='H'||p[1]!='T'||p[2]!='T'||p[3]!='P'||p[4]!='/') return 0;
    if (p[9]<'0'||p[9]>'9'||p[10]<'0'||p[10]>'9'||p[11]<'0'||p[11]>'9') return 0;
    return (p[9]-'0')*100+(p[10]-'0')*10+(p[11]-'0');
}

static int append_http_bytes(const unsigned char *src, uint32_t n,
                             unsigned char *body, uint32_t max, uint32_t *body_len,
                             unsigned char *hdr, uint32_t *hdr_len, int *headers_done, int *status) {
    if (!*headers_done) {
        uint32_t copy = n;
        if (*hdr_len + copy > 8191) copy = 8191 - *hdr_len;
        if (copy) { memcpy(hdr + *hdr_len, src, copy); *hdr_len += copy; }
        int off = find_header_end(hdr, *hdr_len);
        if (off >= 0) {
            *headers_done = 1;
            *status = parse_status(hdr, *hdr_len);
            uint32_t bn = *hdr_len - (uint32_t)off;
            if (*body_len + bn > max) return -2;
            memcpy(body + *body_len, hdr + off, bn); *body_len += bn;
            if (copy < n) {
                uint32_t extra = n - copy;
                if (*body_len + extra > max) return -2;
                memcpy(body + *body_len, src + copy, extra); *body_len += extra;
            }
        }
    } else {
        if (*body_len + n > max) return -2;
        memcpy(body + *body_len, src, n); *body_len += n;
    }
    return 0;
}

extern "C" int tls_https_download(const char *host, const char *path,
                                    uint8_t *body, uint32_t body_max, uint32_t *body_len) {
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context io;
    unsigned char iobuf[4096];
    unsigned char entropy[64];
    unsigned y, m, d;
    build_date(&y, &m, &d);

    init_trust_anchor();
    br_ssl_client_init_full(&sc, &xc, &x2_ta, 1);
    br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof(iobuf), 0);
    if (!fill_entropy(entropy)) return -7;
    br_ssl_engine_inject_entropy(&sc.eng, entropy, sizeof(entropy));
    br_x509_minimal_set_time(&xc, days_from_year_month_day(y, m, d), 12 * 3600);
    br_ssl_engine_add_flags(&sc.eng, BR_OPT_NO_RENEGOTIATION);
    if (!br_ssl_client_reset(&sc, host, 0)) return -1;

    br_sslio_init(&io, &sc.eng, low_read, 0, low_write, 0);

    char req[1024];
    uint32_t rn = 0;
    const char *parts[] = {"GET ", path, " HTTP/1.0\r\nHost: ", host,
                           "\r\nConnection: close\r\nUser-Agent: TerminalOS/1.0\r\n\r\n"};
    for (unsigned i = 0; i < sizeof(parts)/sizeof(parts[0]); ++i) {
        const char *p = parts[i];
        while (*p && rn < sizeof(req)-1) req[rn++] = *p++;
    }
    req[rn] = 0;
    if (br_sslio_write_all(&io, req, rn) < 0 || br_sslio_flush(&io) < 0) return -2;

    unsigned char hdr[8192];
    uint32_t hdr_len = 0, total = 0;
    int headers_done = 0, status = 0;
    unsigned char buf[2048];
    for (;;) {
        int n = br_sslio_read(&io, buf, sizeof(buf));
        if (n < 0) {
            if (headers_done && status == 200) break;
            return -3;
        }
        if (n == 0) {
            if (headers_done && status == 200) break;
            return -3;
        }
        if (append_http_bytes(buf, (uint32_t)n, body, body_max, &total,
                              hdr, &hdr_len, &headers_done, &status) < 0) return -4;
    }
    *body_len = total;
    if (!headers_done || status != 200) return status ? -5 : -6;
    return 0;
}
