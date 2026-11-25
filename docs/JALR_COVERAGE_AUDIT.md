# USA SLUS_214.42 — JALR coverage audit

> **Record status (2026-11-25):** Historical static indirect-call audit at the pinned revision; not a current coverage percentage. See [current status](README.md), [retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md) and [accuracy plan](ACCURACY_PLAN.md). Branch names, open PR instructions, and pending gates in the historical text refer to the documented revision.


**Date:** 2026-11-12  
**Branch:** `recovery/reassembled-source-2026-11-11`  
**Audit implementation:** per-site static JALR records, commit `e50614269c642740dc58c8297da571eb9fa5eae2`  
**CI:** [Actions run #43219107693](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43219107693), 15/15 CTest passed.

## Scope and reproducibility

Input: the preserved USA `SLUS_214.42` ELF (entry `0x00100008`). The audit was run with the CI-built `ee_recomp` from the commit above and `--max-functions=1`. That option caps IR-file emission only: discovery, CFG construction and the JALR site audit still traverse the complete reachable decoded image.

The run completed in 6.98 seconds and reported:

- **7,901** discovered function entries.
- **418,898** unique function-scoped instruction PCs.
- **418,019 supported / 879 unsupported** decoder/lifter classifications (99.8%).

The generated `ee_functions.json` contains a `jalr_sites` record for each caller/site analysis, including caller entry, JALR PC, source register, whether that source is statically known at the JALR, its sampled value, the 32-bit candidate, alignment, executable-range membership and a status string. The source is sampled before the JALR link write and before the delay-slot instruction.

## Results

There are **3,535 caller/site records** representing **3,505 distinct JALR PCs**. The 30 additional records are repeated PCs analyzed from more than one caller root. No site PC received conflicting resolution statuses across those roots.

| Status | Caller/site records | Distinct JALR PCs |
|---|---:|---:|
| Unknown source value | 3,475 | 3,448 |
| Known, aligned, executable target | 56 | 53 |
| Known but zero or misaligned target | 4 | 4 |
| Known, aligned, non-executable target | 0 | 0 |
| **Total** | **3,535** | **3,505** |

The 56 resolved records identify **52 distinct executable target addresses** (some call sites share a target). These are the same 52 JALR-only entries persisted in `static_jalr_targets` / `jalr_only_entries`. The 52 entries were previously checked against the ELF: all are file-backed executable code and each has a `JR $ra` within its first 24 instruction words. Fifty use a common stack-frame wrapper shape; two are short leaf routines. This is structural validation, not proof of all dynamic call paths.

### Unknown source register distribution

Counts below are distinct unknown JALR PCs grouped by the source register:

| Register | Unknown PCs |
|---|---:|
| `$t9` (`$25`) | 2,944 |
| `$v0` (`$2`) | 327 |
| `$v1` (`$3`) | 104 |
| `$fp/$s8` (`$30`) | 24 |
| `$s5` (`$21`) | 18 |
| `$s3` (`$19`) | 9 |
| `$a3` (`$7`) | 8 |
| Other registers | 14 |

The dominant unresolved group is `$t9`: **2,890 of its 2,944** distinct unknown sites (98.2%) have an immediate preceding `lw` that writes `$t9`. Representative instruction sequences are:

```asm
lw    $25, 0x48($16)     ; fetch object member
lw    $25, 0x10($25)     ; fetch function slot
jalr  $25
```

A common `$v0` shape is also memory-indirect:

```asm
lw    $3, 0x0($4)        ; object/vtable pointer
lw    $2, 0x24($3)       ; function slot
jalr  $2
```

For the 327 unknown `$v0` sites, 200 have an immediate preceding load into `$v0` (61.2%). For the 104 unknown `$v1` sites, 54 have an immediate preceding load into `$v1` (51.9%). These patterns indicate runtime object/vtable dispatch rather than a missed arithmetic constant-building idiom.

A smaller subset of `$v1` sites load through an address formed by `LUI` plus a signed displacement. For example, the call at `0x00115334` loads through effective address `0x0043c948`; the corresponding file-backed word in this ELF is zero. The on-disk image therefore does not provide a usable static target at that slot. Treating that load as a constant function pointer would be unsound without modelling runtime initialization.

## Conclusion and next work

The current constant propagation covers the address-building instructions already identified in code and tests: `LUI`, `ORI`, `ANDI`, `XORI`, `ADDIU`, `DADDIU`, and selected SPECIAL arithmetic/shift forms. The site audit does not provide evidence for a further arithmetic-only rule that would safely recover the dominant unresolved calls. Most are function pointers loaded from runtime object/vtable memory; some global slots are zero in the on-disk ELF and require runtime initialization.

Do **not** broaden static recovery by guessing targets from these loads. Further coverage requires a separate, explicitly modelled memory/relocation analysis (including write-before-call and alias checks), interprocedural value propagation, or runtime target tracing. Keep the conservative all-predecessors-agree rule. This audit improves observability; it does not claim that all dynamic JALR destinations have been recovered.
