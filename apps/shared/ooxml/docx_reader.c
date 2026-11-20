#include "docx_reader.h"
#include "docx_styles.h"

#include "xml_buffer.h"
#include "zip32.h"

#include <string.h>

#define DOCX_PART_PATH_BYTES 260
#define DOCX_NUMBERING_ID_COUNT 64

typedef struct {
    char ids[DOCX_NUMBERING_ID_COUNT][16];
    pw_zip_u8 styles[DOCX_NUMBERING_ID_COUNT];
    unsigned count;
} PwDocxNumbering;

static int is_space(pw_zip_u8 c);
static int is_name_start(pw_zip_u8 c);
static int is_name_char(pw_zip_u8 c);
static int local_name_equals(const pw_zip_u8 *name, pw_zip_u32 bytes, const char *expected);
static int tag_attribute_value(const pw_zip_u8 *xml, pw_zip_u32 attributes_start,
                               pw_zip_u32 tag_end, const char *wanted,
                               char *value, unsigned value_capacity, int *found);
static int relationship_target(const pw_zip_u8 *xml, pw_zip_u32 xml_bytes,
                               const char *relationship_suffix, const char *base_part,
                               char *target, unsigned target_capacity, int required);
static int document_relationships_path(const char *document_part,
                                       char *path, unsigned path_capacity);

static pw_zip_u8 numbering_style_for_id(const PwDocxNumbering *numbering, const char *id)
{
    unsigned index;
    for (index = 0; index < numbering->count; ++index)
        if (strcmp(numbering->ids[index], id) == 0) return numbering->styles[index];
    return 0;
}

static int parse_list_numbering(const pw_zip_u8 *xml, pw_zip_u32 bytes,
                                PwDocxNumbering *numbering)
{
    char abstract_ids[DOCX_NUMBERING_ID_COUNT][16];
    pw_zip_u8 abstract_styles[DOCX_NUMBERING_ID_COUNT];
    char num_ids[DOCX_NUMBERING_ID_COUNT][16];
    pw_zip_u8 num_styles[DOCX_NUMBERING_ID_COUNT];
    unsigned abstract_count = 0;
    unsigned num_count = 0;
    unsigned cursor = 0;
    char current_abstract[16];
    char current_num[16];
    char current_num_abstract[16];
    int in_abstract = 0;
    pw_zip_u8 abstract_style = 0;
    int in_num = 0;
    numbering->count = 0;
    current_abstract[0] = current_num[0] = current_num_abstract[0] = '\0';
    if (bytes >= 3 && xml[0] == 0xef && xml[1] == 0xbb && xml[2] == 0xbf) cursor = 3;
    while (cursor < bytes) {
        int closing = 0;
        unsigned name_start;
        unsigned name_bytes;
        unsigned name_end;
        unsigned tag_end;
        if (xml[cursor] != '<') { ++cursor; continue; }
        if (cursor + 4 <= bytes && memcmp(xml + cursor, "<!--", 4) == 0) {
            cursor += 4;
            while (cursor + 3 <= bytes && memcmp(xml + cursor, "-->", 3) != 0) ++cursor;
            if (cursor + 3 > bytes) return 0;
            cursor += 3;
            continue;
        }
        if (cursor + 2 <= bytes && xml[cursor + 1] == '?') {
            cursor += 2;
            while (cursor + 2 <= bytes && !(xml[cursor] == '?' && xml[cursor + 1] == '>')) ++cursor;
            if (cursor + 2 > bytes) return 0;
            cursor += 2;
            continue;
        }
        if (cursor + 2 <= bytes && xml[cursor + 1] == '!') return 0;
        if (cursor + 1 < bytes && xml[cursor + 1] == '/') { closing = 1; ++cursor; }
        ++cursor;
        name_start = cursor;
        if (cursor >= bytes || !is_name_start(xml[cursor])) return 0;
        while (cursor < bytes && is_name_char(xml[cursor])) ++cursor;
        name_end = cursor;
        name_bytes = name_end - name_start;
        while (cursor < bytes && xml[cursor] != '>') {
            if (xml[cursor] == '\'' || xml[cursor] == '"') {
                pw_zip_u8 quote = xml[cursor++];
                while (cursor < bytes && xml[cursor] != quote) ++cursor;
                if (cursor >= bytes) return 0;
                ++cursor;
            } else ++cursor;
        }
        if (cursor >= bytes) return 0;
        tag_end = cursor;
        ++cursor;

        if (closing && local_name_equals(xml + name_start, name_bytes, "abstractNum")) {
            if (in_abstract && abstract_style != 0 && abstract_count < DOCX_NUMBERING_ID_COUNT) {
                strcpy(abstract_ids[abstract_count], current_abstract);
                abstract_styles[abstract_count++] = abstract_style;
            }
            in_abstract = 0;
        } else if (closing && local_name_equals(xml + name_start, name_bytes, "num")) {
            unsigned index;
            if (in_num && current_num[0] != '\0' && current_num_abstract[0] != '\0') {
                for (index = 0; index < abstract_count; ++index) {
                    if (strcmp(abstract_ids[index], current_num_abstract) == 0) {
                        if (num_count < DOCX_NUMBERING_ID_COUNT) {
                            strcpy(num_ids[num_count], current_num);
                            num_styles[num_count] = abstract_styles[index];
                            ++num_count;
                        }
                        break;
                    }
                }
            }
            in_num = 0;
        } else if (!closing && local_name_equals(xml + name_start, name_bytes, "abstractNum")) {
            int found;
            in_abstract = 1;
            abstract_style = 0;
            current_abstract[0] = '\0';
            if (!tag_attribute_value(xml, name_end, tag_end, "abstractNumId",
                    current_abstract, sizeof(current_abstract), &found)) return 0;
            if (!found) in_abstract = 0;
        } else if (!closing && in_abstract &&
                   local_name_equals(xml + name_start, name_bytes, "numFmt")) {
            char value[16];
            int found;
            if (!tag_attribute_value(xml, name_end, tag_end, "val", value, sizeof(value), &found)) return 0;
            if (found && (strcmp(value, "decimal") == 0 ||
                          strcmp(value, "decimalZero") == 0))
                abstract_style = 32;
            else if (found && strcmp(value, "bullet") == 0)
                abstract_style = 64;
        } else if (!closing && local_name_equals(xml + name_start, name_bytes, "num")) {
            int found;
            in_num = 1;
            current_num[0] = current_num_abstract[0] = '\0';
            if (!tag_attribute_value(xml, name_end, tag_end, "numId",
                    current_num, sizeof(current_num), &found)) return 0;
            if (!found) in_num = 0;
        } else if (!closing && in_num &&
                   local_name_equals(xml + name_start, name_bytes, "abstractNumId")) {
            int found;
            if (!tag_attribute_value(xml, name_end, tag_end, "val",
                    current_num_abstract, sizeof(current_num_abstract), &found)) return 0;
            if (!found) current_num_abstract[0] = '\0';
        }
    }
    if (in_abstract || in_num) return 0;
    {
        unsigned index;
        for (index = 0; index < num_count; ++index) {
        if (numbering_style_for_id(numbering, num_ids[index]) == 0 &&
            numbering->count < DOCX_NUMBERING_ID_COUNT) {
            strcpy(numbering->ids[numbering->count], num_ids[index]);
            numbering->styles[numbering->count++] = num_styles[index];
        }
        }
    }
    return 1;
}

static int read_list_numbering(const char *package_path,
                               const char *document_part,
                               pw_zip_u32 max_xml_bytes,
                               PwDocxNumbering *numbering)
{
    PwZip32Buffer relationships;
    PwZip32Buffer numbering_xml;
    char relationships_part[DOCX_PART_PATH_BYTES];
    char numbering_part[DOCX_PART_PATH_BYTES];
    int result;
    int target_result;
    relationships.data = NULL;
    relationships.size = 0;
    numbering_xml.data = NULL;
    numbering_xml.size = 0;
    numbering_part[0] = '\0';
    numbering->count = 0;
    if (!document_relationships_path(document_part, relationships_part,
                                     sizeof(relationships_part))) return PW_DOCX_READ_PACKAGE_ERROR;
    result = pw_zip32_read_member(package_path, relationships_part,
                                  max_xml_bytes, &relationships);
    if (result == PW_ZIP32_NOT_FOUND) return PW_DOCX_READ_OK;
    if (result == PW_ZIP32_TOO_LARGE) return PW_DOCX_READ_TOO_LARGE;
    if (result != PW_ZIP32_OK)
        return result == PW_ZIP32_NO_MEMORY ? PW_DOCX_READ_NO_MEMORY : PW_DOCX_READ_PACKAGE_ERROR;
    target_result = relationship_target(relationships.data, relationships.size,
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering",
        document_part, numbering_part, sizeof(numbering_part), 0);
    pw_zip32_buffer_dispose(&relationships);
    if (target_result < 0) return PW_DOCX_READ_PACKAGE_ERROR;
    if (numbering_part[0] == '\0') return PW_DOCX_READ_OK;
    result = pw_zip32_read_member(package_path, numbering_part,
                                  max_xml_bytes, &numbering_xml);
    if (result == PW_ZIP32_TOO_LARGE) return PW_DOCX_READ_TOO_LARGE;
    if (result != PW_ZIP32_OK)
        return result == PW_ZIP32_NO_MEMORY ? PW_DOCX_READ_NO_MEMORY : PW_DOCX_READ_PACKAGE_ERROR;
    if (!parse_list_numbering(numbering_xml.data, numbering_xml.size, numbering)) {
        pw_zip32_buffer_dispose(&numbering_xml);
        return PW_DOCX_READ_XML_ERROR;
    }
    pw_zip32_buffer_dispose(&numbering_xml);
    return PW_DOCX_READ_OK;
}

static int is_space(pw_zip_u8 c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int is_name_start(pw_zip_u8 c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == ':';
}

static int is_name_char(pw_zip_u8 c)
{
    return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

static int xml_char_allowed(pw_zip_u32 value)
{
    return value == 0x09UL || value == 0x0aUL || value == 0x0dUL ||
           (value >= 0x20UL && value <= 0xd7ffUL) ||
           (value >= 0xe000UL && value <= 0xfffdUL) ||
           (value >= 0x10000UL && value <= 0x10ffffUL);
}

static int utf8_decode(const pw_zip_u8 *bytes, pw_zip_u32 size,
                       pw_zip_u32 *cursor, pw_zip_u32 *value)
{
    pw_zip_u32 at = *cursor;
    pw_zip_u8 first;
    pw_zip_u32 count;
    pw_zip_u32 minimum;
    pw_zip_u32 cp;
    pw_zip_u32 index;
    if (at >= size) return 0;
    first = bytes[at];
    if (first < 0x80) { count = 1; cp = first; minimum = 0; }
    else if ((first & 0xe0) == 0xc0) { count = 2; cp = first & 0x1f; minimum = 0x80; }
    else if ((first & 0xf0) == 0xe0) { count = 3; cp = first & 0x0f; minimum = 0x800; }
    else if ((first & 0xf8) == 0xf0) { count = 4; cp = first & 0x07; minimum = 0x10000; }
    else return 0;
    if (count > size - at) return 0;
    for (index = 1; index < count; ++index) {
        pw_zip_u8 next = bytes[at + index];
        if ((next & 0xc0) != 0x80) return 0;
        cp = (cp << 6) | (next & 0x3f);
    }
    if ((count > 1 && cp < minimum) || cp > 0x10ffffUL ||
        (cp >= 0xd800UL && cp <= 0xdfffUL) || !xml_char_allowed(cp)) return 0;
    *cursor = at + count;
    *value = cp;
    return 1;
}

static int append_bounded(PwXmlBuffer *text, const void *bytes, pw_zip_u32 count,
                          pw_zip_u32 maximum)
{
    if (count > maximum || text->length > maximum - count) {
        text->too_large = 1;
        return 0;
    }
    return pw_xml_append(text, bytes, count);
}

static int append_style_repeat(PwXmlBuffer *styles, pw_zip_u8 style,
                               pw_zip_u32 count, pw_zip_u32 maximum)
{
    pw_zip_u8 repeated[128];
    memset(repeated, style, sizeof(repeated));
    while (count != 0) {
        pw_zip_u32 chunk = count > sizeof(repeated) ? sizeof(repeated) : count;
        if (!append_bounded(styles, repeated, chunk, maximum)) return 0;
        count -= chunk;
    }
    return 1;
}

static int append_styled(PwXmlBuffer *text, PwXmlBuffer *styles,
                         const void *bytes, pw_zip_u32 count,
                         pw_zip_u32 maximum, pw_zip_u8 style)
{
    return append_bounded(text, bytes, count, maximum) &&
           append_style_repeat(styles, style, count, maximum);
}

static int append_codepoint(PwXmlBuffer *text, pw_zip_u32 cp, pw_zip_u32 maximum)
{
    pw_zip_u8 encoded[4];
    pw_zip_u32 count;
    if (!xml_char_allowed(cp)) return 0;
    if (cp < 0x80UL) { encoded[0] = (pw_zip_u8)cp; count = 1; }
    else if (cp < 0x800UL) {
        encoded[0] = (pw_zip_u8)(0xc0 | (cp >> 6));
        encoded[1] = (pw_zip_u8)(0x80 | (cp & 0x3f));
        count = 2;
    } else if (cp < 0x10000UL) {
        encoded[0] = (pw_zip_u8)(0xe0 | (cp >> 12));
        encoded[1] = (pw_zip_u8)(0x80 | ((cp >> 6) & 0x3f));
        encoded[2] = (pw_zip_u8)(0x80 | (cp & 0x3f));
        count = 3;
    } else {
        encoded[0] = (pw_zip_u8)(0xf0 | (cp >> 18));
        encoded[1] = (pw_zip_u8)(0x80 | ((cp >> 12) & 0x3f));
        encoded[2] = (pw_zip_u8)(0x80 | ((cp >> 6) & 0x3f));
        encoded[3] = (pw_zip_u8)(0x80 | (cp & 0x3f));
        count = 4;
    }
    return append_bounded(text, encoded, count, maximum);
}

static int append_xml_text(PwXmlBuffer *text, const pw_zip_u8 *xml,
                           pw_zip_u32 start, pw_zip_u32 end,
                           pw_zip_u32 maximum)
{
    pw_zip_u32 cursor = start;
    while (cursor < end) {
        if (xml[cursor] == '&') {
            pw_zip_u32 entity_start = cursor + 1;
            pw_zip_u32 entity_end = entity_start;
            pw_zip_u32 cp = 0;
            int hex = 0;
            while (entity_end < end && xml[entity_end] != ';' && entity_end - entity_start < 16) ++entity_end;
            if (entity_end >= end || xml[entity_end] != ';' || entity_end == entity_start) return 0;
            if (xml[entity_start] == '#') {
                ++entity_start;
                if (entity_start < entity_end && (xml[entity_start] == 'x' || xml[entity_start] == 'X')) {
                    hex = 1;
                    ++entity_start;
                }
                if (entity_start == entity_end) return 0;
                for (; entity_start < entity_end; ++entity_start) {
                    pw_zip_u8 digit = xml[entity_start];
                    unsigned value;
                    if (digit >= '0' && digit <= '9') value = digit - '0';
                    else if (hex && digit >= 'a' && digit <= 'f') value = digit - 'a' + 10;
                    else if (hex && digit >= 'A' && digit <= 'F') value = digit - 'A' + 10;
                    else return 0;
                    if (cp > (0x10ffffUL - value) / (hex ? 16UL : 10UL)) return 0;
                    cp = cp * (hex ? 16UL : 10UL) + value;
                }
                if (!append_codepoint(text, cp, maximum)) return 0;
            } else if (entity_end - entity_start == 3 && memcmp(xml + entity_start, "amp", 3) == 0) {
                if (!append_codepoint(text, '&', maximum)) return 0;
            } else if (entity_end - entity_start == 2 && memcmp(xml + entity_start, "lt", 2) == 0) {
                if (!append_codepoint(text, '<', maximum)) return 0;
            } else if (entity_end - entity_start == 2 && memcmp(xml + entity_start, "gt", 2) == 0) {
                if (!append_codepoint(text, '>', maximum)) return 0;
            } else if (entity_end - entity_start == 4 && memcmp(xml + entity_start, "quot", 4) == 0) {
                if (!append_codepoint(text, '"', maximum)) return 0;
            } else if (entity_end - entity_start == 4 && memcmp(xml + entity_start, "apos", 4) == 0) {
                if (!append_codepoint(text, '\'', maximum)) return 0;
            } else return 0;
            cursor = entity_end + 1;
        } else {
            pw_zip_u32 next = cursor;
            pw_zip_u32 cp;
            if (!utf8_decode(xml, end, &next, &cp) ||
                !append_bounded(text, xml + cursor, next - cursor, maximum)) return 0;
            cursor = next;
        }
    }
    return 1;
}

static int append_xml_text_styled(PwXmlBuffer *text, PwXmlBuffer *styles,
                                  const pw_zip_u8 *xml, pw_zip_u32 start,
                                  pw_zip_u32 end, pw_zip_u32 maximum,
                                  pw_zip_u8 style)
{
    pw_zip_u32 before = text->length;
    if (!append_xml_text(text, xml, start, end, maximum)) return 0;
    return append_style_repeat(styles, style, text->length - before, maximum);
}

static int local_name_equals(const pw_zip_u8 *name, pw_zip_u32 bytes, const char *expected)
{
    pw_zip_u32 start = 0;
    pw_zip_u32 length;
    pw_zip_u32 expected_bytes = (pw_zip_u32)strlen(expected);
    pw_zip_u32 index;
    for (index = 0; index < bytes; ++index) if (name[index] == ':') start = index + 1;
    length = bytes - start;
    return length == expected_bytes && memcmp(name + start, expected, expected_bytes) == 0;
}

static int tag_value_enabled(const pw_zip_u8 *xml, pw_zip_u32 attributes_start,
                             pw_zip_u32 tag_end)
{
    pw_zip_u32 cursor = attributes_start;
    while (cursor < tag_end) {
        pw_zip_u32 name_start;
        pw_zip_u32 name_bytes;
        while (cursor < tag_end && (is_space(xml[cursor]) || xml[cursor] == '/')) ++cursor;
        if (cursor >= tag_end) break;
        name_start = cursor;
        while (cursor < tag_end && is_name_char(xml[cursor])) ++cursor;
        if (cursor == name_start) { ++cursor; continue; }
        name_bytes = cursor - name_start;
        while (cursor < tag_end && is_space(xml[cursor])) ++cursor;
        if (cursor >= tag_end) break;
        if (xml[cursor] != '=') return 0;
        ++cursor;
        while (cursor < tag_end && is_space(xml[cursor])) ++cursor;
        if (cursor >= tag_end || (xml[cursor] != '\'' && xml[cursor] != '"')) continue;
        {
            pw_zip_u8 quote = xml[cursor++];
            pw_zip_u32 value_start = cursor;
            while (cursor < tag_end && xml[cursor] != quote) ++cursor;
            if (local_name_equals(xml + name_start, name_bytes, "val")) {
                pw_zip_u32 value_bytes = cursor - value_start;
                if ((value_bytes == 1 && xml[value_start] == '0') ||
                    (value_bytes == 5 && memcmp(xml + value_start, "false", 5) == 0) ||
                    (value_bytes == 3 && memcmp(xml + value_start, "off", 3) == 0) ||
                    (value_bytes == 4 && memcmp(xml + value_start, "none", 4) == 0)) return 0;
                return 1;
            }
            if (cursor < tag_end) ++cursor;
        }
    }
    return 1;
}

static int tag_value_equals(const pw_zip_u8 *xml, pw_zip_u32 attributes_start,
                            pw_zip_u32 tag_end, const char *expected)
{
    pw_zip_u32 cursor = attributes_start;
    pw_zip_u32 expected_bytes = (pw_zip_u32)strlen(expected);
    while (cursor < tag_end) {
        pw_zip_u32 name_start;
        pw_zip_u32 name_bytes;
        while (cursor < tag_end && (is_space(xml[cursor]) || xml[cursor] == '/')) ++cursor;
        if (cursor >= tag_end) break;
        name_start = cursor;
        while (cursor < tag_end && is_name_char(xml[cursor])) ++cursor;
        if (cursor == name_start) { ++cursor; continue; }
        name_bytes = cursor - name_start;
        while (cursor < tag_end && is_space(xml[cursor])) ++cursor;
        if (cursor >= tag_end) break;
        if (xml[cursor] != '=') return 0;
        ++cursor;
        while (cursor < tag_end && is_space(xml[cursor])) ++cursor;
        if (cursor >= tag_end || (xml[cursor] != '\'' && xml[cursor] != '"')) return 0;
        {
            pw_zip_u8 quote = xml[cursor++];
            pw_zip_u32 value_start = cursor;
            pw_zip_u32 value_bytes;
            while (cursor < tag_end && xml[cursor] != quote) ++cursor;
            value_bytes = cursor - value_start;
            if (local_name_equals(xml + name_start, name_bytes, "val"))
                return value_bytes == expected_bytes &&
                       memcmp(xml + value_start, expected, expected_bytes) == 0;
            if (cursor < tag_end) ++cursor;
        }
    }
    return 0;
}

static int tag_attribute_value(const pw_zip_u8 *xml, pw_zip_u32 attributes_start,
                               pw_zip_u32 tag_end, const char *wanted,
                               char *value, unsigned value_capacity,
                               int *found)
{
    pw_zip_u32 cursor = attributes_start;
    *found = 0;
    while (cursor < tag_end) {
        pw_zip_u32 name_start;
        pw_zip_u32 name_bytes;
        pw_zip_u32 value_start;
        pw_zip_u32 value_bytes;
        pw_zip_u8 quote;
        while (cursor < tag_end && (is_space(xml[cursor]) || xml[cursor] == '/')) ++cursor;
        if (cursor >= tag_end) break;
        name_start = cursor;
        while (cursor < tag_end && is_name_char(xml[cursor])) ++cursor;
        if (cursor == name_start) return 0;
        name_bytes = cursor - name_start;
        while (cursor < tag_end && is_space(xml[cursor])) ++cursor;
        if (cursor >= tag_end || xml[cursor++] != '=') return 0;
        while (cursor < tag_end && is_space(xml[cursor])) ++cursor;
        if (cursor >= tag_end || (xml[cursor] != '\'' && xml[cursor] != '"')) return 0;
        quote = xml[cursor++];
        value_start = cursor;
        while (cursor < tag_end && xml[cursor] != quote) ++cursor;
        if (cursor >= tag_end) return 0;
        value_bytes = cursor - value_start;
        if (local_name_equals(xml + name_start, name_bytes, wanted)) {
            if (*found || value_bytes >= value_capacity) return 0;
            memcpy(value, xml + value_start, value_bytes);
            value[value_bytes] = '\0';
            *found = 1;
        }
        ++cursor;
    }
    return 1;
}

static int exact_is(const char *value, const char *expected)
{
    return strcmp(value, expected) == 0;
}

static int hex_value(unsigned char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static int resolve_part_target(const char *base_part, const char *target,
                               char *resolved, unsigned resolved_capacity)
{
    char decoded[DOCX_PART_PATH_BYTES];
    char combined[DOCX_PART_PATH_BYTES];
    unsigned segment_starts[DOCX_PART_PATH_BYTES / 2];
    unsigned count = 0;
    unsigned used = 0;
    unsigned input = 0;
    unsigned target_bytes;
    unsigned decoded_bytes = 0;
    unsigned decode_at;
    unsigned base_bytes = (unsigned)strlen(base_part);
    int absolute;
    if (target == NULL || target[0] == '\0') return 0;
    target_bytes = (unsigned)strlen(target);
    if (target_bytes >= sizeof(combined) || resolved_capacity == 0) return 0;
    for (decode_at = 0; decode_at < target_bytes; ++decode_at) {
        unsigned char c = (unsigned char)target[decode_at];
        if (c == '%') {
            int high, low;
            if (decode_at + 2 >= target_bytes) return 0;
            high = hex_value((unsigned char)target[decode_at + 1]);
            low = hex_value((unsigned char)target[decode_at + 2]);
            if (high < 0 || low < 0) return 0;
            c = (unsigned char)((high << 4) | low);
            decode_at += 2;
        }
        if (c < 0x20 || c == 0x7f || c == '\\' || c == ':') return 0;
        decoded[decoded_bytes++] = (char)c;
    }
    decoded[decoded_bytes] = '\0';
    target = decoded;
    target_bytes = decoded_bytes;
    absolute = target[0] == '/';
    if (absolute && target[1] == '/') return 0;
    if (!absolute && base_bytes != 0) {
        unsigned i;
        for (i = base_bytes; i > 0 && base_part[i - 1] != '/'; --i) {}
        if (i != 0) {
            unsigned directory_bytes = i - 1;
            if (directory_bytes >= sizeof(combined)) return 0;
            memcpy(combined, base_part, directory_bytes);
            used = directory_bytes;
        }
    }
    if (used != 0) combined[used++] = '/';
    input = absolute ? 1 : 0;
    while (input <= target_bytes) {
        unsigned start = input;
        unsigned bytes;
        unsigned i;
        while (input < target_bytes && target[input] != '/') {
            unsigned char c = (unsigned char)target[input];
            if (c < 0x20 || c == 0x7f || c == '\\' || c == ':' || c == '?' || c == '#') return 0;
            ++input;
        }
        bytes = input - start;
        if (bytes == 1 && target[start] == '.') {
            /* Dot segments do not add a package member component. */
        } else if (bytes == 2 && target[start] == '.' && target[start + 1] == '.') {
            if (count == 0) return 0;
            used = segment_starts[--count];
            if (used != 0 && combined[used - 1] == '/') --used;
        } else if (bytes != 0) {
            if (count >= sizeof(segment_starts) / sizeof(segment_starts[0]) ||
                used + bytes >= sizeof(combined)) return 0;
            if (used != 0 && combined[used - 1] != '/') combined[used++] = '/';
            segment_starts[count++] = used == 0 ? 0 : used - 1;
            memcpy(combined + used, target + start, bytes);
            used += bytes;
        }
        if (input == target_bytes) break;
        ++input;
    }
    if (used == 0 || used >= resolved_capacity) return 0;
    memcpy(resolved, combined, used);
    resolved[used] = '\0';
    return 1;
}

static int relationship_target(const pw_zip_u8 *xml, pw_zip_u32 xml_bytes,
                               const char *relationship_suffix,
                               const char *base_part, char *target,
                               unsigned target_capacity, int required)
{
    pw_zip_u32 cursor = 0;
    unsigned matches = 0;
    char selected[DOCX_PART_PATH_BYTES];
    if (xml_bytes >= 3 && xml[0] == 0xef && xml[1] == 0xbb && xml[2] == 0xbf) cursor = 3;
    while (cursor < xml_bytes) {
        if (xml[cursor] != '<') { ++cursor; continue; }
        if (cursor + 4 <= xml_bytes && memcmp(xml + cursor, "<!--", 4) == 0) {
            cursor += 4;
            while (cursor + 3 <= xml_bytes && memcmp(xml + cursor, "-->", 3) != 0) ++cursor;
            if (cursor + 3 > xml_bytes) return -1;
            cursor += 3;
            continue;
        }
        if (cursor + 2 <= xml_bytes && (xml[cursor + 1] == '?' || xml[cursor + 1] == '!')) {
            pw_zip_u8 terminator = xml[cursor + 1];
            ++cursor;
            if (terminator == '!') return -1;
            ++cursor;
            while (cursor + 1 < xml_bytes &&
                   !(xml[cursor] == '?' && xml[cursor + 1] == '>')) ++cursor;
            if (cursor + 1 >= xml_bytes) return -1;
            cursor += 2;
            continue;
        }
        {
            int closing = 0;
            pw_zip_u32 name_start;
            pw_zip_u32 name_end;
            pw_zip_u32 tag_end;
            if (cursor + 1 < xml_bytes && xml[cursor + 1] == '/') { closing = 1; ++cursor; }
            ++cursor;
            name_start = cursor;
            if (cursor >= xml_bytes || !is_name_start(xml[cursor])) return -1;
            while (cursor < xml_bytes && is_name_char(xml[cursor])) ++cursor;
            name_end = cursor;
            while (cursor < xml_bytes && xml[cursor] != '>') {
                if (xml[cursor] == '\'' || xml[cursor] == '"') {
                    pw_zip_u8 quote = xml[cursor++];
                    while (cursor < xml_bytes && xml[cursor] != quote) ++cursor;
                    if (cursor >= xml_bytes) return -1;
                    ++cursor;
                } else ++cursor;
            }
            if (cursor >= xml_bytes) return -1;
            tag_end = cursor++;
            if (!closing && local_name_equals(xml + name_start, name_end - name_start, "Relationship")) {
                char type[DOCX_PART_PATH_BYTES];
                char target_value[DOCX_PART_PATH_BYTES];
                char mode[32];
                int have_type, have_target, have_mode;
                if (!tag_attribute_value(xml, name_end, tag_end, "Type", type, sizeof(type), &have_type) ||
                    !tag_attribute_value(xml, name_end, tag_end, "Target", target_value, sizeof(target_value), &have_target) ||
                    !tag_attribute_value(xml, name_end, tag_end, "TargetMode", mode, sizeof(mode), &have_mode)) return -1;
                if (have_type && exact_is(type, relationship_suffix)) {
                    if (!have_target || (have_mode && strcmp(mode, "External") == 0) ||
                        !resolve_part_target(base_part, target_value, selected, sizeof(selected))) return -1;
                    if (++matches > 1) return -1;
                }
            }
        }
    }
    if (matches == 0) return required ? 0 : 1;
    if (strlen(selected) >= target_capacity) return -1;
    strcpy(target, selected);
    return 1;
}

static int document_relationships_path(const char *document_part,
                                       char *path, unsigned path_capacity)
{
    unsigned bytes = (unsigned)strlen(document_part);
    unsigned base = bytes;
    unsigned prefix;
    unsigned needed;
    while (base > 0 && document_part[base - 1] != '/') --base;
    prefix = base;
    needed = prefix + 6U + (bytes - base) + 5U;
    if (needed >= path_capacity) return 0;
    if (prefix != 0) memcpy(path, document_part, prefix);
    memcpy(path + prefix, "_rels/", 6);
    memcpy(path + prefix + 6U, document_part + base, bytes - base);
    memcpy(path + prefix + 6U + (bytes - base), ".rels", 5);
    path[needed] = '\0';
    return 1;
}

static int read_document_styles(const char *package_path,
                                const char *document_part,
                                pw_zip_u32 max_xml_bytes,
                                PwDocxStyles *style_table)
{
    PwZip32Buffer relationships;
    PwZip32Buffer style_xml;
    char relationships_part[DOCX_PART_PATH_BYTES];
    char styles_part[DOCX_PART_PATH_BYTES];
    int result;
    int target_result;
    int style_result;
    relationships.data = NULL;
    relationships.size = 0;
    style_xml.data = NULL;
    style_xml.size = 0;
    styles_part[0] = '\0';
    if (!document_relationships_path(document_part, relationships_part,
                                     sizeof(relationships_part)))
        return PW_DOCX_READ_PACKAGE_ERROR;
    result = pw_zip32_read_member(package_path, relationships_part,
                                  max_xml_bytes, &relationships);
    if (result == PW_ZIP32_NOT_FOUND) return PW_DOCX_READ_OK;
    if (result == PW_ZIP32_TOO_LARGE) return PW_DOCX_READ_TOO_LARGE;
    if (result != PW_ZIP32_OK)
        return result == PW_ZIP32_NO_MEMORY ? PW_DOCX_READ_NO_MEMORY : PW_DOCX_READ_PACKAGE_ERROR;
    target_result = relationship_target(relationships.data, relationships.size,
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles",
        document_part, styles_part, sizeof(styles_part), 0);
    pw_zip32_buffer_dispose(&relationships);
    if (target_result < 0) return PW_DOCX_READ_PACKAGE_ERROR;
    if (styles_part[0] == '\0') return PW_DOCX_READ_OK;
    result = pw_zip32_read_member(package_path, styles_part, max_xml_bytes, &style_xml);
    if (result == PW_ZIP32_TOO_LARGE) return PW_DOCX_READ_TOO_LARGE;
    if (result != PW_ZIP32_OK)
        return result == PW_ZIP32_NO_MEMORY ? PW_DOCX_READ_NO_MEMORY : PW_DOCX_READ_PACKAGE_ERROR;
    style_result = pw_docx_styles_parse(style_xml.data, style_xml.size, style_table);
    pw_zip32_buffer_dispose(&style_xml);
    if (style_result == PW_DOCX_STYLES_NO_MEMORY) return PW_DOCX_READ_NO_MEMORY;
    if (style_result != PW_DOCX_STYLES_OK) return PW_DOCX_READ_XML_ERROR;
    return PW_DOCX_READ_OK;
}

static int resolve_paragraph_style(const PwDocxStyles *style_table,
                                   const char *style_id,
                                   int direct_alignment_set,
                                   pw_zip_u8 direct_alignment,
                                   pw_zip_u8 *run_style,
                                   pw_zip_u8 *paragraph_alignment)
{
    if (!pw_docx_styles_paragraph(style_table, style_id, run_style,
                                  paragraph_alignment)) return 0;
    if (direct_alignment_set)
        *paragraph_alignment = (pw_zip_u8)((*paragraph_alignment & (pw_zip_u8)~24) |
                                           (direct_alignment & 24));
    return 1;
}

static int resolve_run_style(const PwDocxStyles *style_table,
                             const char *character_style_id,
                             pw_zip_u8 paragraph_run_style,
                             pw_zip_u8 direct_mask,
                             pw_zip_u8 direct_value,
                             pw_zip_u8 *run_style)
{
    *run_style = paragraph_run_style;
    if (!pw_docx_styles_character(style_table, character_style_id, run_style)) return 0;
    *run_style = (pw_zip_u8)((*run_style & (pw_zip_u8)~direct_mask) |
                             (direct_value & direct_mask));
    return 1;
}

static int xml_to_text(const pw_zip_u8 *xml, pw_zip_u32 xml_bytes,
                       pw_zip_u32 max_text_bytes, PwXmlBuffer *text,
                       PwXmlBuffer *styles,
                       const PwDocxStyles *style_table,
                       const PwDocxNumbering *numbering)
{
    pw_zip_u32 cursor = 0;
    int in_body = 0;
    int in_text = 0;
    int in_deleted = 0;
    int body_found = 0;
    int body_closed = 0;
    int paragraphs = 0;
    int cells_in_row = 0;
    int suppress_paragraph_separator = 0;
    int in_run_properties = 0;
    int in_paragraph_properties = 0;
    int direct_alignment_set = 0;
    pw_zip_u8 direct_alignment = 0;
    pw_zip_u8 paragraph_run_style = 0;
    pw_zip_u8 direct_run_mask = 0;
    pw_zip_u8 direct_run_value = 0;
    char paragraph_style_id[DOCX_PART_PATH_BYTES];
    char character_style_id[DOCX_PART_PATH_BYTES];
    pw_zip_u8 run_style = 0;
    pw_zip_u8 paragraph_alignment = 0;
    pw_zip_u8 paragraph_list_style = 0;

    paragraph_style_id[0] = '\0';
    character_style_id[0] = '\0';
    if (!resolve_paragraph_style(style_table, NULL, 0, 0,
                                 &paragraph_run_style, &paragraph_alignment)) return 0;
    if (xml_bytes >= 3 && xml[0] == 0xef && xml[1] == 0xbb && xml[2] == 0xbf) cursor = 3;
    while (cursor < xml_bytes) {
        if (xml[cursor] != '<') {
            pw_zip_u32 start = cursor;
            while (cursor < xml_bytes && xml[cursor] != '<') ++cursor;
            if (in_body && in_text && !in_deleted &&
                !append_xml_text_styled(text, styles, xml, start, cursor,
                                        max_text_bytes, (pw_zip_u8)(run_style | paragraph_alignment | paragraph_list_style))) return 0;
            continue;
        }

        if (cursor + 4 <= xml_bytes && memcmp(xml + cursor, "<!--", 4) == 0) {
            cursor += 4;
            while (cursor + 3 <= xml_bytes && memcmp(xml + cursor, "-->", 3) != 0) ++cursor;
            if (cursor + 3 > xml_bytes) return 0;
            cursor += 3;
            continue;
        }
        if (cursor + 9 <= xml_bytes && memcmp(xml + cursor, "<![CDATA[", 9) == 0) {
            pw_zip_u32 start = cursor + 9;
            cursor = start;
            while (cursor + 3 <= xml_bytes && memcmp(xml + cursor, "]]>", 3) != 0) ++cursor;
            if (cursor + 3 > xml_bytes) return 0;
            if (in_body && in_text && !in_deleted &&
                !append_xml_text_styled(text, styles, xml, start, cursor,
                                        max_text_bytes, (pw_zip_u8)(run_style | paragraph_alignment | paragraph_list_style))) return 0;
            cursor += 3;
            continue;
        }
        if (cursor + 2 <= xml_bytes && xml[cursor + 1] == '?') {
            cursor += 2;
            while (cursor + 2 <= xml_bytes && !(xml[cursor] == '?' && xml[cursor + 1] == '>')) ++cursor;
            if (cursor + 2 > xml_bytes) return 0;
            cursor += 2;
            continue;
        }
        if (cursor + 2 <= xml_bytes && xml[cursor + 1] == '!') return 0;

        {
            int closing = 0;
            int self_closing = 0;
            pw_zip_u32 name_start;
            pw_zip_u32 name_bytes;
            pw_zip_u32 name_end;
            pw_zip_u32 tag_end;
            if (cursor + 1 < xml_bytes && xml[cursor + 1] == '/') { closing = 1; ++cursor; }
            ++cursor;
            name_start = cursor;
            if (cursor >= xml_bytes || !is_name_start(xml[cursor])) return 0;
            while (cursor < xml_bytes && is_name_char(xml[cursor])) ++cursor;
            name_end = cursor;
            name_bytes = name_end - name_start;
            while (cursor < xml_bytes && xml[cursor] != '>') {
                if (xml[cursor] == '\'' || xml[cursor] == '"') {
                    pw_zip_u8 quote = xml[cursor++];
                    while (cursor < xml_bytes && xml[cursor] != quote) ++cursor;
                    if (cursor >= xml_bytes) return 0;
                    ++cursor;
                } else {
                    ++cursor;
                }
            }
            if (cursor >= xml_bytes) return 0;
            tag_end = cursor;
            {
                pw_zip_u32 trimmed_end = tag_end;
                while (trimmed_end > name_end && is_space(xml[trimmed_end - 1])) --trimmed_end;
                self_closing = trimmed_end > name_end && xml[trimmed_end - 1] == '/';
            }
            ++cursor;

            if (closing) {
                if (local_name_equals(xml + name_start, name_bytes, "t")) in_text = 0;
                else if (local_name_equals(xml + name_start, name_bytes, "del")) in_deleted = 0;
                else if (local_name_equals(xml + name_start, name_bytes, "rPr")) {
                    if (!resolve_run_style(style_table, character_style_id,
                            paragraph_run_style, direct_run_mask, direct_run_value,
                            &run_style)) return 0;
                    in_run_properties = 0;
                } else if (local_name_equals(xml + name_start, name_bytes, "pPr")) {
                    if (!resolve_paragraph_style(style_table,
                            paragraph_style_id[0] != '\0' ? paragraph_style_id : NULL,
                            direct_alignment_set, direct_alignment,
                            &paragraph_run_style, &paragraph_alignment)) return 0;
                    in_paragraph_properties = 0;
                } else if (local_name_equals(xml + name_start, name_bytes, "r")) run_style = paragraph_run_style;
                else if (local_name_equals(xml + name_start, name_bytes, "body")) {
                    in_body = 0;
                    body_closed = 1;
                }
                continue;
            }

            if (local_name_equals(xml + name_start, name_bytes, "body")) {
                in_body = 1;
                body_found = 1;
            } else if (in_body && local_name_equals(xml + name_start, name_bytes, "r")) {
                run_style = paragraph_run_style;
                character_style_id[0] = '\0';
                direct_run_mask = 0;
                direct_run_value = 0;
            } else if (in_body && local_name_equals(xml + name_start, name_bytes, "rPr")) {
                in_run_properties = 1;
                character_style_id[0] = '\0';
                direct_run_mask = 0;
                direct_run_value = 0;
            } else if (in_body && local_name_equals(xml + name_start, name_bytes, "pPr")) {
                in_paragraph_properties = 1;
                paragraph_style_id[0] = '\0';
                direct_alignment_set = 0;
                direct_alignment = 0;
                paragraph_list_style = 0;
            } else if (in_body && in_paragraph_properties &&
                       local_name_equals(xml + name_start, name_bytes, "pStyle")) {
                int found;
                if (!tag_attribute_value(xml, name_end, tag_end, "val",
                        paragraph_style_id, sizeof(paragraph_style_id), &found)) return 0;
                if (!found) paragraph_style_id[0] = '\0';
            } else if (in_body && in_run_properties &&
                       local_name_equals(xml + name_start, name_bytes, "rStyle")) {
                int found;
                if (!tag_attribute_value(xml, name_end, tag_end, "val",
                        character_style_id, sizeof(character_style_id), &found)) return 0;
                if (!found) character_style_id[0] = '\0';
            } else if (in_body && in_paragraph_properties &&
                       local_name_equals(xml + name_start, name_bytes, "jc")) {
                direct_alignment_set = 1;
                if (tag_value_equals(xml, name_end, tag_end, "center")) direct_alignment = 8;
                else if (tag_value_equals(xml, name_end, tag_end, "right") ||
                         tag_value_equals(xml, name_end, tag_end, "end")) direct_alignment = 16;
                else if (tag_value_equals(xml, name_end, tag_end, "both") ||
                         tag_value_equals(xml, name_end, tag_end, "distribute")) direct_alignment = 24;
                else direct_alignment = 0;
            } else if (in_body && in_paragraph_properties &&
                       local_name_equals(xml + name_start, name_bytes, "numId")) {
                char num_id[16];
                int found;
                if (!tag_attribute_value(xml, name_end, tag_end, "val",
                        num_id, sizeof(num_id), &found)) return 0;
                paragraph_list_style = found ? numbering_style_for_id(numbering, num_id) : 0;
            } else if (in_body && in_run_properties &&
                       local_name_equals(xml + name_start, name_bytes, "b")) {
                direct_run_mask |= 1;
                if (tag_value_enabled(xml, name_end, tag_end)) direct_run_value |= 1;
                else direct_run_value &= (pw_zip_u8)~1;
            } else if (in_body && in_run_properties &&
                       local_name_equals(xml + name_start, name_bytes, "i")) {
                direct_run_mask |= 2;
                if (tag_value_enabled(xml, name_end, tag_end)) direct_run_value |= 2;
                else direct_run_value &= (pw_zip_u8)~2;
            } else if (in_body && in_run_properties &&
                       local_name_equals(xml + name_start, name_bytes, "u")) {
                direct_run_mask |= 4;
                if (tag_value_enabled(xml, name_end, tag_end)) direct_run_value |= 4;
                else direct_run_value &= (pw_zip_u8)~4;
            } else if (in_body && local_name_equals(xml + name_start, name_bytes, "del")) {
                in_deleted = 1;
            } else if (in_body && local_name_equals(xml + name_start, name_bytes, "tr")) {
                cells_in_row = 0;
            } else if (in_body && local_name_equals(xml + name_start, name_bytes, "tc")) {
                if (cells_in_row > 0 && !in_deleted &&
                    !append_styled(text, styles, "\t", 1, max_text_bytes,
                                   (pw_zip_u8)(run_style | paragraph_alignment | paragraph_list_style))) return 0;
                ++cells_in_row;
                suppress_paragraph_separator = 1;
            } else if (in_body && local_name_equals(xml + name_start, name_bytes, "p")) {
                if (paragraphs > 0 && !suppress_paragraph_separator && !in_deleted &&
                    !append_styled(text, styles, "\n", 1, max_text_bytes,
                                   (pw_zip_u8)(paragraph_alignment | paragraph_list_style))) return 0;
                ++paragraphs;
                suppress_paragraph_separator = 0;
                paragraph_style_id[0] = '\0';
                direct_alignment_set = 0;
                direct_alignment = 0;
                paragraph_list_style = 0;
                if (!resolve_paragraph_style(style_table, NULL, 0, 0,
                        &paragraph_run_style, &paragraph_alignment)) return 0;
                in_paragraph_properties = 0;
            } else if (in_body && local_name_equals(xml + name_start, name_bytes, "t")) {
                in_text = 1;
            } else if (in_body && !in_deleted &&
                       (local_name_equals(xml + name_start, name_bytes, "br") ||
                        local_name_equals(xml + name_start, name_bytes, "cr"))) {
                if (!append_styled(text, styles, "\n", 1, max_text_bytes,
                                   (pw_zip_u8)(run_style | paragraph_alignment | paragraph_list_style))) return 0;
            } else if (in_body && !in_deleted && local_name_equals(xml + name_start, name_bytes, "tab")) {
                if (!append_styled(text, styles, "\t", 1, max_text_bytes,
                                   (pw_zip_u8)(run_style | paragraph_alignment | paragraph_list_style))) return 0;
            }

            if (self_closing) {
                if (local_name_equals(xml + name_start, name_bytes, "t")) in_text = 0;
                else if (local_name_equals(xml + name_start, name_bytes, "del")) in_deleted = 0;
                else if (local_name_equals(xml + name_start, name_bytes, "rPr")) {
                    if (!resolve_run_style(style_table, character_style_id,
                            paragraph_run_style, direct_run_mask, direct_run_value,
                            &run_style)) return 0;
                    in_run_properties = 0;
                } else if (local_name_equals(xml + name_start, name_bytes, "pPr")) {
                    if (!resolve_paragraph_style(style_table,
                            paragraph_style_id[0] != '\0' ? paragraph_style_id : NULL,
                            direct_alignment_set, direct_alignment,
                            &paragraph_run_style, &paragraph_alignment)) return 0;
                    in_paragraph_properties = 0;
                }
                else if (local_name_equals(xml + name_start, name_bytes, "body")) {
                    in_body = 0;
                    body_closed = 1;
                }
            }
        }
    }
    return body_found && body_closed;
}

int pw_docx_read_rich(const char *path,
                      pw_zip_u32 max_xml_bytes,
                      pw_zip_u32 max_text_bytes,
                      PwZip32Buffer *utf8_text,
                      PwZip32Buffer *character_style)
{
    PwZip32Buffer xml;
    PwZip32Buffer relationships;
    PwXmlBuffer text;
    PwXmlBuffer styles;
    PwDocxStyles style_table;
    PwDocxNumbering numbering;
    char document_part[DOCX_PART_PATH_BYTES];
    int result;
    int relationship_result;
    if (utf8_text == NULL || character_style == NULL || path == NULL ||
        path[0] == 0 || max_xml_bytes == 0)
        return PW_DOCX_READ_BAD_ARGUMENT;
    utf8_text->data = NULL;
    utf8_text->size = 0;
    character_style->data = NULL;
    character_style->size = 0;
    xml.data = NULL;
    xml.size = 0;
    relationships.data = NULL;
    relationships.size = 0;
    document_part[0] = '\0';
    numbering.count = 0;
    pw_docx_styles_init(&style_table);
    pw_xml_buffer_init(&text);
    pw_xml_buffer_init(&styles);
    result = pw_zip32_read_member(path, "_rels/.rels", max_xml_bytes, &relationships);
    if (result == PW_ZIP32_TOO_LARGE) {
        pw_docx_styles_dispose(&style_table);
        pw_xml_buffer_dispose(&text);
        pw_xml_buffer_dispose(&styles);
        return PW_DOCX_READ_TOO_LARGE;
    }
    if (result != PW_ZIP32_OK) {
        pw_docx_styles_dispose(&style_table);
        pw_xml_buffer_dispose(&text);
        pw_xml_buffer_dispose(&styles);
        return result == PW_ZIP32_NO_MEMORY ? PW_DOCX_READ_NO_MEMORY : PW_DOCX_READ_PACKAGE_ERROR;
    }
    relationship_result = relationship_target(relationships.data, relationships.size,
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument",
        "", document_part, sizeof(document_part), 1);
    pw_zip32_buffer_dispose(&relationships);
    if (relationship_result <= 0) {
        pw_docx_styles_dispose(&style_table);
        pw_xml_buffer_dispose(&text);
        pw_xml_buffer_dispose(&styles);
        return PW_DOCX_READ_PACKAGE_ERROR;
    }
    result = read_list_numbering(path, document_part, max_xml_bytes, &numbering);
    if (result != PW_DOCX_READ_OK) {
        pw_docx_styles_dispose(&style_table);
        pw_xml_buffer_dispose(&text);
        pw_xml_buffer_dispose(&styles);
        return result;
    }
    result = pw_zip32_read_member(path, document_part, max_xml_bytes, &xml);
    if (result == PW_ZIP32_TOO_LARGE) {
        pw_docx_styles_dispose(&style_table);
        pw_xml_buffer_dispose(&text);
        pw_xml_buffer_dispose(&styles);
        return PW_DOCX_READ_TOO_LARGE;
    }
    if (result != PW_ZIP32_OK) {
        pw_docx_styles_dispose(&style_table);
        pw_xml_buffer_dispose(&text);
        pw_xml_buffer_dispose(&styles);
        return result == PW_ZIP32_NO_MEMORY ? PW_DOCX_READ_NO_MEMORY : PW_DOCX_READ_PACKAGE_ERROR;
    }
    result = read_document_styles(path, document_part, max_xml_bytes, &style_table);
    if (result != PW_DOCX_READ_OK) {
        pw_zip32_buffer_dispose(&xml);
        pw_docx_styles_dispose(&style_table);
        pw_xml_buffer_dispose(&text);
        pw_xml_buffer_dispose(&styles);
        return result;
    }
    if (!xml_to_text(xml.data, xml.size, max_text_bytes, &text, &styles,
                     &style_table, &numbering)) {
        int too_large = text.too_large || styles.too_large;
        int no_memory = (text.failed || styles.failed) && !text.invalid_text && !styles.invalid_text;
        pw_zip32_buffer_dispose(&xml);
        pw_docx_styles_dispose(&style_table);
        pw_xml_buffer_dispose(&text);
        pw_xml_buffer_dispose(&styles);
        if (too_large) return PW_DOCX_READ_TOO_LARGE;
        return no_memory ? PW_DOCX_READ_NO_MEMORY : PW_DOCX_READ_XML_ERROR;
    }
    pw_zip32_buffer_dispose(&xml);
    pw_docx_styles_dispose(&style_table);
    utf8_text->data = text.data;
    utf8_text->size = text.length;
    text.data = NULL;
    character_style->data = styles.data;
    character_style->size = styles.length;
    styles.data = NULL;
    pw_xml_buffer_dispose(&text);
    pw_xml_buffer_dispose(&styles);
    return PW_DOCX_READ_OK;
}

int pw_docx_read_text(const char *path,
                      pw_zip_u32 max_xml_bytes,
                      pw_zip_u32 max_text_bytes,
                      PwZip32Buffer *utf8_text)
{
    PwZip32Buffer styles;
    int result;
    styles.data = NULL;
    styles.size = 0;
    result = pw_docx_read_rich(path, max_xml_bytes, max_text_bytes,
                               utf8_text, &styles);
    pw_zip32_buffer_dispose(&styles);
    return result;
}
