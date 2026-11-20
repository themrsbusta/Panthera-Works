#ifndef PANTHERA_OOXML_DOCX_FORMAT_H
#define PANTHERA_OOXML_DOCX_FORMAT_H

typedef unsigned char pw_docx_u8;
typedef unsigned long pw_docx_u32;

enum {
    PW_DOCX_OK = 0,
    PW_DOCX_BAD_ARGUMENT = 1,
    PW_DOCX_NO_MEMORY = 2,
    PW_DOCX_XML_ERROR = 3,
    PW_DOCX_PACKAGE_ERROR = 4
};

enum {
    PW_DOCX_STYLE_BOLD = 1,
    PW_DOCX_STYLE_ITALIC = 2,
    PW_DOCX_STYLE_UNDERLINE = 4,
    PW_DOCX_STYLE_NUMBERED = 32,
    PW_DOCX_STYLE_BULLETED = 64,
    PW_DOCX_STYLE_CHARACTER_MASK = 7,
    PW_DOCX_STYLE_PARAGRAPH_MASK = 24,
    PW_DOCX_STYLE_LIST_MASK = 96
};

/* Write UTF-8 text with byte-parallel flags: bold=1, italic=2, underline=4,
   paragraph alignment in bits 3-4 (center=8, right=16, justify=24), and
   decimal-list paragraph membership in bit 5 (32) and bullet-list membership
   in bit 6 (64). */
int pw_docx_write_rich(const char *path, const pw_docx_u8 *utf8,
                       const pw_docx_u8 *character_style, pw_docx_u32 bytes);

/* Write plain UTF-8 document text as a basic WordprocessingML DOCX package. */
int pw_docx_write_text(const char *path, const pw_docx_u8 *utf8, pw_docx_u32 bytes);

#endif
