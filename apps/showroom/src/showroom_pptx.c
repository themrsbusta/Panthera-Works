#include "showroom_pptx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../shared/ooxml/xml_buffer.h"
#include "../../shared/ooxml/zip32.h"

#define SH_PPTX_ENTRIES (9 + SH_MAX_SLIDES * 2)
#define SH_PPTX_PATH_MAX 48

typedef struct {
    char *name;
    PwXmlBuffer xml;
} ShPptxPart;


static const unsigned short cp437_unicode[128] = {
    0x00c7,0x00fc,0x00e9,0x00e2,0x00e4,0x00e0,0x00e5,0x00e7,
    0x00ea,0x00eb,0x00e8,0x00ef,0x00ee,0x00ec,0x00c4,0x00c5,
    0x00c9,0x00e6,0x00c6,0x00f4,0x00f6,0x00f2,0x00fb,0x00f9,
    0x00ff,0x00d6,0x00dc,0x00a2,0x00a3,0x00a5,0x20a7,0x0192,
    0x00e1,0x00ed,0x00f3,0x00fa,0x00f1,0x00d1,0x00aa,0x00ba,
    0x00bf,0x2310,0x00ac,0x00bd,0x00bc,0x00a1,0x00ab,0x00bb,
    0x2591,0x2592,0x2593,0x2502,0x2524,0x00c1,0x00c2,0x00c0,
    0x00a9,0x2563,0x2551,0x2557,0x255d,0x00a2,0x00a5,0x2510,
    0x2514,0x2534,0x252c,0x251c,0x2500,0x253c,0x00e3,0x00c3,
    0x255a,0x2554,0x2569,0x2566,0x2560,0x2550,0x256c,0x00a4,
    0x00f0,0x00d0,0x00ca,0x00cb,0x00c8,0x0131,0x00cd,0x00ce,
    0x00cf,0x2518,0x250c,0x2588,0x2584,0x00a6,0x00cc,0x2580,
    0x00d3,0x00df,0x00d4,0x00d2,0x00f5,0x00d5,0x00b5,0x00fe,
    0x00de,0x00da,0x00db,0x00d9,0x00fd,0x00dd,0x00af,0x00b4,
    0x00ad,0x00b1,0x2017,0x00be,0x00b6,0x00a7,0x00f7,0x00b8,
    0x00b0,0x00a8,0x00b7,0x00b9,0x00b3,0x00b2,0x25a0,0x00a0
};

static int append_utf8(PwXmlBuffer *xml, unsigned long cp)
{
    unsigned char bytes[4];
    unsigned count;
    if (cp < 0x80) { bytes[0] = (unsigned char)cp; count = 1; }
    else if (cp < 0x800) {
        bytes[0] = (unsigned char)(0xc0 | (cp >> 6));
        bytes[1] = (unsigned char)(0x80 | (cp & 0x3f)); count = 2;
    } else if (cp < 0x10000) {
        bytes[0] = (unsigned char)(0xe0 | (cp >> 12));
        bytes[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        bytes[2] = (unsigned char)(0x80 | (cp & 0x3f)); count = 3;
    } else {
        bytes[0] = (unsigned char)(0xf0 | (cp >> 18));
        bytes[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
        bytes[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        bytes[3] = (unsigned char)(0x80 | (cp & 0x3f)); count = 4;
    }
    return pw_xml_append_text(xml, bytes, count);
}

static int append_cp437_text(PwXmlBuffer *xml, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    while (*cursor != 0) {
        unsigned long cp = *cursor < 0x80 ? *cursor : cp437_unicode[*cursor - 0x80];
        if (!append_utf8(xml, cp)) return 0;
        ++cursor;
    }
    return 1;
}

static int append_title_shape(PwXmlBuffer *xml, const char *text)
{
    return pw_xml_append_ascii(xml,
        "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"Title\"/><p:cNvSpPr txBox=\"1\"/><p:nvPr/></p:nvSpPr>"
        "<p:spPr><a:xfrm><a:off x=\"685800\" y=\"457200\"/><a:ext cx=\"10820400\" cy=\"1371600\"/></a:xfrm>"
        "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:noFill/><a:ln><a:noFill/></a:ln></p:spPr>"
        "<p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:pPr algn=\"l\"/><a:r><a:rPr lang=\"pt-BR\" sz=\"3000\" b=\"1\"/><a:t>") &&
        append_cp437_text(xml, text) &&
        pw_xml_append_ascii(xml,
        "</a:t></a:r><a:endParaRPr lang=\"pt-BR\"/></a:p></p:txBody></p:sp>");
}

static int append_body_shape(PwXmlBuffer *xml, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    if (!pw_xml_append_ascii(xml,
        "<p:sp><p:nvSpPr><p:cNvPr id=\"3\" name=\"Body\"/><p:cNvSpPr txBox=\"1\"/><p:nvPr/></p:nvSpPr>"
        "<p:spPr><a:xfrm><a:off x=\"685800\" y=\"2286000\"/><a:ext cx=\"10820400\" cy=\"4114800\"/></a:xfrm>"
        "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:noFill/><a:ln><a:noFill/></a:ln></p:spPr>"
        "<p:txBody><a:bodyPr wrap=\"square\"/><a:lstStyle/>")) return 0;
    if (*cursor == 0) return pw_xml_append_ascii(xml, "<a:p><a:endParaRPr lang=\"pt-BR\"/></a:p></p:txBody></p:sp>");
    while (*cursor != 0) {
        if (!pw_xml_append_ascii(xml, "<a:p><a:r><a:rPr lang=\"pt-BR\" sz=\"2000\"/><a:t xml:space=\"preserve\">") ) return 0;
        while (*cursor != 0 && *cursor != '\n' && *cursor != '\r') {
            unsigned long cp = *cursor < 0x80 ? *cursor : cp437_unicode[*cursor - 0x80];
            if (!append_utf8(xml, cp)) return 0;
            ++cursor;
        }
        if (!pw_xml_append_ascii(xml, "</a:t></a:r><a:endParaRPr lang=\"pt-BR\"/></a:p>")) return 0;
        if (*cursor == '\r') ++cursor;
        if (*cursor == '\n') ++cursor;
    }
    return pw_xml_append_ascii(xml, "</p:txBody></p:sp>");
}

static int make_slide(ShPptxPart *part, const ShSlide *slide)
{
    return pw_xml_append_ascii(&part->xml,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<p:sld xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
        "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"><p:cSld><p:spTree>"
        "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
        "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/>"
        "<a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>") &&
        append_title_shape(&part->xml, slide->title) &&
        append_body_shape(&part->xml, slide->body) &&
        pw_xml_append_ascii(&part->xml,
        "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>");
}

static int make_relationship(ShPptxPart *part)
{
    return pw_xml_append_ascii(&part->xml,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>"
        "</Relationships>");
}

static int make_static_part(ShPptxPart *part, const char *xml)
{
    return pw_xml_append_ascii(&part->xml, xml);
}

static int add_part(ShPptxPart *parts, unsigned *count, const char *name)
{
    unsigned index = *count;
    parts[index].name = (char *)malloc(SH_PPTX_PATH_MAX);
    if (parts[index].name == NULL) return 0;
    strcpy(parts[index].name, name);
    pw_xml_buffer_init(&parts[index].xml);
    ++*count;
    return 1;
}

static int build_parts(const ShModel *model, ShPptxPart *parts, unsigned *count)
{
    unsigned index;
    char path[256];
    PwXmlBuffer xml;
    *count = 0;

#define ADD_STATIC(name, contents) \
    do { if (!add_part(parts, count, name) || !make_static_part(&parts[*count - 1], contents)) return 0; } while (0)

    if (!add_part(parts, count, "[Content_Types].xml")) return 0;
    if (!pw_xml_append_ascii(&parts[0].xml,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/ppt/presentation.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml\"/>"
        "<Override PartName=\"/ppt/slideMasters/slideMaster1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml\"/>"
        "<Override PartName=\"/ppt/slideLayouts/slideLayout1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml\"/>"
        "<Override PartName=\"/ppt/theme/theme1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.theme+xml\"/>")) return 0;

    pw_xml_buffer_init(&xml);
    if (!pw_xml_append_ascii(&xml,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"ppt/presentation.xml\"/>"
        "</Relationships>")) { pw_xml_buffer_dispose(&xml); return 0; }
    if (!add_part(parts, count, "_rels/.rels")) { pw_xml_buffer_dispose(&xml); return 0; }
    parts[*count - 1].xml = xml;

    pw_xml_buffer_init(&xml);
    if (!pw_xml_append_ascii(&xml,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<p:presentation xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
        "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"><p:sldMasterIdLst>"
        "<p:sldMasterId id=\"2147483648\" r:id=\"rId1\"/></p:sldMasterIdLst><p:sldIdLst>")) { pw_xml_buffer_dispose(&xml); return 0; }
    for (index = 0; index < model->count; ++index) {
        char row[128];
        sprintf(row, "<p:sldId id=\"%u\" r:id=\"rId%u\"/>", 256 + index, index + 2);
        if (!pw_xml_append_ascii(&xml, row)) { pw_xml_buffer_dispose(&xml); return 0; }
    }
    if (!pw_xml_append_ascii(&xml,
        "</p:sldIdLst><p:sldSz cx=\"12192000\" cy=\"6858000\" type=\"screen16x9\"/>"
        "<p:notesSz cx=\"6858000\" cy=\"9144000\"/><p:defaultTextStyle/></p:presentation>")) { pw_xml_buffer_dispose(&xml); return 0; }
    if (!add_part(parts, count, "ppt/presentation.xml")) { pw_xml_buffer_dispose(&xml); return 0; }
    parts[*count - 1].xml = xml;

    pw_xml_buffer_init(&xml);
    if (!pw_xml_append_ascii(&xml,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster\" Target=\"slideMasters/slideMaster1.xml\"/>")) { pw_xml_buffer_dispose(&xml); return 0; }
    for (index = 0; index < model->count; ++index) {
        char row[160];
        sprintf(row, "<Relationship Id=\"rId%u\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide\" Target=\"slides/slide%u.xml\"/>", index + 2, index + 1);
        if (!pw_xml_append_ascii(&xml, row)) { pw_xml_buffer_dispose(&xml); return 0; }
    }
    if (!pw_xml_append_ascii(&xml, "</Relationships>")) { pw_xml_buffer_dispose(&xml); return 0; }
    if (!add_part(parts, count, "ppt/_rels/presentation.xml.rels")) { pw_xml_buffer_dispose(&xml); return 0; }
    parts[*count - 1].xml = xml;

    ADD_STATIC("ppt/slideMasters/slideMaster1.xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<p:sldMaster xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"><p:cSld><p:spTree>"
        "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>"
        "</p:spTree></p:cSld><p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/><p:sldLayoutIdLst><p:sldLayoutId id=\"1\" r:id=\"rId1\"/></p:sldLayoutIdLst><p:txStyles><p:titleStyle/><p:bodyStyle/><p:otherStyle/></p:txStyles></p:sldMaster>");

    pw_xml_buffer_init(&xml);
    if (!pw_xml_append_ascii(&xml,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme\" Target=\"../theme/theme1.xml\"/></Relationships>")) { pw_xml_buffer_dispose(&xml); return 0; }
    if (!add_part(parts, count, "ppt/slideMasters/_rels/slideMaster1.xml.rels")) { pw_xml_buffer_dispose(&xml); return 0; }
    parts[*count - 1].xml = xml;

    ADD_STATIC("ppt/slideLayouts/slideLayout1.xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<p:sldLayout xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" type=\"blank\" preserve=\"1\"><p:cSld name=\"Blank\"><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr></p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>");

    pw_xml_buffer_init(&xml);
    if (!pw_xml_append_ascii(&xml,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster\" Target=\"../slideMasters/slideMaster1.xml\"/></Relationships>")) { pw_xml_buffer_dispose(&xml); return 0; }
    if (!add_part(parts, count, "ppt/slideLayouts/_rels/slideLayout1.xml.rels")) { pw_xml_buffer_dispose(&xml); return 0; }
    parts[*count - 1].xml = xml;
    if (!add_part(parts, count, "ppt/theme/theme1.xml")) return 0;
    if (!make_static_part(&parts[*count - 1],
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"Panthera Classic\"><a:themeElements><a:clrScheme name=\"Panthera\"><a:dk1><a:srgbClr val=\"000000\"/></a:dk1><a:lt1><a:srgbClr val=\"FFFFFF\"/></a:lt1><a:dk2><a:srgbClr val=\"1F1F1F\"/></a:dk2><a:lt2><a:srgbClr val=\"F2F2F2\"/></a:lt2><a:accent1><a:srgbClr val=\"000080\"/></a:accent1><a:accent2><a:srgbClr val=\"0070C0\"/></a:accent2><a:accent3><a:srgbClr val=\"70AD47\"/></a:accent3><a:accent4><a:srgbClr val=\"FFC000\"/></a:accent4><a:accent5><a:srgbClr val=\"ED7D31\"/></a:accent5><a:accent6><a:srgbClr val=\"A5A5A5\"/></a:accent6><a:hlink><a:srgbClr val=\"0563C1\"/></a:hlink><a:folHlink><a:srgbClr val=\"954F72\"/></a:folHlink></a:clrScheme><a:fontScheme name=\"Panthera\"><a:majorFont><a:latin typeface=\"Arial\"/><a:ea typeface=\"\"/><a:cs typeface=\"Arial\"/></a:majorFont><a:minorFont><a:latin typeface=\"Arial\"/><a:ea typeface=\"\"/><a:cs typeface=\"Arial\"/></a:minorFont></a:fontScheme><a:fmtScheme name=\"Panthera\"><a:fillStyleLst><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:fillStyleLst><a:lnStyleLst><a:ln w=\"9525\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln></a:lnStyleLst><a:effectStyleLst><a:effectStyle><a:effectLst/></a:effectStyle></a:effectStyleLst><a:bgFillStyleLst><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:bgFillStyleLst></a:fmtScheme></a:themeElements></a:theme>")) return 0;

    for (index = 0; index < model->count; ++index) {
        ShPptxPart *part;
        sprintf(path, "ppt/slides/slide%u.xml", index + 1);
        if (!add_part(parts, count, path)) return 0;
        part = &parts[*count - 1];
        if (!make_slide(part, &model->slides[index])) return 0;
        sprintf(path, "ppt/slides/_rels/slide%u.xml.rels", index + 1);
        if (!add_part(parts, count, path) || !make_relationship(&parts[*count - 1])) return 0;
        sprintf(path, "<Override PartName=\"/ppt/slides/slide%u.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slide+xml\"/>", index + 1);
        if (!pw_xml_append_ascii(&parts[0].xml, path)) return 0;
    }
    if (!pw_xml_append_ascii(&parts[0].xml, "</Types>")) return 0;
#undef ADD_STATIC
    return 1;
}

static void dispose_parts(ShPptxPart *parts, unsigned count)
{
    unsigned index;
    for (index = 0; index < count; ++index) {
        free(parts[index].name);
        pw_xml_buffer_dispose(&parts[index].xml);
    }
}

static int copy_file(const char *source_path, const char *destination_path)
{
    unsigned char buffer[8192];
    FILE *source = fopen(source_path, "rb");
    FILE *destination;
    int ok = 1;
    if (source == NULL) return 0;
    destination = fopen(destination_path, "wb");
    if (destination == NULL) { fclose(source); return 0; }
    for (;;) {
        size_t count = fread(buffer, 1, sizeof(buffer), source);
        if (count != 0 && fwrite(buffer, 1, count, destination) != count) { ok = 0; break; }
        if (count < sizeof(buffer)) {
            if (ferror(source)) ok = 0;
            break;
        }
    }
    if (fclose(source) != 0) ok = 0;
    if (fclose(destination) != 0) ok = 0;
    return ok;
}

static int file_exists(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return 0;
    fclose(file);
    return 1;
}

static int write_journal(int had_current)
{
    static const unsigned char magic[8] = {'S','H','P','T','X','J','0','1'};
    unsigned char record[10];
    FILE *file;
    memcpy(record, magic, sizeof(magic));
    record[8] = had_current ? 1 : 0;
    record[9] = had_current ? 0xfe : 0xff;
    file = fopen("SHOWROOM.JRN", "wb");
    if (file == NULL) return 0;
    if (fwrite(record, 1, sizeof(record), file) != sizeof(record)) {
        fclose(file);
        return 0;
    }
    return fclose(file) == 0;
}

static int write_commit_marker(void)
{
    unsigned char marker = 0xa5;
    FILE *file = fopen("SHOWROOM.JRN", "ab");
    if (file == NULL) return 0;
    if (fwrite(&marker, 1, 1, file) != 1) { fclose(file); return 0; }
    return fclose(file) == 0;
}

static int read_journal(int *had_current)
{
    static const unsigned char magic[8] = {'S','H','P','T','X','J','0','1'};
    unsigned char record[10];
    int extra, trailing = EOF, read_error, close_error;
    FILE *file = fopen("SHOWROOM.JRN", "rb");
    if (file == NULL) return 0;
    if (fread(record, 1, sizeof(record), file) != sizeof(record)) {
        fclose(file);
        return -1;
    }
    extra = fgetc(file);
    if (extra != EOF) trailing = fgetc(file);
    read_error = ferror(file);
    close_error = fclose(file) != 0;
    if (memcmp(record, magic, sizeof(magic)) != 0 || record[8] > 1 ||
        record[9] != (unsigned char)(record[8] ? 0xfe : 0xff)) return -1;
    *had_current = record[8] != 0;
    if (read_error || close_error) return -2;
    if (extra == EOF) return 1; /* commit not yet recorded */
    if (extra != 0xa5 || trailing != EOF) return -2; /* uncertain marker: roll back */
    return 2; /* commit marker follows a completely closed destination file */
}

int sh_pptx_recover(void)
{
    int had_current = 0;
    int journal = read_journal(&had_current);
    if (journal == 0) return SH_PPTX_RECOVER_NONE;

    /* The marker is appended only after the destination copy and fclose both
       succeed. Without it, restore the previous package even if the guest
       was interrupted during the destination copy. */
    if (journal == 2 && file_exists("SHOWROOM.PPTX")) {
        (void)remove("SHOWROOM.JRN");
        (void)remove("SHOWROOM.TMP");
        return SH_PPTX_RECOVER_COMMIT_KEPT;
    }

    if (journal == -1) {
        /* The commit never starts until the complete journal is closed. A
           partial record therefore means PPTX was not changed by this save. */
        if (remove("SHOWROOM.JRN") != 0 ||
            (file_exists("SHOWROOM.TMP") && remove("SHOWROOM.TMP") != 0))
            return SH_PPTX_RECOVER_ERROR;
        return SH_PPTX_RECOVER_NONE;
    }

    if (had_current) {
        if (!file_exists("SHOWROOM.BAK") ||
            !copy_file("SHOWROOM.BAK", "SHOWROOM.PPTX"))
            return SH_PPTX_RECOVER_ERROR;
        if (remove("SHOWROOM.JRN") != 0 ||
            (file_exists("SHOWROOM.TMP") && remove("SHOWROOM.TMP") != 0))
            return SH_PPTX_RECOVER_ERROR;
        return SH_PPTX_RECOVER_BACKUP_RESTORED;
    }

    if (file_exists("SHOWROOM.PPTX") && remove("SHOWROOM.PPTX") != 0)
        return SH_PPTX_RECOVER_ERROR;
    if (remove("SHOWROOM.JRN") != 0 ||
        (file_exists("SHOWROOM.TMP") && remove("SHOWROOM.TMP") != 0))
        return SH_PPTX_RECOVER_ERROR;
    return SH_PPTX_RECOVER_FIRST_SAVE_REMOVED;
}

int sh_pptx_save(const ShModel *model)
{
    ShPptxPart *parts;
    PwZip32Entry *entries;
    unsigned count = 0;
    unsigned index;
    int had_current = 0;
    FILE *probe;
    int result = SH_PPTX_BUILD_ERROR;

    if (model == NULL || model->count == 0 || model->count > SH_MAX_SLIDES) return SH_PPTX_BUILD_ERROR;
    parts = (ShPptxPart *)calloc(SH_PPTX_ENTRIES, sizeof(ShPptxPart));
    entries = (PwZip32Entry *)calloc(SH_PPTX_ENTRIES, sizeof(PwZip32Entry));
    if (parts == NULL || entries == NULL) { free(parts); free(entries); return SH_PPTX_BUILD_ERROR; }
    if (!build_parts(model, parts, &count)) goto done;
    for (index = 0; index < count; ++index) {
        entries[index].name = parts[index].name;
        entries[index].data = parts[index].xml.data;
        entries[index].size = parts[index].xml.length;
    }
    remove("SHOWROOM.TMP");
    if (pw_zip32_write_store("SHOWROOM.TMP", entries, count) != PW_ZIP32_OK) {
        remove("SHOWROOM.TMP");
        result = SH_PPTX_WRITE_ERROR;
        goto done;
    }

    probe = fopen("SHOWROOM.PPTX", "rb");
    if (probe != NULL) { had_current = 1; fclose(probe); }
    if (had_current) {
        if (!copy_file("SHOWROOM.PPTX", "SHOWROOM.BKT")) {
            remove("SHOWROOM.BKT");
            remove("SHOWROOM.TMP"); result = SH_PPTX_BACKUP_ERROR; goto done;
        }
        if (!copy_file("SHOWROOM.BKT", "SHOWROOM.BAK")) {
            remove("SHOWROOM.BKT");
            remove("SHOWROOM.TMP"); result = SH_PPTX_BACKUP_ERROR; goto done;
        }
        remove("SHOWROOM.BKT");
    }
    if (!write_journal(had_current)) {
        remove("SHOWROOM.JRN");
        remove("SHOWROOM.TMP");
        result = SH_PPTX_BACKUP_ERROR;
        goto done;
    }
    {
        int copy_ok = copy_file("SHOWROOM.TMP", "SHOWROOM.PPTX");
        if (!copy_ok) {
        if (had_current) {
            if (!copy_file("SHOWROOM.BAK", "SHOWROOM.PPTX") ||
                !file_exists("SHOWROOM.PPTX")) {
                result = SH_PPTX_RECOVERY_REQUIRED;
                goto done;
            }
        } else if (file_exists("SHOWROOM.PPTX") && remove("SHOWROOM.PPTX") != 0) {
            result = SH_PPTX_RECOVERY_REQUIRED;
            goto done;
        }
        if (remove("SHOWROOM.JRN") == 0) (void)remove("SHOWROOM.TMP");
        result = SH_PPTX_COMMIT_COPY_ERROR;
        goto done;
        }
    }
    if (!write_commit_marker()) {
        result = SH_PPTX_RECOVERY_REQUIRED;
        goto done;
    }
    /* Keep TMP if journal deletion fails: startup uses it to recognize that
       the new PPTX reached disk before power or the guest was interrupted. */
    if (remove("SHOWROOM.JRN") == 0) (void)remove("SHOWROOM.TMP");
    result = SH_PPTX_OK;
done:
    dispose_parts(parts, count);
    free(parts);
    free(entries);
    return result;
}
