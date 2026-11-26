#!/usr/bin/env python3
"""Build an isolated, opt-in EE deduplication link experiment.

Never replace a production translation library, IR, metadata or objects.
Only directly revalidated, nonstatic, fully published-entry matches may
be coalesced. The resulting library has different dispatch owner identity
and requires a separate runtime correctness comparison.
"""
import argparse
from collections import defaultdict
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

from audit_ee_entry_shadow import basename, compare, read_module
from link_native_objects import load_inputs
from ee_ownership_binary import native_executable_sections


def sha(data):
    return hashlib.sha256(data).hexdigest()


def dispatch_map(functions):
    result = {}
    for fn in functions:
        for pc in fn["block_pcs"]:
            pc = int(pc, 0) if isinstance(pc, str) else pc
            if pc == 0 or pc in result:
                raise ValueError("zero or duplicate published dispatch PC")
            result[pc] = basename(fn["ir_path"])
    return result


def select_routes(metadata, report, verify):
    functions = metadata["functions"]
    by_name = {basename(fn["ir_path"]): fn for fn in functions}
    static = {int(pc, 0) for pc in metadata["discovered_entries"]}
    entry = int(metadata["entry_point"], 0)
    before = dispatch_map(functions)
    if len(by_name) != len(functions):
        raise ValueError("duplicate module name")
    eligible = {}
    for row in report["entries"]:
        source = row["source"]
        if source not in by_name:
            raise ValueError("unknown source module")
        fn = by_name[source]
        pc = int(fn["vaddr"], 0)
        if pc in static or pc == entry or pc == 0:
            continue
        if int(row["pc"], 0) != pc:
            raise ValueError("source entry mismatch")
        owned = {int(value, 0) for value in fn["block_pcs"]}
        if pc not in owned or any(before.get(value) != source for value in owned):
            continue
        for candidate in row["candidates"]:
            if (candidate["status"] != "matching_emitted_ir" or
                    not candidate.get("all_source_dispatch_pcs_match")):
                continue
            owner = candidate["owner"]
            if owner == source or owner not in by_name:
                raise ValueError("invalid candidate owner")
            # Require complete, unique checks, not just the aggregate flag.
            checks = candidate["source_dispatch_checks"]
            if ({int(check["pc"], 0) for check in checks} != owned or
                    len(checks) != len(owned) or
                    any(check["status"] != "matching_emitted_ir" for check in checks)):
                raise ValueError("incomplete source dispatch checks")
            verify(source, owner, candidate, checks)
            eligible.setdefault(source, []).append(owner)

    # Never transitively trust A->B->C. Keep only direct matches to
    # retained owners. If a cyclic group lacks a retained owner, retain
    # its members rather than manufacturing a transitive certificate.
    routes = {}
    for source, owners in sorted(eligible.items()):
        retained = [owner for owner in owners if owner not in eligible]
        if retained:
            routes[source] = min(retained, key=lambda name: (
                int(by_name[name]["vaddr"], 0) not in static,
                int(by_name[name]["vaddr"], 0), name))
    return routes


def coalesce(metadata, routes):
    functions = metadata["functions"]
    before = dispatch_map(functions)
    by_name = {basename(fn["ir_path"]): fn for fn in functions}
    if any(owner in routes or source == owner or source not in by_name or
           owner not in by_name for source, owner in routes.items()):
        raise ValueError("route must terminate at a retained module")
    additions = defaultdict(list)
    for source, owner in sorted(routes.items()):
        additions[owner].extend(by_name[source]["block_pcs"])
    output = []
    for fn in functions:
        name = basename(fn["ir_path"])
        if name in routes:
            continue
        fn = dict(fn)
        fn["block_pcs"] = list(fn["block_pcs"]) + additions[name]
        fn["block_count"] = len(fn["block_pcs"])
        output.append(fn)
    after = dispatch_map(output)
    expected = {pc: routes.get(owner, owner) for pc, owner in before.items()}
    if after != expected:
        raise ValueError("dispatch map changed beyond certified transfers")
    result = dict(metadata, functions=output, function_count=len(output))
    return result


def stage(ir, objects, report_path, output):
    ir, objects, output = ir.resolve(), objects.resolve(), output.resolve()
    if output.exists() or output.is_relative_to(ir) or output.is_relative_to(objects):
        raise ValueError("output must be a new directory outside the original caches")
    metadata_path = ir / "ee_functions.json"
    metadata_bytes = metadata_path.read_bytes()
    report = json.loads(report_path.read_text(encoding="utf-8"))
    if (report.get("schema") != 1 or report.get("mode") != "shadow_only" or
            report.get("metadata_sha256") != sha(metadata_bytes)):
        raise ValueError("shadow report does not match current metadata")
    metadata, manifest, _, _ = load_inputs(metadata_path, objects)
    if json.loads(metadata_bytes) != metadata:
        raise ValueError("metadata changed during validation")
    by_input = {basename(item["input"]): item for item in manifest["objects"]}
    parsed = {}

    def module(name):
        if name not in parsed:
            if Path(name).name != name or name not in by_input:
                raise ValueError("invalid IR inventory name")
            value = read_module(ir / name)
            if value[0] != by_input[name]["ir_sha256"]:
                raise ValueError("IR digest differs from compiled object manifest")
            parsed[name] = value
        return parsed[name]

    def verify(source, owner, candidate, checks):
        a, b = module(source), module(owner)
        if (a[0] != candidate["source_sha256"] or
                b[0] != candidate["owner_sha256"]):
            raise ValueError("shadow candidate IR digest mismatch")
        for check in checks:
            status, signature = compare(a, b, int(check["pc"], 0))
            if (status != "matching_emitted_ir" or
                    signature != check["reachable_signature_sha256"]):
                raise ValueError("shadow comparison did not reproduce")

    routes = select_routes(metadata, report, verify)
    if not routes:
        raise ValueError("no directly verified routes to retained owners")
    merged = coalesce(metadata, routes)
    new_objects = []
    removed = []
    for name, item in by_input.items():
        path = (objects / item["object"]).resolve()
        if name in routes:
            size, _ = native_executable_sections(path.read_bytes())
            removed.append({"source": name, "owner": routes[name],
                            "object_file_bytes": item["size_bytes"],
                            "native_executable_bytes": size})
        else:
            new_objects.append(dict(item, object=str(path)))

    output.mkdir(parents=True, exist_ok=False)
    overlay_objects = output / "objects"
    overlay_objects.mkdir()
    (output / "ee_functions.json").write_text(
        json.dumps(merged, indent=2) + "\n", encoding="utf-8")
    (overlay_objects / "objects.json").write_text(
        json.dumps(dict(manifest, objects=new_objects, object_count=len(new_objects)),
                   indent=2) + "\n", encoding="utf-8")
    receipt = {
        "mode": "experimental_owner_coalescing",
        "metadata_sha256": sha(metadata_bytes),
        "source_functions": len(metadata["functions"]),
        "retained_functions": len(merged["functions"]),
        "dispatch_pc_count": len(dispatch_map(merged["functions"])),
        "removed": removed,
        "excluded_native_executable_bytes": sum(row["native_executable_bytes"] for row in removed),
        "excluded_object_file_bytes": sum(row["object_file_bytes"] for row in removed),
        "runtime_validation_required": True,
    }
    (output / "dedup-plan.json").write_text(
        json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ir", type=Path)
    parser.add_argument("objects", type=Path)
    parser.add_argument("shadow", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--link", action="store_true",
                        help="also link an experimental library in the new directory")
    args = parser.parse_args()
    try:
        if os.environ.get("SDBZ_NATIVE_ALU_CERTIFICATES") == "1":
            raise ValueError("disable native ALU certificates for this experiment")
        receipt = stage(args.ir, args.objects, args.shadow, args.output)
        print(f"EE dedup: {len(receipt['removed'])} modules excluded; "
              f"{receipt['excluded_native_executable_bytes']:,} executable object bytes",
              flush=True)
        if args.link:
            library = args.output / ("game-translated.dll" if os.name == "nt" else
                                     "libgame-translated.dylib" if sys.platform == "darwin" else
                                     "libgame-translated.so")
            subprocess.run([sys.executable, str(Path(__file__).with_name("link_native_objects.py")),
                            str(args.output / "ee_functions.json"),
                            str(args.output / "objects"), str(library)], check=True)
            receipt["linked_library_bytes"] = library.stat().st_size
            receipt["linked_library_sha256"] = sha(library.read_bytes())
            receipt["linked_executable_raw_bytes"] = native_executable_sections(
                library.read_bytes())[0]
            (args.output / "dedup-plan.json").write_text(
                json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"EE dedup refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
