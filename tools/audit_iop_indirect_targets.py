#!/usr/bin/env python3
"""Offline candidate audit of constant JR/JALR targets in discovered IOP code.

No runtime or builder changes. A recovered target is possible static
evidence, not proof of a unique dynamic target or a function boundary.
"""
import argparse
import collections
import json
import struct
from pathlib import Path

from audit_iop_function_discovery import classify, inventory, MASK
from build_iop_aot import relocation_receipt_roots


def constant_indirect_targets(metadata, data, report, max_words=32):
    if max_words < 1:
        raise ValueError("max_words must be positive")
    base = metadata["base"]
    ordinary = {pc for region in report["regions"] for pc in region["ordinary_pcs"]}
    delay = {pc for region in report["regions"] for pc in region["delay_pcs"]}
    entries = {region["entry"] for region in report["regions"]}
    incoming = collections.defaultdict(set)
    for edge in report["edges"]:
        incoming[edge["target"]].add((edge["pc"], edge["kind"]))

    def word_at(pc):
        return struct.unpack_from("<I", data, pc - base)[0]

    def preceding_words(site):
        # Walk only a unique known sequential predecessor, never through
        # a call return, branch target, delay slot or another entry root.
        # Unknown indirect incoming edges are why this is shadow evidence.
        result = []
        pc = site
        while len(result) < max_words and pc not in entries:
            prev = pc - 4
            if (prev not in ordinary or prev in delay or
                    incoming[pc] != {(prev, "next")} or
                    classify(word_at(prev)) != "simple"):
                break
            result.append(prev)
            pc = prev
        return list(reversed(result))

    def interpret_constants(pcs):
        values = {0: (0, ())}
        for pc in pcs:
            word = word_at(pc)
            op, rs, rt = word >> 26, (word >> 21) & 31, (word >> 16) & 31
            rd, funct, imm = (word >> 11) & 31, word & 63, word & 0xffff
            left, right = values.get(rs), values.get(rt)
            dst = rt
            result = None
            if op == 15 and rs == 0:  # LUI
                result = (imm << 16, (pc,))
            elif op in (9, 13) and left is not None:  # ADDIU / ORI
                signed = imm - 0x10000 if imm & 0x8000 else imm
                value = left[0] + signed if op == 9 else left[0] | imm
                result = (value & MASK, left[1] + (pc,))
            elif op == 0 and funct in (0x21, 0x25) and (word >> 6) & 31 == 0:
                dst = rd
                if left is not None and right is not None:  # ADDU / OR
                    value = left[0] + right[0] if funct == 0x21 else left[0] | right[0]
                    result = (value & MASK, tuple(sorted(set(left[1] + right[1] + (pc,)))))
            else:
                # No assumptions about loads, COP0, GP, or other writes.
                values = {0: (0, ())}
                continue
            if dst:
                values.pop(dst, None)
                if result is not None:
                    values[dst] = result
        return values

    observations = []
    unresolved = []
    for row in report["unresolved"]:
        kind, site = row["kind"], row["pc"]
        if kind not in ("indirect_call", "indirect_jump"):
            continue
        if site not in ordinary or site in delay or classify(word_at(site)) != kind:
            continue
        rs = (word_at(site) >> 21) & 31
        pcs = preceding_words(site)
        value = interpret_constants(pcs).get(rs)
        if value is None:
            unresolved.append({"site": site, "kind": kind, "register": rs})
            continue
        target, evidence = value
        observations.append({
            "site": site, "kind": kind, "register": rs, "target": target,
            "producer_pcs": list(evidence),
            "in_text": target % 4 == 0 and base <= target <= base + len(data) - 4,
            "already_entry": target in entries, "already_ordinary": target in ordinary,
            "already_delay": target in delay,
        })
    return {"observations": observations, "unresolved": unresolved,
            "new_call_candidates": sorted({row["target"] for row in observations
                if row["kind"] == "indirect_call" and row["in_text"] and
                not row["already_ordinary"] and not row["already_delay"]}),
            "new_jump_targets": sorted({row["target"] for row in observations
                if row["kind"] == "indirect_jump" and row["in_text"] and
                not row["already_ordinary"] and not row["already_delay"]})}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--relocation-receipt", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    try:
        receipt = json.loads(args.relocation_receipt.read_text(encoding="utf-8"))
        modules = []
        for path in sorted(args.capture.glob("*.json")):
            metadata = json.loads(path.read_text(encoding="utf-8"))
            data = path.with_suffix(".bin").read_bytes()
            baseline = inventory(metadata, data, extended=True)
            roots = relocation_receipt_roots(receipt, metadata, data, baseline)
            report = inventory(metadata, data, extended=True, relocated_call_entries=roots)
            if (report["shared_ordinary_pcs"] or report["ordinary_delay_overlap"]
                    or report["rejected_seeds"]):
                raise ValueError(f"ambiguous ownership: {path.stem}")
            result = constant_indirect_targets(metadata, data, report)
            result.update({"capture_stem": path.stem, "base": metadata["base"],
                           "hash": metadata["hash"], "text_size": len(data)})
            modules.append(result)
        if not modules:
            raise ValueError("no captured IRXs")
        with args.out.open("x", encoding="utf-8") as output:
            json.dump({"schema": 1, "kind": "iop_indirect_target_shadow",
                       "modules": modules}, output, indent=2)
            output.write("\n")
        for module in modules:
            print(f"{module['capture_stem']}: "
                  f"{len(module['observations'])} constant target observations, "
                  f"{len(module['unresolved'])} unresolved sites, "
                  f"{len(module['new_call_candidates'])} new call candidates, "
                  f"{len(module['new_jump_targets'])} new jump targets")
        print(f"Receipt: {args.out}")
    except (OSError, ValueError, KeyError, TypeError, struct.error) as exc:
        parser.exit(1, f"IOP indirect target audit: {exc}\n")


if __name__ == "__main__":
    main()
