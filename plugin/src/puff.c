/*
 * puff.c
 * Minimal raw-deflate decompressor (RFC 1951).
 * Public domain — adapted from Mark Adler's puff.c (zlib contrib).
 *
 * Handles BTYPE 00 (stored), 01 (fixed Huffman), 02 (dynamic Huffman).
 */

#include <string.h>
#include "puff.h"

/* ---- bit-stream state ---- */
typedef struct {
    const uint8_t *in;
    unsigned long  inlen;
    unsigned long  inpos;
    unsigned       bits;    /* buffered bits */
    int            left;    /* bits in buffer */
} State;

static int pull(State *s, int n, unsigned *val)
{
    while (s->left < n) {
        if (s->inpos >= s->inlen) return -1;
        s->bits |= (unsigned)s->in[s->inpos++] << s->left;
        s->left += 8;
    }
    *val = s->bits & ((1u << n) - 1);
    s->bits >>= n;
    s->left -= n;
    return 0;
}

/* ---- Huffman tables ---- */
#define MAXBITS  15
#define MAXLCODES 286
#define MAXDCODES 30

typedef struct {
    short count[MAXBITS + 1];
    short symbol[MAXLCODES + MAXDCODES];
} Huffman;

static int build(Huffman *h, const short *lens, int n)
{
    int i, left;
    short offs[MAXBITS + 1];

    memset(h->count, 0, sizeof(h->count));
    for (i = 0; i < n; i++)
        if (lens[i]) h->count[lens[i]]++;

    /* Check that code lengths are valid */
    left = 1;
    for (i = 1; i <= MAXBITS; i++) {
        left <<= 1;
        left -= h->count[i];
        if (left < 0) return -1;
    }

    offs[1] = 0;
    for (i = 1; i < MAXBITS; i++)
        offs[i + 1] = offs[i] + h->count[i];

    for (i = 0; i < n; i++)
        if (lens[i]) h->symbol[offs[lens[i]]++] = (short)i;

    return left != 0;   /* 1 = incomplete (ok for distance), 0 = complete */
}

static int decode(State *s, const Huffman *h)
{
    int code = 0, first = 0, index = 0;
    int len;
    unsigned bit;
    for (len = 1; len <= MAXBITS; len++) {
        if (pull(s, 1, &bit)) return -10;
        code |= (int)bit;
        if (code - (int)h->count[len] < first) {
            /* Found: code is in [first, first+count) */
            return h->symbol[index + (code - first)];
        }
        index += h->count[len];
        first += h->count[len];
        first <<= 1;
        code  <<= 1;
    }
    return -10;
}

/* ---- length/distance tables ---- */
static const short lens[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const short lext[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const short dists[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const short dext[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

/* Decompress one block of Huffman-coded data */
static int huff_block(State *s, uint8_t *out, unsigned long outlen,
                      unsigned long *outpos, const Huffman *lencode,
                      const Huffman *distcode)
{
    int sym;
    unsigned val;
    for (;;) {
        sym = decode(s, lencode);
        if (sym < 0) return sym;
        if (sym < 256) {
            if (*outpos >= outlen) return -1;
            out[(*outpos)++] = (uint8_t)sym;
        } else if (sym == 256) {
            break;  /* end of block */
        } else {
            int len, dist;
            unsigned long from;
            sym -= 257;
            if (sym >= 29) return -10;
            if (pull(s, lext[sym], &val)) return -1;
            len = lens[sym] + (int)val;

            sym = decode(s, distcode);
            if (sym < 0) return sym;
            if (pull(s, dext[sym], &val)) return -1;
            dist = dists[sym] + (int)val;

            from = *outpos - (unsigned long)dist;
            if (dist > (int)*outpos) return -10;

            while (len--) {
                if (*outpos >= outlen) return -1;
                out[(*outpos)++] = out[from++];
            }
        }
    }
    return 0;
}

/* Fixed Huffman codes per RFC 1951 section 3.2.6 */
static int fixed_block(State *s, uint8_t *out, unsigned long outlen,
                       unsigned long *outpos)
{
    static Huffman lencode, distcode;
    static int built = 0;
    if (!built) {
        short lens_buf[288], dist_buf[30];
        int i;
        for (i = 0;   i <= 143; i++) lens_buf[i] = 8;
        for (i = 144; i <= 255; i++) lens_buf[i] = 9;
        for (i = 256; i <= 279; i++) lens_buf[i] = 7;
        for (i = 280; i <= 287; i++) lens_buf[i] = 8;
        for (i = 0;   i <  30;  i++) dist_buf[i]  = 5;
        build(&lencode,  lens_buf, 288);
        build(&distcode, dist_buf, 30);
        built = 1;
    }
    return huff_block(s, out, outlen, outpos, &lencode, &distcode);
}

/* Dynamic Huffman codes per RFC 1951 section 3.2.7 */
static int dynamic_block(State *s, uint8_t *out, unsigned long outlen,
                         unsigned long *outpos)
{
    static const short order[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
    };
    Huffman lencode, distcode, clencode;
    short lens_buf[MAXLCODES + MAXDCODES];
    short clen_lens[19];
    unsigned nlen, ndist, ncode, val;
    int i, sym;

    if (pull(s, 5, &nlen))  return -1;
    nlen  += 257;
    if (pull(s, 5, &ndist)) return -1;
    ndist += 1;
    if (pull(s, 4, &ncode)) return -1;
    ncode += 4;

    if (nlen > MAXLCODES || ndist > MAXDCODES) return -10;

    memset(clen_lens, 0, sizeof(clen_lens));
    for (i = 0; i < (int)ncode; i++) {
        if (pull(s, 3, &val)) return -1;
        clen_lens[order[i]] = (short)val;
    }
    if (build(&clencode, clen_lens, 19) < 0) return -10;

    /* Read literal/length + distance code lengths */
    memset(lens_buf, 0, sizeof(lens_buf));
    for (i = 0; i < (int)(nlen + ndist); ) {
        sym = decode(s, &clencode);
        if (sym < 0) return sym;
        if (sym < 16) {
            lens_buf[i++] = (short)sym;
        } else {
            int rep, len = 0;
            if (sym == 16) {
                if (i == 0) return -10;
                if (pull(s, 2, &val)) return -1;
                rep = 3 + (int)val;
                len = lens_buf[i - 1];
            } else if (sym == 17) {
                if (pull(s, 3, &val)) return -1;
                rep = 3 + (int)val;
            } else { /* sym == 18 */
                if (pull(s, 7, &val)) return -1;
                rep = 11 + (int)val;
            }
            if (i + rep > (int)(nlen + ndist)) return -10;
            while (rep--) lens_buf[i++] = (short)len;
        }
    }

    if (build(&lencode,  lens_buf,        (int)nlen)  < 0) return -10;
    if (build(&distcode, lens_buf + nlen, (int)ndist) < 0) return -10;

    return huff_block(s, out, outlen, outpos, &lencode, &distcode);
}

/* ---- Public API ---- */
int puff(uint8_t *dest, unsigned long *destlen,
         const uint8_t *source, unsigned long *sourcelen)
{
    State s;
    unsigned long outpos = 0;
    unsigned last, type, val;
    int err;

    s.in    = source;
    s.inlen = *sourcelen;
    s.inpos = 0;
    s.bits  = 0;
    s.left  = 0;

    do {
        if (pull(&s, 1, &last)) { err = -1; goto done; }
        if (pull(&s, 2, &type)) { err = -1; goto done; }

        if (type == 0) {
            /* Stored block */
            unsigned long len, nlen;
            s.bits = 0; s.left = 0;  /* discard partial byte */
            if (pull(&s, 16, &val)) { err = -1; goto done; }
            len = val;
            if (pull(&s, 16, &val)) { err = -1; goto done; }
            nlen = val;
            if ((len & 0xFFFF) != ((~nlen) & 0xFFFF)) { err = -10; goto done; }
            if (outpos + len > *destlen) { err = -1; goto done; }
            if (s.inpos + len > s.inlen)  { err = -1; goto done; }
            memcpy(dest + outpos, source + s.inpos, len);
            outpos   += len;
            s.inpos  += len;
            err = 0;
        } else if (type == 1) {
            err = fixed_block(&s, dest, *destlen, &outpos);
        } else if (type == 2) {
            err = dynamic_block(&s, dest, *destlen, &outpos);
        } else {
            err = -10;
        }
        if (err) goto done;
    } while (!last);

done:
    *destlen  = outpos;
    *sourcelen = s.inpos;
    return err;
}
