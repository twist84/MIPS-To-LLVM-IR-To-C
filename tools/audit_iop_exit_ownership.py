#!/usr/bin/env python3
"""I17e shadow attribution of I17d native exits to retained split routers.

Read-only: no guest execution, recompilation, or cache mutation. The original
build receipts bind DLL bytes, not their retained textual IR; this is an
advisory measurement, not a certificate authorizing native chaining.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import zipfile


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_router(router, table):
    descriptor = re.findall(
        r"\{ptr @name0, i32 (-?\d+), i32 (\d+), i64 (-?\d+), ptr @iop_aot_fn\}",
        table)
    require(len(descriptor) == 1, "expected one module descriptor")
    base, size, code_hash = map(int, descriptor[0])
    base &= 0xffffffff
    code_hash &= 0xffffffffffffffff
    require(size > 0 and size % 4 == 0 and base % 4 == 0
            and base + size <= 0x100000000, "invalid module range")
    match = re.search(
        r"@iop_region_ids = private unnamed_addr constant \[(\d+) x (i8|i16)\] "
        r"\[(.*?)\], align (\d+)", router, re.S)
    require(match is not None, "expected compact split router")
    words, width, values, alignment = match.groups()
    tokens = values.strip().split(",")
    ids = []
    for token in tokens:
        item = re.fullmatch(r"\s*" + width + r" (-?\d+)\s*", token)
        require(item is not None, "invalid compact router value")
        ids.append(int(item.group(1)))
    require(len(ids) == int(words) == size // 4, "router dimensions mismatch")
    require(int(alignment) == (1 if width == "i8" else 2),
            "router alignment mismatch")
    signed_base = base if base < 0x80000000 else base - 0x100000000
    require(f"%map_offset = sub i32 %start, {signed_base}\n" in router
            and f"%map_in_range = icmp ult i32 %map_offset, {size}\n" in router,
            "router range mismatch")
    cases = re.findall(r"^\s+" + width + r" (\d+), label %region_(\d+)\s*$",
                       router, re.M)
    require(cases and all(a == b for a, b in cases), "invalid region switch")
    case_ids = [int(a) for a, _ in cases]
    limit = 255 if width == "i8" else 65535
    require(case_ids == list(range(len(case_ids))) and len(case_ids) <= limit,
            "invalid region ID domain")
    require(set(ids) <= (set(case_ids) | {-1}) and set(case_ids) <= set(ids),
            "unknown or empty region")
    return (base, code_hash), ids


def load_routers(roots, inventory):
    maps = {}
    seen = set()
    for root in roots:
        for filename, digest in inventory.items():
            library = root / "modules" / filename
            if not library.is_file():
                continue
            stem = Path(filename).stem
            require(filename not in seen, f"duplicate build root for {filename}")
            require(sha256(library) == digest, f"replay DLL mismatch: {library}")
            receipt_path = root / "split-receipts" / (stem + ".json")
            if not receipt_path.is_file():
                receipt_path = root / "split-build-receipt.json"
            receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
            require(receipt.get("module") == stem
                    and receipt.get("library_sha256") == digest,
                    f"build receipt mismatch: {stem}")
            router_path = root / "ir" / (stem + ".ll")
            table_path = root / "ir" / (stem + ".table.ll")
            key, ids = read_router(router_path.read_text(encoding="utf-8"),
                                   table_path.read_text(encoding="utf-8"))
            require(key not in maps, "duplicate module identity")
            maps[key] = (stem, ids, {
                "dll_sha256": digest,
                "router_sha256": sha256(router_path),
                "table_sha256": sha256(table_path),
                "receipt_sha256": sha256(receipt_path),
            })
            seen.add(filename)
    require(seen == set(inventory), "missing original split build roots")
    return maps


def owner(pc, base, ids):
    offset = pc - base
    if offset < 0 or offset % 4 or offset // 4 >= len(ids):
        return None
    region = ids[offset // 4]
    return region if region >= 0 else None


def analyze(profile, maps):
    require(profile["schema"] == 1 and profile["dropped"] == 0,
            "incomplete exit profile")
    rows = profile["rows"]
    require(len(rows) == profile["keys"], "profile key count mismatch")
    require(sum(row["entries"] for row in rows) == profile["entries"]
            and sum(row["retired"] for row in rows) == profile["retired"],
            "profile totals mismatch")
    seen = set()
    totals = Counter()
    per_module = defaultdict(Counter)
    edges = Counter()
    for row in rows:
        key = (row["base"], row["hash"])
        identity = tuple(row[k] for k in
                         ("base", "hash", "entry", "resume", "reason", "delay"))
        require(identity not in seen, "duplicate exit profile key")
        seen.add(identity)
        require(key in maps, f"unknown profile module {key}")
        name, ids, _ = maps[key]
        entries, retired, budget, horizon, zero = (
            row[k] for k in ("entries", "retired", "budget", "horizon", "zero"))
        require(all(type(n) is int for n in (entries, retired, budget, horizon, zero))
                and entries > 0 and retired >= entries - zero
                and all(0 <= n <= entries for n in (budget, horizon, zero))
                and row["reason"] in (0, 1) and row["delay"] in (0, 1),
                "invalid exit profile counters")
        start = owner(row["entry"], key[0], ids)
        end = owner(row["resume"], key[0], ids)
        # The aggregate has no joint budget/zero/horizon flags. Exclude all
        # rows containing zero-progress calls instead of assuming independence.
        eligible = entries - budget if (
            row["reason"] == 0 and row["delay"] == 0 and zero == 0) else 0
        if start is None or end is None:
            category = "unowned_or_external"
        elif start == end:
            category = "same_region"
        else:
            category = "cross_region"
        counts = {
            "entries": entries, "retired": retired, "budget_exhausted": budget,
            "horizon_limited": horizon, "zero_progress": zero,
            category + "_entries": entries,
            category + "_remaining_budget_candidates": eligible,
            # Worst-case overlap of horizon-limited and eligible calls.
            category + "_nonhorizon_lower_bound": max(0, eligible - horizon),
        }
        totals.update(counts)
        per_module[name].update(counts)
        if category == "cross_region" and eligible:
            edges[(name, row["entry"], row["resume"], start, end)] += eligible
    return {
        "schema": 1, "advisory_only": True,
        "totals": dict(totals),
        "modules": {name: dict(counts) for name, counts in sorted(per_module.items())},
        "router_inputs": {name: {"base": base, "hash": code_hash, **receipt}
                          for (base, code_hash), (name, _, receipt) in maps.items()},
        "cross_region_edges": [
            {"module": name, "entry": entry, "resume": resume,
             "from_region": start, "to_region": end, "candidates": count}
            for (name, entry, resume, start, end), count in
            sorted(edges.items(), key=lambda item: (-item[1], item[0]))],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, action="append", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    with zipfile.ZipFile(args.archive) as archive:
        receipt = json.loads(archive.read("profiling-receipt.json"))
        profile = json.loads(archive.read("candidate-1/iop-exit-profile.json"))
    require(receipt["correctness_accepted"] is True and receipt["inputs_unchanged"] is True
            and receipt["profile_iop_exits"] is True, "unaccepted diagnostic replay")
    runs = receipt["runs"]["candidate"]
    require(len(runs) == 1, "expected one diagnostic candidate")
    native = runs[0]["iop_aot"]
    require(native["entries"] == profile["entries"]
            and native["instructions"] == profile["retired"],
            "native counters do not reconcile")
    maps = load_routers(args.build_root, receipt["candidate_iop_inventory"])
    require(len(maps) == native["active"] == native["modules"],
            "active module count mismatch")
    result = analyze(profile, maps)
    result["archive_sha256"] = sha256(args.archive)
    with args.out.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    print(json.dumps(result["totals"], indent=2))


if __name__ == "__main__":
    main()
