#ifndef PANTHERA_OOXML_XML_BUFFER_H
#define PANTHERA_OOXML_XML_BUFFER_H

typedef unsigned char pw_xml_u8;
typedef unsigned long pw_xml_u32;

typedef struct {
    pw_xml_u8 *data;
    pw_xml_u32 length;
    pw_xml_u32 capacity;
    int failed;
    int invalid_text;
    int too_large;
} PwXmlBuffer;

void pw_xml_buffer_init(PwXmlBuffer *buffer);
void pw_xml_buffer_dispose(PwXmlBuffer *buffer);
int pw_xml_append(PwXmlBuffer *buffer, const void *bytes, pw_xml_u32 count);
int pw_xml_append_ascii(PwXmlBuffer *buffer, const char *text);
int pw_xml_append_text(PwXmlBuffer *buffer, const pw_xml_u8 *utf8, pw_xml_u32 count);

#endif
