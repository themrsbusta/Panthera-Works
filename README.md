# Panthera Works 1.0

Panthera Notary, Panthera Lab and Panthera Showroom are standalone productivity applications for compatible DOS environments. Compatibility beyond the original tested environment has not been established.

| Application | Executable | Main document format |
|---|---|---|
| Panthera Notary | `bin/Panthera Notary/NOTARY.EXE` | DOCX |
| Panthera Lab | `bin/Panthera Lab/LAB.EXE` | XLSX |
| Panthera Showroom | `bin/Panthera Showroom/SHOWROOM.EXE` | PPTX |

Each folder also contains its launcher, LFN API checker, installation notes and third-party notices. See the corresponding `INSTALL-LFN.txt` before using long filenames.

## Requirements

- Compatible 32-bit x86 DOS environment supporting DOS/32A protected-mode execution.
- An independently installed DOS LFN driver and `CP437UNI.TBL` for DOCX/XLSX/PPTX filenames. They are intentionally not bundled because redistribution rights for DOSLFN remain unresolved.

The applications retain their long-filename support. Native LFN implementations are outside the scope of this release.

## Source

The complete Panthera Works application source is under `apps/`; zlib 1.4 source is under `third_party/zlib-1.4/`. Source-level test harnesses and fixture generators are retained. Generated tests, logs, screenshots, old build attempts, private workstation paths and Craft reference sources are excluded.

The release binaries are the verified DOS builds listed in `PACKAGE-MANIFEST.yaml`. The original development build wrappers are tied to private sibling workspaces and are not included in this public-preparation snapshot. An independently reproducible public build workflow remains to be prepared.

## License

Panthera Research licenses Panthera-owned source code and original project
assets in this repository under the MIT License; see LICENSE and
LICENSE-PANTHERA-STATUS.md. The MIT grant covers only Panthera-authored
material.

Third-party components retain their own licenses and notices. In particular,
zlib 1.4, DOS/32A, and Open Watcom runtime code are governed by the terms in
LICENSES/ and THIRD-PARTY-NOTICES.txt. The MIT license does not relicense
those components. DOSLFN.COM and CP437UNI.TBL are not included.
