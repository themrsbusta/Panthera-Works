#include "showroom_pptx.h"

#include <stdlib.h>
#include <string.h>

#include "../../shared/ooxml/zip32_reader.h"

#define SH_IMPORT_XML_LIMIT 1048576UL
#define SH_IMPORT_PATH_LIMIT 256
#define SH_IMPORT_REL_LIMIT 128
#define SH_IMPORT_ID_LIMIT 64
#define SH_IMPORT_TARGET_LIMIT 256

typedef struct {
    char id[SH_IMPORT_ID_LIMIT];
    char target[SH_IMPORT_TARGET_LIMIT];
    int slide;
} ShImportRelationship;

typedef struct {
    const unsigned char *bytes;
    unsigned long size;
    unsigned long position;
    char name[64];
    unsigned name_length;
    unsigned long tag_start;
    unsigned long content_start;
    int closing;
    int empty;
} ShXmlCursor;

static int name_char(unsigned char value)
{
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == ':' ||
           value == '_' || value == '-' || value == '.';
}

static int ascii_equal(const char *left, const char *right)
{
    while (*left && *right) {
        unsigned char a = (unsigned char)*left++;
        unsigned char b = (unsigned char)*right++;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
        if (a != b) return 0;
    }
    return *left == 0 && *right == 0;
}

static int local_name_equal(const char *qualified, const char *local)
{
    const char *name = strrchr(qualified, ':');
    return ascii_equal(name ? name + 1 : qualified, local);
}

static int next_tag(ShXmlCursor *cursor)
{
    unsigned long p = cursor->position;
    unsigned length;
    while (p < cursor->size && cursor->bytes[p] != '<') ++p;
    if (p >= cursor->size) { cursor->position = p; return 0; }
    cursor->tag_start = p++;
    if (p < cursor->size && cursor->bytes[p] == '?') {
        while (p + 1 < cursor->size &&
               !(cursor->bytes[p] == '?' && cursor->bytes[p + 1] == '>')) ++p;
        cursor->position = p + (p + 1 < cursor->size ? 2 : 0);
        return next_tag(cursor);
    }
    if (p < cursor->size && cursor->bytes[p] == '!') {
        while (p < cursor->size && cursor->bytes[p] != '>') ++p;
        cursor->position = p < cursor->size ? p + 1 : p;
        return next_tag(cursor);
    }
    cursor->closing = p < cursor->size && cursor->bytes[p] == '/';
    if (cursor->closing) ++p;
    length = 0;
    while (p < cursor->size && name_char(cursor->bytes[p])) {
        if (length + 1 >= sizeof(cursor->name)) return -1;
        cursor->name[length++] = (char)cursor->bytes[p++];
    }
    if (length == 0) return -1;
    cursor->name[length] = 0;
    cursor->name_length = length;
    cursor->empty = 0;
    while (p < cursor->size && cursor->bytes[p] != '>') {
        if (cursor->bytes[p] == '/' && p + 1 < cursor->size &&
            cursor->bytes[p + 1] == '>') cursor->empty = 1;
        ++p;
    }
    if (p >= cursor->size) return -1;
    cursor->content_start = p + 1;
    cursor->position = p + 1;
    return 1;
}

static int attribute_value(const unsigned char *xml, unsigned long start,
                           unsigned long end, const char *wanted,
                           char *output, unsigned capacity)
{
    unsigned long p = start;
    while (p < end) {
        unsigned long name_start;
        unsigned name_length;
        unsigned long value_start;
        unsigned value_length;
        char name[64];
        unsigned copy;
        while (p < end && (xml[p] == '<' || xml[p] == '>' || xml[p] == '/' ||
               xml[p] == '?' || xml[p] == ' ' || xml[p] == '\t' ||
               xml[p] == '\r' || xml[p] == '\n')) ++p;
        name_start = p;
        while (p < end && name_char(xml[p])) ++p;
        name_length = (unsigned)(p - name_start);
        if (name_length == 0) { ++p; continue; }
        if (name_length >= sizeof(name)) return 0;
        memcpy(name, xml + name_start, name_length);
        name[name_length] = 0;
        while (p < end && (xml[p] == ' ' || xml[p] == '\t' ||
               xml[p] == '\r' || xml[p] == '\n')) ++p;
        if (p >= end || xml[p] != '=') continue;
        ++p;
        while (p < end && (xml[p] == ' ' || xml[p] == '\t' ||
               xml[p] == '\r' || xml[p] == '\n')) ++p;
        if (p >= end || (xml[p] != '\'' && xml[p] != '"')) return 0;
        {
            unsigned char quote = xml[p++];
            value_start = p;
            while (p < end && xml[p] != quote) ++p;
            if (p >= end) return 0;
            value_length = (unsigned)(p - value_start);
            ++p;
        }
        if (local_name_equal(name, wanted)) {
            if (value_length >= capacity) return 0;
            copy = value_length;
            memcpy(output, xml + value_start, copy);
            output[copy] = 0;
            return 1;
        }
    }
    return 0;
}

static int attribute_value_with_prefix(const unsigned char *xml, unsigned long start,
                                       unsigned long end, const char *wanted,
                                       const char *prefix, char *output,
                                       unsigned capacity)
{
    unsigned long p = start;
    unsigned prefix_length = (unsigned)strlen(prefix);
    while (p < end) {
        unsigned long name_start, value_start;
        unsigned name_length, value_length;
        char name[64];
        while (p < end && (xml[p] == '<' || xml[p] == '>' || xml[p] == '/' ||
               xml[p] == '?' || xml[p] == ' ' || xml[p] == '\t' ||
               xml[p] == '\r' || xml[p] == '\n')) ++p;
        name_start = p;
        while (p < end && name_char(xml[p])) ++p;
        name_length = (unsigned)(p - name_start);
        if (name_length == 0) { ++p; continue; }
        if (name_length >= sizeof(name)) return 0;
        memcpy(name, xml + name_start, name_length);
        name[name_length] = 0;
        while (p < end && (xml[p] == ' ' || xml[p] == '\t' ||
               xml[p] == '\r' || xml[p] == '\n')) ++p;
        if (p >= end || xml[p] != '=') continue;
        ++p;
        while (p < end && (xml[p] == ' ' || xml[p] == '\t' ||
               xml[p] == '\r' || xml[p] == '\n')) ++p;
        if (p >= end || (xml[p] != '\'' && xml[p] != '"')) return 0;
        {
            unsigned char quote = xml[p++];
            value_start = p;
            while (p < end && xml[p] != quote) ++p;
            if (p >= end) return 0;
            value_length = (unsigned)(p - value_start);
            ++p;
        }
        if (local_name_equal(name, wanted) && value_length >= prefix_length &&
            memcmp(xml + value_start, prefix, prefix_length) == 0) {
            if (value_length >= capacity) return 0;
            memcpy(output, xml + value_start, value_length);
            output[value_length] = 0;
            return 1;
        }
    }
    return 0;
}

static int resolve_target(const char *source_part, const char *target,
                          char output[SH_IMPORT_PATH_LIMIT])
{
    char combined[SH_IMPORT_PATH_LIMIT * 2];
    unsigned starts[64];
    unsigned count = 0;
    unsigned combined_used = 0;
    unsigned output_used = 0;
    unsigned i = 0;
    unsigned n;
    if (target == NULL || target[0] == 0) return 0;
    if (target[0] != '/') {
        const char *slash = strrchr(source_part, '/');
        if (slash != NULL) {
            n = (unsigned)(slash - source_part + 1);
            if (n >= sizeof(combined)) return 0;
            memcpy(combined, source_part, n);
            combined_used = n;
        }
    } else {
        ++target;
    }
    n = (unsigned)strlen(target);
    if (combined_used + n >= sizeof(combined)) return 0;
    memcpy(combined + combined_used, target, n + 1);
    combined_used += n;
    while (i < combined_used) {
        unsigned start;
        unsigned length;
        while (i < combined_used && combined[i] == '/') ++i;
        if (i == combined_used) break;
        start = i;
        while (i < combined_used && combined[i] != '/') {
            unsigned char ch = (unsigned char)combined[i];
            if (ch < 0x20 || ch >= 0x7f || ch == ':' || ch == '%' || ch == '\\') return 0;
            ++i;
        }
        length = i - start;
        if (length == 1 && combined[start] == '.') continue;
        if (length == 2 && combined[start] == '.' && combined[start + 1] == '.') {
            if (count == 0) return 0;
            output_used = starts[--count];
            continue;
        }
        if (count >= sizeof(starts) / sizeof(starts[0])) return 0;
        starts[count] = output_used;
        if (count != 0) {
            if (output_used + 1 >= SH_IMPORT_PATH_LIMIT) return 0;
            output[output_used++] = '/';
        }
        ++count;
        if (output_used + length >= SH_IMPORT_PATH_LIMIT) return 0;
        memcpy(output + output_used, combined + start, length);
        output_used += length;
    }
    if (count == 0) return 0;
    output[output_used] = 0;
    return 1;
}

static int relationship_part_path(const char *source, char output[SH_IMPORT_PATH_LIMIT])
{
    const char *slash = strrchr(source, '/');
    const char *file = slash ? slash + 1 : source;
    unsigned directory = slash ? (unsigned)(slash - source + 1) : 0;
    unsigned file_length = (unsigned)strlen(file);
    if (directory + 6 + file_length + 6 >= SH_IMPORT_PATH_LIMIT) return 0;
    if (directory) memcpy(output, source, directory);
    memcpy(output + directory, "_rels/", 6);
    memcpy(output + directory + 6, file, file_length);
    memcpy(output + directory + 6 + file_length, ".rels", 6);
    return 1;
}

static int read_member(const char *package, const char *name, PwZip32Buffer *buffer)
{
    int result = pw_zip32_read_member(package, name, SH_IMPORT_XML_LIMIT, buffer);
    if (result == PW_ZIP32_OK) return SH_PPTX_OK;
    if (result == PW_ZIP32_NOT_FOUND) return SH_PPTX_OPEN_NOT_FOUND;
    if (result == PW_ZIP32_TOO_LARGE) return SH_PPTX_OPEN_LIMIT;
    if (result == PW_ZIP32_NO_MEMORY) return SH_PPTX_OPEN_LIMIT;
    return SH_PPTX_OPEN_INVALID;
}

static int load_relationships(const unsigned char *xml, unsigned long bytes,
                              ShImportRelationship *relations, unsigned *count)
{
    ShXmlCursor cursor;
    int next;
    memset(&cursor, 0, sizeof(cursor));
    cursor.bytes = xml; cursor.size = bytes;
    *count = 0;
    while ((next = next_tag(&cursor)) > 0) {
        unsigned long end = cursor.content_start;
        char id[SH_IMPORT_ID_LIMIT];
        char target[SH_IMPORT_TARGET_LIMIT];
        char type[SH_IMPORT_TARGET_LIMIT];
        while (end < bytes && xml[end] != '>') ++end;
        if (!local_name_equal(cursor.name, "Relationship") || cursor.closing) continue;
        if (*count >= SH_IMPORT_REL_LIMIT) return SH_PPTX_OPEN_LIMIT;
        if (!attribute_value(xml, cursor.tag_start, end, "Id", id, sizeof(id)) ||
            !attribute_value(xml, cursor.tag_start, end, "Target", target, sizeof(target)) ||
            !attribute_value(xml, cursor.tag_start, end, "Type", type, sizeof(type)))
            return SH_PPTX_OPEN_INVALID;
        strcpy(relations[*count].id, id);
        strcpy(relations[*count].target, target);
        relations[*count].slide = 0;
        {
            unsigned length = (unsigned)strlen(type);
            if (length >= 6 && strcmp(type + length - 6, "/slide") == 0)
                relations[*count].slide = 1;
        }
        ++*count;
    }
    return next < 0 ? SH_PPTX_OPEN_INVALID : SH_PPTX_OK;
}

static int find_relationship(ShImportRelationship *relations, unsigned count,
                             const char *id, int slide_only)
{
    unsigned i;
    for (i = 0; i < count; ++i)
        if (strcmp(relations[i].id, id) == 0 && (!slide_only || relations[i].slide))
            return (int)i;
    return -1;
}

static int extract_slide_order(const unsigned char *xml, unsigned long bytes,
                               char ids[SH_MAX_SLIDES][SH_IMPORT_ID_LIMIT],
                               unsigned *count)
{
    ShXmlCursor cursor;
    int next;
    memset(&cursor, 0, sizeof(cursor));
    cursor.bytes = xml; cursor.size = bytes;
    *count = 0;
    while ((next = next_tag(&cursor)) > 0) {
        unsigned long end = cursor.content_start;
        char id[SH_IMPORT_ID_LIMIT];
        if (!local_name_equal(cursor.name, "sldId") || cursor.closing) continue;
        if (*count >= SH_MAX_SLIDES) return SH_PPTX_OPEN_LIMIT;
        while (end < bytes && xml[end] != '>') ++end;
        if (!attribute_value_with_prefix(xml, cursor.tag_start, end, "id", "rId",
                                         id, sizeof(id)))
            return SH_PPTX_OPEN_INVALID;
        strcpy(ids[*count], id);
        ++*count;
    }
    if (next < 0) return SH_PPTX_OPEN_INVALID;
    return *count ? SH_PPTX_OK : SH_PPTX_OPEN_INVALID;
}

static int utf8_next(const unsigned char *source, unsigned long length,
                     unsigned long *position, unsigned long *codepoint)
{
    unsigned char a;
    if (*position >= length) return 0;
    a = source[(*position)++];
    if (a < 0x80) { *codepoint = a; return 1; }
    if ((a & 0xe0) == 0xc0) {
        unsigned char b;
        if (*position >= length) return 0;
        b = source[(*position)++];
        if ((b & 0xc0) != 0x80) return 0;
        *codepoint = ((unsigned long)(a & 0x1f) << 6) | (b & 0x3f);
        return *codepoint >= 0x80;
    }
    if ((a & 0xf0) == 0xe0) {
        unsigned char b, c;
        if (*position + 1 >= length) return 0;
        b = source[(*position)++]; c = source[(*position)++];
        if ((b & 0xc0) != 0x80 || (c & 0xc0) != 0x80) return 0;
        *codepoint = ((unsigned long)(a & 0x0f) << 12) |
                     ((unsigned long)(b & 0x3f) << 6) | (c & 0x3f);
        return *codepoint >= 0x800 && !(*codepoint >= 0xd800 && *codepoint <= 0xdfff);
    }
    if ((a & 0xf8) == 0xf0) {
        unsigned char b, c, d;
        if (*position + 2 >= length) return 0;
        b = source[(*position)++]; c = source[(*position)++]; d = source[(*position)++];
        if ((b & 0xc0) != 0x80 || (c & 0xc0) != 0x80 || (d & 0xc0) != 0x80) return 0;
        *codepoint = ((unsigned long)(a & 7) << 18) | ((unsigned long)(b & 0x3f) << 12) |
                     ((unsigned long)(c & 0x3f) << 6) | (d & 0x3f);
        return *codepoint >= 0x10000 && *codepoint <= 0x10ffff;
    }
    return 0;
}

static int entity_codepoint(const unsigned char *xml, unsigned long length,
                            unsigned long *position, unsigned long *codepoint)
{
    unsigned long p = *position + 1;
    unsigned long value = 0;
    unsigned base = 10;
    if (p >= length) return 0;
    if (xml[p] == '#') {
        ++p;
        if (p < length && (xml[p] == 'x' || xml[p] == 'X')) { base = 16; ++p; }
        if (p >= length) return 0;
        while (p < length && xml[p] != ';') {
            unsigned digit;
            if (xml[p] >= '0' && xml[p] <= '9') digit = xml[p] - '0';
            else if (base == 16 && xml[p] >= 'a' && xml[p] <= 'f') digit = xml[p] - 'a' + 10;
            else if (base == 16 && xml[p] >= 'A' && xml[p] <= 'F') digit = xml[p] - 'A' + 10;
            else return 0;
            if (digit >= base || value > (0x10ffffUL - digit) / base) return 0;
            value = value * base + digit;
            ++p;
        }
        if (p >= length || p == *position + 2) return 0;
        ++p;
    } else {
        unsigned long start = p;
        while (p < length && xml[p] != ';') ++p;
        if (p >= length) return 0;
        if (p - start == 3 && memcmp(xml + start, "amp", 3) == 0) value = '&';
        else if (p - start == 2 && memcmp(xml + start, "lt", 2) == 0) value = '<';
        else if (p - start == 2 && memcmp(xml + start, "gt", 2) == 0) value = '>';
        else if (p - start == 4 && memcmp(xml + start, "quot", 4) == 0) value = '"';
        else if (p - start == 4 && memcmp(xml + start, "apos", 4) == 0) value = '\'';
        else return 0;
        ++p;
    }
    if (value == 0 || value > 0x10ffffUL || (value >= 0xd800 && value <= 0xdfff)) return 0;
    *position = p;
    *codepoint = value;
    return 1;
}

static unsigned char cp437_byte(unsigned long codepoint)
{
    static const unsigned short upper[128] = {
        0x00c7,0x00fc,0x00e9,0x00e2,0x00e4,0x00e0,0x00e5,0x00e7,
        0x00ea,0x00eb,0x00e8,0x00ef,0x00ee,0x00ec,0x00c4,0x00c5,
        0x00c9,0x00e6,0x00c6,0x00f4,0x00f6,0x00f2,0x00fb,0x00f9,
        0x00ff,0x00d6,0x00dc,0x00a2,0x00a3,0x00a5,0x20a7,0x0192,
        0x00e1,0x00ed,0x00f3,0x00fa,0x00f1,0x00d1,0x00aa,0x00ba,
        0x00bf,0x2310,0x00ac,0x00bd,0x00bc,0x00a1,0x00ab,0x00bb,
        0x2591,0x2592,0x2593,0x2502,0x2524,0x00c1,0x00c2,0x00c0,
        0x00a9,0x2563,0x2551,0x2557,0x255d,0x00a2,0x00a5,0x2510,
        0x2514,0x2534,0x252c,0x251c,0x2500,0x253c,0x00e3,0x00c3,
        0x255a,0x2554,0x2569,0x2566,0x2560,0x2550,0x256c,0x00a4,
        0x00f0,0x00d0,0x00ca,0x00cb,0x00c8,0x0131,0x00cd,0x00ce,
        0x00cf,0x2518,0x250c,0x2588,0x2584,0x00a6,0x00cc,0x2580,
        0x00d3,0x00df,0x00d4,0x00d2,0x00f5,0x00d5,0x00b5,0x00fe,
        0x00de,0x00da,0x00db,0x00d9,0x00fd,0x00dd,0x00af,0x00b4,
        0x00ad,0x00b1,0x2017,0x00be,0x00b6,0x00a7,0x00f7,0x00b8,
        0x00b0,0x00a8,0x00b7,0x00b9,0x00b3,0x00b2,0x25a0,0x00a0
    };
    unsigned i;
    if (codepoint >= 0x20 && codepoint <= 0x7e) return (unsigned char)codepoint;
    for (i = 0; i < 128; ++i) if (upper[i] == codepoint) return (unsigned char)(i + 0x80);
    if (codepoint == '\t') return ' ';
    return '?';
}

static int append_codepoint(char *output, unsigned capacity, unsigned *used,
                            unsigned long codepoint)
{
    if (*used + 1 >= capacity) return 0;
    output[(*used)++] = (char)cp437_byte(codepoint);
    output[*used] = 0;
    return 1;
}

static int append_xml_text(const unsigned char *xml, unsigned long length,
                           char *output, unsigned capacity, unsigned *used)
{
    unsigned long p = 0;
    while (p < length) {
        unsigned long codepoint;
        if (xml[p] == '&') {
            if (!entity_codepoint(xml, length, &p, &codepoint)) return SH_PPTX_OPEN_INVALID;
        } else if (!utf8_next(xml, length, &p, &codepoint)) return SH_PPTX_OPEN_INVALID;
        if (codepoint == '\r' || codepoint == '\n') codepoint = ' ';
        if (codepoint < 0x20 && codepoint != '\t') return SH_PPTX_OPEN_INVALID;
        if (!append_codepoint(output, capacity, used, codepoint)) return SH_PPTX_OPEN_LIMIT;
    }
    return SH_PPTX_OK;
}

static int append_newline(char *output, unsigned capacity, unsigned *used)
{
    if (*used && output[*used - 1] != '\n') {
        if (*used + 1 >= capacity) return 0;
        output[(*used)++] = '\n';
        output[*used] = 0;
    }
    return 1;
}

static int extract_shape_text(const unsigned char *xml, unsigned long start,
                              unsigned long end,
                              char *output, unsigned capacity)
{
    ShXmlCursor cursor;
    int next;
    int in_paragraph = 0;
    int paragraph_has_text = 0;
    unsigned used = 0;
    output[0] = 0;
    memset(&cursor, 0, sizeof(cursor));
    cursor.bytes = xml; cursor.size = end;
    cursor.position = start;
    in_paragraph = 0;
    paragraph_has_text = 0;
    used = 0;
    output[0] = 0;
    while (cursor.position < end && (next = next_tag(&cursor)) > 0) {
        if (local_name_equal(cursor.name, "p")) {
            if (!cursor.closing) {
                if (paragraph_has_text && !append_newline(output, capacity, &used))
                    return SH_PPTX_OPEN_LIMIT;
                in_paragraph = 1;
                paragraph_has_text = 0;
            } else in_paragraph = 0;
        } else if (local_name_equal(cursor.name, "br") && !cursor.closing) {
            if (!append_newline(output, capacity, &used)) return SH_PPTX_OPEN_LIMIT;
            paragraph_has_text = 0;
        } else if (in_paragraph && local_name_equal(cursor.name, "t") && !cursor.closing) {
            unsigned long text_end = cursor.position;
            ShXmlCursor inner;
            int found = 0;
            while (text_end < end && xml[text_end] != '<') ++text_end;
            if (text_end >= end || text_end + 4 > end || memcmp(xml + text_end, "</", 2) != 0)
                return SH_PPTX_OPEN_INVALID;
            memset(&inner, 0, sizeof(inner));
            inner.bytes = xml; inner.size = end; inner.position = text_end;
            if (next_tag(&inner) <= 0 || !inner.closing || !local_name_equal(inner.name, "t"))
                return SH_PPTX_OPEN_INVALID;
            found = 1;
            if (found) {
                int result = append_xml_text(xml + cursor.position,
                                            text_end - cursor.position,
                                            output, capacity, &used);
                if (result != SH_PPTX_OK) return result;
                paragraph_has_text = 1;
                cursor.position = inner.position;
            }
        }
    }
    return next < 0 ? SH_PPTX_OPEN_INVALID : SH_PPTX_OK;
}

static int extract_slide(const unsigned char *xml, unsigned long bytes,
                         ShSlide *slide)
{
    ShXmlCursor cursor;
    int next;
    int in_shape = 0;
    unsigned long shape_start = 0;
    slide->title[0] = 0;
    slide->body[0] = 0;
    memset(&cursor, 0, sizeof(cursor));
    cursor.bytes = xml; cursor.size = bytes;
    while ((next = next_tag(&cursor)) > 0) {
        if (local_name_equal(cursor.name, "sp")) {
            if (!cursor.closing && !cursor.empty) {
                in_shape = 1;
                shape_start = cursor.position;
            } else if (cursor.closing && in_shape) {
                unsigned long shape_end = cursor.tag_start;
                char text[SH_BODY_BYTES + 1];
                int result = extract_shape_text(xml, shape_start, shape_end,
                                                text, sizeof(text));
                unsigned length;
                if (result != SH_PPTX_OK) return result;
                length = (unsigned)strlen(text);
                while (length && (text[length - 1] == '\n' || text[length - 1] == ' '))
                    text[--length] = 0;
                if (length) {
                    if (slide->title[0] == 0) {
                        if (length > SH_TITLE_BYTES) return SH_PPTX_OPEN_LIMIT;
                        strcpy(slide->title, text);
                    } else {
                        unsigned used = (unsigned)strlen(slide->body);
                        unsigned needed = length + (used ? 1 : 0);
                        if (needed > SH_BODY_BYTES - used) return SH_PPTX_OPEN_LIMIT;
                        if (used) slide->body[used++] = '\n';
                        memcpy(slide->body + used, text, length + 1);
                    }
                }
                in_shape = 0;
            }
        }
    }
    if (next < 0) return SH_PPTX_OPEN_INVALID;
    if (slide->title[0] == 0 && slide->body[0] == 0) return SH_PPTX_OPEN_INVALID;
    return SH_PPTX_OK;
}

int sh_pptx_load(const char *path, ShModel *model)
{
    ShModel *parsed;
    PwZip32Buffer root_rels, presentation, presentation_rels, slide_xml;
    ShImportRelationship *relations;
    char main_part[SH_IMPORT_PATH_LIMIT];
    char rels_part[SH_IMPORT_PATH_LIMIT];
    char (*slide_ids)[SH_IMPORT_ID_LIMIT];
    unsigned relation_count, slide_count, i;
    int result;
    if (path == NULL || model == NULL) return SH_PPTX_OPEN_ERROR;
    parsed = (ShModel *)calloc(1, sizeof(ShModel));
    relations = (ShImportRelationship *)calloc(SH_IMPORT_REL_LIMIT, sizeof(ShImportRelationship));
    slide_ids = (char (*)[SH_IMPORT_ID_LIMIT])calloc(SH_MAX_SLIDES, SH_IMPORT_ID_LIMIT);
    if (parsed == NULL || relations == NULL || slide_ids == NULL) {
        free(parsed); free(relations); free(slide_ids);
        return SH_PPTX_OPEN_LIMIT;
    }
    memset(&root_rels, 0, sizeof(root_rels));
    memset(&presentation, 0, sizeof(presentation));
    memset(&presentation_rels, 0, sizeof(presentation_rels));
    memset(&slide_xml, 0, sizeof(slide_xml));
    result = read_member(path, "_rels/.rels", &root_rels);
    if (result != SH_PPTX_OK) goto done;
    result = load_relationships(root_rels.data, root_rels.size, relations, &relation_count);
    if (result != SH_PPTX_OK) goto done;
    {
        ShXmlCursor cursor;
        int next;
        int found = 0;
        memset(&cursor, 0, sizeof(cursor)); cursor.bytes = root_rels.data; cursor.size = root_rels.size;
        while ((next = next_tag(&cursor)) > 0) {
            unsigned long end = cursor.content_start;
            char type[SH_IMPORT_TARGET_LIMIT], target[SH_IMPORT_TARGET_LIMIT];
            while (end < root_rels.size && root_rels.data[end] != '>') ++end;
            if (cursor.closing || !local_name_equal(cursor.name, "Relationship")) continue;
            if (attribute_value(root_rels.data, cursor.tag_start, end, "Type", type, sizeof(type)) &&
                attribute_value(root_rels.data, cursor.tag_start, end, "Target", target, sizeof(target)) &&
                strstr(type, "/officeDocument") != NULL) {
                if (!resolve_target("", target, main_part)) { result = SH_PPTX_OPEN_INVALID; goto done; }
                found = 1; break;
            }
        }
        if (next < 0 || !found) { result = SH_PPTX_OPEN_INVALID; goto done; }
    }
    result = read_member(path, main_part, &presentation);
    if (result != SH_PPTX_OK) goto done;
    if (!relationship_part_path(main_part, rels_part)) { result = SH_PPTX_OPEN_INVALID; goto done; }
    result = read_member(path, rels_part, &presentation_rels);
    if (result != SH_PPTX_OK) goto done;
    result = load_relationships(presentation_rels.data, presentation_rels.size,
                                relations, &relation_count);
    if (result != SH_PPTX_OK) goto done;
    result = extract_slide_order(presentation.data, presentation.size, slide_ids, &slide_count);
    if (result != SH_PPTX_OK) goto done;
    sh_model_init(parsed);
    parsed->count = 0;
    parsed->current = 0;
    for (i = 0; i < slide_count; ++i) {
        int relation = find_relationship(relations, relation_count, slide_ids[i], 1);
        char slide_part[SH_IMPORT_PATH_LIMIT];
        if (relation < 0 || !resolve_target(main_part, relations[relation].target, slide_part)) {
            result = SH_PPTX_OPEN_INVALID; goto done;
        }
        result = read_member(path, slide_part, &slide_xml);
        if (result != SH_PPTX_OK) goto done;
        result = extract_slide(slide_xml.data, slide_xml.size, &parsed->slides[i]);
        pw_zip32_buffer_dispose(&slide_xml);
        memset(&slide_xml, 0, sizeof(slide_xml));
        if (result != SH_PPTX_OK) goto done;
        ++parsed->count;
    }
    *model = *parsed;
    result = SH_PPTX_OK;
done:
    pw_zip32_buffer_dispose(&root_rels);
    pw_zip32_buffer_dispose(&presentation);
    pw_zip32_buffer_dispose(&presentation_rels);
    pw_zip32_buffer_dispose(&slide_xml);
    free(relations);
    free(parsed);
    free(slide_ids);
    return result;
}
