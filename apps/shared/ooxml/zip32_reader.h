#ifndef PANTHERA_OOXML_ZIP32_READER_H
#define PANTHERA_OOXML_ZIP32_READER_H

#include "zip32.h"

typedef struct {
    pw_zip_u8 *data;
    pw_zip_u32 size;
} PwZip32Buffer;

/*
 * Read one ZIP32 member by exact package path. Supports STORE and raw DEFLATE.
 * ZIP64, split archives, encrypted members, and files above max_output_bytes
 * are rejected. The caller must dispose the output buffer on success.
 */
int pw_zip32_read_member(const char *path,
                         const char *member_name,
                         pw_zip_u32 max_output_bytes,
                         PwZip32Buffer *output);

void pw_zip32_buffer_dispose(PwZip32Buffer *buffer);

#endif
