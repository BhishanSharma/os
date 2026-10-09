#include "tls/bearssl_client.h"
#include "drivers/rtc.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <bearssl.h>

#ifdef __cplusplus
extern "C" {
#endif
int net_tls_read(unsigned char *buf, size_t len);
int net_tls_write(const unsigned char *buf, size_t len);
void net_download_progress(uint32_t received, uint32_t total);
#ifdef __cplusplus
}
#endif

static int low_read(void *, unsigned char *buf, size_t len) {
    return net_tls_read(buf, len);
}

static int low_write(void *, const unsigned char *buf, size_t len) {
    return net_tls_write(buf, len);
}

/* BearSSL counts days from 0000-01-01 in the proleptic Gregorian calendar
 * (1970-01-01 is day 719528). The leap-year terms count leap years before y,
 * including year 0. */
static uint32_t days_from_year_month_day(unsigned y, unsigned m, unsigned d) {
    uint32_t days = 365u * y + (y + 3u) / 4u - (y + 99u) / 100u + (y + 399u) / 400u;
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

/* Trust store generated from the system CA bundle by scripts/gen-trust-anchors.sh. */
extern "C" const br_x509_trust_anchor *const tls_trust_anchors;
extern "C" const size_t tls_trust_anchors_num;

/* Certificate validity is checked against the CMOS clock. If it is unreadable or
 * set earlier than the build date (a flat CMOS battery, say), use the build date,
 * which is at least a lower bound on the real date. */
static void tls_validation_time(uint32_t *days, uint32_t *seconds) {
    unsigned y, m, d;
    build_date(&y, &m, &d);
    *days = days_from_year_month_day(y, m, d);
    *seconds = 12 * 3600;

    rtc_time_t now;
    if (rtc_read(&now) != 0) return;
    uint32_t rtc_days = days_from_year_month_day(now.year, now.month, now.day);
    if (rtc_days < *days) return;
    *days = rtc_days;
    *seconds = (uint32_t)now.hour * 3600u + (uint32_t)now.minute * 60u + now.second;
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

static uint32_t parse_content_length(const unsigned char *p, uint32_t n) {
    uint32_t pos = 0;
    while (pos < n) {
        uint32_t end = pos;
        while (end + 1 < n && !(p[end] == '\r' && p[end + 1] == '\n')) end++;
        if (end + 1 >= n) break;
        static const char name[] = "content-length";
        uint32_t i = 0;
        while (i < sizeof(name) - 1 && pos + i < end) {
            unsigned char c = p[pos + i];
            if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
            if (c != (unsigned char)name[i]) break;
            i++;
        }
        if (i == sizeof(name) - 1 && pos + i < end && p[pos + i] == ':') {
            i++;
            while (pos + i < end && (p[pos + i] == ' ' || p[pos + i] == '\t')) i++;
            uint32_t value = 0;
            int digits = 0;
            while (pos + i < end && p[pos + i] >= '0' && p[pos + i] <= '9') {
                uint32_t digit = p[pos + i] - '0';
                if (value > (0xFFFFFFFFu - digit) / 10u) return 0;
                value = value * 10u + digit;
                i++;
                digits = 1;
            }
            while (pos + i < end && (p[pos + i] == ' ' || p[pos + i] == '\t')) i++;
            return digits && pos + i == end ? value : 0;
        }
        pos = end + 2;
    }
    return 0;
}

static int append_http_bytes(const unsigned char *src, uint32_t n,
                             unsigned char *body, uint32_t max, uint32_t *body_len,
                             unsigned char *hdr, uint32_t *hdr_len, int *headers_done, int *status,
                             uint32_t *content_length) {
    if (!*headers_done) {
        uint32_t copy = n;
        if (*hdr_len + copy > 8191) copy = 8191 - *hdr_len;
        if (copy) { memcpy(hdr + *hdr_len, src, copy); *hdr_len += copy; }
        int off = find_header_end(hdr, *hdr_len);
        if (off >= 0) {
            *headers_done = 1;
            *status = parse_status(hdr, *hdr_len);
            *content_length = parse_content_length(hdr, *hdr_len);
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

static int tls_engine_error;
static int tls_http_status;

extern "C" int tls_last_error(void) {
    return tls_engine_error;
}

extern "C" int tls_last_http_status(void) {
    return tls_http_status;
}

/* The TLS state and buffers total roughly 40 KiB, more than fits on the 32 KiB
 * boot stack, so they live in .bss. Downloads never run concurrently. */
static br_ssl_client_context tls_sc;
static br_x509_minimal_context tls_xc;
static br_sslio_context tls_io;
static unsigned char tls_iobuf[BR_SSL_BUFSIZE_MONO];
static unsigned char tls_hdr[8192];
static unsigned char tls_buf[2048];

static int https_download(const char *host, const char *path,
                          uint8_t *body, uint32_t body_max, uint32_t *body_len);

extern "C" int tls_https_download(const char *host, const char *path,
                                    uint8_t *body, uint32_t body_max, uint32_t *body_len) {
    tls_engine_error = 0;
    tls_http_status = 0;
    int rc = https_download(host, path, body, body_max, body_len);
    if (rc != 0) tls_engine_error = br_ssl_engine_last_error(&tls_sc.eng);
    return rc;
}

static int https_download(const char *host, const char *path,
                          uint8_t *body, uint32_t body_max, uint32_t *body_len) {
    br_ssl_client_context &sc = tls_sc;
    br_x509_minimal_context &xc = tls_xc;
    br_sslio_context &io = tls_io;
    unsigned char entropy[64];
    uint32_t days, seconds;
    tls_validation_time(&days, &seconds);

    br_ssl_client_init_full(&sc, &xc, tls_trust_anchors, tls_trust_anchors_num);
    br_ssl_engine_set_buffer(&sc.eng, tls_iobuf, sizeof(tls_iobuf), 0);
    if (!fill_entropy(entropy)) return -7;
    br_ssl_engine_inject_entropy(&sc.eng, entropy, sizeof(entropy));
    br_x509_minimal_set_time(&xc, days, seconds);
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

    unsigned char *hdr = tls_hdr;
    uint32_t hdr_len = 0, total = 0, content_length = 0;
    int headers_done = 0, status = 0;
    for (;;) {
        int n = br_sslio_read(&io, tls_buf, sizeof(tls_buf));
        if (n <= 0) break;
        if (append_http_bytes(tls_buf, (uint32_t)n, body, body_max, &total,
                              hdr, &hdr_len, &headers_done, &status, &content_length) < 0) return -4;
        if (headers_done && total) net_download_progress(total, content_length);
        if (headers_done && content_length && total >= content_length) break;
    }
    *body_len = total;
    tls_http_status = status;
    if (!headers_done) return -3;
    if (status != 200) return status ? -5 : -6;
    if (content_length && total < content_length) return -8;
    return 0;
}
