#ifndef BEARSSL_CLIENT_H
#define BEARSSL_CLIENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* BearSSL adapter. The TCP transport is supplied by net.c. */
int tls_https_download(const char *host, const char *path,
                      uint8_t *body, uint32_t body_max, uint32_t *body_len);

#ifdef __cplusplus
}
#endif

#endif
