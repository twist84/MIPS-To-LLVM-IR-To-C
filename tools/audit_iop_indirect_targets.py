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


def producer_slice(pcs, word_at, target_register):
    """Describe local reaching definitions, never dereference guest memory.

    Definitions are a bounded, flat DAG. An absent predecessor or an
    unmodeled operation leaves a live-in/unknown value, not a constant.
    """
    definitions = {}
    nodes = {}

    def source(reg):
        if reg == 0:
            return {"register": 0, "constant": 0}
        if reg not in definitions:
            return {"register": reg, "live_in": True}
        pc = definitions[reg]
        value = {"register": reg, "producer_pc": pc}
        if "constant" in nodes[pc]:
            value["constant"] = nodes[pc]["constant"]
        return value

    for pc in pcs:
        word = word_at(pc)
        op, rs, rt = word >> 26, (word >> 21) & 31, (word >> 16) & 31
        rd, sa, funct = (word >> 11) & 31, (word >> 6) & 31, word & 63
        imm = word & 0xffff
        signed = imm - 0x10000 if imm & 0x8000 else imm
        dst = rt
        row = {"pc": pc, "word": f"{word:08x}"}
        left, right = source(rs), source(rt)
        if op == 15:
            row.update(kind="lui", inputs=[], constant=imm << 16)
        elif op in (8, 9, 10, 11, 12, 13, 14):
            name = {8: "addi", 9: "addiu", 10: "slti", 11: "sltiu",
                    12: "andi", 13: "ori", 14: "xori"}[op]
            row.update(kind=name, inputs=[left],
                       immediate=signed if op in (8, 9, 10, 11) else imm)
            if "constant" in left and op in (9, 12, 13, 14):
                value = left["constant"]
                row["constant"] = {9: value + signed, 12: value & imm,
                                   13: value | imm, 14: value ^ imm}[op] & MASK
        elif op in (32, 33, 35, 36, 37):
            row.update(kind="load", inputs=[left], offset=signed,
                       width={32: 1, 33: 2, 35: 4, 36: 1, 37: 2}[op])
            if "constant" in left:
                row["constant_address"] = (left["constant"] + signed) & MASK
            elif rs == 28 and left.get("live_in"):
                row["gp_relative"] = True
        elif op in (40, 41, 43):
            continue  # stores do not define a GPR
        elif op == 0:
            dst = rd
            if funct in (0, 2, 3):
                row.update(kind={0: "sll", 2: "srl", 3: "sra"}[funct],
                           inputs=[right], shift=sa)
                if "constant" in right and funct in (0, 2):
                    value = right["constant"]
                    row["constant"] = ((value << sa) if funct == 0
                                       else (value >> sa)) & MASK
            elif funct in (4, 6, 7, 32, 33, 34, 35, 36, 37, 38, 39, 42, 43):
                row.update(kind={4: "sllv", 6: "srlv", 7: "srav",
                                 32: "add", 33: "addu", 34: "sub", 35: "subu",
                                 36: "and", 37: "or", 38: "xor", 39: "nor",
                                 42: "slt", 43: "sltu"}[funct], inputs=[left, right])
                if funct in (33, 37) and "constant" in left and "constant" in right:
                    a, b = left["constant"], right["constant"]
                    row["constant"] = (a + b if funct == 33 else a | b) & MASK
            elif funct in (16, 18):
                row.update(kind="mfhi" if funct == 16 else "mflo", inputs=[])
            elif funct in (17, 19, 24, 25, 26, 27):
                continue  # HI/LO writes only
            else:
                definitions.clear()
                continue
        else:
            definitions.clear()
            continue
        if dst:
            row["destination"] = dst
            nodes[pc] = row
            definitions[dst] = pc

    target = source(target_register)
    needed = set()
    pending = [target]
    while pending:
        value = pending.pop()
        pc = value.get("producer_pc")
        if pc is None or pc in needed:
            continue
        needed.add(pc)
        pending.extend(nodes[pc]["inputs"])

    chain = [nodes[pc] for pc in sorted(needed)]
    # Follow register copies to the effective producer for classification.
    root = target
    while "producer_pc" in root:
        node = nodes[root["producer_pc"]]
        inputs = node["inputs"]
        if node["kind"] in ("addu", "or") and len(inputs) == 2:
            if inputs[0].get("constant") == 0:
                root = inputs[1]
                continue
            if inputs[1].get("constant") == 0:
                root = inputs[0]
                continue
        if node["kind"] in ("sll", "srl", "sra") and node.get("shift") == 0:
            root = inputs[0]
            continue
        if node["kind"] in ("addiu", "ori") and node.get("immediate") == 0:
            root = inputs[0]
            continue
        break
    node = nodes.get(root.get("producer_pc"))
    classification = "live_in" if node is None else node["kind"]
    if node and node["kind"] == "load":
        # Scaled-index address calculations are evidence for table lookup,
        # not proof of a bounded switch or a particular memory value.
        address_nodes = set()
        address_pending = list(node["inputs"])
        while address_pending:
            value = address_pending.pop()
            pc = value.get("producer_pc")
            if pc is not None and pc not in address_nodes:
                address_nodes.add(pc)
                address_pending.extend(nodes[pc]["inputs"])
        if "constant_address" in node:
            classification = "constant_address_load"
        elif node.get("gp_relative"):
            classification = "gp_relative_load"
        elif (any(nodes[pc]["kind"] == "sll" and nodes[pc].get("shift") == 2
                  for pc in address_nodes) and
              any(nodes[pc]["kind"] in ("add", "addu") for pc in address_nodes)):
            classification = "scaled_index_load"
        else:
            classification = "register_address_load"

    return {"classification": classification, "target": target,
            "definitions": chain, "window_pcs": pcs}


def constant_indirect_targets(metadata, data, report, max_words=32,
                              audit_producers=False):
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
            observation = {"site": site, "kind": kind, "register": rs}
            if audit_producers:
                observation["producer"] = producer_slice(pcs, word_at, rs)
            unresolved.append(observation)
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
    parser.add_argument("--audit-producers", action="store_true",
                        help="Report bounded local reaching definitions for unresolved sites")
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
            result = constant_indirect_targets(metadata, data, report,
                                               audit_producers=args.audit_producers)
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
            if args.audit_producers:
                counts = collections.Counter(row["producer"]["classification"]
                                             for row in module["unresolved"])
                print(f"  producer classes: {dict(sorted(counts.items()))}")
        print(f"Receipt: {args.out}")
    except (OSError, ValueError, KeyError, TypeError, struct.error) as exc:
        parser.exit(1, f"IOP indirect target audit: {exc}\n")


if __name__ == "__main__":
    main()
