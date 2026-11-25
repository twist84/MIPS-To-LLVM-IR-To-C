# SDBZ (US): IOP startup and EE-to-IOP SIF1 transport

> **Record status (2026-11-25):** Historical SIF1/startup investigation; early synchronization stops are not current gameplay blockers. See [current status](README.md), [retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md) and [accuracy plan](ACCURACY_PLAN.md). Any branch, PR, pending replay or feature-status claims below refer to the documented stage.


## Observed boot blocker (16 November 2026)

The latest real-game boot capture reached 6,945 EE dispatches and stopped at
\`0x001750B4\` while polling IOP-owned \`SMFLAG & 0x00020000\`.
The EE-side \`sceSifSetDChain\` had armed SIF0 channel 5
(\`QWC=0\`, \`CHCR=0x184\`); the actual IOP had not produced the
required flag. No LOADFILE RPC or IOP guest module execution was observed.

The translated EE cache is sufficient for this checkpoint.
Increasing dispatch budgets or pre-setting \`SMFLAG\` is not a fix.

## Transport added in this milestone

\`src/iop/iop_sif1_bridge.{h,cpp}\` models the data movement component
of the EE kernel's SIF1 \`sceSifSetDma\` operation:

- Reads guest EE 16-byte descriptors \`{src,dst,size,attr}\`.
- Checks and stages **all** descriptors before any IOP RAM is changed.
- Supports bounded physical/KSEG EE RAM, EE scratchpad, and IOP RAM aliases.
- Transfers whole 16-byte quadwords, including the final rounded quadword.
- Signals D_STAT channel 6 only after a successful transfer.
- Reports completed IDs through \`sceSifDmaStat\` without inventing a queue.

The native runner connects these through EE syscalls \`0x77\` and
\`0x76\` **only** when an IOP instance was explicitly provided.
Missing IOP state, unsupported attributes, invalid addresses, reboot
requests to destination zero, and unimplemented RPC mailbox destinations
are rejected, not acknowledged.

### Deliberate limitations

The bridge currently completes validated RAM transfers synchronously.
It is not the EE SIF1 hardware DMA chain controller, does not schedule
cycles, and does not execute SIFCMD packets or dispatch RPC calls.
IOP RAM at command-buffer addresses remains ordinary RAM until an IOP
command processor is implemented.

The ordinary launcher still does **not** bring up an IOP BIOS/ROM
image, bootstrap modules, interrupts, SIFCMD, or LOADFILE RPC.
Consequently **this milestone does not claim a real-game boot advance**.

## Next integration checkpoints

1. Model an explicit IOP boot producer from user-supplied legitimate BIOS
   or IOPRP/IRX assets, with kernel scheduling, import resolution, and
   hardware/interrupt services. Avoid publishing readiness until the
   required guest/HLE initialization milestones have actually occurred.
2. Handle SIF1 packet destinations and guest SIFCMD command-buffer
   consumption, including the EE-to-IOP doorbell/interrupt signal.
3. Add IOP-to-EE command replies via the existing SIF0 guest DMA
   transport and verified wake/acknowledgement transitions.
4. Implement LOADFILE server \`0x80000006\` via real SIF RPC requests,
   forwarding resolved module paths through the existing IRX loader.
5. Replay \`scripts\run_sdbz_native.bat --no-recompile\` with the real US ELF,
   saving the boot trace and reporting the next observed guest PC.

Reference: \`ps2-web-recomp\` branch \`SDBZ\`, especially
\`runtime/src/iop/sif.cpp\`. Its initial \`SMFLAG=0x60000\`
is not copied into this project.
