# SDBZ (US) native boot: on-demand IRX loader backend

> **Record status (2026-11-25):** Historical IOP disc backend implementation record, not a current blocker list. See [current status](README.md), [retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md) and [accuracy plan](ACCURACY_PLAN.md). Any branch, PR, pending replay or feature-status claims below refer to the documented stage.


## Real game evidence

The 2026-11-16 07:13 boot logs still stop at `0x001750B4`
after 6,945 EE dispatches while the EE polls IOP SMFLAG
`0x00020000`. No IOP module was started and no LOADFILE RPC was
observed. The new `iop-disc-catalog.json` resolves all seven
ELF-embedded `cdrom0:` filenames, but `runtime_load_count` is zero.

The catalog's static ELF references are **not module load events**.

## Guest-requested disc loading

`iop::c_disc_loader` provides the file and IRX-linking backend for
the pending LOADFILE server `0x80000006`:

1. The request must supply its own `cdrom0:` path. No arbitrary
   installed IRXs are loaded from a speculative static candidate list.
2. Resolve against an extracted disc root, with ASCII-insensitive
   names, optional ISO9660 `;1` suffix, and nested directories.
3. Reject unsupported devices, empty/relative/traversal path
   components, symlink escapes, ambiguous filenames, oversized files,
   and invalid ELF32 MIPS/IRX headers.
4. Pass the actual module bytes to the **existing** guest IOP kernel
   linker. It resolves imports from real exported functions or
   deliberately installed IOP HLE imports.
5. Optionally create and start the IRX entrypoint as a real guest
   R3000A thread. Unresolved dependencies do not produce a fake
   module ID or readiness.

This deliberately does **not** register an EE syscall, fabricate
SIF replies, or mark the IOP as ready.

## Regression

`iop_disc_loader_test` writes small synthetic IRX files into a
temporary extracted-disc layout. It verifies:

- Missing provider imports abort loading.
- Explicitly registered providers permit import linking.
- A guest-requested IRX entrypoint executes in the existing R3000
  scheduler.
- Linking a provider without starting its thread is supported.
- ISO9660 version suffixes, case folding and subdirectories work.
- Invalid paths, ambiguous files, invalid binaries and symlink
  escapes fail closed.

## Next integration

To make this load path reachable from **the actual game**, implement
the EE→IOP SIF1 command sequence and LOADFILE RPC (`0x80000006`)
server in the guest-initialized IOP SIFCMD bridge, including:
a validated parameter DMA, RPC bind and call packets, IOP→EE reply
DMA through an armed SIF0 channel, and callbacks/scheduling.

Even with that bridge, an IOP firmware or explicitly sourced
initialization image must run first to produce SMFLAG readiness.
The current `--iop-irx` bootstrap is optional and cannot replace
firmware or infer the startup sequence from game ELF strings.
