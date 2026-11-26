"""Read-only ELF, COFF and PE executable byte accounting for the EE audit."""
import struct


def executable_segments(data):
    """File-backed executable PT_LOAD bytes (PS2 ELF32 little endian)."""
    if data[:6] != b"\x7fELF\x01\x01":
        raise ValueError("expected little-endian ELF32 guest executable")
    offset, = struct.unpack_from("<I", data, 28)
    size, count = struct.unpack_from("<HH", data, 42)
    if size < 32 or offset + size * count > len(data):
        raise ValueError("invalid ELF program headers")
    total = 0
    for index in range(count):
        kind, file_offset, _, _, filesz, _, flags, _ = struct.unpack_from(
            "<8I", data, offset + size * index)
        if kind == 1 and flags & 1:
            if file_offset + filesz > len(data):
                raise ValueError("executable segment outside ELF")
            total += filesz
    return total


def native_executable_sections(data):
    """Return file-backed executable-section bytes, excluding headers/relocs.

    PE raw section alignment may include padding; separately report PE
    virtual executable bytes when a linked image supplies them.
    """
    if data[:4] == b"\x7fELF":
        if data[5] != 1:
            raise ValueError("unsupported big-endian native ELF")
        if data[4] == 2:
            offset, = struct.unpack_from("<Q", data, 40)
            stride, count = struct.unpack_from("<HH", data, 58)
            fmt, minimum = "<IIQQQQ", 64
        elif data[4] == 1:
            offset, = struct.unpack_from("<I", data, 32)
            stride, count = struct.unpack_from("<HH", data, 46)
            fmt, minimum = "<6I", 40
        else:
            raise ValueError("unknown native ELF class")
        if count == 0 or stride < minimum or offset + stride * count > len(data):
            raise ValueError("invalid or extended native ELF section table")
        total = 0
        for index in range(count):
            _, kind, flags, _, pos, size = struct.unpack_from(fmt, data, offset + stride * index)
            if flags & 4 and kind != 8:
                if pos + size > len(data):
                    raise ValueError("executable ELF section outside file")
                total += size
        return total, None

    pe = data[:2] == b"MZ"
    if pe:
        offset, = struct.unpack_from("<I", data, 60)
        if data[offset:offset + 4] != b"PE\0\0":
            raise ValueError("invalid PE header")
        offset += 4
    else:
        offset = 0
    if not pe and data[:4] == b"\0\0\xff\xff":
        # Microsoft bigobj, used when the ordinary COFF section limit is hit.
        if len(data) < 56 or data[12:28] != bytes.fromhex(
                "c7a1bad1eebaa94baf20faf66aa4dcb8"):
            raise ValueError("unsupported COFF anonymous object")
        count, = struct.unpack_from("<I", data, 44)
        section_table = 56
    else:
        machine, count, _, _, _, optional, _ = struct.unpack_from("<HHIIIHH", data, offset)
        if machine not in (0x14c, 0x8664, 0xaa64, 0x1c4):
            raise ValueError("unsupported native object format")
        section_table = offset + 20 + optional
    if not count or section_table + 40 * count > len(data):
        raise ValueError("invalid COFF section table")
    raw_total = virtual_total = 0
    for index in range(count):
        pos = section_table + 40 * index
        virtual, _, raw, file_offset = struct.unpack_from("<4I", data, pos + 8)
        flags, = struct.unpack_from("<I", data, pos + 36)
        if flags & 0x20000000 or flags & 0x20:
            if raw and (not file_offset or file_offset + raw > len(data)):
                raise ValueError("executable COFF section outside file")
            raw_total += raw
            virtual_total += virtual if virtual else raw
    return raw_total, virtual_total if pe else None
