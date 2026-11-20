# Panthera Notary — architecture record

## Goal

Build a standalone 32-bit NF-DOS word processor. Product requirements drive the
architecture; individual probes validate interfaces and never define a feature
ceiling. WordCraft is a behavioral and architectural study source only. Notary
does not use its source, assets, undocumented structures, or visual identity.

## Execution and platform boundary

- Primary executable: Open Watcom C, DOS LE, bound as a standalone DOS/32A EXE
  using the NFLLM 1.1 workflow (`wcl386`, `SB /B`, `SVER`).
- NFLLM is an execution-workflow reference. DOS/32A is the extender; the
  Panthera program calls the C runtime and verified DOS/DPMI/BIOS services. No
  NFLLM editor API or Panthera runtime library is assumed.
- Panthera DOS Runtime remains an architectural comparison source. Its
  standalone `SC /B` route was validated by the platform probe, but PDR is not
  yet an application API or dependency.
- The current graphics adapter targets VBE 640x480x8 linear framebuffer and
  uses the VBE mode information and DPMI physical mapping interfaces. It is a
  selected target, not the only mode Notary may support in future.
- Keyboard is mandatory. Notary supports mouse interaction when INT 33h is
  available. QEMU attempt46 verified File-menu selection, mouse Save As, and
  Find/Replace toolbar clicks. DOSBox-X pointer input and document selection or
  dragging remain unverified; see the mouse investigation record.

## Product layers

1. **Platform adapter:** DOS/32A process startup, memory allocation, file
   operations, BIOS keyboard, VBE mode setup, DPMI LFB mapping, and clean mode
   restoration.
2. **Document model:** UTF-8 text buffer, cursor and selection positions,
   dirty state, and future paragraph/style/table objects. Storage grows on
   demand; it has no 32 MiB probe-derived cap.
3. **Edit commands and history:** insert, delete, navigation, selection,
   search/replace, and undo/redo are independent of drawing. Typing is grouped
   into history entries; history drops the oldest entry only at its documented
   entry limit or when memory allocation fails.
4. **Layout:** maps document content to visual lines/pages and hit-testable
   positions. The first vertical slice has a text-page layout; pagination,
   styles, tables, headers/footers, comments, review, and references remain
   product work, not removals from the target scope.
5. **Renderer and event loop:** converts UI/document state to VBE pixels and
   maps BIOS keyboard input to commands. F2 provides a direct Save As path;
   Ctrl+S remains implemented but its key event was not delivered by the
   QEMU monitor input route used in the current guest test. Repaint is currently
   a full redraw; incremental invalidation and broader document pointer
   interactions can be added without changing the document model.
6. **Persistence and format adapters:** DOCX is the required final default
   open/save format. The current `.PNT` v1 file is a development-only
   persistence prototype used to validate versioning, integrity checks, and
   safe replacement with a temporary file and retained backup; it is not the
   final Notary format and will not be frozen for release. Plain-text
   import/export and other formats such as RTF, HTML, ODT, and PDF are separate
   interoperability work.

## Memory and file-size policy

- Document, history, clipboard and import buffers allocate dynamically and
  report allocation failures without discarding the current document.
- There is no fixed product document-size limit derived from the 16-bit probe
  or the 32 MiB allocation test. Actual usable memory and DOS filesystem limits
  must be measured in application runs and handled as runtime conditions.
- The platform probe has demonstrated one successful 32 MiB allocation in a
  96 MiB QEMU guest. This does not establish the maximum available allocation.
- The `.PNT` development prototype is staged and validated before replacing
  the in-memory document; its header carries a version, encoding marker, byte
  count, and CRC32. Final DOCX handling must validate the OOXML package and
  preserve the document in the selected default format.

## Verified platform facts and remaining experiments

Verified in QEMU/NF-DOS V23: DOS/32A 26.1.2 standalone binding via NFLLM;
DOS/32A 9.1.2 standalone binding via PDR; DPMI 0.90 and XMS presence; 1, 8,
16 and 32 MiB protected allocations; 4096-byte FAT round-trip; VBE 3.0 mode
list of 93 entries; successful 4F01 information queries; legacy VGA mode
set/readback; and 640x480x8 LFB set/map/write/unmap. These are demonstrated
paths, not universal hardware or product limits.

Before product release, measure larger memory allocations and large-file
streaming, sustained redraw rate, extended-key and navigation/selection edge
cases, DOSBox-X pointer input, document selection and dragging, disk-full and
interrupted saves, malformed-file rejection, overwrite/backup recovery, and
mode restoration. Probe additional VBE modes for portability; do not force the
application to use a reduced feature set because one mode is unavailable on a
given boot.

## Current implementation stage

The current source is a first graphical editing vertical slice: dynamic UTF-8
buffer, CP437 keyboard conversion, insertion/deletion, navigation, selection,
grouped undo/redo, find/replace, clipboard within Notary, development-format
save/open, text import/export, VBE drawing, mouse menus and toolbar clicks, and
safe save replacement. Attempt20 compiled
as a standalone DOS/32A 26.1.2 executable and passed in NF-DOS V23/QEMU with
640x480x8 rendering, UTF-8 text input, grouped undo/redo, F2 Save As, clean
exit, and independent `.PNT` v1 header/content/CRC32 validation. This verifies
the development persistence prototype only; it does not verify final DOCX
support. The 11-byte
`hello world` document was saved to a fresh FAT share; the original V23 image
was mounted read-only and its expected SHA-256 was unchanged. The tested flow
does not yet cover reopen/import/export, failure recovery, larger documents,
or the full keyboard surface. Ctrl+S remains present in the handler but was
not delivered by QEMU's `sendkey ctrl-s` route in this guest, so that route is
not evidence that the shortcut works or fails on physical keyboard input.
This stage is not the final parity claim. DOCX as the default format, styles,
page flow, tables, review, and broader format interoperability remain scheduled
product work.
