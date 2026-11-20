#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>

#include "lab_delimited.h"

#define LAB_DELIMITED_PATH_BYTES 260
#define LAB_DELIMITED_RAW_BYTES (LAB_MAX_CELL_INPUT * 4UL + 1UL)

static const unsigned short cp437_high[128] = {
    0x00c7,0x00fc,0x00e9,0x00e2,0x00e4,0x00e0,0x00e5,0x00e7,
    0x00ea,0x00eb,0x00e8,0x00ef,0x00ee,0x00ec,0x00c4,0x00c5,
    0x00c9,0x00e6,0x00c6,0x00f4,0x00f6,0x00f2,0x00fb,0x00f9,
    0x00ff,0x00d6,0x00dc,0x00a2,0x00a3,0x00a5,0x20a7,0x0192,
    0x00e1,0x00ed,0x00f3,0x00fa,0x00f1,0x00d1,0x00aa,0x00ba,
    0x00bf,0x2310,0x00ac,0x00bd,0x00bc,0x00a1,0x00ab,0x00bb,
    0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,
    0x2555,0x2563,0x2551,0x2557,0x255d,0x255c,0x255b,0x2510,
    0x2514,0x2534,0x252c,0x251c,0x2500,0x253c,0x255e,0x255f,
    0x255a,0x2554,0x2569,0x2566,0x2560,0x2550,0x256c,0x2567,
    0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256b,
    0x256a,0x2518,0x250c,0x2588,0x2584,0x258c,0x2590,0x2580,
    0x03b1,0x00df,0x0393,0x03c0,0x03a3,0x03c3,0x00b5,0x03c4,
    0x03a6,0x0398,0x03a9,0x03b4,0x221e,0x03c6,0x03b5,0x2229,
    0x2261,0x00b1,0x2265,0x2264,0x2320,0x2321,0x00f7,0x2248,
    0x00b0,0x2219,0x00b7,0x221a,0x207f,0x00b2,0x25a0,0x00a0
};

typedef struct {
    const LabCell **cells;
    LabU32 count;
    LabU32 capacity;
    int failed;
} LabDelimitedCells;

static int cp437_encode(unsigned long codepoint)
{
    unsigned i;
    if (codepoint < 0x80) return (int)codepoint;
    for (i = 0; i < 128; ++i)
        if (cp437_high[i] == codepoint) return (int)(i + 0x80);
    return '?';
}

static int utf8_to_cp437(const unsigned char *input, unsigned input_bytes,
                         char *output, unsigned output_capacity,
                         unsigned *output_bytes)
{
    unsigned in = 0;
    unsigned out = 0;
    while (in < input_bytes) {
        unsigned long cp;
        unsigned char lead = input[in++];
        if (lead < 0x80) cp = lead;
        else if ((lead & 0xe0) == 0xc0 && in < input_bytes) {
            unsigned char next = input[in++];
            if ((next & 0xc0) != 0x80) return 0;
            cp = ((unsigned long)(lead & 0x1f) << 6) | (next & 0x3f);
            if (cp < 0x80) return 0;
        } else if ((lead & 0xf0) == 0xe0 && in + 1 < input_bytes) {
            unsigned char next1 = input[in++];
            unsigned char next2 = input[in++];
            if ((next1 & 0xc0) != 0x80 || (next2 & 0xc0) != 0x80) return 0;
            cp = ((unsigned long)(lead & 0x0f) << 12) |
                 ((unsigned long)(next1 & 0x3f) << 6) | (next2 & 0x3f);
            if (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
        } else if ((lead & 0xf8) == 0xf0 && in + 2 < input_bytes) {
            unsigned char next1 = input[in++];
            unsigned char next2 = input[in++];
            unsigned char next3 = input[in++];
            if ((next1 & 0xc0) != 0x80 || (next2 & 0xc0) != 0x80 ||
                (next3 & 0xc0) != 0x80) return 0;
            cp = ((unsigned long)(lead & 0x07) << 18) |
                 ((unsigned long)(next1 & 0x3f) << 12) |
                 ((unsigned long)(next2 & 0x3f) << 6) | (next3 & 0x3f);
            if (cp < 0x10000 || cp > 0x10ffff) return 0;
        } else return 0;
        if (cp == 0 || (cp < 0x20 && cp != 9 && cp != 10 && cp != 13)) return 0;
        if (out + 1 >= output_capacity) return 0;
        output[out++] = (char)cp437_encode(cp);
    }
    output[out] = '\0';
    *output_bytes = out;
    return 1;
}

static int write_utf8(FILE *file, unsigned long cp)
{
    unsigned char bytes[3];
    unsigned count;
    if (cp < 0x80) { bytes[0] = (unsigned char)cp; count = 1; }
    else if (cp < 0x800) {
        bytes[0] = (unsigned char)(0xc0 | (cp >> 6));
        bytes[1] = (unsigned char)(0x80 | (cp & 0x3f));
        count = 2;
    } else {
        bytes[0] = (unsigned char)(0xe0 | (cp >> 12));
        bytes[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        bytes[2] = (unsigned char)(0x80 | (cp & 0x3f));
        count = 3;
    }
    return fwrite(bytes, 1, count, file) == count;
}

static int write_field(FILE *file, const char *value, unsigned char delimiter)
{
    unsigned i;
    unsigned bytes = (unsigned)strlen(value);
    int quote = 0;
    if (bytes != 0 && (value[0] == ' ' || value[bytes - 1] == ' ' ||
                       value[0] == '\t' || value[bytes - 1] == '\t')) quote = 1;
    for (i = 0; i < bytes; ++i)
        if ((unsigned char)value[i] == delimiter || value[i] == '"' ||
            value[i] == '\r' || value[i] == '\n') quote = 1;
    if (quote && fputc('"', file) == EOF) return 0;
    for (i = 0; i < bytes; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (quote && c == '"' && fputc('"', file) == EOF) return 0;
        if (c < 0x80) {
            if (fputc(c, file) == EOF) return 0;
        } else if (!write_utf8(file, cp437_high[c - 0x80])) return 0;
    }
    return !quote || fputc('"', file) != EOF;
}

static int cell_pointer_compare(const void *left, const void *right)
{
    const LabCell *a = *(const LabCell * const *)left;
    const LabCell *b = *(const LabCell * const *)right;
    if (a->row < b->row) return -1;
    if (a->row > b->row) return 1;
    if (a->column < b->column) return -1;
    if (a->column > b->column) return 1;
    return 0;
}

static int collect_cell(const LabCell *cell, void *context)
{
    LabDelimitedCells *list = (LabDelimitedCells *)context;
    if (list->count >= list->capacity) { list->failed = 1; return 0; }
    list->cells[list->count++] = cell;
    return 1;
}

static int field_to_sheet(LabSheet *sheet, const unsigned char *raw,
                          unsigned raw_bytes, char *decoded,
                          unsigned decoded_capacity, LabU32 row,
                          LabU32 column)
{
    unsigned decoded_bytes;
    int result;
    if (row >= LAB_MAX_ROWS || column >= LAB_MAX_COLUMNS ||
        !utf8_to_cp437(raw, raw_bytes, decoded, decoded_capacity, &decoded_bytes))
        return LAB_DELIMITED_BAD_FORMAT;
    result = lab_sheet_set_input(sheet, row, (LabU16)column, decoded);
    if (result == LAB_MODEL_OK) return LAB_DELIMITED_OK;
    return result == LAB_MODEL_NO_MEMORY ? LAB_DELIMITED_NO_MEMORY : LAB_DELIMITED_BAD_FORMAT;
}

static int append_raw(unsigned char *field, unsigned *used, int value)
{
    if (value < 0 || *used + 1 >= LAB_DELIMITED_RAW_BYTES) return 0;
    field[(*used)++] = (unsigned char)value;
    field[*used] = 0;
    return 1;
}

int lab_delimited_load(LabWorkbook **workbook, const char *path,
                       unsigned char delimiter)
{
    FILE *file = NULL;
    LabWorkbook *candidate = NULL;
    LabSheet *sheet;
    unsigned char *field = NULL;
    char *decoded = NULL;
    unsigned used = 0;
    LabU32 row = 0;
    LabU32 column = 0;
    int quoted = 0;
    int quote_closed = 0;
    int field_present = 0;
    int row_present = 0;
    int value;
    int result = LAB_DELIMITED_BAD_ARGUMENT;
    if (workbook == NULL || *workbook == NULL || path == NULL ||
        (delimiter != ',' && delimiter != '\t') ||
        strlen(path) >= LAB_DELIMITED_PATH_BYTES) return LAB_DELIMITED_BAD_ARGUMENT;
    file = fopen(path, "rb");
    if (file == NULL) return LAB_DELIMITED_IO_ERROR;
    field = (unsigned char *)malloc(LAB_DELIMITED_RAW_BYTES);
    decoded = (char *)malloc(LAB_MAX_CELL_INPUT + 1UL);
    candidate = lab_workbook_create();
    if (field == NULL || decoded == NULL || candidate == NULL) {
        result = LAB_DELIMITED_NO_MEMORY;
        goto done;
    }
    sheet = lab_workbook_active_sheet(candidate);
    field[0] = 0;
    /* Skip a UTF-8 BOM when present; otherwise rewind to the first byte. */
    {
        unsigned char bom[3];
        size_t got = fread(bom, 1, 3, file);
        if (got != 3 || bom[0] != 0xef || bom[1] != 0xbb || bom[2] != 0xbf) {
            clearerr(file);
            if (fseek(file, 0L, SEEK_SET) != 0) { result = LAB_DELIMITED_IO_ERROR; goto done; }
        }
    }
    result = LAB_DELIMITED_OK;
    while ((value = fgetc(file)) != EOF) {
        if (quoted) {
            if (value == '"') {
                int next = fgetc(file);
                if (next == '"') {
                    if (!append_raw(field, &used, '"')) { result = LAB_DELIMITED_BAD_FORMAT; goto done; }
                } else {
                    quoted = 0;
                    quote_closed = 1;
                    if (next != EOF && ungetc(next, file) == EOF) { result = LAB_DELIMITED_IO_ERROR; goto done; }
                }
            } else if (!append_raw(field, &used, value)) {
                result = LAB_DELIMITED_BAD_FORMAT;
                goto done;
            }
            continue;
        }
        if (quote_closed) {
            if (value == delimiter) {
                result = field_to_sheet(sheet, field, used, decoded,
                                        LAB_MAX_CELL_INPUT + 1UL, row, column);
                if (result != LAB_DELIMITED_OK) goto done;
                if (column + 1 >= LAB_MAX_COLUMNS) { result = LAB_DELIMITED_BAD_FORMAT; goto done; }
                ++column; used = 0; field[0] = 0;
                quote_closed = 0; field_present = 0; row_present = 1;
                continue;
            }
            if (value == '\r' || value == '\n') {
                result = field_to_sheet(sheet, field, used, decoded,
                                        LAB_MAX_CELL_INPUT + 1UL, row, column);
                if (result != LAB_DELIMITED_OK) goto done;
                if (value == '\r') { int next = fgetc(file); if (next != '\n' && next != EOF && ungetc(next, file) == EOF) { result = LAB_DELIMITED_IO_ERROR; goto done; } }
                if (row + 1 > LAB_MAX_ROWS) { result = LAB_DELIMITED_BAD_FORMAT; goto done; }
                ++row; column = 0; used = 0; field[0] = 0;
                quote_closed = 0; field_present = 0; row_present = 0;
                continue;
            }
            result = LAB_DELIMITED_BAD_FORMAT;
            goto done;
        }
        if (value == '"') {
            if (field_present || used != 0) { result = LAB_DELIMITED_BAD_FORMAT; goto done; }
            quoted = 1; field_present = 1; row_present = 1;
        } else if (value == delimiter) {
            result = field_to_sheet(sheet, field, used, decoded,
                                    LAB_MAX_CELL_INPUT + 1UL, row, column);
            if (result != LAB_DELIMITED_OK) goto done;
            if (column + 1 >= LAB_MAX_COLUMNS) { result = LAB_DELIMITED_BAD_FORMAT; goto done; }
            ++column; used = 0; field[0] = 0; field_present = 0; row_present = 1;
        } else if (value == '\r' || value == '\n') {
            result = field_to_sheet(sheet, field, used, decoded,
                                    LAB_MAX_CELL_INPUT + 1UL, row, column);
            if (result != LAB_DELIMITED_OK) goto done;
            if (value == '\r') { int next = fgetc(file); if (next != '\n' && next != EOF && ungetc(next, file) == EOF) { result = LAB_DELIMITED_IO_ERROR; goto done; } }
            if (row + 1 > LAB_MAX_ROWS) { result = LAB_DELIMITED_BAD_FORMAT; goto done; }
            ++row; column = 0; used = 0; field[0] = 0; field_present = 0; row_present = 0;
        } else {
            field_present = 1;
            row_present = 1;
            if (!append_raw(field, &used, value)) { result = LAB_DELIMITED_BAD_FORMAT; goto done; }
        }
    }
    if (ferror(file)) { result = LAB_DELIMITED_IO_ERROR; goto done; }
    if (quoted) { result = LAB_DELIMITED_BAD_FORMAT; goto done; }
    if (field_present || row_present || quote_closed) {
        result = field_to_sheet(sheet, field, used, decoded,
                                LAB_MAX_CELL_INPUT + 1UL, row, column);
        if (result != LAB_DELIMITED_OK) goto done;
    }
    if (lab_sheet_recalculate(sheet) != LAB_MODEL_OK) {
        result = LAB_DELIMITED_NO_MEMORY;
        goto done;
    }
    result = LAB_DELIMITED_OK;
done:
    if (file != NULL && fclose(file) != 0 && result == LAB_DELIMITED_OK)
        result = LAB_DELIMITED_IO_ERROR;
    if (field != NULL) free(field);
    if (decoded != NULL) free(decoded);
    if (result == LAB_DELIMITED_OK) {
        lab_workbook_destroy(*workbook);
        *workbook = candidate;
    } else lab_workbook_destroy(candidate);
    return result;
}

int lab_delimited_save(const LabSheet *sheet, const char *path,
                       unsigned char delimiter)
{
    LabDelimitedCells list;
    char temporary[LAB_DELIMITED_PATH_BYTES];
    char backup[LAB_DELIMITED_PATH_BYTES];
    FILE *file = NULL;
    LabU32 max_row = 0;
    LabU16 max_column = 0;
    LabU32 index = 0;
    LabU32 row;
    LabU32 column;
    int have_cells = 0;
    int had_original;
    int result = LAB_DELIMITED_BAD_ARGUMENT;
    size_t path_bytes;
    memset(&list, 0, sizeof(list));
    temporary[0] = '\0';
    backup[0] = '\0';
    if (sheet == NULL || path == NULL || (delimiter != ',' && delimiter != '\t'))
        return LAB_DELIMITED_BAD_ARGUMENT;
    path_bytes = strlen(path);
    if (path_bytes < 5 || path_bytes >= sizeof(temporary) || path[path_bytes - 4] != '.')
        return LAB_DELIMITED_BAD_ARGUMENT;
    list.capacity = lab_sheet_count(sheet);
    if (list.capacity != 0) {
        list.cells = (const LabCell **)malloc(list.capacity * sizeof(LabCell *));
        if (list.cells == NULL) return LAB_DELIMITED_NO_MEMORY;
        if (!lab_sheet_visit(sheet, collect_cell, &list) || list.failed || list.count != list.capacity) {
            result = LAB_DELIMITED_NO_MEMORY;
            goto done;
        }
        qsort(list.cells, list.count, sizeof(LabCell *), cell_pointer_compare);
        max_row = list.cells[list.count - 1]->row;
        max_column = list.cells[list.count - 1]->column;
        have_cells = 1;
    }
    memcpy(temporary, path, path_bytes + 1);
    memcpy(backup, path, path_bytes + 1);
    memcpy(temporary + path_bytes - 3, "TMP", 4);
    memcpy(backup + path_bytes - 3, "BAK", 4);
    remove(temporary);
    file = fopen(temporary, "wb");
    if (file == NULL) { result = LAB_DELIMITED_IO_ERROR; goto done; }
    if (have_cells) {
        for (row = 0; row <= max_row; ++row) {
            for (column = 0; column <= max_column; ++column) {
                const char *value = "";
                if (index < list.count && list.cells[index]->row == row &&
                    list.cells[index]->column == column) {
                    value = list.cells[index]->input != NULL ? list.cells[index]->input : "";
                    ++index;
                }
                if (!write_field(file, value, delimiter) ||
                    (column < max_column && fputc(delimiter, file) == EOF)) {
                    result = LAB_DELIMITED_IO_ERROR;
                    goto done;
                }
            }
            if (fputs("\r\n", file) == EOF) { result = LAB_DELIMITED_IO_ERROR; goto done; }
        }
    }
    if (fflush(file) != 0 || ferror(file)) { result = LAB_DELIMITED_IO_ERROR; goto done; }
    if (fclose(file) != 0) { file = NULL; result = LAB_DELIMITED_IO_ERROR; goto done; }
    file = NULL;
    had_original = access(path, 0) == 0;
    if (had_original) {
        remove(backup);
        if (rename(path, backup) != 0) { result = LAB_DELIMITED_IO_ERROR; goto done; }
    }
    if (rename(temporary, path) != 0) {
        if (had_original) rename(backup, path);
        result = LAB_DELIMITED_IO_ERROR;
        goto done;
    }
    result = LAB_DELIMITED_OK;
done:
    if (file != NULL) fclose(file);
    if (result != LAB_DELIMITED_OK && temporary[0] != '\0') remove(temporary);
    if (list.cells != NULL) free((void *)list.cells);
    return result;
}
