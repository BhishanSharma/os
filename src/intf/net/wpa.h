// wpa.h - WPA2-Personal: the passphrase, the 4-way handshake, group keys
//
// The Wi-Fi driver joins the network (authentication and association are
// plain management frames); then the access point starts the 4-way handshake
// with EAPOL-Key frames. Both sides prove they know the passphrase and agree
// on fresh keys: a pairwise key for traffic between us and the access point,
// and a group key for its broadcasts. The card encrypts and decrypts with
// those keys itself (AES-CCMP); this file only does the handshake.
//
// Only WPA2-PSK with AES (CCMP) is supported: key descriptor version 2
// (HMAC-SHA1 MICs, AES key wrap), the AKM most home routers use.
#ifndef WPA_H
#define WPA_H

#include <stdint.h>

/* wpa_input() events: what the driver must do now. */
#define WPA_EV_PTK   1   /* install `tk` as the pairwise key */
#define WPA_EV_GTK   2   /* install `gtk` (key index `gtk_idx`) as the group key */
#define WPA_EV_DONE  4   /* the 4-way handshake finished: data may flow */

typedef struct {
    uint8_t pmk[32];                  /* from the passphrase and the network name */
    uint8_t aa[6], spa[6];            /* the access point's address, ours */
    uint8_t ie[64];                   /* our RSN element, exactly as in the association request */
    int ie_len;
    uint8_t anonce[32], snonce[32];
    uint8_t kck[16], kek[16], tk[16]; /* MIC key, key-encryption key, temporal (traffic) key */
    uint8_t gtk[32];
    int gtk_len, gtk_idx;
    uint8_t replay[8];                /* last replay counter accepted */
    int have_replay;
    int have_ptk;                     /* message 1 seen and keys derived */
    int done;                         /* message 3 accepted */
    const char *error;                /* why the last frame was rejected */
} wpa_t;

/* The pairwise master key: PBKDF2-HMAC-SHA1(passphrase, SSID, 4096, 32). */
void wpa_passphrase(const char *pass, const uint8_t *ssid, int ssid_len, uint8_t pmk[32]);

/* Our RSN element: WPA2, CCMP pairwise, PSK. `group_cipher` is the access
 * point's (4 = CCMP). Returns the length written to `out` (22 bytes). */
int wpa_rsn_ie(uint8_t *out, uint8_t group_cipher);

/* Start a handshake with access point `aa`. `random` gives our nonce. */
void wpa_begin(wpa_t *w, const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6],
               const uint8_t *ie, int ie_len, const uint8_t random[32]);

/* An EAPOL frame from the access point (from the 802.1X header on). If an
 * answer is due it is written to `reply` (at most 256 bytes) and its length
 * returned; 0 = no answer; -1 = frame rejected (see w->error). `events`
 * gets WPA_EV_* bits. */
int wpa_input(wpa_t *w, const uint8_t *eapol, int len, uint8_t *reply, int *events);

#endif
