"""Synthetic MIPS ELF inspection contract; no retail game assets required."""
import importlib.util
import struct
import tempfile
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "inspect_sdbz_elf_hotspots.py"
spec = importlib.util.spec_from_file_location("inspect_sdbz_elf_hotspots", SCRIPT)
inspector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inspector)


def build_elf(path):
    words = [0] * 16
    words[0] = 0x3c081001     # lui t0,0x1001
    words[1] = 0x8d09f000     # lw t1,-4096(t0) -> 0x1000f000
    words[2] = 0xad09f010     # sw t1,-4080(t0) -> 0x1000f010
    words[3] = 0x0c040008     # jal 0x00100020
    words[4] = 0             # delay-slot nop
    words[5] = 0x10000002     # beq zero,zero,0x00100020
    words[6] = 0
    words[7] = 0x0120f809     # jalr ra,t1 (only metadata resolves it)
    words[8] = 0x03e00008     # jr ra
    raw = struct.pack("<%dI" % len(words), *words)
    ident = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9)
    header = struct.pack(
        "<16sHHIIIIIHHHHHH", ident, 2, 8, 1,
        0x100000, 52, 0, 0, 52, 32, 1, 0, 0, 0
    )
    segment = struct.pack(
        "<IIIIIIII", 1, 0x100, 0x100000, 0x100000,
        len(raw), len(raw), 5, 0x1000
    )
    path.write_bytes(header + segment + bytes(0x100 - len(header) - len(segment)) + raw)


def main():
    with tempfile.TemporaryDirectory() as temp:
        binary = Path(temp) / "fixture.elf"
        build_elf(binary)
        elf = inspector.Elf(binary)
        assert elf.entry == 0x100000
        assert elf.read32(0x100004) == 0x8d09f000
        assert inspector.branch(0x0c040008, 0x10000c) == ("jal", 0x100020)
        assert inspector.branch(0x10000002, 0x100014) == ("beq", 0x100020)
        assert inspector.local_base(elf, 0x100004, 8) == 0x10010000
        assert inspector.local_base(elf, 0x100008, 8) == 0x10010000
        refs = inspector.memory_refs(elf, [0x1000f000, 0x1000f010])
        assert refs[0x1000f000][0][0:2] == (0x100004, "lw"), refs
        assert refs[0x1000f010][0][0:2] == (0x100008, "sw"), refs

        metadata = [{
            "vaddr": "0x00100020", "size": 8,
            "name": "fixture", "block_pcs": ["0x00100020"],
        }]
        jalr = [{
            "pc": "0x0010001c", "target": "0x00100020",
            "status": "resolved_executable",
        }]
        report = inspector.analyze(
            elf, [0x100020], [0x1000f000, 0x1000f010], metadata, jalr
        )
        for expected in [
            "HOTSPOT 0x00100020",
            "fixture entry=0x00100020",
            "0010000c jal",
            "00100014 beq",
            "0010001c jalr(metadata)",
            "0x1000f000: 1 local-constant candidate references",
            "0x1000f010: 1 local-constant candidate references",
            "[read] lw",
            "[write] sw",
        ]:
            assert expected in report, expected
        for address in (0x10001c, 0x100020):
            assert elf.read32(address) is not None
    print("inspect_sdbz_elf_hotspots: PASS")


if __name__ == "__main__":
    main()
