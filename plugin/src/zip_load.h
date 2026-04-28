/*
 * zip_load.h
 * Extracts the first ROM file from a ZIP archive.
 */

#ifndef ZIP_LOAD_H
#define ZIP_LOAD_H

#include <stdint.h>

/*
 * Open a ZIP file and decompress the first entry with a recognized ROM
 * extension (.a26, .a78, .bin).
 *
 *   zip_path  - path to the .zip file
 *   out_size  - receives the decompressed data size
 *   out_name  - receives the inner filename (up to name_cap-1 chars), or NULL
 *   name_cap  - capacity of out_name buffer
 *
 * Returns malloc'd buffer on success (caller must free), NULL on failure.
 * Supports stored (method 0) and deflate (method 8).
 */
uint8_t *zip_load_rom(const char *zip_path, long *out_size,
                      char *out_name, int name_cap);

#endif /* ZIP_LOAD_H */
