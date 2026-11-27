#!/usr/bin/env python3
"""Read-only original EE lowering cost and current native footprint audit.

Instruction counts refer to generated LLVM text, not machine instructions.
Mnemonic attribution is lexical: branch setup, shared block writebacks,
cloned paths and unannotated control lowering are not exclusive costs.
COFF symbol spans may include padding; no machine-code PC mapping is inferred.
"""
import argparse
from collections import Counter, defaultdict
import heapq
import json
from pathlib import Path
import re
import sys

from audit_ee_helper_inventory import (HEADER, END, coff_spans, ir_inventory,
                                       read_manifest, sha, verified_object)
from audit_ee_entry_shadow import basename
from ee_ownership_binary import native_executable_sections

ANNOTATION = re.compile(rb"^\s*;\s+(0x[0-9a-fA-F]+):\s+(\S+)")
LABEL = re.compile(rb"^([A-Za-z_][\w.$]*):(?:\s*;.*)?$")
ASSIGN = re.compile(rb"^(%[\w.$]+)\s*=\s*(.*)$")
COUNT = re.compile(rb"@ee_count_guest_instruction\(ptr %rt, i32 (\d+), i32 (\d+)\)")
CALL = re.compile(rb"@([\w.$]+)\s*\(")
CPU_GEP = re.compile(rb"^getelementptr(?: inbounds)? %ee_cpu_state, ptr %cpu, i32 0, i32 (\d+)")
POINTER = re.compile(rb"\bptr (%[\w.$]+)")
OPS = set(b"""ret br switch indirectbr invoke callbr resume catchswitch catchret
cleanupret unreachable fneg add fadd sub fsub mul fmul udiv sdiv fdiv urem srem
frem shl lshr ashr and or xor extractelement insertelement shufflevector
extractvalue insertvalue alloca load store fence cmpxchg atomicrmw getelementptr
trunc zext sext fptrunc fpext fptoui fptosi uitofp sitofp ptrtoint inttoptr
bitcast addrspacecast icmp fcmp phi select freeze call va_arg landingpad
catchpad cleanuppad""".split())
TARGETS = ("ee_guard_guest_store_trace", "ee_guard_guest_v0_trace",
           "ee_count_reference_annulled_slot", "ee_try_count_alu_run")
ACCOUNTING = set(TARGETS[2:]) | {"ee_count_guest_instruction",
                                  "ee_publish_instruction_observation"}
BANDS = ((1, 4), (5, 16), (17, 32), (33, 128), (129, 512))


def band(n):
    for lo, hi in BANDS:
        if lo <= n <= hi:
            return f"{lo}-{hi}"
    return "513+" if n > 512 else "no-observed-pcs"


def add_to_top(heap, row, key, limit):
    item = (row[key], row["module"], row.get("block", ""), row)
    if len(heap) < limit:
        heapq.heappush(heap, item)
    elif item[:3] > heap[0][:3]:
        heapq.heapreplace(heap, item)


def ranked(heap):
    return [item[3] for item in sorted(heap, key=lambda item: item[:3], reverse=True)]


def instruction(line, state_ptrs):
    """Return counts for one supported generated instruction, or None."""
    stripped = line.strip()
    match = ASSIGN.fullmatch(stripped)
    dest, body = (match[1], match[2]) if match else (None, stripped)
    words = body.split(None, 1)
    if not words:
        return None
    op = words[0]
    if op in (b"tail", b"musttail", b"notail"):
        op = words[1].split(None, 1)[0]
    if op not in OPS:
        return None
    key = op.decode("ascii")
    result = Counter(ir_instructions=1, ir_instruction_line_bytes=len(line))
    result["ir_op_" + key] += 1
    if op == b"getelementptr":
        gep = CPU_GEP.match(body)
        if gep and dest:
            field = int(gep[1])
            state_ptrs[dest] = field
            result["cpu_state_geps"] += 1
            result["gpr_geps"] += int(field == 0)
        if b"ptr %rt," in body:
            result["runtime_geps"] += 1
    if op in (b"load", b"store"):
        pointers = POINTER.findall(body)
        # For generated stores the address is the last pointer operand.
        ptr = pointers[-1] if pointers else None
        if ptr in state_ptrs:
            result["cpu_state_" + key + "s"] += 1
            if state_ptrs[ptr] == 0:
                result["gpr_" + key + "s"] += 1
    if op in (b"call", b"invoke", b"callbr"):
        callee = CALL.search(body)
        if callee:
            name = callee[1].decode("ascii")
            result["direct_calls"] += 1
            if name in ACCOUNTING:
                result["accounting_calls"] += 1
            if name in TARGETS:
                result["target_call_" + name] += 1
            if name.startswith(("ee_mem_", "ee_store_quadword", "ee_shared_store_quadword")):
                result["memory_helper_calls"] += 1
            if name in ("ee_raise_exception", "ee_exception_return"):
                result["exception_helper_calls"] += 1
        else:
            result["indirect_calls"] += 1
    return result


def scan_guest(raw, guest):
    matches = [m for m in HEADER.finditer(raw) if m[1].decode("ascii") == guest]
    if len(matches) != 1:
        raise ValueError("expected exactly one guest function")
    start = matches[0]
    end = END.search(raw, start.end())
    if end is None:
        raise ValueError("missing guest function end")
    # Skip the single-line function header, retain original byte lengths.
    first = raw.find(b"\n", start.end()) + 1
    if first <= 0:
        raise ValueError("unterminated guest function header")
    body = raw[first:end.start()]
    total, unassigned = Counter(), Counter()
    mnemonic = defaultdict(Counter)
    pc_cost = defaultdict(Counter)
    pc_mnemonics = defaultdict(set)
    counted, annotated = set(), set()
    blocks = {}
    current = None
    region = None
    state_ptrs = {}
    in_switch = False
    for line in body.splitlines(keepends=True):
        stripped = line.strip()
        label = LABEL.fullmatch(stripped)
        if label:
            current = label[1].decode("ascii")
            if current in blocks:
                raise ValueError("duplicate guest LLVM block")
            blocks[current] = Counter()
            total["llvm_blocks"] += 1
            if current.startswith(("bb_0x", "alu_count_", "dispatch_unknown")) or current == "entry":
                region = None
            continue
        ann = ANNOTATION.match(line)
        if ann:
            pc, name = int(ann[1], 16), ann[2].decode("utf-8")
            region = (pc, name)
            annotated.add(pc)
            pc_mnemonics[pc].add(name)
            total["annotation_occurrences"] += 1
            mnemonic[name]["annotation_occurrences"] += 1
            pc_cost[pc]["annotation_occurrences"] += 1
            if current is not None:
                blocks[current]["annotation_occurrences"] += 1
            continue
        if not stripped or stripped.startswith(b";"):
            continue
        if in_switch:
            if stripped == b"]":
                in_switch = False
            else:
                total["switch_case_lines"] += 1
            continue
        counts = instruction(line, state_ptrs)
        if counts is None:
            # Fail closed rather than silently report incomplete instruction counts.
            raise ValueError(f"unsupported generated instruction line: {stripped[:120]!r}")
        if current is None:
            raise ValueError("guest instruction outside a block")
        if stripped.startswith(b"switch ") and stripped.endswith(b"["):
            in_switch = True
        if current.startswith("fastmem_"):
            counts["fastmem_region_instructions"] += 1
        if current in ("entry", "dispatch_unknown"):
            counts["entry_dispatch_instructions"] += 1
        if stripped.startswith(b"ret "):
            counts["exit_instructions"] += 1
        call = COUNT.search(line)
        if call:
            counted.add(int(call[1]))
        total.update(counts)
        blocks[current].update(counts)
        if region is None:
            unassigned.update(counts)
        else:
            pc, name = region
            mnemonic[name].update(counts)
            pc_cost[pc].update(counts)
    if in_switch:
        raise ValueError("unterminated guest switch")
    pcs = annotated | counted
    total["guest_pcs"] = len(pcs)
    total["annotated_pcs"] = len(annotated)
    total["count_only_pcs"] = len(counted - annotated)
    total["guest_body_text_bytes"] = len(body)
    total["annotation_excess"] = total["annotation_occurrences"] - len(annotated)
    for name, values in mnemonic.items():
        values["function_scoped_annotated_pcs"] = sum(name in names for names in pc_mnemonics.values())
    return total, unassigned, mnemonic, pc_cost, blocks, pcs


def audit(ir, original, experiment, limit, hot_pcs=None):
    meta_raw = (ir / "ee_functions.json").read_bytes()
    functions = json.loads(meta_raw)["functions"]
    names = {basename(f["ir_path"]): f["host_symbol"] for f in functions}
    paths = {p.name: p for p in ir.glob("*.ll")}
    if len(names) != len(functions) or set(names) != set(paths):
        raise ValueError("IR inventory differs from metadata")
    current = experiment / "objects"
    base_raw, base, base_rows = read_manifest(original, set(names))
    overlay_raw, overlay, rows = read_manifest(current, set(names))
    plan = json.loads((experiment / "helper-plan.json").read_text(encoding="utf-8"))
    dll = experiment / "game-translated.dll"
    if (plan.get("mode") != "experimental_shared_cop1_add" or
            plan.get("metadata_sha256") != sha(meta_raw) or
            (experiment / "ee_functions.json").read_bytes() != meta_raw or
            plan.get("source_functions") != len(names) or plan.get("retained_functions") != len(names) or
            overlay.get("production_cache_compatible") is not False or
            overlay.get("experimental_mixed_pipeline") != "O5-selective-Oz-shared-observation-counter-quadword-cop1-mul-cop1-add" or
            plan.get("native_target") != overlay.get("native_target") or
            overlay.get("native_target") != base.get("native_target") or
            overlay.get("target_triple") != base.get("target_triple") or
            dll.stat().st_size != plan.get("linked_library_bytes") or
            sha(dll.read_bytes()) != plan.get("linked_library_sha256")):
        raise ValueError("expected matching cumulative COP1 addition overlay")
    trial_rows = {row["module"]: row for row in plan["modules"]}
    if (len(trial_rows) != len(plan["modules"]) or not set(trial_rows) <= set(names) or
            len(trial_rows) != plan["recompiled_modules"] or
            len(names) - len(trial_rows) != plan["reused_modules"]):
        raise ValueError("addition receipt inventory differs")
    totals, unassigned = Counter(), Counter()
    opcodes, bands = defaultdict(Counter), defaultdict(Counter)
    target_stats = defaultdict(Counter)
    module_rows, block_heap = [], []
    unique_pcs = set()
    hot = set(hot_pcs or ())
    hot_rows = defaultdict(Counter)
    for number, (name, guest) in enumerate(sorted(names.items()), 1):
        raw = paths[name].read_bytes()
        digest = sha(raw)
        if name in trial_rows:
            trial = trial_rows[name]
            record = rows[name]
            if (trial["source_ir_sha256"] != digest or
                    trial["transformed_ir_sha256"] != record.get("transformed_ir_sha256") or
                    trial["trial"]["sha256"] != record["sha256"] or
                    trial["trial"]["object_bytes"] != record["size_bytes"]):
                raise ValueError(f"addition receipt differs: {name}")
        objects = {"o5": verified_object(original, base_rows[name], digest),
                   "current": verified_object(current, rows[name], digest)}
        total, unattributed, mnemonic, pc_cost, blocks, pcs = scan_guest(raw, guest)
        inventory = ir_inventory(raw, guest)
        for profile, data in objects.items():
            executable = native_executable_sections(data)[0]
            spans = coff_spans(data)
            span = spans.get(guest, 0)
            total[profile + "_executable_bytes"] = executable
            total[profile + "_guest_named_span_bytes"] = span
            total[profile + "_guest_named_span_present"] = int(guest in spans)
            for target in TARGETS:
                if target in spans:
                    target_stats[target][profile + "_named_copies"] += 1
                    target_stats[target][profile + "_named_span_bytes"] += spans[target]
        for target in TARGETS:
            if target in inventory:
                info = inventory[target]
                t = target_stats[target]
                t["definitions"] += 1
                t["original_body_ir_bytes"] += info["ir_bytes"]
                t["guest_direct_calls"] += info["guest_direct_calls"]
                t["helper_direct_calls"] += info["helper_direct_calls"]
        total["modules"] = 1
        unique_pcs.update(pcs)
        totals.update(total)
        unassigned.update(unattributed)
        bands[band(len(pcs))].update(total)
        for opcode, costs in mnemonic.items():
            opcodes[opcode].update(costs)
        for pc in hot & pcs:
            hot_rows[pc].update(pc_cost[pc])
            hot_rows[pc]["function_scopes"] += 1
        for label, costs in blocks.items():
            add_to_top(block_heap, dict(costs, module=name, block=label),
                       "ir_instructions", limit)
        row = dict(total, module=name, host_symbol=guest,
                   unassigned_ir_instructions=unattributed["ir_instructions"],
                   original_ir_sha256=digest)
        if pcs:
            row["ir_instructions_per_guest_pc"] = total["ir_instructions"] / len(pcs)
            row["current_executable_bytes_per_guest_pc"] = total["current_executable_bytes"] / len(pcs)
        module_rows.append(row)
        if number % 1024 == 0 or number == len(names):
            print(f"[{number}/{len(names)}] lowering cost audit", flush=True)
    return {
        "schema": 1, "mode": "read_only_ee_lowering_cost",
        "metadata_sha256": sha(meta_raw), "original_manifest_sha256": sha(base_raw),
        "current_manifest_sha256": sha(overlay_raw),
        "current_library_sha256": plan["linked_library_sha256"],
        "totals": dict(totals, unique_observed_guest_pcs=len(unique_pcs)),
        "unassigned_region": unassigned, "by_mnemonic": dict(sorted(opcodes.items())),
        "by_size_band": dict(bands), "remaining_helpers": dict(target_stats),
        "largest_llvm_blocks": ranked(block_heap),
        "largest_native_modules": sorted(module_rows, key=lambda r: r["current_executable_bytes"], reverse=True)[:limit],
        "hot_pc_regions": {f"0x{pc:08x}": row for pc, row in sorted(hot_rows.items())},
        "modules": module_rows,
        "limitations": [
            "Original emitted IR text only; current overlay executable bytes are measured, not current transformed IR counts.",
            "Instruction lines count each LLVM operation once; switch cases and labels are separate.",
            "Mnemonic and PC region costs are lexical approximations, not exclusive per-instruction lowering or dynamic execution costs.",
            "Unannotated control lowering, cloned ALU paths, delay slots, shared writebacks and count-only PCs may affect attribution.",
            "Direct state-pointer classification covers recognized CPU-state GEPs only; fastmem region and helper-call metrics are not exhaustive memory or fault checks.",
            "COFF named spans are approximate and may include padding or omit inlined/renamed functions.",
            "Native executable bytes per PC are module-level ratios including helpers; no native per-opcode cost is inferred.",
            "Optional hot PCs are caller-supplied, not an inferred execution profile.",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ir", type=Path)
    parser.add_argument("objects", type=Path)
    parser.add_argument("--experiment", type=Path, required=True)
    parser.add_argument("--json", type=Path, required=True)
    parser.add_argument("--top", type=int, default=200)
    parser.add_argument("--hot-pc", action="append", type=lambda s: int(s, 0), default=[])
    args = parser.parse_args()
    try:
        output = args.json.resolve()
        if (args.top < 1 or output.exists() or any(output.is_relative_to(path.resolve())
                for path in (args.ir, args.objects, args.experiment))):
            raise ValueError("use positive --top and a new report outside caches")
        report = audit(args.ir.resolve(), args.objects.resolve(), args.experiment.resolve(),
                       args.top, args.hot_pc)
        with output.open("x", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2)
            stream.write("\n")
        total = report["totals"]
        print(f"{total['guest_pcs']:,} function-scoped guest PCs; "
              f"{total['ir_instructions']:,} original guest IR instructions")
        print(f"Native executable bytes: {total['o5_executable_bytes']:,} -> "
              f"{total['current_executable_bytes']:,}")
        for target, stats in report["remaining_helpers"].items():
            print(f"{target}: {stats.get('current_named_span_bytes', 0):,} current named span bytes; "
                  f"{stats['guest_direct_calls']:,} original direct guest calls")
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"EE lowering cost audit refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
