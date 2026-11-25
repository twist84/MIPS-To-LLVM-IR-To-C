# SDBZ US: guest SIF LOADFILE RPC binding and module-start transport

## Scope

The native runner now constructs a bounded IOP LOADFILE RPC service **only**
when an explicit guest IOP module is running. The actual game run
without `--iop-irx` still cannot perform IOP initialization, publish
SMFLAG, or reach the first RPC. No ELF candidate IRX is loaded
automatically at startup.

Once IOP guest code has actually invoked `sceSifInitCmd`, the EE
`sceSifSetDma` HLE can transfer an RPC packet to IOP command buffer
`0x0001E000`, and may transfer the parameter payload to the special
`0xDEADBEF0` SIF data destination.

The implemented SIFCMD RPC subset recognizes:

- `0x80000009` SIF RPC bind for **LOADFILE server `0x80000006`**.
- `0x8000000A` SIF RPC call to LOADFILE method 0.
- Method 0 requires a bounded send payload with the guest pathname
  at offset 8, and a real EE receive buffer for the eight-byte result.
- Nonzero IOP module argument length is deliberately rejected until
  argument propagation is supported. Unknown servers and methods
  also fail closed.

For a valid call, the guest-provided `cdrom0:` pathname passes into
`iop::c_disc_loader`. That loader opens the actual file under the
extracted disc root, resolves imports, creates the IRX's guest R3000
thread, and starts it. The RPC server returns the created module
identifier or a negative error, and queues a 48-byte SIF reply.

The reply is not injected into the EE until guest SIFCMD registered
an EE receive address via system command `0x80000002` **and** the EE
has armed SIF0 channel 5. The reply is written into the EE receive
buffer and raises the channel-5 D_STAT event. A second reply cannot
overwrite the first while channel 5 remains unacknowledged.

## Validated properties

- Synthetic R3000 guest code must execute `sceSifInitCmd` before
  the LOADFILE endpoint accepts traffic.
- Full SIF1 DMA descriptor validation runs before side effects.
- RPC bind and call receive the original guest identity fields.
- A distinct SIF1 data transfer supplies the original call payload.
- Invalid/unsupported packets, missing payload and unknown servers
  are rejected without partial IOP RAM changes or module execution.
- Successful IRX startup schedules an actual guest R3000 thread,
  which is executed by the regression harness.
- Missing disc modules return an error instead of success.
- No path synthesizes IOP SMFLAG readiness.

## Known limitations

This is a **bounded HLE LOADFILE subset**, not a BIOS, complete
SIF RPC implementation, or timing-accurate IOP runtime. A successful
module-start request is currently acknowledged once its guest thread
is queued, not after a fully modelled module-start callback. Full
argument passing, other LOADFILE methods, bidirectional SIFCMD
callbacks, SIF0 chain-tag details, correct hardware scheduling,
automatic IOP power-on and real firmware services are still required.

The most recent actual SDBZ US boot stopped at EE `0x001750B4`,
6,945 dispatches, waiting for an IOP-produced SMFLAG bit.
Do not claim a further game boot until a run with authentic IOP
startup demonstrates it.


## 2026-11-18 13:05: reboot metadata and LOADFILE/IOPHEAP service batch

The matched retail replay reached 6,960 dispatches, accepted SIF_CMD_RESET_CMD, and issued a 64-byte SIFRPC CALL to LOADFILE server 0x80000006, method 0xFF, with zero send bytes and four receive bytes. The method queries the actual LOADFILE version, which is now extracted from the selected on-disc IOPRP image's PsIIloadfile marker, not inferred from its filename or a default. A missing marker remains unsupported. Reset invalidates previous version metadata. The real result is delivered through armed EE SIF0.

IOPHEAP method 3 now copies actual mounted file bytes into a live, sufficiently large IOPHEAP allocation. Missing disc files return -2; invalid destinations, truncated paths, unknown methods and unbacked bytes fail closed. Regression covers the retail packet, version provenance, reset invalidation, file copies, failure replies and heap bounds. LOADFILE buffer method 6, module lookup method 9, nonzero module arguments, asynchronous completion and firmware IRX execution remain unimplemented. SetSyscall 0x12C remains unsupported.


## 2026-11-18 13:24: accepted RPC tracing and module scheduling

The matching local run has 100,000 dispatches and 3,871,691 guest instruction attempts (the configured dispatch cap), with four new reachable EE functions and no unsupported RPC stop. The trace repeats guest PCs 0x0018E0A0 and 0x001938E0 between successive SIF1 request/reply cycles. The static disc catalog still shows runtime_load_count=0 but is **not wired to runtime LOADFILE**, so that number is not evidence of zero actual loads.

The HLE bridge now retains a side-effect-free diagnostic snapshot of **committed** RPC operations (CID, server, method, client, sizes, return word, path, LOADFILE backend/link status, module and thread IDs). The runner logs the actual service results, collapsing identical requests unless --trace was requested. After a successful real IRX LOADFILE startup it schedules a bounded guest R3000 slice instead of leaving its thread ready forever when EE WaitSema never blocks; non-idle/non-budget stop reasons remain explicit hard failures. No fabricated module starts, completed RPCs or IOP readiness are introduced. The integrated regression asserts real version RPC and successful/missing LOADFILE diagnostics, and verifies the real IRX entrypoint executes on the guest CPU.


A real R3000 module may require more than one bounded IOP quantum. The native HLE runner now gives the IOP scheduler a bounded turn after each accepted SIFRPC transport, not only on the initial LOADFILE success; ordinary guest threads resume across calls. Invalid/unhandled guest instructions and syscalls still stop with their actual PC/opcode. A regression fixture executes a NOP in the first limited scheduler slice and reaches its breakpoint only on the later turn. No invented successful module load is implied.

The guest transport scheduler only receives bounded turns when `c_kernel::has_guest_work()` finds a ready R3000 thread or a time-blocked guest thread. No modules, dormant threads, hard-faulted threads, and untimed waits cannot advance IOP cycles from a spurious HLE SIFRPC poll. Regression checks the transition empty → ready → ready after first quantum → faulted after breakpoint.

## 2026-11-18 13:40 — SIO2MAN real IRX bootstrap and coherent IOP kernel HLE

### Strict local evidence

The matched set `boot(20261118-134052).log`, `expansion(20261118-134052).log`, `first-boot(20261118-134051).json`, `iop-disc-catalog(20261118-134052).json`, and `translation-cache(20261118-134051).json` has the same ELF SHA-256 `1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`. The final expansion replay and JSON report agree: 100,000 EE dispatches, 3,871,691 guest instruction attempts, final EE PC `0x0018E0A0`, and 304 translated functions. The modeled HLE IOP reboot and actual LOADFILE version RPC (method `0xFF`, reply bytes `3000`) complete.

Next the EE repeatedly attempts `cdrom0:\SIO2MAN.IRX;1` through LOADFILE method 0, 512-byte send and 8-byte reply. Every captured attempt returned `0xFFFFFFFF`, load status `link_failed` (7) and linker status `invalid_module` (3), with module=0 and thread=0. This is neither an uncompiled EE PC nor an `unresolved_import` (5) response.

### Real original disc metadata and linker fixes

The *original* USA SIO2MAN.IRX from the user-provided disc has an IOPMOD entry offset `0x634`, GP offset `0x8FB0`, PT_LOAD memory `0x0FF0`; similar original modules place GP beyond the copied module image. The prior importer wrongly checked `gp_offset < PT_LOAD/IOPMOD total`. GP is a MIPS signed-small-data base anchor and can legitimately lie outside the loaded segment; the loader now validates that `base + gp_offset` is in physical IOP RAM. The entrypoint still must lie in the module and be aligned. Synthetic tests prove both accepted out-of-image GP and rejection of unaddressable GP, without redistributing original IRX bytes.

The original SIO2MAN export library `sio2man` is sparse: ordinal 4 and 27 are holes, while later ordinals are valid. Import resolution must retain later entries; the export collector and loadcore's real registered-export table checks now recognize a zero hole only if the immediately following entry is a valid, matching executable export, otherwise terminate. A separate synthetic sparse-export regression preserves ordinal identity and prevents invented gap exports.

Import failures preserve library and ordinal from the decoded guest import table throughout the linker, disc loader, SIFRPC observation, and normal boot log as `unresolved=LIB#ORD`. This disambiguates absent kernel providers from malformed IRX headers. A rejected import cannot commit IOP RAM, a module ID, a thread, an EE RPC success, or artificial SIF readiness.

### Coherent service-family HLE, not an IRX success bypass

The original SIO2MAN import set spans eight libraries: `loadcore` (6), `intrman` (4/5/6/7/17/18), `stdio` (4), `dmacman` (28/32/33/34/35), `thbase` (4/6/20), `thevent` (4/6/7/8/10), `thsemap` (4/6/8/11), `sysclib` (8/29/36). The new `c_kernel_hle_bridge` provides explicitly registered R3000 guest-callable stubs for this whole verified family and shares the actual IOP kernel state, memory and thread scheduler. It reinstalls after HLE reset but never in the ROM0 firmware mode. Unknown imports remain unresolved.

Handlers use real guest memory, thread creation/start/ID, event bits and blocking/wakeup, semaphore queues/count/status, registered kernel interrupt mask/state, bounded IOP string/classification/number functions and bounded stdio debug output. SIO2 DMA11/12 requests are represented in actual guest-readable IOP MMIO banks (with persistent STR and PCR control), **without fabricating a DMA data transfer, device reply, completion IRQ or controller readiness**. Unknown/unsupported hardware behavior remains fail-closed. The unrelated SIF DMA regression's old expectation that `0x1F801540` was unsupported is corrected to test the actual unassigned gap at `0x1F80154C`.

An independent **synthetic** complete SIO2MAN-import fixture verifies all 28 imported ordinals, successful guest-linkage, real R3000 syscall execution, string and object operations, DMA register requests without completion, reset reinstall and absence of unimplemented exports. It contains no original game code or assets. The test suite now has **99** CTest cases. Running the updated retail game is necessary before claiming that the real SIO2MAN, PADMAN or any later IRX executes successfully.

### Limitations and next evidence

This batch does not implement actual SIO2 packet/device transfer, interrupt callback invocation, real firmware ROM0/UDNL execution, full LOADFILE async lifecycle, or all PS2 kernel library ordinals. `SetSyscall(0x12C)` also remains a separate EE HLE omission. Unknown kernel imports and unsupported DMA behavior are explicit blockers. After a `git pull` and `scripts\run_ps2_game.bat`, use the next *matched five-file* diagnostic batch to verify actual runtime module ID/thread ID, exact unresolved `LIB#ORD` or guest syscall stop PC/opcode, and whether the 100,000-dispatch loop is broken.

## 2026-11-18 14:10 — first genuine SIO2MAN R3000 stop

The matched five-file batch (boot, expansion, first-boot, catalog and cache
at 14:10:39–14:10:41) uses original US ELF SHA-256
`1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`.
LOADFILE #13 successfully loads and links SIO2MAN.IRX (module 1, thread
1, no unresolved import); 49 guest R3000 instructions execute before
stop=4 (memory_fault), PC 0x00020008, opcode 0xAC440000 (sw a0,0(v0)).
EE stops at 7,006 dispatches / 951,429 guest instruction attempts,
EPC 0x00175064. Ignored SetSyscall(0x12C) is a separate observation,
not a demonstrated cause of the IOP memory fault.

The actual HLE-first R3000 guest CPU had not attached its physical MMIO
bus; only ROM0 startup did so. This patch binds the real existing IOP
MMIO bus at HLE bootstrap and after IOP reboot, with synthetic R3000
SW/LW register-access, clean reset and ROM0 isolation tests. Fault
diagnostics now identify the actual guest memory address and register
state. No SIO2 reply, DMA completion, IRQ or later successful game
boot is invented. The next local five-file run remains authoritative.

## SIO2 configuration-register family

`ps2-web-recomp` implements SIO2 SEND3 (0x1F808200–23F),
per-port CTRL1/CTRL2 (0x1F808240–25F), FIFO and control/status
(0x1F808260–274). The native HLE model now accepts bounded guest
writes/reads of configuration registers and queued TX bytes through
the real IOP bus; CTRL reset clears queued work, CTRL start records a
pending transfer. The RX FIFO still rejects reads without a genuine
controller or memory-card producer, and STAT does not claim a completed
transaction. Tests cover register bounds, pending TX data, reset, lack
of IRQ and unproduced replies. This remains an unverified next-boot
implementation batch.
