#!/usr/bin/env python3
"""Build and inspect a disposable MBR/FAT16 disk for NF-DOS integration tests."""

from __future__ import annotations

import argparse
import math
import os
import re
import struct
import sys
import zipfile
from pathlib import Path
from xml.etree import ElementTree

SECTOR = 512
TOTAL_SECTORS = 131072  # 64 MiB
PARTITION_START = 63
SECTORS_PER_CLUSTER = 4
RESERVED = 1
FATS = 2
ROOT_ENTRIES = 512
FAT_SECTORS = 128
ROOT_SECTORS = ROOT_ENTRIES * 32 // SECTOR
DATA_START = PARTITION_START + RESERVED + FATS * FAT_SECTORS + ROOT_SECTORS
DATA_SECTORS = TOTAL_SECTORS - DATA_START
CLUSTER_COUNT = DATA_SECTORS // SECTORS_PER_CLUSTER
EOC = 0xFFFF


def dos83(name: str, used: set[bytes]) -> tuple[bytes, bool]:
    base, dot, ext = name.rpartition(".")
    if not dot:
        base, ext = name, ""
    if not base or len(name.encode("utf-16le")) > 510 or any(c in name for c in '\\/:*?"<>|'):
        raise ValueError(f"Invalid FAT long filename: {name}")
    valid = bool(re.fullmatch(r"[A-Za-z0-9_.$~-]{1,8}", base)) and bool(
        not ext or re.fullmatch(r"[A-Za-z0-9_.$~-]{1,3}", ext)
    )
    if valid:
        short = base.upper().ljust(8).encode("ascii") + ext.upper().ljust(3).encode("ascii")
        if short not in used:
            used.add(short)
            return short, name.upper() != name or len(ext) > 3
    safe_base = re.sub(r"[^A-Z0-9]", "", base.upper()) or "FILE"
    safe_ext = re.sub(r"[^A-Z0-9]", "", ext.upper())[:3]
    for number in range(1, 10):
        alias_base = safe_base[: 7 - len(str(number))] + "~" + str(number)
        short = alias_base.ljust(8).encode("ascii") + safe_ext.ljust(3).encode("ascii")
        if short not in used:
            used.add(short)
            return short, True
    raise ValueError(f"Unable to make a unique DOS 8.3 alias for {name}")


def lfn_entries(name: str, short: bytes) -> list[bytes]:
    units = list(struct.unpack("<" + "H" * (len(name.encode("utf-16le")) // 2), name.encode("utf-16le")))
    units.append(0)
    while len(units) % 13:
        units.append(0xFFFF)
    chunks = [units[i:i + 13] for i in range(0, len(units), 13)]
    checksum = 0
    for value in short:
        checksum = (((checksum & 1) << 7) + (checksum >> 1) + value) & 0xFF
    records = []
    for index in reversed(range(len(chunks))):
        rec = bytearray(32)
        rec[0] = index + 1 | (0x40 if index == len(chunks) - 1 else 0)
        rec[11] = 0x0F
        rec[12] = 0
        rec[13] = checksum
        chunk = chunks[index]
        for off, selected in ((1, chunk[:5]), (14, chunk[5:11]), (28, chunk[11:13])):
            struct.pack_into("<" + "H" * len(selected), rec, off, *selected)
        struct.pack_into("<H", rec, 26, 0)
        records.append(bytes(rec))
    return records


def chs(lba: int) -> bytes:
    heads, sectors = 16, 63
    cylinder, rem = divmod(lba, heads * sectors)
    head, sector0 = divmod(rem, sectors)
    sector = sector0 + 1
    if cylinder > 1023:
        return bytes((head, 0xFF, 0xFF))
    return bytes((head, (sector & 0x3F) | ((cylinder >> 2) & 0xC0), cylinder & 0xFF))


def boot_sector(partition_sectors: int) -> bytes:
    b = bytearray(SECTOR)
    b[0:3] = b"\xEB\x3C\x90"
    b[3:11] = b"MSDOS5.0"
    struct.pack_into("<HBHBHHBHHHII", b, 11,
                     SECTOR, SECTORS_PER_CLUSTER, RESERVED, FATS,
                     ROOT_ENTRIES, 0, 0xF8, FAT_SECTORS, 63, 16, 0,
                     partition_sectors)
    b[36] = 0x80
    b[38] = 0x29
    struct.pack_into("<I", b, 39, 0x504E5431)
    b[43:54] = b"PANTHERA   "
    b[54:62] = b"FAT16   "
    b[510:512] = b"\x55\xAA"
    return bytes(b)


def create(image: Path, source: Path) -> None:
    if image.exists():
        raise FileExistsError(f"Refusing to overwrite existing disk image: {image}")
    files = sorted((p for p in source.iterdir() if p.is_file()), key=lambda p: p.name.upper())
    if not files or len(files) > ROOT_ENTRIES - 1:
        raise ValueError("Input must contain between 1 and 511 root files")
    used = 0
    entries: list[tuple[Path, bytes, bool, int, bytes]] = []
    used_short_names: set[bytes] = set()
    fat = [0xFFF8, 0xFFFF] + [0] * CLUSTER_COUNT
    first_cluster = 2
    for path in files:
        short, has_lfn = dos83(path.name, used_short_names)
        data = path.read_bytes()
        clusters = math.ceil(len(data) / (SECTOR * SECTORS_PER_CLUSTER))
        if first_cluster + clusters > CLUSTER_COUNT + 2:
            raise ValueError(f"Disk image is too small for {path.name}")
        start = first_cluster if clusters else 0
        for i in range(clusters):
            fat[first_cluster + i] = first_cluster + i + 1 if i + 1 < clusters else EOC
        first_cluster += clusters
        used += clusters
        entries.append((path, short, has_lfn, start, data))

    root_records = sum(1 + (len(lfn_entries(path.name, short)) if has_lfn else 0)
                       for path, short, has_lfn, _, _ in entries)
    if root_records > ROOT_ENTRIES:
        raise ValueError(f"Root directory needs {root_records} entries; limit is {ROOT_ENTRIES}")

    image.parent.mkdir(parents=True, exist_ok=True)
    temp = image.with_suffix(image.suffix + ".tmp")
    try:
        with temp.open("xb") as f:
            f.truncate(TOTAL_SECTORS * SECTOR)
            mbr = bytearray(SECTOR)
            mbr[446] = 0x80
            mbr[447:450] = chs(PARTITION_START)
            mbr[450] = 0x06  # FAT16, 16-bit sector count
            mbr[451:454] = chs(TOTAL_SECTORS - 1)
            struct.pack_into("<II", mbr, 454, PARTITION_START, TOTAL_SECTORS - PARTITION_START)
            mbr[510:512] = b"\x55\xAA"
            f.seek(0)
            f.write(mbr)
            f.seek(PARTITION_START * SECTOR)
            f.write(boot_sector(TOTAL_SECTORS - PARTITION_START))

            fat_blob = struct.pack("<" + "H" * len(fat), *fat)
            fat_blob = fat_blob.ljust(FAT_SECTORS * SECTOR, b"\0")
            fat_start = (PARTITION_START + RESERVED) * SECTOR
            for n in range(FATS):
                f.seek(fat_start + n * FAT_SECTORS * SECTOR)
                f.write(fat_blob)

            root_start = (PARTITION_START + RESERVED + FATS * FAT_SECTORS) * SECTOR
            f.seek(root_start)
            root_index = 0
            for path, short, has_lfn, cluster, data in entries:
                if has_lfn:
                    for lfn_rec in lfn_entries(path.name, short):
                        f.seek(root_start + root_index * 32)
                        f.write(lfn_rec)
                        root_index += 1
                rec = bytearray(32)
                rec[0:11] = short
                rec[11] = 0x20
                struct.pack_into("<HHHHHH", rec, 14, 0, 0, 0, 0, 0, 0)
                struct.pack_into("<H", rec, 26, cluster)
                struct.pack_into("<I", rec, 28, len(data))
                f.seek(root_start + root_index * 32)
                f.write(rec)
                root_index += 1
                if data:
                    data_lba = DATA_START + (cluster - 2) * SECTORS_PER_CLUSTER
                    f.seek(data_lba * SECTOR)
                    f.write(data)
                    f.seek(root_start + root_index * 32)
            f.flush()
            os.fsync(f.fileno())
        os.replace(temp, image)
    finally:
        if temp.exists():
            temp.unlink()
    print(f"Created MBR/FAT16 image: {image} ({TOTAL_SECTORS * SECTOR} bytes, {len(entries)} files, {used} clusters)")


def read_root(image: Path) -> dict[str, tuple[str, int, int, bytes]]:
    raw = image.read_bytes()
    if len(raw) != TOTAL_SECTORS * SECTOR or raw[510:512] != b"\x55\xAA":
        raise ValueError("Unexpected disk image size or MBR signature")
    start, sectors = struct.unpack_from("<II", raw, 454)
    if start != PARTITION_START or sectors != TOTAL_SECTORS - PARTITION_START:
        raise ValueError("Unexpected MBR partition geometry")
    bpb = raw[start * SECTOR:(start + 1) * SECTOR]
    if bpb[510:512] != b"\x55\xAA" or bpb[54:59] != b"FAT16":
        raise ValueError("Missing FAT16 partition boot sector")
    fat_lba = start + RESERVED
    fat = list(struct.unpack_from("<" + "H" * (FAT_SECTORS * SECTOR // 2), raw, fat_lba * SECTOR))
    root_lba = start + RESERVED + FATS * FAT_SECTORS
    data_lba = root_lba + ROOT_SECTORS
    result: dict[str, tuple[str, int, int, bytes]] = {}
    pending_lfn: list[bytes] = []
    for i in range(ROOT_ENTRIES):
        rec = raw[root_lba * SECTOR + i * 32:root_lba * SECTOR + (i + 1) * 32]
        if rec[0] == 0:
            break
        if rec[0] == 0xE5:
            pending_lfn.clear()
            continue
        if rec[11] == 0x0F:
            pending_lfn.append(rec)
            continue
        if rec[11] & 0x18:
            pending_lfn.clear()
            continue
        base = rec[:8].decode("ascii").rstrip()
        ext = rec[8:11].decode("ascii").rstrip()
        alias = base + ("." + ext if ext else "")
        name = alias
        if pending_lfn:
            units: list[int] = []
            for lfn_rec in sorted(pending_lfn, key=lambda item: item[0] & 0x1F):
                units.extend(struct.unpack_from("<5H", lfn_rec, 1))
                units.extend(struct.unpack_from("<6H", lfn_rec, 14))
                units.extend(struct.unpack_from("<2H", lfn_rec, 28))
            text = bytearray()
            for unit in units:
                if unit in (0, 0xFFFF):
                    break
                text.extend(struct.pack("<H", unit))
            if text:
                name = text.decode("utf-16le")
        pending_lfn.clear()
        cluster = struct.unpack_from("<H", rec, 26)[0]
        size = struct.unpack_from("<I", rec, 28)[0]
        chunks = bytearray()
        current = cluster
        visited = set()
        while size and 2 <= current < 0xFFF8:
            if current in visited or current >= len(fat):
                raise ValueError(f"Invalid FAT chain for {name}")
            visited.add(current)
            offset = (data_lba + (current - 2) * SECTORS_PER_CLUSTER) * SECTOR
            chunks.extend(raw[offset:offset + SECTORS_PER_CLUSTER * SECTOR])
            nxt = fat[current]
            if nxt >= 0xFFF8:
                break
            current = nxt
        if size and len(chunks) < size:
            raise ValueError(f"Truncated FAT chain for {name}")
        result[name] = (alias, cluster, size, bytes(chunks[:size]))
    return result


def extract(image: Path, dest: Path) -> None:
    if dest.exists():
        raise FileExistsError(f"Refusing to overwrite extraction directory: {dest}")
    entries = read_root(image)
    dest.mkdir(parents=True)
    for name, (_, _, _, data) in entries.items():
        (dest / name).write_bytes(data)
    print(f"Extracted {len(entries)} FAT16 root files to {dest}")


def verify(image: Path, source: Path, dest: Path) -> None:
    entries = read_root(image)
    names = {name.upper() for name in entries}
    required = {"SHOWROOM.PPTX", "SHOWROOM.BAK"}
    if not required.issubset(names):
        raise ValueError(f"Expected saved PPTX and backup, found: {sorted(names)}")
    original = source / "SHOWROOM.PPTX"
    backup = next(name for name in entries if name.upper() == "SHOWROOM.BAK")
    if entries[backup][3] != original.read_bytes():
        raise ValueError("SHOWROOM.BAK does not preserve the imported PPTX bytes")
    extract(image, dest)
    pptx_name = next(name for name in entries if name.upper() == "SHOWROOM.PPTX")
    pptx = dest / pptx_name
    with zipfile.ZipFile(pptx) as archive:
        slides = [n for n in archive.namelist() if re.fullmatch(r"ppt/slides/slide\d+\.xml", n)]
        texts = []
        for name in slides:
            root = ElementTree.fromstring(archive.read(name))
            texts.extend(node.text or "" for node in root.iter("{http://schemas.openxmlformats.org/drawingml/2006/main}t"))
    if not any("2026" in text for text in texts):
        raise ValueError(f"Edited marker absent from saved slide text: {texts}")
    print("PASS: final SHOWROOM.PPTX is present on FAT16 and contains edited 2026 text")
    print("PASS: SHOWROOM.BAK on FAT16 matches the original imported PPTX bytes")
    print("FAT16 root entries:", ", ".join(sorted(entries)))


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    p_create = sub.add_parser("create")
    p_create.add_argument("image", type=Path)
    p_create.add_argument("source", type=Path)
    p_extract = sub.add_parser("extract")
    p_extract.add_argument("image", type=Path)
    p_extract.add_argument("destination", type=Path)
    p_verify = sub.add_parser("verify")
    p_verify.add_argument("image", type=Path)
    p_verify.add_argument("source", type=Path)
    p_verify.add_argument("destination", type=Path)
    args = parser.parse_args()
    if args.command == "create":
        create(args.image, args.source)
    elif args.command == "extract":
        extract(args.image, args.destination)
    else:
        verify(args.image, args.source, args.destination)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
