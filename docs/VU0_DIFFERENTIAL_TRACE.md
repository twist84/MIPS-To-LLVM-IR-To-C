# VU0 interpreter differential trace format

The VU0 lower integer/branch and ordinary-memory subset has a direct,
pinned-PCSX2 interpreter differential path. This document defines the
JSONL interchange format and the limits of that validation.

## Pinned reference

Use PCSX2 revision
`82cac38b2af19d593dedfd9ddaa63448e7ec493c` (the revision audited for the
lower-op/interlock/pipe/VI-backup ordering correction).

Relevant reference implementation:

- `pcsx2/VU0microInterp.cpp`: `_vu0Exec`, `vu0Exec`, and
  `InterpVU0::Execute`.
- `pcsx2/VUops.cpp`: lower-op metadata, `_vuTestLowerStalls`,
  `_vuTestPipes`, and VI backup accessors.

The adapter must use the interpreter, not microVU/JIT. It must disable speed
hacks that alter cycle accounting and execute one micro pair at a time. Capture
state after `vu0Exec` returns, when the interpreter's cycle has advanced and
branch/E retirement has completed. The micro-PC must be normalized to the
byte-address representation used by this project.

## JSONL contract

A trace is UTF-8 JSON Lines. The first line is a header:

```json
{
  "schema": "mips-to-llvm-vu0-trace-v1",
  "kind": "header",
  "engine": "pcsx2-interpreter",
  "revision": "82cac38b2af19d593dedfd9ddaa63448e7ec493c",
  "program_sha256": "sha256-of-the-4096-byte-micro-memory",
  "initial_state": {
    "vi": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    "tpc": 0,
    "cycle": 0,
    "data_sha256": "sha256-of-the-4096-byte-vu0-data-memory"
  }
}
```

Each subsequent line records one completed pair. `lower` and `upper` are the
raw 32-bit instruction words. `state.vi` contains all 32 architectural VI
register values (including the special-register slots). `state.vf` contains
all 32 VF registers as raw unsigned X/Y/Z/W lane bit patterns. The remaining fields
are the state needed to validate the currently implemented scalar/branch
subset:

```json
{
  "kind": "step",
  "step": 0,
  "pc": 0,
  "lower": 1006634032,
  "upper": 0,
  "state": {
    "vi": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    "vf": [[0, 0, 0, 1065353216],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0],
           [0, 0, 0, 0]],
    "cycle": 1,
    "micro_pc": 8,
    "tpc": 1,
    "branch": 0,
    "branch_pc": 0,
    "delay_branch_pc": 0,
    "take_delay_branch": false,
    "vi_backup_cycles": 0,
    "vi_backup_register": 0,
    "vi_backup_old_value": 0,
    "e_bit": 0,
    "m_bit": false,
    "vpu_status": 1,
    "running": true,
    "data_sha256": "sha256-of-the-4096-byte-vu0-data-memory"
  }
}
```

The shared fixture provides a deterministic 72-pair program, 63 of whose pairs execute. The
candidate producer executes one runtime pair per exact-cycle callback and
writes a trace. The pinned PCSX2 adapter executes one interpreter pair through
vu0Exec(&VU0) and writes the same format when VU0_TRACE_OUTPUT is set. The
trace records the executed pair PC as well as both instruction words.

The comparator checks program identity and initial state, then compares every
pair PC, instruction word, VI/VF register lane, listed architectural field and VU0 data-memory
SHA-256 exactly after each pair.
Engine name and source revision are informational and intentionally need not
match. A mismatch reports the first differing VI register or state field.

## Comparator

After generating one trace from the pinned PCSX2 interpreter and one from this
project's VU0 runtime, run:

```sh
python3 tools/compare_vu0_traces.py pcsx2.jsonl candidate.jsonl
```

Exit codes: 0 means exact match, 1 means architectural mismatch, and 2 means
malformed/incompatible traces.

## Current implementation and limitation

The repository now contains all pieces needed to produce both sides of the
trace:

- A shared deterministic 72-pair microprogram fixture, including ILW/ISW,
  masked LQ/SQ, and indexed LQI/SQI/LQD/SQD.
- The in-tree candidate trace executable, built as `ee_vu0_trace_candidate`.
- A pinned PCSX2 GoogleTest adapter that captures one pair after each
  `vu0Exec(&VU0)` call when `VU0_TRACE_OUTPUT` is set.
- The JSONL comparator and regression tests, including executed-PC comparison.

CTest validates the candidate trace path and its expected 63 executed pairs.
To run the real cross-engine comparison, use either
`tools/vu0_diff/run_differential.py` with a PCSX2 checkout and its dependencies,
or trigger the manual GitHub Actions workflow named **VU0 PCSX2 differential**.
That workflow checks out the pinned PCSX2 revision, builds its core test target,
runs the paired trace capture, compares both JSONL files, and uploads the traces.

The earlier 41-pair integer/branch fixture was compared directly against
PCSX2 in differential run #7, with an exact match. The expanded 45-pair
ordinary-memory fixture passed the direct PCSX2 comparison in differential
run #9: all 45 pairs, including ILW/ISW and the complete per-pair 4 KB data
memory SHA-256, matched exactly. The trace consumed 47 cycles, including
the branch-only IALU interlock policy observed in the pinned interpreter.
The 49-pair fixture additionally exercises masked VF LQ/SQ operations and
preservation of unselected vector lanes. The direct pinned PCSX2 comparison
passed in [the 49-pair differential run](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43316781870):
all 49 executed pairs, VF/VI state, 4 KB data-memory digests, and E-bit
termination timing matched exactly (51 cycles). The runtime models the
four-cycle FMAC completion deadline for ordinary LQ/SQ at E termination.
The follow-up [VF hazard differential run](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43318464992)
also passed all 49 executed pairs, now including a masked LQ-to-SQ
read-after-write dependency on VF3.x. The candidate and PCSX2 agreed on
the dependent store's stall, final cycle count, VF/VI registers, and memory
digests. The [53-pair indexed-memory differential run](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43320848379)
then passed with LQI/SQI/LQD/SQD, covering post-increment and pre-decrement
VI address updates, masked VF loads and stores, and a dependent indexed VF
store. PCSX2 and the candidate matched after every one of the 53 executed
pairs, finishing at cycle 61 and micro-PC 0x1e0. Only lower memory VF
lane interlocks are verified; upper-vector dependencies and VU1-mapped
memory accesses remain out of scope.

## Expanded sign-branch trace (Batch 12)

The 72-pair fixture adds IBLTZ on a stable negative VI11, IBGTZ on
zero VI0, IBLEZ on zero VI0, and IBGEZ on negative VI11. Distinct delay
and fallthrough sentinels verify both paths, with the exact 63 executed
PCs and program SHA-256 pinned by the candidate CTest. The original
53-pair PCSX2 result above remains historical; the expanded trace has
not yet been compared against the pinned PCSX2 interpreter. The expected
71-cycle end state is a candidate regression, not measured hardware.
