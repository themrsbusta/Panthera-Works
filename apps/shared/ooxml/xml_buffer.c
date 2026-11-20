#include "xml_buffer.h"

#include <stdlib.h>
#include <string.h>

#define PW_XML_MAX_BYTES 0x7ffffffeUL

void pw_xml_buffer_init(PwXmlBuffer *buffer)
{
    if (buffer == NULL) return;
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
    buffer->failed = 0;
    buffer->invalid_text = 0;
    buffer->too_large = 0;
}

void pw_xml_buffer_dispose(PwXmlBuffer *buffer)
{
    if (buffer == NULL) return;
    if (buffer->data != NULL) free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
    buffer->failed = 0;
    buffer->invalid_text = 0;
    buffer->too_large = 0;
}

static int reserve(PwXmlBuffer *buffer, pw_xml_u32 extra)
{
    pw_xml_u32 need;
    pw_xml_u32 capacity;
    pw_xml_u8 *next;
    if (buffer == NULL || buffer->failed) return 0;
    if (extra > PW_XML_MAX_BYTES - buffer->length) {
        buffer->failed = 1;
        return 0;
    }
    need = buffer->length + extra + 1;
    if (need <= buffer->capacity) return 1;
    capacity = buffer->capacity == 0 ? 1024UL : buffer->capacity;
    while (capacity < need) {
        if (capacity > PW_XML_MAX_BYTES / 2) {
            capacity = need;
            break;
        }
        capacity *= 2;
    }
    next = (pw_xml_u8 *)realloc(buffer->data, capacity);
    if (next == NULL) {
        buffer->failed = 1;
        return 0;
    }
    buffer->data = next;
    buffer->capacity = capacity;
    if (buffer->length == 0) buffer->data[0] = 0;
    return 1;
}

int pw_xml_append(PwXmlBuffer *buffer, const void *bytes, pw_xml_u32 count)
{
    if (buffer == NULL || (bytes == NULL && count != 0) || !reserve(buffer, count)) return 0;
    if (count != 0) memcpy(buffer->data + buffer->length, bytes, count);
    buffer->length += count;
    buffer->data[buffer->length] = 0;
    return 1;
}

int pw_xml_append_ascii(PwXmlBuffer *buffer, const char *text)
{
    unsigned long length;
    if (text == NULL) return 0;
    length = (unsigned long)strlen(text);
    if (length > PW_XML_MAX_BYTES) return 0;
    return pw_xml_append(buffer, text, (pw_xml_u32)length);
}

static int utf8_next(const pw_xml_u8 *text, pw_xml_u32 bytes,
                     pw_xml_u32 *offset, pw_xml_u32 *codepoint,
                     pw_xml_u32 *sequence_bytes)
{
    pw_xml_u32 at = *offset;
    pw_xml_u8 first;
    pw_xml_u32 value;
    pw_xml_u32 count;
    pw_xml_u32 minimum;
    pw_xml_u32 index;
    if (at >= bytes) return 0;
    first = text[at];
    if (first < 0x80) { value = first; count = 1; minimum = 0; }
    else if ((first & 0xe0) == 0xc0) { value = first & 0x1f; count = 2; minimum = 0x80; }
    else if ((first & 0xf0) == 0xe0) { value = first & 0x0f; count = 3; minimum = 0x800; }
    else if ((first & 0xf8) == 0xf0) { value = first & 0x07; count = 4; minimum = 0x10000; }
    else return 0;
    if (count > bytes - at) return 0;
    for (index = 1; index < count; ++index) {
        pw_xml_u8 continuation = text[at + index];
        if ((continuation & 0xc0) != 0x80) return 0;
        value = (value << 6) | (continuation & 0x3f);
    }
    if ((count > 1 && value < minimum) || value > 0x10ffffUL ||
        (value >= 0xd800UL && value <= 0xdfffUL)) return 0;
    *offset = at + count;
    *codepoint = value;
    *sequence_bytes = count;
    return 1;
}

static int xml_character_allowed(pw_xml_u32 value)
{
    return value == 0x09UL || value == 0x0aUL || value == 0x0dUL ||
           (value >= 0x20UL && value <= 0xd7ffUL) ||
           (value >= 0xe000UL && value <= 0xfffdUL) ||
           (value >= 0x10000UL && value <= 0x10ffffUL);
}

int pw_xml_append_text(PwXmlBuffer *buffer, const pw_xml_u8 *utf8, pw_xml_u32 count)
{
    pw_xml_u32 offset = 0;
    if (buffer == NULL || (utf8 == NULL && count != 0)) return 0;
    while (offset < count) {
        pw_xml_u32 start = offset;
        pw_xml_u32 value;
        pw_xml_u32 sequence_bytes;
        const char *escape = NULL;
        if (!utf8_next(utf8, count, &offset, &value, &sequence_bytes) ||
            !xml_character_allowed(value)) {
            buffer->failed = 1;
            buffer->invalid_text = 1;
            return 0;
        }
        if (sequence_bytes == 1) {
            switch (value) {
                case '&': escape = "&amp;"; break;
                case '<': escape = "&lt;"; break;
                case '>': escape = "&gt;"; break;
                case '"': escape = "&quot;"; break;
                case '\'': escape = "&apos;"; break;
                default: break;
            }
        }
        if (escape != NULL) {
            if (!pw_xml_append_ascii(buffer, escape)) return 0;
        } else if (!pw_xml_append(buffer, utf8 + start, sequence_bytes)) return 0;
    }
    return 1;
}
