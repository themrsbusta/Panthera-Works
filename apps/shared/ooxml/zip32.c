#include "zip32.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PW_ZIP32_MAX_ENTRIES 4096UL
#define PW_ZIP32_NAME_MAX 255
#define PW_ZIP32_VERSION 20
#define PW_ZIP32_UTF8_FLAG 0x0800
#define PW_ZIP32_DOS_DATE 0x0021 /* 1980-01-01 */

typedef struct {
    char name[PW_ZIP32_NAME_MAX + 1];
    pw_zip_u16 name_bytes;
    pw_zip_u32 size;
    pw_zip_u32 crc;
    pw_zip_u32 local_offset;
} PwZip32Record;

static pw_zip_u32 g_crc_table[256];
static int g_crc_ready;

static void crc_table_init(void)
{
    unsigned long index;
    for (index = 0; index < 256; ++index) {
        pw_zip_u32 value = (pw_zip_u32)index;
        unsigned bit;
        for (bit = 0; bit < 8; ++bit) {
            if (value & 1UL) value = (value >> 1) ^ 0xedb88320UL;
            else value >>= 1;
        }
        g_crc_table[index] = value;
    }
    g_crc_ready = 1;
}

pw_zip_u32 pw_zip32_crc32(const pw_zip_u8 *data, pw_zip_u32 size)
{
    pw_zip_u32 crc = 0xffffffffUL;
    pw_zip_u32 index;
    if (!g_crc_ready) crc_table_init();
    if (data == NULL && size != 0) return 0;
    for (index = 0; index < size; ++index)
        crc = g_crc_table[(crc ^ data[index]) & 0xffUL] ^ (crc >> 8);
    return crc ^ 0xffffffffUL;
}

static int write_u16(FILE *file, pw_zip_u16 value)
{
    if (fputc((int)(value & 0xff), file) == EOF) return 0;
    if (fputc((int)((value >> 8) & 0xff), file) == EOF) return 0;
    return 1;
}

static int write_u32(FILE *file, pw_zip_u32 value)
{
    if (!write_u16(file, (pw_zip_u16)(value & 0xffffUL))) return 0;
    if (!write_u16(file, (pw_zip_u16)((value >> 16) & 0xffffUL))) return 0;
    return 1;
}

static int valid_name(const char *name, unsigned *name_bytes)
{
    unsigned length = 0;
    unsigned segment_start = 0;
    unsigned index;
    if (name == NULL || name[0] == 0) return 0;
    if (name[0] == '/' || name[0] == '\\') return 0;
    while (name[length] != 0) {
        unsigned char value = (unsigned char)name[length];
        if (value < 0x20 || value >= 0x7f || value == ':' || value == '\\') return 0;
        if (value == '/') {
            unsigned segment_bytes = length - segment_start;
            if (segment_bytes == 0) return 0;
            if (segment_bytes == 1 && name[segment_start] == '.') return 0;
            if (segment_bytes == 2 && name[segment_start] == '.' && name[segment_start + 1] == '.') return 0;
            segment_start = length + 1;
        }
        ++length;
        if (length > PW_ZIP32_NAME_MAX) return 0;
    }
    if (length == segment_start) return 0;
    if (length - segment_start == 1 && name[segment_start] == '.') return 0;
    if (length - segment_start == 2 && name[segment_start] == '.' && name[segment_start + 1] == '.') return 0;
    for (index = 0; index < length; ++index) {
        if (name[index] == 0) return 0;
    }
    *name_bytes = length;
    return 1;
}

static int write_local_header(FILE *file, const PwZip32Record *record)
{
    if (!write_u32(file, 0x04034b50UL) ||
        !write_u16(file, PW_ZIP32_VERSION) ||
        !write_u16(file, PW_ZIP32_UTF8_FLAG) ||
        !write_u16(file, 0) || /* STORE */
        !write_u16(file, 0) || /* midnight */
        !write_u16(file, PW_ZIP32_DOS_DATE) ||
        !write_u32(file, record->crc) ||
        !write_u32(file, record->size) ||
        !write_u32(file, record->size) ||
        !write_u16(file, record->name_bytes) ||
        !write_u16(file, 0)) return 0;
    return fwrite(record->name, 1, record->name_bytes, file) == record->name_bytes;
}

static int write_central_record(FILE *file, const PwZip32Record *record)
{
    if (!write_u32(file, 0x02014b50UL) ||
        !write_u16(file, PW_ZIP32_VERSION) || /* made by: DOS, ZIP 2.0 */
        !write_u16(file, PW_ZIP32_VERSION) ||
        !write_u16(file, PW_ZIP32_UTF8_FLAG) ||
        !write_u16(file, 0) ||
        !write_u16(file, 0) ||
        !write_u16(file, PW_ZIP32_DOS_DATE) ||
        !write_u32(file, record->crc) ||
        !write_u32(file, record->size) ||
        !write_u32(file, record->size) ||
        !write_u16(file, record->name_bytes) ||
        !write_u16(file, 0) ||
        !write_u16(file, 0) ||
        !write_u16(file, 0) ||
        !write_u16(file, 0) ||
        !write_u32(file, 0x20UL) || /* DOS archive attribute */
        !write_u32(file, record->local_offset)) return 0;
    return fwrite(record->name, 1, record->name_bytes, file) == record->name_bytes;
}

int pw_zip32_write_store(const char *path,
                         const PwZip32Entry *entries,
                         unsigned long entry_count)
{
    PwZip32Record *records;
    FILE *file;
    unsigned long index;
    long central_offset;
    long end_offset;
    unsigned long central_size;
    int result = PW_ZIP32_OK;

    if (path == NULL || path[0] == 0 || entries == NULL || entry_count == 0)
        return PW_ZIP32_BAD_ARGUMENT;
    if (entry_count > PW_ZIP32_MAX_ENTRIES || entry_count > 65535UL)
        return PW_ZIP32_UNSUPPORTED;

    records = (PwZip32Record *)calloc(entry_count, sizeof(PwZip32Record));
    if (records == NULL) return PW_ZIP32_NO_MEMORY;

    for (index = 0; index < entry_count; ++index) {
        unsigned name_bytes;
        unsigned long prior;
        if (!valid_name(entries[index].name, &name_bytes) ||
            (entries[index].data == NULL && entries[index].size != 0)) {
            result = PW_ZIP32_BAD_ARGUMENT;
            goto done;
        }
        for (prior = 0; prior < index; ++prior) {
            if (strcmp(entries[index].name, records[prior].name) == 0) {
                result = PW_ZIP32_BAD_ARGUMENT;
                goto done;
            }
        }
        memcpy(records[index].name, entries[index].name, name_bytes + 1);
        records[index].name_bytes = (pw_zip_u16)name_bytes;
        records[index].size = entries[index].size;
        records[index].crc = pw_zip32_crc32(entries[index].data, entries[index].size);
    }

    file = fopen(path, "wb");
    if (file == NULL) {
        result = PW_ZIP32_IO_ERROR;
        goto done;
    }

    for (index = 0; index < entry_count; ++index) {
        long offset = ftell(file);
        if (offset < 0 || (unsigned long)offset > 0x7fffffffUL) {
            result = PW_ZIP32_UNSUPPORTED;
            break;
        }
        records[index].local_offset = (pw_zip_u32)offset;
        if (!write_local_header(file, &records[index]) ||
            (entries[index].size != 0 &&
             fwrite(entries[index].data, 1, entries[index].size, file) != entries[index].size)) {
            result = PW_ZIP32_IO_ERROR;
            break;
        }
    }

    central_offset = ftell(file);
    if (result == PW_ZIP32_OK &&
        (central_offset < 0 || (unsigned long)central_offset > 0x7fffffffUL))
        result = PW_ZIP32_UNSUPPORTED;

    if (result == PW_ZIP32_OK) {
        for (index = 0; index < entry_count; ++index) {
            if (!write_central_record(file, &records[index])) {
                result = PW_ZIP32_IO_ERROR;
                break;
            }
        }
    }

    end_offset = ftell(file);
    if (result == PW_ZIP32_OK &&
        (end_offset < 0 || central_offset < 0 || end_offset < central_offset ||
         (unsigned long)end_offset > 0x7fffffffUL))
        result = PW_ZIP32_UNSUPPORTED;

    if (result == PW_ZIP32_OK) {
        central_size = (unsigned long)(end_offset - central_offset);
        if (!write_u32(file, 0x06054b50UL) ||
            !write_u16(file, 0) ||
            !write_u16(file, 0) ||
            !write_u16(file, (pw_zip_u16)entry_count) ||
            !write_u16(file, (pw_zip_u16)entry_count) ||
            !write_u32(file, (pw_zip_u32)central_size) ||
            !write_u32(file, (pw_zip_u32)central_offset) ||
            !write_u16(file, 0)) result = PW_ZIP32_IO_ERROR;
    }

    if (fclose(file) != 0 && result == PW_ZIP32_OK) result = PW_ZIP32_IO_ERROR;

done:
    free(records);
    return result;
}
