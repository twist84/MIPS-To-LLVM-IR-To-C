import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from iop_chain_pilot import append_chain_entry

TABLE = "{ptr @name0, i32 4096, i32 16, i64 -1, ptr @iop_aot_fn}"
# Two synthetic regions with a controllable ordinary/abnormal exit.
ROUTER = """
@iop_region_ids = private unnamed_addr constant [4 x i8] [
  i8 0, i8 0, i8 1, i8 1
], align 1
define i32 @iop_aot_fn(ptr %c, i32 %start) {
entry:
  %budgetp = getelementptr i8, ptr %c, i64 56
  %executedp = getelementptr i8, ptr %c, i64 60
  %pcp = getelementptr i8, ptr %c, i64 64
  %nextp = getelementptr i8, ptr %c, i64 68
  %delayp = getelementptr i8, ptr %c, i64 72
  %budget = load i32, ptr %budgetp
  %gpr = load ptr, ptr %c
  %mode = load i32, ptr %gpr
  %map_offset = sub i32 %start, 4096
  %map_in_range = icmp ult i32 %map_offset, 16
  %map_id = trunc i32 %map_offset to i8
  switch i8 %map_id, label %region_0 [
    i8 0, label %region_0
    i8 1, label %region_1
  ]
region_0:
  %small = icmp ult i32 %budget, 2
  %used = select i1 %small, i32 %budget, i32 2
  store i32 %used, ptr %executedp
  store i32 4104, ptr %pcp
  store i32 4108, ptr %nextp
  %delay = icmp eq i32 %mode, 1
  %d = zext i1 %delay to i32
  store i32 %d, ptr %delayp
  %fail = icmp eq i32 %mode, 4
  %r = zext i1 %fail to i32
  ret i32 %r
region_1:
  ret i32 1
}
define i32 @iop_aot_fn_region_1(ptr %c, i32 %start) {
entry:
  %budgetp = getelementptr i8, ptr %c, i64 56
  %executedp = getelementptr i8, ptr %c, i64 60
  %pcp = getelementptr i8, ptr %c, i64 64
  %nextp = getelementptr i8, ptr %c, i64 68
  %gpr = load ptr, ptr %c
  %mode = load i32, ptr %gpr
  %budget = load i32, ptr %budgetp
  %small = icmp ult i32 %budget, 3
  %used = select i1 %small, i32 %budget, i32 3
  %zero = icmp eq i32 %mode, 2
  %count = select i1 %zero, i32 0, i32 %used
  store i32 %count, ptr %executedp
  %pc = select i1 %zero, i32 4104, i32 4112
  %next = add i32 %pc, 4
  store i32 %pc, ptr %pcp
  store i32 %next, ptr %nextp
  %fail = icmp eq i32 %mode, 3
  %r = zext i1 %fail to i32
  ret i32 %r
}
"""


class Context(ctypes.Structure):
    _fields_ = [(name, ctypes.c_void_p) for name in
                ("gpr", "hi", "lo", "ram", "cpu", "read", "write")] + [
                    (name, ctypes.c_uint32) for name in
                    ("budget", "executed", "pc", "next", "delay", "branch")]


class ChainPilotTest(unittest.TestCase):
    def test_rejects_invalid_pairs(self):
        for entry, resume in ((4096, 4100), (4097, 4104), (4096, 4112)):
            with self.assertRaises(ValueError):
                append_chain_entry(ROUTER, TABLE, entry, resume)

    @unittest.skipUnless(shutil.which("clang"), "clang unavailable")
    def test_native_chain(self):
        table = TABLE
        if os.name == "nt":
            table += "\ndefine dllexport i32 @iop_aot_module_count"
        ir = append_chain_entry(ROUTER, table, 4096, 4104)
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "chain.ll"
            lib = Path(tmp) / ("chain.dll" if os.name == "nt" else "chain.so")
            src.write_text(ir, encoding="utf-8")
            command = ["clang", "-O2", "-shared", "-fuse-ld=lld",
                       "-Wno-override-module", str(src), "-o", str(lib)]
            if os.name == "nt":
                command += ["-nostdlib", "-Wl,/noentry"]
            else:
                command += ["-fPIC"]
            subprocess.run(command, check=True)
            library = ctypes.CDLL(str(lib))
            fn = library.iop_aot_chain_entry_v1
            fn.argtypes = [ctypes.POINTER(Context), ctypes.c_uint32]
            fn.restype = ctypes.c_uint32
            for budget, mode, count, pc, reason, delay in (
                (1, 0, 1, 4104, 0, 0),
                (2, 0, 2, 4104, 0, 0),
                (3, 0, 3, 4112, 0, 0),
                (5, 0, 5, 4112, 0, 0),
                (9, 0, 5, 4112, 0, 0),
                (9, 1, 2, 4104, 0, 1),
                (9, 2, 2, 4104, 1, 0),
                (9, 3, 5, 4112, 1, 0),
                (9, 4, 2, 4104, 1, 0),
            ):
                gpr = ctypes.c_uint32(mode)
                ctx = Context(gpr=ctypes.addressof(gpr), budget=budget)
                actual = fn(ctypes.byref(ctx), 4096)
                self.assertEqual((ctx.budget, ctx.executed, ctx.pc, actual, ctx.delay),
                                 (budget, count, pc, reason, delay))
            if os.name == "nt":
                # Release the DLL before TemporaryDirectory attempts removal.
                import _ctypes
                _ctypes.FreeLibrary(library._handle)


if __name__ == "__main__":
    unittest.main()
