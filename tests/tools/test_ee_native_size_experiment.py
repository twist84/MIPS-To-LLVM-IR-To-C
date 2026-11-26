#!/usr/bin/env python3
"""Selection gate and real object replacement with unchanged source inputs."""
import sys
from pathlib import Path
from tempfile import TemporaryDirectory

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import llvmlite.binding as llvm
from benchmark_ee_native_size import emit_native, sha
from build_ee_size_experiment import candidate, should_replace


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
