#ifndef PANTHERA_OOXML_ZIP32_H
#define PANTHERA_OOXML_ZIP32_H

/*
 * Minimal ZIP32 package writer for Panthera Works.
 * Produces uncompressed (STORE) members.  The caller is responsible for
 * writing to a temporary path and committing the finished archive atomically.
 */

typedef unsigned char pw_zip_u8;
typedef unsigned short pw_zip_u16;
typedef unsigned long pw_zip_u32;

typedef struct {
    const char *name;
    const pw_zip_u8 *data;
    pw_zip_u32 size;
} PwZip32Entry;

enum {
    PW_ZIP32_OK = 0,
    PW_ZIP32_BAD_ARGUMENT = 1,
    PW_ZIP32_NO_MEMORY = 2,
    PW_ZIP32_IO_ERROR = 3,
    PW_ZIP32_UNSUPPORTED = 4,
    PW_ZIP32_NOT_FOUND = 5,
    PW_ZIP32_CORRUPT = 6,
    PW_ZIP32_UNSUPPORTED_METHOD = 7,
    PW_ZIP32_TOO_LARGE = 8,
    PW_ZIP32_BAD_CRC = 9
};

/*
 * Write a standards-compliant single-disk ZIP32 archive using method STORE.
 * Entry names must be unique, relative ASCII paths with forward slashes.
 * ZIP64, encryption, data descriptors, and compression are not emitted.
 */
int pw_zip32_write_store(const char *path,
                         const PwZip32Entry *entries,
                         unsigned long entry_count);

pw_zip_u32 pw_zip32_crc32(const pw_zip_u8 *data, pw_zip_u32 size);

#endif
