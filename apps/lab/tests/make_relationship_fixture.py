"""Generate a small DEFLATE XLSX for worksheet relationship resolution tests."""
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


output = Path(__file__).parent / "fixtures" / "external-relationship-target.xlsx"
parts = {
    "[Content_Types].xml": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="xml" ContentType="application/xml"/>
<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>
<Override PartName="/xl/custom/data-sheet.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>
</Types>""",
    "_rels/.rels": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rootBook" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>
</Relationships>""",
    "xl/workbook.xml": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
<!-- Decoy markup in comments must not be parsed as a real sheet: <sheet name="CommentDecoy" sheetId="9" r:id="badId"/> -->
<sheets><sheet name="RelationTarget" sheetId="1" r:id="rId7"/></sheets>
</workbook>""",
    "xl/_rels/workbook.xml.rels": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<!-- Decoy relationship in a comment must not shadow rId7: <Relationship Id="rId7" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/> -->
<Relationship Id="unused" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>
<Relationship Id="rId7" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="custom/data-sheet.xml"/>
</Relationships>""",
    "xl/custom/data-sheet.xml": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData>
<row r="1"><c r="A1"><v>22</v></c><c r="B1"><f>A1*2</f><v>44</v></c>
<c r="C1" t="inlineStr"><is><t>REL TARGET</t></is></c></row>
</sheetData></worksheet>""",
    # Deliberate decoy. A reader that still hard-codes sheet1.xml shows 999.
    "xl/worksheets/sheet1.xml": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData>
<row r="1"><c r="A1"><v>999</v></c><c r="C1" t="inlineStr"><is><t>DECOY</t></is></c></row>
</sheetData></worksheet>""",
}
output.parent.mkdir(parents=True, exist_ok=True)
with ZipFile(output, "w", compression=ZIP_DEFLATED) as archive:
    for name, data in parts.items():
        archive.writestr(name, data)
print(output)

# A corrupt package must be rejected without replacing the currently loaded sheet.
# Its relationship contains a literal parent-directory segment and the archive has
# a matching noncanonical member, so the test can detect a resolver that accepts it.
unsafe_output = output.parent / "external-traversal-target.xlsx"
unsafe_parts = dict(parts)
unsafe_parts["xl/_rels/workbook.xml.rels"] = parts["xl/_rels/workbook.xml.rels"].replace(
    'Target="custom/data-sheet.xml"', 'Target="../sheet-escape.xml"'
)
unsafe_parts["xl/../sheet-escape.xml"] = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData>
<row r="1"><c r="A1"><v>777</v></c><c r="C1" t="inlineStr"><is><t>SHOULD NOT LOAD</t></is></c></row>
</sheetData></worksheet>"""
with ZipFile(unsafe_output, "w", compression=ZIP_DEFLATED) as archive:
    for name, data in unsafe_parts.items():
        archive.writestr(name, data)
print(unsafe_output)
