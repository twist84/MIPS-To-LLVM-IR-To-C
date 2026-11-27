#!/usr/bin/env python3
"""Create a tiny ISO9660 image for the OpenPS2BIOS mounted-disc probe."""

from __future__ import annotations

import argparse
from pathlib import Path


SECTOR_SIZE = 2048
SECTOR_COUNT = 64
ROOT_LBA = 18
SYSTEM_CNF_LBA = 19
ELF_LBA = 20


def both_endian_16(value: int) -> bytes:
    return value.to_bytes(2, "little") + value.to_bytes(2, "big")


def both_endian_32(value: int) -> bytes:
    return value.to_bytes(4, "little") + value.to_bytes(4, "big")


def directory_record(extent: int, size: int, flags: int, identifier: bytes) -> bytes:
    record = bytearray((33 + len(identifier) + (1 if len(identifier) % 2 == 0 else 0)))
    record[0] = len(record)
    record[1] = 0
    record[2:10] = both_endian_32(extent)
    record[10:18] = both_endian_32(size)
    record[18:25] = bytes((126, 1, 1, 0, 0, 0, 0))
    record[25] = flags
    record[26] = 0
    record[27] = 0
    record[28:32] = both_endian_16(1)
    record[32] = len(identifier)
    record[33:33 + len(identifier)] = identifier
    return bytes(record)


def create_iso(path: Path) -> None:
    image = bytearray(SECTOR_SIZE * SECTOR_COUNT)
    pvd = memoryview(image)[16 * SECTOR_SIZE:17 * SECTOR_SIZE]
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[40:72] = b"OPENPS2BIOS TEST DISC".ljust(32, b" ")
    pvd[80:88] = both_endian_32(SECTOR_COUNT)
    pvd[120:124] = both_endian_16(1)
    pvd[124:128] = both_endian_16(1)
    pvd[128:132] = both_endian_16(SECTOR_SIZE)
    pvd[132:140] = both_endian_32(0)
    pvd[156:190] = directory_record(ROOT_LBA, SECTOR_SIZE, 2, b"\x00")

    terminator = memoryview(image)[17 * SECTOR_SIZE:18 * SECTOR_SIZE]
    terminator[0] = 255
    terminator[1:6] = b"CD001"
    terminator[6] = 1

    system_cnf = b"BOOT2 = cdrom0:\\TEST.ELF;1\r\nVER = 1.00\r\n"
    elf = bytearray(52)
    elf[:4] = b"\x7fELF"
    elf[4:7] = bytes((1, 1, 1))
    elf[16:18] = (2).to_bytes(2, "little")
    elf[18:20] = (8).to_bytes(2, "little")
    elf[20:24] = (1).to_bytes(4, "little")
    elf[24:28] = (0x00100000).to_bytes(4, "little")
    root = memoryview(image)[ROOT_LBA * SECTOR_SIZE:(ROOT_LBA + 1) * SECTOR_SIZE]
    records = (
        directory_record(ROOT_LBA, SECTOR_SIZE, 2, b"\x00") +
        directory_record(ROOT_LBA, SECTOR_SIZE, 2, b"\x01") +
        directory_record(SYSTEM_CNF_LBA, len(system_cnf), 0, b"SYSTEM.CNF;1") +
        directory_record(ELF_LBA, len(elf), 0, b"TEST.ELF;1")
    )
    root[:len(records)] = records
    image[SYSTEM_CNF_LBA * SECTOR_SIZE:
          SYSTEM_CNF_LBA * SECTOR_SIZE + len(system_cnf)] = system_cnf
    image[ELF_LBA * SECTOR_SIZE:ELF_LBA * SECTOR_SIZE + len(elf)] = elf
    path.write_bytes(image)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    create_iso(args.output)
    print(f"Created {args.output} ({SECTOR_COUNT} sectors)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
