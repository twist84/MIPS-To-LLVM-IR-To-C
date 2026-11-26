#!/usr/bin/env python3
"""Compare static generated-code costs using the production LLVM target.

Examples:
  python tools/audit_ir_cost.py "build/ps2-game-hle/ir/*1906f0*.ll"
  python tools/audit_ir_cost.py "saved-ir/*.ll" --compare-dir build/ps2-game-hle/ir
  python tools/audit_ir_cost.py "saved-ir/*.ll" --profile 4 --json-out audit.json

Counts include internal helpers, not just the guest entry symbol. Native
assembly counts are static x86 mnemonic counts, not dynamic instructions.
Object bytes include metadata and relocations. No cache or source is written.
"""
import argparse
import glob
import hashlib
import json
import re
from pathlib import Path

import llvmlite.binding as llvm
import verify_ir


# LLVM's x86 assembler output has one mnemonic per instruction line.
# Skip directives, labels, comments and metadata. Standalone prefixes
# are not separate retired instructions; count the following mnemonic.
MNEMONIC = re.compile(r"^\s+([a-zA-Z][a-zA-Z0-9_.]*)\s*(?:[^\n]*)$")
PREFIXES = {"lock", "rep", "repe", "repne", "repz", "repnz", "data16", "addr32"}


def assembly_counts(assembly):
    counts = {"instructions": 0, "calls": 0, "branches": 0}
    for line in assembly.splitlines():
        line = line.split("#", 1)[0].rstrip()
        match = MNEMONIC.match(line)
        if not match or ":" in line.split()[0]:
            continue
        mnemonic = match.group(1).lower()
        if mnemonic in PREFIXES:
            operands = line.strip().split()
            if len(operands) < 2:
                continue
            mnemonic = operands[1].lower()
        counts["instructions"] += 1
        counts["calls"] += mnemonic.startswith("call")
        counts["branches"] += mnemonic.startswith("j") or mnemonic.startswith("loop")
    return counts


def ir_counts(module):
    functions = [fn for fn in module.functions if not fn.is_declaration]
    instructions = 0
    guest_instructions = 0
    for fn in functions:
        count = sum(1 for block in fn.blocks for _ in block.instructions)
        instructions += count
        if fn.name.startswith("ee_fn_"):
            guest_instructions += count
    return {"functions": len(functions), "instructions": instructions,
            "guest_function_instructions": guest_instructions}


def measure(path, profile, target_triple, machine):
    raw = path.read_bytes()
    with llvm.create_context() as context, llvm.parse_assembly(
            raw.decode("utf-8"), context=context) as module:
        module.verify()
        source = ir_counts(module)
        module.triple = target_triple
        module.data_layout = str(machine.target_data)
        module.verify()
        if verify_ir.uses_ir_pipeline(profile, raw):
            verify_ir.optimize_guest_ir(module, machine)
        after = ir_counts(module)
        assembly = assembly_counts(machine.emit_assembly(module))
        object_bytes = len(machine.emit_object(module))
    return {"name": path.name, "sha256": hashlib.sha256(raw).hexdigest(),
            "source_ir": source, "pipeline_ir": after,
            "native_assembly": assembly, "object_bytes": object_bytes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", help="IR paths or glob patterns")
    parser.add_argument("--profile", type=int, choices=range(6), default=5)
    parser.add_argument("--compare-dir", type=Path,
                        help="compare the exact same module basenames in this directory")
    parser.add_argument("--json-out", type=Path)
    args = parser.parse_args()
    files = sorted({Path(name).resolve() for pattern in args.inputs
                    for name in glob.glob(pattern, recursive=True) if Path(name).is_file()})
    if not files:
        parser.error("no IR files matched")
    if len({path.name for path in files}) != len(files):
        parser.error("duplicate basenames; select a single IR generation")
    if args.compare_dir:
        missing = [path.name for path in files
                   if not (args.compare_dir / path.name).is_file()]
        if missing:
            parser.error("missing comparison modules: " + ", ".join(missing[:8]))
    triple = llvm.get_default_triple()
    if not triple.startswith(("x86_64", "i386", "i686")):
        parser.error("native mnemonic counting currently supports x86 only")
    machine = verify_ir.get_target_machine(args.profile, triple)
    result = {"schema": "ee-static-ir-cost-v1", "profile": args.profile,
              "target_triple": triple, "llvm_version": list(llvm.llvm_version_info),
              "native_target": verify_ir.native_target_identity(args.profile),
              "ir_pipeline": verify_ir.ir_pipeline_id(args.profile), "modules": []}
    for path in files:
        before = measure(path, args.profile, triple, machine)
        row = {"before": before}
        if args.compare_dir:
            row["after"] = measure(args.compare_dir / path.name, args.profile, triple, machine)
        result["modules"].append(row)
        print(path.name)
        for label, values in row.items():
            print(f"  {label}: IR {values['source_ir']['instructions']:,} "
                  f"-> {values['pipeline_ir']['instructions']:,}; "
                  f"native {values['native_assembly']['instructions']:,} "
                  f"calls {values['native_assembly']['calls']:,} "
                  f"branches {values['native_assembly']['branches']:,}; "
                  f"object {values['object_bytes']:,} bytes", flush=True)
    if args.json_out:
        verify_ir.write_atomic(args.json_out,
                               (json.dumps(result, indent=2) + "\n").encode("utf-8"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
