#!/usr/bin/env python3
"""Exact-body cross-module scalar helper sharing over the expanded overlay."""
import argparse
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

from audit_ee_helper_inventory import ir_inventory, read_manifest, verified_object, sha
from audit_ee_scalar_memory_pilot import (outline, reconstruct_parent, verify_pilot_parent,
                                         plans_without_modules, read_json, stable_receipt, winners)
from benchmark_ee_native_size import emit_native
from build_ee_cop1_mul_experiment import rows_of, check_object
from build_ee_helper_experiment import refs
from build_ee_quadword_experiment import body_of
from ee_ownership_binary import native_executable_sections
from link_native_objects import load_inputs

LOCAL = "ee_pilot_scalar_"
SHARED = "ee_shared_scalar_"
HELPERS = tuple(LOCAL + operation + str(bits)
                for operation in ("read", "write") for bits in (8, 16, 32, 64))


def shared_name(name):
    return SHARED + name[len(LOCAL):]


def share_helpers(raw, guest, counts, expected, owners, module):
    inventory = ir_inventory(raw, guest)
    selected = {n for n in HELPERS if counts.get(n, 0)}
    if ({n for n in inventory if n.startswith(LOCAL)} != selected or
            any(refs(raw, shared_name(n)) for n in HELPERS)):
        raise ValueError("scalar helper inventory differs")
    result = raw
    for name in sorted(selected):
        operation, bits = re.fullmatch(r"(read|write)(8|16|32|64)", name[len(LOCAL):]).groups()
        ty = ("i" + bits).encode()
        write = operation == "write"
        ret = b"void" if write else ty
        args = b"ptr %rt, i32 %address" + (b", " + ty + b" %value" if write else b"")
        types = b"ptr, i32" + (b", " + ty if write else b"")
        target = "ee_mem_" + operation + bits
        body = body_of(raw, name)
        header = b"define internal " + ret + b" @" + name.encode() + b"(" + args + b") noinline {"
        info = inventory[name]
        if (body.splitlines()[0] != header or sha(body) != expected[name] or
                info["direct_dependencies"] != [target] or
                info["guest_direct_calls"] != counts[name] or info["helper_direct_calls"] or
                refs(raw, name) != counts[name] + 1):
            raise ValueError("scalar helper body, dependency or usage differs: " + name)
        declaration = (rb"^declare[ \t]+" + ret + rb"[ \t]+@" + target.encode() +
                       rb"[ \t]*\([ \t]*" + re.escape(types) + rb"[ \t]*\)[ \t]*\r?$")
        if len(re.findall(declaration, raw, re.M)) != 1 or any(
                m.group(1).decode() == target for m in
                re.finditer(rb"^define\b[^\r\n]*?@([\w.$]+)\(", raw, re.M)):
            raise ValueError("scalar fallback ABI differs: " + name)
        shared = shared_name(name).encode()
        if owners[name] == module:
            replacement = body.replace(header, b"define " + ret + b" @" + shared +
                                       b"(" + args + b") noinline {", 1)
        else:
            replacement = b"declare " + ret + b" @" + shared + b"(" + types + b") noinline"
        result = result.replace(body, replacement, 1)
        result = re.sub(rb"@" + name.encode() + rb"(?![\w.$])", b"@" + shared, result)
        if refs(result, name):
            raise ValueError("incomplete scalar rewrite")
    return result


def local_parent(task):
    name, guest, path, base_record, receipts, plans, scalar_row = task
    raw = path.read_bytes()
    if sha(raw) != base_record["ir_sha256"]:
        raise ValueError("original scalar IR differs: " + name)
    before, prior_row = reconstruct_parent(raw, guest, name, receipts, plans)
    if (prior_row["transformed_ir_sha256"] != base_record["transformed_ir_sha256"] or
            prior_row["trial"]["sha256"] != base_record["sha256"] or
            scalar_row["parent_ir_sha256"] != sha(before) or
            scalar_row["parent_object_sha256"] != base_record["sha256"] or
            scalar_row["original_ir_sha256"] != sha(raw)):
        raise ValueError("scalar parent reconstruction differs: " + name)
    local, counts = outline(before, guest)
    if counts != scalar_row["counts"] or sha(local) != scalar_row["candidate_ir_sha256"]:
        raise ValueError("local scalar outline differs: " + name)
    return raw, local, counts


def trial(task):
    local_task, obj_dir, record, triple, expected, owners, out = task
    name, guest = local_task[:2]
    scalar_row = local_task[-1]
    raw, local, counts = local_parent(local_task)
    data = verified_object(obj_dir, record, sha(raw))
    mode = record.get("experimental_pipeline", "O5")
    if (mode != scalar_row["mode"] or record["transformed_ir_sha256"] != sha(local) or
            sha(data) != scalar_row["candidate"]["sha256"] or
            native_executable_sections(data)[0] != scalar_row["candidate"]["executable_bytes"]):
        raise ValueError("local scalar object differs: " + name)
    changed = share_helpers(local, guest, counts, expected, owners, name)
    result = emit_native(changed, triple, guest, mode, object_output=out)
    return {"module": name, "mode": mode, "source_ir_sha256": sha(raw),
            "parent_ir_sha256": sha(local), "transformed_ir_sha256": sha(changed),
            "source_object_sha256": sha(data), "source_object_bytes": len(data),
            "source_executable_bytes": scalar_row["candidate"]["executable_bytes"],
            "shared_definitions": sorted(shared_name(n) for n in HELPERS if owners[n] == name),
            "removed_ir_bytes": len(local)-len(changed), "trial": result}


def residual_candidates(trials, accepted, expected_count):
    rows = {r["module"]: r for r in trials}
    if (len(rows) != len(trials) or not set(accepted) <= set(rows) or
            len(rows) - len(accepted) != expected_count or
            {n for n, r in rows.items() if r.get("executable_saving", 0) > 0} != set(accepted)):
        raise ValueError("expanded scalar trial inventory differs")
    return {n: r for n, r in rows.items() if n not in accepted}


def verify_shared_parent(folder, meta, names, parent_raw, manifest, records, parent_plan, scalar):
    raw, current, entries = read_manifest(folder / "objects", set(names))
    plan = read_json(folder / "helper-plan.json")
    dll = folder / "game-translated.dll"
    if (plan["mode"] != "experimental_shared_scalar_memory" or
            plan["metadata_sha256"] != sha(meta) or
            (folder / "ee_functions.json").read_bytes() != meta or
            plan["source_manifest_sha256"] != sha(parent_raw) or
            plan["prior_library_sha256"] != parent_plan["linked_library_sha256"] or
            plan["native_target"] != manifest["native_target"] or
            current["native_target"] != manifest["native_target"] or
            current["target_triple"] != manifest["target_triple"] or
            current.get("production_cache_compatible") is not False or
            current["experimental_mixed_pipeline"] != manifest["experimental_mixed_pipeline"] + "-shared" or
            plan["retained_functions"] != len(names) or plan["source_functions"] != len(names) or
            plan["recompiled_modules"] != len(scalar) or plan["reused_modules"] != len(names)-len(scalar) or
            not plan.get("runtime_validation_required") or
            dll.stat().st_size != plan["linked_library_bytes"] or sha(dll.read_bytes()) != plan["linked_library_sha256"]):
        raise ValueError("shared scalar parent identity differs")
    rows = rows_of(plan)
    owners, expected = plan["shared_owners"], plan["scalar_body_sha256"]
    if (set(rows) != set(scalar) or len(rows) != len(plan["modules"]) or
            set(owners) != set(HELPERS) or set(expected) != set(HELPERS) or
            any(n not in scalar for n in owners.values())):
        raise ValueError("shared parent inventory differs")
    for n, item in entries.items():
        before = records[n]
        if (item["ir_sha256"] != before["ir_sha256"] or
                item.get("experimental_pipeline") != before.get("experimental_pipeline")):
            raise ValueError("shared parent source or policy differs: " + n)
        if n not in rows:
            if any(item.get(k) != before.get(k) for k in
                   ("sha256", "size_bytes", "transformed_ir_sha256", "defined_symbols")):
                raise ValueError("unmodified shared parent object differs: " + n)
            continue
        row = rows[n]
        definitions = sorted(shared_name(k) for k in HELPERS if owners[k] == n)
        if (row["shared_definitions"] != definitions or
                sorted(item["defined_symbols"]) != sorted(before["defined_symbols"] + definitions) or
                row["source_ir_sha256"] != before["ir_sha256"] or
                row["source_object_sha256"] != before["sha256"] or
                row["source_object_bytes"] != before["size_bytes"] or
                row["parent_ir_sha256"] != before["transformed_ir_sha256"] or
                row["transformed_ir_sha256"] != item["transformed_ir_sha256"] or
                row["trial"]["sha256"] != item["sha256"] or
                row["trial"]["object_bytes"] != item["size_bytes"] or
                not item.get("experimental_shared_scalar")):
            raise ValueError("shared parent row differs: " + n)
    if Counter(s for item in entries.values() for s in item["defined_symbols"] if s.startswith(SHARED)) != Counter(
            shared_name(k) for k in HELPERS):
        raise ValueError("shared parent owner count differs")
    for key, total in (
            ("executable_saving", sum(row["source_executable_bytes"]-row["trial"]["executable_bytes"] for row in rows.values())),
            ("object_saving", sum(row["source_object_bytes"]-row["trial"]["object_bytes"] for row in rows.values())),
            ("removed_ir_bytes", sum(row["removed_ir_bytes"] for row in rows.values()))):
        if plan[key] != total:
            raise ValueError("shared parent aggregate differs: " + key)
    load_inputs(folder / "ee_functions.json", folder / "objects")
    return raw, current, entries, plan


def revisit_trial(task):
    name, guest, path, record, receipts, plans, previous, folder, triple, expected, owners, out = task
    raw = path.read_bytes()
    data = verified_object(folder, record, sha(raw))
    before, prior = reconstruct_parent(raw, guest, name, receipts, plans)
    mode = check_object(raw, data, record, prior)
    if (sha(before) != record["transformed_ir_sha256"] or
            previous["original_ir_sha256"] != sha(raw) or
            previous["parent_ir_sha256"] != sha(before) or
            previous["parent_object_sha256"] != sha(data) or
            previous["parent_object_bytes"] != len(data) or
            previous["mode"] != mode or
            previous["parent_executable_bytes"] != native_executable_sections(data)[0]):
        raise ValueError("residual trial parent differs: " + name)
    local, counts = outline(before, guest)
    if (counts != previous["counts"] or
            (local != before) != ("candidate" in previous) or
            (local != before and sha(local) != previous["candidate_ir_sha256"])):
        raise ValueError("residual outline differs: " + name)
    row = {"module": name, "mode": mode, "source_ir_sha256": sha(raw),
           "parent_ir_sha256": sha(before), "source_object_sha256": sha(data),
           "source_object_bytes": len(data), "source_executable_bytes": previous["parent_executable_bytes"],
           "counts": counts, "shared_definitions": [], "removed_ir_bytes": 0}
    if local == before:
        return row
    if name in owners.values():
        raise ValueError("residual trial cannot own a shared helper")
    changed = share_helpers(local, guest, counts, expected, owners, name)
    baseline = emit_native(before, triple, guest, mode)
    if baseline["sha256"] != record["sha256"]:
        raise ValueError("residual parent object not reproducible: " + name)
    candidate = emit_native(changed, triple, guest, mode, object_output=out)
    saving = row["source_executable_bytes"] - candidate["executable_bytes"]
    if saving <= 0:
        out.unlink()
    row.update(transformed_ir_sha256=sha(changed), trial=candidate,
               removed_ir_bytes=len(before)-len(changed), executable_saving=saving)
    return row


# Only match the exact terminal alignment arms emitted by module_builder.
# Keep every original guard and predecessor block; merge the cold call/ret
# by exception code and delay-slot status. PC and BadVAddr are SSA PHIs.
ALIGNMENT = re.compile(
    rb"^  br i1 %t\d+, label %memory_alignment_fault_(?P<id>\d+), "
    rb"label %memory_alignment_ok_(?P=id)\n"
    rb"(?P<fault>memory_alignment_fault_(?P=id):\n"
    rb"  call i32 @ee_raise_exception\(ptr %cpu, ptr %rt, i32 (?P<code>[45]), "
    rb"i32 (?P<pc>\d+), i32 (?P<bd>[01]), i32 (?P<addr>%t\d+|-?\d+)\)\n"
    rb"  ret i32 1\n)(?=memory_alignment_ok_(?P=id):\n)", re.M)
ALIGN_JOIN = b"ee_alignment_merge_"


def alignment_groups(body):
    groups = {}
    for match in ALIGNMENT.finditer(body):
        key = (match["code"], match["bd"])
        groups.setdefault(key, []).append(match)
    return {key: group for key, group in groups.items() if len(group) >= 2}


def merge_alignment_faults(raw, guest):
    body = body_of(raw, guest)
    if ALIGN_JOIN in body:
        raise ValueError("alignment join symbol already present")
    groups = alignment_groups(body)
    if not groups:
        return raw, 0, 0
    replacements, tails = [], []
    for (code, bd), matches in sorted(groups.items()):
        tag = ALIGN_JOIN + code + b"_" + bd
        labels = [b"memory_alignment_fault_" + m["id"] for m in matches]
        if len(labels) != len(set(labels)):
            raise ValueError("duplicate alignment fault label")
        for match, label in zip(matches, labels):
            replacements.append((match.start("fault"), match.end("fault"),
                                 label + b":\n  br label %" + tag + b"\n"))
        def phi(field):
            return b", ".join(b"[ " + m[field] + b", %" + label + b" ]"
                              for m, label in zip(matches, labels))
        pc, address = b"%" + tag + b"_pc", b"%" + tag + b"_addr"
        tails.append(tag + b":\n  " + pc + b" = phi i32 " + phi("pc") +
                     b"\n  " + address + b" = phi i32 " + phi("addr") +
                     b"\n  call i32 @ee_raise_exception(ptr %cpu, ptr %rt, i32 " +
                     code + b", i32 " + pc + b", i32 " + bd + b", i32 " +
                     address + b")\n  ret i32 1\n")
    result = body
    for start, end, replacement in sorted(replacements, reverse=True):
        result = result[:start] + replacement + result[end:]
    close = result.rfind(b"\n}")
    if close < 0:
        raise ValueError("guest definition terminator differs")
    result = result[:close+1] + b"\n".join(tails) + result[close+1:]
    return raw.replace(body, result, 1), len(replacements), len(tails)


def verify_residual_alignment_parent(folder, meta, names, parent_raw, manifest, records, parent_plan):
    raw, current, entries = read_manifest(folder / "objects", set(names))
    plan = read_json(folder / "helper-plan.json")
    dll = folder / "game-translated.dll"
    if (plan["mode"] != "experimental_residual_shared_scalar" or
            plan["metadata_sha256"] != sha(meta) or
            (folder / "ee_functions.json").read_bytes() != meta or
            plan["source_manifest_sha256"] != sha(parent_raw) or
            plan["prior_library_sha256"] != parent_plan["linked_library_sha256"] or
            plan["native_target"] != manifest["native_target"] or
            current["native_target"] != manifest["native_target"] or
            current["target_triple"] != manifest["target_triple"] or
            current["experimental_mixed_pipeline"] != manifest["experimental_mixed_pipeline"] + "-residual" or
            current.get("production_cache_compatible") is not False or
            plan["source_functions"] != len(names) or plan["retained_functions"] != len(names) or
            plan["shared_owners"] != parent_plan["shared_owners"] or
            plan["scalar_body_sha256"] != parent_plan["scalar_body_sha256"] or
            not plan.get("runtime_validation_required") or
            dll.stat().st_size != plan["linked_library_bytes"] or
            sha(dll.read_bytes()) != plan["linked_library_sha256"]):
        raise ValueError("residual alignment parent identity differs")
    rows = rows_of(plan)
    trial_raw = (folder / "scalar-residual-trials.json").read_bytes()
    trials = json.loads(trial_raw)
    if (sha(trial_raw) != plan["trials_sha256"] or
            len(trials) != plan["attempted_modules"] or
            len({r["module"] for r in trials}) != len(trials) or
            [r for r in trials if r.get("executable_saving", 0) > 0] != plan["modules"] or
            len(rows) != plan["recompiled_modules"] or
            len(names)-len(rows) != plan["reused_modules"] or
            not set(rows) <= set(names)):
        raise ValueError("residual alignment trial inventory differs")
    for name, item in entries.items():
        before = records[name]
        if any(item.get(k) != before.get(k) for k in
               ("ir_sha256", "defined_symbols", "experimental_pipeline")):
            raise ValueError("residual source or symbol identity differs: " + name)
        if name not in rows:
            if any(item.get(k) != before.get(k) for k in
                   ("sha256", "size_bytes", "transformed_ir_sha256")):
                raise ValueError("unmodified residual object differs: " + name)
            continue
        row = rows[name]
        if (before.get("experimental_shared_scalar") or row["shared_definitions"] or
                row["source_ir_sha256"] != before["ir_sha256"] or
                row["parent_ir_sha256"] != before["transformed_ir_sha256"] or
                row["source_object_sha256"] != before["sha256"] or
                row["source_object_bytes"] != before["size_bytes"] or
                row["trial"]["sha256"] != item["sha256"] or
                row["trial"]["object_bytes"] != item["size_bytes"] or
                row["transformed_ir_sha256"] != item["transformed_ir_sha256"] or
                row["executable_saving"] <= 0 or
                row["executable_saving"] != row["source_executable_bytes"]-row["trial"]["executable_bytes"] or
                not item.get("experimental_shared_scalar")):
            raise ValueError("residual alignment receipt differs: " + name)
    for key, total in (
            ("executable_saving", sum(r["executable_saving"] for r in rows.values())),
            ("object_saving", sum(r["source_object_bytes"]-r["trial"]["object_bytes"] for r in rows.values())),
            ("removed_ir_bytes", sum(r["removed_ir_bytes"] for r in rows.values()))):
        if plan[key] != total:
            raise ValueError("residual alignment aggregate differs: " + key)
    load_inputs(folder / "ee_functions.json", folder / "objects")
    return raw, current, entries, plan, rows


def reconstruct_alignment_parent(task):
    (name, guest, path, base, receipts, plans, scalar, residual, shared_row,
     folder, record, triple, expected, owners) = task[:14]
    raw = path.read_bytes()
    data = verified_object(folder, record, sha(raw))
    before, prior = reconstruct_parent(raw, guest, name, receipts, plans)
    if (sha(raw) != base["ir_sha256"] or
            sha(before) != base["transformed_ir_sha256"] or
            prior["trial"]["sha256"] != base["sha256"]):
        raise ValueError("alignment base reconstruction differs: " + name)
    if scalar is not None or residual is not None:
        local, counts = outline(before, guest)
        row = scalar if scalar is not None else residual
        if counts != row["counts"] or local == before:
            raise ValueError("alignment scalar counts differ: " + name)
        if scalar is not None and (sha(local) != scalar["candidate_ir_sha256"] or
                                   shared_row["parent_ir_sha256"] != sha(local)):
            raise ValueError("alignment local scalar digest differs: " + name)
        before = share_helpers(local, guest, counts, expected, owners, name)
    if sha(before) != record["transformed_ir_sha256"]:
        raise ValueError("alignment parent reconstruction differs: " + name)
    mode = record.get("experimental_pipeline", "O5")
    if mode != prior["mode"]:
        raise ValueError("alignment parent policy differs")
    return raw, data, before, mode


def alignment_trial(task):
    name, guest = task[:2]
    record, triple = task[10:12]
    object_output = task[14] if len(task) == 15 else None
    raw, data, before, mode = reconstruct_alignment_parent(task)
    candidate, sites, groups = merge_alignment_faults(before, guest)
    row = {"module": name, "mode": mode, "original_ir_sha256": sha(raw),
           "parent_ir_sha256": sha(before), "parent_object_sha256": sha(data),
           "parent_executable_bytes": native_executable_sections(data)[0],
           "parent_object_bytes": len(data), "merged_sites": sites, "remaining_calls": groups,
           "ir_bytes_saved": len(before)-len(candidate)}
    if candidate != before:
        baseline = emit_native(before, triple, guest, mode)
        if baseline["sha256"] != record["sha256"]:
            raise ValueError("alignment parent object not reproducible: " + name)
        row["candidate"] = emit_native(candidate, triple, guest, mode, object_output=object_output)
        row["candidate_ir_sha256"] = sha(candidate)
        row["executable_saving"] = row["parent_executable_bytes"]-row["candidate"]["executable_bytes"]
        row["object_saving"] = len(data)-row["candidate"]["object_bytes"]
    return row


def alignment_winners(pilot, selected, candidates, meta, raw, current, entries, plan):
    if (pilot["mode"] != "read_only_ee_alignment_merge_pilot" or
            pilot["metadata_sha256"] != sha(meta) or
            pilot["parent_manifest_sha256"] != sha(raw) or
            pilot["parent_library_sha256"] != plan["linked_library_sha256"] or
            pilot["native_target"] != current["native_target"] or
            pilot["inventory_modules"] != len(entries) or
            pilot["eligible_modules"] != len(candidates) or
            pilot["eligible_call_reductions"] != sum(r[0] for r in candidates)):
        raise ValueError("alignment pilot provenance differs")
    accepted = winners(pilot, selected)
    reduction = {n: count for count, _, n in candidates}
    for row in pilot["modules"]:
        n = row["module"]
        item = entries[n]
        if (row["original_ir_sha256"] != item["ir_sha256"] or
                row["parent_ir_sha256"] != item["transformed_ir_sha256"] or
                row["parent_object_sha256"] != item["sha256"] or
                row["parent_object_bytes"] != item["size_bytes"] or
                row["mode"] != item.get("experimental_pipeline", "O5") or
                row["merged_sites"] - row["remaining_calls"] != reduction[n] or
                not 1 <= row["remaining_calls"] <= 4 or
                "candidate" not in row):
            raise ValueError("alignment pilot row differs: " + n)
    if not accepted:
        raise ValueError("alignment pilot has no measured wins")
    return accepted


def verify_alignment_overlay(folder, meta, names, raw, current, entries, plan, promoted, pilot_raw):
    overlay_raw, overlay, records = read_manifest(folder / "objects", set(names))
    receipt = read_json(folder / "helper-plan.json")
    dll = folder / "game-translated.dll"
    if (receipt["mode"] != "experimental_selective_alignment_merge" or
            receipt["metadata_sha256"] != sha(meta) or
            (folder / "ee_functions.json").read_bytes() != meta or
            receipt["source_manifest_sha256"] != sha(raw) or
            receipt["prior_library_sha256"] != plan["linked_library_sha256"] or
            receipt["pilot_sha256"] != sha(pilot_raw) or
            receipt["native_target"] != current["native_target"] or
            overlay["native_target"] != current["native_target"] or
            overlay["target_triple"] != current["target_triple"] or
            overlay["experimental_mixed_pipeline"] != current["experimental_mixed_pipeline"] + "-alignment-pilot" or
            overlay.get("production_cache_compatible") is not False or
            receipt["source_functions"] != len(names) or receipt["retained_functions"] != len(names) or
            receipt["recompiled_modules"] != len(promoted) or
            receipt["reused_modules"] != len(names)-len(promoted) or
            not receipt.get("runtime_validation_required") or
            dll.stat().st_size != receipt["linked_library_bytes"] or
            sha(dll.read_bytes()) != receipt["linked_library_sha256"]):
        raise ValueError("alignment overlay identity differs")
    rows = rows_of(receipt)
    if len(receipt["modules"]) != len(rows) or set(rows) != set(promoted):
        raise ValueError("alignment overlay inventory differs")
    for name, item in records.items():
        previous = entries[name]
        if any(item.get(k) != previous.get(k) for k in
               ("ir_sha256", "defined_symbols", "experimental_pipeline")):
            raise ValueError("alignment overlay symbol or source differs: " + name)
        if name in rows:
            row = rows[name]
            if (stable_receipt(row) != stable_receipt(promoted[name]) or
                    row["candidate"]["sha256"] != item["sha256"] or
                    row["candidate"]["object_bytes"] != item["size_bytes"] or
                    row["candidate_ir_sha256"] != item["transformed_ir_sha256"] or
                    item.get("experimental_alignment_merge") is not True):
                raise ValueError("alignment overlay trial differs: " + name)
        elif (item.get("experimental_alignment_merge") or
              any(item.get(k) != previous.get(k) for k in
                  ("sha256", "size_bytes", "transformed_ir_sha256"))):
            raise ValueError("unmodified alignment overlay object differs: " + name)
    for key, field in (("executable_saving", "executable_saving"),
                       ("object_saving", "object_saving"),
                       ("removed_ir_bytes", "ir_bytes_saved"),
                       ("merged_sites", "merged_sites"),
                       ("remaining_calls", "remaining_calls")):
        if receipt[key] != sum(row[field] for row in rows.values()):
            raise ValueError("alignment overlay aggregate differs: " + key)
    load_inputs(folder / "ee_functions.json", folder / "objects")
    return overlay_raw, overlay, records, receipt


def alignment_remaining(candidates, promoted):
    remaining = {n for _, _, n in candidates}
    if len(remaining) != len(candidates) or not set(promoted) <= remaining:
        raise ValueError("alignment candidate inventory differs")
    return remaining - set(promoted)


# A rotate, unlike a bare right shift, is a bijection on i32. Unknown
# misaligned PCs cannot alias any accepted aligned case, including zero.
ENTRY_SWITCH = re.compile(
    rb"^entry:\n(?:  %quadword_read_slot = alloca \{ i64, i64 \}, align 16\n)?"
    rb"  (?P<ptr>%t\d+) = getelementptr %ee_cpu_state, ptr %cpu, i32 0, i32 6\n"
    rb"  (?P<pc>%t\d+) = load i32, ptr (?P=ptr)\n"
    rb"(?P<switch>  switch i32 (?P=pc), label %dispatch_unknown \[\n"
    rb"(?P<cases>(?:    i32 \d+, label %bb_0x[0-9a-fA-F]+\n)+)(?P<close> {1,2}\]\n))"
    rb"(?=dispatch_unknown:\n)", re.M)
ENTRY_CASE = re.compile(rb"    i32 (\d+), label %(bb_0x[0-9a-fA-F]+)\n")


def rotate_pc(pc):
    if not 0 <= pc <= 0xffffffff:
        raise ValueError("entry PC outside i32")
    return (pc >> 2) | ((pc & 3) << 30)


def entry_switch(body):
    match = ENTRY_SWITCH.search(body)
    if match is None:
        return None, []
    cases = [(int(m[1]), m[2]) for m in ENTRY_CASE.finditer(match["cases"])]
    pcs = {pc for pc, _ in cases}
    if (len(pcs) != len(cases) or 0 not in pcs or
            any(not 0 <= pc <= 0xffffffff or pc & 3 for pc in pcs)):
        return None, []
    return match, cases


def normalize_entry_switch(raw, guest):
    body = body_of(raw, guest)
    match, cases = entry_switch(body)
    if match is None or len(cases) < 4:
        return raw, len(cases)
    if b"%ee_entry_rot_" in body:
        raise ValueError("entry rotation temporary already present")
    pc = match["pc"]
    replacement = (b"  %ee_entry_rot_low = lshr i32 " + pc + b", 2\n"
                   b"  %ee_entry_rot_high = shl i32 " + pc + b", 30\n"
                   b"  %ee_entry_rot_key = or i32 %ee_entry_rot_low, %ee_entry_rot_high\n"
                   b"  switch i32 %ee_entry_rot_key, label %dispatch_unknown [\n")
    replacement += b"".join(b"    i32 " + str(rotate_pc(value)).encode() +
                            b", label %" + label + b"\n" for value, label in cases)
    replacement += match["close"]
    new_body = body[:match.start("switch")] + replacement + body[match.end("switch"):]
    return raw.replace(body, new_body, 1), len(cases)


def verify_full_alignment(folder, meta, names, parent_raw, manifest, records, parent_plan, candidates, promoted):
    raw, current, entries = read_manifest(folder / "objects", set(names))
    plan = read_json(folder / "helper-plan.json")
    dll = folder / "game-translated.dll"
    trial_raw = (folder / "alignment-trials.json").read_bytes()
    trials = json.loads(trial_raw)
    selected = alignment_remaining(candidates, promoted)
    reductions = {n: count for count, _, n in candidates}
    if (plan["mode"] != "experimental_expanded_alignment_merge" or
            plan["metadata_sha256"] != sha(meta) or
            (folder / "ee_functions.json").read_bytes() != meta or
            plan["source_manifest_sha256"] != sha(parent_raw) or
            plan["prior_library_sha256"] != parent_plan["linked_library_sha256"] or
            plan["pilot_sha256"] != parent_plan["pilot_sha256"] or
            plan["native_target"] != manifest["native_target"] or
            current["native_target"] != manifest["native_target"] or
            current["target_triple"] != manifest["target_triple"] or
            current["experimental_mixed_pipeline"] != manifest["experimental_mixed_pipeline"] + "-full" or
            current.get("production_cache_compatible") is not False or
            plan["source_functions"] != len(names) or plan["retained_functions"] != len(names) or
            plan["trials_sha256"] != sha(trial_raw) or
            len(trials) != len(selected) or plan["attempted_modules"] != len(selected) or
            {r["module"] for r in trials} != selected or
            plan["retained_pilot_modules"] != len(promoted) or
            plan["eligible_modules"] != len(candidates) or
            plan["eligible_call_reductions"] != sum(reductions.values()) or
            not plan.get("runtime_validation_required") or
            dll.stat().st_size != plan["linked_library_bytes"] or
            sha(dll.read_bytes()) != plan["linked_library_sha256"]):
        raise ValueError("full alignment overlay identity differs")
    for row in trials:
        name = row["module"]
        previous = records[name]
        if (row["original_ir_sha256"] != previous["ir_sha256"] or
                row["parent_ir_sha256"] != previous["transformed_ir_sha256"] or
                row["parent_object_sha256"] != previous["sha256"] or
                row["parent_object_bytes"] != previous["size_bytes"] or
                row["mode"] != previous.get("experimental_pipeline", "O5") or
                row["executable_saving"] != row["parent_executable_bytes"]-row["candidate"]["executable_bytes"] or
                row["object_saving"] != row["parent_object_bytes"]-row["candidate"]["object_bytes"] or
                row["merged_sites"]-row["remaining_calls"] != reductions[name]):
            raise ValueError("full alignment trial differs: " + name)
    wins = [row for row in trials if row["executable_saving"] > 0]
    if (wins != plan["modules"] or plan["recompiled_modules"] != len(wins) or
            plan["reused_modules"] != len(names)-len(wins) or
            plan["rejected_or_unchanged_modules"] != len(trials)-len(wins) or
            plan["cumulative_alignment_modules"] != len(promoted)+len(wins)):
        raise ValueError("full alignment winner inventory differs")
    accepted = {r["module"]: r for r in wins}
    for name, item in entries.items():
        previous = records[name]
        if any(item.get(k) != previous.get(k) for k in
               ("ir_sha256", "defined_symbols", "experimental_pipeline")):
            raise ValueError("full alignment source or symbol differs: " + name)
        if name in accepted:
            row = accepted[name]
            if (row["candidate"]["sha256"] != item["sha256"] or
                    row["candidate"]["object_bytes"] != item["size_bytes"] or
                    row["candidate_ir_sha256"] != item["transformed_ir_sha256"] or
                    item.get("experimental_alignment_merge") is not True):
                raise ValueError("full alignment accepted object differs: " + name)
        elif any(item.get(k) != previous.get(k) for k in
                 ("sha256", "size_bytes", "transformed_ir_sha256", "experimental_alignment_merge")):
            raise ValueError("unmodified full alignment object differs: " + name)
    for key, field in (("executable_saving", "executable_saving"),
                       ("object_saving", "object_saving"),
                       ("removed_ir_bytes", "ir_bytes_saved"),
                       ("merged_sites", "merged_sites"), ("remaining_calls", "remaining_calls")):
        if plan[key] != sum(r[field] for r in wins):
            raise ValueError("full alignment aggregate differs: " + key)
    if (plan["cumulative_alignment_executable_saving"] != parent_plan["executable_saving"]+plan["executable_saving"] or
            plan["cumulative_call_reductions"] != parent_plan["merged_sites"]-parent_plan["remaining_calls"]+
            plan["merged_sites"]-plan["remaining_calls"]):
        raise ValueError("full alignment cumulative totals differ")
    load_inputs(folder / "ee_functions.json", folder / "objects")
    return raw, current, entries, plan, dict(promoted, **accepted)


def entry_trial(task):
    parent_task, row, folder, record, cases = task[:5]
    object_output = task[5] if len(task) == 6 else None
    name, guest = parent_task[:2]
    triple = parent_task[11]
    raw, _, before, mode = reconstruct_alignment_parent(parent_task)
    if row is not None:
        before, sites, groups = merge_alignment_faults(before, guest)
        if (sha(before) != row["candidate_ir_sha256"] or sites != row["merged_sites"] or
                groups != row["remaining_calls"] or row["candidate"]["sha256"] != record["sha256"]):
            raise ValueError("entry pilot alignment reconstruction differs: " + name)
    data = verified_object(folder, record, sha(raw))
    if sha(before) != record["transformed_ir_sha256"]:
        raise ValueError("entry pilot parent IR differs: " + name)
    candidate, count = normalize_entry_switch(before, guest)
    if candidate == before or count != cases:
        raise ValueError("entry switch differs: " + name)
    baseline = emit_native(before, triple, guest, mode)
    if baseline["sha256"] != record["sha256"]:
        raise ValueError("entry parent object not reproducible: " + name)
    trial = emit_native(candidate, triple, guest, mode, object_output=object_output)
    return {"module": name, "mode": mode, "switch_cases": count,
            "original_ir_sha256": sha(raw), "parent_ir_sha256": sha(before),
            "parent_object_sha256": sha(data), "parent_object_bytes": len(data),
            "parent_executable_bytes": baseline["executable_bytes"],
            "candidate_ir_sha256": sha(candidate), "candidate": trial,
            "executable_saving": baseline["executable_bytes"]-trial["executable_bytes"],
            "object_saving": len(data)-trial["object_bytes"],
            "ir_bytes_saved": len(before)-len(candidate)}


def entry_winners(pilot, inventory, selected, skipped, eligible, meta, raw, current, records, plan):
    if (pilot["mode"] != "read_only_ee_entry_rotation_pilot" or
            pilot["metadata_sha256"] != sha(meta) or
            pilot["parent_manifest_sha256"] != sha(raw) or
            pilot["parent_library_sha256"] != plan["linked_library_sha256"] or
            pilot["native_target"] != current["native_target"] or
            pilot["inventory_modules"] != len(records) or
            pilot["canonical_switch_modules"] != len(inventory) or
            pilot["eligible_modules"] != eligible or
            pilot["skipped"] != dict(skipped) or
            pilot["total_switch_cases"] != sum(r["switch_cases"] for r in inventory) or
            pilot["entry_inventory"] != inventory):
        raise ValueError("entry pilot provenance or inventory differs")
    # Every selected entry trial must have compiled a candidate. The
    # read-only entry report predates the generic rewritten_modules field.
    if any("candidate" not in row for row in pilot["modules"]):
        raise ValueError("entry pilot missing compiled candidate")
    positive = winners(dict(pilot, rewritten_modules=len(pilot["modules"])), selected)
    for row in pilot["modules"]:
        name = row["module"]
        item = records[name]
        if (row["original_ir_sha256"] != item["ir_sha256"] or
                row["parent_ir_sha256"] != item["transformed_ir_sha256"] or
                row["parent_object_sha256"] != item["sha256"] or
                row["parent_object_bytes"] != item["size_bytes"] or
                row["mode"] != item.get("experimental_pipeline", "O5") or
                row["switch_cases"] != selected[name]["switch_cases"]):
            raise ValueError("entry pilot trial identity differs: " + name)
    # Native code size alone is not a reliable proxy for linked image
    # size when jump-table data or relocations change. First linked test
    # conservatively requires both executable and object savings.
    accepted = {n: row for n, row in positive.items() if row["object_saving"] > 0}
    if not accepted:
        raise ValueError("no measured entry wins on both size metrics")
    return accepted


def entry_expansion_candidates(eligible, sampled):
    names = {r["module"] for r in eligible}
    if len(names) != len(eligible) or not set(sampled) <= names:
        raise ValueError("entry expansion inventory differs")
    return {r["module"]: r for r in eligible if r["module"] not in sampled}


def verify_entry_overlay(folder, meta, names, raw, current, records, plan, accepted, pilot_raw):
    overlay_raw, overlay, entries = read_manifest(folder / "objects", set(names))
    receipt = read_json(folder / "helper-plan.json")
    dll = folder / "game-translated.dll"
    pilot = json.loads(pilot_raw)
    if (receipt["mode"] != "experimental_selective_entry_rotation" or
            receipt["metadata_sha256"] != sha(meta) or
            (folder / "ee_functions.json").read_bytes() != meta or
            receipt["source_manifest_sha256"] != sha(raw) or
            receipt["prior_library_sha256"] != plan["linked_library_sha256"] or
            receipt["pilot_sha256"] != sha(pilot_raw) or
            receipt["native_target"] != current["native_target"] or
            overlay["native_target"] != current["native_target"] or
            overlay["target_triple"] != current["target_triple"] or
            overlay["experimental_mixed_pipeline"] != current["experimental_mixed_pipeline"] + "-entry-pilot" or
            overlay.get("production_cache_compatible") is not False or
            receipt["source_functions"] != len(names) or receipt["retained_functions"] != len(names) or
            receipt["recompiled_modules"] != len(accepted) or receipt["reused_modules"] != len(names)-len(accepted) or
            receipt["eligible_modules"] != pilot["eligible_modules"] or
            receipt["sampled_modules"] != pilot["selected_modules"] or
            receipt["acceptance_policy"] != "positive_executable_and_object_saving" or
            not receipt.get("runtime_validation_required") or
            dll.stat().st_size != receipt["linked_library_bytes"] or
            sha(dll.read_bytes()) != receipt["linked_library_sha256"]):
        raise ValueError("entry overlay identity differs")
    rows = rows_of(receipt)
    if len(receipt["modules"]) != len(rows) or set(rows) != set(accepted):
        raise ValueError("entry overlay inventory differs")
    for n, item in entries.items():
        prev = records[n]
        if any(item.get(k) != prev.get(k) for k in
               ("ir_sha256", "defined_symbols", "experimental_pipeline", "experimental_alignment_merge")):
            raise ValueError("entry overlay source or symbol differs: " + n)
        if n in rows:
            row = rows[n]
            if (stable_receipt(row) != stable_receipt(accepted[n]) or
                    row["candidate"]["sha256"] != item["sha256"] or
                    row["candidate"]["object_bytes"] != item["size_bytes"] or
                    row["candidate_ir_sha256"] != item["transformed_ir_sha256"] or
                    item.get("experimental_entry_rotation") is not True):
                raise ValueError("entry overlay candidate differs: " + n)
        elif (item.get("experimental_entry_rotation") or
              any(item.get(k) != prev.get(k) for k in ("sha256", "size_bytes", "transformed_ir_sha256"))):
            raise ValueError("unmodified entry object differs: " + n)
    for key, field in (("executable_saving", "executable_saving"),
                       ("object_saving", "object_saving"),
                       ("removed_ir_bytes", "ir_bytes_saved"), ("switch_cases", "switch_cases")):
        if receipt[key] != sum(r[field] for r in rows.values()):
            raise ValueError("entry overlay aggregate differs: " + key)
    load_inputs(folder / "ee_functions.json", folder / "objects")
    return overlay_raw, overlay, entries, receipt


def entry_pilot(args, ir, out, meta, names, base_records, rows, small_plans, scalar,
                residual, shared_rows, expected, owners, folder, entries,
                candidates, promoted, link_raw, link_manifest, link_entries, link_plan):
    full = args.entry_from.resolve()
    raw, current, records, plan, alignment_rows = verify_full_alignment(
        full, meta, names, link_raw, link_manifest, link_entries, link_plan, candidates, promoted)
    inventory, bands, largest = [], {}, []
    skipped = Counter()
    for name, guest in sorted(names.items()):
        original = (ir / name).read_bytes()
        if sha(original) != base_records[name]["ir_sha256"]:
            raise ValueError("entry source differs: " + name)
        match, cases = entry_switch(body_of(original, guest))
        if match is None:
            skipped["noncanonical"] += 1
            continue
        nonzero = [pc for pc, _ in cases if pc]
        span = max(nonzero)-min(nonzero)+4 if nonzero else 0
        record = records[name]
        row = {"module": name, "mode": record.get("experimental_pipeline", "O5"),
               "switch_cases": len(cases), "nonzero_cases": len(nonzero),
               "nonzero_span_bytes": span,
               "unique_targets": len({target for _, target in cases}),
               "object_bytes": record["size_bytes"]}
        inventory.append(row)
        if len(cases) < 4:
            skipped["small_switch"] += 1
            continue
        band = next((n for n in (8, 16, 32, 128, 512) if len(cases) <= n), 2**32)
        bands.setdefault((row["mode"], band), []).append(row)
        largest.append(row)
    selected = {r["module"]: r for r in sorted(largest, key=lambda r: (r["switch_cases"], r["object_bytes"], r["module"]))[-8:]}
    for group in bands.values():
        ordered = sorted(group, key=lambda r: (r["switch_cases"], r["object_bytes"], r["module"]))
        count = min(4, len(ordered))
        for i in range(count):
            row = ordered[i*(len(ordered)-1)//max(1, count-1)]
            selected[row["module"]] = row
    if not selected:
        raise ValueError(f"no canonical multi-entry switches: inventory={len(names)}, "
                         f"canonical={len(inventory)}, skipped={dict(skipped)}")
    accepted = None
    expansion = args.entry_expand_from is not None
    link_folder, link_raw, link_manifest, link_entries, link_plan = full, raw, current, records, plan
    started = time.monotonic()
    if args.entry_promote:
        pilot_raw = args.entry_promote.read_bytes()
        accepted = entry_winners(json.loads(pilot_raw), inventory, selected, skipped,
                                 len(largest), meta, raw, current, records, plan)
        sampled = set(selected)
        if expansion:
            link_folder = args.entry_expand_from.resolve()
            link_raw, link_manifest, link_entries, link_plan = verify_entry_overlay(
                link_folder, meta, names, raw, current, records, plan, accepted, pilot_raw)
            selected = entry_expansion_candidates(largest, sampled)
            if not selected:
                raise ValueError("no remaining entry trials")
        else:
            selected = {n: selected[n] for n in accepted}
        out.mkdir(parents=True)
        (out / "objects").mkdir()
    tasks = []
    for name, row in sorted(selected.items()):
        parent_task = (name, names[name], ir / name, base_records[name],
                       tuple(r.get(name) for r in rows), small_plans,
                       scalar.get(name), residual.get(name), shared_rows.get(name),
                       folder / "objects", entries[name], current["target_triple"], expected, owners)
        task = (parent_task, alignment_rows.get(name), full / "objects",
                records[name], row["switch_cases"])
        if accepted is not None:
            task += (out / "objects" / (Path(name).stem + ".entry.o"),)
        tasks.append(task)
    results = []
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for start in range(0, len(tasks), 128 * args.jobs):
            for row in pool.map(entry_trial, tasks[start:start+128*args.jobs]):
                name = row["module"]
                if accepted is not None and not expansion and stable_receipt(row) != stable_receipt(accepted[name]):
                    raise ValueError("entry candidate differs from pilot: " + name)
                if expansion and (row["executable_saving"] <= 0 or row["object_saving"] <= 0):
                    (out / "objects" / (Path(name).stem + ".entry.o")).unlink()
                results.append(row)
                if not expansion or len(results) % 128 == 0 or len(results) == len(tasks):
                    print(f"[{len(results)}/{len(tasks)}] {name}: "
                          f"{row['executable_saving']:+,} executable / {row['object_saving']:+,} object bytes", flush=True)
    if accepted is not None:
        kept = [r for r in results if r["executable_saving"] > 0 and r["object_saving"] > 0]
        if not kept:
            raise ValueError("no further measured entry wins")
        changes = {r["module"]: r for r in kept}
        objects = []
        for n, record in link_entries.items():
            if n in changes:
                row = changes[n]
                objects.append(dict(record, object=Path(n).stem + ".entry.o",
                                    size_bytes=row["candidate"]["object_bytes"],
                                    sha256=row["candidate"]["sha256"],
                                    transformed_ir_sha256=row["candidate_ir_sha256"],
                                    experimental_entry_rotation=True))
            else:
                objects.append(dict(record, object=str((link_folder / "objects" / record["object"]).resolve())))
        (out / "ee_functions.json").write_bytes(meta)
        overlay = dict(link_manifest, objects=objects, object_bytes=sum(r["size_bytes"] for r in objects),
                       experimental_mixed_pipeline=link_manifest["experimental_mixed_pipeline"] +
                           ("-full" if expansion else "-entry-pilot"),
                       production_cache_compatible=False)
        (out / "objects" / "objects.json").write_text(json.dumps(overlay, indent=2)+"\n", encoding="utf-8")
        receipt = {"mode": ("experimental_expanded_entry_rotation" if expansion else
                            "experimental_selective_entry_rotation"),
                   "metadata_sha256": sha(meta), "source_manifest_sha256": sha(link_raw),
                   "prior_library_sha256": link_plan["linked_library_sha256"],
                   "native_target": link_manifest["native_target"], "pilot_sha256": sha(pilot_raw),
                   "source_functions": len(names), "retained_functions": len(names),
                   "recompiled_modules": len(kept), "reused_modules": len(names)-len(kept),
                   "executable_saving": sum(r["executable_saving"] for r in kept),
                   "object_saving": sum(r["object_saving"] for r in kept),
                   "removed_ir_bytes": sum(r["ir_bytes_saved"] for r in kept),
                   "switch_cases": sum(r["switch_cases"] for r in kept),
                   "eligible_modules": len(largest), "sampled_modules": len(sampled),
                   "acceptance_policy": "positive_executable_and_object_saving",
                   "wall_seconds": time.monotonic()-started, "runtime_validation_required": True,
                   "modules": kept}
        if expansion:
            trials = out / "entry-trials.json"
            trials.write_text(json.dumps(results, indent=2)+"\n", encoding="utf-8")
            receipt.update(trials_sha256=sha(trials.read_bytes()), attempted_modules=len(results),
                           retained_pilot_modules=len(accepted),
                           skipped_pilot_rejections=len(sampled)-len(accepted),
                           rejected_or_unchanged_modules=len(results)-len(kept),
                           cumulative_entry_modules=len(accepted)+len(kept),
                           cumulative_executable_saving=link_plan["executable_saving"]+receipt["executable_saving"],
                           cumulative_object_saving=link_plan["object_saving"]+receipt["object_saving"],
                           cumulative_switch_cases=link_plan["switch_cases"]+receipt["switch_cases"])
        library = out / "game-translated.dll"
        subprocess.run([sys.executable, str(Path(__file__).with_name("link_native_objects.py")),
                        str(out / "ee_functions.json"), str(out / "objects"), str(library)], check=True)
        data = library.read_bytes()
        receipt.update(linked_library_bytes=len(data), linked_library_sha256=sha(data),
                       linked_executable_raw_bytes=native_executable_sections(data)[0])
        (out / "helper-plan.json").write_text(json.dumps(receipt, indent=2)+"\n", encoding="utf-8")
        return receipt
    report = {"mode": "read_only_ee_entry_rotation_pilot",
              "metadata_sha256": sha(meta), "parent_manifest_sha256": sha(raw),
              "parent_library_sha256": plan["linked_library_sha256"], "native_target": current["native_target"],
              "inventory_modules": len(names), "canonical_switch_modules": len(inventory),
              "eligible_modules": len(largest), "skipped": dict(skipped),
              "total_switch_cases": sum(r["switch_cases"] for r in inventory),
              "selected_modules": len(results),
              "executable_saving": sum(r["executable_saving"] for r in results),
              "object_saving": sum(r["object_saving"] for r in results),
              "ir_bytes_saved": sum(r["ir_bytes_saved"] for r in results),
              "modules": results, "entry_inventory": inventory}
    with out.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    return report


def alignment_pilot(args, ir, out, meta, names, base_records, rows, small_plans,
                    scalar, expected, owners, parent_raw, manifest, records, parent_plan):
    folder = args.alignment_from.resolve()
    shared_rows = rows_of(parent_plan)
    raw, current, entries, plan, residual = verify_residual_alignment_parent(
        folder, meta, names, parent_raw, manifest, records, parent_plan)
    if set(scalar) & set(residual):
        raise ValueError("alignment scalar inventories overlap")
    # Read only original IR, whose alignment arms are untouched by previous
    # transforms. Scan once, sample by actual mergeable site count and size.
    candidates = []
    for name, guest in names.items():
        original = (ir / name).read_bytes()
        if sha(original) != base_records[name]["ir_sha256"]:
            raise ValueError("stale alignment source: " + name)
        groups = alignment_groups(body_of(original, guest))
        if groups:
            sites = sum(map(len, groups.values()))
            candidates.append((sites-len(groups), entries[name]["size_bytes"], name))
    if not candidates:
        raise ValueError("no mergeable alignment faults")
    candidates.sort()
    bands = {}
    for item in candidates:
        band = next((n for n in (1, 4, 16, 64, 256) if item[0] <= n), 2**32)
        bands.setdefault(band, []).append(item)
    selected = {item[2] for item in candidates[-8:]}
    for group in bands.values():
        count = min(8, len(group))
        selected.update(group[i*(len(group)-1)//max(1, count-1)][2] for i in range(count))
    promoted = None
    expansion = args.alignment_expand_from is not None
    link_folder, link_raw, link_manifest, link_entries, link_plan = folder, raw, current, entries, plan
    started = time.monotonic()
    if args.alignment_promote:
        pilot_raw = args.alignment_promote.read_bytes()
        promoted = alignment_winners(json.loads(pilot_raw), selected, candidates,
                                     meta, raw, current, entries, plan)
        selected = set(promoted)
        if expansion:
            link_folder = args.alignment_expand_from.resolve()
            link_raw, link_manifest, link_entries, link_plan = verify_alignment_overlay(
                link_folder, meta, names, raw, current, entries, plan, promoted, pilot_raw)
            if args.entry_from:
                return entry_pilot(args, ir, out, meta, names, base_records, rows, small_plans,
                                   scalar, residual, shared_rows, expected, owners, folder, entries,
                                   candidates, promoted, link_raw, link_manifest, link_entries, link_plan)
            selected = alignment_remaining(candidates, promoted)
            if not selected:
                raise ValueError("no remaining alignment candidates")
        out.mkdir(parents=True)
        (out / "objects").mkdir()
    tasks = [(n, names[n], ir / n, base_records[n], tuple(r.get(n) for r in rows),
              small_plans, scalar.get(n), residual.get(n), shared_rows.get(n),
              folder / "objects", entries[n], current["target_triple"], expected, owners)
             for n in sorted(selected)]
    if promoted is not None:
        tasks = [task + (out / "objects" / (Path(task[0]).stem + ".alignment.o"),)
                 for task in tasks]
    result = []
    reduction = {n: count for count, _, n in candidates}
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for start in range(0, len(tasks), 128 * args.jobs):
            for row in pool.map(alignment_trial, tasks[start:start+128*args.jobs]):
                name = row["module"]
                if row["merged_sites"] - row["remaining_calls"] != reduction[name] or "candidate" not in row:
                    raise ValueError("alignment candidate site count differs: " + name)
                if promoted is not None and not expansion and stable_receipt(row) != stable_receipt(promoted[name]):
                    raise ValueError("alignment candidate differs from pilot: " + name)
                if expansion and row["executable_saving"] <= 0:
                    (out / "objects" / (Path(name).stem + ".alignment.o")).unlink()
                result.append(row)
                if not expansion or len(result) % 128 == 0 or len(result) == len(tasks):
                    print(f"[{len(result)}/{len(tasks)}] {name}: "
                          f"{row.get('executable_saving', 0):+,} executable bytes saved", flush=True)
    if promoted is not None:
        accepted = [r for r in result if r["executable_saving"] > 0]
        if not accepted:
            raise ValueError("no further measured alignment wins")
        changes = {r["module"]: r for r in accepted}
        objects = []
        for n, record in link_entries.items():
            if n in changes:
                row = changes[n]
                objects.append(dict(record, object=Path(n).stem + ".alignment.o",
                                    size_bytes=row["candidate"]["object_bytes"],
                                    sha256=row["candidate"]["sha256"],
                                    transformed_ir_sha256=row["candidate_ir_sha256"],
                                    experimental_alignment_merge=True))
            else:
                objects.append(dict(record, object=str((link_folder / "objects" / record["object"]).resolve())))
        (out / "ee_functions.json").write_bytes(meta)
        overlay = dict(link_manifest, objects=objects, object_bytes=sum(r["size_bytes"] for r in objects),
                       experimental_mixed_pipeline=link_manifest["experimental_mixed_pipeline"] +
                           ("-full" if expansion else "-alignment-pilot"),
                       production_cache_compatible=False)
        (out / "objects" / "objects.json").write_text(json.dumps(overlay, indent=2)+"\n", encoding="utf-8")
        receipt = {"mode": ("experimental_expanded_alignment_merge" if expansion else
                            "experimental_selective_alignment_merge"),
                   "metadata_sha256": sha(meta), "source_manifest_sha256": sha(link_raw),
                   "prior_library_sha256": link_plan["linked_library_sha256"],
                   "native_target": link_manifest["native_target"], "pilot_sha256": sha(pilot_raw),
                   "source_functions": len(names), "retained_functions": len(names),
                   "recompiled_modules": len(accepted), "reused_modules": len(names)-len(accepted),
                   "executable_saving": sum(r["executable_saving"] for r in accepted),
                   "object_saving": sum(r["object_saving"] for r in accepted),
                   "removed_ir_bytes": sum(r["ir_bytes_saved"] for r in accepted),
                   "merged_sites": sum(r["merged_sites"] for r in accepted),
                   "remaining_calls": sum(r["remaining_calls"] for r in accepted),
                   "wall_seconds": time.monotonic()-started, "runtime_validation_required": True,
                   "modules": accepted}
        if expansion:
            trials = out / "alignment-trials.json"
            trials.write_text(json.dumps(result, indent=2)+"\n", encoding="utf-8")
            receipt.update(trials_sha256=sha(trials.read_bytes()), attempted_modules=len(result),
                           eligible_modules=len(candidates), eligible_call_reductions=sum(r[0] for r in candidates),
                           retained_pilot_modules=len(promoted),
                           rejected_or_unchanged_modules=len(result)-len(accepted),
                           cumulative_alignment_modules=len(promoted)+len(accepted),
                           cumulative_alignment_executable_saving=link_plan["executable_saving"]+receipt["executable_saving"],
                           cumulative_call_reductions=link_plan["merged_sites"]-link_plan["remaining_calls"]+
                                                     receipt["merged_sites"]-receipt["remaining_calls"])
        library = out / "game-translated.dll"
        subprocess.run([sys.executable, str(Path(__file__).with_name("link_native_objects.py")),
                        str(out / "ee_functions.json"), str(out / "objects"), str(library)], check=True)
        data = library.read_bytes()
        receipt.update(linked_library_bytes=len(data), linked_library_sha256=sha(data),
                       linked_executable_raw_bytes=native_executable_sections(data)[0])
        (out / "helper-plan.json").write_text(json.dumps(receipt, indent=2)+"\n", encoding="utf-8")
        return receipt
    report = {"mode": "read_only_ee_alignment_merge_pilot", "metadata_sha256": sha(meta),
              "parent_manifest_sha256": sha(raw), "parent_library_sha256": plan["linked_library_sha256"],
              "native_target": current["native_target"], "inventory_modules": len(names),
              "eligible_modules": len(candidates), "eligible_call_reductions": sum(r[0] for r in candidates),
              "selected_modules": len(result), "rewritten_modules": sum("candidate" in r for r in result),
              "executable_saving": sum(r.get("executable_saving", 0) for r in result),
              "object_saving": sum(r.get("object_saving", 0) for r in result),
              "ir_bytes_saved": sum(r["ir_bytes_saved"] for r in result), "modules": result}
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    return report


def stage(args):
    started = time.monotonic()
    ir = args.ir.resolve()
    folders = [p.resolve() for p in (args.counter_experiment, args.quadword_experiment,
                                    args.multiply_experiment, args.add_experiment)]
    pilot, expanded = args.pilot_experiment.resolve(), args.experiment.resolve()
    out = args.output.resolve()
    shared_parent = args.revisit_from.resolve() if args.revisit_from else None
    inputs = (ir, *folders, pilot, expanded) + ((shared_parent,) if shared_parent else ())
    if args.entry_expand_from:
        if not args.entry_promote or not args.entry_from:
            raise ValueError("--entry-expand-from requires --entry-from and --entry-promote")
        inputs += (args.entry_expand_from.resolve(),)
    if args.entry_promote and not args.entry_from:
        raise ValueError("--entry-promote requires --entry-from")
    if args.entry_from:
        if not args.alignment_expand_from:
            raise ValueError("--entry-from requires --alignment-expand-from")
        inputs += (args.entry_from.resolve(),)
    if args.alignment_expand_from:
        if not args.alignment_promote or not args.alignment_from:
            raise ValueError("--alignment-expand-from requires --alignment-from and --alignment-promote")
        inputs += (args.alignment_expand_from.resolve(),)
    if args.alignment_promote and not args.alignment_from:
        raise ValueError("--alignment-promote requires --alignment-from")
    if args.alignment_from:
        if shared_parent is None:
            raise ValueError("--alignment-from requires --revisit-from")
        inputs += (args.alignment_from.resolve(),)
    if out.exists() or any(out.is_relative_to(p) for p in inputs):
        raise ValueError("use a new output outside input caches")
    meta = (ir / "ee_functions.json").read_bytes()
    functions = json.loads(meta)["functions"]
    names = {Path(f["ir_path"]).name: f["host_symbol"] for f in functions}
    if len(names) != len(functions):
        raise ValueError("duplicate module metadata")
    plans = [read_json(p / "helper-plan.json") for p in folders]
    manifests = [read_manifest(p / "objects", set(names)) for p in folders]
    modes = ("experimental_shared_counter", "experimental_shared_quadword",
             "experimental_shared_cop1_mul", "experimental_shared_cop1_add")
    for i, (folder, plan, (raw, manifest, _)) in enumerate(zip(folders, plans, manifests)):
        dll = folder / "game-translated.dll"
        if (plan["mode"] != modes[i] or plan["metadata_sha256"] != sha(meta) or
                (folder / "ee_functions.json").read_bytes() != meta or
                plan["retained_functions"] != len(names) or
                plan["native_target"] != manifest["native_target"] or
                manifest["native_target"] != manifests[-1][1]["native_target"] or
                manifest.get("production_cache_compatible") is not False or
                dll.stat().st_size != plan["linked_library_bytes"] or
                sha(dll.read_bytes()) != plan["linked_library_sha256"]):
            raise ValueError("prior experiment identity differs")
        if i and (plan["source_manifest_sha256"] != sha(manifests[i-1][0]) or
                  plan["prior_library_sha256"] != plans[i-1]["linked_library_sha256"]):
            raise ValueError("prior manifest chain differs")
    base_raw, base_manifest, base_records = manifests[-1]
    pilot_raw, pilot_manifest, pilot_records, pilot_plan, first = verify_pilot_parent(
        pilot, meta, names, base_raw, base_manifest, base_records, plans[-1])
    parent_raw, manifest, records, parent_plan, rest = verify_pilot_parent(
        expanded, meta, names, pilot_raw, pilot_manifest, pilot_records, pilot_plan, expanded=True)
    if (set(first) & set(rest) or parent_plan["retained_pilot_modules"] != len(first) or
            parent_plan["cumulative_scalar_modules"] != len(first)+len(rest)):
        raise ValueError("scalar overlay inventories overlap or differ")
    scalar = dict(first, **rest)
    if {n for n, r in records.items() if r.get("experimental_scalar_memory")} != set(scalar):
        raise ValueError("scalar overlay flags differ")
    rows = [rows_of(p) for p in plans]
    small_plans = plans_without_modules(plans)
    def task_for(n):
        return (n, names[n], ir / n, base_records[n],
                tuple(r.get(n) for r in rows), small_plans, scalar[n])
    owners, expected = {}, {}
    for helper in HELPERS:
        candidates = sorted(n for n, row in scalar.items() if row["counts"].get(helper, 0))
        if not candidates:
            raise ValueError("missing scalar width/direction: " + helper)
        # Prefer an O5 owner for the common implementation, then stable name.
        owner = min(candidates, key=lambda n: (scalar[n]["mode"] != "O5", n))
        _, local, _ = local_parent(task_for(owner))
        owners[helper] = owner
        expected[helper] = sha(body_of(local, helper))
    residual = None
    if shared_parent is not None:
        trial_raw = (expanded / "scalar-trials.json").read_bytes()
        if sha(trial_raw) != parent_plan["trials_sha256"]:
            raise ValueError("expanded trial receipt digest differs")
        all_trials = json.loads(trial_raw)
        if len(all_trials) != parent_plan["attempted_modules"]:
            raise ValueError("expanded trial count differs")
        residual = residual_candidates(all_trials, rest, parent_plan["rejected_or_unchanged_modules"])
        if set(residual) & set(scalar) or not set(residual) <= set(names):
            raise ValueError("residual scalar inventory overlaps accepted modules")
        parent_raw, manifest, records, parent_plan = verify_shared_parent(
            shared_parent, meta, names, parent_raw, manifest, records, parent_plan, scalar)
        if parent_plan["shared_owners"] != owners or parent_plan["scalar_body_sha256"] != expected:
            raise ValueError("shared scalar owner identities differ")
        # No previous winners are recompiled; all residual modules use
        # declarations only, and the eight owner objects are unchanged.
        expanded = shared_parent
    if args.alignment_from:
        return alignment_pilot(args, ir, out, meta, names, base_records, rows, small_plans,
                               scalar, expected, owners, parent_raw, manifest, records, parent_plan)
    out.mkdir(parents=True)
    staged = out / "objects"
    staged.mkdir()
    results = []
    selected = sorted(residual if residual is not None else scalar)
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for start in range(0, len(selected), args.jobs*128):
            batch = selected[start:start+args.jobs*128]
            if residual is None:
                tasks = [(task_for(n), expanded / "objects", records[n], manifest["target_triple"],
                          expected, owners, staged / (Path(n).stem + ".shared-scalar.o")) for n in batch]
                worker = trial
            else:
                tasks = [(n, names[n], ir / n, records[n], tuple(r.get(n) for r in rows),
                          small_plans, residual[n], expanded / "objects", manifest["target_triple"],
                          expected, owners, staged / (Path(n).stem + ".shared-scalar.o")) for n in batch]
                worker = revisit_trial
            for row in pool.map(worker, tasks):
                results.append(row)
                if len(results) % 128 == 0 or len(results) == len(selected):
                    print(f"[{len(results)}/{len(selected)}] shared scalar emission", flush=True)
    accepted = results if residual is None else [r for r in results if r.get("executable_saving", 0) > 0]
    wins = sum(r["source_executable_bytes"]-r["trial"]["executable_bytes"] for r in accepted)
    if wins <= 0:
        raise ValueError("cross-module sharing did not reduce aggregate executable size")
    changed = {r["module"]: r for r in accepted}
    entries = []
    for n, record in records.items():
        if n in changed:
            row = changed[n]
            symbols = list(record["defined_symbols"]) + row["shared_definitions"]
            if len(symbols) != len(set(symbols)):
                raise ValueError("duplicate shared definition")
            entries.append(dict(record, object=Path(n).stem + ".shared-scalar.o",
                                size_bytes=row["trial"]["object_bytes"], sha256=row["trial"]["sha256"],
                                defined_symbols=symbols, transformed_ir_sha256=row["transformed_ir_sha256"],
                                experimental_shared_scalar=True))
        else:
            entries.append(dict(record, object=str((expanded / "objects" / record["object"]).resolve())))
    if Counter(s for r in entries for s in r["defined_symbols"] if s.startswith(SHARED)) != Counter(
            shared_name(n) for n in HELPERS):
        raise ValueError("shared scalar owner inventory differs")
    (out / "ee_functions.json").write_bytes(meta)
    overlay = dict(manifest, objects=entries, object_bytes=sum(r["size_bytes"] for r in entries),
                   experimental_mixed_pipeline=manifest["experimental_mixed_pipeline"] +
                       ("-residual" if residual is not None else "-shared"),
                   production_cache_compatible=False)
    (staged / "objects.json").write_text(json.dumps(overlay, indent=2)+"\n", encoding="utf-8")
    plan = {"mode": ("experimental_residual_shared_scalar" if residual is not None else
                     "experimental_shared_scalar_memory"), "metadata_sha256": sha(meta),
            "source_manifest_sha256": sha(parent_raw), "prior_library_sha256": parent_plan["linked_library_sha256"],
            "native_target": manifest["native_target"], "source_functions": len(names),
            "retained_functions": len(names), "recompiled_modules": len(accepted),
            "reused_modules": len(names)-len(accepted), "shared_owners": owners,
            "scalar_body_sha256": expected, "executable_saving": wins,
            "object_saving": sum(r["source_object_bytes"]-r["trial"]["object_bytes"] for r in accepted),
            "removed_ir_bytes": sum(r["removed_ir_bytes"] for r in accepted),
            "wall_seconds": time.monotonic()-started, "runtime_validation_required": True,
            "modules": accepted}
    if residual is not None:
        report = out / "scalar-residual-trials.json"
        report.write_text(json.dumps(results, indent=2)+"\n", encoding="utf-8")
        plan.update(trials_sha256=sha(report.read_bytes()), attempted_modules=len(results),
                    rewritten_modules=sum("trial" in row for row in results),
                    rejected_or_unchanged_modules=len(results)-len(accepted),
                    retained_shared_modules=len(scalar),
                    cumulative_scalar_modules=len(scalar)+len(accepted))
    library = out / "game-translated.dll"
    subprocess.run([sys.executable, str(Path(__file__).with_name("link_native_objects.py")),
                    str(out / "ee_functions.json"), str(staged), str(library)], check=True)
    data = library.read_bytes()
    plan.update(linked_library_bytes=len(data), linked_library_sha256=sha(data),
                linked_executable_raw_bytes=native_executable_sections(data)[0])
    (out / "helper-plan.json").write_text(json.dumps(plan, indent=2)+"\n", encoding="utf-8")
    return plan


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ir", type=Path)
    for flag in ("counter-experiment", "quadword-experiment", "multiply-experiment",
                 "add-experiment", "pilot-experiment", "experiment", "output"):
        parser.add_argument("--"+flag, type=Path, required=True)
    parser.add_argument("--revisit-from", type=Path, help="accepted shared scalar overlay")
    parser.add_argument("--alignment-from", type=Path, help="accepted residual overlay; output is a read-only alignment pilot JSON")
    parser.add_argument("--alignment-promote", type=Path, help="link measured wins from an existing alignment pilot JSON")
    parser.add_argument("--alignment-expand-from", type=Path, help="accepted linked alignment pilot overlay; retain its winners and trial all remaining eligible modules")
    parser.add_argument("--entry-from", type=Path, help="accepted full alignment overlay; output is a read-only entry-switch pilot JSON")
    parser.add_argument("--entry-promote", type=Path, help="link only measured entry pilot wins on both size metrics")
    parser.add_argument("--entry-expand-from", type=Path, help="accepted selective entry overlay; trial all unsampled eligible switches")
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    try:
        if os.name != "nt" or not 1 <= args.jobs <= 16 or os.environ.get("SDBZ_NATIVE_ALU_CERTIFICATES") == "1":
            raise ValueError("Windows experiment requires 1–16 jobs and disabled certificates")
        result = stage(args)
        print(f"EE experiment: {result['executable_saving']:,} executable bytes saved", flush=True)
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(f"EE shared scalar experiment refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
