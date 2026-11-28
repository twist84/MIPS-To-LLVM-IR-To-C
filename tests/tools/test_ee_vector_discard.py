#!/usr/bin/env python3
"""Differential exact vector discard tests on production arithmetic templates."""

import ctypes
from pathlib import Path
import random
import re
import sys

import llvmlite.binding as llvm

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from ee_vector_discard import SHAPE, helper, vector_discard


def template(path):
    return (ROOT / path).read_text(encoding="utf-8").split('R"IR(', 1)[1].split(')IR"', 1)[0]


def arithmetic():
    text = (template("src/runtime/ee_vu_fmac4_ir.h") +
            template("src/llvm/ee_cop1_mul_ir.h"))
    names = ("vu_mul4", "vu_add4", "vu_clz26_4", "vu_mul_correct4",
             "vu_mul_booth_row4", "vu_mul_csa4",
             "ee_cop1_mul_correct", "ee_cop1_booth_row", "ee_cop1_mul_csa")
    return "\n".join([line for line in text.splitlines() if line.startswith("declare ")] +
                     [helper(text, name) for name in names])


WRAPPER = f"""
define void @probe(ptr %a, ptr %b, ptr %out, i32 %kind) {{
entry:
  %av = load <4 x i32>, ptr %a, align 4
  %bv = load <4 x i32>, ptr %b, align 4
  %mul = icmp eq i32 %kind, 0
  br i1 %mul, label %m, label %add
m:
  %mv = call {SHAPE} @vu_mul4(<4 x i32> %av, <4 x i32> %bv)
  br label %done
add:
  %sub = icmp eq i32 %kind, 2
  %sv = call {SHAPE} @vu_add4(<4 x i32> %av, <4 x i32> %bv, i1 %sub)
  br label %done
done:
  %v = phi {SHAPE} [%mv, %m], [%sv, %add]
  %bits = extractvalue {SHAPE} %v, 0
  %o = extractvalue {SHAPE} %v, 1
  %u = extractvalue {SHAPE} %v, 2
  %oi = zext <4 x i1> %o to <4 x i32>
  %ui = zext <4 x i1> %u to <4 x i32>
  %p1 = getelementptr i32, ptr %out, i32 4
  %p2 = getelementptr i32, ptr %out, i32 8
  store <4 x i32> %bits, ptr %out, align 4
  store <4 x i32> %oi, ptr %p1, align 4
  store <4 x i32> %ui, ptr %p2, align 4
  ret void
}}
"""


def compile_probe(text, opt):
    module = llvm.parse_assembly(text + WRAPPER)
    machine = llvm.Target.from_default_triple().create_target_machine(
        cpu=llvm.get_host_cpu_name(), features=llvm.get_host_cpu_features().flatten(),
        opt=opt)
    module.triple = llvm.get_default_triple()
    module.data_layout = str(machine.target_data)
    module.verify()
    if opt:
        with llvm.create_pipeline_tuning_options(opt) as tuning:
            with llvm.create_pass_builder(machine, tuning) as builder:
                with builder.getModulePassManager() as passes:
                    passes.run(module, builder)
    module.verify()
    engine = llvm.create_mcjit_compiler(module, machine)
    engine.finalize_object()
    address = engine.get_function_address("probe")
    assert address
    function = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p,
                                ctypes.c_void_p, ctypes.c_uint32)(address)
    return engine, function


def run():
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    raw = arithmetic()
    candidate, names = vector_discard(raw.encode("utf-8"))
    assert names == ["vu_mul4", "vu_add4"]
    try:
        vector_discard(candidate)
    except ValueError:
        pass
    else:
        raise AssertionError("reapplying the transform should be refused")
    rng = random.Random(257)
    lanes = ctypes.c_uint32 * 4
    results = ctypes.c_uint32 * 12
    x, y, a, b = lanes(), lanes(), results(), results()
    count = 0
    for opt in (0, 3):
        baseline_engine, baseline = compile_probe(raw, opt)
        candidate_engine, trial = compile_probe(candidate.decode("utf-8"), opt)
        for i in range(25000):
            for lane in range(4):
                lhs, rhs = rng.getrandbits(32), rng.getrandbits(32)
                if i % 5 == 0:
                    lhs &= 0x807fffff
                elif i % 5 == 1:
                    rhs &= 0x807fffff
                elif i % 5 == 2:
                    if lane % 2:
                        lhs &= 0x807fffff
                    else:
                        rhs &= 0x807fffff
                elif i % 5 == 3:
                    lhs = (lhs & 0x807fffff) | ((1 + lane) << 23)
                    rhs = (rhs & 0x807fffff) | (200 << 23)
                x[lane], y[lane] = lhs, rhs
            for kind in range(3):
                baseline(x, y, a, kind)
                trial(x, y, b, kind)
                assert list(a) == list(b), (opt, i, kind, list(x), list(y), list(a), list(b))
                count += 4
        baseline_engine.close()
        candidate_engine.close()
    print(f"Exact vector discard: {count} lane tuples matched")


if __name__ == "__main__":
    run()
