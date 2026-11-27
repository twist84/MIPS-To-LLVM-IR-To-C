#!/usr/bin/env python3
"""Read-only audit of relocation-backed absolute jump table candidates.

A contiguous relocated pointer run is not a proven switch bound.
Original file data is not assumed equal to mutable guest RAM.
"""
import argparse
import collections
import hashlib
import json
import struct
from pathlib import Path

from audit_iop_function_discovery import inventory, MASK
from audit_iop_indirect_targets import constant_indirect_targets
from audit_iop_irx_metadata import parse_irx, capture_mismatches
from build_iop_aot import relocation_receipt_roots


def indexed_load_base(producer):
    """Recognize only LW(constant + (index << 2) + signed offset).

    Do not use arbitrary constants somewhere in a nested pointer DAG.
    Return an address candidate, not a proven valid index range.
    """
    nodes = {node["pc"]: node for node in producer["definitions"]}
    target = producer["target"]
    load = nodes.get(target.get("producer_pc"), {})
    if load.get("kind") != "load" or load.get("width") != 4:
        return None
    address = nodes.get(load["inputs"][0].get("producer_pc"), {})
    if address.get("kind") != "addu":
        return None
    for scaled, constant in (address["inputs"], reversed(address["inputs"])):
        shift = nodes.get(scaled.get("producer_pc"), {})
        if (shift.get("kind") == "sll" and shift.get("shift") == 2 and
                "constant" in constant and "constant" not in scaled):
            return (constant["constant"] + load["offset"]) & MASK
    return None


def table_candidates(metadata, linked, parsed, original, discovery, indirect,
                     max_entries=256):
    if max_entries < 1:
        raise ValueError("max_entries must be positive")
    base = metadata["base"]
    end = base + len(linked)
    ordinary = {pc for region in discovery["regions"] for pc in region["ordinary_pcs"]}
    delay = {pc for region in discovery["regions"] for pc in region["delay_pcs"]}
    entries = {region["entry"] for region in discovery["regions"]}
    owners = collections.defaultdict(set)
    for region in discovery["regions"]:
        for pc in region["ordinary_pcs"]:
            owners[pc].add(region["entry"])
    # Require a single allocated, file-backed, non-executable section.
    data_sections = [s for s in parsed["sections"]
                     if s["type"] == 1 and s["flags"] & 2 and not s["flags"] & 4]
    executable = [(base + s["address"], base + s["address"] + s["size"])
                  for s in parsed["sections"] if s["flags"] & 4]
    rels = collections.defaultdict(list)
    for rel in parsed["relocations"]:
        rels[rel["offset"]].append(rel["type"])
    result = []
    for row in indirect["unresolved"]:
        if row["kind"] != "indirect_jump":
            continue
        address = indexed_load_base(row["producer"])
        record = {"site": row["site"], "table_base": address,
                  "entries": [], "bound_proven": False}
        result.append(record)
        if address is None:
            record["stop"] = "unsupported_address_expression"
            continue
        start = address - base
        sections = [s for s in data_sections
                    if s["address"] <= start and start + 4 <= s["address"] + s["size"]]
        if start < 0 or start % 4 or len(sections) != 1:
            record["stop"] = "not_unique_file_backed_data_section"
            continue
        section = sections[0]
        record["section"] = section["name"]
        limit = section["address"] + section["size"]
        # Follow the loader's R_MIPS_32 in-place addend rule. Do not
        # synthesize unrelocated pointers or scan arbitrary data words.
        for i in range(max_entries):
            offset = start + i * 4
            if offset + 4 > min(limit, len(original)):
                record["stop"] = "section_or_file_end"
                break
            if rels[offset] != [2]:
                record["stop"] = "missing_or_ambiguous_r_mips_32"
                break
            addend = struct.unpack_from("<I", original, offset)[0]
            target = (base + addend) & MASK
            if (target % 4 or not base <= target <= end - 4 or
                    not any(lo <= target and target + 4 <= hi for lo, hi in executable)):
                record["stop"] = "not_aligned_executable_text"
                break
            if offset + 4 <= len(linked):
                actual = struct.unpack_from("<I", linked, offset)[0]
                if actual != target:
                    record["stop"] = "linked_word_mismatch"
                    break
            record["entries"].append({
                "slot": base + offset, "target": target,
                "linked_word_verified": offset + 4 <= len(linked),
                "already_ordinary": target in ordinary, "already_delay": target in delay,
                "already_entry": target in entries,
                "shares_site_owner": bool(owners[row["site"]] & owners[target]),
            })
        else:
            record["stop"] = "audit_limit"
    return result


def discover_table_edges(metadata, linked, discovery, roots, tables):
    """Reconstruct a shadow CFG with non-exhaustive table candidate edges.

    Retain unresolved indirect jumps; do not promote targets into roots.
    Report conflicts instead of silently choosing an owner.
    """
    pairs = sorted({(table["site"], entry["target"])
                    for table in tables for entry in table["entries"]})
    sites = {row["pc"] for row in discovery["unresolved"]
             if row["kind"] == "indirect_jump"}
    if any(site not in sites for site, _ in pairs):
        raise ValueError("table candidate source not in baseline indirect inventory")
    expanded = inventory(metadata, linked, extended=True,
                         relocated_call_entries=roots, candidate_jump_edges=pairs)
    old = {pc for region in discovery["regions"]
           for pc in region["ordinary_pcs"] + region["delay_pcs"]}
    new = {pc for region in expanded["regions"]
           for pc in region["ordinary_pcs"] + region["delay_pcs"]}
    old_entries = {region["entry"] for region in discovery["regions"]}
    new_entries = {region["entry"] for region in expanded["regions"]}
    return {
        "candidate_edges": [{"site": site, "target": target} for site, target in pairs],
        "newly_reached_pcs": sorted(new - old),
        "new_function_entries": sorted(new_entries - old_entries),
        "previous_candidate_count": discovery["candidate_count"],
        "previous_unvisited_words": discovery["unvisited_aligned_words"],
        "inventory": expanded,
        "bounds_proven": False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--disc-root", type=Path, required=True)
    parser.add_argument("--relocation-receipt", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--discover-targets", action="store_true",
                        help="Shadow CFG with non-exhaustive table candidate edges")
    args = parser.parse_args()
    try:
        receipt = json.loads(args.relocation_receipt.read_text(encoding="utf-8"))
        modules = []
        disc = args.disc_root.resolve()
        for path in sorted(args.capture.glob("*.json")):
            metadata = json.loads(path.read_text(encoding="utf-8"))
            linked = path.with_suffix(".bin").read_bytes()
            baseline = inventory(metadata, linked, extended=True)
            roots = relocation_receipt_roots(receipt, metadata, linked, baseline)
            discovery = inventory(metadata, linked, extended=True, relocated_call_entries=roots)
            if (discovery["shared_ordinary_pcs"] or discovery["ordinary_delay_overlap"]
                    or discovery["rejected_seeds"]):
                raise ValueError(f"ambiguous discovery: {path.stem}")
            matches = receipt["matches"].get(path.stem, [])
            if len(matches) != 1:
                raise ValueError(f"no unique original ELF match: {path.stem}")
            match = matches[0]
            elf = (disc / match["path"]).resolve()
            if not elf.is_relative_to(disc):
                raise ValueError("ELF path escapes disc root")
            raw = elf.read_bytes()
            if hashlib.sha256(raw).hexdigest() != match["elf_sha256"]:
                raise ValueError(f"original ELF changed: {elf}")
            parsed, original = parse_irx(raw)
            if capture_mismatches(metadata, linked, parsed):
                raise ValueError(f"original ELF metadata differs: {elf}")
            indirect = constant_indirect_targets(metadata, linked, discovery,
                                                 audit_producers=True)
            tables = table_candidates(metadata, linked, parsed, original, discovery, indirect)
            new_targets = sorted({e["target"] for t in tables for e in t["entries"]
                                  if not e["already_ordinary"] and not e["already_delay"]})
            module = {"capture_stem": path.stem, "base": metadata["base"],
                      "text_size": len(linked), "hash": metadata["hash"],
                      "elf_sha256": match["elf_sha256"], "tables": tables,
                      "new_jump_candidates": new_targets}
            if args.discover_targets:
                module["table_discovery"] = discover_table_edges(
                    metadata, linked, discovery, roots, tables)
            modules.append(module)
        if not modules:
            raise ValueError("no captures")
        with args.out.open("x", encoding="utf-8") as output:
            json.dump({"schema": 1, "kind": "iop_jump_table_shadow", "modules": modules},
                      output, indent=2)
            output.write("\n")
        for module in modules:
            tables = module["tables"]
            print(f"{module['capture_stem']}: {sum(bool(t['entries']) for t in tables)}/"
                  f"{len(tables)} relocation-backed table candidates, "
                  f"{sum(len(t['entries']) for t in tables)} pointer slots, "
                  f"{len(module['new_jump_candidates'])} previously unreached targets")
            for t in tables:
                address = t["table_base"]
                print(f"  JR {t['site']:08x}: "
                      f"{format(address, '08x') if address is not None else 'unknown'} "
                      f"{len(t['entries'])} slots; {t['stop']}")
            if args.discover_targets:
                shadow = module["table_discovery"]
                inv = shadow["inventory"]
                print(f"  CFG shadow: {len(shadow['candidate_edges'])} edges, "
                      f"{len(shadow['newly_reached_pcs'])} new words, "
                      f"{len(shadow['new_function_entries'])} new function roots, "
                      f"{len(inv['shared_ordinary_pcs'])} shared PCs, "
                      f"{len(inv['ordinary_delay_overlap'])} delay overlaps, "
                      f"{inv['unvisited_aligned_words']} unvisited words")
        print(f"Receipt: {args.out}")
    except (OSError, ValueError, KeyError, TypeError, struct.error) as exc:
        parser.exit(1, f"IOP jump table audit: {exc}\n")


if __name__ == "__main__":
    main()
