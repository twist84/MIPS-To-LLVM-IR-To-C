#!/usr/bin/env python3
"""End-to-end native object, link, and dispatcher smoke test."""

import ctypes
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile


class U128(ctypes.Structure):
    _fields_ = [("lo", ctypes.c_uint64), ("hi", ctypes.c_uint64)]


class EEState(ctypes.Structure):
    _fields_ = [
        ("gpr", U128 * 32),
        ("hi", ctypes.c_uint64),
        ("lo", ctypes.c_uint64),
        ("hi1", ctypes.c_uint64),
        ("lo1", ctypes.c_uint64),
        ("sa", ctypes.c_uint32),
        ("pc", ctypes.c_uint32),
        ("exit_reason", ctypes.c_uint32),
        ("pad", ctypes.c_uint32),
        ("cop0", ctypes.c_uint32 * 32),
        ("fpr", ctypes.c_uint64 * 32),
        ("vu0", U128 * 32),
        ("fcsr", ctypes.c_uint32),
        ("fpu_acc", ctypes.c_uint32),
        ("fpu_acc_overflow", ctypes.c_uint32),
    ]


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def make_aligned_state():
    backing = ctypes.create_string_buffer(ctypes.sizeof(EEState) + 15)
    address = ctypes.addressof(backing)
    aligned_address = (address + 15) & ~15
    state = ctypes.cast(aligned_address, ctypes.POINTER(EEState)).contents
    return backing, state


def main():
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: test_object_emission.py compare_fixture verify_ir_script "
            "link_native_objects_script source_root"
        )

    fixture = pathlib.Path(sys.argv[1]).resolve()
    verify_script = pathlib.Path(sys.argv[2]).resolve()
    link_script = pathlib.Path(sys.argv[3]).resolve()
    source_root = pathlib.Path(sys.argv[4]).resolve()

    with tempfile.TemporaryDirectory(prefix="ee-object-emission-") as temp:
        root = pathlib.Path(temp)
        ir_path = root / "scalar_compare.ll"
        object_dir = root / "objects"
        metadata_path = root / "ee_functions.json"
        library_path = root / "libguest_compare.so"

        subprocess.run([str(fixture), str(ir_path)], check=True)
        subprocess.run(
            [
                sys.executable,
                str(verify_script),
                str(ir_path),
                "--emit-dir=" + str(object_dir),
                "--progress=1",
            ],
            check=True,
        )

        object_path = object_dir / "scalar_compare.o"
        manifest_path = object_dir / "objects.json"
        require(object_path.is_file(), "native object file was not written")
        require(manifest_path.is_file(), "object manifest was not written")

        object_data = object_path.read_bytes()
        require(len(object_data) > 256, "native object is unexpectedly small")
        require(object_data[:4] == b"\x7fELF",
                "host object is not an ELF relocatable object")

        # The static cost audit uses the same backend profile as the
        # production object emitter and must not rewrite the source.
        original_ir = ir_path.read_bytes()
        audit_json = root / "ir-cost.json"
        subprocess.run([
            sys.executable, str(source_root / "tools" / "audit_ir_cost.py"),
            str(ir_path), "--profile", "0", "--compare-dir", str(root),
            "--json-out", str(audit_json),
        ], check=True)
        audit = json.loads(audit_json.read_text(encoding="utf-8"))
        require(audit["profile"] == 0, audit)
        row = audit["modules"][0]
        require(row["before"] == row["after"], row)
        require(row["before"]["object_bytes"] == len(object_data), row)
        require(row["before"]["source_ir"] == row["before"]["pipeline_ir"], row)
        require(row["before"]["native_assembly"]["instructions"] > 0, row)
        require(ir_path.read_bytes() == original_ir, "cost audit rewrote source IR")


        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        require(manifest["complete"], "object manifest reports incomplete emission")
        require(manifest["input_module_count"] == 1, "wrong input module count")
        require(manifest["object_count"] == 1, "wrong emitted object count")
        require(manifest["target_triple"] == manifest["target_triple"].strip(),
                "target triple is empty or malformed")

        entry = manifest["objects"][0]
        require(entry["object"] == object_path.name, "manifest object path mismatch")
        require(entry["size_bytes"] == len(object_data), "manifest object size mismatch")
        require(
            entry["sha256"] == hashlib.sha256(object_data).hexdigest(),
            "manifest object digest mismatch",
        )
        require(
            "guest_scalar_compare_test" in entry["defined_symbols"],
            "manifest is missing the generated guest function symbol",
        )

        # Build the same metadata shape produced by ee_recomp. The dispatcher
        # needs the guest entry and all block PCs in addition to native symbols.
        metadata = {
            "entry_point": "0x00120000",
            "function_count": 1,
            "functions_discovered": 1,
            "functions": [
                {
                    "name": "scalar_compare_test",
                    "host_symbol": "guest_scalar_compare_test",
                    "vaddr": "0x00120000",
                    "block_count": 1,
                    "block_pcs": ["0x00120000"],
                    "ir_path": str(ir_path),
                }
            ],
        }
        metadata_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")

        subprocess.run(
            [
                sys.executable,
                str(link_script),
                str(metadata_path),
                str(object_dir),
                str(library_path),
                "--source-root",
                str(source_root),
            ],
            check=True,
        )

        link_manifest = library_path.with_suffix(library_path.suffix + ".link.json")
        require(library_path.is_file(), "shared native library was not linked")
        require(link_manifest.is_file(), "linked library manifest was not written")
        linked = json.loads(link_manifest.read_text(encoding="utf-8"))
        require(linked["complete"], "link manifest reports incomplete linking")
        require(linked["object_count"] == 1, "wrong linked object count")
        require(linked["dispatcher_entry"] == "ee_game_run",
                "host dispatcher export is missing from link manifest")

        library = ctypes.CDLL(str(library_path))
        game_run = library.ee_game_run
        game_run.argtypes = [
            ctypes.POINTER(EEState),
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_uint32),
        ]
        game_run.restype = ctypes.c_uint32

        backing, state = make_aligned_state()
        _ = backing
        mask64 = 0xFFFFFFFFFFFFFFFF
        state.gpr[4].lo = mask64 - 2
        state.gpr[5].lo = 1
        state.gpr[6].lo = 0
        state.gpr[7].lo = mask64
        state.gpr[10].lo = mask64
        state.gpr[11].lo = 0
        state.gpr[12].lo = mask64
        state.gpr[13].lo = 1
        for reg in (2, 3, 6, 7, 8, 9, 10, 12):
            state.gpr[reg].hi = 0xA500000000000000 | reg

        dispatch_count = ctypes.c_uint32()
        guest_pc = ctypes.c_uint32()
        dispatch_status = ctypes.c_uint32()
        exit_reason = game_run(
            ctypes.pointer(state),
            None,
            8,
            ctypes.byref(dispatch_count),
            ctypes.byref(guest_pc),
            ctypes.byref(dispatch_status),
        )

        require(exit_reason == 3, f"dispatcher returned exit {exit_reason}, expected DISPATCH")
        require(state.exit_reason == 3, "CPU state exit reason was not updated")
        require(dispatch_count.value == 1,
                f"expected one translated call, got {dispatch_count.value}")
        require(guest_pc.value == 0x00120024,
                f"wrong dispatcher continuation PC 0x{guest_pc.value:08x}")
        require(dispatch_status.value == 0,
                f"expected EXITED dispatcher status, got {dispatch_status.value}")

        expected = {2: 1, 3: 1, 6: 1, 7: 0, 8: 1, 9: 0, 10: 1, 12: 1, 0: 0}
        for reg, value in expected.items():
            require(state.gpr[reg].lo == value,
                    f"GPR[{reg}] expected {value}, got 0x{state.gpr[reg].lo:x}")
        for reg in (2, 3, 6, 7, 8, 9, 10, 12):
            expected_hi = 0xA500000000000000 | reg
            require(state.gpr[reg].hi == expected_hi,
                    f"GPR[{reg}] upper half changed: 0x{state.gpr[reg].hi:x}")

    print("object_emission_and_dispatch: PASS")


if __name__ == "__main__":
    main()
