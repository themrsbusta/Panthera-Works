#include "zip32_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zlib.h"

#define PW_ZIP32_EOCD_SIGNATURE 0x06054b50UL
#define PW_ZIP32_CENTRAL_SIGNATURE 0x02014b50UL
#define PW_ZIP32_LOCAL_SIGNATURE 0x04034b50UL
#define PW_ZIP32_EOCD_MAX 65557UL
#define PW_ZIP32_MAX_ENTRIES 4096U
#define PW_ZIP32_MAX_NAME 255U
#define PW_ZIP32_INPUT_CHUNK 16384U

static pw_zip_u16 read_u16(const pw_zip_u8 *bytes)
{
    return (pw_zip_u16)((pw_zip_u16)bytes[0] | ((pw_zip_u16)bytes[1] << 8));
}

static pw_zip_u32 read_u32(const pw_zip_u8 *bytes)
{
    return (pw_zip_u32)bytes[0] |
           ((pw_zip_u32)bytes[1] << 8) |
           ((pw_zip_u32)bytes[2] << 16) |
           ((pw_zip_u32)bytes[3] << 24);
}

static int seek_to(FILE *file, pw_zip_u32 offset)
{
    if (offset > 0x7fffffffUL) return 0;
    return fseek(file, (long)offset, SEEK_SET) == 0;
}

static int read_exact(FILE *file, void *buffer, unsigned long bytes)
{
    return bytes == 0 || fread(buffer, 1, bytes, file) == bytes;
}

static int valid_member_name(const char *name, unsigned *name_bytes)
{
    unsigned length = 0;
    unsigned segment_start = 0;
    if (name == NULL || name[0] == 0 || name[0] == '/' || name[0] == '\\') return 0;
    while (name[length] != 0) {
        unsigned char ch = (unsigned char)name[length];
        if (ch < 0x20 || ch >= 0x7f || ch == ':' || ch == '\\') return 0;
        if (ch == '/') {
            unsigned segment = length - segment_start;
            if (segment == 0 || (segment == 1 && name[segment_start] == '.') ||
                (segment == 2 && name[segment_start] == '.' && name[segment_start + 1] == '.')) return 0;
            segment_start = length + 1;
        }
        if (++length > PW_ZIP32_MAX_NAME) return 0;
    }
    if (length == segment_start) return 0;
    if (length - segment_start == 1 && name[segment_start] == '.') return 0;
    if (length - segment_start == 2 && name[segment_start] == '.' && name[segment_start + 1] == '.') return 0;
    *name_bytes = length;
    return 1;
}

static int inflate_member(FILE *file, pw_zip_u32 data_offset,
                          pw_zip_u32 compressed_size, pw_zip_u32 expected_size,
                          pw_zip_u8 *output)
{
    z_stream stream;
    pw_zip_u8 input[PW_ZIP32_INPUT_CHUNK];
    pw_zip_u8 overflow_byte;
    pw_zip_u32 input_read = 0;
    int result;
    memset(&stream, 0, sizeof(stream));
    result = inflateInit2(&stream, -MAX_WBITS);
    if (result != Z_OK) return 0;
    if (!seek_to(file, data_offset)) {
        inflateEnd(&stream);
        return 0;
    }

    for (;;) {
        uLong before_in;
        uLong before_out;
        pw_zip_u32 remaining;

        if (stream.avail_in == 0 && input_read < compressed_size) {
            pw_zip_u32 chunk = compressed_size - input_read;
            if (chunk > PW_ZIP32_INPUT_CHUNK) chunk = PW_ZIP32_INPUT_CHUNK;
            if (!read_exact(file, input, chunk)) {
                inflateEnd(&stream);
                return 0;
            }
            stream.next_in = input;
            stream.avail_in = (uInt)chunk;
            input_read += chunk;
        }

        remaining = expected_size - (pw_zip_u32)stream.total_out;
        if (remaining == 0) {
            stream.next_out = &overflow_byte;
            stream.avail_out = 1;
        } else {
            stream.next_out = output + (pw_zip_u32)stream.total_out;
            stream.avail_out = (uInt)remaining;
        }
        before_in = stream.total_in;
        before_out = stream.total_out;
        result = inflate(&stream, Z_NO_FLUSH);
        if ((pw_zip_u32)stream.total_out > expected_size) {
            inflateEnd(&stream);
            return 0;
        }
        if (result == Z_STREAM_END) break;
        if (result != Z_OK || (before_in == stream.total_in && before_out == stream.total_out &&
                              input_read == compressed_size && stream.avail_in == 0)) {
            inflateEnd(&stream);
            return 0;
        }
    }

    result = stream.total_out == expected_size && stream.total_in == compressed_size;
    inflateEnd(&stream);
    return result;
}

void pw_zip32_buffer_dispose(PwZip32Buffer *buffer)
{
    if (buffer == NULL) return;
    free(buffer->data);
    buffer->data = NULL;
    buffer->size = 0;
}

int pw_zip32_read_member(const char *path,
                         const char *member_name,
                         pw_zip_u32 max_output_bytes,
                         PwZip32Buffer *output)
{
    FILE *file = NULL;
    long signed_size;
    pw_zip_u32 file_size;
    pw_zip_u32 tail_size;
    pw_zip_u32 tail_offset;
    pw_zip_u8 *tail = NULL;
    pw_zip_u8 eocd[22];
    pw_zip_u32 eocd_offset = 0;
    pw_zip_u16 entry_count;
    pw_zip_u32 central_size;
    pw_zip_u32 central_offset;
    pw_zip_u32 central_cursor;
    pw_zip_u32 selected_local_offset = 0;
    pw_zip_u32 selected_compressed = 0;
    pw_zip_u32 selected_uncompressed = 0;
    pw_zip_u32 selected_crc = 0;
    pw_zip_u16 selected_method = 0;
    pw_zip_u16 selected_flags = 0;
    unsigned requested_name_bytes;
    unsigned matches = 0;
    unsigned index;
    int result = PW_ZIP32_CORRUPT;
    PwZip32Buffer decoded;

    decoded.data = NULL;
    decoded.size = 0;
    if (output == NULL) return PW_ZIP32_BAD_ARGUMENT;
    output->data = NULL;
    output->size = 0;
    if (path == NULL || path[0] == 0 || !valid_member_name(member_name, &requested_name_bytes))
        return PW_ZIP32_BAD_ARGUMENT;
    if (max_output_bytes > 0x7ffffffeUL) return PW_ZIP32_TOO_LARGE;

    file = fopen(path, "rb");
    if (file == NULL) return PW_ZIP32_IO_ERROR;
    if (fseek(file, 0L, SEEK_END) != 0 || (signed_size = ftell(file)) < 22L) {
        result = PW_ZIP32_CORRUPT;
        goto done;
    }
    if ((unsigned long)signed_size > 0x7fffffffUL) {
        result = PW_ZIP32_UNSUPPORTED;
        goto done;
    }
    file_size = (pw_zip_u32)signed_size;
    tail_size = file_size < PW_ZIP32_EOCD_MAX ? file_size : PW_ZIP32_EOCD_MAX;
    tail_offset = file_size - tail_size;
    tail = (pw_zip_u8 *)malloc(tail_size);
    if (tail == NULL) { result = PW_ZIP32_NO_MEMORY; goto done; }
    if (!seek_to(file, tail_offset) || !read_exact(file, tail, tail_size)) {
        result = PW_ZIP32_IO_ERROR;
        goto done;
    }

    {
        long cursor = (long)tail_size - 22L;
        int found = 0;
        for (; cursor >= 0; --cursor) {
            pw_zip_u16 comment;
            if (read_u32(tail + cursor) != PW_ZIP32_EOCD_SIGNATURE) continue;
            comment = read_u16(tail + cursor + 20);
            if ((pw_zip_u32)cursor + 22UL + comment != tail_size) continue;
            memcpy(eocd, tail + cursor, sizeof(eocd));
            eocd_offset = tail_offset + (pw_zip_u32)cursor;
            found = 1;
            break;
        }
        if (!found) { result = PW_ZIP32_CORRUPT; goto done; }
    }

    if (read_u16(eocd + 4) != 0 || read_u16(eocd + 6) != 0 ||
        read_u16(eocd + 8) != read_u16(eocd + 10) ||
        read_u16(eocd + 10) == 0xffffU || read_u32(eocd + 12) == 0xffffffffUL ||
        read_u32(eocd + 16) == 0xffffffffUL) {
        result = PW_ZIP32_UNSUPPORTED;
        goto done;
    }
    entry_count = read_u16(eocd + 10);
    central_size = read_u32(eocd + 12);
    central_offset = read_u32(eocd + 16);
    if (entry_count == 0 || entry_count > PW_ZIP32_MAX_ENTRIES ||
        central_offset > eocd_offset || central_size > eocd_offset - central_offset) {
        result = PW_ZIP32_CORRUPT;
        goto done;
    }

    central_cursor = central_offset;
    for (index = 0; index < entry_count; ++index) {
        pw_zip_u8 header[46];
        pw_zip_u16 name_bytes;
        pw_zip_u16 extra_bytes;
        pw_zip_u16 comment_bytes;
        pw_zip_u16 disk_start;
        pw_zip_u32 record_bytes;
        pw_zip_u8 name[PW_ZIP32_MAX_NAME + 1];
        int selected;

        if (central_cursor > central_offset + central_size ||
            central_offset + central_size - central_cursor < sizeof(header) ||
            !seek_to(file, central_cursor) || !read_exact(file, header, sizeof(header)) ||
            read_u32(header) != PW_ZIP32_CENTRAL_SIGNATURE) {
            result = PW_ZIP32_CORRUPT;
            goto done;
        }
        name_bytes = read_u16(header + 28);
        extra_bytes = read_u16(header + 30);
        comment_bytes = read_u16(header + 32);
        disk_start = read_u16(header + 34);
        record_bytes = 46UL + name_bytes + extra_bytes + comment_bytes;
        if (record_bytes > central_offset + central_size - central_cursor || disk_start != 0) {
            result = PW_ZIP32_CORRUPT;
            goto done;
        }
        selected = name_bytes == requested_name_bytes && name_bytes <= PW_ZIP32_MAX_NAME;
        if (selected) {
            if (!read_exact(file, name, name_bytes)) { result = PW_ZIP32_IO_ERROR; goto done; }
            name[name_bytes] = 0;
            selected = memcmp(name, member_name, name_bytes) == 0;
        }
        if (selected) {
            pw_zip_u16 flags = read_u16(header + 8);
            pw_zip_u16 method = read_u16(header + 10);
            pw_zip_u32 comp = read_u32(header + 20);
            pw_zip_u32 uncomp = read_u32(header + 24);
            if (++matches != 1) { result = PW_ZIP32_CORRUPT; goto done; }
            if ((flags & (1U | 0x40U | 0x2000U)) != 0 ||
                (flags & ~(0x0808U)) != 0 ||
                comp == 0xffffffffUL || uncomp == 0xffffffffUL ||
                read_u32(header + 42) == 0xffffffffUL) {
                result = PW_ZIP32_UNSUPPORTED;
                goto done;
            }
            selected_flags = flags;
            selected_method = method;
            selected_crc = read_u32(header + 16);
            selected_compressed = comp;
            selected_uncompressed = uncomp;
            selected_local_offset = read_u32(header + 42);
        }
        central_cursor += record_bytes;
    }
    if (central_cursor != central_offset + central_size) { result = PW_ZIP32_CORRUPT; goto done; }
    if (matches == 0) { result = PW_ZIP32_NOT_FOUND; goto done; }
    if (selected_uncompressed > max_output_bytes) { result = PW_ZIP32_TOO_LARGE; goto done; }
    if (selected_method != 0 && selected_method != 8) { result = PW_ZIP32_UNSUPPORTED_METHOD; goto done; }
    if (selected_method == 0 && selected_compressed != selected_uncompressed) {
        result = PW_ZIP32_CORRUPT;
        goto done;
    }

    {
        pw_zip_u8 local[30];
        pw_zip_u16 local_name_bytes;
        pw_zip_u16 local_extra_bytes;
        pw_zip_u16 local_flags;
        pw_zip_u16 local_method;
        pw_zip_u32 data_offset;
        pw_zip_u32 data_end;
        pw_zip_u8 local_name[PW_ZIP32_MAX_NAME + 1];

        if (selected_local_offset > central_offset || central_offset - selected_local_offset < sizeof(local) ||
            !seek_to(file, selected_local_offset) || !read_exact(file, local, sizeof(local)) ||
            read_u32(local) != PW_ZIP32_LOCAL_SIGNATURE) {
            result = PW_ZIP32_CORRUPT;
            goto done;
        }
        local_flags = read_u16(local + 6);
        local_method = read_u16(local + 8);
        local_name_bytes = read_u16(local + 26);
        local_extra_bytes = read_u16(local + 28);
        if (local_name_bytes != requested_name_bytes || local_name_bytes > PW_ZIP32_MAX_NAME ||
            local_method != selected_method || local_flags != selected_flags ||
            (local_flags & (1U | 0x40U | 0x2000U)) != 0) {
            result = PW_ZIP32_CORRUPT;
            goto done;
        }
        if (!read_exact(file, local_name, local_name_bytes) ||
            memcmp(local_name, member_name, local_name_bytes) != 0) {
            result = PW_ZIP32_CORRUPT;
            goto done;
        }
        data_offset = selected_local_offset + 30UL + local_name_bytes + local_extra_bytes;
        if (data_offset > central_offset || selected_compressed > central_offset - data_offset) {
            result = PW_ZIP32_CORRUPT;
            goto done;
        }
        data_end = data_offset + selected_compressed;
        if (data_end > central_offset || data_end > file_size) {
            result = PW_ZIP32_CORRUPT;
            goto done;
        }
        decoded.data = (pw_zip_u8 *)malloc((unsigned long)selected_uncompressed + 1UL);
        if (decoded.data == NULL) { result = PW_ZIP32_NO_MEMORY; goto done; }
        decoded.size = selected_uncompressed;
        if (selected_method == 0) {
            if (!seek_to(file, data_offset) || !read_exact(file, decoded.data, selected_compressed)) {
                result = PW_ZIP32_IO_ERROR;
                goto done;
            }
        } else if (!inflate_member(file, data_offset, selected_compressed,
                                   selected_uncompressed, decoded.data)) {
            result = PW_ZIP32_CORRUPT;
            goto done;
        }
        decoded.data[decoded.size] = 0;
        if (pw_zip32_crc32(decoded.data, decoded.size) != selected_crc) {
            result = PW_ZIP32_BAD_CRC;
            goto done;
        }
    }

    output->data = decoded.data;
    output->size = decoded.size;
    decoded.data = NULL;
    result = PW_ZIP32_OK;

done:
    if (file != NULL) fclose(file);
    free(tail);
    pw_zip32_buffer_dispose(&decoded);
    return result;
}
