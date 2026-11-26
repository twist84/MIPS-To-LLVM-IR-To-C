#!/usr/bin/env python3
"""Read-only cross-module guest PC and block overlap audit for generated EE IR.

Counts distinct guest PCs per module, not native instructions or bytes.
Repeated lowering within one module (delay slots, counted ALU paths) is
reported separately. Never removes or rewrites IR or object files.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re

GUEST_PC = re.compile(rb"^\s*; (0x[0-9a-fA-F]+): ")
BLOCK_PC = re.compile(rb"^bb_(0x[0-9a-fA-F]+):")


def scan_module(path):
    pcs = set()
    blocks = set()
    occurrences = 0
    with path.open("rb") as source:
        for line in source:
            match = GUEST_PC.match(line)
            if match:
                pcs.add(int(match[1], 16))
                occurrences += 1
            elif (match := BLOCK_PC.match(line)):
                blocks.add(int(match[1], 16))
    return pcs, blocks, occurrences


def audit(paths, object_sizes=None, top=20):
    pc_counts = Counter()
    block_counts = Counter()
    modules = []
    for path in sorted(paths):
        pcs, blocks, occurrences = scan_module(path)
        if not pcs:
            continue
        pc_counts.update(pcs)
        block_counts.update(blocks)
        modules.append((path, pcs, blocks, occurrences))
    if not modules:
        raise ValueError("no guest instruction annotations found")
    total_pcs = sum(pc_counts.values())
    total_blocks = sum(block_counts.values())
    overlap = []
    for path, pcs, blocks, occurrences in modules:
        shared = sum(pc_counts[pc] > 1 for pc in pcs)
        if shared:
            overlap.append({
                "module": path.name,
                "guest_pcs": len(pcs),
                "shared_guest_pcs": shared,
                "shared_block_starts": sum(block_counts[pc] > 1 for pc in blocks),
                "object_file_bytes": (object_sizes or {}).get(path.stem + ".o"),
            })
    overlap.sort(key=lambda item: (-item["shared_guest_pcs"], item["module"]))
    return {
        "modules_scanned": len(paths),
        "guest_modules": len(modules),
        "unique_annotated_guest_pcs": len(pc_counts),
        "module_scoped_guest_pcs": total_pcs,
        "cross_module_excess_pc_copies": total_pcs - len(pc_counts),
        "cross_module_excess_pc_percent": 100.0 * (total_pcs - len(pc_counts)) / total_pcs,
        "pcs_in_multiple_modules": sum(n > 1 for n in pc_counts.values()),
        "within_module_excess_annotations": sum(n - len(pcs) for _, pcs, _, n in modules),
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
            "counts IR guest instruction comments, not native instructions",
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
