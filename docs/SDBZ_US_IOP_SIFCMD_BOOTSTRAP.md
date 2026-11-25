# SDBZ (US): guest-initialized IOP SIFCMD bootstrap

> **Record status (2026-11-25):** Historical SIFCMD bootstrap record; current retail execution has progressed beyond early bootstrap. See [current status](README.md), [retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md) and [accuracy plan](ACCURACY_PLAN.md). Any branch, PR, pending replay or feature-status claims below refer to the documented stage.


## Why this milestone exists

In the most recent real-game boot, the EE initialized SIF0 channel 5
and repeatedly polled SMFLAG bit \`0x00020000\`. No IOP guest code had
produced this bit. The previous milestone provided validated EE-to-IOP
SIF1 RAM transfers, but no IOP-side SIFCMD receiver existed.

This milestone connects the first **guest-executed** IOP SIFCMD imports
to the EE SIF1 bridge without synthesizing an IOP boot or success flags.

## Guest behavior

\`c_sifcmd_bridge\` registers executable R3000 stubs for three sifcmd
ordinals:

| Ordinal | Guest operation | Implementation |
|---|---|---|
| 4 | sceSifInitCmd | Initializes IOP command endpoint and publishes its SUBADDR to shared SIF registers |
| 6 | sceSifGetSreg | Reads one of 32 IOP command system registers |
| 7 | sceSifSetSreg | Sets one of 32 IOP command system registers |

The stub must actually be called by guest R3000 instructions. Simply
installing the library does not mark the endpoint as initialized.

The endpoint uses the reserved IOP command buffer at \`0x0001E000\`,
as in \`ps2-web-recomp/runtime/src/iop/sif.cpp\`.
The EE-to-IOP SIF1 transfer recognizes this destination, validates the
original packet's size and command semantics, and stages each transfer
before committing IOP RAM or changing any SIFCMD state.

Supported system command IDs:

- \`0x80000001\`: publish a bounded IOP SREG index and value.
- \`0x80000002\` (opt=0): store the EE command-receive address.

These commands are *not* RPC completion. Unknown commands, such as
\`0x80000009\` (RPC bind), still fail closed because there is no IOP
guest RPC dispatcher or callback scheduler yet.

## Test scope

\`iop_sifcmd_bridge_test\` actually runs guest R3000 code through the
IOP scheduler and the registered HLE import stub. It verifies:

- No published SUBADDR or command delivery before the import executes.
- Correct SUBADDR after guest sceSifInitCmd, with SMFLAG still zero.
- SIF1 system-command packet data copied to IOP RAM and SREG updated.
- Capturing the EE-side command receive address.
- Transactional rejection of unknown RPC CIDs in a two-descriptor batch.
- Reserved command-buffer overlap protection.
- Another guest thread invoking sceSifSetSreg successfully.

## Remaining work

A real IOP BIOS/initialization environment, interrupt-driven SIFCMD
callbacks, bidirectional command replies, LOADFILE \`0x80000006\` RPC,
and guest-triggered SMFLAG readiness are **not** implemented by this
milestone. The ordinary launcher still does not boot an IOP firmware
image. Do not claim SDBZ has advanced beyond its last observed PC
without another authentic game execution.
