#!/usr/bin/env python3
"""Shadow-only internal EE entry analysis. Never modifies translation inputs.

The comparison is deliberately stricter than guest CFG equivalence: compare
emitted LLVM for an exact nonzero dispatch case, alpha-renaming generated
temporaries and auxiliary labels. No result authorizes a dispatcher rewrite.
"""
import argparse
from collections import Counter, defaultdict, deque
from functools import lru_cache
import hashlib
import json
from pathlib import Path
import re
import sys

from audit_ee_function_overlap import scan_details

FUNCTION = re.compile(r"^define i32 @ee_fn_[0-9a-fA-F]+\(ptr %cpu, ptr %rt\) \{$", re.M)
LABEL = re.compile(r"^([A-Za-z_][A-Za-z_0-9.]*):$")
CASE = re.compile(r"i32 (\d+), label %(bb_0x[0-9a-fA-F]+)")
SELECTOR = re.compile(
    r"(?:(%quadword_read_slot = alloca \{ i64, i64 \}, align 16)\n)?"
    r"(%t\d+) = getelementptr %ee_cpu_state, ptr %cpu, i32 0, i32 6\n"
    r"(%t\d+) = load i32, ptr \2\n"
    r"switch i32 \3, label %dispatch_unknown \[")
TOKEN = re.compile(r"%[A-Za-z_][A-Za-z_0-9.]*")
TEMP = re.compile(r"%t\d+$")
UNSUPPORTED = re.compile(r'\b(?:blockaddress|indirectbr|callbr|invoke|landingpad|resume|asm|catchswitch|catchpad|cleanuppad|catchret|cleanupret)\b|@ee_fn_|["!$]')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def clean(text):
    return tuple(line.strip() for line in text.splitlines()
                 if line.strip() and not line.lstrip().startswith(";"))


def read_module(path):
    data = path.read_bytes()
    text = data.decode("utf-8")
    matches = list(FUNCTION.finditer(text))
    if len(matches) != 1:
        raise ValueError("unsupported_guest_function_signature")
    match = matches[0]
    end = text.find("\n}", match.end())
    if end < 0:
        raise ValueError("missing_function_end")
    # Compare all non-guest declarations, types, globals, helper bodies and
    # attributes exactly. Unused helper differences intentionally reject.
    environment = digest((text[:match.start()] + "\n" + text[end + 2:]).encode())
    blocks = {}
    current = None
    for line in clean(text[match.end():end]):
        label = LABEL.fullmatch(line)
        if label:
            current = label[1]
            if current in blocks:
                raise ValueError("duplicate_label")
            blocks[current] = []
        elif current is None:
            raise ValueError("missing_label")
        else:
            blocks[current].append(line)
    prefix = blocks.get("entry", [])
    if "]" not in prefix or prefix[-1] != "]":
        raise ValueError("unsupported_entry_switch")
    switch = next((i for i, line in enumerate(prefix) if line.startswith("switch ")), -1)
    if switch < 0 or not SELECTOR.fullmatch("\n".join(prefix[:switch + 1])):
        raise ValueError("unsupported_entry_prologue")
    cases = {}
    for line in prefix[switch + 1:-1]:
        case = CASE.fullmatch(line)
        if not case:
            raise ValueError("unsupported_entry_case")
        pc, target = int(case[1]), case[2]
        if pc in cases or target not in blocks:
            raise ValueError("invalid_entry_case")
        cases[pc] = target
    return digest(data), environment, tuple(prefix[:switch + 1]), cases, blocks


def signature(module, pc):
    _, environment, prefix, cases, blocks = module
    # Do not accept zero sentinels or aliases to a different guest block.
    entry = cases.get(pc)
    if not pc or entry is None or int(entry[3:], 16) != pc:
        raise ValueError("not_exact_resume_case")
    names = {}
    labels = {}
    queue = deque()
    seen = set()

    def label_id(label):
        if label == "entry":
            return "%entry"
        if label not in seen:
            seen.add(label)
            queue.append(label)
        if label.startswith("bb_0x") or label == "dispatch_unknown":
            return "%" + label
        if label not in labels:
            labels[label] = "%$label" + str(len(labels))
        return labels[label]

    def replace(match):
        token = match[0]
        if token[1:] in blocks:
            return label_id(token[1:])
        if TEMP.fullmatch(token):
            if token not in names:
                names[token] = "%$temp" + str(len(names))
            return names[token]
        return token

    # The entry selector is independently checked, not compared as a full
    # switch: other resume cases are not executed for this invocation.
    # Preserve the prologue and seed its SSA mappings, but do not traverse
    # the unknown-PC arm.
    normalized_prefix = tuple(TOKEN.sub(replace, line) for line in prefix)
    queue.clear()
    seen.clear()
    labels.clear()
    label_id(entry)
    body = []
    while queue:
        label = queue.popleft()
        lines = blocks[label]
        if not lines or any(UNSUPPORTED.search(line) for line in lines):
            raise ValueError("unsupported_reachable_ir")
        if any(re.search(r"\blabel %entry\b", line) for line in lines):
            raise ValueError("branch_to_dispatch_prologue")
        normalized = tuple(TOKEN.sub(replace, line) for line in lines)
        body.append((label_id(label), normalized))
    # A valid generated LLVM module supplies SSA definitions and verified
    # dominance. This tool is not an LLVM verifier or a native ABI proof.
    return environment, normalized_prefix, tuple(body)


def compare(source, owner, pc):
    for module, role in ((source, "source"), (owner, "owner")):
        try:
            result = signature(module, pc)
        except ValueError as error:
            return role + "_" + str(error), None
        if role == "source":
            expected = result
        elif result[0] != expected[0]:
            return "module_environment_differs", None
        elif result[1] != expected[1]:
            return "entry_prologue_differs", None
        elif result[2] != expected[2]:
            return "reachable_ir_differs", None
    return "matching_emitted_ir", digest(repr(expected).encode())


def basename(path):
    return path.replace("\\", "/").rsplit("/", 1)[-1]


def analyze(root, baseline):
    metadata_path = root / "ee_functions.json"
    metadata_bytes = metadata_path.read_bytes()
    metadata = json.loads(metadata_bytes)
    functions = metadata["functions"]
    static = {int(pc, 0) for pc in metadata["discovered_entries"]}
    by_name = {basename(fn["ir_path"]): fn for fn in functions}
    paths = {path.name: path for path in root.glob("*.ll")}
    rows = {row["module"]: row for row in baseline["module_details"]}
    if set(paths) != set(by_name) or set(paths) != set(rows):
        raise ValueError("baseline, IR and metadata inventories differ")
    if sum(p.stat().st_size for p in paths.values()) != baseline["aggregate_ir_bytes"]:
        raise ValueError("baseline aggregate IR size differs")
    entry_to_module = {int(fn["vaddr"], 0): name for name, fn in by_name.items()}
    if len(entry_to_module) != len(by_name):
        raise ValueError("duplicate function entry")
    dispatch = defaultdict(list)
    for name, fn in by_name.items():
        for pc in fn["block_pcs"]:
            dispatch[int(pc, 0)].append(name)

    # The old audit is only a scope filter, not evidence of equivalence.
    # Scan the actual overlap modules and record all observed coverage,
    # including PCs that are not legal resume entries.
    targets = {pc: name for pc, name in entry_to_module.items()
               if pc not in static and rows[name]["shared_guest_pcs"]}
    owners = defaultdict(list)
    for name, row in rows.items():
        if not row["shared_guest_pcs"]:
            continue
        pcs, blocks, _, _, _, switch = scan_details(paths[name])
        for pc in (pcs | blocks) & targets.keys():
            if name != targets[pc]:
                owners[pc].append((name, switch and pc in blocks))

    @lru_cache(maxsize=8)
    def parsed(name):
        return read_module(paths[name])

    results = []
    counts = Counter()
    for pc, source in sorted(targets.items()):
        candidates = []
        for owner, resume in sorted(owners[pc]):
            candidate = {"owner": owner, "owner_static": int(by_name[owner]["vaddr"], 0) in static}
            if not resume:
                candidate["status"] = "owner_pc_not_resume_block"
            else:
                try:
                    a, b = parsed(source), parsed(owner)
                    status, fingerprint = compare(a, b, pc)
                    candidate.update(status=status, source_sha256=a[0], owner_sha256=b[0],
                                     reachable_signature_sha256=fingerprint)
                    if status == "matching_emitted_ir":
                        # A matching entry is not enough to remove a module.
                        # Independently compare every metadata-owned dispatch PC.
                        checks = []
                        for owned in sorted({int(value, 0) for value in by_name[source]["block_pcs"]}):
                            check, certificate = compare(a, b, owned)
                            checks.append({"pc": hex(owned), "status": check,
                                           "reachable_signature_sha256": certificate})
                            if check != "matching_emitted_ir":
                                break
                        candidate["source_dispatch_checks"] = checks
                        candidate["all_source_dispatch_pcs_match"] = (
                            bool(checks) and all(check["status"] == "matching_emitted_ir"
                                                 for check in checks))
                        candidate["owner_has_published_entry"] = owner in dispatch[pc]
                except (ValueError, UnicodeError) as error:
                    candidate.update(status="unsupported_module", detail=str(error))
            counts[candidate["status"]] += 1
            candidates.append(candidate)
        results.append({
            "pc": hex(pc), "source": source,
            "source_native_executable_bytes": rows[source]["native_executable_bytes"],
            "metadata_dispatch_owners": sorted(dispatch[pc]),
            "source_dispatch_blocks": len(by_name[source]["block_pcs"]),
            "candidates": candidates,
        })
    return {
        "schema": 1, "mode": "shadow_only",
        "metadata_sha256": digest(metadata_bytes),
        "target_modules": len(targets),
        "targets_with_other_pc_coverage": sum(bool(row["candidates"]) for row in results),
        "targets_with_matching_emitted_ir": sum(any(c["status"] == "matching_emitted_ir"
                                                    for c in row["candidates"]) for row in results),
        "candidate_status_counts": dict(sorted(counts.items())),
        "targets_with_all_dispatch_pcs_matching": sum(
            any(c.get("all_source_dispatch_pcs_match", False) for c in row["candidates"])
            for row in results),
        "entries": results,
        "limitations": [
            "baseline selects scope; it is not a current-cache authentication or native proof",
            "matching emitted IR is not a deletion certificate or a dispatcher routing authorization",
            "comparison deliberately rejects different prologues, helper environments and unsupported IR",
            "only the requested nonzero entry is compared; other source dispatch PCs remain live",
            "native object hashes, host dispatch accounting and external helper ABI require separate validation",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ir", type=Path)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--json", type=Path, required=True)
    args = parser.parse_args()
    try:
        # Never overwrite an IR/cache artifact or the baseline.
        output = args.json.resolve()
        if output == args.baseline.resolve() or output.is_relative_to(args.ir.resolve()):
            raise ValueError("output must be outside IR and distinct from baseline")
        report = analyze(args.ir, json.loads(args.baseline.read_text(encoding="utf-8")))
        with output.open("x", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2)
            stream.write("\n")
    except (OSError, ValueError, KeyError) as error:
        print(f"EE entry shadow audit: {error}", file=sys.stderr)
        return 1
    print(f"EE entry shadow audit: {report['target_modules']} nonstatic overlapping modules; "
          f"{report['targets_with_other_pc_coverage']} covered entry PCs; "
          f"{report['targets_with_matching_emitted_ir']} matching emitted IR")
    for status, count in report["candidate_status_counts"].items():
        print(f"  {status}: {count}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
