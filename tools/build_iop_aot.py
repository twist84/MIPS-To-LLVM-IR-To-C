#!/usr/bin/env python3
"""Build one ahead-of-time library per IOP IRX module from a runner capture.

1. Capture: run the game once with SDBZ_IOP_AOT_CAPTURE=CAPTURE_DIR. Every
   loaded IRX writes NAME@BASE.bin (relocated, linked .text) and .json.
2. This script: iop_recomp (C++) translates each module to LLVM IR
   (NAME@BASE.ll + NAME@BASE.table.ll), clang compiles the IR, and lld links
   OUT/modules/IRX_NAME.dll (.so elsewhere) -- one library per module.
   Multiple captured variants of one name receive collision suffixes.
   Modules are built in parallel.
3. Run the game with SDBZ_IOP_AOT=OUT/modules. The runtime loads every
   library in the directory; each module activates only when its bytes in
   IOP RAM match the capture, otherwise it is interpreted.

usage: build_iop_aot.py --build-dir build --capture DIR --out DIR [--opt 2]
"""

import argparse
import concurrent.futures
import collections
import json
import hashlib
import re
import struct
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from audit_iop_function_discovery import inventory


def safe_module_name(name):
    name = re.sub(r"[^A-Za-z0-9_]", "_", name) or "module"
    # Keep filenames portable across case-sensitive and Windows hosts.
    if re.fullmatch(r"CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9]", name, re.I):
        name = "_" + name
    return name


def recover_export_library(text, metadata):
    """Recover the first validated library from an older address-only capture.

    Only accept a linked table whose nonzero entries are present in the
    loader's recorded exports. Do not treat arbitrary marker bytes as names.
    """
    exports = metadata.get("exports", [])
    if not isinstance(exports, list) or not all(
            isinstance(entry, int) and 0 < entry <= 0xffffffff
            for entry in exports):
        return ""
    known = set(exports)
    if not known:
        return ""
    for offset in range(0, len(text) - 23, 4):
        if text[offset:offset + 4] != b"\x00\x00\xc0\x41":
            continue
        raw = text[offset + 12:offset + 20].split(b"\x00", 1)[0]
        if not raw or any(ch < 0x21 or ch > 0x7e for ch in raw):
            continue
        entries = []
        for ordinal in range(1024):
            pos = offset + 20 + ordinal * 4
            if pos + 4 > len(text):
                break
            entry = struct.unpack_from("<I", text, pos)[0]
            if entry == 0:
                following = (struct.unpack_from("<I", text, pos + 4)[0]
                             if pos + 8 <= len(text) else 0)
                if following in known:
                    continue  # Same single-hole rule as the IRX loader.
                if entries:
                    return raw.decode("ascii")
                break
            if entry not in known or entry & 3:
                break
            entries.append(entry)
    return ""


def prepare_captures(capture, destination):
    """Stage named captures without modifying original linked bytes/metadata."""
    records = []
    for path in sorted(capture.glob("*.json")):
        metadata = json.loads(path.read_text(encoding="utf-8"))
        binary = path.with_suffix(".bin")
        text = binary.read_bytes()
        export = metadata.get("export_library")
        if not isinstance(export, str) or not export:
            export = recover_export_library(text, metadata)
        # Older captures did not record the LOADFILE filename. These two
        # exact internal titles correspond to the observed SDBZ disc IRXs.
        # This is a naming fallback only, never an activation identity.
        legacy_names = {
            "CRI_ADX_Driver": "CRI_ADXI",
            "ARIKA_IOP_Control_Driver": "ARKD_DVD",
        }
        source = metadata.get("source_name")
        if not isinstance(source, str):
            source = ""
        name = safe_module_name(source or legacy_names.get(metadata["name"])
                                or export or metadata["name"])
        base = metadata["base"]
        digest = metadata["hash"]
        if (not isinstance(base, int) or not 0 <= base <= 0xffffffff or
                not isinstance(digest, str) or
                not re.fullmatch(r"[0-9a-fA-F]{1,16}", digest)):
            raise ValueError(f"invalid IOP capture identity: {path}")
        records.append((path, binary, metadata, name, base, digest.lower()))

    counts = collections.Counter(row[3].casefold() for row in records)
    reserved = {row[3].casefold() for row in records}
    used = set()
    destination.mkdir(parents=True, exist_ok=True)
    for path, binary, metadata, name, base, digest in records:
        stem = name
        if counts[name.casefold()] > 1:
            stem = f"{name}@{base:08x}"
            if stem.casefold() in reserved or stem.casefold() in used:
                stem = f"{stem}-{digest}"
            candidate = stem
            serial = 2
            while stem.casefold() in reserved or stem.casefold() in used:
                stem = f"{candidate}-{serial}"
                serial += 1
        used.add(stem.casefold())
        # The exported descriptor receives the logical name, not the
        # collision suffix. Base, hash and executable text stay unchanged.
        metadata["name"] = name
        shutil.copyfile(binary, destination / f"{stem}.bin")
        (destination / f"{stem}.json").write_text(
            json.dumps(metadata) + "\n", encoding="utf-8")
    return len(records)


def relocation_receipt_match(receipt, metadata, data):
    """Select the single original ELF record for a linked capture identity."""
    if receipt.get("schema") != 1 or receipt.get("kind") != "iop_original_irx_metadata":
        raise ValueError("invalid relocation receipt schema")
    identity = {"base": metadata["base"], "text_size": len(data),
                "hash": metadata["hash"].lower()}
    matches = []
    for records in receipt["matches"].values():
        if len(records) != 1:
            continue
        record = records[0]
        shadow = record.get("relocation_discovery")
        if shadow and shadow["capture_identity"] == identity:
            matches.append(record)
    if len(matches) != 1:
        raise ValueError("relocation receipt needs one unique capture identity")
    return matches[0]


def relocation_receipt_roots(receipt, metadata, data, baseline):
    """Recheck linked JALs; never consume saved ownership lists."""
    record = relocation_receipt_match(receipt, metadata, data)
    shadow = record["relocation_discovery"]
    if (shadow["previous_candidate_count"] != baseline["candidate_count"] or
            shadow["previous_unvisited_words"] != baseline["unvisited_aligned_words"]):
        raise ValueError("relocation receipt baseline differs from current discovery")
    base = metadata["base"]
    reached = {pc for region in baseline["regions"]
               for pc in region["ordinary_pcs"] + region["delay_pcs"]}
    executable = [(base + section["address"],
                   base + section["address"] + section["size"])
                  for section in record["metadata"]["sections"]
                  if section["flags"] & 4]
    verified = {(call["site"], call["target"])
                for call in record["comparison"]["verified_relocated_calls"]}

    def valid_pc(pc):
        return (type(pc) is int and pc % 4 == 0 and
                base <= pc <= base + len(data) - 4 and
                any(lo <= pc and pc + 4 <= hi for lo, hi in executable))

    roots = set()
    for row in shadow["new_roots"]:
        target = row["entry"]
        if not valid_pc(target) or target in reached or target in roots or not row["call_sites"]:
            raise ValueError("invalid relocation root")
        for site in row["call_sites"]:
            if not valid_pc(site) or (site, target) not in verified:
                raise ValueError("missing relocation call evidence")
            word = struct.unpack_from("<I", data, site - base)[0]
            resolved = (((site + 4) & 0xf0000000) | ((word & 0x03ffffff) << 2))
            if word >> 26 != 3 or resolved != target:
                raise ValueError("linked JAL differs from relocation receipt")
        roots.add(target)
    return sorted(roots)


def table_backed_inventory(metadata, data, baseline, receipt, roots, disc_root,
                           cross_block=False):
    """Reconstruct candidate edges from the original ELF and linked bytes.

    Tables are not assumed immutable or exhaustive. Only discovery ownership
    changes; the residual and dynamic indirect dispatch domains remain intact.
    """
    from audit_iop_indirect_targets import constant_indirect_targets
    from audit_iop_irx_metadata import (parse_irx, capture_mismatches,
                                        compare_capture, discover_relocation_roots)
    from audit_iop_jump_tables import (table_candidates, cap_adjacent_tables,
                                       discover_table_edges)

    record = relocation_receipt_match(receipt, metadata, data)
    disc = disc_root.resolve()
    elf = (disc / record["path"]).resolve()
    if not elf.is_relative_to(disc):
        raise ValueError("ELF path escapes disc root")
    raw = elf.read_bytes()
    if hashlib.sha256(raw).hexdigest() != record["elf_sha256"]:
        raise ValueError(f"original ELF changed: {elf}")
    parsed, original = parse_irx(raw)
    # Staging intentionally canonicalizes the descriptor name. Recover the
    # original captured name from the identity-matched receipt for ELF checks.
    original_metadata = dict(metadata, name=record["comparison"]["captured_name"])
    if capture_mismatches(original_metadata, data, parsed):
        raise ValueError(f"original ELF metadata differs: {elf}")
    initial = inventory(metadata, data, extended=True)
    comparison = compare_capture(original_metadata, data, parsed, original, initial)
    verified = discover_relocation_roots(metadata, data, parsed, comparison, initial)
    if sorted(row["entry"] for row in verified["new_roots"]) != roots:
        raise ValueError("original ELF relocation roots differ from receipt")
    indirect = constant_indirect_targets(metadata, data, baseline, audit_producers=True,
                                         cross_block=cross_block)
    tables = table_candidates(metadata, data, parsed, original, baseline, indirect)
    if cross_block:
        tables = cap_adjacent_tables(tables)
        for table in tables:
            if "uncapped_entry_count" in table:
                print(f"split table candidate cap: JR {table['site']:08x}, "
                      f"{table['uncapped_entry_count']} -> {len(table['entries'])} slots")
    shadow = discover_table_edges(metadata, data, baseline, roots, tables)
    expanded = shadow["inventory"]
    print(f"split tables: {len(shadow['candidate_edges'])} candidate edges, "
          f"{len(shadow['newly_reached_pcs'])} newly reached words, "
          f"{len(shadow['new_function_entries'])} new roots")
    return expanded


def prepare_split_pilot(staged, selected, pilot, residual=False, extended=False,
                        relocation_receipt=None, table_disc_root=None,
                        cross_block_tables=False, residual_max_pcs=None):
    """Select one canonical capture, re-audit bytes, reject ambiguous owners."""
    if residual_max_pcs is not None and (not residual or
            type(residual_max_pcs) is not int or residual_max_pcs < 1):
        raise ValueError("residual PC limit must be positive and requires residual coverage")
    candidates = [path for path in staged.glob("*.json")
                  if path.stem.casefold() == selected.casefold()]
    if len(candidates) != 1:
        raise ValueError(f"split pilot must select one exact staged IRX stem: {selected}")
    meta = candidates[0]
    binary = meta.with_suffix(".bin")
    metadata = json.loads(meta.read_text(encoding="utf-8"))
    data = binary.read_bytes()
    if extended and not residual:
        raise ValueError("extended discovery requires residual dispatch coverage")
    report = inventory(metadata, data, extended=extended)
    if cross_block_tables and table_disc_root is None:
        raise ValueError("cross-block tables require original ELF table discovery")
    if table_disc_root is not None and relocation_receipt is None:
        raise ValueError("table discovery requires relocation evidence")
    if relocation_receipt is not None:
        if not (extended and residual):
            raise ValueError("relocation discovery requires extended residual coverage")
        roots = relocation_receipt_roots(relocation_receipt, metadata, data, report)
        report = inventory(metadata, data, extended=True, relocated_call_entries=roots)
        print(f"split relocations: {len(roots)} verified candidate roots")
        if table_disc_root is not None:
            if (report["shared_ordinary_pcs"] or report["ordinary_delay_overlap"]
                    or report["rejected_seeds"]):
                raise ValueError("ambiguous baseline ownership")
            report = table_backed_inventory(metadata, data, report, relocation_receipt,
                                            roots, table_disc_root,
                                            cross_block=cross_block_tables)
    if (report["shared_ordinary_pcs"] or report["ordinary_delay_overlap"] or
            report["rejected_seeds"] or len(data) % 4):
        raise ValueError("split pilot refuses ambiguous or invalid ownership")
    # A conservative classifier stop is not permission to execute that
    # encoding natively. Keep unsupported boundaries on the interpreter.
    rejected = {item["pc"] for item in report["unresolved"]
                if item["kind"] in ("unsupported", "unsupported_delay", "missing_delay")}
    if extended:
        # The audit can follow interpreter-only operations to discover
        # subsequent code. Do not assign those operations to functions.
        rejected.update(report["extended"]["interpreter_linear_pcs"])
    regions = [[pc for pc in region["ordinary_pcs"] if pc not in rejected]
               for region in report["regions"]]
    regions = [region for region in regions if region]
    if not regions:
        raise ValueError("split pilot has no eligible regions")
    discovered_count = sum(map(len, regions))
    if extended:
        print(f"split extended: {report['candidate_count']} candidates, "
              f"{len(regions)} eligible function regions, "
              f"{discovered_count} eligible ordinary PCs")
    residual_count = 0
    if residual:
        # Preserve the legacy lifter's dispatch domain, not a claim that
        # unvisited words are code. The same C++ decoder will decide whether
        # an actual entry executes natively or requests interpreter fallback.
        owned = {pc for region in regions for pc in region}
        remaining = [pc for pc in range(report["base"],
                                         report["base"] + len(data), 4)
                     if pc not in owned]
        residual_count = len(remaining)
        if remaining:
            limit = residual_max_pcs or residual_count
            # These are bounded dispatch partitions, not discovered functions.
            # Preserve the original ascending PC order and all existing
            # function region IDs. Delay-slot emission remains in the lifter.
            regions.extend(remaining[i:i + limit]
                           for i in range(0, residual_count, limit))
        if discovered_count + residual_count != len(data) // 4:
            raise ValueError("split residual coverage is not an exact partition")
        flat = [pc for region in regions for pc in region]
        if sorted(flat) != list(range(report["base"], report["base"] + len(data), 4)):
            raise ValueError("split residual contains duplicate or missing dispatch PCs")
    pilot.mkdir()
    shutil.copyfile(meta, pilot / meta.name)
    shutil.copyfile(binary, pilot / binary.name)
    plan = pilot / "ownership.regions"
    lines = [f"IOPREG1 {report['base']} {report['text_size']} "
             f"{int(report['hash'], 16)} {len(regions)}"]
    lines.extend(" ".join(map(str, [len(region), *region])) for region in regions)
    plan.write_text("\n".join(lines) + "\n", encoding="ascii")
    print(f"split pilot: {meta.stem}, {len(regions)} regions, "
          f"{sum(map(len, regions))} ordinary PCs")
    if residual:
        print(f"split residual: {discovered_count} discovered PCs, "
              f"{residual_count} residual PCs, "
              f"{len(data) // 4} aligned dispatch PCs")
        if residual_max_pcs is not None:
            count = (residual_count + residual_max_pcs - 1) // residual_max_pcs
            print(f"split residual partitions: {count}, "
                  f"at most {residual_max_pcs} PCs each (not functions)")
    return plan


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def compact_split_router(router, plan, narrow_ids=False):
    """Replace the generated PC switch with a checked dense region map.

    Verify the old switch against the already validated ownership plan.
    Keep all region calls, context writes and budget/fallback paths intact.
    The map is dispatch ownership, not a guest jump-target prediction.
    """
    rows = plan.splitlines()
    header = rows[0].split() if rows else []
    if len(header) != 5 or header[0] != "IOPREG1":
        raise ValueError("invalid compact router plan header")
    base, size, _, count = map(int, header[1:])
    if (not 0 <= base <= 0xffffffff or base % 4 or size <= 0 or size % 4 or
            base + size > 0xffffffff or not 1 <= count <= 65535 or
            count > size // 4 or len(rows) != count + 1):
        raise ValueError("invalid compact router plan dimensions")
    ids = [-1] * (size // 4)
    cases = []
    for region, row in enumerate(rows[1:]):
        values = list(map(int, row.split()))
        if (not values or values[0] < 1 or values[0] != len(values) - 1 or
                values[1:] != sorted(values[1:])):
            raise ValueError("invalid compact router region")
        for pc in values[1:]:
            if not base <= pc < base + size or pc % 4:
                raise ValueError("invalid compact router PC")
            index = (pc - base) // 4
            if ids[index] != -1:
                raise ValueError("duplicate compact router PC")
            ids[index] = region
            signed_pc = pc if pc < 0x80000000 else pc - 0x100000000
            cases.append(f"    i32 {signed_pc}, label %region_{region}\n")
    old = ("dispatch:\n  switch i32 %start, label %fallback [\n"
           + "".join(cases) + "  ]\n")
    if router.count(old) != 1 or "@iop_region_ids" in router:
        raise ValueError("generated router does not match validated ownership plan")

    width = len(ids)
    # Reserve the all-ones sentinel for unowned PCs. 255 valid regions
    # (IDs 0..254) fit in a byte; 256 require the original i16 map.
    id_type = "i8" if narrow_ids and count <= 255 else "i16"
    alignment = 1 if id_type == "i8" else 2
    table_type = f"[{width} x {id_type}]"
    table = (f"@iop_region_ids = private unnamed_addr constant {table_type} [\n"
             + ",\n".join("  " + ", ".join(f"{id_type} {value}" for value in ids[i:i + 16])
                            for i in range(0, width, 16))
             + f"\n], align {alignment}\n\n")
    signed_base = base if base < 0x80000000 else base - 0x100000000
    replacement = (
        "dispatch:\n"
        f"  %map_offset = sub i32 %start, {signed_base}\n"
        f"  %map_in_range = icmp ult i32 %map_offset, {size}\n"
        "  %map_low = and i32 %map_offset, 3\n"
        "  %map_aligned = icmp eq i32 %map_low, 0\n"
        "  %map_valid = and i1 %map_in_range, %map_aligned\n"
        "  br i1 %map_valid, label %map_lookup, label %fallback\n"
        "map_lookup:\n"
        "  %map_word = lshr i32 %map_offset, 2\n"
        "  %map_index = zext i32 %map_word to i64\n"
        f"  %map_ptr = getelementptr inbounds {table_type}, ptr @iop_region_ids, "
        "i64 0, i64 %map_index\n"
        f"  %map_id = load {id_type}, ptr %map_ptr, align {alignment}\n"
        f"  switch {id_type} %map_id, label %fallback [\n"
        + "".join(f"    {id_type} {region}, label %region_{region}\n"
                  for region in range(count))
        + "  ]\n")
    return table + router.replace(old, replacement, 1)


def split_cache_key(source, provenance, reuse_ir=False):
    payload = {"schema": 1, "ir_sha256": sha256_file(source),
               "provenance": provenance}
    if reuse_ir:
        # The entire plan is validated before lifting. For the object cache
        # only, identical IR and compiler inputs can survive a plan change.
        # Retain capture, staged metadata, lifter and compiler identities.
        if not isinstance(provenance, dict) or "plan_sha256" not in provenance:
            raise ValueError("IR cache reuse requires plan provenance")
        payload = {"schema": 2, "ir_sha256": payload["ir_sha256"],
                   "source_name": source.name,
                   "provenance": {key: value for key, value in provenance.items()
                                  if key != "plan_sha256"}}
    return hashlib.sha256(json.dumps(payload, sort_keys=True,
                                    separators=(",", ":")).encode("utf-8")).hexdigest()


def cached_split_object(source, obj, cache, provenance, compile_object,
                        reuse_ir=False):
    """Validate immutable entries and optionally migrate the current plan's hit."""
    if cache is None:
        compile_object(source, obj)
        return False
    key = split_cache_key(source, provenance, reuse_ir)
    entry = cache / key

    def valid_entry(path, expected_key):
        try:
            meta = json.loads((path / "receipt.json").read_text(encoding="utf-8"))
            binary = path / "object.o"
            return (meta["key"] == expected_key and binary.stat().st_size > 0 and
                    meta["size"] == binary.stat().st_size and
                    meta["sha256"] == sha256_file(binary))
        except (OSError, ValueError, KeyError, TypeError):
            return False

    if entry.exists():
        if not valid_entry(entry, key):
            raise ValueError(f"invalid split object cache entry: {entry}")
        shutil.copyfile(entry / "object.o", obj)
        return True

    # Promote only the exact legacy key for this validated plan. Never
    # scan or trust arbitrary old cache receipts from unrelated plans.
    hit = False
    if reuse_ir:
        legacy_key = split_cache_key(source, provenance)
        legacy = cache / legacy_key
        if legacy.exists():
            if not valid_entry(legacy, legacy_key):
                raise ValueError(f"invalid split object cache entry: {legacy}")
            shutil.copyfile(legacy / "object.o", obj)
            hit = True
    if not hit:
        compile_object(source, obj)
    if obj.stat().st_size == 0:
        raise ValueError("empty split object")
    # Publish receipt and object atomically; keep legacy entries unchanged.
    with tempfile.TemporaryDirectory(prefix=".pending-", dir=cache) as tmp:
        pending = Path(tmp) / "entry"
        pending.mkdir()
        shutil.copyfile(obj, pending / "object.o")
        (pending / "receipt.json").write_text(json.dumps({
            "key": key, "size": obj.stat().st_size, "sha256": sha256_file(obj)
        }) + "\n", encoding="utf-8")
        try:
            pending.rename(entry)
        except OSError:
            if not valid_entry(entry, key):
                raise
    return hit


def select_split_stems(staged, selected, all_modules=False):
    available = {path.stem.casefold(): path.stem
                 for path in staged.glob("*.json")}
    if all_modules:
        if not available:
            raise ValueError("no staged IRX captures")
        return sorted(available.values())
    stems = []
    for name in selected:
        stem = available.get(name.casefold())
        if stem is None:
            raise ValueError(f"split pilot must select one exact staged IRX stem: {name}")
        if stem in stems:
            raise ValueError(f"duplicate split module: {name}")
        stems.append(stem)
    return sorted(stems)


def split_compile_schedule(sources_by_stem):
    """Prioritize large IR as a cost proxy, independent of linker order."""
    return sorted(((stem, source) for stem, sources in sources_by_stem.items()
                   for source in sources),
                  key=lambda job: (-job[1].stat().st_size, job[0], job[1].name))


def compile_split_modules(args, clang, ir_dir, obj_dir, lib_dir, stems, suffix,
                          provenance_by_stem):
    sources_by_stem = {}
    for stem in stems:
        sources = sorted(ir_dir.glob(f"{stem}.region.*.ll"))
        if not sources:
            raise ValueError(f"split pilot emitted no region IR: {stem}")
        sources += [ir_dir / f"{stem}.ll", ir_dir / f"{stem}.table.ll"]
        sources_by_stem[stem] = sources

    jobs = split_compile_schedule(sources_by_stem)
    started = time.monotonic()

    def compile_object(source, obj):
        command = [clang, f"-O{args.opt}", "-c", "-Wno-override-module"]
        if args.region_cache is not None:
            command += ["--no-default-config"]
        subprocess.run([*command, str(source), "-o", str(obj)], check=True)

    def compile_one(stem, source):
        obj = obj_dir / (source.stem + ".o")
        start = time.monotonic()
        hit = cached_split_object(source, obj, args.region_cache,
                                  provenance_by_stem[stem], compile_object,
                                  reuse_ir=getattr(args, "region_cache_ir_key", False))
        finish = time.monotonic()
        return stem, obj, start, finish, {
            "source": source.name, "object": obj.name, "cached": hit,
            "seconds": finish - start,
            "ir_bytes": source.stat().st_size, "object_bytes": obj.stat().st_size,
        }

    completed = {stem: {} for stem in stems}
    count = 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futures = [pool.submit(compile_one, stem, source) for stem, source in jobs]
        for future in concurrent.futures.as_completed(futures):
            stem, obj, start, finish, observation = future.result()
            completed[stem][observation["source"]] = (obj, observation, start, finish)
            count += 1
            print(f"IOP objects [{count}/{len(jobs)}]: {obj.name}"
                  + (" (cached)" if observation["cached"] else ""))

    compiled = time.monotonic()
    libraries = []
    receipts = []
    multi = len(stems) > 1
    if multi:
        receipt_dir = lib_dir.parent / "split-receipts"
        receipt_dir.mkdir()
    for stem in stems:
        # Preserve the original per-module linker ordering, not queue order.
        rows = [completed[stem][source.name] for source in sources_by_stem[stem]]
        objects = [row[0] for row in rows]
        observations = [row[1] for row in rows]
        hits = sum(int(row["cached"]) for row in observations)
        library = lib_dir / f"{stem}{suffix}"
        command = [clang, "-shared", "-fuse-ld=lld", "-o", str(library),
                   *map(str, objects)]
        if os.name == "nt":
            command += ["-nostdlib", "-Wl,/noentry", "-Wl,/timestamp:0"]
        link_started = time.monotonic()
        subprocess.run(command, check=True)
        linked = time.monotonic()
        object_bytes = sum(obj.stat().st_size for obj in objects)
        library_bytes = library.stat().st_size
        compile_seconds = (max(row[3] for row in rows) - min(row[2] for row in rows)
                           if multi else compiled - started)
        print(f"{stem}: split cache: {hits} reused, {len(objects) - hits} compiled")
        print(f"split compile {compile_seconds:.3f}s, "
              f"link {linked - link_started:.3f}s, "
              f"{object_bytes} object bytes, {library_bytes} library bytes")
        slowest = sorted(observations, key=lambda row: (-row["seconds"], row["source"]))
        for row in slowest[:5]:
            print(f"split slow object: {row['source']} {row['seconds']:.3f}s, "
                  f"{row['ir_bytes']} IR bytes, {row['object_bytes']} object bytes"
                  + (" (cached)" if row["cached"] else ""))
        receipt = {
            "schema": 1, "module": stem, "opt": args.opt, "jobs": args.jobs,
            "router": ("compact-narrow" if getattr(args, "split_narrow_router", False)
                       else "compact" if getattr(args, "split_compact_router", False)
                       else "switch"),
            "compile_seconds": compile_seconds, "link_seconds": linked - link_started,
            "cache_hits": hits, "object_bytes": object_bytes,
            "library_bytes": library_bytes, "objects": observations,
            "library_sha256": sha256_file(library),
        }
        receipt_path = (receipt_dir / f"{stem}.json" if multi else
                        lib_dir.parent / "split-build-receipt.json")
        with receipt_path.open("x", encoding="utf-8") as stream:
            json.dump(receipt, stream, indent=2)
            stream.write("\n")
        receipts.append(receipt)
        libraries.append(library)
    if multi:
        aggregate = {
            "schema": 1, "kind": "iop_split_multi_build",
            "opt": args.opt, "jobs": args.jobs,
            "compile_seconds": compiled - started,
            "link_seconds": sum(row["link_seconds"] for row in receipts),
            "cache_hits": sum(row["cache_hits"] for row in receipts),
            "object_count": len(jobs),
            "object_bytes": sum(row["object_bytes"] for row in receipts),
            "library_bytes": sum(row["library_bytes"] for row in receipts),
            "modules": [{**{key: row[key] for key in
                            ("module", "cache_hits", "object_bytes", "library_bytes",
                             "library_sha256")},
                         "object_count": len(row["objects"]),
                         "receipt": f"split-receipts/{row['module']}.json"}
                        for row in receipts],
        }
        with (lib_dir.parent / "split-build-receipt.json").open("x", encoding="utf-8") as stream:
            json.dump(aggregate, stream, indent=2)
            stream.write("\n")
    return libraries


def compile_split_pilot(args, clang, ir_dir, obj_dir, lib_dir, stem, suffix,
                        provenance=None):
    return compile_split_modules(args, clang, ir_dir, obj_dir, lib_dir,
                                 [stem], suffix, {stem: provenance})[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--opt", default="2", choices=["0", "1", "2", "3"])
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--bootstrap", action="store_true",
                        help="Build missing libraries without clearing existing output files")
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--split-module", action="append",
                           help="Compile an exact staged IRX stem in regions (repeatable)")
    selection.add_argument("--split-all", action="store_true",
                           help="Compile all staged IRXs in regions")
    parser.add_argument("--split-narrow-router", action="store_true",
                        help="Use i8 region IDs when compact dispatch has at most 255 regions")
    parser.add_argument("--split-compact-router", action="store_true",
                        help="Experimental: checked dense region-ID map for split dispatch")
    parser.add_argument("--split-residual-max-pcs", type=int,
                        help="Experimental: bound residual dispatch partitions, not function regions")
    parser.add_argument("--split-residual", action="store_true",
                        help="Add one residual region covering every remaining aligned PC")
    parser.add_argument("--split-extended", action="store_true",
                        help="Experimental extended discovery; requires --split-residual")
    parser.add_argument("--split-relocation-receipt", type=Path,
                        help="Use unique relocation evidence from an original IRX audit")
    parser.add_argument("--split-cross-block-tables", action="store_true",
                        help="Experimental cross-block table ownership with adjacent candidate caps")
    parser.add_argument("--split-table-disc-root", type=Path,
                        help="Experimental: rederive non-exhaustive table edges from original IRXs")
    parser.add_argument("--region-cache-ir-key", action="store_true",
                        help="Opt-in exact-IR cache reuse across validated ownership plans")
    parser.add_argument("--region-cache", type=Path,
                        help="Opt-in content-addressed split object cache outside inputs/output")
    args = parser.parse_args()
    split_mode = args.split_all or bool(args.split_module)
    if args.split_narrow_router and not args.split_compact_router:
        parser.error("--split-narrow-router requires --split-compact-router")
    if args.split_compact_router and not split_mode:
        parser.error("--split-compact-router requires --split-module or --split-all")
    if args.split_cross_block_tables and args.split_table_disc_root is None:
        parser.error("--split-cross-block-tables requires --split-table-disc-root")
    if args.split_table_disc_root is not None and args.split_relocation_receipt is None:
        parser.error("--split-table-disc-root requires --split-relocation-receipt")
    if args.split_relocation_receipt is not None and not (
            split_mode and args.split_extended and args.split_residual):
        parser.error("--split-relocation-receipt requires extended residual splitting")
    if args.split_extended and not (split_mode and args.split_residual):
        parser.error("--split-extended requires --split-module or --split-all and --split-residual")
    if args.split_residual_max_pcs is not None and (
            not args.split_residual or args.split_residual_max_pcs < 1):
        parser.error("--split-residual-max-pcs requires --split-residual and a positive limit")
    if args.split_residual and not split_mode:
        parser.error("--split-residual requires --split-module or --split-all")
    if args.region_cache_ir_key and args.region_cache is None:
        parser.error("--region-cache-ir-key requires --region-cache")
    if args.region_cache is not None:
        if not split_mode:
            parser.error("--region-cache requires --split-module or --split-all")
        args.region_cache = args.region_cache.resolve()
        for protected in (args.capture.resolve(), args.out.resolve()):
            if (args.region_cache.is_relative_to(protected) or
                    protected.is_relative_to(args.region_cache)):
                parser.error("region cache must be separate from captures and output")
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if split_mode and (args.bootstrap or args.out.exists()):
        parser.error("split pilot requires a fresh output directory, without --bootstrap")

    if args.bootstrap:
        # Generate into a private directory, then publish only completed
        # artifacts. Never delete unrelated files in the user's destination.
        args.out = args.out.resolve()
        args.out.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="iop-aot-", dir=args.out.parent) as temporary:
            staging = Path(temporary)
            build(args, staging)
            for subdir in ("ir", "obj", "modules"):
                destination = args.out / subdir
                destination.mkdir(parents=True, exist_ok=True)
                for source in (staging / subdir).iterdir():
                    source.replace(destination / source.name)
        print(f"build_iop_aot: libraries published to {args.out / 'modules'}")
        return 0
    return build(args, args.out)


def build(args, out):
    exe = ".exe" if os.name == "nt" else ""
    recomp = args.build_dir / f"iop_recomp{exe}"
    if not recomp.is_file():
        recomp = args.build_dir / "Release" / f"iop_recomp{exe}"
    clang = shutil.which("clang")
    if clang is None and os.name == "nt":
        candidate = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "LLVM" / "bin" / "clang.exe"
        if candidate.is_file():
            clang = str(candidate)
    if clang is None:
        sys.exit("build_iop_aot: clang not found on PATH")
    if not recomp.is_file():
        sys.exit("build_iop_aot: iop_recomp has not been built")

    ir_dir = out / "ir"
    obj_dir = out / "obj"
    lib_dir = out / "modules"
    for folder in (ir_dir, obj_dir, lib_dir):
        folder.mkdir(parents=True, exist_ok=True)
        for stale in folder.iterdir():
            if stale.is_file():
                stale.unlink()
    started = time.monotonic()
    # The C++ lifter names its output from the capture stem and exports
    # the descriptor from metadata. Stage canonical names for both.
    provenance_by_stem = {}
    split_mode = args.split_all or bool(args.split_module)
    with tempfile.TemporaryDirectory(prefix="iop-named-captures-") as temporary:
        staged = Path(temporary) / "staged"
        prepare_captures(args.capture, staged)
        if split_mode:
            selected = select_split_stems(staged, args.split_module, args.split_all)
            relocation_receipt = (json.loads(args.split_relocation_receipt.read_text(encoding="utf-8"))
                                  if args.split_relocation_receipt is not None else None)
            common_provenance = None
            if args.region_cache is not None:
                args.region_cache.mkdir(parents=True, exist_ok=True)
                common_provenance = {
                    "lifter_sha256": sha256_file(recomp),
                    "compiler_sha256": sha256_file(clang),
                    "compiler_version": subprocess.check_output(
                        [clang, "--no-default-config", "--version"], text=True),
                    "target": subprocess.check_output(
                        [clang, "--no-default-config", "-dumpmachine"], text=True).strip(),
                    "options": [f"-O{args.opt}", "-c", "-Wno-override-module",
                                "--no-default-config"],
                    "platform": os.name,
                }
            for index, stem in enumerate(selected):
                pilot = Path(temporary) / f"pilot-{index}"
                plan = prepare_split_pilot(staged, stem, pilot,
                                           residual=args.split_residual,
                                           extended=args.split_extended,
                                           relocation_receipt=relocation_receipt,
                                           table_disc_root=args.split_table_disc_root,
                                           cross_block_tables=args.split_cross_block_tables,
                                           residual_max_pcs=args.split_residual_max_pcs)
                subprocess.run([str(recomp), str(pilot), str(ir_dir), str(plan)], check=True)
                if args.split_compact_router:
                    router_path = ir_dir / f"{stem}.ll"
                    router = router_path.read_text(encoding="utf-8")
                    compact = compact_split_router(router, plan.read_text(encoding="ascii"),
                                                   narrow_ids=args.split_narrow_router)
                    router_path.write_text(compact, encoding="utf-8")
                    print(f"{stem}: compact router {len(router.encode('utf-8'))} -> "
                          f"{len(compact.encode('utf-8'))} IR bytes")
                provenance_by_stem[stem] = (
                    {**common_provenance,
                     "plan_sha256": sha256_file(plan),
                     "capture_sha256": sha256_file(next(pilot.glob("*.bin"))),
                     "metadata_sha256": sha256_file(next(pilot.glob("*.json")))}
                    if common_provenance is not None else None)
        else:
            subprocess.run([str(recomp), str(staged), str(ir_dir)], check=True)
    stems = sorted(p.name[:-len(".table.ll")] for p in ir_dir.glob("*.table.ll"))
    if not stems:
        sys.exit("build_iop_aot: no translated IRX modules")
    suffix = ".dll" if os.name == "nt" else ".so"

    if split_mode:
        if stems != selected:
            raise ValueError("split pilot emitted unexpected module tables")
        libraries = compile_split_modules(args, clang, ir_dir, obj_dir, lib_dir,
                                          stems, suffix, provenance_by_stem)
        for library in libraries:
            print(f"build_iop_aot: {library}")
        print(f"build_iop_aot: {len(libraries)} split module libraries "
              f"({time.monotonic() - started:.1f}s total)")
        return 0

    def build_one(stem):
        objects = []
        for source in (ir_dir / f"{stem}.ll", ir_dir / f"{stem}.table.ll"):
            obj = obj_dir / (source.name[:-len(".ll")] + ".o")
            subprocess.run([clang, f"-O{args.opt}", "-c", "-Wno-override-module",
                            str(source), "-o", str(obj)], check=True)
            objects.append(str(obj))
        library = lib_dir / f"{stem}{suffix}"
        command = [clang, "-shared", "-fuse-ld=lld", "-o", str(library), *objects]
        if os.name == "nt":
            command += ["-nostdlib", "-Wl,/noentry", "-Wl,/timestamp:0"]
        subprocess.run(command, check=True)
        return library

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        libraries = list(pool.map(build_one, stems))
    for library in libraries:
        print(f"build_iop_aot: {library}")
    print(f"build_iop_aot: {len(libraries)} module librar"
          f"{'y' if len(libraries) == 1 else 'ies'} in {lib_dir} "
          f"({time.monotonic() - started:.1f}s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
