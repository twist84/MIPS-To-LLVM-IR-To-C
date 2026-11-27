#!/usr/bin/env python3
"""Selection gate and real object replacement with unchanged source inputs."""
import sys
import json
from pathlib import Path
from tempfile import TemporaryDirectory

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import llvmlite.binding as llvm
from benchmark_ee_native_size import emit_native, sha
from build_ee_size_experiment import candidate, should_replace, reuse_trials


def run():
    assert should_replace(100, 200, 99, 199)
    assert not should_replace(100, 200, 100, 199)
    assert not should_replace(100, 200, 99, 200)
    assert not should_replace(100, 200, 101, 100)
    raw = b"""define i32 @ee_fn_00001000(ptr %cpu, ptr %rt) {
entry:
  %a = load i32, ptr %cpu
  %b = add i32 %a, 1
  store i32 %b, ptr %cpu
  ret i32 1
}
"""
    with TemporaryDirectory() as tmp:
        root = Path(tmp)
        ir, obj, out = root / "test.ll", root / "base.o", root / "candidate.o"
        ir.write_bytes(raw)
        base = emit_native(raw, llvm.get_default_triple(), "ee_fn_00001000", "O5",
                           object_output=obj)
        record = {"ir_sha256": sha(raw), "sha256": base["sha256"],
                  "size_bytes": base["object_bytes"]}
        row = candidate(ir, obj, record,
                        {"native_executable_bytes": base["executable_bytes"], "guest_pcs": 1},
                        "ee_fn_00001000", llvm.get_default_triple(), out)
        assert out.exists() == row["accepted"]
        assert ir.read_bytes() == raw and sha(obj.read_bytes()) == base["sha256"]
        if row["accepted"]:
            assert sha(out.read_bytes()) == row["trial"]["sha256"]
        # A rejected trial can be reused without emitting its object again.
        old = root / "previous"
        (old / "objects").mkdir(parents=True)
        metadata_bytes, manifest_bytes = b"{}", b"{}"
        (old / "ee_functions.json").write_bytes(metadata_bytes)
        name = "test.ll"
        prior = dict(row, accepted=False, executable_saving=0, object_saving=0)
        prior["trial"] = dict(row["trial"], executable_bytes=base["executable_bytes"])
        target = {"cpu": "test"}
        plan = {"mode": "experimental_selective_native_size",
                "metadata_sha256": sha(metadata_bytes), "source_manifest_sha256": sha(manifest_bytes),
                "native_target": target, "max_guest_pcs": 32, "attempted_modules": 1,
                "replaced_modules": 0, "executable_saving": 0, "object_saving": 0,
                "modules": [prior]}
        (old / "size-plan.json").write_text(json.dumps(plan))
        (old / "objects" / "objects.json").write_text(json.dumps({
            "experimental_mixed_pipeline": "O5-selective-Oz", "production_cache_compatible": False,
            "objects": [dict(record, input=name)]}))
        audit = {name: {"guest_pcs": 1, "native_executable_bytes": base["executable_bytes"]}}
        args = (old, metadata_bytes, manifest_bytes, {"native_target": target},
                audit, {name: record}, [name], 128)
        results, replacements = reuse_trials(*args)
        assert results == {name: prior} and replacements == {}
        plan["source_manifest_sha256"] = "wrong"
        (old / "size-plan.json").write_text(json.dumps(plan))
        try:
            reuse_trials(*args)
        except ValueError:
            pass
        else:
            raise AssertionError("stale previous manifest accepted")
        ir.write_bytes(raw + b"\n")
        try:
            candidate(ir, obj, record,
                      {"native_executable_bytes": base["executable_bytes"], "guest_pcs": 1},
                      "ee_fn_00001000", llvm.get_default_triple(), root / "stale.o")
        except ValueError:
            pass
        else:
            raise AssertionError("stale IR accepted")
    print("PASS: selective native size replacement gate")


if __name__ == "__main__":
    run()
