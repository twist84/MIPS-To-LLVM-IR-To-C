#!/usr/bin/env python3
"""Ensure EE trace-helper SSA is not deleted by O2/O3 textual IR passes.

The dead-temp optimizer may drop only numbered %tN temporaries. It must
never drop named SSA declarations in the instruction-attempt/PC recorder.
"""

import pathlib
import subprocess
import sys
import tempfile

import llvmlite.binding as llvm


TRACE_DEFINITIONS = (
    "  %trace_ptr_field = getelementptr i8, ptr %rt, i64 ",
    "  %trace_ptr = load ptr, ptr %trace_ptr_field",
    "  %trace_enabled = icmp ne ptr %trace_ptr, null",
    "  br i1 %trace_enabled, label %trace_window, label %done",
    "define internal void @ee_guard_guest_store_trace(",
    "  %enabled = load i8, ptr %enabled_ptr",
    "define internal void @ee_guard_guest_v0_trace(",
    "  %remaining = load i32, ptr %remaining_ptr",
)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_trace_helper_optimizer.py ee_mmi_ir_fixture")

    fixture = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="ee-trace-opt-") as temporary:
        for level in range(4):
            ir_path = pathlib.Path(temporary) / f"trace-opt-{level}.ll"
            subprocess.run(
                [str(fixture), str(ir_path), str(level)],
                check=True,
            )
            ir = ir_path.read_text(encoding="utf-8")
            for instruction in TRACE_DEFINITIONS:
                if ir.count(instruction) != 1:
                    raise AssertionError(
                        f"O{level}: guest trace helper missing or duplicates "
                        f"definition {instruction!r}"
                    )

            module = llvm.parse_assembly(ir)
            module.verify()

    print("trace_helper_optimizer: PASS (O0-O3; named trace SSA preserved)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
