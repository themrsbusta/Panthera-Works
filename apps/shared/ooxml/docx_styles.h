#ifndef PANTHERA_OOXML_DOCX_STYLES_H
#define PANTHERA_OOXML_DOCX_STYLES_H

#include "zip32.h"

typedef struct PwDocxStyleEntry PwDocxStyleEntry;

typedef struct {
    PwDocxStyleEntry *entries;
    unsigned count;
    unsigned capacity;
    char *default_paragraph_id;
    pw_zip_u8 default_run_mask;
    pw_zip_u8 default_run_value;
    pw_zip_u8 default_paragraph_mask;
    pw_zip_u8 default_paragraph_value;
} PwDocxStyles;

enum {
    PW_DOCX_STYLES_OK = 0,
    PW_DOCX_STYLES_XML_ERROR = 1,
    PW_DOCX_STYLES_NO_MEMORY = 2
};

void pw_docx_styles_init(PwDocxStyles *styles);
void pw_docx_styles_dispose(PwDocxStyles *styles);
int pw_docx_styles_parse(const pw_zip_u8 *xml, pw_zip_u32 xml_bytes,
                         PwDocxStyles *styles);
int pw_docx_styles_paragraph(const PwDocxStyles *styles,
                             const char *style_id,
                             pw_zip_u8 *run_style,
                             pw_zip_u8 *paragraph_alignment);
int pw_docx_styles_character(const PwDocxStyles *styles,
                             const char *style_id,
                             pw_zip_u8 *run_style);

#endif
