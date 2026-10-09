#ifndef BEARSSL_CLIENT_H
#define BEARSSL_CLIENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* BearSSL adapter. The TCP transport is supplied by net.c. */
int tls_https_download(const char *host, const char *path,
                      uint8_t *body, uint32_t body_max, uint32_t *body_len);
/* BearSSL engine error (BR_ERR_*) from the last failed download, or 0. */
int tls_last_error(void);
/* HTTP status code of the last download, or 0 if no response header arrived. */
int tls_last_http_status(void);

#ifdef __cplusplus
}
#endif

#endif
