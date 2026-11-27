#!/usr/bin/env python3
"""Read-only original IRX ELF metadata audit against captured linked text.

Matches loader metadata, not a cryptographic identity of the original ELF.
Never authorizes native lowering or interprets all pointers as functions.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from audit_iop_function_discovery import inventory


def checked(data, offset, size):
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        raise ValueError("ELF range outside file")
    return data[offset:offset + size]


def cstring(data, offset):
    if not 0 <= offset < len(data):
        raise ValueError("ELF string outside table")
    end = data.find(b"\0", offset)
    if end == -1:
        raise ValueError("unterminated ELF string")
    return data[offset:end].decode("utf-8", errors="replace")


def parse_irx(data):
    if len(data) < 52 or data[:6] != b"\x7fELF\x01\x01":
        raise ValueError("not ELF32 little-endian")
    (kind, machine, _, _, phoff, shoff, _, _, phsize, phnum,
     shsize, shnum, shstr) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    if kind not in (0xff80, 0xff81) or machine != 8 or phsize < 32 or not phnum:
        raise ValueError("not a Sony IOP relocatable ELF")
    checked(data, phoff, phsize * phnum)
    programs = [struct.unpack_from("<8I", data, phoff + i * phsize)
                for i in range(phnum)]
    loads = [p for p in programs if p[0] == 1]
    modules = [p for p in programs if p[0] == 0x70000080]
    if len(loads) != 1 or len(modules) != 1:
        raise ValueError("expected exactly one load and IOPMOD segment")
    load, module = loads[0], modules[0]
    text = checked(data, load[1], load[4])
    if not text or load[5] < len(text):
        raise ValueError("invalid load segment")
    mod = checked(data, module[1], 26)
    _, entry, gp, text_size, data_size, bss_size = struct.unpack_from("<6I", mod)
    if text_size > len(text) or entry >= max(load[5], text_size + data_size + bss_size):
        raise ValueError("invalid IOPMOD text or entry")
    name = data[module[1] + 26:module[1] + 82].split(b"\0", 1)[0]
    name = name.decode("utf-8", errors="replace") or "IRX"

    sections = []
    if shnum:
        if shsize < 40:
            raise ValueError("invalid section header size")
        checked(data, shoff, shsize * shnum)
        sections = [struct.unpack_from("<10I", data, shoff + i * shsize)
                    for i in range(shnum)]
    names = b""
    if shstr:
        if shstr >= len(sections) or sections[shstr][1] != 3:
            raise ValueError("invalid section name table")
        section = sections[shstr]
        names = checked(data, section[4], section[5])

    def section_name(index):
        return cstring(names, sections[index][0]) if names else ""

    symbols = []
    relocations = []
    for index, section in enumerate(sections):
        _, typ, flags, addr, offset, size, link, info, align, entsize = section
        if typ in (2, 11):  # SYMTAB / DYNSYM
            if entsize != 16 or size % entsize or link >= len(sections):
                raise ValueError("invalid symbol table")
            strings = sections[link]
            if strings[1] != 3:
                raise ValueError("invalid symbol string table")
            table = checked(data, strings[4], strings[5])
            for n, value, length, sym_info, _, sec in struct.iter_unpack(
                    "<IIIBBH", checked(data, offset, size)):
                if sym_info & 15 != 2 or not 0 < sec < len(sections):
                    continue
                target = sections[sec]
                if not target[2] & 4 or value % 4 or not (
                        target[3] <= value < target[3] + target[5]) or value >= text_size:
                    continue
                symbols.append({"name": cstring(table, n), "offset": value,
                                "size": length, "section": section_name(sec)})
        elif typ == 9:  # REL, same offsets as irx_loader.cpp
            if size % 8:
                raise ValueError("invalid REL table")
            for site, rel_info in struct.iter_unpack("<II", checked(data, offset, size)):
                relocations.append({"offset": site, "type": rel_info & 255,
                                    "symbol_index": rel_info >> 8})
    return {"name": name, "entry_offset": entry, "gp_offset": gp,
            "text_size": text_size, "data_size": data_size, "bss_size": bss_size,
            "elf_type": kind, "section_count": len(sections),
            "sections": [{"name": section_name(i), "type": s[1], "flags": s[2],
                          "address": s[3], "size": s[5]} for i, s in enumerate(sections)],
            "function_symbols": symbols, "relocations": relocations}, text


def capture_mismatches(metadata, linked, parsed):
    base = metadata["base"]
    fields = {
        "name": (metadata["name"], parsed["name"]),
        "text_size": (len(linked), parsed["text_size"]),
        "entry": (metadata.get("entry"), base + parsed["entry_offset"]),
        "gp": (metadata.get("gp"), base + parsed["gp_offset"]),
    }
    return {name: {"captured": values[0], "elf": values[1]}
            for name, values in fields.items() if values[0] != values[1]}


def compare_capture(metadata, linked, parsed, original, discovery):
    base = metadata["base"]
    if capture_mismatches(metadata, linked, parsed):
        return None
    known = {r["entry"] for r in discovery["regions"]}
    reached = {pc for r in discovery["regions"]
               for pc in r["ordinary_pcs"] + r["delay_pcs"]}
    reloc_sites = {rel["offset"] for rel in parsed["relocations"]}
    candidates = []
    verified = []
    for rel in parsed["relocations"]:
        site, kind = rel["offset"], rel["type"]
        if site % 4 or site + 4 > len(original) or site + 4 > len(linked):
            continue
        before = struct.unpack_from("<I", original, site)[0]
        after = struct.unpack_from("<I", linked, site)[0]
        target = None
        if kind == 4 and before >> 26 == 3:
            expected = ((before & 0xfc000000) |
                        (((before & 0x03ffffff) + (base >> 2)) & 0x03ffffff))
            if after == expected:
                target = (((base + site + 4) & 0xf0000000) |
                          ((after & 0x03ffffff) << 2))
                verified.append({"site": base + site, "target": target,
                                 "site_reached": base + site in reached})
        elif kind == 2 and after == (before + base) & 0xffffffff:
            target = after
        if target is not None and base <= target < base + len(linked) and target % 4 == 0:
            candidates.append({"site": base + site, "target": target,
                               "kind": "relocated_jal" if kind == 4 else "relocated_word",
                               "already_entry": target in known,
                               "already_reached": target in reached})
    differing = []
    for site in range(0, len(linked) - 3, 4):
        if site not in reloc_sites and original[site:site + 4] != linked[site:site + 4]:
            differing.append(base + site)
    return {"metadata_match_only": True,
            "nonrelocation_changed_word_count": len(differing),
            "nonrelocation_changed_pcs": differing,
            "verified_relocated_calls": verified,
            "relocation_target_candidates": candidates,
            "new_symbol_entries": sorted({base + s["offset"] for s in parsed["function_symbols"]}
                                         - known)}


def discover_relocation_roots(metadata, linked, parsed, comparison, discovery):
    """Read-only shadow: use verified JALs in executable section ranges.

    No R_MIPS_32 values, unmatched ELFs, or already reached delay slots
    are promoted. Even a relocation-backed call is candidate evidence.
    """
    base = metadata["base"]
    ranges = [(base + s["address"], base + s["address"] + s["size"])
              for s in parsed["sections"] if s["flags"] & 4 and s["size"]]

    def executable(pc):
        return any(lo <= pc and pc + 4 <= hi for lo, hi in ranges)

    sites = {}
    for call in comparison["relocation_target_candidates"]:
        if (call["kind"] == "relocated_jal" and not call["already_reached"] and
                executable(call["site"]) and executable(call["target"])):
            sites.setdefault(call["target"], set()).add(call["site"])
    roots = sorted(sites)
    expanded = inventory(metadata, linked, extended=True, relocated_call_entries=roots)
    old = {pc for r in discovery["regions"]
           for pc in r["ordinary_pcs"] + r["delay_pcs"]}
    new = {pc for r in expanded["regions"]
           for pc in r["ordinary_pcs"] + r["delay_pcs"]}
    return {
        "capture_identity": {"base": base, "text_size": len(linked),
                             "hash": metadata["hash"]},
        "new_roots": [{"entry": pc, "call_sites": sorted(sites[pc])} for pc in roots],
        "newly_reached_pcs": sorted(new - old),
        "previous_candidate_count": discovery["candidate_count"],
        "previous_unvisited_words": discovery["unvisited_aligned_words"],
        "inventory": expanded,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--disc-root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--discover-relocations", action="store_true",
                        help="Shadow discovery from verified executable JAL relocations")
    args = parser.parse_args()
    try:
        captures = []
        for path in sorted(args.capture.glob("*.json")):
            metadata = json.loads(path.read_text(encoding="utf-8"))
            linked = path.with_suffix(".bin").read_bytes()
            discovery = inventory(metadata, linked, extended=True)
            captures.append((path, metadata, linked, discovery))
        if not captures or not args.disc_root.is_dir():
            raise ValueError("capture or disc root missing")
        results = {path.stem: [] for path, _, _, _ in captures}
        errors = []
        unmatched_diagnostics = {path.stem: [] for path, _, _, _ in captures}
        scanned = 0
        # Search loose IRX files only. Do not infer identity from filenames.
        for path in sorted(args.disc_root.rglob("*")):
            if not path.is_file() or path.suffix.lower() != ".irx":
                continue
            scanned += 1
            raw = path.read_bytes()
            try:
                parsed, original = parse_irx(raw)
            except (ValueError, struct.error) as exc:
                errors.append({"path": str(path.relative_to(args.disc_root)), "error": str(exc)})
                continue
            for capture, meta, linked, discovery in captures:
                match = compare_capture(meta, linked, parsed, original, discovery)
                if match is not None:
                    record = {
                        "path": str(path.relative_to(args.disc_root)),
                        "elf_sha256": hashlib.sha256(raw).hexdigest(),
                        "metadata": parsed, "comparison": match,
                    }
                    if args.discover_relocations:
                        record["relocation_discovery"] = discover_relocation_roots(
                            meta, linked, parsed, match, discovery)
                    results[capture.stem].append(record)
                else:
                    unmatched_diagnostics[capture.stem].append({
                        "path": str(path.relative_to(args.disc_root)),
                        "mismatches": capture_mismatches(meta, linked, parsed),
                    })
        receipt = {"schema": 1, "kind": "iop_original_irx_metadata",
                   "scanned_irxs": scanned, "parse_errors": errors, "matches": results,
                   "unmatched_diagnostics": {name: unmatched_diagnostics[name]
                                             for name, matches in results.items() if not matches}}
        # Keep ambiguous source matches in the audit, but do not publish
        # their alternative ownership inventories as accepted evidence.
        for matches in results.values():
            if len(matches) != 1:
                for match in matches:
                    match.pop("relocation_discovery", None)
        with args.out.open("x", encoding="utf-8") as output:
            json.dump(receipt, output, indent=2)
            output.write("\n")
        for name, matches in results.items():
            print(f"{name}: {len(matches)} metadata matches")
            for match in matches:
                m, c = match["metadata"], match["comparison"]
                print(f"  {match['path']}: {len(m['function_symbols'])} function symbols, "
                      f"{len(m['relocations'])} relocations, "
                      f"{len(c['verified_relocated_calls'])} verified relocated JALs, "
                      f"{len(c['relocation_target_candidates'])} in-text target candidates")
                if "relocation_discovery" in match:
                    shadow = match["relocation_discovery"]
                    inv = shadow["inventory"]
                    print(f"  shadow: {len(shadow['new_roots'])} relocation roots, "
                          f"{len(shadow['newly_reached_pcs'])} new words, "
                          f"{inv['candidate_count']} candidates, "
                          f"{inv['unvisited_aligned_words']} unvisited words, "
                          f"{len(inv['shared_ordinary_pcs'])} shared PCs, "
                          f"{len(inv['ordinary_delay_overlap'])} delay overlaps")
            if not matches:
                for candidate in sorted(unmatched_diagnostics[name],
                                        key=lambda row: len(row["mismatches"]))[:2]:
                    print(f"  nearest: {candidate['path']}: "
                          f"{json.dumps(candidate['mismatches'], sort_keys=True)}")
        print(f"Receipt: {args.out} ({scanned} loose IRXs scanned)")
    except (OSError, ValueError, KeyError, TypeError, struct.error) as exc:
        parser.exit(1, f"IRX metadata audit: {exc}\n")


if __name__ == "__main__":
    main()
