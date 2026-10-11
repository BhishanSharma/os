// inflate.h - gzip decompression for the loader
#ifndef INFLATE_H
#define INFLATE_H

#include <stdint.h>
#include <stddef.h>

/* The uncompressed size a gzip file says it holds (its last 4 bytes); 0 if not gzip. */
uint32_t gzip_size(const uint8_t *gz, size_t len);

/* Decompress into `out` (gzip_size bytes); 0 on success, -1 if damaged. */
int gunzip(const uint8_t *gz, size_t len, uint8_t *out, size_t out_len);

#endif
