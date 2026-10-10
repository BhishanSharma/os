// wpa.c - WPA2-Personal handshake (IEEE 802.11-2016 12.7), using BearSSL's
// SHA-1, HMAC and AES.
#include "net/wpa.h"
#include "lib/string.h"
#include "bearssl.h"

/* EAPOL-Key frame (802.1X header, then the key descriptor). */
#define EAPOL_HDR      4
#define KEY_INFO       (EAPOL_HDR + 1)
#define KEY_LEN        (EAPOL_HDR + 3)
#define KEY_REPLAY     (EAPOL_HDR + 5)
#define KEY_NONCE      (EAPOL_HDR + 13)
#define KEY_MIC        (EAPOL_HDR + 77)
#define KEY_DATA_LEN   (EAPOL_HDR + 93)
#define KEY_DATA       (EAPOL_HDR + 95)

#define KI_VERSION     0x0007
#define KI_PAIRWISE    0x0008
#define KI_INSTALL     0x0040
#define KI_ACK         0x0080
#define KI_MIC         0x0100
#define KI_SECURE      0x0200
#define KI_ENCRYPTED   0x1000

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static void hmac_sha1(const uint8_t *key, int klen, const uint8_t *a, int alen, const uint8_t *b, int blen,
                      uint8_t out[20]) {
    br_hmac_key_context kc;
    br_hmac_context hc;
    br_hmac_key_init(&kc, &br_sha1_vtable, key, (size_t)klen);
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, a, (size_t)alen);
    if (blen) br_hmac_update(&hc, b, (size_t)blen);
    br_hmac_out(&hc, out);
}

void wpa_passphrase(const char *pass, const uint8_t *ssid, int ssid_len, uint8_t pmk[32]) {
    br_hmac_key_context kc;
    br_hmac_context hc;
    br_hmac_key_init(&kc, &br_sha1_vtable, pass, strlen(pass));
    for (int block = 1; block <= 2; block++) {
        uint8_t salt[36], u[20], t[20];
        memcpy(salt, ssid, (size_t)ssid_len);
        salt[ssid_len] = 0; salt[ssid_len + 1] = 0; salt[ssid_len + 2] = 0;
        salt[ssid_len + 3] = (uint8_t)block;
        br_hmac_init(&hc, &kc, 0);
        br_hmac_update(&hc, salt, (size_t)ssid_len + 4);
        br_hmac_out(&hc, u);
        memcpy(t, u, 20);
        for (int i = 1; i < 4096; i++) {
            br_hmac_init(&hc, &kc, 0);
            br_hmac_update(&hc, u, 20);
            br_hmac_out(&hc, u);
            for (int k = 0; k < 20; k++) t[k] ^= u[k];
        }
        memcpy(pmk + (block - 1) * 20, t, block == 1 ? 20 : 12);
    }
}

/* PRF-384 (802.11 12.7.1.2): the pairwise transient key from the PMK. */
static void derive_ptk(wpa_t *w) {
    static const char label[] = "Pairwise key expansion";
    uint8_t data[6 + 6 + 32 + 32 + 1], buf[23 + sizeof(data)];
    int a_first = memcmp(w->aa, w->spa, 6) < 0;
    memcpy(data, a_first ? w->aa : w->spa, 6);
    memcpy(data + 6, a_first ? w->spa : w->aa, 6);
    int n_first = memcmp(w->anonce, w->snonce, 32) < 0;
    memcpy(data + 12, n_first ? w->anonce : w->snonce, 32);
    memcpy(data + 44, n_first ? w->snonce : w->anonce, 32);
    memcpy(buf, label, 23);                                 /* the label and its 0 byte */
    memcpy(buf + 23, data, 76);
    uint8_t ptk[60];
    for (int i = 0; i < 3; i++) {
        buf[23 + 76] = (uint8_t)i;
        hmac_sha1(w->pmk, 32, buf, 23 + 77, 0, 0, ptk + 20 * i);
    }
    memcpy(w->kck, ptk, 16);
    memcpy(w->kek, ptk + 16, 16);
    memcpy(w->tk, ptk + 32, 16);
}

/* HMAC-SHA1-128 over the whole EAPOL frame with the MIC field zeroed. */
static void compute_mic(const wpa_t *w, uint8_t *frame, int len, uint8_t mic[16]) {
    uint8_t saved[16], full[20];
    memcpy(saved, frame + KEY_MIC, 16);
    memset(frame + KEY_MIC, 0, 16);
    hmac_sha1(w->kck, 16, frame, len, 0, 0, full);
    memcpy(frame + KEY_MIC, saved, 16);
    memcpy(mic, full, 16);
}

/* AES key unwrap (RFC 3394) with the KEK. Returns 0 if the check value fits. */
static int key_unwrap(const uint8_t kek[16], const uint8_t *in, int len, uint8_t *out) {
    int n = len / 8 - 1;
    if (len % 8 || n < 2) return -1;
    br_aes_ct_cbcdec_keys ctx;
    br_aes_ct_cbcdec_init(&ctx, kek, 16);
    uint8_t a[8], b[16];
    memcpy(a, in, 8);
    memcpy(out, in + 8, (size_t)n * 8);
    for (int j = 5; j >= 0; j--) {
        for (int i = n; i >= 1; i--) {
            uint64_t t = (uint64_t)n * (uint64_t)j + (uint64_t)i;
            memcpy(b, a, 8);
            for (int k = 0; k < 8; k++) b[7 - k] ^= (uint8_t)(t >> (8 * k));
            memcpy(b + 8, out + (i - 1) * 8, 8);
            uint8_t iv[16] = {0};                           /* CBC with a zero IV: one block = AES^-1 */
            br_aes_ct_cbcdec_run(&ctx, iv, b, 16);
            memcpy(a, b, 8);
            memcpy(out + (i - 1) * 8, b + 8, 8);
        }
    }
    for (int k = 0; k < 8; k++)
        if (a[k] != 0xA6) return -1;
    return 0;
}

int wpa_rsn_ie(uint8_t *out, uint8_t group_cipher) {
    static const uint8_t ie[] = {
        48, 20, 1, 0,                         /* RSN element, version 1 */
        0x00, 0x0F, 0xAC, 0,                  /* group cipher (filled in) */
        1, 0, 0x00, 0x0F, 0xAC, 4,            /* 1 pairwise cipher: CCMP */
        1, 0, 0x00, 0x0F, 0xAC, 2,            /* 1 AKM: PSK */
        0, 0,                                 /* RSN capabilities */
    };
    memcpy(out, ie, sizeof(ie));
    out[7] = group_cipher;
    return (int)sizeof(ie);
}

void wpa_begin(wpa_t *w, const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6],
               const uint8_t *ie, int ie_len, const uint8_t random[32]) {
    memset(w, 0, sizeof(*w));
    memcpy(w->pmk, pmk, 32);
    memcpy(w->aa, aa, 6);
    memcpy(w->spa, spa, 6);
    if (ie_len > (int)sizeof(w->ie)) ie_len = sizeof(w->ie);
    memcpy(w->ie, ie, (size_t)ie_len);
    w->ie_len = ie_len;
    memcpy(w->snonce, random, 32);
}

/* An EAPOL-Key reply: `info`, the replay counter just accepted, optional nonce and data, MIC. */
static int build_reply(const wpa_t *w, uint8_t version, uint16_t info, const uint8_t *nonce,
                       const uint8_t *data, int data_len, uint8_t *out) {
    int len = KEY_DATA + data_len;
    memset(out, 0, (size_t)len);
    out[0] = version;
    out[1] = 3;                                             /* EAPOL-Key */
    put_be16(out + 2, (uint16_t)(len - EAPOL_HDR));
    out[EAPOL_HDR] = 2;                                     /* RSN key descriptor */
    put_be16(out + KEY_INFO, info);
    memcpy(out + KEY_REPLAY, w->replay, 8);
    if (nonce) memcpy(out + KEY_NONCE, nonce, 32);
    put_be16(out + KEY_DATA_LEN, (uint16_t)data_len);
    if (data_len) memcpy(out + KEY_DATA, data, (size_t)data_len);
    compute_mic(w, out, len, out + KEY_MIC);
    return len;
}

/* The GTK key data element (KDE) in decrypted key data. */
static int find_gtk(wpa_t *w, const uint8_t *data, int len) {
    for (int i = 0; i + 2 <= len;) {
        uint8_t type = data[i], l = data[i + 1];
        if (type == 0xDD && l == 0) break;                  /* padding */
        if (i + 2 + l > len) break;
        const uint8_t *e = data + i + 2;
        if (type == 0xDD && l >= 6 && e[0] == 0x00 && e[1] == 0x0F && e[2] == 0xAC && e[3] == 1) {
            int glen = l - 6;
            if (glen < 16 || glen > 32) return -1;
            w->gtk_idx = e[4] & 3;
            memcpy(w->gtk, e + 6, (size_t)glen);
            w->gtk_len = glen;
            return 0;
        }
        i += 2 + l;
    }
    return -1;
}

int wpa_input(wpa_t *w, const uint8_t *eapol, int len, uint8_t *reply, int *events) {
    *events = 0;
    w->error = 0;
    if (len < KEY_DATA || eapol[1] != 3) return 0;          /* not an EAPOL-Key frame */
    int body = be16(eapol + 2);
    if (EAPOL_HDR + body > len || EAPOL_HDR + body < KEY_DATA) { w->error = "short frame"; return -1; }
    len = EAPOL_HDR + body;
    if (eapol[EAPOL_HDR] != 2) { w->error = "not an RSN key frame (old WPA?)"; return -1; }
    uint16_t info = be16(eapol + KEY_INFO);
    if ((info & KI_VERSION) != 2) { w->error = "key descriptor version is not 2 (TKIP or SHA-256 network?)"; return -1; }
    if (!(info & KI_ACK)) return 0;                         /* not from the authenticator */
    int data_len = be16(eapol + KEY_DATA_LEN);
    if (KEY_DATA + data_len > len) { w->error = "key data too long"; return -1; }
    if (w->have_replay && memcmp(eapol + KEY_REPLAY, w->replay, 8) <= 0) { w->error = "replayed frame"; return -1; }
    uint8_t version = eapol[0];

    if ((info & KI_PAIRWISE) && !(info & KI_MIC)) {
        /* Message 1: the access point's nonce. Derive the keys, answer with ours. */
        memcpy(w->anonce, eapol + KEY_NONCE, 32);
        derive_ptk(w);
        w->have_ptk = 1;
        memcpy(w->replay, eapol + KEY_REPLAY, 8);
        w->have_replay = 1;
        return build_reply(w, version, 2 | KI_PAIRWISE | KI_MIC, w->snonce, w->ie, w->ie_len, reply);
    }

    if (!w->have_ptk) { w->error = "key frame before message 1"; return -1; }
    uint8_t frame[512], mic[16];
    if (len > (int)sizeof(frame)) { w->error = "frame too long"; return -1; }
    memcpy(frame, eapol, (size_t)len);
    compute_mic(w, frame, len, mic);
    if (!(info & KI_MIC) || memcmp(mic, eapol + KEY_MIC, 16) != 0) {
        w->error = "wrong MIC: the password is probably wrong";
        return -1;
    }
    uint8_t plain[256];
    int plain_len = 0;
    if (info & KI_ENCRYPTED) {
        if (data_len > (int)sizeof(plain) + 8 || key_unwrap(w->kek, eapol + KEY_DATA, data_len, plain) != 0) {
            w->error = "could not decrypt the key data";
            return -1;
        }
        plain_len = data_len - 8;
    }
    memcpy(w->replay, eapol + KEY_REPLAY, 8);

    if (info & KI_PAIRWISE) {
        /* Message 3: install the pairwise key (and the group key it carries). */
        if (memcmp(w->anonce, eapol + KEY_NONCE, 32) != 0) { w->error = "nonce changed in message 3"; return -1; }
        if (!(info & KI_INSTALL)) { w->error = "message 3 without the install bit"; return -1; }
        *events = WPA_EV_PTK | WPA_EV_DONE;
        if (plain_len && find_gtk(w, plain, plain_len) == 0) *events |= WPA_EV_GTK;
        w->done = 1;
        return build_reply(w, version, 2 | KI_PAIRWISE | KI_MIC | KI_SECURE, 0, 0, 0, reply);
    }

    /* Group key handshake message 1: a new group key. */
    if (!plain_len || find_gtk(w, plain, plain_len) != 0) { w->error = "group key message without a key"; return -1; }
    *events = WPA_EV_GTK;
    return build_reply(w, version, 2 | KI_MIC | KI_SECURE, 0, 0, 0, reply);
}
