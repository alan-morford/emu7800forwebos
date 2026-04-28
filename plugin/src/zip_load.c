/*
 * zip_load.c
 * Extracts the first ROM file from a ZIP archive.
 *
 * Reads the central directory (always has correct sizes, regardless of the
 * bit-3 data-descriptor flag) then seeks to the local file data to extract.
 * Handles stored (method 0) and deflate (method 8) using bundled puff.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zip_load.h"
#include "puff.h"

/* ---- ZIP signatures / field offsets ---- */
#define SIG_EOCD   0x06054b50u   /* end of central directory */
#define SIG_CD     0x02014b50u   /* central directory entry */
#define SIG_LOCAL  0x04034b50u   /* local file header */

#define EOCD_MIN_SIZE  22
#define CD_FIXED_SIZE  46
#define LOCAL_FIXED    30

#define METHOD_STORED   0
#define METHOD_DEFLATE  8
#define MAX_ROM_SIZE    (512 * 1024)

static uint16_t u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((unsigned)p[1] << 8));
}
static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)(p[0] | ((unsigned)p[1] << 8)
                    | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24));
}

static int is_rom_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    return (strcasecmp(dot, ".a26") == 0 ||
            strcasecmp(dot, ".a78") == 0 ||
            strcasecmp(dot, ".bin") == 0);
}

/*
 * Locate the End of Central Directory record.
 * Returns the file offset of the EOCD, or -1 on failure.
 * Scans backward from the end to handle files with a trailing comment.
 */
static long find_eocd(FILE *f, long file_size)
{
    uint8_t buf[4];
    long pos;
    /* Maximum comment length is 65535; scan that far back */
    long scan_start = file_size - EOCD_MIN_SIZE;
    long scan_end   = file_size - EOCD_MIN_SIZE - 65535;
    if (scan_end < 0) scan_end = 0;

    for (pos = scan_start; pos >= scan_end; pos--) {
        if (fseek(f, pos, SEEK_SET) != 0) break;
        if (fread(buf, 1, 4, f) != 4) break;
        if (u32(buf) == SIG_EOCD) return pos;
    }
    return -1;
}

uint8_t *zip_load_rom(const char *zip_path, long *out_size,
                      char *out_name, int name_cap)
{
    FILE *f;
    long file_size, eocd_pos, cd_offset;
    uint32_t cd_entries, cd_size;
    uint8_t eocd[22];
    uint8_t *cd_data = NULL;
    uint8_t *result  = NULL;
    uint32_t i;

    f = fopen(zip_path, "rb");
    if (!f) return NULL;

    /* Get file size */
    if (fseek(f, 0, SEEK_END) != 0) goto done;
    file_size = ftell(f);

    /* Find EOCD */
    eocd_pos = find_eocd(f, file_size);
    if (eocd_pos < 0) goto done;

    if (fseek(f, eocd_pos, SEEK_SET) != 0) goto done;
    if (fread(eocd, 1, 22, f) != 22) goto done;
    if (u32(eocd) != SIG_EOCD) goto done;

    cd_entries = (uint32_t)u16(eocd + 10);
    cd_size    = u32(eocd + 12);
    cd_offset  = (long)u32(eocd + 16);

    /* Read entire central directory */
    if (cd_size == 0 || cd_size > (uint32_t)(file_size)) goto done;
    cd_data = (uint8_t *)malloc(cd_size);
    if (!cd_data) goto done;
    if (fseek(f, cd_offset, SEEK_SET) != 0) goto done;
    if (fread(cd_data, 1, cd_size, f) != cd_size) goto done;

    /* Walk central directory entries */
    {
        uint32_t cd_pos = 0;
        for (i = 0; i < cd_entries && !result; i++) {
            uint16_t method, fname_len, extra_len, comment_len;
            uint32_t comp_size, uncomp_size, local_offset;
            char fname[256];
            const char *base;
            uint8_t *entry;

            if (cd_pos + CD_FIXED_SIZE > cd_size) break;
            entry = cd_data + cd_pos;

            if (u32(entry) != SIG_CD) break;

            method       = u16(entry + 10);
            comp_size    = u32(entry + 20);
            uncomp_size  = u32(entry + 24);
            fname_len    = u16(entry + 28);
            extra_len    = u16(entry + 30);
            comment_len  = u16(entry + 32);
            local_offset = u32(entry + 42);

            cd_pos += CD_FIXED_SIZE;

            /* Copy filename */
            {
                uint32_t rd = fname_len < (uint32_t)(sizeof(fname) - 1)
                            ? fname_len : (uint32_t)(sizeof(fname) - 1);
                if (cd_pos + fname_len > cd_size) break;
                memcpy(fname, cd_data + cd_pos, rd);
                fname[rd] = '\0';
            }
            cd_pos += fname_len + extra_len + comment_len;

            /* Basename (strip in-zip path) */
            {
                const char *sl = strrchr(fname, '/');
                base = sl ? sl + 1 : fname;
            }

            if (!is_rom_ext(base)) continue;
            if (uncomp_size == 0 || uncomp_size > (uint32_t)MAX_ROM_SIZE) continue;
            if (comp_size == 0 || comp_size > (uint32_t)MAX_ROM_SIZE) continue;

            /* Seek to local file header and skip it */
            {
                uint8_t lhdr[LOCAL_FIXED];
                uint16_t lname_len, lextra_len;
                long data_pos;

                if (fseek(f, (long)local_offset, SEEK_SET) != 0) break;
                if (fread(lhdr, 1, LOCAL_FIXED, f) != LOCAL_FIXED) break;
                if (u32(lhdr) != SIG_LOCAL) break;
                lname_len  = u16(lhdr + 26);
                lextra_len = u16(lhdr + 28);
                data_pos = (long)local_offset + LOCAL_FIXED + lname_len + lextra_len;
                if (fseek(f, data_pos, SEEK_SET) != 0) break;
            }

            /* Extract data using sizes from central directory */
            if (method == METHOD_STORED) {
                result = (uint8_t *)malloc(comp_size);
                if (!result) break;
                if (fread(result, 1, comp_size, f) != comp_size) {
                    free(result); result = NULL; break;
                }
                *out_size = (long)comp_size;

            } else if (method == METHOD_DEFLATE) {
                uint8_t *comp = (uint8_t *)malloc(comp_size);
                if (!comp) break;
                if (fread(comp, 1, comp_size, f) != comp_size) {
                    free(comp); break;
                }
                result = (uint8_t *)malloc(uncomp_size);
                if (!result) { free(comp); break; }
                {
                    unsigned long dlen = (unsigned long)uncomp_size;
                    unsigned long slen = (unsigned long)comp_size;
                    int err = puff(result, &dlen, comp, &slen);
                    free(comp);
                    if (err != 0) { free(result); result = NULL; break; }
                    *out_size = (long)dlen;
                }
            } else {
                /* Unsupported compression method */
                continue;
            }

            /* Copy inner filename for machine-type detection */
            if (out_name && name_cap > 0) {
                strncpy(out_name, base, name_cap - 1);
                out_name[name_cap - 1] = '\0';
            }
        }
    }

done:
    free(cd_data);
    fclose(f);
    return result;
}
