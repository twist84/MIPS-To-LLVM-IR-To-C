#!/usr/bin/env python3
"""Read-only, conservative R3000A control-flow inventory of linked IRX captures.

This is a candidate discovery audit, not a decoder or proof of native safety.
No runtime, compiler or capture files are changed.
"""

import argparse
import collections
import json
import re
import struct
from pathlib import Path

MASK = 0xffffffff


def fnv1a(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def classify(word):
    op = word >> 26
    rs = (word >> 21) & 31
    rt = (word >> 16) & 31
    rd = (word >> 11) & 31
    sa = (word >> 6) & 31
    funct = word & 63
    if op == 0:
        if funct == 8 and rt == rd == sa == 0:
            return "return" if rs == 31 else "indirect_jump"
        if funct == 9 and rt == sa == 0:
            return "indirect_call" if rd else "indirect_jump"
        if funct in (0, 2, 3) and rs == 0:
            return "simple"
        if funct in (4, 6, 7, 32, 33, 34, 35, 36, 37, 38, 39, 42, 43) and sa == 0:
            return "simple"
        if funct in (16, 18) and rs == rt == sa == 0:
            return "simple"
        if funct in (17, 19) and rt == rd == sa == 0:
            return "simple"
        if funct in (24, 25, 26, 27) and rd == sa == 0:
            return "simple"
    elif op == 2:
        return "jump"
    elif op == 3:
        return "call"
    elif op == 1 and rt in (0, 1, 16, 17):
        return "branch_call" if rt in (16, 17) else "branch"
    elif op in (4, 5) or (op in (6, 7) and rt == 0):
        return "branch"
    elif op in (8, 9, 10, 11, 12, 13, 14, 32, 33, 35, 36, 37, 40, 41, 43):
        return "simple"
    elif op == 15 and rs == 0:
        return "simple"
    return "unsupported"


def inventory(metadata, data):
    base, size = metadata["base"], metadata["text_size"]
    digest = metadata["hash"]
    if (type(base) is not int or type(size) is not int or base & 3 or
            not 0 <= base <= MASK or size <= 0 or size != len(data) or
            base + size > MASK + 1 or not isinstance(digest, str) or
            not re.fullmatch(r"[0-9a-fA-F]{1,16}", digest) or
            fnv1a(data) != int(digest, 16)):
        raise ValueError("invalid linked capture identity")
    exports = metadata.get("exports", [])
    if not isinstance(exports, list):
        raise ValueError("invalid export inventory")

    def contains(pc):
        return type(pc) is int and pc % 4 == 0 and base <= pc <= base + size - 4

    def word_at(pc):
        return struct.unpack_from("<I", data, pc - base)[0]

    seeds = collections.defaultdict(set)
    rejected = []

    def seed(pc, origin):
        if contains(pc):
            seeds[pc].add(origin)
        else:
            rejected.append({"pc": pc, "origin": origin})

    seed(metadata.get("entry"), "module_entry")
    for pc in exports:
        seed(pc, "export")
    pending = collections.deque(sorted(seeds))
    graph = {}
    delay_by_branch = {}
    unresolved = set()
    edges = set()

    def edge(pc, target, kind, continuation=True):
        edges.add((pc, target, kind))
        if not contains(target):
            unresolved.add((pc, target, "external_" + kind))
        elif continuation:
            graph[pc].append(target)
            pending.append(target)

    while pending:
        pc = pending.popleft()
        if pc in graph:
            continue
        graph[pc] = []
        word = word_at(pc)
        kind = classify(word)
        if kind == "unsupported":
            unresolved.add((pc, pc, "unsupported"))
            continue
        if kind == "simple":
            edge(pc, (pc + 4) & MASK, "next")
            continue

        slot = (pc + 4) & MASK
        if not contains(slot):
            unresolved.add((pc, slot, "missing_delay"))
            continue
        delay_by_branch[pc] = slot
        if classify(word_at(slot)) != "simple":
            unresolved.add((pc, slot, "unsupported_delay"))
            continue

        resume = (pc + 8) & MASK
        if kind in ("call", "jump"):
            target = (((pc + 4) & 0xf0000000) | ((word & 0x03ffffff) << 2))
            edge(pc, target, kind, kind == "jump")
            if kind == "call":
                if contains(target):
                    seed(target, "direct_call")
                    pending.append(target)
                edge(pc, resume, "call_resume")
        elif kind in ("branch", "branch_call"):
            imm = word & 0xffff
            offset = (imm - 0x10000 if imm & 0x8000 else imm) * 4
            target = (pc + 4 + offset) & MASK
            edge(pc, target, kind, kind == "branch")
            if kind == "branch_call":
                if contains(target):
                    seed(target, "direct_call")
                    pending.append(target)
                edge(pc, resume, "call_resume")
            else:
                # Even statically predictable branch outcomes remain candidates.
                edge(pc, resume, "branch_fallthrough")
        else:
            unresolved.add((pc, pc, kind))
            if kind == "indirect_call":
                edge(pc, resume, "call_resume")

    owners = collections.defaultdict(set)
    regions = []
    for entry in sorted(seeds):
        visited = set()
        frontier = [entry]
        boundary = set()
        slots = set()
        while frontier:
            pc = frontier.pop()
            if pc in visited:
                continue
            if pc != entry and pc in seeds:
                boundary.add(pc)
                continue
            visited.add(pc)
            owners[pc].add(entry)
            if pc in delay_by_branch:
                slots.add(delay_by_branch[pc])
            frontier.extend(graph[pc])
        regions.append({
            "entry": entry, "origins": sorted(seeds[entry]),
            "ordinary_pcs": sorted(visited), "delay_pcs": sorted(slots),
            "other_entry_boundaries": sorted(boundary),
        })

    delay_pcs = set(delay_by_branch.values())
    ordinary = set(graph)
    overlaps = [{"pc": pc, "entries": sorted(entries)}
                for pc, entries in sorted(owners.items()) if len(entries) > 1]
    return {
        "name": metadata["name"], "base": base, "text_size": size,
        "hash": digest.lower(), "candidate_count": len(seeds),
        "ordinary_count": len(ordinary), "delay_count": len(delay_pcs),
        "unvisited_aligned_words": size // 4 - len(ordinary | delay_pcs),
        "ordinary_delay_overlap": sorted(ordinary & delay_pcs),
        "shared_ordinary_pcs": overlaps, "rejected_seeds": rejected,
        "regions": regions,
        "edges": [{"pc": pc, "target": target, "kind": kind}
                  for pc, target, kind in sorted(edges)],
        "unresolved": [{"pc": pc, "target": target, "kind": kind}
                       for pc, target, kind in sorted(unresolved)],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    captures = sorted(args.capture.glob("*.json"))
    if not captures:
        parser.error("no captured IRX metadata")
    modules = []
    try:
        for path in captures:
            record = inventory(json.loads(path.read_text(encoding="utf-8")),
                               path.with_suffix(".bin").read_bytes())
            record["capture_stem"] = path.stem
            modules.append(record)
        # Exclusive creation: never replace a capture or previous receipt.
        with args.out.open("x", encoding="utf-8") as output:
            json.dump({"schema": 1, "kind": "iop_candidate_discovery",
                       "modules": modules}, output, indent=2)
            output.write("\n")
    except (OSError, ValueError, KeyError, TypeError) as exc:
        parser.exit(1, f"IOP discovery audit: {exc}\n")
    for record in modules:
        print(f"{record['capture_stem']}: {record['candidate_count']} candidates, "
              f"{record['ordinary_count']} ordinary PCs, "
              f"{record['delay_count']} delay PCs, "
              f"{len(record['shared_ordinary_pcs'])} shared PCs, "
              f"{len(record['unresolved'])} unresolved edges")
    print(f"Receipt: {args.out}")


if __name__ == "__main__":
    main()
