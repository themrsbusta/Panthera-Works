#include "../ooxml/docx_format.h"
#include "../ooxml/docx_reader.h"
#include "../ooxml/xml_buffer.h"
#include "../ooxml/zip32_reader.h"

#include <stdio.h>
#include <string.h>

static int contains_ascii(const pw_zip_u8 *data, pw_zip_u32 bytes,
                          const char *needle)
{
    pw_zip_u32 index;
    pw_zip_u32 needle_bytes = (pw_zip_u32)strlen(needle);
    if (needle_bytes > bytes) return 0;
    for (index = 0; index <= bytes - needle_bytes; ++index)
        if (memcmp(data + index, needle, needle_bytes) == 0) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    static const pw_xml_u8 text[] = "Panthera & <Works>\nDOCX test: caf\xc3\xa9\nDefault paragraph\n";
    static const pw_docx_u8 rich_text[] =
        "Bold Italic Underline\nRight Align\nJustified\nLeft";
    static const pw_docx_u8 list_text[] = "One\nTwo\nBullet One\nBullet Two\nOutside\nRestart\nPlain";
    static const pw_xml_u8 xml_input[] = "A&B <C> \"quoted\" 'text'";
    static const char xml_expected[] = "A&amp;B &lt;C&gt; &quot;" "quoted&quot; &apos;text&apos;";
    static const pw_docx_u8 invalid_utf8[] = {0xc0, 0x80};
    PwXmlBuffer escaped;
    PwXmlBuffer rejected;
    PwZip32Buffer imported;
    PwZip32Buffer imported_style;
    pw_docx_u8 rich_style[sizeof(rich_text) - 1];
    pw_docx_u8 list_style[sizeof(list_text) - 1];
    static const pw_docx_u8 paragraph_alignment[] = {8, 16, 24, 0};
    pw_docx_u8 external_style[sizeof(text) - 1];
    unsigned external_paragraph = 0;
    unsigned list_paragraph = 0;
    int result;
    unsigned i;
    unsigned paragraph = 0;

    memset(list_style, 0, sizeof(list_style));
    for (i = 0; i < sizeof(list_style); ++i) {
        if (list_paragraph < 2 || list_paragraph == 5)
            list_style[i] = PW_DOCX_STYLE_NUMBERED;
        else if (list_paragraph == 2 || list_paragraph == 3)
            list_style[i] = PW_DOCX_STYLE_BULLETED;
        if (list_text[i] == '\n') ++list_paragraph;
    }
    result = pw_docx_write_rich("LIST.ZIP", list_text, list_style, sizeof(list_text) - 1);
    if (result != PW_DOCX_OK) {
        fprintf(stderr, "FAIL: numbered-list DOCX writer returned %d\n", result);
        return 10;
    }
    imported.data = NULL;
    imported.size = 0;
    imported_style.data = NULL;
    imported_style.size = 0;
    result = pw_docx_read_rich("LIST.ZIP", 1024UL * 1024UL,
                               1024UL * 1024UL, &imported, &imported_style);
    if (result != PW_DOCX_READ_OK || imported.size != sizeof(list_text) - 1 ||
        imported_style.size != sizeof(list_style) ||
        memcmp(imported.data, list_text, sizeof(list_text) - 1) != 0 ||
        memcmp(imported_style.data, list_style, sizeof(list_style)) != 0) {
        fprintf(stderr, "FAIL: numbered-list rich round trip returned %d, text=%lu style=%lu\n",
                result, (unsigned long)imported.size, (unsigned long)imported_style.size);
        pw_zip32_buffer_dispose(&imported);
        pw_zip32_buffer_dispose(&imported_style);
        return 11;
    }
    pw_zip32_buffer_dispose(&imported);
    pw_zip32_buffer_dispose(&imported_style);

    {
        PwZip32Buffer numbering_xml;
        numbering_xml.data = NULL;
        numbering_xml.size = 0;
        result = pw_zip32_read_member("LIST.ZIP", "word/numbering.xml",
                                      1024UL * 1024UL, &numbering_xml);
        if (result != PW_ZIP32_OK || numbering_xml.data == NULL ||
            !contains_ascii(numbering_xml.data, numbering_xml.size, "w:val=\"decimal\"") ||
            !contains_ascii(numbering_xml.data, numbering_xml.size, "w:val=\"bullet\"") ||
            !contains_ascii(numbering_xml.data, numbering_xml.size,
                            "w:abstractNumId w:val=\"1\"")) {
            fprintf(stderr, "FAIL: mixed list numbering definitions returned %d\n", result);
            pw_zip32_buffer_dispose(&numbering_xml);
            return 15;
        }
        pw_zip32_buffer_dispose(&numbering_xml);
    }

    pw_xml_buffer_init(&escaped);
    if (!pw_xml_append_text(&escaped, xml_input, sizeof(xml_input) - 1) ||
        escaped.length != (pw_xml_u32)strlen(xml_expected) ||
        memcmp(escaped.data, xml_expected, escaped.length) != 0) {
        fputs("FAIL: XML escaping\n", stderr);
        pw_xml_buffer_dispose(&escaped);
        return 1;
    }
    pw_xml_buffer_dispose(&escaped);

    pw_xml_buffer_init(&rejected);
    if (pw_xml_append_text(&rejected, invalid_utf8, sizeof(invalid_utf8)) ||
        !rejected.invalid_text) {
        fputs("FAIL: invalid UTF-8 accepted as XML text\n", stderr);
        pw_xml_buffer_dispose(&rejected);
        return 2;
    }
    pw_xml_buffer_dispose(&rejected);

    result = pw_docx_write_text("NOTARY.ZIP", text, sizeof(text) - 1);
    if (result != PW_DOCX_OK) {
        fprintf(stderr, "FAIL: DOCX package writer returned %d\n", result);
        return 3;
    }
    imported.data = NULL;
    imported.size = 0;
    result = pw_docx_read_text("NOTARY.ZIP", 1024UL * 1024UL,
                               1024UL * 1024UL, &imported);
    if (result != PW_DOCX_READ_OK || imported.size != sizeof(text) - 1 ||
        memcmp(imported.data, text, sizeof(text) - 1) != 0) {
        fprintf(stderr, "FAIL: DOCX store round trip returned %d, size %lu\n",
                result, (unsigned long)imported.size);
        pw_zip32_buffer_dispose(&imported);
        return 5;
    }
    pw_zip32_buffer_dispose(&imported);

    memset(rich_style, 0, sizeof(rich_style));
    memset(rich_style, 1, 4);
    memset(rich_style + 5, 2, 6);
    memset(rich_style + 12, 4, 9);
    for (i = 0; i < sizeof(rich_style); ++i) {
        rich_style[i] |= paragraph_alignment[paragraph];
        if (rich_text[i] == '\n') ++paragraph;
    }
    result = pw_docx_write_rich("RICH.ZIP", rich_text, rich_style, sizeof(rich_text) - 1);
    if (result != PW_DOCX_OK) {
        fprintf(stderr, "FAIL: rich DOCX writer returned %d\n", result);
        return 7;
    }
    imported.data = NULL;
    imported.size = 0;
    imported_style.data = NULL;
    imported_style.size = 0;
    result = pw_docx_read_rich("RICH.ZIP", 1024UL * 1024UL,
                               1024UL * 1024UL, &imported, &imported_style);
    if (result != PW_DOCX_READ_OK || imported.size != sizeof(rich_text) - 1 ||
        imported_style.size != sizeof(rich_style) ||
        memcmp(imported.data, rich_text, sizeof(rich_text) - 1) != 0 ||
        memcmp(imported_style.data, rich_style, sizeof(rich_style)) != 0) {
        fprintf(stderr, "FAIL: rich DOCX round trip returned %d, text=%lu style=%lu\n",
                result, (unsigned long)imported.size, (unsigned long)imported_style.size);
        pw_zip32_buffer_dispose(&imported);
        pw_zip32_buffer_dispose(&imported_style);
        return 8;
    }
    pw_zip32_buffer_dispose(&imported);
    pw_zip32_buffer_dispose(&imported_style);

    if (argc > 1 && argv[1] != NULL) {
        imported.data = NULL;
        imported.size = 0;
        imported_style.data = NULL;
        imported_style.size = 0;
        for (i = 0; i < sizeof(external_style); ++i) {
            if (text[i] == '\n') {
                external_style[i] = external_paragraph < 2 ? 16 : 8;
                ++external_paragraph;
            } else if (external_paragraph == 0) external_style[i] = 20;
            else if (external_paragraph == 1) external_style[i] = 17;
            else external_style[i] = 10;
        }
        result = pw_docx_read_rich(argv[1], 1024UL * 1024UL,
                                   1024UL * 1024UL, &imported, &imported_style);
        if (result != PW_DOCX_READ_OK || imported.size != sizeof(text) - 1 ||
            imported_style.size != sizeof(external_style) ||
            memcmp(imported.data, text, sizeof(text) - 1) != 0 ||
            memcmp(imported_style.data, external_style, sizeof(external_style)) != 0) {
            fprintf(stderr, "FAIL: external DOCX read returned %d, size %lu\n",
                    result, (unsigned long)imported.size);
            pw_zip32_buffer_dispose(&imported);
            pw_zip32_buffer_dispose(&imported_style);
            return 6;
        }
        pw_zip32_buffer_dispose(&imported);
        pw_zip32_buffer_dispose(&imported_style);
    }
    if (argc > 2 && argv[2] != NULL) {
        imported.data = NULL;
        imported.size = 0;
        result = pw_docx_read_text(argv[2], 1024UL * 1024UL,
                                   1024UL * 1024UL, &imported);
        if (result != PW_DOCX_READ_PACKAGE_ERROR || imported.data != NULL || imported.size != 0) {
            fprintf(stderr, "FAIL: package traversal target returned %d, size %lu\n",
                    result, (unsigned long)imported.size);
            pw_zip32_buffer_dispose(&imported);
            return 9;
        }
    }
    if (argc > 3 && argv[3] != NULL) {
        static const pw_docx_u8 numbered_text[] = "Decimal\nBullet";
        pw_docx_u8 numbered_style[sizeof(numbered_text) - 1];
        PwZip32Buffer member;
        memset(numbered_style, 0, sizeof(numbered_style));
        memset(numbered_style, 0, sizeof(numbered_style));
        memset(numbered_style, PW_DOCX_STYLE_NUMBERED, 8);
        memset(numbered_style + 8, PW_DOCX_STYLE_BULLETED,
               sizeof(numbered_style) - 8);
        member.data = NULL;
        member.size = 0;
        result = pw_zip32_read_member(argv[3], "custom/_rels/main.xml.rels",
                                      1024UL * 1024UL, &member);
        if (result != PW_ZIP32_OK) {
            printf("FAIL: numbering relationship member read returned %d\n", result);
            pw_zip32_buffer_dispose(&member);
            return 13;
        }
        pw_zip32_buffer_dispose(&member);
        member.data = NULL;
        member.size = 0;
        result = pw_zip32_read_member(argv[3], "custom/numbering.xml",
                                      1024UL * 1024UL, &member);
        if (result != PW_ZIP32_OK) {
            printf("FAIL: numbering definition member read returned %d\n", result);
            pw_zip32_buffer_dispose(&member);
            return 14;
        }
        pw_zip32_buffer_dispose(&member);
        imported.data = NULL;
        imported.size = 0;
        imported_style.data = NULL;
        imported_style.size = 0;
        result = pw_docx_read_rich(argv[3], 1024UL * 1024UL,
                                   1024UL * 1024UL, &imported, &imported_style);
        if (result != PW_DOCX_READ_OK || imported.size != sizeof(numbered_text) - 1 ||
            imported_style.size != sizeof(numbered_style) ||
            memcmp(imported.data, numbered_text, sizeof(numbered_text) - 1) != 0 ||
            memcmp(imported_style.data, numbered_style, sizeof(numbered_style)) != 0) {
            printf("FAIL: external decimal/bullet numbering discrimination returned %d, text=%lu style=%lu\n",
                   result, (unsigned long)imported.size, (unsigned long)imported_style.size);
            pw_zip32_buffer_dispose(&imported);
            pw_zip32_buffer_dispose(&imported_style);
            return 12;
        }
        pw_zip32_buffer_dispose(&imported);
        pw_zip32_buffer_dispose(&imported_style);
    }
    result = pw_docx_write_text("BAD.ZIP", invalid_utf8, sizeof(invalid_utf8));
    if (result != PW_DOCX_XML_ERROR) {
        fprintf(stderr, "FAIL: invalid XML input returned %d\n", result);
        return 4;
    }
    puts("PASS: DOCX package/style relationships, basedOn inheritance, formatting precedence, decimal/bullet list round trips, numbering-type discrimination, traversal rejection and STORE/DEFLATE.");
    return 0;
}
