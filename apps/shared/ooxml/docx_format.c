#include "docx_format.h"

#include "xml_buffer.h"
#include "zip32.h"

#include <stdio.h>
#include <string.h>

static const char g_content_types[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
    "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
    "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
    "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
    "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
    "<Override PartName=\"/word/numbering.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>"
    "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
    "</Types>";

static const char g_package_relationships[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
    "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>"
    "</Relationships>";

static const char g_styles[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
    "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Arial\" w:hAnsi=\"Arial\" w:eastAsia=\"Arial\"/>"
    "<w:sz w:val=\"24\"/><w:szCs w:val=\"24\"/></w:rPr></w:rPrDefault>"
    "<w:pPrDefault><w:pPr><w:spacing w:after=\"120\"/></w:pPr></w:pPrDefault></w:docDefaults>"
    "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/>"
    "<w:qFormat/></w:style></w:styles>";

static const char g_document_relationships[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
    "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering\" Target=\"numbering.xml\"/>"
    "</Relationships>";

static int append_numbering_xml(PwXmlBuffer *numbering,
                                pw_docx_u32 decimal_list_count,
                                pw_docx_u32 bullet_list_count)
{
    pw_docx_u32 index;
    if (!pw_xml_append_ascii(numbering,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:abstractNum w:abstractNumId=\"0\"><w:multiLevelType w:val=\"singleLevel\"/>"
        "<w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/>"
        "<w:lvlText w:val=\"%1.\"/><w:lvlJc w:val=\"left\"/>"
        "<w:pPr><w:tabs><w:tab w:val=\"num\" w:pos=\"720\"/></w:tabs>"
        "<w:ind w:left=\"720\" w:hanging=\"360\"/></w:pPr></w:lvl></w:abstractNum>"
        "<w:abstractNum w:abstractNumId=\"1\"><w:multiLevelType w:val=\"singleLevel\"/>"
        "<w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt w:val=\"bullet\"/>"
        "<w:lvlText w:val=\"&#x2022;\"/><w:lvlJc w:val=\"left\"/>"
        "<w:pPr><w:tabs><w:tab w:val=\"num\" w:pos=\"720\"/></w:tabs>"
        "<w:ind w:left=\"720\" w:hanging=\"360\"/></w:pPr>"
        "<w:rPr><w:rFonts w:ascii=\"Symbol\" w:hAnsi=\"Symbol\"/></w:rPr>"
        "</w:lvl></w:abstractNum>")) return 0;
    for (index = 1; index <= decimal_list_count; ++index) {
        char entry[80];
        int length = sprintf(entry,
            "<w:num w:numId=\"%lu\"><w:abstractNumId w:val=\"0\"/></w:num>",
            (unsigned long)index);
        if (length <= 0 || !pw_xml_append(numbering, entry, (pw_docx_u32)length)) return 0;
    }
    for (index = 1; index <= bullet_list_count; ++index) {
        char entry[80];
        int length = sprintf(entry,
            "<w:num w:numId=\"%lu\"><w:abstractNumId w:val=\"1\"/></w:num>",
            (unsigned long)(decimal_list_count + index));
        if (length <= 0 || !pw_xml_append(numbering, entry, (pw_docx_u32)length)) return 0;
    }
    return pw_xml_append_ascii(numbering, "</w:numbering>");
}

static const char g_core_properties[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
    "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" "
    "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
    "<dc:creator>Panthera Softwares</dc:creator><cp:lastModifiedBy>Panthera Notary</cp:lastModifiedBy>"
    "</cp:coreProperties>";

static pw_docx_u32 utf8_width(const pw_docx_u8 *text, pw_docx_u32 end,
                              pw_docx_u32 position)
{
    pw_docx_u8 lead = text[position];
    if (lead < 0x80) return 1;
    if ((lead & 0xe0) == 0xc0 && position + 1 < end) return 2;
    if ((lead & 0xf0) == 0xe0 && position + 2 < end) return 3;
    if ((lead & 0xf8) == 0xf0 && position + 3 < end) return 4;
    return 1;
}

static int append_run(PwXmlBuffer *document, const pw_docx_u8 *text,
                      pw_docx_u32 start, pw_docx_u32 bytes, pw_docx_u8 style)
{
    if (style & 7) {
        if (!pw_xml_append_ascii(document, "<w:r><w:rPr>")) return 0;
        if ((style & 1) && !pw_xml_append_ascii(document, "<w:b/>")) return 0;
        if ((style & 2) && !pw_xml_append_ascii(document, "<w:i/>")) return 0;
        if ((style & 4) && !pw_xml_append_ascii(document, "<w:u w:val=\"single\"/>")) return 0;
        if (!pw_xml_append_ascii(document, "</w:rPr><w:t xml:space=\"preserve\">")) return 0;
    } else if (!pw_xml_append_ascii(document, "<w:r><w:t xml:space=\"preserve\">")) return 0;
    return pw_xml_append_text(document, text + start, bytes) &&
           pw_xml_append_ascii(document, "</w:t></w:r>");
}

static int append_paragraph(PwXmlBuffer *document, const pw_docx_u8 *text,
                            const pw_docx_u8 *character_style,
                            pw_docx_u32 start, pw_docx_u32 end,
                            pw_docx_u8 alignment, pw_docx_u32 num_id)
{
    pw_docx_u32 cursor = start;
    if (!pw_xml_append_ascii(document,
            "<w:p><w:pPr><w:pStyle w:val=\"Normal\"/>")) return 0;
    if (alignment == 8 && !pw_xml_append_ascii(document, "<w:jc w:val=\"center\"/>")) return 0;
    if (alignment == 16 && !pw_xml_append_ascii(document, "<w:jc w:val=\"right\"/>")) return 0;
    if (alignment == 24 && !pw_xml_append_ascii(document, "<w:jc w:val=\"both\"/>")) return 0;
    if (num_id != 0) {
        char num_pr[96];
        int length = sprintf(num_pr,
            "<w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"%lu\"/></w:numPr>",
            (unsigned long)num_id);
        if (length <= 0 || !pw_xml_append(document, num_pr, (pw_docx_u32)length)) return 0;
        if (!pw_xml_append_ascii(document, "<w:ind w:left=\"720\" w:hanging=\"360\"/>")) return 0;
    }
    if (!pw_xml_append_ascii(document, "</w:pPr>")) return 0;
    while (cursor < end) {
        pw_docx_u32 run_start = cursor;
        pw_docx_u8 style = character_style != NULL ? (pw_docx_u8)(character_style[cursor] & PW_DOCX_STYLE_CHARACTER_MASK) : 0;
        cursor += utf8_width(text, end, cursor);
        while (cursor < end &&
               (character_style != NULL ? (character_style[cursor] & PW_DOCX_STYLE_CHARACTER_MASK) : 0) == style) {
            cursor += utf8_width(text, end, cursor);
        }
        if (!append_run(document, text, run_start, cursor - run_start, style)) return 0;
    }
    return pw_xml_append_ascii(document, "</w:p>");
}

static int build_document_xml(PwXmlBuffer *document,
                              const pw_docx_u8 *text,
                              const pw_docx_u8 *character_style,
                              pw_docx_u32 bytes,
                              pw_docx_u32 decimal_list_count)
{
    pw_docx_u32 line_start = 0;
    pw_docx_u32 cursor = 0;
    pw_docx_u32 num_id = 0;
    pw_docx_u32 decimal_id = 0;
    pw_docx_u32 bullet_id = 0;
    pw_docx_u32 active_num_id = 0;
    pw_docx_u8 previous_list_kind = 0;
    if (!pw_xml_append_ascii(document,
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
            "<w:body>")) return 0;

    while (cursor <= bytes) {
        if (cursor == bytes || text[cursor] == '\n' || text[cursor] == '\r') {
            pw_docx_u8 alignment = 0;
            pw_docx_u8 list_kind = character_style != NULL && line_start < bytes ?
                (pw_docx_u8)(character_style[line_start] & PW_DOCX_STYLE_LIST_MASK) : 0;
            if (list_kind != previous_list_kind) {
                if (list_kind == PW_DOCX_STYLE_NUMBERED) active_num_id = ++decimal_id;
                else if (list_kind == PW_DOCX_STYLE_BULLETED)
                    active_num_id = decimal_list_count + ++bullet_id;
                else active_num_id = 0;
            }
            if (character_style != NULL) {
                if (line_start < cursor) alignment = (pw_docx_u8)(character_style[line_start] & PW_DOCX_STYLE_PARAGRAPH_MASK);
                else if (cursor < bytes) alignment = (pw_docx_u8)(character_style[cursor] & PW_DOCX_STYLE_PARAGRAPH_MASK);
            }
            if (!append_paragraph(document, text, character_style,
                                  line_start, cursor, alignment, active_num_id)) return 0;
            previous_list_kind = list_kind;
            if (cursor == bytes) break;
            if (text[cursor] == '\r' && cursor + 1 < bytes && text[cursor + 1] == '\n') ++cursor;
            ++cursor;
            line_start = cursor;
        } else ++cursor;
    }

    return pw_xml_append_ascii(document,
        "<w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/>"
        "<w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" "
        "w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/></w:sectPr>"
        "</w:body></w:document>");
}

static void add_literal(PwZip32Entry *entry, const char *name, const char *text)
{
    entry->name = name;
    entry->data = (const pw_zip_u8 *)text;
    entry->size = (pw_zip_u32)strlen(text);
}

int pw_docx_write_rich(const char *path, const pw_docx_u8 *utf8,
                       const pw_docx_u8 *character_style, pw_docx_u32 bytes)
{
    PwXmlBuffer document;
    PwXmlBuffer numbering;
    PwZip32Entry entries[7];
    pw_docx_u32 cursor;
    pw_docx_u32 line_start = 0;
    pw_docx_u32 decimal_list_count = 0;
    pw_docx_u32 bullet_list_count = 0;
    pw_docx_u8 previous_list_kind = 0;
    int result;
    if (path == NULL || path[0] == 0 || (utf8 == NULL && bytes != 0)) return PW_DOCX_BAD_ARGUMENT;
    pw_xml_buffer_init(&document);
    pw_xml_buffer_init(&numbering);
    if (character_style != NULL) {
        for (cursor = 0; cursor <= bytes; ++cursor) {
            if (cursor == bytes || utf8[cursor] == '\n' || utf8[cursor] == '\r') {
                pw_docx_u8 list_kind = line_start < bytes ?
                    (pw_docx_u8)(character_style[line_start] & PW_DOCX_STYLE_LIST_MASK) : 0;
                if (list_kind != previous_list_kind) {
                    if (list_kind == PW_DOCX_STYLE_NUMBERED) ++decimal_list_count;
                    else if (list_kind == PW_DOCX_STYLE_BULLETED) ++bullet_list_count;
                }
                previous_list_kind = list_kind;
                if (cursor < bytes && utf8[cursor] == '\r' &&
                    cursor + 1 < bytes && utf8[cursor + 1] == '\n') ++cursor;
                line_start = cursor + 1;
            }
        }
    }
    if (!append_numbering_xml(&numbering, decimal_list_count, bullet_list_count)) {
        int code = numbering.failed ? PW_DOCX_NO_MEMORY : PW_DOCX_XML_ERROR;
        pw_xml_buffer_dispose(&numbering);
        pw_xml_buffer_dispose(&document);
        return code;
    }
    if (!build_document_xml(&document, utf8, character_style, bytes, decimal_list_count)) {
        int code = document.invalid_text ? PW_DOCX_XML_ERROR : PW_DOCX_NO_MEMORY;
        pw_xml_buffer_dispose(&numbering);
        pw_xml_buffer_dispose(&document);
        return code;
    }

    add_literal(&entries[0], "[Content_Types].xml", g_content_types);
    add_literal(&entries[1], "_rels/.rels", g_package_relationships);
    entries[2].name = "word/document.xml";
    entries[2].data = document.data;
    entries[2].size = document.length;
    add_literal(&entries[3], "word/styles.xml", g_styles);
    add_literal(&entries[4], "word/_rels/document.xml.rels", g_document_relationships);
    add_literal(&entries[5], "docProps/core.xml", g_core_properties);
    entries[6].name = "word/numbering.xml";
    entries[6].data = numbering.data;
    entries[6].size = numbering.length;

    result = pw_zip32_write_store(path, entries, 7);
    pw_xml_buffer_dispose(&numbering);
    pw_xml_buffer_dispose(&document);
    if (result == PW_ZIP32_OK) return PW_DOCX_OK;
    if (result == PW_ZIP32_NO_MEMORY) return PW_DOCX_NO_MEMORY;
    if (result == PW_ZIP32_IO_ERROR) return PW_DOCX_PACKAGE_ERROR;
    return PW_DOCX_BAD_ARGUMENT;
}

int pw_docx_write_text(const char *path, const pw_docx_u8 *utf8, pw_docx_u32 bytes)
{
    return pw_docx_write_rich(path, utf8, NULL, bytes);
}
