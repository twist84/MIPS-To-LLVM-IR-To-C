# SDBZ US: IOP module-load call-site recovery

> **Record status (2026-11-25):** Historical original IRX module-call audit; later gameplay demonstrates progress beyond initial module-load checkpoints. See [current status](README.md), [retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md) and [accuracy plan](ACCURACY_PLAN.md). Any branch, PR, pending replay or feature-status claims below refer to the documented stage.


## Source and reproducibility

- Game: **Super Dragon Ball Z (US)**, `SLUS_214.42`.
- ELF SHA-256: `1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`.
- Analysis: 2026-11-16, direct disassembly of the user-provided ELF.
- This document contains derived addresses, control-flow descriptions and
  factual module-name strings. The copyrighted ELF and IRXs are not committed.
- Reproduce or compare with
  `python tools/analyze_iop_module_paths.py PS2_GAME/SLUS_214.42 --json build/sdbz/iop-module-paths.json`.

## Seven static candidate paths and loader calls

All strings are embedded as `cdrom0:\\<MODULE>.IRX;1` in the US ELF.
`JAL` destinations below are instructions verified by disassembly,
**not a runtime call trace**.

| IRX | Path guest VA | Path-to-argument code | Call instruction | Direct callee |
| --- | --- | --- | --- | --- |
| PADMAN | 0x004B70F0 | 0x00109F28 / 0x00109F2C | 0x00109F38 | 0x00103E40 |
| SIO2MAN | 0x004B7180 | 0x0010C528 / 0x0010C530 | 0x0010C538 | 0x00103E40 |
| MCMAN | 0x004DCE30 | 0x00422174 / 0x0042217C | 0x00422184 | 0x0017DEA0 |
| MCSERV | 0x004DCE50 | 0x004221B0 / 0x004221DC | 0x004221E0 | 0x0017DEA0 |
| LIBSD | 0x004DCE70 | 0x0042220C / 0x00422238 | 0x0042223C | 0x0017DEA0 |
| CRI_ADXI | 0x004DCEB0 | 0x004222E8 / 0x004222F0 | 0x004222F4 | 0x0017DEA0 |
| ARKD_DVD | 0x004DCF00 | 0x004223A8 / 0x004223AC | 0x004223B0 | 0x0017DEA0 |

`MCSERV` and `LIBSD` have no references in the earlier
`iop-module-paths.json` because the first static xref scanner stops
on a control-flow boundary. Both call sites reuse a `LUI $a0,0x004E`
value through branch-dependent code before adding the signed low half.
The new `call_candidates` field uses a linear backward slice and marks
such inferred call associations with `control_flow_ambiguous=true`.
The disassembly independently confirms the call instructions and
nearby argument-construction operands; full path reachability still
depends on CFG and branch outcomes.

## Loader call chain in the actual game ELF

- `0x00103E40` is a higher-level wrapper; its `JAL 0x0017DEA0`
  occurs at `0x00103E88` in its retry branch and at
  `0x00103EC8` in its other branch.
- `0x0017DEA0` is a thin wrapper calling `0x0017DC78`
  (at `0x0017DEAC`).
- `0x0017DC78` builds the RPC request using the supplied name
  and arguments, and calls `0x0017D4A0` for service initialization
  before calling `0x00178BE8`.
- `0x0017D4A0` constructs the SIF RPC server identifier
  `0x80000006` at `0x0017D4C8`/`0x0017D4D0`, then
  calls the SIF binding routine at `0x00178A08`.
- The reference implementation
  `ps2-web-recomp/SDBZ/runtime/src/iop/sif.cpp` handles
  LOADFILE server `0x80000006`: method 0x00 accepts a path at
  request offset +8; method 0x06 accepts a module buffer.

The code at `0x00422170` contains five module-loading calls in
the **static success-path order** MCMAN → MCSERV → LIBSD → CRI_ADXI →
ARKD_DVD, with conditional errors and formatting between calls.
The caller at `0x004224CC` executes `JAL 0x00422170`.
PADMAN and SIO2MAN belong to separate initialization routines
and cannot be placed into one global timeline solely from these sites.

## Current real-game boot frontier, 2026-11-16 05:44

The uploaded `expansion(20261116-054423).log`,
`boot(20261116-054423).log` and
`first-boot(20261116-054423).json` show:

- **6,945 EE dispatches**, no newly required guest translations.
- The guest reaches `0x004225B4` (dispatch 6144 return context)
  and then invokes `0x0010C480` from `0x004225CC`.
- During SIF initialization the guest executes
  `SifSetDChain` (channel 5, QWC=0, CHCR=0x184), but there
  is no IOP producer in the default runner.
- The guest executes `sceSifGetReg(4)` through syscall at
  `0x001750B4`. Its `0x00177CF8` poll loop checks SMFLAG bit
  `0x00020000`, which remains clear.
- After **256** repeated zero reads, the guard stops with
  `EE_EXIT_UNSUPPORTED` at `0x001750B4`.
- This trace contains **no completed LOADFILE SIF RPC method 0/6
  request** and no observed module-load path or successful IOP boot.

## Next development target

Build the legitimate IOP/SIF startup path that publishes its own SMFLAG
and handles the corresponding SIF DMA handshake. Then instrument
LOADFILE RPC requests *at receipt* (server, method, request contents,
guest callsite, result and ordering), and have those **observed**
requests drive IRX loading. An IRX name appearing in the ELF or a
nearby JAL is not evidence that the game executed that module load.
