#!/usr/bin/env python3
"""Read-only EE helper duplication inventory for Windows COFF native objects.

Count exact emitted IR bodies and static direct call sites separately from
native symbol-attributed executable spans. COFF spans include possible
alignment/padding and are not exact function sizes or removable bytes.
The audit neither changes linkage nor recommends removing hot helpers.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

from ee_ownership_binary import native_executable_sections

HEADER = re.compile(rb"^define\b[^\r\n]*?@([A-Za-z_.$][\w.$]*)\(", re.M)
CALL = re.compile(rb"\b(?:call|invoke)\b[^@\r\n]*@([A-Za-z_.$][\w.$]*)\s*\(")
END = re.compile(rb"^}[ \t]*\r?$", re.M)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def ir_inventory(raw, guest):
    definitions = {}
    for match in HEADER.finditer(raw):
        end = END.search(raw, match.end())
        if end is None:
            raise ValueError("unterminated generated IR definition")
        name = match[1].decode("ascii")
        if name in definitions:
            raise ValueError("duplicate IR definition")
        body = raw[match.start():end.end()]
        # Only internal definitions are candidates; preserve literal
        # constants, types, attributes and SSA names in the digest.
        internal = b" internal " in body.splitlines()[0]
        calls = Counter(x.decode("ascii") for x in CALL.findall(body))
        definitions[name] = (internal, body, calls)
    if guest not in definitions or definitions[guest][0]:
        raise ValueError("missing external guest definition")
    helpers = {}
    for name, (internal, body, calls) in definitions.items():
        if not internal:
            continue
        helpers[name] = {
            "body_sha256": sha(body), "ir_bytes": len(body),
            "guest_direct_calls": definitions[guest][2][name],
            "helper_direct_calls": sum(c[name] for n, (i, _, c) in definitions.items() if i),
            "direct_dependencies": sorted(calls),
        }
    return helpers


def coff_spans(data):
    """Return non-overlapping named function spans, not exact code sizes.

    Use the next distinct symbol boundary (including non-functions) or
    section end. Multiple function symbols at one address are ambiguous;
    omit their spans rather than counting the same bytes more than once.
    Support ordinary COFF and Microsoft bigobj; reject other formats.
    """
    big = data[:4] == b"\0\0\xff\xff"
    if big:
        if len(data) < 56 or data[12:28] != bytes.fromhex("c7a1bad1eebaa94baf20faf66aa4dcb8"):
            raise ValueError("invalid bigobj")
        sections, symbol_pos, count = struct.unpack_from("<III", data, 44)
        table, stride = 56, 20
    else:
        machine, sections, _, symbol_pos, count, optional, _ = struct.unpack_from("<HHIIIHH", data)
        if machine != 0x8664 or optional:
            raise ValueError("expected x86-64 COFF object")
        table, stride = 20, 18
    if not sections or table + sections * 40 > len(data):
        raise ValueError("invalid section table")
    executable = {}
    for index in range(sections):
        pos = table + index * 40
        size, file_pos = struct.unpack_from("<II", data, pos + 16)
        flags, = struct.unpack_from("<I", data, pos + 36)
        if flags & 0x20000000 or flags & 0x20:
            if size and (not file_pos or file_pos + size > len(data)):
                raise ValueError("invalid executable section extent")
            executable[index + 1] = size
    if not count:
        return {}
    strings = symbol_pos + count * stride
    if symbol_pos < table + sections * 40 or strings + 4 > len(data):
        raise ValueError("invalid symbol table")
    string_size, = struct.unpack_from("<I", data, strings)
    if string_size < 4 or strings + string_size > len(data):
        raise ValueError("invalid COFF string table")

    def symbol_name(pos):
        raw = data[pos:pos + 8]
        if raw[:4] == b"\0" * 4:
            offset, = struct.unpack_from("<I", raw, 4)
            if not 4 <= offset < string_size:
                raise ValueError("invalid long symbol name")
            end = data.find(b"\0", strings + offset, strings + string_size)
            if end < 0:
                raise ValueError("unterminated symbol name")
            raw = data[strings + offset:end]
        else:
            raw = raw.split(b"\0", 1)[0]
        return raw.decode("ascii", errors="replace")

    boundaries = defaultdict(lambda: defaultdict(set))
    functions = defaultdict(lambda: defaultdict(set))
    i = 0
    while i < count:
        pos = symbol_pos + i * stride
        value, section, kind, storage, aux = struct.unpack_from(
            "<IiHBB" if big else "<IhHBB", data, pos + 8)
        if i + 1 + aux > count:
            raise ValueError("invalid COFF auxiliary count")
        if section in executable and value < executable[section]:
            name = symbol_name(pos)
            # Section symbols and debug/file records are not code boundaries.
            if storage in (2, 3, 6, 105) and not name.startswith("."):
                boundaries[section][value].add(name)
                if kind & 0x30 == 0x20:
                    functions[section][value].add(name)
        i += 1 + aux
    spans = defaultdict(list)
    for section, starts in boundaries.items():
        ordered = sorted(set(starts) | {executable[section]})
        for start, end in zip(ordered, ordered[1:]):
            names = functions[section].get(start, set())
            if len(names) == 1 and starts[start] == names:
                spans[next(iter(names))].append(end - start)
    # Ambiguous repeated names are not attributed.
    return {name: parts[0] for name, parts in spans.items() if len(parts) == 1}


def read_manifest(directory, names):
    data = (directory / "objects.json").read_bytes()
    manifest = json.loads(data)
    rows = {Path(item["input"]).name: item for item in manifest["objects"]}
    if (not manifest.get("complete") or len(rows) != len(manifest["objects"]) or
            set(rows) != names or manifest.get("optimization_level") != 5 or
            not manifest.get("target_triple", "").endswith("windows-msvc")):
        raise ValueError("expected complete matching Windows native manifest")
    return data, manifest, rows


def verified_object(directory, item, raw_hash):
    path = directory / item["object"]
    data = path.read_bytes()
    if (item["ir_sha256"] != raw_hash or item["sha256"] != sha(data) or
            item["size_bytes"] != len(data)):
        raise ValueError(f"stale IR or object: {path}")
    return data


def audit(ir, original, experiment):
    metadata_bytes = (ir / "ee_functions.json").read_bytes()
    metadata = json.loads(metadata_bytes)
    functions = {Path(fn["ir_path"]).name: fn["host_symbol"] for fn in metadata["functions"]}
    paths = {path.name: path for path in ir.glob("*.ll")}
    if len(functions) != len(metadata["functions"]) or set(paths) != set(functions):
        raise ValueError("IR inventory differs from metadata")
    base_bytes, base, base_rows = read_manifest(original, set(functions))
    mixed = experiment / "objects"
    mixed_bytes, overlay, mixed_rows = read_manifest(mixed, set(functions))
    plan = json.loads((experiment / "size-plan.json").read_text(encoding="utf-8"))
    trials = {row["module"]: row for row in plan["modules"]}
    if (plan["mode"] != "experimental_selective_native_size" or
            plan["metadata_sha256"] != sha(metadata_bytes) or
            plan["source_manifest_sha256"] != sha(base_bytes) or
            (experiment / "ee_functions.json").read_bytes() != metadata_bytes or
            overlay.get("production_cache_compatible") is not False or
            overlay.get("experimental_mixed_pipeline") != "O5-selective-Oz" or
            overlay["native_target"] != base["native_target"] or
            len(trials) != len(plan["modules"]) or set(trials) != set(functions)):
        raise ValueError("expected full-inventory matching selective Oz experiment")
    groups = {}
    totals = {mode: Counter() for mode in ("O5", "mixed")}
    for number, (name, guest) in enumerate(sorted(functions.items()), 1):
        raw = paths[name].read_bytes()
        digest = sha(raw)
        helpers = ir_inventory(raw, guest)
        row = trials[name]
        b, m = base_rows[name], mixed_rows[name]
        if (row["ir_sha256"] != digest or row["baseline_sha256"] != b["sha256"] or
                m["sha256"] != (row["trial"]["sha256"] if row["accepted"] else b["sha256"])):
            raise ValueError(f"trial/manifest mismatch: {name}")
        objects = {"O5": verified_object(original, b, digest),
                   "mixed": verified_object(mixed, m, digest)}
        spans = {mode: coff_spans(data) for mode, data in objects.items()}
        for mode, data in objects.items():
            size = native_executable_sections(data)[0]
            attributed = sum(spans[mode].get(helper, 0) for helper in helpers)
            guest_span = spans[mode].get(guest, 0)
            if attributed + guest_span > size:
                raise ValueError("overlapping native span accounting")
            totals[mode].update(modules=1, executable_bytes=size,
                                helper_named_span_bytes=attributed,
                                guest_named_span_bytes=guest_span,
                                unattributed_bytes=size-attributed-guest_span,
                                modules_with_guest_symbol=int(guest_span > 0))
        for helper, info in helpers.items():
            key = (helper, info["body_sha256"])
            group = groups.setdefault(key, {
                "name": helper, "body_sha256": info["body_sha256"],
                "body_ir_bytes": info["ir_bytes"], "definitions": 0,
                "guest_direct_calls": 0, "helper_direct_calls": 0,
                "direct_dependencies": info["direct_dependencies"],
                "native": {mode: {"named_copies": 0, "span_bytes": 0,
                                  "min_span_bytes": None, "max_span_bytes": 0}
                           for mode in spans},
            })
            group["definitions"] += 1
            for field in ("guest_direct_calls", "helper_direct_calls"):
                group[field] += info[field]
            for mode, table in spans.items():
                if helper not in table:
                    continue
                amount = table[helper]
                n = group["native"][mode]
                n["named_copies"] += 1
                n["span_bytes"] += amount
                n["min_span_bytes"] = amount if n["min_span_bytes"] is None else min(n["min_span_bytes"], amount)
                n["max_span_bytes"] = max(n["max_span_bytes"], amount)
        if number % 1024 == 0 or number == len(functions):
            print(f"[{number}/{len(functions)}] helper inventory", flush=True)
    return {
        "schema": 1, "mode": "read_only_ee_helper_inventory",
        "metadata_sha256": sha(metadata_bytes), "original_manifest_sha256": sha(base_bytes),
        "mixed_manifest_sha256": sha(mixed_bytes),
        "modules": len(functions), "native": totals,
        "groups": sorted(groups.values(), key=lambda g: (-g["native"]["mixed"]["span_bytes"], g["name"], g["body_sha256"])),
        "limitations": [
            "COFF symbol spans include alignment and may omit renamed, inlined, folded or stripped helpers",
            "not exact function sizes, exclusive execution costs or removable byte estimates",
            "identical IR body text does not certify equivalent transitive dependencies or linkage",
            "static direct call counts do not represent runtime frequencies",
            "no IR, object, cache or linkage modifications",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ir", type=Path)
    parser.add_argument("objects", type=Path)
    parser.add_argument("--experiment", type=Path, required=True)
    parser.add_argument("--json", type=Path, required=True)
    args = parser.parse_args()
    try:
        output = args.json.resolve()
        if output.exists() or any(output.is_relative_to(path.resolve()) for path in
                                  (args.ir, args.objects, args.experiment)):
            raise ValueError("use a new report outside all caches")
        report = audit(args.ir.resolve(), args.objects.resolve(), args.experiment.resolve())
        with output.open("x", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2)
            stream.write("\n")
        for mode, total in report["native"].items():
            print(f"{mode}: {total['helper_named_span_bytes']:,} named helper span bytes / "
                  f"{total['executable_bytes']:,} executable bytes")
        for group in report["groups"][:15]:
            native = group["native"]["mixed"]
            print(f"{group['name']}: {native['named_copies']:,} named copies, "
                  f"{native['span_bytes']:,} mixed span bytes")
    except (OSError, ValueError, KeyError, TypeError, struct.error) as error:
        print(f"EE helper inventory refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
