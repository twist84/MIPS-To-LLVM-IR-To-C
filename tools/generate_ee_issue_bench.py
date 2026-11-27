#!/usr/bin/env python3
"""Generate identical straight-line R5900 counter benchmarks and candidate traces.

No hardware results are generated here. The trace describes the body only;
counter reads, setup and fences are deliberately outside the candidate model.
"""
import argparse
import json
from pathlib import Path

# Each block contains exactly two instruction words, using caller-saved
# GPRs and FPRs. No assembler scheduling, pseudo-ops or branch overhead.
CASES = {
    "nop_pair": (0x00000000, 0x00000000),
    "alu_independent": (0x25080001, 0x25290001),
    "alu_raw": (0x25080001, 0x25080001),
    "load_independent": (0x8c880000, 0x25290001),
    "load_raw": (0x8c880000, 0x25090001),
    "mac_mflo": (0x01090018, 0x00005012),
    "mac_repeat": (0x01090018, 0x01090018),
    "cop1_op_move": (0x46010100, 0x440a1000),
    "cop1_move_op": (0x440a1000, 0x46010100),
}
REPEATS = (16, 64, 256)
SAMPLES = 11


def generate(out: Path) -> list[dict]:
    out.mkdir(parents=True, exist_ok=True)
    assembly = [".text", ".set noreorder", ".set noat"]
    header = ["/* Generated; do not edit. */", "#pragma once",
              "#include <stdint.h>",
              "typedef uint32_t (*bench_fn)(const uint32_t *);"]
    rows = []
    for name, block in CASES.items():
        for repeat in REPEATS:
            symbol = f"bench_{name}_{repeat}"
            words = block * repeat
            # Setup uses known initialized positive operands; f0=f1=f2=1.
            # Align the body after the two fixed start-boundary instructions.
            assembly += [".balign 16", f".globl {symbol}", f"{symbol}:",
                         "    addiu $8,$0,1", "    addiu $9,$0,2",
                         "    lui $11,0x3f80", "    mtc1 $11,$f0",
                         "    mtc1 $11,$f1", "    mtc1 $11,$f2",
                         ".balign 16", "    .word 0", "    .word 0",
                         "    sync.p", "    mfpc $2,0",
                         f"{symbol}_body:"]
            assembly += [f"    .word 0x{word:08x}" for word in words]
            assembly += ["    sync.p", "    mfpc $3,0",
                         "    subu $2,$3,$2", "    jr $31", "    nop"]
            header.append(f"extern uint32_t {symbol}(const uint32_t *);")
            trace = f"{symbol}.csv"
            base = 0x1000
            lines = ["attempt,pc,opcode_word,kind,reference_slot,hle_service"]
            for i, word in enumerate(words):
                lines.append(f"{i + 1},0x{base + 4*i:08x},0x{word:08x},0,{i + 1},0")
            lines.append(f"# truncated=false range=0x{base:08x}:0x{base + 4*(len(words)-1):08x}")
            (out / trace).write_text("\n".join(lines) + "\n", encoding="utf-8")
            rows.append({"case": name, "repeat": repeat,
                         "instructions": len(words), "symbol": symbol, "trace": trace})
    header += ["struct bench_case { const char *name; unsigned repeat; bench_fn run; };",
               "static const struct bench_case cases[] = {"]
    header += [f'    {{"{row["case"]}", {row["repeat"]}, {row["symbol"]}}},'
               for row in rows]
    header += ["};", f"#define BENCH_SAMPLES {SAMPLES}"]
    (out / "bench.S").write_text("\n".join(assembly) + "\n", encoding="utf-8")
    (out / "bench_cases.h").write_text("\n".join(header) + "\n", encoding="utf-8")
    (out / "cases.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    return rows


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    print(f"Generated {len(generate(args.out))} counter benchmark bodies; no measurements.")
