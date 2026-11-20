from pathlib import Path
import sys
import zipfile


def package(path: Path, traversal: bool = False) -> None:
    target_a = "../../outside.xml" if traversal else "slides/custom-first.xml"
    members = {
        "_rels/.rels": '''<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rMain" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="documents/main.xml"/></Relationships>''',
        "documents/main.xml": '''<?xml version="1.0"?><p:presentation xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" xmlns:x="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><p:sldIdLst><p:sldId id="256" x:id="rIdB"/><p:sldId id="257" x:id="rIdA"/></p:sldIdLst></p:presentation>''',
        "documents/_rels/main.xml.rels": f'''<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rIdA" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide" Target="slides/custom-second.xml"/><Relationship Id="rIdB" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide" Target="{target_a}"/></Relationships>''',
        "documents/slides/custom-first.xml": '''<?xml version="1.0"?><p:sld xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"><p:cSld><p:spTree><p:sp><p:txBody><a:p><a:r><a:t>Caf&#233; &lt;Intro&gt; &amp;</a:t></a:r></a:p></p:txBody></p:sp><p:sp><p:txBody><a:p><a:r><a:t>Agenda</a:t></a:r></a:p><a:p><a:r><a:t>Line 2</a:t></a:r></a:p></p:txBody></p:sp></p:spTree></p:cSld></p:sld>''',
        "documents/slides/custom-second.xml": '''<?xml version="1.0"?><p:sld xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"><p:cSld><p:spTree><p:sp><p:txBody><a:p><a:r><a:t>Close</a:t></a:r></a:p></p:txBody></p:sp><p:sp><p:txBody><a:p><a:r><a:t>Done</a:t></a:r></a:p></p:txBody></p:sp></p:spTree></p:cSld></p:sld>''',
    }
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in members.items():
            archive.writestr(name, data.encode("utf-8"))


if __name__ == "__main__":
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    package(out / "IMPORT-FIXTURE.PPTX")
    package(out / "TRAVERSAL-FIXTURE.PPTX", traversal=True)
