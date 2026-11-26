#!/usr/bin/env python3
"""Exercise exact body grouping and native COFF function-span attribution."""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from audit_ee_helper_inventory import coff_spans, ir_inventory


def fixture(big=False):
    stride, header = (20, 56) if big else (18, 20)
    raw_pos = header + 40
    symbol_pos = raw_pos + 64
    names = [("guest", 0, 0x20, 2), ("helper", 24, 0x20, 3),
             ("alias", 24, 0, 3), ("cold", 40, 0x20, 3),
             ("end", 56, 0, 3)]
    if big:
        data = bytearray(56)
        data[:4] = b"\0\0\xff\xff"
        data[12:28] = bytes.fromhex("c7a1bad1eebaa94baf20faf66aa4dcb8")
        struct.pack_into("<III", data, 44, 1, symbol_pos, len(names))
    else:
        data = bytearray(struct.pack("<HHIIIHH", 0x8664, 1, 0, symbol_pos, len(names), 0, 0))
    section = bytearray(40)
    section[:5] = b".text"
    struct.pack_into("<II", section, 16, 64, raw_pos)
    struct.pack_into("<I", section, 36, 0x60000020)
    data += section + bytes(64)
    for name, offset, kind, storage in names:
        data += name.encode().ljust(8, b"\0")
        data += struct.pack("<IiHBB" if big else "<IhHBB", offset, 1, kind, storage, 0)
    data += struct.pack("<I", 4)
    assert len(data) == symbol_pos + stride * len(names) + 4
    return bytes(data)


def main():
    for big in (False, True):
        data = fixture(big)
        assert coff_spans(data) == {"guest": 24, "cold": 16}
        try:
            coff_spans(data[:-1])
        except ValueError:
            pass
        else:
            raise AssertionError("truncated string table accepted")
    raw = b"""define internal void @helper(ptr %rt) {
entry:
  call void @cold(ptr %rt)
  ret void
}
define void @guest(ptr %rt) {
entry:
  call void @helper(ptr %rt)
  call void @helper(ptr %rt)
  ret void
}
define internal void @cold(ptr %rt) noinline cold {
entry:
  ret void
}
"""
    helpers = ir_inventory(raw, "guest")
    assert set(helpers) == {"helper", "cold"}
    assert helpers["helper"]["guest_direct_calls"] == 2
    assert helpers["cold"]["helper_direct_calls"] == 1
    assert helpers["helper"]["direct_dependencies"] == ["cold"]
    changed = ir_inventory(raw.replace(b"noinline cold", b"noinline"), "guest")
    assert helpers["cold"]["body_sha256"] != changed["cold"]["body_sha256"]
    print("PASS: EE helper inventory")


if __name__ == "__main__":
    main()
