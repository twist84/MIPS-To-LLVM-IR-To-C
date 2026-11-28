#!/usr/bin/env python3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from build_ee_dedup_experiment import coalesce, dispatch_map, select_routes


def fn(pc, blocks):
    return {"vaddr": hex(pc), "ir_path": f"{pc:x}.ll",
            "block_pcs": [hex(p) for p in blocks]}


def candidate(owner, pcs):
    return {"owner": owner, "status": "matching_emitted_ir",
            "all_source_dispatch_pcs_match": True,
            "source_dispatch_checks": [{"pc": hex(pc), "status": "matching_emitted_ir"}
                                       for pc in pcs]}


def row(pc, candidates):
    return {"pc": hex(pc), "source": f"{pc:x}.ll", "candidates": candidates}


def run():
    metadata = {"entry_point": "0x1000", "discovered_entries": ["0x1000"],
                "functions": [fn(0x1000, [0x1000]), fn(0x1004, [0x1004, 0x1008]),
                              fn(0x100c, [0x100c]), fn(0x1010, [0x1010])],
                "function_count": 4}
    report = {"entries": [row(0x1004, [candidate("1000.ll", [0x1004, 0x1008])]),
                          row(0x100c, [candidate("1004.ll", [0x100c])]),
                          row(0x1010, [candidate("1000.ll", [0x1010])]),
                          row(0x1000, [candidate("1010.ll", [0x1000])])]}
    seen = []
    routes = select_routes(metadata, report, lambda *args: seen.append(args[:2]))
    assert routes == {"1004.ll": "1000.ll", "1010.ll": "1000.ll"}
    assert len(seen) == 3
    merged = coalesce(metadata, routes)
    assert merged["function_count"] == 2
    assert dispatch_map(merged["functions"]) == {
        0x1000: "1000.ll", 0x1004: "1000.ll", 0x1008: "1000.ll",
        0x100c: "100c.ll", 0x1010: "1000.ll"}
    assert metadata["function_count"] == 4
    assert metadata["functions"][0]["block_pcs"] == ["0x1000"]
    # No transitive or circular owner routes.
    for bad in ({"100c.ll": "1004.ll", "1004.ll": "1000.ll"},
                {"1004.ll": "1004.ll"}):
        try:
            coalesce(metadata, bad)
        except ValueError:
            pass
        else:
            raise AssertionError("invalid route accepted")
    report["entries"][0]["candidates"][0]["source_dispatch_checks"].pop()
    try:
        select_routes(metadata, report, lambda *args: None)
    except ValueError:
        pass
    else:
        raise AssertionError("incomplete dispatch evidence accepted")



def staging():
    import hashlib
    import json
    import struct
    from tempfile import TemporaryDirectory
    from audit_ee_entry_shadow import compare
    from build_ee_dedup_experiment import stage
    from test_ee_entry_shadow import BODY, emit

    def digest(data):
        return hashlib.sha256(data).hexdigest()

    with TemporaryDirectory() as tmp:
        root = Path(tmp)
        ir, objects = root / "ir", root / "objects"
        ir.mkdir()
        objects.mkdir()
        a, b = ir / "1008.ll", ir / "1000.ll"
        source = emit(a, 0x1008, BODY)
        owner = emit(b, 0x1000,
                     "bb_0x1000:\n  ; 0x1000: nop\n  br label %bb_0x1008\n" + BODY,
                     cases=[0x1000, 0x1008, 0x1010])
        status, fingerprint = compare(source, owner, 0x1008)
        assert status == "matching_emitted_ir"
        metadata = {"entry_point": "0x1000", "discovered_entries": ["0x1000"],
                    "functions": [dict(fn(0x1000, [0x1000, 0x1010]),
                                       host_symbol="ee_fn_00001000"),
                                  dict(fn(0x1008, [0x1008]),
                                       host_symbol="ee_fn_00001008")],
                    "function_count": 2}
        meta = ir / "ee_functions.json"
        meta.write_text(json.dumps(metadata), encoding="utf-8")
        # Minimal COFF object with one executable section. The experiment
        # stages existing object files; it does not compile or link them.
        data = bytearray(72)
        struct.pack_into("<HHIIIHH", data, 0, 0x8664, 1, 0, 0, 0, 0, 0)
        struct.pack_into("<4I", data, 28, 0, 0, 12, 60)
        struct.pack_into("<I", data, 56, 0x60000020)
        records = []
        for p, symbol in ((a, "ee_fn_00001008"), (b, "ee_fn_00001000")):
            obj = objects / (p.stem + ".o")
            obj.write_bytes(data)
            records.append({"input": str(p), "object": obj.name,
                            "size_bytes": len(data), "sha256": digest(data),
                            "ir_sha256": digest(p.read_bytes()), "defined_symbols": [symbol]})
        manifest = objects / "objects.json"
        manifest.write_text(json.dumps({"complete": True, "target_triple": "x86_64-pc-windows-msvc",
                                         "optimization_level": 0, "object_count": 2,
                                         "objects": records}), encoding="utf-8")
        match = candidate("1000.ll", [0x1008])
        match.update(source_sha256=source[0], owner_sha256=owner[0])
        match["source_dispatch_checks"][0]["reachable_signature_sha256"] = fingerprint
        shadow = root / "shadow.json"
        report = {"schema": 1, "mode": "shadow_only", "metadata_sha256": digest(meta.read_bytes()),
                  "entries": [row(0x1008, [match])]}
        shadow.write_text(json.dumps(report), encoding="utf-8")
        originals = {p: p.read_bytes() for p in (a, b, meta, manifest, objects / "1008.o",
                                                objects / "1000.o")}
        receipt = stage(ir, objects, shadow, root / "experiment")
        assert receipt["retained_functions"] == 1
        assert receipt["excluded_native_executable_bytes"] == 12
        assert receipt["dispatch_pc_count"] == 3
        assert receipt["runtime_validation_required"]
        _, _, validated, linked = __import__("link_native_objects").load_inputs(
            root / "experiment" / "ee_functions.json", root / "experiment" / "objects")
        assert len(validated) == 1 and linked == [objects / "1000.o"]
        assert set(validated[0]["block_pcs"]) == {0x1000, 0x1008, 0x1010}
        assert all(p.read_bytes() == original for p, original in originals.items())
        # A changed IR cannot reuse a stale comparison or object digest.
        a.write_bytes(a.read_bytes() + b"\n")
        try:
            stage(ir, objects, shadow, root / "stale")
        except ValueError:
            pass
        else:
            raise AssertionError("stale IR accepted")
        assert not (root / "stale").exists()


def raster_validation():
    from tempfile import TemporaryDirectory
    from replay_ee_dedup_experiment import validate_raster_outputs
    with TemporaryDirectory() as tmp:
        root = Path(tmp)
        for arm, value in (("baseline", "0"), ("control", "0"), ("dedup", "1")):
            folder = root / arm
            folder.mkdir()
            (folder / "stderr.log").write_bytes(("[gpu] raster lazy_setup=" + value + "\r\n").encode())
            (folder / "report.json").write_bytes(b'{}')
            (folder / "gs-webgpu.ppm").write_bytes(b'image')
        assert validate_raster_outputs(root)["dedup"]["lazy_setup"] == "1"
        for name, wrong in (("stderr.log", b"[gpu] raster lazy_setup=0\n"),
                            ("report.json", b'{"different":true}'),
                            ("gs-webgpu.ppm", b'changed')):
            path = root / "dedup" / name
            original = path.read_bytes()
            path.write_bytes(wrong)
            try:
                validate_raster_outputs(root)
            except ValueError:
                pass
            else:
                raise AssertionError("incorrect raster replay accepted: " + name)
            path.write_bytes(original)
        for arm in ("baseline", "control", "dedup"):
            size = 8 if arm == "dedup" else 16
            (root / arm / "stderr.log").write_bytes(
                f"[gpu] raster lazy_setup=0\n[gpu] raster workgroup={size}x{size} ({size}x{size} CPU tiles)\n".encode())
        assert validate_raster_outputs(root, tile8=True)["dedup"]["tile"] == 8
        wrong = root / "dedup" / "stderr.log"
        wrong.write_bytes(b"[gpu] raster lazy_setup=0\n[gpu] raster workgroup=8x8 (16x16 CPU tiles)\n")
        try:
            validate_raster_outputs(root, tile8=True)
        except ValueError:
            pass
        else:
            raise AssertionError("tile8 accepted wrong CPU tile size")
        wrong.write_bytes(b"[gpu] raster lazy_setup=0\n[gpu] raster workgroup=8x8 (8x8 CPU tiles)\n")
        from replay_ee_graphics_profile import WORKER_TIMING_FIELDS
        import json
        for index, arm in enumerate(("baseline", "control", "dedup")):
            report = {"runtime_profile": {"schema": "ee-runtime-profile-v1", "wall_ns": index},
                      "vu1_aot": {"program_profiles": []},
                      "vu1_worker": {key: index for key in WORKER_TIMING_FIELDS},
                      "gs_fields_started": 10}
            (root / arm / "report.json").write_text(json.dumps(report))
        result = validate_raster_outputs(root, tile8=True, async_raster=True)
        assert result["dedup"]["normalized_report_sha256"] == result["baseline"]["normalized_report_sha256"]
        candidate = root / "dedup" / "report.json"
        report = json.loads(candidate.read_text())
        report["gs_fields_started"] += 1
        candidate.write_text(json.dumps(report))
        try:
            validate_raster_outputs(root, tile8=True, async_raster=True)
        except ValueError:
            pass
        else:
            raise AssertionError("async raster accepted changed guest field count")


def async_workload_validation():
    import json
    from tempfile import TemporaryDirectory
    from replay_ee_dedup_experiment import (async_workload_evidence,
                                           GRAPHICS_WORKLOAD_KEYS, VU1_WORKLOAD_KEYS)
    with TemporaryDirectory() as tmp:
        root = Path(tmp)
        for index, arm in enumerate(("baseline", "control", "dedup")):
            folder = root / arm
            folder.mkdir()
            report = {key: 10 for key in GRAPHICS_WORKLOAD_KEYS}
            report.update(vu1_aot={key: 1 for key in VU1_WORKLOAD_KEYS},
                          dispatches=40, guest_instruction_attempts=100 + index,
                          m2_runtime={"enabled": index != 2},
                          vu1_worker={"native_overlap_profile": index != 2})
            (folder / "report.json").write_text(json.dumps(report))
            (folder / "gs-webgpu.ppm").write_bytes(b"same")
        evidence = async_workload_evidence(root)
        assert evidence["graphics_workload_and_final_image_match"]
        assert not evidence["architectural_equivalence_proven"]
        (root / "dedup" / "gs-webgpu.ppm").write_bytes(b"changed")
        assert not async_workload_evidence(root)["graphics_workload_and_final_image_match"]


if __name__ == "__main__":
    run()
    staging()
    raster_validation()
    async_workload_validation()
    from tempfile import TemporaryDirectory
    from replay_ee_dedup_experiment import lean_overrides, report_options, LEAN_DIAGNOSTICS
    assert lean_overrides("all") == {key: "0" for key in LEAN_DIAGNOSTICS.values()}
    for name, key in LEAN_DIAGNOSTICS.items():
        assert lean_overrides(name) == {key: "0"}
    with TemporaryDirectory() as tmp:
        marker = Path(tmp) / "stdout.log"
        marker.write_text("startup\nEE report options: hotspots=1 boundary_timing=0 (history retained)\n")
        assert report_options(marker) == {"hotspots": True, "boundary": False}
    print("PASS: isolated EE dedup route selection and exact dispatch inventory")
