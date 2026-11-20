import sys
import zipfile


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit("usage: make_deflated_fixture.py target.doc [officeDocument-target]")
    office_document_target = sys.argv[2] if len(sys.argv) == 3 else "custom/%6dain.xml"
    document = (
        '<?xml version="1.0" encoding="UTF-8"?>'
        '<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
        '<w:body><w:p><w:pPr><w:pStyle w:val="BodyChild"/></w:pPr>'
        '<w:r><w:rPr><w:rStyle w:val="UnderlineChild"/><w:b w:val="0"/></w:rPr>'
        '<w:t xml:space="preserve">Panthera &amp; &lt;Works&gt;</w:t></w:r></w:p>'
        '<w:p><w:pPr><w:pStyle w:val="BodyChild"/></w:pPr>'
        '<w:r><w:t xml:space="preserve">DOCX test: café</w:t></w:r></w:p>'
        '<w:p><w:r><w:t>Default paragraph</w:t></w:r></w:p>'
        '<w:p/><w:sectPr/></w:body></w:document>'
    )
    decoy_document = (
        '<?xml version="1.0" encoding="UTF-8"?>'
        '<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
        '<w:body><w:p><w:r><w:t>DECOY WRONG DOCUMENT</w:t></w:r></w:p></w:body></w:document>'
    )
    parts = {
        "[Content_Types].xml": (
            '<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
            '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
            '<Default Extension="xml" ContentType="application/xml"/>'
            '<Override PartName="/custom/main.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>'
            '<Override PartName="/custom/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/>'
            '</Types>'
        ),
        "_rels/.rels": (
            '<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="'
            + office_document_target + '"/>'
            '</Relationships>'
        ),
        "custom/_rels/main.xml.rels": (
            '<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            '<Relationship Id="rIdStyles" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>'
            '</Relationships>'
        ),
        "custom/styles.xml": (
            '<?xml version="1.0"?><w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
            '<w:docDefaults><w:rPrDefault><w:rPr><w:i/></w:rPr></w:rPrDefault>'
            '<w:pPrDefault><w:pPr><w:jc w:val="center"/></w:pPr></w:pPrDefault></w:docDefaults>'
            '<w:style w:type="paragraph" w:default="1" w:styleId="Normal"/>'
            '<w:style w:type="paragraph" w:styleId="BodyBase"><w:pPr><w:jc w:val="center"/></w:pPr><w:rPr><w:b/></w:rPr></w:style>'
            '<w:style w:type="paragraph" w:styleId="BodyChild"><w:basedOn w:val="BodyBase"/><w:pPr><w:jc w:val="right"/></w:pPr><w:rPr><w:i w:val="0"/></w:rPr></w:style>'
            '<w:style w:type="character" w:styleId="UnderlineBase"><w:rPr><w:u w:val="single"/></w:rPr></w:style>'
            '<w:style w:type="character" w:styleId="UnderlineChild"><w:basedOn w:val="UnderlineBase"/></w:style>'
            '</w:styles>'
        ),
        "custom/main.xml": document,
        "word/document.xml": decoy_document,
    }
    with zipfile.ZipFile(sys.argv[1], "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as target:
        for name, content in parts.items():
            target.writestr(name, content, compress_type=zipfile.ZIP_DEFLATED)
    with zipfile.ZipFile(sys.argv[1], "r") as checked:
        bad = [item.filename for item in checked.infolist() if item.compress_type != zipfile.ZIP_DEFLATED]
        if bad:
            raise SystemExit(f"fixture contains non-DEFLATE parts: {bad}")


if __name__ == "__main__":
    main()
