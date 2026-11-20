#include "../ooxml/zip32.h"

#include <stdio.h>
#include <string.h>

static const pw_zip_u8 g_types[] =
    "<?xml version=\"1.0\"?><Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"/>";
static const pw_zip_u8 g_relationships[] =
    "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"/>";
static const pw_zip_u8 g_document[] =
    "<?xml version=\"1.0\"?><w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body><w:p><w:r><w:t>Panthera</w:t></w:r></w:p></w:body></w:document>";

int main(int argc, char **argv)
{
    static const pw_zip_u8 crc_input[] = "123456789";
    static const PwZip32Entry entries[] = {
        {"[Content_Types].xml", g_types, sizeof(g_types) - 1},
        {"_rels/.rels", g_relationships, sizeof(g_relationships) - 1},
        {"word/document.xml", g_document, sizeof(g_document) - 1}
    };
    static const PwZip32Entry duplicate_entries[] = {
        {"same.xml", g_types, sizeof(g_types) - 1},
        {"same.xml", g_document, sizeof(g_document) - 1}
    };
    static const PwZip32Entry unsafe_entry[] = {
        {"../escape.xml", g_types, sizeof(g_types) - 1}
    };
    const char *output = argc > 1 ? argv[1] : "ZIPSMOK.ZIP";
    int result;

    if (pw_zip32_crc32(crc_input, sizeof(crc_input) - 1) != 0xcbf43926UL) {
        fputs("FAIL: CRC-32 vector\n", stderr);
        return 1;
    }
    if (pw_zip32_write_store(output, duplicate_entries, 2) != PW_ZIP32_BAD_ARGUMENT) {
        fputs("FAIL: duplicate package member accepted\n", stderr);
        return 2;
    }
    if (pw_zip32_write_store(output, unsafe_entry, 1) != PW_ZIP32_BAD_ARGUMENT) {
        fputs("FAIL: unsafe package member accepted\n", stderr);
        return 3;
    }
    result = pw_zip32_write_store(output, entries, 3);
    if (result != PW_ZIP32_OK) {
        fprintf(stderr, "FAIL: ZIP32 writer returned %d\n", result);
        return 4;
    }
    printf("PASS: CRC-32, member-name validation, and three-part ZIP32 creation: %s\n", output);
    return 0;
}
