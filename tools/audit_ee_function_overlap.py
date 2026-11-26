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
import struct
import subprocess
import sys

from ee_ownership_binary import executable_segments, native_executable_sections
from ee_discovery_evidence import EVIDENCE_NAME, collect, load

GUEST_PC = re.compile(rb"^\s*; (0x[0-9a-fA-F]+): ")
BLOCK_PC = re.compile(rb"^bb_(0x[0-9a-fA-F]+):")


ENTRY_CASE = re.compile(rb"^\s*i32 \d+, label %bb_(0x[0-9a-fA-F]+)\s*$")
COUNT_PC = re.compile(rb"\bcall void @ee_count_guest_instruction\(ptr %rt, i32 (\d+), i32 ")
EDGE = re.compile(rb"\blabel %(bb_0x[0-9a-fA-F]+)\b")
GUEST_FUNCTION = re.compile(rb"^define\b.*@ee_fn_[0-9a-fA-F]+\(")


def scan_details(path, *, with_graph=False):
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
    if with_graph:
        graph = {pc: (frozenset(members[pc]), frozenset(edges[pc] & blocks),
                      frozenset(edges[pc] - blocks)) for pc in blocks}
        return annotations | counted, blocks, occurrences, annotations, terminal, saw_switch, graph
    return annotations | counted, blocks, occurrences, len(annotations), terminal, saw_switch


def scan_module(path):
    pcs, blocks, occurrences, _, _, _ = scan_details(path)
    return pcs, blocks, occurrences



def shared_tail_regions(left, right):
    """Common closed regions with at least one reachable guest terminal.

    Equality here is equality of observed guest instruction membership and
    emitted guest-label successors (including external targets), not a proof
    of equivalent native execution or architectural state publication.
    """
    shared = {pc for pc in left.keys() & right.keys()
              if left[pc] == right[pc] and left[pc][0]}
    predecessors = defaultdict(set)
    for pc in shared:
        for successor in left[pc][1]:
            predecessors[successor].add(pc)
    pending = [pc for pc in shared if not left[pc][1] <= shared]
    while pending:
        pc = pending.pop()
        if pc not in shared:
            continue
        shared.remove(pc)
        pending.extend(predecessors[pc] & shared)
    # A closed cycle with no path to a terminal is not a tail.
    reaches_exit = {pc for pc in shared if not left[pc][1]}
    pending = list(reaches_exit)
    while pending:
        pc = pending.pop()
        new = (predecessors[pc] & shared) - reaches_exit
        reaches_exit.update(new)
        pending.extend(new)
    # Group connected shared blocks; leave single-block terminals to the
    # existing distinct terminal-block inventory.
    remaining = set(reaches_exit)
    while remaining:
        first = min(remaining)
        pending = [first]
        component = {first}
        remaining.remove(first)
        while pending:
            pc = pending.pop()
            adjacent = (predecessors[pc] | left[pc][1]) & remaining
            remaining.difference_update(adjacent)
            component.update(adjacent)
            pending.extend(adjacent)
        # Exclude a component if any branch can reach a closed nonterminal
        # loop rather than another member of this candidate tail.
        if len(component) > 1 and all(left[pc][1] <= component for pc in component):
            yield component


def audit(paths, object_sizes=None, top=20, *, metadata=None, discovery=None, runtime_sources=None, native_sizes=None):
    pc_counts = Counter()
    block_counts = Counter()
    modules = []
    owners = defaultdict(list)
    tails = defaultdict(list)
    switch_modules = 0
    rows = []
    module_index = {}
    annotated_pcs = set()
    graphs = []
    tail_region_count = 0
    largest_tail_regions = []
    for path in sorted(paths):
        pcs, blocks, occurrences, annotations, terminal, has_switch, graph = scan_details(path, with_graph=True)
        if not pcs:
            continue
        pc_counts.update(pcs)
        annotated_pcs.update(annotations)
        graphs.append(graph)
        block_counts.update(blocks)
        index = len(modules)
        module_index[path.name] = index
        for pc in pcs:
            owners[pc].append(index)
        for pc, members in terminal.items():
            tails[(pc, members)].append(index)
        switch_modules += has_switch
        modules.append((path, pcs, blocks, occurrences, len(annotations)))
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
            other_path, other_pcs, other_blocks, _, _ = modules[other]
            if len(blocks & other_blocks) > 1:
                for region in shared_tail_regions(graphs[index], graphs[other]):
                    tail_region_count += 1
                    row = {
                        "modules": [path.name, other_path.name],
                        "guest_blocks": len(region),
                        "guest_pcs": len(set().union(*(graphs[index][pc][0] for pc in region))),
                        "block_starts": [hex(pc) for pc in sorted(region)],
                    }
                    largest_tail_regions.append(row)
                    largest_tail_regions.sort(key=lambda item: (-item["guest_pcs"], item["modules"]))
                    del largest_tail_regions[top:]
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
        row = {
                "module": path.name,
                "guest_pcs": len(pcs),
                "guest_blocks": len(blocks),
                "shared_guest_percent": 100.0 * shared / len(pcs),
                "shared_guest_pcs": shared,
                "shared_block_starts": sum(block_counts[pc] > 1 for pc in blocks),
                "object_file_bytes": (object_sizes or {}).get(path.stem + ".o"),
                "native_executable_bytes": (native_sizes or {}).get(path.stem + ".o"),
                "wholly_contained": index in contained,
                "partially_overlapping": index in partial,
            }
        rows.append(row)
        if shared:
            overlap.append(row)
    for index, row in enumerate(rows):
        row["wholly_contained"] = index in contained
        row["partially_overlapping"] = index in partial
    ownership = discovery_ownership(metadata, discovery, runtime_sources, module_index, modules, owners, rows)
    largest_native = sorted(
        (row for row in rows if row["object_file_bytes"] is not None),
        key=lambda row: (-row["object_file_bytes"], row["module"]))
    overlap.sort(key=lambda item: (-item["shared_guest_pcs"], item["module"]))
    return {
        "modules_scanned": len(paths),
        "aggregate_ir_bytes": sum(path.stat().st_size for path in paths),
        "discovery": ownership,
        "largest_native_modules": largest_native[:top],
        "largest_native_executable_modules": sorted(largest_native, key=lambda row: (-(row["native_executable_bytes"] or 0), row["module"]))[:top],
        "multi_block_shared_tail_pair_regions": tail_region_count,
        "largest_shared_tail_regions": largest_tail_regions,
        "module_details": rows,
        "guest_modules": len(modules),
        "unique_annotated_or_counted_guest_pcs": len(pc_counts),
        "unique_annotated_guest_pcs": len(annotated_pcs),
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
            "shared-tail candidates compare guest PC membership and observed guest-label edges, not native semantics",
            "shared guest PCs do not prove removable machine-code duplication",
            "within-module repeated annotations are excluded from cross-module counts",
            "object sizes are optional metadata and are not attributed to shared PCs",
            "scan a single current IR cache, not backup or alternate profiles",
        ],
    }



def runtime_evidence(logs):
    """Positive evidence only: an expansion sibling is not a proven miss."""
    result = defaultdict(set)
    for path in logs:
        with path.open(encoding="utf-8", errors="replace") as source:
            for line in source:
                for pc, sources in collect(line).items():
                    result[pc].update(sources)
    return result


def discovery_ownership(metadata, discovery, runtime_sources, module_index, modules, owners, rows):
    if metadata is None:
        return None
    functions = metadata["functions"]
    static = {int(pc, 0) for pc in metadata.get("discovered_entries", [])}
    source_map = {int(pc, 0): set(values) for pc, values in
                  (discovery or {}).get("entry_sources", {}).items()}
    evidence = runtime_sources or {}
    entries = static | set(source_map) | set(evidence)
    module_entries = {}
    order = {}
    dispatch_owners = defaultdict(list)
    for position, fn in enumerate(functions):
        pc = int(fn["vaddr"], 0)
        # Metadata may be moved between Windows and Unix.
        name = fn["ir_path"].replace("\\", "/").rsplit("/", 1)[-1]
        entries.add(pc)
        if name not in module_index:
            continue
        index = module_index[name]
        order[index] = position
        module_entries[pc] = index
        for block in fn.get("block_pcs", []):
            dispatch_owners[int(block, 0)].append(index)

    counts = Counter()
    shared_by_source = Counter()
    result = {}
    for pc in sorted(entries):
        sources = set(source_map.get(pc, ())) | set(evidence.get(pc, ()))
        if pc in static and not source_map.get(pc):
            sources.add("static_unattributed")
        if pc not in static and pc in module_entries and not evidence.get(pc):
            sources.add("nonstatic_inventory_unattributed")
        if not sources:
            sources.add("unattributed")
        counts.update(sources)
        index = module_entries.get(pc)
        other_owners = [owner for owner in owners.get(pc, ()) if owner != index]
        previous = [owner for owner in other_owners
                    if index is not None and order.get(owner, 1 << 60) < order[index]]
        row = {
            "sources": sorted(sources),
            "module": modules[index][0].name if index is not None else None,
            "other_native_modules_covering_pc": len(other_owners),
            "earlier_inventory_modules_covering_pc": len(previous),
            "other_dispatch_owners": sum(owner != index for owner in dispatch_owners[pc]),
        }
        if index is not None:
            rows[index]["entry_pc"] = hex(pc)
            rows[index]["discovery_sources"] = sorted(sources)
            rows[index]["earlier_inventory_modules_covering_entry"] = len(previous)
            if rows[index]["shared_guest_pcs"]:
                shared_by_source.update(sources)
        result[hex(pc)] = row
    return {
        "static_entries": len(static),
        "static_source_snapshot_available": discovery is not None,
        "sources_count": dict(counts),
        "overlapping_modules_by_source": dict(shared_by_source),
        "confirmed_runtime_misses_covered_by_other_modules": sum(
            "runtime_cache_miss" in row["sources"] and row["other_native_modules_covering_pc"] > 0
            for row in result.values()),
        "nonstatic_entries_covered_by_earlier_inventory": sum(
            int(pc, 0) not in static and row["earlier_inventory_modules_covering_pc"] > 0
            for pc, row in result.items()),
        "entries": result,
        "limitations": [
            "source categories are non-exclusive; missing historic logs cannot prove a runtime cache miss",
            "earlier inventory order is not necessarily historical compilation order after recovery",
            "dispatch ownership is distinct from native code coverage",
        ],
    }


def read_object_inventory(manifest_path):
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if not manifest.get("complete"):
        raise ValueError("object manifest is incomplete")
    sizes, sections = {}, {}
    for item in manifest["objects"]:
        name = item["object"]
        if name in sizes or Path(name).name != name:
            raise ValueError("invalid or duplicate object filename")
        path = manifest_path.parent / name
        data = path.read_bytes()
        if len(data) != item["size_bytes"]:
            raise ValueError(f"object size differs from manifest: {path}")
        sizes[name] = len(data)
        sections[name], _ = native_executable_sections(data)
    return sizes, sections


def summary(report, output=sys.stderr):
    def line(label, key):
        value = report.get(key)
        print(f"{label}: {value:,}" if isinstance(value, int) else
              f"{label}: {value if value is not None else 'not supplied'}", file=output)
    for label, key in (
        ("Guest modules", "guest_modules"),
        ("Unique guest PCs", "unique_guest_pcs"),
        ("Module-scoped guest PCs", "module_scoped_guest_pcs"),
        ("Excess guest PC copies", "cross_module_excess_pc_copies"),
        ("Unique guest blocks", "unique_block_starts"),
        ("Module-scoped guest blocks", "module_scoped_block_starts"),
        ("Multiply emitted guest blocks", "blocks_in_multiple_modules"),
        ("Contained modules", "wholly_contained_modules"),
        ("Partially overlapping modules", "partially_overlapping_modules"),
        ("Shared terminal-block candidates", "shared_terminal_block_candidates"),
        ("Multi-block shared tail pair regions", "multi_block_shared_tail_pair_regions"),
        ("Original ELF bytes", "original_elf_bytes"),
        ("Executable ELF segment bytes", "executable_elf_segment_bytes"),
        ("IR bytes", "aggregate_ir_bytes"),
        ("Native object bytes", "aggregate_native_object_bytes"),
        ("Native object executable section bytes", "native_object_executable_bytes"),
        ("Linked library bytes", "linked_library_bytes"),
        ("Linked executable section bytes", "linked_executable_section_bytes"),
        ("Native executable bytes / unique guest PC", "native_bytes_per_unique_guest_instruction"),
    ):
        line(label, key)
    print("Worst overlapping modules:", file=output)
    for row in report["largest_overlaps"]:
        print(f"  {row['module']}: {row['shared_guest_pcs']}/{row['guest_pcs']} PCs, "
              f"{row['shared_block_starts']} shared blocks", file=output)
    if report["discovery"]:
        print("Discovery sources (non-exclusive):", report["discovery"]["sources_count"], file=output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ir_dir", type=Path)
    parser.add_argument("--objects", type=Path, help="complete objects.json manifest")
    parser.add_argument("--elf", type=Path, help="original ELF (defaults to metadata path)")
    parser.add_argument("--library", type=Path, help="linked DLL/shared library")
    parser.add_argument("--metadata", type=Path, help="defaults to ir_dir/ee_functions.json")
    parser.add_argument("--discovery-tool", type=Path, help="built ee_discovery_audit executable")
    parser.add_argument("--discovery-cache", type=Path, help="defaults to ir_dir/ee_discovery_cache.bin")
    parser.add_argument("--runtime-evidence", type=Path, help="ELF-bound runtime discovery ledger")
    parser.add_argument("--log", type=Path, action="append", default=[], help="historical expansion log; repeatable")
    parser.add_argument("--json", type=Path, help="write JSON report here; console prints summary")
    parser.add_argument("--top", type=int, default=20)
    args = parser.parse_args()
    if args.top < 1:
        parser.error("--top must be positive")
    try:
        meta_path = args.metadata or args.ir_dir / "ee_functions.json"
        metadata = json.loads(meta_path.read_text(encoding="utf-8")) if meta_path.is_file() else None
        elf = args.elf
        if elf is None and metadata and metadata.get("elf"):
            from shared_ee_ir import elf_path
            candidate = elf_path(metadata, meta_path.parent)
            if candidate.is_file():
                elf = candidate
        discovery = None
        if args.discovery_tool:
            if elf is None:
                raise ValueError("--discovery-tool requires the original ELF")
            cache = args.discovery_cache or args.ir_dir / "ee_discovery_cache.bin"
            proc = subprocess.run([str(args.discovery_tool.resolve()), str(elf), str(cache)],
                                  capture_output=True, text=True, check=True)
            discovery = json.loads(proc.stdout)
            if metadata and set(discovery["entry_sources"]) != set(metadata.get("discovered_entries", [])):
                raise ValueError("discovery snapshot does not match metadata entry inventory")
        sizes = native = None
        library = args.library
        if args.objects:
            sizes, native = read_object_inventory(args.objects)
            if library is None:
                for name in ("game-translated.dll", "libgame-translated.so"):
                    candidate = args.objects.parent.parent / name
                    if candidate.is_file():
                        library = candidate
                        break
        paths = list(args.ir_dir.glob("*.ll"))
        if metadata:
            names = {fn["ir_path"].replace("\\", "/").rsplit("/", 1)[-1]
                     for fn in metadata["functions"]}
            if names != {path.name for path in paths}:
                raise ValueError("IR files differ from authoritative function inventory")
        if sizes is not None and set(sizes) != {path.stem + ".o" for path in paths}:
            raise ValueError("object manifest and IR module inventory differ")
        evidence = runtime_evidence(args.log)
        ledger = args.runtime_evidence
        if ledger is None and args.objects:
            candidate = args.objects.parent.parent / EVIDENCE_NAME
            if candidate.is_file():
                ledger = candidate
        if ledger is not None:
            if elf is None or not ledger.is_file():
                raise ValueError("runtime evidence requires existing ledger and original ELF")
            for pc, sources in load(ledger, elf).items():
                evidence[pc].update(sources)
        report = audit(paths, sizes, args.top, metadata=metadata, discovery=discovery,
                       runtime_sources=evidence, native_sizes=native)
        if elf:
            data = elf.read_bytes()
            report["original_elf_bytes"] = len(data)
            report["executable_elf_segment_bytes"] = executable_segments(data)
        if sizes is not None:
            report["aggregate_native_object_bytes"] = sum(sizes.values())
            report["native_object_executable_bytes"] = sum(native.values())
            report["native_bytes_per_unique_guest_instruction"] = (
                sum(native.values()) / report["unique_guest_pcs"])
        if library:
            data = library.read_bytes()
            report["linked_library_bytes"] = len(data)
            raw, virtual = native_executable_sections(data)
            report["linked_executable_section_bytes"] = raw
            report["linked_executable_virtual_bytes"] = virtual
        report["limitations"].append(
            "object executable bytes are file-backed section sizes, not attributable duplicate bytes; linked executable bytes include host/dispatcher code")
        if args.json:
            destination = args.json.resolve()
            protected = [args.ir_dir.resolve()]
            if args.objects:
                protected.append(args.objects.parent.resolve())
            if any(directory == destination or directory in destination.parents for directory in protected):
                raise ValueError("write the audit JSON outside IR and object cache directories")
            args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            summary(report, sys.stdout)
            print(f"JSON report: {args.json}")
        else:
            print(json.dumps(report, indent=2))
            summary(report)
    except (OSError, ValueError, KeyError, struct.error, subprocess.CalledProcessError) as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    main()
