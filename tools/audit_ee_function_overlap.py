#!/usr/bin/env python3
"""Read-only cross-module guest PC and block overlap audit for generated EE IR.

Counts distinct guest PCs per module, not native instructions or bytes.
Repeated lowering within one module (delay slots, counted ALU paths) is
reported separately. Never removes or rewrites IR or object files.
"""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re

GUEST_PC = re.compile(rb"^\s*; (0x[0-9a-fA-F]+): ")
BLOCK_PC = re.compile(rb"^bb_(0x[0-9a-fA-F]+):")


ENTRY_CASE = re.compile(rb"^\s*i32 \d+, label %bb_(0x[0-9a-fA-F]+)\s*$")
COUNT_PC = re.compile(rb"\bcall void @ee_count_guest_instruction\(ptr %rt, i32 (\d+), i32 ")
EDGE = re.compile(rb"\blabel %(bb_0x[0-9a-fA-F]+)\b")
GUEST_FUNCTION = re.compile(rb"^define\b.*@ee_fn_[0-9a-fA-F]+\(")


def scan_details(path):
    """Use the guest entry switch, not all guest-looking LLVM labels."""
    annotations, counted, cases, labels = set(), set(), set(), set()
    members, edges = defaultdict(set), defaultdict(set)
    occurrences = 0
    current = None
    saw_switch = in_switch = in_guest = saw_definition = False
    with path.open("rb") as source:
        for line in source:
            if line.startswith(b"define "):
                if in_guest:
                    break
                saw_definition = True
                in_guest = bool(GUEST_FUNCTION.match(line))
            if saw_definition and not in_guest:
                continue
            if in_guest and line.strip() == b"}":
                break
            if b"switch i32 " in line and b"label %dispatch_unknown [" in line:
                saw_switch = in_switch = True
            if in_switch:
                case = ENTRY_CASE.match(line)
                if case:
                    cases.add(int(case[1], 16))
                if line.strip() == b"]":
                    in_switch = False
            block = BLOCK_PC.match(line)
            if block:
                current = int(block[1], 16)
                labels.add(current)
            match = GUEST_PC.match(line)
            if match:
                pc = int(match[1], 16)
                annotations.add(pc)
                occurrences += 1
                if current is not None:
                    members[current].add(pc)
            match = COUNT_PC.search(line)
            if match:
                pc = int(match[1])
                counted.add(pc)
                if current is not None:
                    members[current].add(pc)
            if current is not None:
                edges[current].update(int(target[3:], 16) for target in EDGE.findall(line))
    # Appended metadata may omit duplicate dispatch owners, whereas the
    # original IR switch lists every emitted block. External stubs are absent.
    blocks = (cases & labels) if saw_switch else (labels & members.keys())
    blocks.discard(0)
    terminal = {pc: frozenset(members[pc]) for pc in blocks
                if members[pc] and not (edges[pc] & blocks)}
    return annotations | counted, blocks, occurrences, len(annotations), terminal, saw_switch


def scan_module(path):
    pcs, blocks, occurrences, _, _, _ = scan_details(path)
    return pcs, blocks, occurrences


def audit(paths, object_sizes=None, top=20):
    pc_counts = Counter()
    block_counts = Counter()
    modules = []
    owners = defaultdict(list)
    tails = defaultdict(list)
    switch_modules = 0
    for path in sorted(paths):
        pcs, blocks, occurrences, annotated, terminal, has_switch = scan_details(path)
        if not pcs:
            continue
        pc_counts.update(pcs)
        block_counts.update(blocks)
        index = len(modules)
        for pc in pcs:
            owners[pc].append(index)
        for pc, members in terminal.items():
            tails[(pc, members)].append(index)
        switch_modules += has_switch
        modules.append((path, pcs, blocks, occurrences, annotated))
    if not modules:
        raise ValueError("no guest instruction annotations found")
    total_pcs = sum(pc_counts.values())
    total_blocks = sum(block_counts.values())
    overlap = []
    contained, partial = set(), set()
    equal_pairs = contained_pairs = partial_pairs = pc_only_pairs = 0
    for index, (path, pcs, blocks, occurrences, annotated) in enumerate(modules):
        candidates = {owner for pc in pcs for owner in owners[pc] if owner > index}
        for other in candidates:
            _, other_pcs, other_blocks, _, _ = modules[other]
            if pcs == other_pcs and blocks == other_blocks:
                equal_pairs += 1
                contained.update((index, other))
            elif pcs <= other_pcs and blocks <= other_blocks:
                contained_pairs += 1
                contained.add(index)
            elif other_pcs <= pcs and other_blocks <= blocks:
                contained_pairs += 1
                contained.add(other)
            elif blocks & other_blocks:
                partial_pairs += 1
                partial.update((index, other))
            else:
                pc_only_pairs += 1
        shared = sum(pc_counts[pc] > 1 for pc in pcs)
        if shared:
            overlap.append({
                "module": path.name,
                "guest_pcs": len(pcs),
                "guest_blocks": len(blocks),
                "shared_guest_percent": 100.0 * shared / len(pcs),
                "shared_guest_pcs": shared,
                "shared_block_starts": sum(block_counts[pc] > 1 for pc in blocks),
                "object_file_bytes": (object_sizes or {}).get(path.stem + ".o"),
            })
    overlap.sort(key=lambda item: (-item["shared_guest_pcs"], item["module"]))
    return {
        "modules_scanned": len(paths),
        "guest_modules": len(modules),
        "unique_annotated_guest_pcs": len(pc_counts),
        "unique_guest_pcs": len(pc_counts),
        "module_scoped_guest_pcs": total_pcs,
        "cross_module_excess_pc_copies": total_pcs - len(pc_counts),
        "cross_module_excess_pc_percent": 100.0 * (total_pcs - len(pc_counts)) / total_pcs,
        "pcs_in_multiple_modules": sum(n > 1 for n in pc_counts.values()),
        "within_module_excess_annotations": sum(n - annotated for _, _, _, n, annotated in modules),
        "guest_modules_with_entry_switch": switch_modules,
        "blocks_in_multiple_modules": sum(n > 1 for n in block_counts.values()),
        "wholly_contained_modules": len(contained),
        "strict_containment_pairs": contained_pairs,
        "equal_cfg_coverage_pairs": equal_pairs,
        "partially_overlapping_modules": len(partial),
        "partial_overlap_pairs": partial_pairs,
        "pc_only_overlap_pairs": pc_only_pairs,
        "shared_terminal_block_candidates": sum(len(group) > 1 for group in tails.values()),
        "largest_shared_terminal_blocks": [
            {"pc": hex(pc), "guest_pcs": len(members), "module_count": len(group),
             "modules": [modules[i][0].name for i in group[:top]]}
            for (pc, members), group in sorted(
                tails.items(), key=lambda item: (-len(item[1]), item[0][0], sorted(item[0][1])))
            if len(group) > 1
        ][:top],
        "unique_block_starts": len(block_counts),
        "module_scoped_block_starts": total_blocks,
        "cross_module_excess_block_copies": total_blocks - len(block_counts),
        "overlapping_modules": len(overlap),
        "largest_overlaps": overlap[:top],
        "most_duplicated_pcs": [
            {"pc": hex(pc), "module_count": count}
            for pc, count in sorted(pc_counts.items(), key=lambda item: (-item[1], item[0]))[:top]
            if count > 1
        ],
        "limitations": [
            "counts IR guest comments plus literal accounting calls, not native instructions",
            "containment compares observed PC and block-entry sets, not edge equivalence",
            "terminal-block candidates are not proof of equivalent shared-tail regions",
            "shared guest PCs do not prove removable machine-code duplication",
            "within-module repeated annotations are excluded from cross-module counts",
            "object sizes are optional metadata and are not attributed to shared PCs",
            "scan a single current IR cache, not backup or alternate profiles",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ir_dir", type=Path)
    parser.add_argument("--objects", type=Path, help="optional objects.json manifest")
    parser.add_argument("--top", type=int, default=20)
    args = parser.parse_args()
    if args.top < 1:
        parser.error("--top must be positive")
    sizes = None
    try:
        if args.objects is not None:
            manifest = json.loads(args.objects.read_text(encoding="utf-8"))
            sizes = {item["object"]: item["size_bytes"] for item in manifest["objects"]}
        report = audit(list(args.ir_dir.glob("*.ll")), sizes, args.top)
    except (OSError, ValueError, KeyError) as exc:
        parser.error(str(exc))
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
