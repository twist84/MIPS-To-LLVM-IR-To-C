#!/usr/bin/env python3
"""Native size pilot: deterministic sampling and actual LLVM object emission."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import llvmlite.binding as llvm
from benchmark_ee_native_size import MODES, band_for, emit_native, select_modules, summarize, size_attribute_ir, verify_size_attributes


def run():
    rows = [{"module": f"{i}.ll", "guest_pcs": pcs, "native_executable_bytes": size}
            for i, (pcs, size) in enumerate(((1, 10), (2, 20), (4, 30),
                                             (5, 40), (32, 50), (700, 1000)))]
    assert band_for(4) == "1-4" and band_for(513) == "513+"
    selected = select_modules(rows, 2, 1, ["1.ll"])
    assert set(selected) == {row["module"] for row in rows}
    assert selected["5.ll"] == ["band:513+", "largest"]
    assert selected["1.ll"] == ["explicit"]
    raw = b"""define i32 @ee_fn_00001000(ptr %cpu, ptr %rt) {
entry:
  %x = load i32, ptr %cpu
  %y = add i32 %x, 3
  store i32 %y, ptr %cpu
  ret i32 1
}
"""
    # Exercise the LLVM 22 textual attribute path even on LLVM 20 CI.
    fixture = raw.decode() + "\n" + """declare void @external()
define internal void @helper() alwaysinline {
entry:
  ret void
}
"""
    for size_level in (1, 2):
        annotated, count, attrs = size_attribute_ir(fixture, size_level)
        assert count == 2
        with llvm.parse_assembly(annotated) as module:
            verify_size_attributes(module, count, attrs)
            assert b"optsize" in b" ".join(module.get_function("helper").attributes)
            assert (b"minsize" in b" ".join(module.get_function("helper").attributes)) == (size_level == 2)
            assert not list(module.get_function("external").attributes)
    results = {mode: emit_native(raw, llvm.get_default_triple(), "ee_fn_00001000", mode)
               for mode in MODES}
    for mode, result in results.items():
        assert result["executable_bytes"] > 0
        assert result["object_bytes"] >= result["executable_bytes"]
        assert len(result["sha256"]) == 64
        result["executable_delta_vs_reemitted_o5"] = (
            result["executable_bytes"] - results["O5"]["executable_bytes"])
    assert results["O5"]["middle_end_size_level"] is None
    assert results["Oz"]["middle_end_size_level"] == 2
    assert results["Oz"]["size_optimization_method"] == (
        "function_attributes" if llvm.llvm_version_info[0] >= 22 else "pipeline_level")
    assert summarize([{"variants": results}])["O5"]["executable_delta_vs_o5"] == 0
    print("PASS: native size pilot sample selection and four emitted pipelines")


if __name__ == "__main__":
    run()
