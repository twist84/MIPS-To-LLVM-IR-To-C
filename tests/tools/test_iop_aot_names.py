#!/usr/bin/env python3
"""Host-only IOP AOT naming fixtures; no retail captures required."""
import json
from pathlib import Path
import struct
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from build_iop_aot import prepare_captures, recover_export_library


def capture(directory, stem, name, base, export=None, binary=None):
    entries = [base + 4, base + 8]
    if binary is None:
        binary = (struct.pack("<III", 0x41c00000, 0, 0x101) +
                  b"CRI_ADXI" + struct.pack("<IIII", entries[0], 0, entries[1], 0))
    metadata = {"name": name, "base": base, "hash": "1234567890abcdef",
                "exports": entries, "text_size": len(binary)}
    if export is not None:
        metadata["export_library"] = export
    (directory / f"{stem}.json").write_text(json.dumps(metadata), encoding="utf-8")
    (directory / f"{stem}.bin").write_bytes(binary)
    return metadata, binary


def main():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        original = root / "original"
        original.mkdir()
        legacy, binary = capture(original, "CRI_ADX_Driver@000513b0",
                                 "CRI_ADX_Driver", 0x513b0)
        assert recover_export_library(binary, legacy) == "CRI_ADXI"
        assert recover_export_library(binary, {"exports": [0x1234]}) == ""
        assert recover_export_library(binary[:-4], legacy) == ""
        capture(original, "other@00060000", "verbose_module", 0x60000,
                export="ARKD_DVD", binary=b"\0" * 16)
        capture(original, "fallback@00070000", "no exports", 0x70000,
                binary=b"\0" * 16)
        output = root / "output"
        assert prepare_captures(original, output) == 3
        assert {p.stem for p in output.glob("*.json")} == {
            "CRI_ADXI", "ARKD_DVD", "no_exports"}
        staged = json.loads((output / "CRI_ADXI.json").read_text())
        assert staged == dict(legacy, name="CRI_ADXI")
        assert (output / "CRI_ADXI.bin").read_bytes() == binary
        assert json.loads((original / "CRI_ADX_Driver@000513b0.json").read_text()) == legacy

        # Preserve independently linked variants, including Windows
        # case-insensitive collisions and equal base/different image names.
        capture(original, "variant1", "other", 0x60000, export="cri_adxi")
        capture(original, "variant2", "other", 0x60000, export="CRI_ADXI")
        collision = root / "collision"
        assert prepare_captures(original, collision) == 5
        stems = [p.stem for p in collision.glob("*.json")]
        assert len(set(s.casefold() for s in stems)) == 5
        assert "CRI_ADXI@000513b0" in stems
        assert sum(s.casefold().startswith("cri_adxi@00060000") for s in stems) == 2
        assert "ARKD_DVD" in stems
        # Some IRXs have only internal display titles in old captures.
        # The captured LOADFILE name is authoritative when present.
        unnamed = root / "unnamed"
        unnamed.mkdir()
        capture(unnamed, "adx", "CRI_ADX_Driver", 0x513b0,
                binary=b"\0" * 16)
        capture(unnamed, "arkd", "ARIKA_IOP_Control_Driver", 0x67570,
                binary=b"\0" * 16)
        capture(unnamed, "named", "verbose_title", 0x80000,
                export="OTHERLIB", binary=b"\0" * 16)
        named = unnamed / "named.json"
        metadata = json.loads(named.read_text())
        metadata["source_name"] = "ORIGINAL"
        named.write_text(json.dumps(metadata))
        repaired = root / "repaired"
        assert prepare_captures(unnamed, repaired) == 3
        assert {p.stem for p in repaired.glob("*.json")} == {
            "CRI_ADXI", "ARKD_DVD", "ORIGINAL"}
        assert json.loads((repaired / "CRI_ADXI.json").read_text())["base"] == 0x513b0
        assert json.loads((repaired / "ARKD_DVD.json").read_text())["base"] == 0x67570
    print("IOP AOT export names: PASS")


if __name__ == "__main__":
    main()
