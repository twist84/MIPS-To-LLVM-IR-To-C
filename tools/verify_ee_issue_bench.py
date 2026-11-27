#!/usr/bin/env python3
"""Authenticate linked R5900 microbenchmark bodies against generated words."""
import argparse
from pathlib import Path
import struct

from generate_ee_issue_bench import CASES, REPEATS


def checked_slice(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError("ELF table or section outside file")
    return data[offset:offset + size]


def read_symbols(data):
    if len(data) < 52 or data[:7] != b"\x7fELF\x01\x01\x01":
        raise ValueError("expected ELF32 little-endian version 1")
    fields = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
    if fields[1:3] != (2, 8) or fields[3] != 1:
        raise ValueError("expected linked MIPS executable")
    shoff, shentsize, shnum = fields[6], fields[11], fields[12]
    if shentsize != 40 or not shnum:
        raise ValueError("invalid ELF section table")
    section_table = checked_slice(data, shoff, shentsize * shnum)
    sections = list(struct.iter_unpack("<10I", section_table))
    symbols = {}
    wanted = {f"bench_{name}_{n}_{suffix}"
              for name in CASES for n in REPEATS
              for suffix in ("body", "body_end")}
    for section in sections:
        if section[1] != 2:  # SHT_SYMTAB, not a stripped image
            continue
        offset, size, link, entry_size = section[4], section[5], section[6], section[9]
        if entry_size != 16 or size % entry_size or link >= shnum:
            raise ValueError("invalid symbol table")
        strings = sections[link]
        if strings[1] != 3:
            raise ValueError("missing symbol string table")
        string_data = checked_slice(data, strings[4], strings[5])
        for name_offset, address, _, _, _, index in struct.iter_unpack(
                "<IIIBBH", checked_slice(data, offset, size)):
            if name_offset >= len(string_data):
                raise ValueError("invalid symbol name offset")
            end = string_data.find(b"\0", name_offset)
            if end < 0:
                raise ValueError("unterminated symbol name")
            name = string_data[name_offset:end].decode("ascii", errors="replace")
            if name not in wanted:
                continue
            if name in symbols or not 0 < index < shnum:
                raise ValueError(f"invalid or duplicate benchmark symbol: {name}")
            symbols[name] = (address, index)
    if set(symbols) != wanted:
        raise ValueError("missing linked benchmark body symbols")
    return sections, symbols


def verify(path):
    data = path.read_bytes()
    sections, symbols = read_symbols(data)
    total = 0
    for name, block in CASES.items():
        for n in REPEATS:
            prefix = f"bench_{name}_{n}"
            start, index = symbols[prefix + "_body"]
            end, end_index = symbols[prefix + "_body_end"]
            section = sections[index]
            expected = struct.pack(f"<{len(block)*n}I", *(block*n))
            if (index != end_index or start % 16 or end - start != len(expected) or
                    section[1] != 1 or section[2] & 6 != 6 or
                    start < section[3] or end > section[3] + section[5]):
                raise ValueError(f"{prefix}: invalid linked body extent/alignment")
            actual = checked_slice(data, section[4] + start - section[3], len(expected))
            if actual != expected:
                raise ValueError(f"{prefix}: linked opcode words differ")
            total += len(block)*n
    print(f"Verified {len(CASES)*len(REPEATS)} linked aligned bodies, {total} words.")
    return total


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    args = parser.parse_args()
    try:
        verify(args.elf)
    except (ValueError, OSError, struct.error) as error:
        parser.exit(1, f"ELF verification refused: {error}\n")
