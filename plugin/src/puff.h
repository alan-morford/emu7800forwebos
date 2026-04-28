/*
 * puff.h
 * Minimal raw-deflate decompressor (RFC 1951).
 * Public domain — adapted from Mark Adler's puff.c (zlib contrib).
 */

#ifndef PUFF_H
#define PUFF_H

#include <stdint.h>

/*
 * Decompress raw deflate data (no zlib/gzip header).
 *   dest     - output buffer
 *   destlen  - in: capacity, out: bytes written
 *   source   - compressed input
 *   sourcelen- in: input size, out: bytes consumed
 * Returns 0 on success, non-zero on error.
 */
int puff(uint8_t *dest, unsigned long *destlen,
         const uint8_t *source, unsigned long *sourcelen);

#endif /* PUFF_H */
