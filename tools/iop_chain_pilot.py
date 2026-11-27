"""Optional single-edge IOP chaining entry, separate from the legacy ABI entry."""
from audit_iop_exit_ownership import read_router, owner


def append_chain_entry(router, table, entry, resume):
    (base, _), ids = read_router(router, table)
    source = owner(entry, base, ids)
    target = owner(resume, base, ids)
    if source is None or target is None or source == target:
        raise ValueError("chain pilot requires distinct owned ordinary entry/resume PCs")
    if "@iop_aot_chain_entry_v1" in router:
        raise ValueError("chain entry already exists")
    if not (0 <= entry < 0x80000000 and 0 <= resume < 0x7ffffffc):
        raise ValueError("chain pilot requires physical PCs")
    # Export only this alternative entry. The original module descriptor and
    # router stay unchanged. The host must authorize the device-safe budget.
    export = "dllexport " if "define dllexport i32 @iop_aot_module_count" in table else ""
    return router + f"""
define {export}i32 @iop_aot_chain_entry_pc_v1() {{
entry:
  ret i32 {entry}
}}

define {export}i32 @iop_aot_chain_entry_v1(ptr %c, i32 %start) {{
entry:
  %budgetp = getelementptr i8, ptr %c, i64 56
  %executedp = getelementptr i8, ptr %c, i64 60
  %pcp = getelementptr i8, ptr %c, i64 64
  %nextp = getelementptr i8, ptr %c, i64 68
  %delayp = getelementptr i8, ptr %c, i64 72
  %budget = load i32, ptr %budgetp
  %reason = call i32 @iop_aot_fn(ptr %c, i32 %start)
  %first = load i32, ptr %executedp
  %pc = load i32, ptr %pcp
  %next = load i32, ptr %nextp
  %delay = load i32, ptr %delayp
  %entry_ok = icmp eq i32 %start, {entry}
  %resume_ok = icmp eq i32 %pc, {resume}
  %next_ok = icmp eq i32 %next, {resume + 4}
  %delay_ok = icmp eq i32 %delay, 0
  %normal = icmp eq i32 %reason, 0
  %progress = icmp ne i32 %first, 0
  %remaining = icmp ult i32 %first, %budget
  %pair = and i1 %entry_ok, %resume_ok
  %ordinary = and i1 %next_ok, %delay_ok
  %exit_ok = and i1 %normal, %progress
  %a = and i1 %pair, %ordinary
  %b = and i1 %exit_ok, %remaining
  %chain = and i1 %a, %b
  br i1 %chain, label %second, label %done
second:
  %left = sub i32 %budget, %first
  store i32 %left, ptr %budgetp
  %reason2 = call i32 @iop_aot_fn_region_{target}(ptr %c, i32 {resume})
  %count2 = load i32, ptr %executedp
  %total = add i32 %first, %count2
  store i32 %total, ptr %executedp
  store i32 %budget, ptr %budgetp
  ; A zero-progress ordinary return would have interpreted one instruction
  ; in the outer dispatcher. Preserve that behavior after combining counts.
  %zero = icmp eq i32 %count2, 0
  %result = select i1 %zero, i32 1, i32 %reason2
  ret i32 %result
done:
  ret i32 %reason
}}
"""
