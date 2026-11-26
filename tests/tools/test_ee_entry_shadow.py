#!/usr/bin/env python3
"""Shadow comparison exercises exact emitted dispatch and reachable effects."""
import json
from pathlib import Path
import sys
from tempfile import TemporaryDirectory

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from audit_ee_entry_shadow import analyze, compare, read_module

HEADER = "%ee_cpu_state = type { i32, i32, i32, i32, i32, i32, i32 }\ndeclare void @fault(ptr, i32)\n"
BODY = """bb_0x1008:
  ; 0x1008: lw
  %t10 = load i32, ptr %cpu
  %t11 = icmp eq i32 %t10, 0
  br i1 %t11, label %fault_10, label %ok_10
fault_10:
  call void @fault(ptr %rt, i32 4104)
  ret i32 4
ok_10:
  store i32 %t10, ptr %cpu
  br label %bb_0x1010
bb_0x1010:
  ; 0x1010: jr
  ret i32 1
"""


def emit(path, entry, body, *, cases=None, header=HEADER, prologue=""):
    cases = cases if cases is not None else [entry, 0x1010]
    lines = [f"    i32 {pc}, label %bb_0x{pc:x}" for pc in cases]
    lines.append(f"    i32 0, label %bb_0x{entry:x}")
    path.write_text(header + f"define i32 @ee_fn_{entry:08x}(ptr %cpu, ptr %rt) {{\n"
                    "entry:\n" + prologue +
                    "  %t0 = getelementptr %ee_cpu_state, ptr %cpu, i32 0, i32 6\n"
                    "  %t1 = load i32, ptr %t0\n"
                    "  switch i32 %t1, label %dispatch_unknown [\n" +
                    "\n".join(lines) + "\n  ]\n"
                    "dispatch_unknown:\n  ret i32 3\n" + body + "}\n",
                    encoding="utf-8")
    return read_module(path)


def run():
    with TemporaryDirectory() as tmp:
        root = Path(tmp)
        a, b = root / "a.ll", root / "b.ll"
        source = emit(a, 0x1008, BODY)
        owner_body = ("bb_0x1000:\n  ; 0x1000: nop\n  br label %bb_0x1008\n" +
                      BODY.replace("%t10", "%t80").replace("%t11", "%t81")
                      .replace("fault_10", "fault_80").replace("ok_10", "ok_80"))
        owner = emit(b, 0x1000, owner_body, cases=[0x1000, 0x1008, 0x1010])
        status, fingerprint = compare(source, owner, 0x1008)
        assert status == "matching_emitted_ir" and len(fingerprint) == 64
        assert compare(source, owner, 0)[0] == "source_not_exact_resume_case"
        assert compare(source, emit(b, 0x1000, owner_body, cases=[0x1000, 0x1010]),
                       0x1008)[0] == "owner_not_exact_resume_case"
        for old, new in [("i32 4104", "i32 4108"), ("ret i32 4", "ret i32 3"),
                         ("store i32 %t80", "store i32 0"),
                         ("br label %bb_0x1010", "br label %bb_0x1000")]:
            altered = emit(b, 0x1000, owner_body.replace(old, new),
                           cases=[0x1000, 0x1008, 0x1010])
            assert compare(source, altered, 0x1008)[0] == "reachable_ir_differs"
        altered = emit(b, 0x1000, owner_body, cases=[0x1000, 0x1008, 0x1010],
                       header=HEADER + "declare void @different()\n")
        assert compare(source, altered, 0x1008)[0] == "module_environment_differs"
        altered = emit(b, 0x1000, owner_body, cases=[0x1000, 0x1008, 0x1010],
                       prologue="  %quadword_read_slot = alloca { i64, i64 }, align 16\n")
        assert compare(source, altered, 0x1008)[0] == "entry_prologue_differs"
        # End-to-end shadow report: metadata ownership is not silently
        # transferred; covered instructions and legal cases are separate.
        emit(b, 0x1000, owner_body, cases=[0x1000, 0x1008, 0x1010])
        metadata = {"discovered_entries": ["0x1000"], "functions": [
            {"vaddr": "0x1000", "ir_path": "b.ll", "block_pcs": ["0x1000", "0x1010"]},
            {"vaddr": "0x1008", "ir_path": "a.ll", "block_pcs": ["0x1008"]}]}
        (root / "ee_functions.json").write_text(json.dumps(metadata))
        baseline = {"aggregate_ir_bytes": a.stat().st_size + b.stat().st_size,
                    "module_details": [{"module": p.name, "shared_guest_pcs": 2,
                                        "native_executable_bytes": 100} for p in (a, b)]}
        result = analyze(root, baseline)
        assert result["targets_with_matching_emitted_ir"] == 1
        assert result["entries"][0]["metadata_dispatch_owners"] == ["a.ll"]
        assert result["entries"][0]["candidates"][0]["owner_static"]
        assert result["entries"][0]["candidates"][0]["all_source_dispatch_pcs_match"]
        assert not result["entries"][0]["candidates"][0]["owner_has_published_entry"]
        # Stale inventory is rejected before analysis.
        baseline["aggregate_ir_bytes"] += 1
        try:
            analyze(root, baseline)
        except ValueError:
            pass
        else:
            raise AssertionError("stale baseline accepted")



def generated(exe):
    """Exercise the actual EE emitter's entry selector and SSA/CFG syntax."""
    import struct
    from test_recomp_discovery_cache import compile_ir, make_elf
    with TemporaryDirectory() as tmp:
        root = Path(tmp)
        elf = root / "synthetic.elf"
        make_elf(elf)
        image = bytearray(elf.read_bytes())
        # Conditional branch with a nontrivial delay slot and a join.
        for offset, instruction in (
            (0x108, 0x10800003),  # beq a0,zero,0x100018
            (0x10c, 0x24420001),  # addiu v0,v0,1
            (0x110, 0x24420002),
            (0x114, 0x00000000),
            (0x118, 0x24420003),
            (0x11c, 0x03e00008),
            (0x120, 0x00000000),
        ):
            struct.pack_into("<I", image, offset, instruction)
        elf.write_bytes(image)
        for name, entry in (("owner", 0x100008), ("source", 0x100018)):
            out = root / name
            result = compile_ir(exe, elf, out, entry)
            assert result.returncode == 0, result.stdout + result.stderr
            meta = json.loads((out / "ee_functions.json").read_text())
            path = out / Path(meta["functions"][0]["ir_path"]).name
            module = read_module(path)
            assert compare(module, module, entry)[0] == "matching_emitted_ir"
            if name == "owner":
                owner = module
                assert 0x100018 in owner[3], owner[3]
            else:
                source = module
        status, _ = compare(source, owner, 0x100018)
        # Different function-scoped publication or safepoint choices are
        # legitimate reasons to refuse routing; never require a positive
        # certificate merely because the guest block starts agree.
        assert status in ("matching_emitted_ir", "reachable_ir_differs",
                          "module_environment_differs", "entry_prologue_differs"), status
        print("Generated internal entry:", status)


if __name__ == "__main__":
    run()
    if len(sys.argv) > 1:
        generated(sys.argv[1])
    print("PASS: EE internal-entry shadow comparison")
