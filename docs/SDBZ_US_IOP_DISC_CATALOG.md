# SDBZ US — IRX disc discovery and explicit bootstrap resolution

> **Record status (2026-11-25):** Historical disc catalog investigation; retain the original catalog and its provenance. See [current status](README.md), [retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md) and [accuracy plan](ACCURACY_PLAN.md). Any branch, PR, pending replay or feature-status claims below refer to the documented stage.


## Current state

The real-game boot stops at EE `0x001750B4` because IOP SMFLAG
`0x00020000` is never published. The newest user logs show 6,945
EE dispatches and zero IOP producer execution. No command-line
`--iop-irx` option was passed.

The ELF static path scanner already identified seven `cdrom0:` IRX
filename candidates. **Those strings are not confirmed calls, a
dependency graph, or an IOP firmware boot sequence.**

## What the new code does

`tools/iop_disc_catalog.py` resolves candidate paths against the
extracted disc folder containing `SLUS_214.42` by default, or a
specified `--disc-root`. It handles Windows and ISO9660-style case
variation and optional `;1`, rejects `..`, ambiguous matches and
symlink traversal, bounds file size, and checks the ELF32 MIPS header.

`tools/run_boot_session.py` now automatically generates
`iop-disc-catalog.json` **before the first replay**, including
resolved/missing/unsupported candidates. This is diagnostic
discovery, not execution. The wrapper is separate from `run_game.py`,
so the user's existing translated EE library and `--no-recompile`
provenance are unchanged.

Optional explicit bootstrapping can select a disc filename without
manually writing its host path:

```powershell
python tools/run_boot_session.py PS2_GAME/SLUS_214.42 --work-dir=build/sdbz --report=build/sdbz/first-boot.json --iop-auto-irx=PADMAN.IRX --no-recompile
```

**Caution:** that option loads and starts the selected module using
the existing experimental `--iop-irx` mechanism. Supply any necessary
provider modules explicitly in dependency order, and never assume
that PADMAN is a valid standalone boot module. The command above
is an example of lookup syntax, *not* a recommended game-boot recipe.

## Missing native-boot functionality

- Start real IOP boot firmware/initialization, not a synthetic ready bit.
- Wire actual SIF RPC LOADFILE `0x80000006` calls to on-demand IRX
  lookup, import linking and module entrypoint execution.
- Implement bidirectional SIFCMD/RPC replies, callbacks and IOP cycle
  scheduling. The catalog alone cannot advance the EE past its SMFLAG
  poll.

Do not preload all seven static paths at startup or infer runtime
execution order from adjacent ELF strings.
