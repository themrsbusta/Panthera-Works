#include "docx_styles.h"

#include <stdlib.h>
#include <string.h>

#define DOCX_STYLE_VALUE_BYTES 256U

struct PwDocxStyleEntry {
    char *id;
    char *based_on;
    pw_zip_u8 type;
    pw_zip_u8 run_mask;
    pw_zip_u8 run_value;
    pw_zip_u8 paragraph_mask;
    pw_zip_u8 paragraph_value;
};

enum { STYLE_OTHER = 0, STYLE_PARAGRAPH = 1, STYLE_CHARACTER = 2 };

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

static int local_name_equals(const pw_zip_u8 *name, pw_zip_u32 bytes,
                             const char *expected)
{
    pw_zip_u32 start = 0;
    pw_zip_u32 i;
    pw_zip_u32 expected_bytes = (pw_zip_u32)strlen(expected);
    for (i = 0; i < bytes; ++i) if (name[i] == ':') start = i + 1;
    return bytes - start == expected_bytes &&
           memcmp(name + start, expected, expected_bytes) == 0;
}

static int get_attribute(const pw_zip_u8 *xml, pw_zip_u32 attributes_start,
                         pw_zip_u32 tag_end, const char *wanted,
                         char *value, unsigned capacity, int *found)
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
            if (*found || value_bytes >= capacity) return 0;
            memcpy(value, xml + value_start, value_bytes);
            value[value_bytes] = '\0';
            *found = 1;
        }
        ++cursor;
    }
    return 1;
}

static char *copy_string(const char *value)
{
    unsigned bytes;
    char *copy;
    if (value == NULL) return NULL;
    bytes = (unsigned)strlen(value);
    copy = (char *)malloc(bytes + 1U);
    if (copy != NULL) memcpy(copy, value, bytes + 1U);
    return copy;
}

static int add_style(PwDocxStyles *styles, const char *id, pw_zip_u8 type,
                     pw_zip_u8 is_default, int *index)
{
    PwDocxStyleEntry *entries;
    unsigned next_capacity;
    unsigned i;
    for (i = 0; i < styles->count; ++i)
        if (strcmp(styles->entries[i].id, id) == 0) return PW_DOCX_STYLES_XML_ERROR;
    if (styles->count == styles->capacity) {
        next_capacity = styles->capacity == 0 ? 16U : styles->capacity * 2U;
        if (next_capacity < styles->capacity ||
            next_capacity > 0x7fffffffUL / sizeof(*entries)) return PW_DOCX_STYLES_NO_MEMORY;
        entries = (PwDocxStyleEntry *)realloc(styles->entries,
                                              next_capacity * sizeof(*entries));
        if (entries == NULL) return PW_DOCX_STYLES_NO_MEMORY;
        styles->entries = entries;
        styles->capacity = next_capacity;
    }
    styles->entries[styles->count].id = copy_string(id);
    styles->entries[styles->count].based_on = NULL;
    styles->entries[styles->count].type = type;
    styles->entries[styles->count].run_mask = 0;
    styles->entries[styles->count].run_value = 0;
    styles->entries[styles->count].paragraph_mask = 0;
    styles->entries[styles->count].paragraph_value = 0;
    if (styles->entries[styles->count].id == NULL) return PW_DOCX_STYLES_NO_MEMORY;
    *index = (int)styles->count++;
    if (is_default && type == STYLE_PARAGRAPH) {
        char *default_id = copy_string(id);
        if (default_id == NULL) return PW_DOCX_STYLES_NO_MEMORY;
        free(styles->default_paragraph_id);
        styles->default_paragraph_id = default_id;
    }
    return PW_DOCX_STYLES_OK;
}

static pw_zip_u8 style_type(const char *value)
{
    if (strcmp(value, "paragraph") == 0) return STYLE_PARAGRAPH;
    if (strcmp(value, "character") == 0) return STYLE_CHARACTER;
    return STYLE_OTHER;
}

static int value_is_false(const pw_zip_u8 *xml, pw_zip_u32 start,
                          pw_zip_u32 end)
{
    pw_zip_u32 cursor = start;
    while (cursor < end && is_space(xml[cursor])) ++cursor;
    if (end - cursor == 1 && xml[cursor] == '0') return 1;
    if (end - cursor == 5 && memcmp(xml + cursor, "false", 5) == 0) return 1;
    if (end - cursor == 3 && memcmp(xml + cursor, "off", 3) == 0) return 1;
    if (end - cursor == 4 && memcmp(xml + cursor, "none", 4) == 0) return 1;
    return 0;
}

static int value_is_true(const char *value)
{
    return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
           strcmp(value, "on") == 0;
}

static int tag_enabled(const pw_zip_u8 *xml, pw_zip_u32 attributes_start,
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
        if (cursor == name_start) return 0;
        name_bytes = cursor - name_start;
        while (cursor < tag_end && is_space(xml[cursor])) ++cursor;
        if (cursor >= tag_end || xml[cursor++] != '=') return 0;
        while (cursor < tag_end && is_space(xml[cursor])) ++cursor;
        if (cursor >= tag_end || (xml[cursor] != '\'' && xml[cursor] != '"')) return 0;
        {
            pw_zip_u8 quote = xml[cursor++];
            pw_zip_u32 value_start = cursor;
            while (cursor < tag_end && xml[cursor] != quote) ++cursor;
            if (cursor >= tag_end) return 0;
            if (local_name_equals(xml + name_start, name_bytes, "val"))
                return !value_is_false(xml, value_start, cursor);
            ++cursor;
        }
    }
    return 1;
}

void pw_docx_styles_init(PwDocxStyles *styles)
{
    memset(styles, 0, sizeof(*styles));
}

void pw_docx_styles_dispose(PwDocxStyles *styles)
{
    unsigned i;
    if (styles == NULL) return;
    for (i = 0; i < styles->count; ++i) {
        free(styles->entries[i].id);
        free(styles->entries[i].based_on);
    }
    free(styles->entries);
    free(styles->default_paragraph_id);
    pw_docx_styles_init(styles);
}

int pw_docx_styles_parse(const pw_zip_u8 *xml, pw_zip_u32 xml_bytes,
                         PwDocxStyles *styles)
{
    pw_zip_u32 cursor = 0;
    int current_style = -1;
    int in_document_defaults = 0;
    int in_default_run_container = 0;
    int in_default_paragraph_container = 0;
    pw_zip_u8 *run_mask = NULL;
    pw_zip_u8 *run_value = NULL;
    pw_zip_u8 *paragraph_mask = NULL;
    pw_zip_u8 *paragraph_value = NULL;
    if (xml == NULL || styles == NULL) return PW_DOCX_STYLES_XML_ERROR;
    pw_docx_styles_init(styles);
    if (xml_bytes >= 3 && xml[0] == 0xef && xml[1] == 0xbb && xml[2] == 0xbf) cursor = 3;
    while (cursor < xml_bytes) {
        int closing = 0;
        int self_closing = 0;
        pw_zip_u32 name_start;
        pw_zip_u32 name_end;
        pw_zip_u32 tag_end;
        if (xml[cursor] != '<') { ++cursor; continue; }
        if (cursor + 4 <= xml_bytes && memcmp(xml + cursor, "<!--", 4) == 0) {
            cursor += 4;
            while (cursor + 3 <= xml_bytes && memcmp(xml + cursor, "-->", 3) != 0) ++cursor;
            if (cursor + 3 > xml_bytes) goto malformed;
            cursor += 3;
            continue;
        }
        if (cursor + 2 <= xml_bytes && xml[cursor + 1] == '?') {
            cursor += 2;
            while (cursor + 1 < xml_bytes && !(xml[cursor] == '?' && xml[cursor + 1] == '>')) ++cursor;
            if (cursor + 1 >= xml_bytes) goto malformed;
            cursor += 2;
            continue;
        }
        if (cursor + 2 <= xml_bytes && xml[cursor + 1] == '!') goto malformed;
        if (cursor + 1 < xml_bytes && xml[cursor + 1] == '/') { closing = 1; ++cursor; }
        ++cursor;
        name_start = cursor;
        if (cursor >= xml_bytes || !is_name_start(xml[cursor])) goto malformed;
        while (cursor < xml_bytes && is_name_char(xml[cursor])) ++cursor;
        name_end = cursor;
        while (cursor < xml_bytes && xml[cursor] != '>') {
            if (xml[cursor] == '\'' || xml[cursor] == '"') {
                pw_zip_u8 quote = xml[cursor++];
                while (cursor < xml_bytes && xml[cursor] != quote) ++cursor;
                if (cursor >= xml_bytes) goto malformed;
                ++cursor;
            } else ++cursor;
        }
        if (cursor >= xml_bytes) goto malformed;
        tag_end = cursor;
        {
            pw_zip_u32 end = tag_end;
            while (end > name_end && is_space(xml[end - 1])) --end;
            self_closing = end > name_end && xml[end - 1] == '/';
        }
        ++cursor;

        if (closing) {
            if (local_name_equals(xml + name_start, name_end - name_start, "rPr")) {
                run_mask = NULL; run_value = NULL;
            } else if (local_name_equals(xml + name_start, name_end - name_start, "pPr")) {
                paragraph_mask = NULL; paragraph_value = NULL;
            } else if (local_name_equals(xml + name_start, name_end - name_start, "style")) {
                current_style = -1;
            } else if (local_name_equals(xml + name_start, name_end - name_start, "rPrDefault")) {
                in_default_run_container = 0;
            } else if (local_name_equals(xml + name_start, name_end - name_start, "pPrDefault")) {
                in_default_paragraph_container = 0;
            } else if (local_name_equals(xml + name_start, name_end - name_start, "docDefaults")) {
                in_document_defaults = 0;
            }
            continue;
        }

        if (local_name_equals(xml + name_start, name_end - name_start, "docDefaults")) {
            in_document_defaults = 1;
        } else if (local_name_equals(xml + name_start, name_end - name_start, "rPrDefault") && in_document_defaults) {
            in_default_run_container = 1;
        } else if (local_name_equals(xml + name_start, name_end - name_start, "pPrDefault") && in_document_defaults) {
            in_default_paragraph_container = 1;
        } else if (local_name_equals(xml + name_start, name_end - name_start, "style")) {
            char id[DOCX_STYLE_VALUE_BYTES];
            char type[32];
            char default_value[16];
            int have_id, have_type, have_default;
            int add_result;
            pw_zip_u8 type_code;
            if (!get_attribute(xml, name_end, tag_end, "styleId", id, sizeof(id), &have_id) ||
                !get_attribute(xml, name_end, tag_end, "type", type, sizeof(type), &have_type) ||
                !get_attribute(xml, name_end, tag_end, "default", default_value, sizeof(default_value), &have_default)) goto malformed;
            if (!have_id || !have_type) goto malformed;
            type_code = style_type(type);
            add_result = add_style(styles, id, type_code,
                (pw_zip_u8)(have_default && value_is_true(default_value)), &current_style);
            if (add_result == PW_DOCX_STYLES_NO_MEMORY) goto no_memory;
            if (add_result != PW_DOCX_STYLES_OK) goto malformed;
        } else if (local_name_equals(xml + name_start, name_end - name_start, "basedOn") && current_style >= 0) {
            char parent[DOCX_STYLE_VALUE_BYTES];
            int found;
            char *copy;
            if (!get_attribute(xml, name_end, tag_end, "val", parent, sizeof(parent), &found)) goto malformed;
            if (found) {
                copy = copy_string(parent);
                if (copy == NULL) goto no_memory;
                free(styles->entries[current_style].based_on);
                styles->entries[current_style].based_on = copy;
            }
        } else if (local_name_equals(xml + name_start, name_end - name_start, "rPr")) {
            if (current_style >= 0) {
                run_mask = &styles->entries[current_style].run_mask;
                run_value = &styles->entries[current_style].run_value;
            } else if (in_document_defaults && in_default_run_container) {
                run_mask = &styles->default_run_mask;
                run_value = &styles->default_run_value;
            }
        } else if (local_name_equals(xml + name_start, name_end - name_start, "pPr")) {
            if (current_style >= 0) {
                paragraph_mask = &styles->entries[current_style].paragraph_mask;
                paragraph_value = &styles->entries[current_style].paragraph_value;
            } else if (in_document_defaults && in_default_paragraph_container) {
                paragraph_mask = &styles->default_paragraph_mask;
                paragraph_value = &styles->default_paragraph_value;
            }
        } else if (run_mask != NULL && local_name_equals(xml + name_start, name_end - name_start, "b")) {
            *run_mask |= 1;
            if (tag_enabled(xml, name_end, tag_end)) *run_value |= 1;
            else *run_value &= (pw_zip_u8)~1;
        } else if (run_mask != NULL && local_name_equals(xml + name_start, name_end - name_start, "i")) {
            *run_mask |= 2;
            if (tag_enabled(xml, name_end, tag_end)) *run_value |= 2;
            else *run_value &= (pw_zip_u8)~2;
        } else if (run_mask != NULL && local_name_equals(xml + name_start, name_end - name_start, "u")) {
            *run_mask |= 4;
            if (tag_enabled(xml, name_end, tag_end)) *run_value |= 4;
            else *run_value &= (pw_zip_u8)~4;
        } else if (paragraph_mask != NULL && local_name_equals(xml + name_start, name_end - name_start, "jc")) {
            char alignment[32];
            int found;
            if (!get_attribute(xml, name_end, tag_end, "val", alignment, sizeof(alignment), &found)) goto malformed;
            if (found) {
                *paragraph_mask |= 24;
                *paragraph_value &= (pw_zip_u8)~24;
                if (strcmp(alignment, "center") == 0) *paragraph_value |= 8;
                else if (strcmp(alignment, "right") == 0 || strcmp(alignment, "end") == 0) *paragraph_value |= 16;
                else if (strcmp(alignment, "both") == 0 || strcmp(alignment, "distribute") == 0) *paragraph_value |= 24;
            }
        }

        if (self_closing) {
            if (local_name_equals(xml + name_start, name_end - name_start, "rPr")) { run_mask = NULL; run_value = NULL; }
            else if (local_name_equals(xml + name_start, name_end - name_start, "pPr")) { paragraph_mask = NULL; paragraph_value = NULL; }
            else if (local_name_equals(xml + name_start, name_end - name_start, "style")) current_style = -1;
            else if (local_name_equals(xml + name_start, name_end - name_start, "rPrDefault")) in_default_run_container = 0;
            else if (local_name_equals(xml + name_start, name_end - name_start, "pPrDefault")) in_default_paragraph_container = 0;
            else if (local_name_equals(xml + name_start, name_end - name_start, "docDefaults")) in_document_defaults = 0;
        }
    }
    return PW_DOCX_STYLES_OK;

no_memory:
    pw_docx_styles_dispose(styles);
    return PW_DOCX_STYLES_NO_MEMORY;
malformed:
    pw_docx_styles_dispose(styles);
    return PW_DOCX_STYLES_XML_ERROR;
}

static int find_style(const PwDocxStyles *styles, const char *id)
{
    unsigned i;
    if (id == NULL || id[0] == '\0') return -1;
    for (i = 0; i < styles->count; ++i)
        if (strcmp(styles->entries[i].id, id) == 0) return (int)i;
    return -1;
}

static int apply_style_chain(const PwDocxStyles *styles, const char *id,
                             pw_zip_u8 expected_type,
                             pw_zip_u8 *run_value,
                             pw_zip_u8 *paragraph_value)
{
    unsigned *chain;
    unsigned chain_count = 0;
    int index;
    unsigned i;
    if (id == NULL || id[0] == '\0' || styles->count == 0) return 1;
    if (styles->count > 0x7fffffffUL / sizeof(*chain)) return 0;
    chain = (unsigned *)malloc(styles->count * sizeof(*chain));
    if (chain == NULL) return 0;
    index = find_style(styles, id);
    while (index >= 0 && chain_count < styles->count) {
        PwDocxStyleEntry *entry = &styles->entries[index];
        int parent;
        if (entry->type != expected_type) break;
        for (i = 0; i < chain_count; ++i) if (chain[i] == (unsigned)index) {
            free(chain);
            return 0;
        }
        chain[chain_count++] = (unsigned)index;
        parent = find_style(styles, entry->based_on);
        if (parent < 0 || styles->entries[parent].type != expected_type) break;
        index = parent;
    }
    while (chain_count != 0) {
        PwDocxStyleEntry *entry = &styles->entries[chain[--chain_count]];
        *run_value = (pw_zip_u8)((*run_value & (pw_zip_u8)~entry->run_mask) |
                                 (entry->run_value & entry->run_mask));
        if (paragraph_value != NULL)
            *paragraph_value = (pw_zip_u8)((*paragraph_value & (pw_zip_u8)~entry->paragraph_mask) |
                                           (entry->paragraph_value & entry->paragraph_mask));
    }
    free(chain);
    return 1;
}

int pw_docx_styles_paragraph(const PwDocxStyles *styles,
                             const char *style_id,
                             pw_zip_u8 *run_style,
                             pw_zip_u8 *paragraph_alignment)
{
    const char *selected;
    if (styles == NULL || run_style == NULL || paragraph_alignment == NULL) return 0;
    *run_style = styles->default_run_value & styles->default_run_mask;
    *paragraph_alignment = styles->default_paragraph_value & styles->default_paragraph_mask;
    selected = style_id != NULL && style_id[0] != '\0' ? style_id : styles->default_paragraph_id;
    return apply_style_chain(styles, selected, STYLE_PARAGRAPH,
                             run_style, paragraph_alignment);
}

int pw_docx_styles_character(const PwDocxStyles *styles,
                             const char *style_id,
                             pw_zip_u8 *run_style)
{
    if (styles == NULL || run_style == NULL) return 0;
    return apply_style_chain(styles, style_id, STYLE_CHARACTER, run_style, NULL);
}
