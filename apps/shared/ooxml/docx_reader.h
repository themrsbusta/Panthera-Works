#ifndef PANTHERA_OOXML_DOCX_READER_H
#define PANTHERA_OOXML_DOCX_READER_H

#include "zip32_reader.h"

enum {
    PW_DOCX_READ_OK = 0,
    PW_DOCX_READ_BAD_ARGUMENT = 1,
    PW_DOCX_READ_NO_MEMORY = 2,
    PW_DOCX_READ_XML_ERROR = 3,
    PW_DOCX_READ_PACKAGE_ERROR = 4,
    PW_DOCX_READ_TOO_LARGE = 5
};

/* Read direct run styles and paragraph alignment into byte-parallel flags. */
int pw_docx_read_rich(const char *path,
                      pw_zip_u32 max_xml_bytes,
                      pw_zip_u32 max_text_bytes,
                      PwZip32Buffer *utf8_text,
                      PwZip32Buffer *character_style);

/* Import body text as UTF-8 and discard direct run styling. */
int pw_docx_read_text(const char *path,
                      pw_zip_u32 max_xml_bytes,
                      pw_zip_u32 max_text_bytes,
                      PwZip32Buffer *utf8_text);

#endif
