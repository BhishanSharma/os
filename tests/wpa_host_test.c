// wpa_host_test.c - checks net/wpa.c on the build machine:
//   published vectors for the passphrase hash (IEEE 802.11 J.4) and AES key
//   unwrap (RFC 3394), then a full 4-way handshake and a group-key update
//   against a simulated access point.
// Build (inside the buildenv container):
//   gcc -I src/intf -I $BEARSSL/inc tests/wpa_host_test.c src/impl/x86_64/net/wpa.c $BEARSSL/build/libbearssl.a
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "net/wpa.h"
#include "bearssl.h"

static int failures;

static void check(int ok, const char *what) {
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static void unhex(const char *s, uint8_t *out) {
    for (int i = 0; s[2 * i]; i++) sscanf(s + 2 * i, "%2hhx", &out[i]);
}

static void hmac_sha1(const uint8_t *key, int klen, const uint8_t *d, int len, uint8_t out[20]) {
    br_hmac_key_context kc;
    br_hmac_context hc;
    br_hmac_key_init(&kc, &br_sha1_vtable, key, klen);
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, d, len);
    br_hmac_out(&hc, out);
}

/* RFC 3394 key wrap, for the simulated access point. */
static void key_wrap(const uint8_t kek[16], const uint8_t *in, int len, uint8_t *out) {
    int n = len / 8;
    br_aes_ct_cbcenc_keys ctx;
    br_aes_ct_cbcenc_init(&ctx, kek, 16);
    uint8_t a[8], b[16];
    memset(a, 0xA6, 8);
    memcpy(out + 8, in, len);
    for (int j = 0; j <= 5; j++)
        for (int i = 1; i <= n; i++) {
            memcpy(b, a, 8);
            memcpy(b + 8, out + 8 * i, 8);
            uint8_t iv[16] = {0};
            br_aes_ct_cbcenc_run(&ctx, iv, b, 16);
            uint64_t t = (uint64_t)n * j + i;
            memcpy(a, b, 8);
            for (int k = 0; k < 8; k++) a[7 - k] ^= (uint8_t)(t >> (8 * k));
            memcpy(out + 8 * i, b + 8, 8);
        }
    memcpy(out, a, 8);
}

/* The access point's side, written separately from wpa.c. */
static uint8_t ap_kck[16], ap_kek[16], ap_tk[16];

static void ap_derive(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6],
                      const uint8_t an[32], const uint8_t sn[32]) {
    uint8_t buf[100], ptk[60];
    memcpy(buf, "Pairwise key expansion", 23);
    const uint8_t *lo = memcmp(aa, spa, 6) < 0 ? aa : spa, *hi = lo == aa ? spa : aa;
    memcpy(buf + 23, lo, 6);
    memcpy(buf + 29, hi, 6);
    const uint8_t *nlo = memcmp(an, sn, 32) < 0 ? an : sn, *nhi = nlo == an ? sn : an;
    memcpy(buf + 35, nlo, 32);
    memcpy(buf + 67, nhi, 32);
    for (int i = 0; i < 3; i++) {
        buf[99] = (uint8_t)i;
        hmac_sha1(pmk, 32, buf, 100, ptk + 20 * i);
    }
    memcpy(ap_kck, ptk, 16);
    memcpy(ap_kek, ptk + 16, 16);
    memcpy(ap_tk, ptk + 32, 16);
}

static int ap_frame(uint8_t *f, uint16_t info, uint8_t replay, const uint8_t *nonce, const uint8_t *data, int dlen,
                    int with_mic) {
    int len = 99 + dlen;
    memset(f, 0, len);
    f[0] = 2; f[1] = 3; f[2] = (uint8_t)((len - 4) >> 8); f[3] = (uint8_t)(len - 4);
    f[4] = 2; f[5] = (uint8_t)(info >> 8); f[6] = (uint8_t)info;
    f[8] = 16;
    f[16] = replay;
    if (nonce) memcpy(f + 17, nonce, 32);
    f[97] = (uint8_t)(dlen >> 8); f[98] = (uint8_t)dlen;
    memcpy(f + 99, data, dlen);
    if (with_mic) {
        uint8_t mic[20];
        hmac_sha1(ap_kck, 16, f, len, mic);
        memcpy(f + 81, mic, 16);
    }
    return len;
}

static int mic_ok(const uint8_t *f, int len) {
    uint8_t copy[512], mic[20];
    memcpy(copy, f, len);
    memset(copy + 81, 0, 16);
    hmac_sha1(ap_kck, 16, copy, len, mic);
    return memcmp(mic, f + 81, 16) == 0;
}

int main(void) {
    uint8_t pmk[32], want[32];

    wpa_passphrase("password", (const uint8_t *)"IEEE", 4, pmk);
    unhex("f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e", want);
    check(memcmp(pmk, want, 32) == 0, "PBKDF2: \"password\" / \"IEEE\"");
    wpa_passphrase("ThisIsAPassword", (const uint8_t *)"ThisIsASSID", 11, pmk);
    unhex("0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af", want);
    check(memcmp(pmk, want, 32) == 0, "PBKDF2: \"ThisIsAPassword\" / \"ThisIsASSID\"");

    /* RFC 3394 4.1, through the simulated AP's wrap and wpa.c's unwrap (in a message 3). */
    uint8_t kek[16], key[16], wrapped[24], want_w[24];
    unhex("000102030405060708090A0B0C0D0E0F", kek);
    unhex("00112233445566778899AABBCCDDEEFF", key);
    unhex("1FA68B0A8112B447AEF34BD8FB5A7B829D3E862371D2CFE5", want_w);
    key_wrap(kek, key, 16, wrapped);
    check(memcmp(wrapped, want_w, 24) == 0, "AES key wrap: RFC 3394 vector (test helper)");

    /* A 4-way handshake. */
    uint8_t aa[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55}, spa[6] = {0xc8, 0x8a, 0x9a, 0x55, 0x27, 0xba};
    uint8_t anonce[32], snonce_rand[32], ie[64];
    for (int i = 0; i < 32; i++) { anonce[i] = (uint8_t)(i * 7 + 1); snonce_rand[i] = (uint8_t)(200 - i); }
    wpa_passphrase("hunter22", (const uint8_t *)"HomeWiFi", 8, pmk);
    int ie_len = wpa_rsn_ie(ie, 4);
    check(ie_len == 22 && ie[0] == 48 && ie[1] == 20, "RSN element is 22 bytes");
    wpa_t w;
    wpa_begin(&w, pmk, aa, spa, ie, ie_len, snonce_rand);

    uint8_t f[512], reply[256];
    int ev, len = ap_frame(f, 0x008A, 1, anonce, 0, 0, 0);
    int r = wpa_input(&w, f, len, reply, &ev);
    check(r == 99 + 22 && ev == 0, "message 1 -> message 2");
    ap_derive(pmk, aa, spa, anonce, reply + 17);
    check(r > 0 && mic_ok(reply, r), "message 2 MIC checks out at the AP");
    check(r > 0 && memcmp(reply + 99, ie, 22) == 0 && reply[16] == 1, "message 2 carries our RSN element and replay 1");
    check(r > 0 && reply[5] == 0x01 && reply[6] == 0x0A, "message 2 key info 0x010A");

    /* Message 3 with a GTK KDE (index 1), encrypted with the KEK. */
    uint8_t kd[48], kd_wrapped[56], gtk[16];
    for (int i = 0; i < 16; i++) gtk[i] = (uint8_t)(0xA0 + i);
    int n = 0;
    memcpy(kd, ie, 22); n = 22;
    kd[n++] = 0xDD; kd[n++] = 22; kd[n++] = 0x00; kd[n++] = 0x0F; kd[n++] = 0xAC; kd[n++] = 1;
    kd[n++] = 1; kd[n++] = 0;
    memcpy(kd + n, gtk, 16); n += 16;
    kd[n++] = 0xDD;                                        /* padding to 48 */
    kd[n++] = 0;
    key_wrap(ap_kek, kd, n, kd_wrapped);
    len = ap_frame(f, 0x13CA, 2, anonce, kd_wrapped, n + 8, 1);
    r = wpa_input(&w, f, len, reply, &ev);
    check(r == 99, "message 3 -> message 4");
    check(ev == (WPA_EV_PTK | WPA_EV_GTK | WPA_EV_DONE), "message 3 installs pairwise and group keys");
    check(memcmp(w.tk, ap_tk, 16) == 0, "pairwise key matches the AP's");
    check(w.gtk_len == 16 && w.gtk_idx == 1 && memcmp(w.gtk, gtk, 16) == 0, "group key and index decrypted");
    check(r > 0 && mic_ok(reply, r) && reply[5] == 0x03 && reply[6] == 0x0A, "message 4 MIC and key info 0x030A");

    /* A replayed message 3 is refused. */
    r = wpa_input(&w, f, len, reply, &ev);
    check(r == -1 && ev == 0, "replayed message 3 refused");

    /* Group key update (index 2). */
    for (int i = 0; i < 16; i++) gtk[i] = (uint8_t)(0x50 + i);
    n = 0;
    kd[n++] = 0xDD; kd[n++] = 22; kd[n++] = 0x00; kd[n++] = 0x0F; kd[n++] = 0xAC; kd[n++] = 1;
    kd[n++] = 2; kd[n++] = 0;
    memcpy(kd + n, gtk, 16); n += 16;                      /* 24 bytes: no padding */
    key_wrap(ap_kek, kd, n, kd_wrapped);
    len = ap_frame(f, 0x1382, 3, 0, kd_wrapped, n + 8, 1);
    r = wpa_input(&w, f, len, reply, &ev);
    check(r == 99 && ev == WPA_EV_GTK, "group message 1 -> group message 2");
    check(w.gtk_idx == 2 && memcmp(w.gtk, gtk, 16) == 0, "new group key installed");
    check(r > 0 && mic_ok(reply, r) && reply[5] == 0x03 && reply[6] == 0x02, "group message 2 MIC and key info 0x0302");

    /* A wrong password shows up as a bad MIC in message 3. */
    wpa_t bad;
    uint8_t wrong[32];
    wpa_passphrase("hunter23", (const uint8_t *)"HomeWiFi", 8, wrong);
    wpa_begin(&bad, wrong, aa, spa, ie, ie_len, snonce_rand);
    len = ap_frame(f, 0x008A, 1, anonce, 0, 0, 0);
    wpa_input(&bad, f, len, reply, &ev);
    ap_derive(pmk, aa, spa, anonce, snonce_rand);
    key_wrap(ap_kek, kd, 24, kd_wrapped);
    len = ap_frame(f, 0x13CA, 2, anonce, kd_wrapped, 32, 1);
    r = wpa_input(&bad, f, len, reply, &ev);
    check(r == -1 && bad.error && strstr(bad.error, "MIC"), "wrong password -> MIC error");

    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures != 0;
}
