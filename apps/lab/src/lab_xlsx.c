/* XLSX writer: original SpreadsheetML parts in an uncompressed ZIP package. */
#include <ctype.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lab_xlsx.h"
#include "lab_inflate.h"

#define LAB_XLSX_PATH_BYTES 260


typedef struct {
    char name[64];
    unsigned name_bytes;
    LabU32 crc;
    LabU32 bytes;
    LabU32 local_offset;
} LabZipEntry;

typedef struct {
    FILE *file;
    LabZipEntry *entries;
    unsigned count;
    unsigned capacity;
    LabU32 crc;
    LabU32 entry_bytes;
    int failed;
} LabZipWriter;

typedef struct {
    const LabCell **cells;
    LabU32 capacity;
    LabU32 count;
    int failed;
} LabCellList;

static const LabU16 cp437_high[128] = {
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

static int zip_write_bytes(LabZipWriter *zip, const void *data, unsigned bytes)
{
    const unsigned char *cursor = (const unsigned char *)data;
    unsigned i;
    if (zip == NULL || zip->file == NULL || zip->failed) return 0;
    if (bytes != 0 && fwrite(data, 1, bytes, zip->file) != bytes) {
        zip->failed = 1;
        return 0;
    }
    for (i = 0; i < bytes; ++i) {
        unsigned bit;
        zip->crc ^= cursor[i];
        for (bit = 0; bit < 8; ++bit)
            zip->crc = (zip->crc >> 1) ^ ((zip->crc & 1) ? 0xedb88320UL : 0);
    }
    if (0xffffffffUL - zip->entry_bytes < bytes) {
        zip->failed = 1;
        return 0;
    }
    zip->entry_bytes += bytes;
    return 1;
}

static int zip_u16(LabZipWriter *zip, unsigned value)
{
    unsigned char b[2];
    b[0] = (unsigned char)value;
    b[1] = (unsigned char)(value >> 8);
    return zip_write_bytes(zip, b, 2);
}

static int zip_u32(LabZipWriter *zip, LabU32 value)
{
    unsigned char b[4];
    b[0] = (unsigned char)value;
    b[1] = (unsigned char)(value >> 8);
    b[2] = (unsigned char)(value >> 16);
    b[3] = (unsigned char)(value >> 24);
    return zip_write_bytes(zip, b, 4);
}

static int zip_begin_entry(LabZipWriter *zip, const char *name)
{
    LabZipEntry *entry;
    LabZipEntry *next;
    long offset;
    unsigned length;
    unsigned capacity;
    if (zip == NULL || name == NULL || zip->count >= 65535U) return 0;
    length = (unsigned)strlen(name);
    if (length == 0 || length >= sizeof(((LabZipEntry *)0)->name)) return 0;
    if (zip->count == zip->capacity) {
        capacity = zip->capacity == 0 ? 8 : zip->capacity * 2;
        if (capacity < zip->capacity || capacity > 65535U ||
            capacity > 0x7fffffffUL / (unsigned)sizeof(LabZipEntry)) return 0;
        next = (LabZipEntry *)realloc(zip->entries, capacity * sizeof(LabZipEntry));
        if (next == NULL) return 0;
        zip->entries = next;
        zip->capacity = capacity;
    }
    offset = ftell(zip->file);
    if (offset < 0) return 0;
    entry = &zip->entries[zip->count];
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->name, name, length + 1);
    entry->name_bytes = length;
    entry->local_offset = (LabU32)offset;
    zip->crc = 0xffffffffUL;
    zip->entry_bytes = 0;
    if (!zip_u32(zip, 0x04034b50UL) || !zip_u16(zip, 20) ||
        !zip_u16(zip, 8) || !zip_u16(zip, 0) || !zip_u16(zip, 0) ||
        !zip_u16(zip, 0) || !zip_u32(zip, 0) || !zip_u32(zip, 0) ||
        !zip_u32(zip, 0) || !zip_u16(zip, length) || !zip_u16(zip, 0) ||
        !zip_write_bytes(zip, name, length)) return 0;
    /* ZIP sizes and CRC cover the entry payload, not its local header. */
    zip->crc = 0xffffffffUL;
    zip->entry_bytes = 0;
    return 1;
}

static int zip_end_entry(LabZipWriter *zip)
{
    LabZipEntry *entry;
    LabU32 crc;
    if (zip == NULL || zip->count >= zip->capacity || zip->failed) return 0;
    entry = &zip->entries[zip->count];
    crc = zip->crc ^ 0xffffffffUL;
    entry->crc = crc;
    entry->bytes = zip->entry_bytes;
    if (!zip_u32(zip, 0x08074b50UL) || !zip_u32(zip, crc) ||
        !zip_u32(zip, entry->bytes) || !zip_u32(zip, entry->bytes)) return 0;
    ++zip->count;
    return 1;
}

static int zip_finish(LabZipWriter *zip)
{
    LabU32 directory_offset;
    LabU32 directory_bytes;
    unsigned i;
    long start;
    long finish;
    if (zip == NULL || zip->failed || zip->count == 0 || zip->count > 65535U) return 0;
    start = ftell(zip->file);
    if (start < 0) return 0;
    directory_offset = (LabU32)start;
    for (i = 0; i < zip->count; ++i) {
        LabZipEntry *entry = &zip->entries[i];
        if (!zip_u32(zip, 0x02014b50UL) || !zip_u16(zip, 20) ||
            !zip_u16(zip, 20) || !zip_u16(zip, 8) || !zip_u16(zip, 0) ||
            !zip_u16(zip, 0) || !zip_u16(zip, 0) || !zip_u32(zip, entry->crc) ||
            !zip_u32(zip, entry->bytes) || !zip_u32(zip, entry->bytes) ||
            !zip_u16(zip, entry->name_bytes) || !zip_u16(zip, 0) ||
            !zip_u16(zip, 0) || !zip_u16(zip, 0) || !zip_u16(zip, 0) ||
            !zip_u32(zip, 0) || !zip_u32(zip, entry->local_offset) ||
            !zip_write_bytes(zip, entry->name, entry->name_bytes)) return 0;
    }
    finish = ftell(zip->file);
    if (finish < start) return 0;
    directory_bytes = (LabU32)(finish - start);
    /* The EOCD is not part of a ZIP entry, so bypass the entry CRC counter. */
    zip->failed = 0;
    if (fwrite("PK\005\006\000\000\000\000", 1, 8, zip->file) != 8) return 0;
    if (!zip_u16(zip, zip->count) || !zip_u16(zip, zip->count) ||
        !zip_u32(zip, directory_bytes) || !zip_u32(zip, directory_offset) ||
        !zip_u16(zip, 0)) return 0;
    return !zip->failed && fflush(zip->file) == 0;
}

static int xml_ascii(LabZipWriter *zip, const char *text)
{
    return zip_write_bytes(zip, text, (unsigned)strlen(text));
}

static int xml_codepoint(LabZipWriter *zip, LabU16 value)
{
    unsigned char out[3];
    unsigned bytes;
    if (value < 0x80) { out[0] = (unsigned char)value; bytes = 1; }
    else if (value < 0x800) {
        out[0] = (unsigned char)(0xc0 | (value >> 6));
        out[1] = (unsigned char)(0x80 | (value & 0x3f));
        bytes = 2;
    } else {
        out[0] = (unsigned char)(0xe0 | (value >> 12));
        out[1] = (unsigned char)(0x80 | ((value >> 6) & 0x3f));
        out[2] = (unsigned char)(0x80 | (value & 0x3f));
        bytes = 3;
    }
    return zip_write_bytes(zip, out, bytes);
}

static int xml_text(LabZipWriter *zip, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    while (*cursor != 0) {
        unsigned char c = *cursor++;
        LabU16 value;
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') return 0;
        if (c == '&') { if (!xml_ascii(zip, "&amp;")) return 0; continue; }
        if (c == '<') { if (!xml_ascii(zip, "&lt;")) return 0; continue; }
        if (c == '>') { if (!xml_ascii(zip, "&gt;")) return 0; continue; }
        if (c == '"') { if (!xml_ascii(zip, "&quot;")) return 0; continue; }
        if (c == '\'') { if (!xml_ascii(zip, "&apos;")) return 0; continue; }
        value = c < 0x80 ? c : cp437_high[c - 0x80];
        if (!xml_codepoint(zip, value)) return 0;
    }
    return 1;
}

static int xlsx_static_entry(LabZipWriter *zip, const char *name, const char *body)
{
    return zip_begin_entry(zip, name) && xml_ascii(zip, body) && zip_end_entry(zip);
}

static int cell_pointer_compare(const void *left, const void *right)
{
    const LabCell *a = *(const LabCell **)left;
    const LabCell *b = *(const LabCell **)right;
    if (a->row < b->row) return -1;
    if (a->row > b->row) return 1;
    if (a->column < b->column) return -1;
    if (a->column > b->column) return 1;
    return 0;
}

static int collect_cell(const LabCell *cell, void *context)
{
    LabCellList *list = (LabCellList *)context;
    if (list->count >= list->capacity) { list->failed = 1; return 0; }
    list->cells[list->count++] = cell;
    return 1;
}

static int write_cell(LabZipWriter *zip, const LabCell *cell)
{
    char reference[16];
    char row[16];
    const char *input = cell->input != NULL ? cell->input : "";
    const char *display = lab_cell_display(cell);
    if (!lab_cell_reference_format(cell->row, cell->column, reference, sizeof(reference))) return 0;
    if (cell->kind == LAB_CELL_TEXT) {
        return xml_ascii(zip, "<c r=\"") && xml_ascii(zip, reference) &&
            xml_ascii(zip, "\" t=\"inlineStr\"><is><t xml:space=\"preserve\">") &&
            xml_text(zip, input) && xml_ascii(zip, "</t></is></c>");
    }
    if (cell->kind == LAB_CELL_NUMBER) {
        return xml_ascii(zip, "<c r=\"") && xml_ascii(zip, reference) &&
            xml_ascii(zip, "\"><v>") && xml_text(zip, input) && xml_ascii(zip, "</v></c>");
    }
    if (cell->kind == LAB_CELL_FORMULA) {
        unsigned error = cell->calc_error != LAB_CALC_NONE;
        if (error) {
            return xml_ascii(zip, "<c r=\"") && xml_ascii(zip, reference) &&
                xml_ascii(zip, "\" t=\"e\"><f>") && xml_text(zip, input + 1) &&
                xml_ascii(zip, "</f><v>") && xml_text(zip, display) &&
                xml_ascii(zip, "</v></c>");
        }
        return xml_ascii(zip, "<c r=\"") && xml_ascii(zip, reference) &&
            xml_ascii(zip, "\"><f>") && xml_text(zip, input + 1) &&
            xml_ascii(zip, "</f><v>") && xml_text(zip, display) &&
            xml_ascii(zip, "</v></c>");
    }
    /* Empty and future cell kinds are deliberately omitted. */
    (void)row;
    return 1;
}

static int write_worksheet(LabZipWriter *zip, LabCellList *list,
                           LabWorkbook *workbook, LabU32 sheet_index)
{
    LabU32 i;
    LabU32 active_row;
    LabU16 active_column;
    LabU32 open_row = 0xffffffffUL;
    char row_text[16];
    char active_cell[24];
    char part_name[64];
    sprintf(part_name, "xl/worksheets/sheet%lu.xml", sheet_index + 1);
    if (lab_workbook_get_sheet_selection(workbook, sheet_index,
                                         &active_row, &active_column) != LAB_MODEL_OK ||
        !lab_cell_reference_format(active_row, active_column,
                                   active_cell, sizeof(active_cell)) ||
        !zip_begin_entry(zip, part_name) ||
        !xml_ascii(zip, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>" 
                        "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
                        "<sheetViews><sheetView workbookViewId=\"0\"><selection activeCell=\"") ||
        !xml_ascii(zip, active_cell) ||
        !xml_ascii(zip, "\" sqref=\"") || !xml_ascii(zip, active_cell) ||
        !xml_ascii(zip, "\"/></sheetView></sheetViews><sheetData>")) return 0;
    for (i = 0; i < list->count; ++i) {
        const LabCell *cell = list->cells[i];
        if (cell->kind == LAB_CELL_EMPTY) continue;
        if (cell->row != open_row) {
            if (open_row != 0xffffffffUL && !xml_ascii(zip, "</row>")) return 0;
            open_row = cell->row;
            sprintf(row_text, "%lu", open_row + 1);
            if (!xml_ascii(zip, "<row r=\"") || !xml_ascii(zip, row_text) || !xml_ascii(zip, "\">")) return 0;
        }
        if (!write_cell(zip, cell)) return 0;
    }
    if (open_row != 0xffffffffUL && !xml_ascii(zip, "</row>")) return 0;
    return xml_ascii(zip, "</sheetData></worksheet>") && zip_end_entry(zip);
}

static int path_sidecar(const char *path, const char *extension, char *output, unsigned bytes)
{
    unsigned length;
    unsigned i;
    int dot = -1;
    if (path == NULL || output == NULL) return 0;
    length = (unsigned)strlen(path);
    if (length == 0 || length + 5 >= bytes) return 0;
    for (i = 0; i < length; ++i) {
        if (path[i] == '/' || path[i] == '\\') dot = -1;
        else if (path[i] == '.') dot = (int)i;
    }
    if (dot < 0) return 0;
    memcpy(output, path, (unsigned)dot);
    output[dot] = '\0';
    strcat(output, extension);
    return 1;
}

static int path_is_xlsx(const char *path)
{
    const char *dot;
    unsigned i;
    static const char extension[] = ".XLSX";
    if (path == NULL) return 0;
    dot = strrchr(path, '.');
    if (dot == NULL || strlen(dot) != 5) return 0;
    for (i = 0; i < 5; ++i)
        if (toupper((unsigned char)dot[i]) != extension[i]) return 0;
    return 1;
}

static int write_content_types(LabZipWriter *zip, LabWorkbook *workbook)
{
    LabU32 i;
    char part_name[256];
    if (!zip_begin_entry(zip, "[Content_Types].xml") ||
        !xml_ascii(zip, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                        "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
                        "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>")) return 0;
    for (i = 0; i < lab_workbook_sheet_count(workbook); ++i) {
        sprintf(part_name, "<Override PartName=\"/xl/worksheets/sheet%lu.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>", i + 1);
        if (!xml_ascii(zip, part_name)) return 0;
    }
    return xml_ascii(zip, "</Types>") && zip_end_entry(zip);
}

static int write_workbook_xml(LabZipWriter *zip, LabWorkbook *workbook)
{
    LabU32 i;
    char number[16];
    if (!zip_begin_entry(zip, "xl/workbook.xml") ||
        !xml_ascii(zip, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                        "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
                        "<bookViews><workbookView activeTab=\"")) return 0;
    sprintf(number, "%lu", lab_workbook_active_index(workbook));
    if (!xml_ascii(zip, number) || !xml_ascii(zip, "\"/></bookViews><sheets>")) return 0;
    for (i = 0; i < lab_workbook_sheet_count(workbook); ++i) {
        sprintf(number, "%lu", i + 1);
        if (!xml_ascii(zip, "<sheet name=\"") ||
            !xml_text(zip, lab_workbook_sheet_name(workbook, i)) ||
            !xml_ascii(zip, "\" sheetId=\"") || !xml_ascii(zip, number) ||
            !xml_ascii(zip, "\" r:id=\"rId") || !xml_ascii(zip, number) ||
            !xml_ascii(zip, "\"/>")) return 0;
    }
    return xml_ascii(zip, "</sheets><calcPr calcId=\"191029\" fullCalcOnLoad=\"1\"/></workbook>") &&
           zip_end_entry(zip);
}

static int write_workbook_relationships(LabZipWriter *zip, LabWorkbook *workbook)
{
    LabU32 i;
    char row[256];
    if (!zip_begin_entry(zip, "xl/_rels/workbook.xml.rels") ||
        !xml_ascii(zip, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">")) return 0;
    for (i = 0; i < lab_workbook_sheet_count(workbook); ++i) {
        sprintf(row, "<Relationship Id=\"rId%lu\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet%lu.xml\"/>", i + 1, i + 1);
        if (!xml_ascii(zip, row)) return 0;
    }
    sprintf(row, "<Relationship Id=\"rId%lu\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/></Relationships>", lab_workbook_sheet_count(workbook) + 1);
    return xml_ascii(zip, row) && zip_end_entry(zip);
}

static int write_sheet_package(LabZipWriter *zip, LabWorkbook *workbook,
                               LabU32 index)
{
    LabSheet *sheet = lab_workbook_sheet(workbook, index);
    LabCellList list;
    LabU32 count;
    int ok = 0;
    memset(&list, 0, sizeof(list));
    if (sheet == NULL || lab_sheet_recalculate(sheet) != LAB_MODEL_OK) return 0;
    count = lab_sheet_count(sheet);
    if (count > 0x7fffffffUL / (LabU32)sizeof(LabCell *)) return 0;
    list.capacity = count;
    if (count != 0) {
        list.cells = (const LabCell **)malloc(count * sizeof(LabCell *));
        if (list.cells == NULL) return 0;
        if (!lab_sheet_visit(sheet, collect_cell, &list) || list.failed || list.count != count) goto done;
        qsort(list.cells, list.count, sizeof(LabCell *), cell_pointer_compare);
    }
    ok = write_worksheet(zip, &list, workbook, index);
done:
    free(list.cells);
    return ok;
}

static int make_package(LabWorkbook *workbook, const char *path)
{
    LabZipWriter zip;
    LabU32 i;
    int result = LAB_XLSX_IO_ERROR;
    memset(&zip, 0, sizeof(zip));
    if (workbook == NULL || lab_workbook_sheet_count(workbook) == 0) return LAB_XLSX_BAD_ARGUMENT;
    if (lab_workbook_sheet_count(workbook) > 65530UL) return LAB_XLSX_UNSUPPORTED;
    zip.file = fopen(path, "wb");
    if (zip.file == NULL) return LAB_XLSX_IO_ERROR;
    if (!write_content_types(&zip, workbook) ||
        !xlsx_static_entry(&zip, "_rels/.rels",
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
        "</Relationships>") ||
        !write_workbook_xml(&zip, workbook) ||
        !write_workbook_relationships(&zip, workbook) ||
        !xlsx_static_entry(&zip, "xl/styles.xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        "<fonts count=\"1\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>"
        "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill patternType=\"gray125\"/></fill></fills>"
        "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
        "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
        "<cellXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/></cellXfs>"
        "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles></styleSheet>")) goto done;
    for (i = 0; i < lab_workbook_sheet_count(workbook); ++i)
        if (!write_sheet_package(&zip, workbook, i)) goto done;
    if (!zip_finish(&zip)) goto done;
    result = LAB_XLSX_OK;
done:
    if (fclose(zip.file) != 0) result = LAB_XLSX_IO_ERROR;
    free(zip.entries);
    return result;
}

#define LAB_XLSX_IMPORT_LIMIT (8UL * 1024UL * 1024UL)

static LabU16 zip_get16(const unsigned char *p)
{
    return (LabU16)(p[0] | ((LabU16)p[1] << 8));
}

static LabU32 zip_get32(const unsigned char *p)
{
    return (LabU32)p[0] | ((LabU32)p[1] << 8) |
           ((LabU32)p[2] << 16) | ((LabU32)p[3] << 24);
}

static LabU32 zip_crc32(const unsigned char *data, LabU32 bytes)
{
    LabU32 crc = 0xffffffffUL;
    LabU32 i;
    for (i = 0; i < bytes; ++i) {
        unsigned bit;
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320UL : 0);
    }
    return crc ^ 0xffffffffUL;
}

/* Read stored or raw-DEFLATE ZIP entries from an XLSX package. */
static int zip_read_entry(const char *path, const char *wanted,
                          unsigned char **output, LabU32 *output_bytes)
{
    FILE *file = NULL;
    unsigned char *tail = NULL;
    long file_bytes;
    unsigned tail_bytes;
    unsigned eocd = 0xffffffffUL;
    LabU32 directory_offset;
    LabU32 directory_bytes;
    LabU32 eocd_offset;
    LabU16 entries;
    unsigned i;
    int result = LAB_XLSX_BAD_PACKAGE;
    if (path == NULL || wanted == NULL || output == NULL || output_bytes == NULL) return LAB_XLSX_BAD_ARGUMENT;
    *output = NULL;
    *output_bytes = 0;
    file = fopen(path, "rb");
    if (file == NULL) return LAB_XLSX_IO_ERROR;
    if (fseek(file, 0, SEEK_END) != 0 || (file_bytes = ftell(file)) < 22) goto done;
    tail_bytes = (unsigned)(file_bytes < 65557L ? file_bytes : 65557L);
    tail = (unsigned char *)malloc(tail_bytes);
    if (tail == NULL) { result = LAB_XLSX_NO_MEMORY; goto done; }
    if (fseek(file, file_bytes - tail_bytes, SEEK_SET) != 0 || fread(tail, 1, tail_bytes, file) != tail_bytes) goto done;
    for (i = tail_bytes - 22; ; --i) {
        if (zip_get32(tail + i) == 0x06054b50UL &&
            i + 22 + zip_get16(tail + i + 20) <= tail_bytes) { eocd = i; break; }
        if (i == 0) break;
    }
    if (eocd == 0xffffffffUL) { result = LAB_XLSX_BAD_EOCD; goto done; }
    entries = zip_get16(tail + eocd + 10);
    directory_bytes = zip_get32(tail + eocd + 12);
    directory_offset = zip_get32(tail + eocd + 16);
    eocd_offset = (LabU32)(file_bytes - tail_bytes + eocd);
    if (zip_get16(tail + eocd + 4) != 0 || zip_get16(tail + eocd + 6) != 0 ||
        zip_get16(tail + eocd + 8) != entries || entries > 4096 ||
        directory_offset > 0xffffffffUL - directory_bytes ||
        directory_offset + directory_bytes > eocd_offset) { result = LAB_XLSX_BAD_DIRECTORY; goto done; }
    if (fseek(file, (long)directory_offset, SEEK_SET) != 0) goto done;
    for (i = 0; i < entries; ++i) {
        unsigned char central[46];
        unsigned name_bytes, extra_bytes, comment_bytes;
        unsigned char name[256];
        unsigned char local_filename[256];
        LabU16 method, flags;
        LabU32 compressed, uncompressed, expected_crc, local_offset;
        unsigned char local[30];
        unsigned char *payload = NULL;
        unsigned char *encoded = NULL;
        unsigned local_name, local_extra;
        if (fread(central, 1, sizeof(central), file) != sizeof(central) ||
            zip_get32(central) != 0x02014b50UL) { result = LAB_XLSX_BAD_DIRECTORY; goto done; }
        flags = zip_get16(central + 8); method = zip_get16(central + 10);
        expected_crc = zip_get32(central + 16);
        compressed = zip_get32(central + 20); uncompressed = zip_get32(central + 24);
        name_bytes = zip_get16(central + 28); extra_bytes = zip_get16(central + 30);
        comment_bytes = zip_get16(central + 32); local_offset = zip_get32(central + 42);
        if (name_bytes == 0 || name_bytes >= sizeof(name)) { result = LAB_XLSX_BAD_DIRECTORY; goto done; }
        if (fread(name, 1, name_bytes, file) != name_bytes || memchr(name, 0, name_bytes) != NULL) { result = LAB_XLSX_BAD_DIRECTORY; goto done; }
        name[name_bytes] = 0;
        if (fseek(file, (long)extra_bytes + comment_bytes, SEEK_CUR) != 0) goto done;
        if (strcmp((const char *)name, wanted) != 0) continue;
        if ((flags & 1) != 0) { result = LAB_XLSX_UNSUPPORTED; goto done; }
        if (method != 0 && method != 8) { result = LAB_XLSX_UNSUPPORTED; goto done; }
        if ((method == 0 && compressed != uncompressed) ||
            compressed > LAB_XLSX_IMPORT_LIMIT || uncompressed > LAB_XLSX_IMPORT_LIMIT) {
            result = uncompressed > LAB_XLSX_IMPORT_LIMIT ? LAB_XLSX_PACKAGE_TOO_LARGE : LAB_XLSX_BAD_PACKAGE;
            goto done;
        }
        if (fseek(file, (long)local_offset, SEEK_SET) != 0) { result = LAB_XLSX_LOCAL_SEEK_FAILED; goto done; }
        if (fread(local, 1, sizeof(local), file) != sizeof(local)) { result = LAB_XLSX_LOCAL_READ_FAILED; goto done; }
        if (zip_get32(local) != 0x04034b50UL) { result = LAB_XLSX_LOCAL_SIGNATURE_MISMATCH; goto done; }
        local_name = zip_get16(local + 26); local_extra = zip_get16(local + 28);
        if (zip_get16(local + 6) != flags || zip_get16(local + 8) != method ||
            local_name != name_bytes || local_name >= sizeof(local_filename) ||
            fread(local_filename, 1, local_name, file) != local_name ||
            memcmp(local_filename, name, local_name) != 0 ||
            fseek(file, (long)local_extra, SEEK_CUR) != 0) { result = LAB_XLSX_BAD_LOCAL_FIELDS; goto done; }
        if (compressed != 0) {
            encoded = (unsigned char *)malloc(compressed);
            if (encoded == NULL) { result = LAB_XLSX_NO_MEMORY; goto done; }
            if (fread(encoded, 1, compressed, file) != compressed) { free(encoded); goto done; }
        }
        if (uncompressed != 0) {
            payload = (unsigned char *)malloc(uncompressed + 1);
            if (payload == NULL) { if (encoded != NULL) free(encoded); result = LAB_XLSX_NO_MEMORY; goto done; }
        } else {
            payload = (unsigned char *)malloc(1);
            if (payload == NULL) { if (encoded != NULL) free(encoded); result = LAB_XLSX_NO_MEMORY; goto done; }
        }
        if (method == 0) {
            if (uncompressed != 0) memcpy(payload, encoded, uncompressed);
        } else if (!lab_inflate_raw(encoded, compressed, payload, uncompressed)) {
            if (encoded != NULL) free(encoded);
            free(payload); result = LAB_XLSX_BAD_DEFLATE; goto done;
        }
        if (encoded != NULL) free(encoded);
        if (zip_crc32(payload, uncompressed) != expected_crc) { free(payload); result = LAB_XLSX_BAD_CRC; goto done; }
        payload[uncompressed] = 0;
        *output = payload; *output_bytes = uncompressed;
        result = LAB_XLSX_OK;
        goto done;
    }
    result = LAB_XLSX_ENTRY_NOT_FOUND;
done:
    if (file != NULL) fclose(file);
    if (tail != NULL) free(tail);
    return result;
}

static int cp437_from_unicode(LabU32 value)
{
    unsigned i;
    if (value < 0x80) return (int)value;
    for (i = 0; i < 128; ++i) if (cp437_high[i] == value) return (int)(i + 0x80);
    return '?';
}

static int append_xml_utf8(char *out, unsigned capacity, unsigned *used,
                           const unsigned char *text, unsigned bytes)
{
    unsigned i = 0;
    while (i < bytes) {
        LabU32 cp;
        unsigned char c = text[i++];
        if (c < 0x80) cp = c;
        else if ((c & 0xe0) == 0xc0 && i < bytes) cp = ((LabU32)(c & 0x1f) << 6) | (text[i++] & 0x3f);
        else if ((c & 0xf0) == 0xe0 && i + 1 < bytes) cp = ((LabU32)(c & 0x0f) << 12) | ((LabU32)(text[i++] & 0x3f) << 6) | (text[i++] & 0x3f);
        else return 0;
        if (cp == '&' && i < bytes) {
            unsigned start = i;
            while (i < bytes && text[i] != ';' && i - start < 12) ++i;
            if (i >= bytes || text[i] != ';') return 0;
            if (i - start == 3 && memcmp(text + start, "amp", 3) == 0) cp = '&';
            else if (i - start == 2 && memcmp(text + start, "lt", 2) == 0) cp = '<';
            else if (i - start == 2 && memcmp(text + start, "gt", 2) == 0) cp = '>';
            else if (i - start == 4 && memcmp(text + start, "quot", 4) == 0) cp = '"';
            else if (i - start == 4 && memcmp(text + start, "apos", 4) == 0) cp = '\'';
            else if (i - start > 1 && text[start] == '#') {
                unsigned j = start + 1; unsigned base = 10; LabU32 v = 0;
                if (j < i && (text[j] == 'x' || text[j] == 'X')) { base = 16; ++j; }
                if (j == i) return 0;
                while (j < i) {
                    unsigned d; unsigned char h = text[j++];
                    if (h >= '0' && h <= '9') d = h - '0';
                    else if (base == 16 && h >= 'a' && h <= 'f') d = h - 'a' + 10;
                    else if (base == 16 && h >= 'A' && h <= 'F') d = h - 'A' + 10;
                    else return 0;
                    if (v > (0x10ffffUL - d) / base) return 0;
                    v = v * base + d;
                }
                cp = v;
            } else return 0;
            ++i;
        }
        if (cp == 0 || cp > 0xffff || (cp < 0x20 && cp != 9 && cp != 10 && cp != 13)) return 0;
        if (*used + 1 >= capacity) return 0;
        out[(*used)++] = (char)cp437_from_unicode(cp);
    }
    out[*used] = 0;
    return 1;
}

static int xml_attribute(const char *tag, const char *key, char *out, unsigned capacity)
{
    const char *p = tag;
    unsigned key_bytes = (unsigned)strlen(key);
    while (*p && *p != '>') {
        const char *start;
        unsigned n;
        while (*p && *p != '>' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
        if (!*p || *p == '>') break;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '/') ++p;
        start = p;
        while (*p && *p != '>' && *p != '=' && *p != ' ' && *p != '\t') ++p;
        n = (unsigned)(p - start);
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '=') { while (*p && *p != '>' && *p != ' ') ++p; continue; }
        ++p; while (*p == ' ' || *p == '\t') ++p;
        if ((*p == '\'' || *p == '"') && n == key_bytes && memcmp(start, key, n) == 0) {
            char quote = *p++; unsigned used = 0;
            while (*p && *p != quote) {
                if (used + 1 >= capacity) return 0;
                out[used++] = *p++;
            }
            if (*p != quote) return 0;
            out[used] = 0; return 1;
        }
        if (*p == '\'' || *p == '"') { char quote = *p++; while (*p && *p != quote) ++p; if (*p) ++p; }
    }
    return 0;
}

static const char *xml_find_start_tag_from(const char *cursor, const char *wanted,
                                           const char **tag_end)
{
    unsigned wanted_bytes = (unsigned)strlen(wanted);
    while (cursor != NULL && (cursor = strchr(cursor, '<')) != NULL) {
        const char *name = cursor + 1;
        const char *end;
        unsigned name_bytes = 0;
        if (strncmp(cursor, "<!--", 4) == 0) {
            end = strstr(cursor + 4, "-->");
            if (end == NULL) return NULL;
            cursor = end + 3;
            continue;
        }
        if (strncmp(cursor, "<![CDATA[", 9) == 0) {
            end = strstr(cursor + 9, "]]>" );
            if (end == NULL) return NULL;
            cursor = end + 3;
            continue;
        }
        if (*name == '!') return NULL;
        if (*name == '?') {
            end = strstr(cursor + 2, "?>");
            if (end == NULL) return NULL;
            cursor = end + 2;
            continue;
        }
        if (*name == '/') {
            end = strchr(name, '>');
            if (end == NULL) return NULL;
            cursor = end + 1;
            continue;
        }
        while (name[name_bytes] && name[name_bytes] != '>' &&
               name[name_bytes] != '/' && name[name_bytes] != ' ' &&
               name[name_bytes] != '\t' && name[name_bytes] != '\r' &&
               name[name_bytes] != '\n') ++name_bytes;
        end = strchr(name + name_bytes, '>');
        if (end == NULL) return NULL;
        if (name_bytes == wanted_bytes && memcmp(name, wanted, wanted_bytes) == 0) {
            if (tag_end != NULL) *tag_end = end;
            return cursor;
        }
        cursor = end + 1;
    }
    return NULL;
}

static int xlsx_part_path_is_safe(const char *path)
{
    const char *segment;
    if (path == NULL || *path == 0 || *path == '/') return 0;
    segment = path;
    while (*segment) {
        const char *end = segment;
        unsigned bytes;
        while (*end && *end != '/') {
            if (*end == '\\' || *end == ':') return 0;
            ++end;
        }
        bytes = (unsigned)(end - segment);
        if (bytes == 0 || (bytes == 1 && segment[0] == '.') ||
            (bytes == 2 && segment[0] == '.' && segment[1] == '.')) return 0;
        if (*end == 0) return 1;
        segment = end + 1;
        if (*segment == 0) return 0;
    }
    return 0;
}

static int xlsx_resolve_sheet_path(const unsigned char *relationships_xml,
                                   const char *relationship_id,
                                   char *path, unsigned path_bytes)
{
    const char *sheet_end;
    const char *cursor = (const char *)relationships_xml;
    if (relationships_xml == NULL || relationship_id == NULL || *relationship_id == 0) return 0;
    while ((cursor = xml_find_start_tag_from(cursor, "Relationship", &sheet_end)) != NULL) {
        char id[128], type[256], target[LAB_XLSX_PATH_BYTES];
        char normalized[LAB_XLSX_PATH_BYTES];
        unsigned target_bytes, used = 0, i;
        int worksheet_relation;
        if (xml_attribute(cursor, "Id", id, sizeof(id)) &&
            xml_attribute(cursor, "Type", type, sizeof(type)) &&
            xml_attribute(cursor, "Target", target, sizeof(target)) &&
            strcmp(id, relationship_id) == 0) {
            worksheet_relation = strlen(type) >= 10 &&
                strcmp(type + strlen(type) - 10, "/worksheet") == 0;
            if (!worksheet_relation) return 0;
            target_bytes = (unsigned)strlen(target);
            if (target_bytes == 0 || target_bytes >= sizeof(normalized) ||
                strchr(target, '\\') != NULL || strchr(target, ':') != NULL ||
                strchr(target, '?') != NULL || strchr(target, '#') != NULL) return 0;
            /* Resolve a package URI to an archive member; never extract this path. */
            if (target[0] == '/') {
                if (target_bytes <= 1 || target[1] == '/' ||
                    target_bytes < 4 || memcmp(target + 1, "xl/", 3) != 0) return 0;
                for (i = 1; i < target_bytes; ++i) normalized[used++] = target[i];
            } else {
                if (target_bytes >= sizeof(normalized) - 3) return 0;
                normalized[used++] = 'x'; normalized[used++] = 'l'; normalized[used++] = '/';
                for (i = 0; i < target_bytes; ++i) normalized[used++] = target[i];
            }
            normalized[used] = 0;
            if (!xlsx_part_path_is_safe(normalized)) return 0;
            if (used + 1 > path_bytes) return 0;
            memcpy(path, normalized, used + 1);
            return 1;
        }
        cursor = sheet_end + 1;
    }
    return 0;
}

static int append_xml_text(char *out, unsigned capacity, unsigned *used,
                           const char *start, const char *end)
{
    if (end < start) return 0;
    return append_xml_utf8(out, capacity, used, (const unsigned char *)start, (unsigned)(end - start));
}

typedef struct {
    char **items;
    LabU32 count;
    LabU32 capacity;
} LabSharedStrings;

static void shared_strings_destroy(LabSharedStrings *strings)
{
    LabU32 i;
    for (i = 0; i < strings->count; ++i) free(strings->items[i]);
    free(strings->items);
    memset(strings, 0, sizeof(*strings));
}

static int parse_shared_strings(LabSharedStrings *strings, unsigned char *xml)
{
    char *cursor = (char *)xml;
    char *value = (char *)malloc(LAB_MAX_CELL_INPUT + 1);
    unsigned used = 0;
    int in_item = 0;
    int saw_sst = 0, closed_sst = 0;
    int result = LAB_XLSX_BAD_XML;
    if (value == NULL) return LAB_XLSX_NO_MEMORY;
    while (*cursor) {
        char *tag = strchr(cursor, '<');
        char *end;
        char name[24]; unsigned name_bytes = 0; int closing = 0;
        if (tag == NULL) break;
        cursor = tag + 1;
        if (cursor[0] == '!' && cursor[1] == '-' && cursor[2] == '-') {
            end = strstr(cursor + 3, "-->"); if (end == NULL) goto done; cursor = end + 3; continue;
        }
        if (*cursor == '?') { end = strchr(cursor, '>'); if (end == NULL) goto done; cursor = end + 1; continue; }
        if (*cursor == '/') { closing = 1; ++cursor; }
        while (*cursor && *cursor != '>' && *cursor != '/' && *cursor != ' ' && *cursor != '\t' && *cursor != '\r' && *cursor != '\n') {
            if (name_bytes + 1 >= sizeof(name)) goto done;
            name[name_bytes++] = *cursor++;
        }
        name[name_bytes] = 0;
        end = strchr(cursor, '>'); if (end == NULL) goto done;
        if (!closing && strcmp(name, "sst") == 0) saw_sst = 1;
        else if (closing && strcmp(name, "sst") == 0) closed_sst = 1;
        else if (!closing && strcmp(name, "si") == 0) {
            if (in_item) goto done;
            in_item = 1; used = 0; value[0] = 0;
        } else if (closing && strcmp(name, "si") == 0) {
            char **next;
            char *copy;
            if (!in_item || strings->count >= 65535UL) goto done;
            copy = (char *)malloc(used + 1);
            if (copy == NULL) { result = LAB_XLSX_NO_MEMORY; goto done; }
            memcpy(copy, value, used + 1);
            if (strings->count == strings->capacity) {
                LabU32 capacity = strings->capacity == 0 ? 16 : strings->capacity * 2;
                if (capacity > 65535UL) capacity = 65535UL;
                next = (char **)realloc(strings->items, capacity * sizeof(char *));
                if (next == NULL) { free(copy); result = LAB_XLSX_NO_MEMORY; goto done; }
                strings->items = next; strings->capacity = capacity;
            }
            strings->items[strings->count++] = copy;
            in_item = 0;
        } else if (in_item && !closing && strcmp(name, "t") == 0) {
            char *text_start, *text_end;
            if (end > tag && end[-1] == '/') { cursor = end + 1; continue; }
            text_start = end + 1; text_end = strstr(text_start, "</t>");
            if (text_end == NULL || !append_xml_text(value, LAB_MAX_CELL_INPUT + 1, &used, text_start, text_end)) goto done;
            cursor = text_end + 4; continue;
        }
        cursor = end + 1;
    }
    if (in_item || !saw_sst || !closed_sst) goto done;
    result = LAB_XLSX_OK;
done:
    free(value);
    return result;
}

static int decimal_u32(const char *text, LabU32 *value)
{
    LabU32 result = 0;
    const unsigned char *p = (const unsigned char *)text;
    if (text == NULL || value == NULL || *p == 0) return 0;
    while (*p) {
        unsigned digit;
        if (*p < '0' || *p > '9') return 0;
        digit = *p++ - '0';
        if (result > (0xffffffffUL - digit) / 10) return 0;
        result = result * 10 + digit;
    }
    *value = result;
    return 1;
}

static int load_worksheet(LabSheet *sheet, unsigned char *xml,
                          const LabSharedStrings *shared,
                          LabU32 *active_row, LabU16 *active_column)
{
    char *cursor = (char *)xml;
    unsigned char *tag;
    int in_cell = 0;
    int saw_worksheet = 0, closed_worksheet = 0;
    int saw_sheet_data = 0, closed_sheet_data = 0;
    char reference[24], type[24];
    char *value = NULL, *formula = NULL, *inline_text = NULL;
    unsigned value_used = 0, formula_used = 0, text_used = 0;
    unsigned capacity = LAB_MAX_CELL_INPUT + 1;
    int result = LAB_XLSX_BAD_XML;
    value = (char *)malloc(capacity); formula = (char *)malloc(capacity); inline_text = (char *)malloc(capacity);
    if (active_row == NULL || active_column == NULL) goto done;
    *active_row = 0;
    *active_column = 0;
    if (value == NULL || formula == NULL || inline_text == NULL) { result = LAB_XLSX_NO_MEMORY; goto done; }
    while ((tag = (unsigned char *)strchr(cursor, '<')) != NULL) {
        unsigned char *end;
        char name[24]; unsigned name_bytes = 0;
        int closing = 0;
        cursor = (char *)tag + 1;
        if (cursor[0] == '!' && cursor[1] == '-' && cursor[2] == '-') {
            end = (unsigned char *)strstr(cursor + 3, "-->"); if (end == NULL) goto done; cursor = (char *)end + 3; continue;
        }
        if (*cursor == '?') { end = (unsigned char *)strchr(cursor, '>'); if (end == NULL) goto done; cursor = (char *)end + 1; continue; }
        if (*cursor == '/') { closing = 1; ++cursor; }
        while (*cursor && *cursor != '>' && *cursor != '/' && *cursor != ' ' && *cursor != '\t' && *cursor != '\r' && *cursor != '\n') {
            if (name_bytes + 1 >= sizeof(name)) goto done;
            name[name_bytes++] = *cursor++;
        }
        name[name_bytes] = 0;
        end = (unsigned char *)strchr(cursor, '>'); if (end == NULL) goto done;
        if (!closing && strcmp(name, "worksheet") == 0) saw_worksheet = 1;
        else if (closing && strcmp(name, "worksheet") == 0) closed_worksheet = 1;
        else if (!closing && strcmp(name, "sheetData") == 0) saw_sheet_data = 1;
        else if (closing && strcmp(name, "sheetData") == 0) closed_sheet_data = 1;
        else if (!closing && strcmp(name, "selection") == 0) {
            char selected_cell[24];
            LabU32 selected_row;
            LabU16 selected_column;
            if (xml_attribute((const char *)tag, "activeCell", selected_cell,
                              sizeof(selected_cell)) &&
                lab_cell_reference_parse(selected_cell, &selected_row,
                                         &selected_column)) {
                *active_row = selected_row;
                *active_column = selected_column;
            }
        }
        if (!closing && (strcmp(name, "f") == 0 || strcmp(name, "v") == 0 || strcmp(name, "t") == 0)) {
            const char *close_tag;
            char *text_start;
            char *text_end;
            unsigned *used;
            char *target;
            if (end > tag && end[-1] == '/') { cursor = (char *)end + 1; continue; }
            close_tag = strcmp(name, "f") == 0 ? "</f>" : (strcmp(name, "v") == 0 ? "</v>" : "</t>");
            text_start = (char *)end + 1;
            text_end = strstr(text_start, close_tag);
            used = strcmp(name, "f") == 0 ? &formula_used : (strcmp(name, "v") == 0 ? &value_used : &text_used);
            target = strcmp(name, "f") == 0 ? formula : (strcmp(name, "v") == 0 ? value : inline_text);
            if (text_end == NULL || !append_xml_text(target, capacity, used, text_start, text_end)) goto done;
            cursor = text_end + strlen(close_tag);
            continue;
        }
        if (!closing && strcmp(name, "c") == 0) {
            char tag_copy[512]; unsigned n;
            if (end > tag && end[-1] == '/') { cursor = (char *)end + 1; continue; }
            n = (unsigned)(end - tag + 1);
            if (n >= sizeof(tag_copy)) goto done;
            memcpy(tag_copy, tag, n); tag_copy[n] = 0;
            if (!xml_attribute(tag_copy, "r", reference, sizeof(reference))) goto done;
            if (!xml_attribute(tag_copy, "t", type, sizeof(type))) type[0] = 0;
            value_used = formula_used = text_used = 0;
            value[0] = formula[0] = inline_text[0] = 0;
            in_cell = 1;
        } else if (closing && strcmp(name, "c") == 0 && in_cell) {
            LabU32 row; LabU16 column; int model_result;
            in_cell = 0;
            if (!lab_cell_reference_parse(reference, &row, &column)) goto done;
            if (formula_used != 0) {
                char *expression = (char *)malloc(formula_used + 2);
                if (expression == NULL) { result = LAB_XLSX_NO_MEMORY; goto done; }
                expression[0] = '='; memcpy(expression + 1, formula, formula_used + 1);
                model_result = lab_sheet_set_input(sheet, row, column, expression); free(expression);
            } else if (strcmp(type, "inlineStr") == 0) {
                model_result = lab_sheet_set_text(sheet, row, column, inline_text);
            } else if (strcmp(type, "str") == 0) {
                model_result = lab_sheet_set_text(sheet, row, column, value);
            } else if (strcmp(type, "s") == 0) {
                LabU32 index;
                if (!decimal_u32(value, &index) || index >= shared->count) { result = LAB_XLSX_BAD_XML; goto done; }
                model_result = lab_sheet_set_text(sheet, row, column, shared->items[index]);
            } else if (strcmp(type, "e") == 0 || strcmp(type, "b") == 0) {
                result = LAB_XLSX_UNSUPPORTED; goto done;
            } else if (value_used != 0) {
                model_result = lab_sheet_set_input(sheet, row, column, value);
            } else model_result = LAB_MODEL_OK;
            if (model_result != LAB_MODEL_OK) { result = model_result == LAB_MODEL_NO_MEMORY ? LAB_XLSX_NO_MEMORY : LAB_XLSX_BAD_XML; goto done; }
        }
        cursor = (char *)end + 1;
    }
    if (in_cell || !saw_worksheet || !closed_worksheet ||
        !saw_sheet_data || !closed_sheet_data) goto done;
    result = LAB_XLSX_OK;
done:
    if (value != NULL) free(value);
    if (formula != NULL) free(formula);
    if (inline_text != NULL) free(inline_text);
    return result;
}

int lab_xlsx_load(LabWorkbook **workbook, const char *path)
{
    unsigned char *workbook_xml = NULL;
    unsigned char *relationships_xml = NULL;
    unsigned char *shared_xml = NULL;
    LabU32 workbook_bytes = 0;
    LabU32 relationships_bytes = 0;
    LabU32 shared_bytes = 0;
    LabSharedStrings shared;
    LabWorkbook *candidate = NULL;
    const char *cursor;
    const char *tag_end;
    const char *tag;
    LabU32 active_tab = 0;
    char attribute[128];
    int result = LAB_XLSX_BAD_XML;
    memset(&shared, 0, sizeof(shared));
    if (workbook == NULL || *workbook == NULL || path == NULL ||
        strlen(path) >= LAB_XLSX_PATH_BYTES || !path_is_xlsx(path))
        return LAB_XLSX_BAD_ARGUMENT;
    result = zip_read_entry(path, "xl/workbook.xml", &workbook_xml, &workbook_bytes);
    if (result != LAB_XLSX_OK) return result;
    result = zip_read_entry(path, "xl/_rels/workbook.xml.rels",
                            &relationships_xml, &relationships_bytes);
    if (result != LAB_XLSX_OK) { free(workbook_xml); return result; }
    if (workbook_bytes == 0 || relationships_bytes == 0 ||
        memchr(workbook_xml, 0, workbook_bytes) != NULL ||
        memchr(relationships_xml, 0, relationships_bytes) != NULL) {
        result = LAB_XLSX_BAD_XML;
        goto done;
    }
    result = zip_read_entry(path, "xl/sharedStrings.xml", &shared_xml, &shared_bytes);
    if (result == LAB_XLSX_OK) {
        if (shared_bytes == 0 || memchr(shared_xml, 0, shared_bytes) != NULL) result = LAB_XLSX_BAD_XML;
        else result = parse_shared_strings(&shared, shared_xml);
        free(shared_xml);
        shared_xml = NULL;
        if (result != LAB_XLSX_OK) goto done;
    } else if (result != LAB_XLSX_ENTRY_NOT_FOUND) {
        goto done;
    }
    candidate = lab_workbook_create_empty();
    if (candidate == NULL) { result = LAB_XLSX_NO_MEMORY; goto done; }
    cursor = (const char *)workbook_xml;
    while ((tag = xml_find_start_tag_from(cursor, "sheet", &tag_end)) != NULL) {
        unsigned char *xml = NULL;
        LabU32 xml_bytes = 0;
        unsigned name_used = 0;
        char encoded_name[128];
        char name[LAB_WORKSHEET_NAME_BYTES + 1];
        char relationship_id[128];
        char worksheet_path[LAB_XLSX_PATH_BYTES];
        LabU32 worksheet_index;
        LabU32 active_row;
        LabU16 active_column;
        int model_result;
        if (!xml_attribute(tag, "name", encoded_name, sizeof(encoded_name)) ||
            !xml_attribute(tag, "r:id", relationship_id, sizeof(relationship_id)) ||
            !append_xml_utf8(name, sizeof(name), &name_used,
                             (const unsigned char *)encoded_name,
                             (unsigned)strlen(encoded_name)) ||
            !xlsx_resolve_sheet_path(relationships_xml, relationship_id,
                                     worksheet_path, sizeof(worksheet_path))) {
            result = LAB_XLSX_BAD_XML;
            goto done;
        }
        result = zip_read_entry(path, worksheet_path, &xml, &xml_bytes);
        if (result != LAB_XLSX_OK) goto done;
        if (xml_bytes == 0 || memchr(xml, 0, xml_bytes) != NULL) {
            free(xml); result = LAB_XLSX_BAD_PACKAGE; goto done;
        }
        model_result = lab_workbook_add_sheet(candidate, name, &worksheet_index);
        if (model_result != LAB_MODEL_OK) {
            free(xml);
            result = model_result == LAB_WORKBOOK_NO_MEMORY ? LAB_XLSX_NO_MEMORY : LAB_XLSX_BAD_XML;
            goto done;
        }
        result = load_worksheet(lab_workbook_active_sheet(candidate), xml, &shared,
                                &active_row, &active_column);
        free(xml);
        if (result != LAB_XLSX_OK) goto done;
        if (lab_workbook_set_sheet_selection(candidate, worksheet_index,
                                             active_row, active_column) != LAB_MODEL_OK) {
            result = LAB_XLSX_BAD_XML;
            goto done;
        }
        if (lab_sheet_recalculate(lab_workbook_active_sheet(candidate)) != LAB_MODEL_OK) {
            result = LAB_XLSX_BAD_PACKAGE;
            goto done;
        }
        cursor = tag_end + 1;
    }
    if (lab_workbook_sheet_count(candidate) == 0) { result = LAB_XLSX_BAD_XML; goto done; }
    if (xml_find_start_tag_from((const char *)workbook_xml, "workbookView", &tag_end) != NULL &&
        xml_attribute(xml_find_start_tag_from((const char *)workbook_xml, "workbookView", NULL),
                      "activeTab", attribute, sizeof(attribute))) {
        if (!decimal_u32(attribute, &active_tab) || active_tab >= lab_workbook_sheet_count(candidate)) {
            result = LAB_XLSX_BAD_XML;
            goto done;
        }
    }
    if (lab_workbook_set_active(candidate, active_tab) != LAB_MODEL_OK) {
        result = LAB_XLSX_BAD_XML;
        goto done;
    }
    result = LAB_XLSX_OK;
done:
    free(workbook_xml);
    free(relationships_xml);
    free(shared_xml);
    shared_strings_destroy(&shared);
    if (result == LAB_XLSX_OK) {
        lab_workbook_destroy(*workbook);
        *workbook = candidate;
    } else lab_workbook_destroy(candidate);
    return result;
}

int lab_xlsx_save(LabWorkbook *workbook, const char *path)
{
    char temporary[LAB_XLSX_PATH_BYTES];
    char backup[LAB_XLSX_PATH_BYTES];
    int had_original;
    int result;
    if (workbook == NULL || lab_workbook_sheet_count(workbook) == 0 ||
        path == NULL || strlen(path) >= LAB_XLSX_PATH_BYTES || !path_is_xlsx(path))
        return LAB_XLSX_BAD_ARGUMENT;
    if (!path_sidecar(path, ".TMP", temporary, sizeof(temporary)) ||
        !path_sidecar(path, ".BAK", backup, sizeof(backup))) return LAB_XLSX_BAD_ARGUMENT;
    remove(temporary);
    result = make_package(workbook, temporary);
    if (result != LAB_XLSX_OK) { remove(temporary); return result; }
    had_original = access(path, 0) == 0;
    if (had_original) {
        remove(backup);
        if (rename(path, backup) != 0) { remove(temporary); return LAB_XLSX_IO_ERROR; }
    }
    if (rename(temporary, path) != 0) {
        if (had_original) rename(backup, path);
        remove(temporary);
        return LAB_XLSX_IO_ERROR;
    }
    /* Keep the prior valid workbook as a recovery copy after replacement. */
    return LAB_XLSX_OK;
}
