import sys
import zipfile


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: make_numbering_fixture.py target.docx")
    parts = {
        "[Content_Types].xml": (
            '<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
            '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
            '<Default Extension="xml" ContentType="application/xml"/>'
            '<Override PartName="/custom/main.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>'
            '</Types>'
        ),
        "_rels/.rels": (
            '<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            '<Relationship Id="doc" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="custom/main.xml"/>'
            '</Relationships>'
        ),
        "custom/_rels/main.xml.rels": (
            '<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            '<Relationship Id="num" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering" Target="numbering.xml"/>'
            '</Relationships>'
        ),
        "custom/main.xml": (
            '<?xml version="1.0"?><w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
            '<w:body><w:p><w:pPr><w:numPr><w:ilvl w:val="0"/><w:numId w:val="2"/></w:numPr></w:pPr>'
            '<w:r><w:t>Decimal</w:t></w:r></w:p>'
            '<w:p><w:pPr><w:numPr><w:ilvl w:val="0"/><w:numId w:val="3"/></w:numPr></w:pPr>'
            '<w:r><w:t>Bullet</w:t></w:r></w:p><w:sectPr/></w:body></w:document>'
        ),
        "custom/numbering.xml": (
            '<?xml version="1.0"?><w:numbering xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
            '<w:abstractNum w:abstractNumId="5"><w:lvl w:ilvl="0"><w:numFmt w:val="decimal"/></w:lvl></w:abstractNum>'
            '<w:abstractNum w:abstractNumId="6"><w:lvl w:ilvl="0"><w:numFmt w:val="bullet"/></w:lvl></w:abstractNum>'
            '<w:num w:numId="2"><w:abstractNumId w:val="5"/></w:num>'
            '<w:num w:numId="3"><w:abstractNumId w:val="6"/></w:num>'
            '</w:numbering>'
        ),
    }
    with zipfile.ZipFile(sys.argv[1], "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as target:
        for name, content in parts.items():
            target.writestr(name, content, compress_type=zipfile.ZIP_DEFLATED)


if __name__ == "__main__":
    main()
