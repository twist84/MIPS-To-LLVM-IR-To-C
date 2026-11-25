# SDBZ USA IOP HLE — ps2-web-recomp parity audit

> **Record status (2026-11-25):** Pinned IOP HLE parity investigation; the initial ordinal counts are not a fresh count of the merged runtime. See [current status](README.md), [retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md) and [accuracy plan](ACCURACY_PLAN.md). Any branch, PR, pending replay or feature-status claims below refer to the documented stage.


## Baseline and scope

Main architectural reference: [twist84/ps2-web-recomp, `SDBZ` branch](https://github.com/twist84/ps2-web-recomp/tree/SDBZ), specifically [`runtime/src/iop/libs.cpp`](https://github.com/twist84/ps2-web-recomp/blob/SDBZ/runtime/src/iop/libs.cpp), blob `061b8a092688660005764d9d45d9fb1f348546df`.

Current native LLVM-IR implementation: [`src/iop/iop_kernel_hle_bridge.cpp`](../src/iop/iop_kernel_hle_bridge.cpp); the baseline audited blob was `623875f9bacb21633a539ce2529570b81132c474`.

The reference file defines **172 explicitly named `def("library", ordinal, ...)` handlers**. The HLE-first runtime now installs **196 guest-callable export ordinals** across
the kernel bridge (154), CDVDMAN bridge (14), SIFCMD bridge (15) and
SIFMAN bridge (13). **137** of the kernel bridge exports match explicitly
named `libs.cpp` reference definitions. The earlier 152 count omitted
seven pre-existing registrations in separate device/SIF bridges. Counts include no claim about full behavioral equivalence. The reference also uses loops/dynamic registrations (e.g. `secrman#4/#5`) and separate IOP modules (`io.cpp`, `sif.cpp`, `cdvd.cpp`, `spu2.cpp`) and EE kernel syscalls. The full reference runtime is larger than these 172 direct definitions.

Never bulk-register an unimplemented opcode. Each `k_exports` entry installs a **real executable import stub**, so an unimplemented handler must remain unresolved instead of returning fabricated success.

## Explicit `libs.cpp` IOP service comparison

| Service library | Registered / reference explicit | Missing from this bridge |
|---|---:|---|
| `sysmem` | 8/8 | — |
| `loadcore` | 4/6 | `12 QueryBootMode`, `27 SetRebootTimeLibraryHandlingMode` |
| `intrman` | 9/9 | — |
| `thbase` | 25/37 | `12 DisableDispatchThread`, `13 EnableDispatchThread`, `16 RotateThreadReadyQueue`, `17 iRotateThreadReadyQueue`, `18 ReleaseWaitThread`, `19 iReleaseWaitThread`, `21 CheckThreadStack`, `29 SuspendThread`, `31 ResumeThread`, `36 iSetAlarm`, `38 iCancelAlarm`, `41 GetSystemStatusFlag` |
| `thsemap` | 8/8 | — |
| `thevent` | 11/11 | — |
| `thmsgbx` | 6/6 | — |
| `thfpool` | 6/6 | — |
| `thvpool` | 8/8 | — |
| `sysclib` | 29/31 | `4 setjmp`, `5 longjmp` |
| `stdio` | 3/3 | — |
| `modload` | 6/8 | `7 LoadStartModule`, `8 StartModule` |
| `vblank` | 6/6 | — |
| `timrman` | 0/13 | `4 AllocHardTimer`, `5 ReferHardTimer`, `6 FreeHardTimer`, `7 SetTimerMode`, `8 GetTimerStatus`, `9 SetTimerCounter`, `10 GetTimerCounter`, `11 SetTimerCompare`, `16 GetHardTimerIntrCode`, `20 SetTimerHandler`, `22 SetupHardTimer`, `23 StartHardTimer`, `24 StopHardTimer` |
| `dmacman` | 5/7 | `16 dmac_set_dpcr2`, `17 dmac_get_dpcr2` |
| `heaplib` | 4/4 | — |
| `secrman` | 1/1 | — (explicit `#4/#5` loop definitions are safe failures) |

## Implemented this Windows-log-driven batch

- Guest-thread scheduling: `thbase` thread creation/start/termination/deletion, `StartThreadArgs`, `ExitDeleteThread`, modeled sleep/delay/wakeup counts, priority update, guest-readable thread status, virtual system time, and guest timer callbacks.
- Synchronization: real stateful `thevent` and `thsemap` create/delete/wait/poll/status paths, waiter wakeups, bounded guest-memory output.
- Message/pools: guest-owned mailbox queues, `heaplib` allocations, fixed and variable pool block ownership, allocation/poll/free/status paths, and delayed waiters.
- Memory/string services: `sysmem` actual allocation ownership and size queries; bounded `sysclib` parsing, tokenization, memory/word operations; stdio guest debug output.
- Interrupt/cache state: IOP interrupt mask and coherent direct RAM cache-flush operations; neither synthesizes a DMA/IRQ completion.

Tests cover the new imports through guest R3000 stubs, plus conservative rejection of unknown library/ordinal pairs. Existing EE/IOP hardware and kernel bridges remain separate.

## Stopgap implementations to avoid

Some reference HLE methods return hard-coded successful values or simulate timing without calibrated hardware. These are not evidence of a complete guest-visible service. In this project, keep unsupported hardware operations fail-closed: do not invent `secrman` card authentication, DMA transfer completion, vblank events, actual IOP TIMER interrupts, `modload` success, SIO2 controller responses or textured GS output.

## Next coherent parity batches

1. **IOP module lifecycle**: `modload` guest module registry/load/start/wait/search, exact LOADFILE/R3000 interactions; review installed exports before missing imports in `MCMAN/MCSERV/PADMAN`.
2. **Synchronization and PS2SDK system**: remaining `thbase` priority/dispatch/release-wait variants, `sysclib` nonlocal jump and extended formatted-output parity; rigorously test guest CPU control flow.
3. **Timing**: `timrman`, IOP vblank, interrupt-context alarm callbacks, and EE/IOP clock calibration. Current timer scheduling remains an explicitly modeled guest-instruction clock, not hardware-certified timing.
4. **Device services**: `secrman`, `ioman`, `libsd`, `cdvdman`, `sifman/sifcmd`, `sio2man`; reconcile existing runtime bridges and actual IRX exports instead of assuming all belong to `k_exports`.

The 2026-11-19 22:41 Windows evidence had a final committed `MCMAN.IRX` unresolved import `thbase#33` after the prior `sysclib#12` blocker cleared, with 14,385 final EE dispatches. That old log **does not validate any HLE changes made afterwards**. Newly matched Windows logs must confirm `MCMAN` linking and its entrypoint behavior. CLI compilation progress is never evidence of guest boot success.

## Acceptance protocol

For each batch: correct guest ABI and pointer bounds; tested state mutations; unknown import failure preserved; IRX loader atomic on rejection; complete CI at exact new branch HEAD; only then a matched Windows replay can establish actual game progress. CI green alone never establishes playable boot.

## 2026-11-19 23:30 — MODLOAD guest registry and buffer batch

The matched Windows replay again stopped at **14,385 dispatches**, with a
committed LOADFILE request for `MCMAN.IRX` failing on
`modload#13` (the preceding `thbase#33` dependency was cleared).
The diagnostic run has zero newly discovered functions, and all three
GS snapshots are black.

Added six R3000-callable `modload` exports, based on
`ps2-web-recomp/runtime/src/iop/libs.cpp`:

- `#13 SetCheckKelfPathCallback`: records a validated guest callback
  pointer, but **does not verify KELF files, execute callbacks, or
  authorize authentication**. Unsupported KELF loading remains rejected.
- `#10 LoadModuleBuffer`: resolves guest ELF/IRX bytes into the
  existing transactional IOP linker and publishes a real module ID
  only after successful linking. Does not start the guest module.
- `#16 GetModuleIdList`, `#18 GetModuleIdListByName`,
  `#17 ReferModuleStatus` and `#22 SearchModuleByName`:
  report only actual linked modules, with bounded guest output buffers,
  case-insensitive name search, and unknown-module errors.

`#7 LoadStartModule` and `#8 StartModule` remain deliberately
unregistered: guest module start/wait completion and caller unblock
must be implemented coherently before exposing them. This batch does
not claim that MCMAN's entrypoint runs or that EE execution progresses.
The new HLE implementations are exercised through R3000 import stubs.

The preceding 48 explicit reference gaps are reduced to **42**
by adding six implementations. The new Windows boot must validate
`MCMAN.IRX` and any subsequent imports; static/cached IRX data
are not proof of successful module execution.

## 2026-11-19 23:41 — IOMAN CD-ROM + device/security boundary batch

The matched ZIP contains five artifacts and three black PPM captures.
Final replay is **14,385 dispatches**, with zero newly discovered IR
functions. `SIO2MAN.IRX` started; `MCMAN.IRX` failed on
`ioman#20` (the preceding `modload#13` dependency cleared).
The source ELF SHA-256 and disc root match the previous run.

Using `ps2-web-recomp/runtime/src/iop/io.cpp` and the PS2SDK
`ioman.h` ABI, the native bridge now provides a read-only
`c_folder_sector_source`-backed `ioman` subset:

- `#4 open`, `#5 close`, `#6 read`, `#8 lseek`:
  per-fd position/ownership, sector-aligned reads, bounded IOP RAM,
  correct failure on invalid fds and unsupported writes.
- `#7 write`: only TTY stdout/stderr via the existing debug console;
  CD-ROM writes and unknown devices are rejected.
- `#16 getstat`: actual on-disc file mode/size in the bounded
  40-byte guest structure.
- `#20 AddDrv`, `#21 DelDrv`: **documented failed HLE boundary**.
  The reference itself returns `-1` for these operations, and
  the native runtime does not synchronously execute guest
  device-init/deinit callbacks. Driver registration or memory-card
  readiness is never fabricated.
- `secrman#4/#5/#6`: explicitly *unsuccessful* security boundaries;
  SecrAuthCard returns false for physical or absent cards. The file-backed
  slot 1 card completes its emulated handshake after a SIO2 reset-auth.

Only these 11 new ordinals were registered. Other unsupported IOMAN
operations remain unresolved. The real CD-ROM source is attached only
for the existing validated extracted-disc boot mode, and all
IO descriptors reset on IOP reboot. Guest R3000 tests exercise file
open/read/seek/stat/close, invalid operations, rejected driver
registration and denied security calls.

The explicit `libs.cpp` count becomes **131/172**, with **41**
remaining gaps; the `ioman` reference lives in `io.cpp` and is not
part of the 172 explicitly named `libs.cpp` handlers.

**Next Windows validation:** MCMAN link/start outcome, subsequent
imports, EE dispatches beyond 14,385, and authentic GS framebuffer.
CI passing alone does not establish this.

## 2026-11-19 — CDVDMAN clock and mounted-disc state batch

The next matched Windows ZIP again stopped at **14,385 EE dispatches**,
with one successful `SIO2MAN.IRX` load/start and a new rejected
`MCMAN.IRX` link due to **`cdvdman#24`**. The preceding
`ioman#20` import was cleared. The final replay generated zero new
EE function modules; its three GS images were all black.

The existing extracted-disc-only `c_cdvdman_bridge` now includes
guest R3000 syscall stubs for `#4 sceCdInit`, `#8 sceCdGetError`,
`#12 sceCdGetDiskType`, `#13 sceCdDiskReady`,
`#24 sceCdReadClock`, `#28 sceCdStatus`, in addition to
existing `#10 sceCdSearchFile`. These are grounded in the validated
mounted virtual DVD and (for the RTC) host UTC calendar converted
to the reference's JST BCD layout. The 8-byte clock output is
guest-memory checked before writing.

The subset does not implement CDVDMAN asynchronous reads,
real seek timing, callbacks, or interrupt completion, and does not
register those unsupported imports. The mounted-media ready status
reflects only the virtual disc backend, not a physical PS2 drive.
Guest R3000 tests verify CDVDMAN clock imports and media queries,
unknown ordinal rejection, invalid memory rejection and original
search-file semantics.

The **172-handler libs.cpp reference total is unchanged**: these
CDVDMAN handlers live in `runtime/src/iop/cdvd.cpp`. The native
HLE ordinal total increases from **146 to 152**. The 41 explicit
`libs.cpp` parity gaps remain separate. The next matched real
Windows boot must confirm if MCMAN links/starts and whether EE
execution or framebuffer contents progress.

## 2026-11-20 — MCSERV SIFMAN initialization / mailbox batch

Matched archive `ps2-game-hle(2).zip` (five diagnostic files plus three
black PPMs) uses the same USA `SLUS_214.42` SHA-256 and disc root as
the previous Windows run. **Final replay 14,523 EE dispatches (+138)**
and zero new EE IR functions, with 3,524 cached IR files.
`SIO2MAN.IRX` linked/started (module 1/thread 1), then
`MCMAN.IRX` linked/started (module 2/thread 3), and an authentic
`MCSERV.IRX` LOADFILE request failed on **`sifman#5`**,
`sceSifInit`. The failing MCSERV request had no module ID or
entrypoint thread. The EE ended via `KExit` at EPC `0x001748C4`;
all GS display, upload and native WebGPU snapshots are black.

Using `ps2-web-recomp/runtime/src/iop/sif.cpp` as primary reference
and cross-checking PS2SDK `iop/system/sifman/src/sifman.c`, the
existing SIFMAN bridge now registers **13 ordinals**:
`#4 sceSifDma2Init`, `#5 sceSifInit`, `#6 sceSifSetDChain`,
`#7 sceSifSetDma`, `#8 sceSifDmaStat`,
`#21/#22 Get/SetMSFlag`, `#23/#24 Get/SetSMFlag`,
`#25/#26 GetMainAddr/GetSubAddr`, `#27 SetSubAddr`,
`#29 sceSifCheckInit`.

`#4/#5` configure actual guest-visible IOP DMA priority and channel
control registers, with reinitialization state owned by the SIFMAN
bridge. `#6` initializes the SIF1 DMA chain's block-count register
but **does not start a phantom DMA transfer**. `#8` reports no
in-flight DMA for the bridge's synchronous `#7` transfer model.
`#29` reports whether guest `sceSifInit` actually executed.
Flag and address operations use the shared EE/IOP SIF mailbox model,
preserve producer/acknowledger ownership, and validate guest
subaddresses. Installed import stubs do not set SMFLAG, fabricate
BOOTEND or complete an RPC. SIFMAN interrupt handlers and asynchronous
transfer callbacks remain unresolved.

Tests exercise genuine R3000 guest `#5` execution, IOP DMA register
effects, reinitialization/reset semantics, mailbox register ownership,
SIF status and deliberate failures on unimplemented imports and
invalid guest pointers.

**Next real boot:** determine whether `MCSERV.IRX` now links, and
diagnose any subsequent `sifcmd` RPC or guest `mcman` dependency
rather than bulk registering stubs. The preceding +138 dispatches
do not prove video output or fully initialized memory-card hardware.

## 2026-11-20 — MCSERV guest SIFCMD RPC server batch

The next matched Windows archive, `ps2-game-hle(3).zip`, retains the
correct USA ELF SHA-256 and disc folder. It contains one final replay
with **14,523 EE dispatches** (no change from the previous batch),
3,524 cached IR modules, and **zero newly generated EE functions**.
Two LOADFILE operations succeeded and started guest R3000 module threads:
`SIO2MAN.IRX` (module 1/thread 1) and `MCMAN.IRX` (module 2/thread 3).
The real `MCSERV.IRX` attempt failed linking on `sifcmd#14`
(`sceSifInitRpc`), rather than the previous `sifman#5`.
All three GS diagnostic images remain completely black; terminal
EE syscall is still `KExit` at EPC `0x001748C4`.

Based on `ps2-web-recomp/runtime/src/iop/sif.cpp`, the IOP SIFCMD
bridge now registers a **coherent eight-ordinal guest RPC family**:

- `#14 sceSifInitRpc`: idempotent guest-side RPC initialization
  after SIFCMD is actually initialized, never by registering imports;
- `#17 sceSifRegisterRpc`, `#19 sceSifSetRpcQueue`,
  `#24 sceSifRemoveRpc`, `#25 sceSifRemoveRpcQueue`: validated
  guest server/function/buffer/queue ownership and lifecycle;
- `#22 sceSifRpcLoop`: the real IOP guest thread blocks pending
  requests, wakes only on an actual accepted EE SIF1 RPC call, runs
  its registered R3000 callback, and returns through a dedicated
  completion trampoline;
- `#18 sceSifCheckStatRpc`: reads actual client-busy status from
  validated IOP memory;
- `#23 sceSifGetOtherData`: bounded EE RAM to IOP RAM copy with
  explicit backend and address validation.

The existing SIFRPC bridge recognizes **only registered guest servers**.
An incoming EE bind can be accepted after the guest server exists;
incoming calls are validated and staged, and **no EE output or SIF0
reply is produced until the guest handler executes**. The genuine
R3000 callback's return pointer determines reply data, with bounded
IOP and EE RAM, and the SIF0 response uses the pre-existing armed
DMAC transport. All guest server/pending-call state resets on IOP reboot.

A focused regression executes guest R3000 `sceSifInitRpc`, blocks a
real RPC-loop guest thread, supplies an EE SIF1 bind/call, verifies
that no premature reply is sent, checks an actual guest callback's
write to the response buffer, and verifies the outgoing SIF0 reply.
Unimplemented `sifcmd#12/#13/#15/#16/#28` remain unregistered:
real command-send, IOP-to-EE client RPC, and callback interrupt
semantics must not be fabricated. The current MCSERV static
`sifcmd` import set (`#14/#17/#19/#22/#23/#24/#25`) is now
registered, but **only a subsequent matched Windows boot** can prove
that MCSERV links and its guest thread starts or runs correctly.

The native HLE ordinal total is **177**, including bridge exports
defined outside `libs.cpp`. The explicit `libs.cpp` match count
remains **131/172**.

## 2026-11-20 — LOADFILE module startup argument ABI after LIBSD link

Matched Windows archive `ps2-game-hle(4).zip` has all five expected
diagnostics and three black GS PPMs, with the unchanged USA
`SLUS_214.42` SHA-256 and extracted-disc root. The **final**
committed SIFRPC evidence now shows four successful IRX load/start
attempts: `SIO2MAN.IRX` (module 1/thread 1), `MCMAN.IRX`
(2/3), `MCSERV.IRX` (3/4) and `LIBSD.IRX` (4/5). This
proves the prior `sifcmd#14` linker dependency cleared and four
entrypoint threads were started, **not** that their callbacks or
device initialization completed.

After the fourth module, the next genuine EE SIF1 transaction is
a 512-byte method-0 LOADFILE RPC to server `0x80000006`, with a
64-byte command, `recv_size=8` and record ID 5. It fails closed
as `invalid_arguments`, rather than reporting another IRX load.
The previous parser rejected every nonzero argument length, although
PS2SDK's real `_lf_module_load_arg` defines a bounded
`arg_len` (word 0), 252-byte path at byte 8, and 252-byte
NUL-separated module arguments at byte 260. The raw payload bytes
are not in this log, so the precise argument values must be
confirmed by the next real boot, not inferred from the RPC header.

The loader now validates `arg_len <= 252`, verifies the
complete 512-byte DMA payload for nonempty arguments, ensures
each supplied argument is NUL-terminated, and retains the exact
bytes for guest startup. `c_disc_loader` passes these arguments
to the linked module's genuine guest `_start(argc, argv)` ABI.
`c_kernel` stores `argv[0]` (guest module path), all subsequent
arguments, and a null pointer terminator inside the **thread-owned
bounded IOP stack**, rather than passing host pointers or silently
dropping arguments. This preserves guest-visible argument lifetime
and keeps the guest stack pointer below its initial argv storage.

Regression tests cover the guest argv pointers/strings, a
PS2SDK-shaped 512-byte SIF1 LOADFILE packet with nonempty
NUL-delimited argument list, and rejection of oversized or
unterminated argument lists. Missing/invalid modules still fail
closed; there is no synthetic memory-card initialization or
successful startup result.

The final replay ended at **11,043 EE dispatches**, with
`KExit` EPC `0x00175064`; the expansion pass discovered two
additional EE functions and the translation cache now has **3,526**
IR entries. The lower dispatch count compared to the prior 14,523
is a different stop point, not evidence that four IRX modules
failed to link. GS display, upload and WebGPU images remain black.

Next Windows batch should confirm whether the new argument-bearing
request is accepted, the exact next requested IRX path, actual
IOP guest execution, and whether any new unresolved import,
runtime service or GS producer becomes the next blocker.

## 2026-11-20 00:47 — CRI_ADXI inbound import / reverse SIFRPC transport

The matched `ps2-game-hle(5).zip` contains five diagnostics and
three solid-black GS PPMs. Same USA ELF SHA-256 and mounted disc
root as the last boot, **14,805 final EE dispatches** (previously
11,043), with **zero new discovered EE functions** and 3,526 cached
IR modules. Genuine committed LOADFILE RPC sequence 17 now attempts
`cdrom0:\\CRI_ADXI.IRX;1`: `load_status=7`,
`link_status=5`, `module=thread=0`, first unresolved
import **`sifcmd#16`**. Earlier `SIO2MAN`, `MCMAN`,
`MCSERV` and `LIBSD` remain linked/started. This is module
thread creation evidence, not proof of their successful
initialization. The EE terminates with `KExit` syscall at
`EPC=0x001748C4`; no nonblack GS pixels.

`sifcmd#16` is **`sceSifCallRpc`**, an IOP-originated call
to an EE RPC server, not another EE-to-IOP LOADFILE operation.
The paired `#15 sceSifBindRpc` is implemented as a coherent
reverse-RPC client lifecycle using
`ps2-web-recomp/runtime/src/iop/sif.cpp` and PS2SDK's
`sifrpc-common.h` client-packet ABI:

- Guest `sceSifBindRpc` validates its client descriptor,
  server ID and mode and stages an actual IOP-to-EE 36-byte
  RPC BIND packet. A synchronous guest caller blocks.
  The client is **not bound** until a matching EE Rend arrives
  with nonzero server data and a validated EE buffer.
- Guest `sceSifCallRpc` checks the bound client,
  stacked MIPS O32 arguments, source/receive memory spans,
  method, mode and payload length. It snapshots real IOP
  bytes and stages the original 56-byte CALL packet with
  the client/server identity and EE destination.
- The outbound packets use the existing queued SIF0
  transport: **only an armed EE DMAC channel 5** transfers
  the command and optional payload. There is no fake EE
  server invocation or completion.
- A real incoming 48-byte SIF1 Rend must match the
  pending client pointer, packet/transaction ID and
  BIND/CALL command. Only then are the guest client's
  bound/busy fields updated, and any blocked guest
  thread awakened. A mismatched/stale Rend rejects the
  entire staged SIF1 descriptor list without DMA side
  effects, including data transfers before the packet.
- Asynchronous callback pointers are deliberately
  **rejected**, not silently ignored: scheduling genuine
  IOP R3000 callback execution needs a separate audited
  callback trampoline. Callers using the supported
  blocking or callback-free NOWAIT form retain genuine
  pending/completion semantics.
- The guest-R3000 regression executes actual
  sceSifInitRpc, sceSifBindRpc and sceSifCallRpc import
  stubs, validates SIF0 packet/data emission, a
  blocked caller, forged reply rejection, matching
  EE-to-IOP reply/data DMA and replay rejection.

The native HLE registration count is now **179**.
SIFCMD #15/#16 are defined in reference `sif.cpp`, so
the **131/172** explicit reference `libs.cpp` audit
is unchanged. Only the next matched real Windows boot
can prove whether CRI_ADXI actually links/starts or
uses an unimplemented callback/EE server path. Do not
infer sound output or GS rendering from CI.

## 2026-11-20 — CRI_ADXI SYSCLIB formatted output imports

The next matched Windows archive `ps2-game-hle(6).zip` includes all
five diagnostic files and three all-black GS captures. Its final
`SLUS_214.42` replay matches the expected USA ELF SHA-256 and
disc folder. **14,805 EE dispatches** (no change from the previous
Windows boot), **0 new EE function modules** in expansion, and
**3,526 translation-cache entries**. Four actual LOADFILEs
continue to link and start guest module threads:
`SIO2MAN.IRX` (1/1), `MCMAN.IRX` (2/3),
`MCSERV.IRX` (3/4), and `LIBSD.IRX` (4/5).
Their startup is not proof of completed module initialization.
The genuine `CRI_ADXI.IRX` method-0 LOADFILE attempt (seq 17)
now fails linking on **`sysclib#19 sprintf`**, rather than
the previous `sifcmd#16`. The EE ends at the same
`KExit` syscall, EPC `0x001748C4`, with no nonblack GS pixels.

The reference `ps2-web-recomp/runtime/src/iop/libs.cpp`
implements both `sysclib#19 sprintf` and
`sysclib#42 vsprintf` using a 32-bit guest formatting helper.
The native kernel HLE now registers and executes both in one
coherent formatting batch. This is **real guest output**, not a
return-zero success stub:

- `#19` reads format from IOP RAM, then O32 variable arguments
  beginning at `a2/a3`, with subsequent words at `sp+16`.
  `#42` reads the caller's guest `va_list` through validated
  32-bit IOP RAM accesses.
- Supported C format conversions: signed and unsigned decimal,
  hex/octal, pointer, byte character, bounded guest strings,
  double-precision floating point, escaped percent, widths,
  precision and common formatting flags; `ll` arguments and
  double words use the guest's 64-bit argument alignment.
  PS2SDK's IOP `long` remains 32 bits, regardless of host
  machine ABI.
- Format and each requested argument have hard upper limits.
  Guest format/string/pointer reads and the complete destination
  span are verified **before any output write**. Invalid
  pointers, unsupported conversion patterns, oversized widths
  and dangerous `%n` are refused without changing guest
  destination RAM or faking a formatted string.
- Regression tests execute actual guest-import calls for
  `sprintf` and `vsprintf`, compare NUL-terminated guest
  output and return byte counts, cover 64-bit integer/double
  guest arguments, and verify transactional refusal.

Registration is now **181 native HLE ordinals**, including
**148** in the kernel bridge. Since the added services are
both explicitly defined in the reference `libs.cpp`,
reference matches increase from 131 to **133/172**; the
`sysclib` family is **29/31**. No other device-driver or
EE rendering behavior is claimed.

**Next matched Windows boot is required** before declaring
`CRI_ADXI.IRX` successfully linked or started, since CI
only validates synthetic guest import and ABI fixtures.

## 2026-11-20 — CRI_ADXI VBLANK wait batch (archive 7)

The matched Windows final replay in `ps2-game-hle(7).zip` has 14,805 EE
dispatches, 3,526 translation-cache entries, zero new IR functions,
17 committed LOADFILE requests and four actually started IRX modules.
`CRI_ADXI.IRX` still fails its *link*, but its first unresolved import
has changed from `sysclib#19` to `vblank#5`. The terminal guest
`KExit` EPC remains `0x001748C4`; all three GS PPMs contain zero
nonblack pixels. This is evidence that the previously added SYSCLIB
formatter cleared a link dependency, **not** proof of gameplay,
audio initialization or successful CRI startup.

The reference `ps2-web-recomp/runtime/src/iop/libs.cpp` and PS2SDK
`iop/kernel/include/vblank.h` agree on VBLANK ordinals `#4..#7`.
They now install executable R3000 import stubs with real thread
blocking: `#4/#6` wait for the next VBlank **start**, and `#5/#7`
wait for the next VBlank **end**. A separately delivered matching
edge wakes the original guest thread and returns zero. Opposite edges
do not wake it; no host timeout, synthetic interrupt or fake GPU
present event is used. Wait state is cleared on IOP reboot.

The EE runner forwards **start** only when its scanout scheduler has
actually delivered a field-start event. The current scanout clock
does **not** expose a validated end-edge event, so `#5/#7` will
remain blocked if invoked until that hardware-model event is
implemented. The reference's callback registration `#8/#9` remains
unregistered: no guest callback or interrupt-context completion is
invented. The reference-compatibility EE clock is not hardware cycle
calibration.

Regression tests use actual guest R3000 calls for every wait ordinal,
assert no wake without a matching edge, test start/end disjointness,
and verify the guest resumes and reaches its own breakpoint after
the matching edge. Link eligibility has advanced in synthetic tests;
only a new matched Windows archive can establish actual CRI link
and startup outcomes.

## 2026-11-20 — archive 8: ARKD_DVD CDVDMAN + LIBSD SPU2 SSBUS

Archive `ps2-game-hle(8).zip` contains a matched USA retail final replay,
five JSON/log diagnostic artifacts and three black PPM captures. SHA-256
`1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`
and the mounted disc root both match the baseline.
Final replay: **11,295 dispatches**, 3,529 cache entries, **three new
translated functions** discovered during auto-expansion.
Unlike archive 7, `CRI_ADXI.IRX` really linked and started:
committed LOADFILE request 17 returned module 5/thread 7, zero link error.
Request 18 attempted `ARKD_DVD.IRX` but failed to link with
`cdvdman#5` (`sceCdStandby`); no module or thread was created.
Separately the genuine `LIBSD.IRX` thread 5 faulted on a 32-bit
write to `0xBF801404` (PC `0x00059774`), a SPU2 SSBUS
configuration register. This is not an audio sample or DMA transfer.
The terminal EE condition also changed to exception code 8 at
EPC `0x00175064`; it is not the earlier KExit. GS accepted 37
batches, but all three PPMs contain zero nonblack pixels.

Based on `ps2-web-recomp/runtime/src/iop/cdvd.cpp` and PS2SDK's
`cdvdman.h`, `cdvdman#5 sceCdStandby` is an idempotent
standby request for the already mounted extracted-disc backend.
`cdvdman#11 sceCdSync` returns the idle queue state; it does
not complete physical N-commands, DMA, callbacks or a spindle
transition. These two handlers have executable guest import stubs;
unknown CDVDMAN commands remain unregistered.

Following PS2SDK's `ssbusc.c` and PCSX2's IOP hardware map,
IOP MMIO now retains 32-bit SPU2 SSBUS base-address and
delay registers at `0x1F801404`, `0x1F80140C`,
`0x1F801014` and `0x1F801414`. They reset with the
IOP and reject unsupported widths. They do not expose SPU2
audio RAM, generate sound, launch DMA or assert interrupts.
Regression tests exercise direct guest-KSEG1 CPU access,
readback, rejection of unimplemented MMIO, reset behavior and
the newly linked `sceCdStandby` R3000 import.

**Important:** the LIBSD guest MMIO repair and ARKD_DVD module
link eligibility are synthetic/structural claims only until the next
Windows replay. The lower EE dispatch count is not evidence of
regression by itself; the new path terminates at an entirely
different EE EPC and new IOP guest fault.


## 2026-11-20 — Windows archive 9: CDVDMAN asynchronous read batch

The matched `ps2-game-hle(9).zip` has the expected USA ELF hash
`1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`,
the same extracted-disc root, 15,057 EE dispatches, 3,529 cached IR
functions, and five successfully linked/started guest IOP modules.
LOADFILE request 18 for `ARKD_DVD.IRX` still failed linking, but
its first missing import changed from `cdvdman#5` to `cdvdman#6`.
No ARKD module ID or thread was created; this is a link dependency,
not evidence that a guest read was executed. The EE stopped at
syscall-exception EPC `0x001748C4`. All three PPMs are fully black.

New `sceCdRead` HLE uses the validated folder-sector source and
reference `ps2-web-recomp/runtime/src/iop/cdvd.cpp` command state.
Only actual mounted 2048-byte sectors with a fully validated IOP
guest-RAM output region are accepted. Actual disc data is staged
at request time, then committed after an explicit virtual IOP
instruction-clock event. No host-time polling, physical CDVD IRQ,
DMA completion, SIF reply or unimplemented guest callback is invented.
The compatibility delay is not calibrated to reference physical
400us + 250us-per-sector disc timing.

`sceCdSync` polling returns the genuine pending state; a nonpolling
wait blocks the caller until the staged transfer is committed.
`sceCdStatus` reports busy during the pending interval. The kernel
advances explicitly scheduled HLE events while waiting and clears
them on reboot. Synthetic guest R3000 tests cover import linking,
the original JAL import call, exact returned mounted-sector bytes,
polling/busy/idle states, invalid-source rejection and zero
fabricated interrupt.

ARKD_DVD additionally imports CDVDMAN `#15/#37/#38/#75`,
`vblank#8` and LIBSD functions. Their runtime behavior remains
unverified; static import inventory alone does not establish it.


## 2026-11-20 — Windows archive 10: complete observed CDVDMAN import set

The matched `ps2-game-hle(10).zip` replay has the expected USA
`SLUS_214.42` SHA-256 and extracted disc root. Its final replay still
reports 15,057 EE dispatches, 3,529 translation-cache entries, five
linked/started IOP modules and 18 committed RPC/LOADFILE operations.
The first unresolved ARKD_DVD import moved from `cdvdman#6` to
`cdvdman#15 sceCdStop`; ARKD_DVD itself remains unlinked, with
no module ID or guest entry thread. The EE still ends with a syscall
exception at EPC `0x001748C4`. All three PPM snapshots are black.

The full ARKD_DVD static CDVDMAN import inventory contains #4, #5,
#6, #8, #10, #12, #13, #15, #28, #37, #38, #75. The only missing
CDVDMAN exports before this change were **#15 sceCdStop,
#37 sceCdCallback, #38 sceCdPause, #75 sceCdMmode**.
`MCMAN.IRX` imports only #24, which is already implemented. These
are the complete observed CDVDMAN import gaps in the archive, not
a statement about unscanned modules in future boot paths.

For extracted, already mounted media, Stop and Pause acknowledge
idempotent spindle requests without mutating the actual queued
file-backed transfer or inventing completion. Mmode stores only
supported auto/DVD selections; a request to switch to absent CD
media is rejected. These calls do not interact with physical CDVD
DMA or pretend that another disc was inserted.

Unlike a fake callback registration stub, sceCdCallback records and
returns the previous validated guest handler pointer, retains its
registering-module GP and dispatches **actual guest R3000 code**
with the CDVD read completion reason after a staged file-backed
read finishes. A one-shot kernel callback thread is entered through
the registered guest function and released when it returns to the
dedicated R3000 breakpoint trampoline. This is a guest-thread HLE
approximation to interrupt-context delivery, **not** proof of
hardware-accurate interrupt timing or physical disc operation.
The guest completion handler is never invoked merely by linking
or by calling Stop/Pause/Mmode.

Regression coverage exercises all 14 executable CDVDMAN import
stubs, actual IRX guest callback registration and replacement,
guest instruction execution and GP preservation on genuine sector
completion, protection against early callbacks, and no fabricated
physical IRQ or N-command completion. Unsupported seek #7 and
other non-catalog hardware commands remain unregistered.

**Next Windows milestone:** Verify whether ARKD_DVD links and starts.
The static inventory also names `vblank#8` and LIBSD dependencies,
which have independent behavior and may become subsequent blockers.
No game-boot or visible-frame claim is justified by import parity
and synthetic CI alone.

## 2026-11-20 — matched Windows 02:26:45 archive: SIFCMD and VBLANK import parity

The matched final replay confirms that the previous four CDVDMAN
import blockers (#15/#37/#38/#75) were resolved. It still has five
started IRX modules, 18 committed RPC operations, 15,057 EE
dispatches and 3,529 translation-cache entries. The next unresolved
ARKD_DVD import is **`sifcmd#12 sceSifSendCmd`**; no ARKD module
or thread was started. The terminal EE exception is unchanged at
EPC `0x001748C4`. PPM captures remain fully black.

The full seven-module static catalog contains exactly four previously
unregistered imports in these two libraries: `sifcmd#12` used by
ARKD_DVD, `sifcmd#28 isceSifSendCmd` (intr variant) used by PADMAN,
and `vblank#8 RegisterVblankHandler`/`#9 ReleaseVblankHandler`
(PADMAN uses both; ARKD uses #8). Other observed imports in those
two libraries already had implementations. Static dependencies
are not proof of runtime handler execution.

The native SIFCMD bridge now stages **actual R3000 guest command
packet bytes**, with the two stack-passed extra-data parameters,
and enqueues them into the existing IOP-to-EE SIF0 DMA queue.
Enqueueing does not synthesize an EE reply or DMAC interrupt; an
actual armed EE SIF0 receiver must accept the packet. Bounded
packet/payload lengths and guest IO/EE memory spans are validated.
The intr-context ordinal uses the same transport and does not
fabricate an IOP interrupt.

The VBLANK bridge now retains actual guest handler pointers and
registration GP per start/end edge, supports removal by
(edge, function), and calls the **real R3000 guest callback**
through a one-shot return trampoline only when the scanout
scheduler delivers the matching edge. No registration or wait
manufactures an edge. The existing EE scanout producer supplies
start edges; end-edge delivery is still unsupported until a
real producer exists. Registration is cleared on IOP reboot.

Regression tests verify the original guest R3000 import stubs,
complete SIFCMD packet plus extra payload traversing an armed
EE SIF0 receiver, atomic failure for invalid guest spans,
VBLANK edge matching, actual callback argument execution,
registration removal and reboot clearing.

**Next Windows replay:** confirm that ARKD_DVD now links and
starts; otherwise diagnose its newly exposed first unresolved
import. No playable boot, sound or rendered image is implied.

## 2026-11-20 02:55 — ARKD guest RPC bind (matched Windows evidence)

Matched archive: `ps2-game-hle(20261120-025514).zip`, USA ELF SHA-256
`1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`.
The final committed replay starts all six modules: SIO2MAN, MCMAN,
MCSERV, LIBSD, CRI_ADXI and ARKD_DVD (module 6/thread 8), with no
unresolved IRX imports. The five earlier modules remain started.
Successful linkage and thread creation do not establish module initialization.

The next genuine EE SIF1 packet is a 64-byte BIND, CID `0x80000009`,
server `0x500`, client `0x005A9330`, IOP destination `0x0001E000`,
rejected as `unknown_server`. The final replay stops after 11,429
EE dispatches, with 3,530 translation-cache hash entries and
syscall exception EPC `0x00175064`. All three GS PPMs are black.
The earlier 15,057 dispatches are a different stop path, not
evidence of a loss of working IOP modules.

The primary `ps2-web-recomp/SDBZ` reference, `runtime/src/iop/sif.cpp`,
defers an EE bind until a guest IOP RPC server actually registers.
Its Arika RPC trace explicitly recognizes server IDs `0x500..0x503`.
The native implementation now stages only those specifically identified
guest-owned server IDs when not yet registered; unrelated unknown
servers and unsupported CALL methods still fail closed.
The bounded pending bind is released only after real guest
`sceSifRegisterRpc` and `sceSifSetRpcQueue` have made a server ready.
No SIF0 Rend, client binding, guest callback, or completion interrupt
is synthesized before that event. Reset clears pending bind state.

The reference has an IOP-clock-based bind timeout. This native bridge
does not yet have a calibrated shared deadline for these requests;
they remain bounded by queue capacity but can wait indefinitely.
The next matched Windows archive must establish whether ARKD_DVD's
guest thread really registers server `0x500` and can service its
subsequent RPC operations; passing synthetic CI is not boot validation.

## 2026-11-20 03:20 — Guest IOP progress while EE waits for ARKD bind

Matched archive: `ps2-game-hle(20261120-032006).zip`. Same USA
`SLUS_214.42` SHA-256 `1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`,
same extracted disc and final committed replay (`replay=0`); no expansion
retry or new function generation. All six LOADFILE modules still link
and start; `ARKD_DVD.IRX` is module 6, initial guest thread 8.

PR #145's deferred `0x500` BIND now commits as SIFRPC sequence **19**
(client `0x005A9330`). Its registered guest server has not yet been
observed. The runtime executes one 256-instruction IOP scheduler turn
after the SIF1 transaction, but this is not proof of server registration;
the diagnostic `thread=0` is an unpopulated result field on
`budget_exhausted`, not evidence that no IOP thread executed.

The EE next traps at a real **WaitSema** (`v1=0x44`, A0 semaphore 8),
not `_SetAlarm` (`v0=0x18` is a stale return register). The runner
refuses the zero-count wait because there are no ready EE peers and its
existing IOP-completion advancement is enabled only for ROM0 boot.
Final replay: **11,434 EE dispatches** (+5), 19 committed RPCs,
terminal EPC `0x00174CE4`, Cause `0x20`. The three GS PPMs are
entirely zero. No new import, unsupported instruction or MMIO fault
is demonstrated by this archive. The translation cache does not report
a new function in the final replay.

**Change:** Extend the existing no-ready-EE-peer WaitSema rendezvous
to the HLE-first path only while a real deferred EE RPC BIND exists
**and** `c_kernel::has_guest_work()` finds ready/timed IOP work.
The IOP executes a bounded guest R3000 quantum; the RPC bridge may
pump only an already-queued SIF0 reply, and the EE still requires the
actual DMAC5 interrupt and original guest iSignalSema to finish
WaitSema. No fabricated server, reply, interrupt, wakeup or virtual
clock completion is permitted. Preserve the ROM0-only path unchanged.

The reference `ps2-web-recomp/SDBZ` drives its IOP kernel from elapsed
EE cycles (see `runtime/src/iop/kernel.cpp::run`), processes guest
thread scheduling and commands while EE services are blocked, and
defers SIFRPC BIND until the guest registers a matching server
(`runtime/src/iop/sif.cpp`). The native runner is still using a
bounded uncalibrated IOP guest-instruction compatibility clock; this
batch is a targeted EE/IOP rendezvous, **not** calibrated full
EE/IOP parallel scheduling.

Regression: no IOP guest thread -> no wait advancement; dormant guest
thread -> no advancement; genuine started R3000 thread -> bounded
execution but no premature Rend; only registration moves the pending
bind into a SIF0 reply, and only an armed SIF0 can deliver it.
Existing standalone EE WaitSema negative tests continue to require
fail-closed when no work or peer exists.

**Next matched Windows validation:** confirm whether the HLE WaitSema
guest turns reach actual ARKD guest registration for server `0x500`,
whether a real DMAC5 callback signals the semaphore, and identify
the next supported or faulting guest instruction. A bounded wait
exhaustion remains a reported blocker, not synthetic success.

## 2026-11-20 03:36:57 — IOP R3000 RAM mirror fault after EE WaitSema rendezvous

Matched archive: `ps2-game-hle(20261120-033657).zip`. Only the
committed `replay=0` is present; `boot.log`, `expansion.log`,
`first-boot.json`, `iop-disc-catalog.json`, `translation-cache.json`
and all three GS PPMs belong to that replay. The ELF SHA-256 is
`1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`.

All six committed IRX loads again link/start; the `ARKD_DVD.IRX`
LOADFILE is sequence 18 (module 6, initial thread 8), followed by
committed 64-byte SIFRPC BIND sequence 19 (server `0x500`, client
`0x005A9330`), pending and without any observed registration or SIF0
reply. The EE stops at `WaitSema(8)`, EPC `0x00174CE4`, Cause `0x20`,
11,434 dispatches. The HLE-first WaitSema rendezvous from draft
PR #146 executes one guest IOP turn (256 R3000 instructions) and
then stops on `e_kernel_stop::memory_fault` (`reason=4`), thread 1,
PC `0x00202080`, opcode `0x00000000`, after another 38 guest
instructions. This is an instruction-fetch failure outside the
native CPU's previous strict 2 MiB RAM window, not an observed
unresolved IRX import or proven failure in ARKD's initial thread.
All three PPM captures have zero nonblack pixels.

### Reference parity and implementation

The primary `ps2-web-recomp/SDBZ` reference uses `ram_ptr(a)` with
`phys(a) & (kRamSize - 1)` and fetches IOP instructions as RAM for
physical addresses below `0x00800000`. PCSX2's IOP memory LUT
similarly maps 2 MiB RAM across the first 8 MiB of physical space
(its first 0x80 64-KiB pages index RAM with `i & 0x1f`).
Consequently the attempted PC `0x00202080` aliases backing offset
`0x00002080`, and native `c_cpu::read`/`write` must preserve that
address translation for aligned instruction fetches and data accesses.

Add a strictly bounded 8 MiB IOP RAM mirror window to `c_cpu`,
retaining the 2 MiB backing store, original guest virtual addresses,
the KSEG0/KSEG1 restrictions, alignment checks, ROM0 and MMIO
routing, and rejection at or above physical `0x00800000` when
unmapped. Leave the existing DMA buffer and kernel allocation
ownership checks conservative; this batch does not license
arbitrary DMA or heap addresses beyond actual backing RAM.

Test executed R3000 code at `0x00202080` against backing RAM
`0x00002080`; check the other mirrors and KSEG aliases, and reject
unmapped addresses, unaligned writes and KSEG2-only virtual
addresses. Preserve all existing hardware/ROM0 tests.
Add the actual fault address/access width and guest RA/GP/SP to
the EE WaitSema IOP-stop diagnostic to identify the next genuine
blocker without assuming the RAM mirror will complete RPC binding.

**Next Windows replay must confirm** whether the guest progresses
through the previous PC, whether server `0x500` registers and replies
via an armed SIF0 receiver, whether EE `WaitSema(8)` wakes through
the original DMA5 callback, and what exact IOP/EE fault follows.
Static module startup and CI tests alone do not establish gameplay.

## 2026-11-20 03:47:27 — IRX guest execution exhausts bounded wait

Matched archive: `ps2-game-hle(20261120-034727).zip` has exactly one
final `replay=0`, zero newly discovered EE functions, and consistent
`boot.log`, `expansion.log`, `first-boot.json`, `iop-disc-catalog.json`,
`translation-cache.json`, and three GS PPM captures. USA game ELF SHA-256
remains `1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`.

The IOP RAM mirror regression has reached past the earlier attempted
fetch at `0x00202080`, without the previous memory fault. Six real
IRX loads link/start; committed SIFRPC sequence 19 queues one BIND
for ARKD server `0x500` (client `0x005A9330`). Guest registration and
an SIF0 reply have **not** been observed.

EE still stops at original `WaitSema(8)`, EPC `0x00174CE4`,
after 11,434 dispatches. The IOP advances the entire configured
2,048-turn limit of 256 guest instructions per turn (524,288 guest
instruction slots during that wait) without a fatal guest fault, yet
`binds=1 replies=0` when exhausted, and the last IOP CPU PC is
`0x0020B920`. All three GS PPMs remain entirely black.

**Do not infer successful IOP initialization from an uninterrupted
instruction count.** The final PC aliases backing IOP RAM offset
`0x0000B920` and the difference from 2,048 × 256 × 4 bytes is exactly
`0x0000B920`. This is consistent with — but does not prove — a
linear NOP walk through mirrored uninitialized RAM. The current
kernel completion result reports `thread=0` on `budget_exhausted`
even though a real thread was chosen. It does not expose whether a
high-priority guest thread starved ARKD's initial thread 8.

**Diagnostic-only changes in the existing draft PR #145:** report
the last actually scheduled IOP thread on a successful bounded turn;
expose immutable `c_kernel::threads()` snapshots; capture per-thread
status/priority/wait type/wake, entry and current PC, guest opcode
in mapped RAM, and whether each PC falls in a linked IRX image.
Log only before the first wait turn and after turns 1, 64, 512 and
the configured bound; additionally capture snapshots on real IOP
faults. Test scheduler reporting, snapshot immutability, and cleanup
after reboot.

The next Windows archive should identify actual scheduled thread
IDs and whether any guest code enters ARKD module 6. Continue to
require real `sceSifRegisterRpc`, armed SIF0 transfer, and original
DMAC5/iSignalSema completion. Do not fabricate a server, inject a
callback, alter EE semaphore count, assume unlimited budget, or
blindly mark the bind successful.

## 2026-11-20 03:57:15 — IRX module-start thread priority and return lifecycle

Matched archive `ps2-game-hle(20261120-035715).zip`: one final replay
(`replay=0`), six linked/started IRX loads, 19 committed RPCs, one
pending server `0x500` bind, 11,434 EE dispatches, and a blocked
`WaitSema(8)` at EPC `0x00174CE4`. The 2,048 IOP wait turns execute
524,288 guest R3000 instruction slots without registration or SIF0
response. All three GS PPM captures remain completely black.

The bounded snapshots prove that several IOP module-start threads are
running at priority **1** after leaving their linked IRX images and
advancing through zero-filled mirrored RAM; their retained `ra=0`
allows the original `JR $ra` to return into address zero and run NOPs.
At turn 2,048 SIO2MAN startup thread 1 has PC `0x0020FA38`, and
ARKD_DVD startup thread 8 has PC `0x0020B920`, both mapped as
`image_module=0` with zero opcodes. ARKD's guest-created worker
threads **9–12** are ready at priority **50** but remain at their
unchanged entrypoints inside module 6; module-start priority 1
starves them. A separate worker thread 13 has priority 39 and also
remains at its entry. These are actual guest-created threads, not
invented RPC services. The log supports a scheduler/lifecycle
starvation diagnosis; it does **not** establish that the ARKD server
would have registered after worker execution.

**Primary reference:** `ps2-web-recomp/SDBZ/runtime/src/iop/kernel.cpp`
uses `kDefaultModulePriority = 8` for `start_module`, calls the guest
`_start(argc, argv)` with `ra=g_exit_stub`, and only when the guest
returns does its exit callback mark that startup thread dormant and
publish the actual module result. Its general `start_thread` also
installs this exit trampoline. Our HLE-first runner instead used
priority 1 and seeded module `ra=0`; the kernel only implemented
special return trampolines for alarms and one-shot callbacks.

**Fix in the existing draft PR #145:** use default priority 8 for
LOADFILE-created startup threads; keep an explicitly supplied
priority unchanged. Tag the module's real initial guest thread and
install a reserved MIPS BREAK return trampoline at `0x00013FD0`
(next to existing alarm/callback return stubs), setting the guest
`ra` whenever that module thread is started, including its
`StartThreadArgs` context reset. Only an actual guest `JR $ra`
followed by execution of the trampoline retires that module-start
thread as dormant while preserving its guest `v0` result.
This does not synthesize a successful module result, bind a server,
or signal an EE semaphore; guest background worker creation and
priority rules continue to come from executed IOP instructions.

Regressions cover a real R3000 `JR $ra` plus delay-slot result,
no further scheduling after a module returns, matching default 8
priority, and preservation of explicit caller-chosen priority.

The reference kernel also seeds `ra=g_exit_stub` for **ordinary**
`StartThread`/`StartThreadArgs` guest threads. ARKD worker threads 9–12
had `ra=0` in the same snapshot and could otherwise repeat the bad
fall-through upon returning. This batch therefore reserves a separate
`0x00013FC0` BREAK trampoline for ordinary guest threads, retains
the preexisting alarm and one-shot callback return trampolines,
and retires a generic worker only after its actual guest MIPS return.
Tests exercise that separate worker path and verify no spurious module
completion or fabricated signal.

**Limitations and next Windows replay:** The present LOADFILE bridge
publishes the module ID before observing the entrypoint's return;
full load-module completion/result timing parity remains a separate
issue. Observe genuine guest startup returns and worker execution,
then `sceSifRegisterRpc` for server `0x500`, any real SIF0 reply,
and EE `WaitSema(8)` DMA5 callback wakeup. Do not claim these steps
from CI alone.

## Windows clean Clang boot — missing runner stack reserve (2026-11-20)

The latest user-provided diagnostic set consists of `boot.log`,
`expansion.log`, `iop-disc-catalog.json`, and
`translation-cache.json` from a clean Windows LLVM `clang++` build.
It is *not* a matched final traced boot: the expansion terminated
on its first non-traced `replay=0`, before the wrapper could execute
its separate final traced replay. Both logs contain the same six
lines, ending in `exit=4294967295` (`0xFFFFFFFF`, -1 signed),
`elapsed=1.34s`, no reached EE dispatch count, and no observed
IOP runtime events. No `first-boot.json` was supplied. The catalog
correctly labels its seven IRX candidates and 279 imports as
`static-only`, with zero runtime load events. The validated
translation cache records only the entry-function IR
`sub_0x100008_0x100008.ll`; no EE expansion was possible after the
runner process quit.

Code inspection identified a concrete build configuration mismatch:
`src/tools/ee_game_runner_main.cpp` holds
`s_kernel_thread threads[256]` on the main function's stack;
each thread stores a full `s_ee_vu0_state` with two 4 KiB
micro/data memories (over 2 MiB for VU0 snapshots alone). However
`CMakeLists.txt` reserved an 8 MiB stack with `if(MSVC)`, which
misses the LLVM `clang++` *GNU-like* driver used for the user's
fresh Ninja build. Windows' default PE stack reserve is much too
small for that frame. This is a proven **configuration defect**,
not yet a proven complete explanation for the numeric `-1` exit.

PR #145 now applies the same 8 MiB stack reserve for all native
Windows compiler drivers through `LINKER:/STACK:8388608`.
The Windows native WebGPU CI job inspects the *linked Clang PE header*
using `dumpbin /headers` and requires an `800000` stack reserve;
it also executes `ee_game_runner.exe --help` as a startup smoke
check. Both MSVC and fresh Clang builds are retained. No EE, IOP,
RPC, guest timing, or shader behavior was altered.

**Next validation:** after new CI passes, re-run the existing
`scripts\run_ps2_game.bat` from the current PR branch and inspect a
matched final traced log bundle, including `first-boot.json`.
Do not attribute ARKD registration or a genuine DMA5/EE semaphore
wake to this host-only fix before observed runtime evidence.

## 2026-11-20 04:56:35 — original LIBSD SPU2 register write

Matched user Windows ZIP `ps2-game-hle(20261120-045635).zip` includes
`boot.log`, `expansion.log`, `first-boot.json`, three GS PPM images,
`iop-disc-catalog.json`, and `translation-cache.json` from the same run.
Fresh Windows LLVM native WebGPU/raylib startup now succeeds. Four
expansion passes added 128, 128, 128, and 35 IR functions, leaving
420 recorded translated IR files. The final trace reached 11,434 EE
dispatches / 16,026,712 reached guest instructions. The EE is still
waiting on semaphore 8 at EPC `0x00174CE4`; the deferred ARKD bind
remains server `0x500`, client `0x005A9330`, with no guest RPC
server registration or real reply.

**Positive lifecycle evidence:** all six actual IRX guest module
`_start` threads now return through the new `0x00013FD0`
trampoline: modules 1–6 return guest `v0` 0,2,2,0,2,0 respectively.
The high-priority module-start starvation from the prior 03:57:15
run is resolved. ARKD module 6 creates four actual worker threads
9–12 at priority 50. A LIBSD-related guest thread 13, priority 39,
runs far enough to issue a real R3000 halfword store at PC
`0x0005C8E8` (`SH`, opcode `0xA4450000`) with virtual address
`0xBF900760`. Its IOP physical address is `0x1F900760`.
The kernel stops with `e_kernel_stop::memory_fault`, address
`0xBF900760`, width 2, write 1. This is an unimplemented SPU2
**master-volume register**, not an 8 MiB IOP RAM alias, opcode
decoder failure, successful audio transfer, or EE translation gap.
The IOP scheduler cannot deliver the pending ARKD bind while this
guest memory fault remains.

**Primary reference:** `ps2-web-recomp/SDBZ/runtime/src/iop/hw.cpp`
routes IOP physical `0x1F900000..0x1F9007FF` to its
`spu2::read/write`. `runtime/src/iop/spu2.cpp` maps each core
through its 0x400-byte window, maps master volume/control through
offsets `0x760` and `0x788`, uses 16-bit little-endian reads
and writes, and exposes the `0x7C0` SPDIF register bank. Core
`0x19A` ATTR, `0x19C` IRQA, `0x1A8` TSA, `0x1AC`
data port and `0x344` STATX require their own state/semantics.

**New PR #145 batch:** add an isolated `iop::c_spu2` register
model routed through `iop::c_hw`; map and reset the original
master/core/SPDIF banks; preserve guest-programmed 16-/32-bit
register values, IRQA/TSA, statx settle and 2 MiB sound-RAM
data-port state, and only assert IRQ9 for a genuine guest-selected
SPU port/IRQA collision. Reject unsupported widths or dynamic
voice envelope/NAX reads once playback is requested. This is an
intermediate **register/port** milestone, not a sound engine.
No voice mixer, sampled audio tick, DMA4/7 completion, generic
arbitrary MMIO ready bit, or ARKD RPC response is synthesized.
A R3000 `SH` test uses the exact original faulting opcode and
address; additional regressions check master readback, core
isolation, STATX settle, sound-RAM port/IRQ semantics, invalid
accesses and reset.

**Next evidence needed:** an authentic Windows replay after
pulling the unchanged PR branch. Revisit the IOP scheduler,
record whether the original SPU2 store now executes, and identify
the next guest SPU2 status/DMA operation or real
`sceSifRegisterRpc(0x500)` call. No game boot success is implied.

## 2026-11-20 05:09:58 — ARKD guest RPC server registered; thsemap #8 error

Matched Windows replay `ps2-game-hle(20261120-050958).zip` contains
`boot.log`, `expansion.log`, `first-boot.json`, the two native
WebGPU/GS diagnostic snapshots plus upload PPM,
`iop-disc-catalog.json` and `translation-cache.json`.
The SPU2 register failure from the preceding 04:56:35 batch has
cleared: the final replay reaches 11,773 EE dispatches, 16,030,642
guest instruction attempts, 421 recorded LLVM IR files, 29 committed
IOP SIF RPC requests and all six original module-start returns.
All three PPM captures remain entirely black.

**Positive milestone**: the genuine ARKD guest worker registers
SIF RPC server `0x00000500`, releasing the one previously deferred
bind for EE client `0x005A9330`. The EE then binds the other
ARKD servers `0x501`, `0x502`, `0x503`, issues method `0x12`,
method `0x11`, and requests method `0x01` on `0x500`.
This is real IOP guest registration and attempted RPC dispatch,
not an invented ready flag or host-owned success response.
No end-to-end method return, DMA5 reply, or game rendering is yet
verified; the final EE exception at `0x80000180` is a stop caused
by the IOP service failure.

**New blocker**: when the guest RPC queue calls method `1`, IOP
thread 9 stops after 81 executed instructions at `0x000147A8`,
opcode `0x0000000C`, `e_kernel_stop::unhandled_syscall = 2`.
The exact HLE syscall stub offset
`0x147A8 - 0x14000 - 8 = 61 * 0x20` identifies export table
index 61, `thsemap` ordinal `8` / `WaitSema`. This is a
*handled, documented IOP library function*; the bridge incorrectly
returned false for an unknown guest semaphore ID, converting a
guest API error into a terminal host syscall failure. Without the
guest `a0` value in this batch, the reason the requested semaphore
ID is missing remains unverified.

**Primary reference**: `ps2-web-recomp/SDBZ/runtime/src/iop/libs.cpp`
defines unknown semaphore `-408`, unknown event flag `-409`,
unknown mailbox `-410`, unknown variable pool `-411`, and unknown
fixed pool `-412`. These errors are genuine negative guest return
values, *not* successful waits or fabricated kernel objects.
The original reference `WaitSema` explicitly returns `-408`
when its ID does not exist.

**PR #145 implementation**: the five corresponding guest HLE
kernel families return reference errors for *known imported
operations* targeting missing IDs, while unsupported ordinals and
unexpected internal failures remain fail-closed. Invalid
`thsemap` #8 requests emit a once-per-unknown-ID diagnostic
containing requested ID, guest thread, PC, RA and currently
registered semaphore counts/IDs. The unknown-ID diagnostic set
is cleared on IOP reboot. Regression tests execute real R3000
HLE import stubs after deleting a known semaphore/event and
check all five negative error code families; unknown IDs do
not allocate, wake, signal or fake a completion.

**Next matched replay**: inspect `IOP HLE WaitSema unknown id`
and the actual guest response after `-408`; determine whether
the RPC method handles it or needs further authentic producer/
allocator investigation. Keep SIF0/DMA5 and guest RPC completion
evidence separate from successful server registration.

## 2026-11-20 05:21:52 — ARKD method 1 survives unknown WaitSema; IOP scheduling gap

Matched replay `ps2-game-hle(20261120-052152).zip` contains boot and expansion
logs, first-boot report, disc catalog, translation cache and all three GS
snapshots. The native runner reuses 421 cached LLVM functions, reaches the
100,000 EE dispatch ceiling (16,912,906 EE instruction attempts), and
commits the same 29 SIF RPC commands. Six original IRX start threads and
ARKD server registrations `0x500..0x503` remain established. All three
images are black. This is not a successful game boot.

The prior fatal `thsemap` #8 at IOP PC `0x000147A8` is now handled as the
reference kernel error `-408`; its requested ID is **zero**, not an
allocated semaphore. The diagnostic reports IOP thread 9, RA `0x0007D648`,
and existing semaphore IDs `2..8`. Do not invent ID zero or imply its
producer has been found. Guest methods `0x12` and `0x11` reach idle after
65 and 60 R3000 instructions respectively and have subsequent observed
EE DMA5 callbacks. Method `0x01` instead exhausts the initial 256-step
IOP guest quantum without a verified finish/reply.

From EE dispatch 11,779 onward the native trace alternates between
`0x00178DE8` and `0x001BD58C` and reaches its dispatch limit. The
runner previously invoked guest IOP slices only at selected EE SIF
HLE boundaries and trapped `WaitSema`, not while EE executed this
translated-code polling loop. The primary `ps2-web-recomp/SDBZ`
`runtime/src/iop/kernel.cpp` advances IOP work against elapsed EE
cycles. The follow-up change therefore schedules at most one bounded
256-instruction R3000 slice per 2,048 elapsed EE guest cycles while an
actual guest RPC remains pending and IOP work is runnable. This is a
narrow HLE-first scheduling step, **not** a full calibrated IOP clock.

The IOP guest call pending count is read from actual unfinished RPC
records. Errors and guest memory faults propagate as explicit stops;
no reply, DMA5 completion, semaphore or readiness is synthesized. A
regression checks that a committed guest request remains pending with
unchanged EE return memory and no DMA5 until its genuine R3000 handler
finishes; the pending count then drops to zero. CI and the next local
Windows replay must validate the runtime path. In particular, the
origin and guest handling of `WaitSema(0)` remain unresolved.

### Windows Clang CI environment regression at commit 6123aa14

The first CI run after the 05:21:52 IOP scheduling batch passed
SIF DMA acceptance and ROM0 acceptance. The Windows native WebGPU
MSVC compile and Linux native WebGPU job passed; the *separate*
Windows Clang clean-tree build failed before project code compiled:
the hosted image's LLVM/Clang 20 is rejected by Visual Studio 2026
MSVC STL 14.52.37025 (`yvals_core.h` STL1000 requires Clang >=22).
This was a compiler/STL image mismatch, not an IOP logic regression.
Pin the official LLVM 22.1.8 Windows installer in the existing
native WebGPU workflow with its published SHA-256; do not define
`_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH` to suppress the STL
version guard. Final CI acceptance still requires passing jobs on
the replacement commit, not only on the earlier implementation.

## 2026-11-20 05:39:29 — guest ARKD RPC scheduler reveals LIBSD SPDIF control write

Matched Windows replay `ps2-game-hle(20261120-053929).zip` includes the
complete five-report diagnostic set and three GS PPM captures. The preceding
EE-cycle-driven IOP scheduling batch is running: it advances genuine guest
R3000 code beyond the initial 256-instruction method-`0x500:1` slice.
The replay now stops after **11,980 EE dispatches** and **16,032,692 EE
instruction attempts** with 421 cached LLVM functions, 29 committed RPC
commands, and the original six IRX module-start threads. All three GS
images are fully black (display 512x448, upload 1024x64, WebGPU 512x448).
Lower EE dispatches are due to an earlier explicit hardware stop, not a
successful boot or regression to a prior stage.

**Exact stop:** IOP thread 9, guest PC `0x0005A824`, R3000 opcode
`0xA4A20000` (`sh v0, 0(a1)`), value `8`, effective address
`0xBF9007CA`, width 2, fault reason `memory_fault=4`, pending guest
RPC calls 1. The same once-per-ID `WaitSema(0)` diagnostic occurs at
`0x000147A8` with RA `0x0007D648`; its producer remains unverified.
The EE resumes no genuine `0x500:1` reply or new DMA5 signal after this
fault.

**Primary reference:** `ps2-web-recomp/SDBZ/runtime/src/iop/spu2.cpp`
`write_global()` explicitly recognizes MODE `0x7C6` and MEDIA
`0x7C8` and ignores other global writes, without implying playback,
DMA or interrupt completion. The secondary PS2SDK
`iop/sound/libsd/src/freesd.c`, `InitSpdif()`, explicitly programs
MODE=`0x900`, MEDIA=`0x200` and `U16_REGISTER(0x7CA)=8` in that
order. This matches the exact guest value and address in the replay.
The primary reference `read_global()` reads the unlatched `0x7CA` as
zero. Nearby undocumented `0x7C4` and `0x7CC` remain unsupported in
the HLE-first model.

**Batch implementation:** Add only the recognized `0x7CA` SPDIF control
halfword: accept its write without side effects; return reference zero on
read. Do not generalize this to all missing SPU2 MMIO offsets and do not
fabricate readiness, IRQ9, DMA completion, or ARKD replies. The focused
R3000 regression stages the exact opcode at the faulting PC, runs the
original SPDIF initialization register sequence, checks unchanged
MODE/MEDIA/OUT readback, rejects nearby unverified addresses and
unsupported byte width, and verifies that no IOP interrupt occurs.

**Next verification:** require the full CI suite on the final branch
HEAD and a fresh matched Windows replay. Confirm the guest passes
`0x0005A824`, then inspect genuine ARKD method `0x500:1` completion,
SIF0/DMA5 delivery and any further verified IOP dependencies.

## 2026-11-20 05:46:12 — authentic DMA4 interrupt disable during ARKD RPC

Matched Windows replay `ps2-game-hle(20261120-054612).zip` contains `boot.log`,
`expansion.log`, `first-boot.json`, `iop-disc-catalog.json`,
`translation-cache.json`, and all three GS/WebGPU PPM captures. It reaches
**18,365 EE dispatches**, **16,096,545 guest instruction attempts**, reuses
**421 cached LLVM functions**, and commits **29 RPC commands**. All six
original IRX startup threads finish and server `0x500` really registers,
releasing its deferred bind. All three images remain black; ARKD method
`0x500:1` has not returned. There are 32 observed EE DMA5 callbacks, but
none is evidence that the still-pending method `1` has replied.

**Previous blocker cleared:** the original LIBSD write at SPU2
`0xBF9007CA` no longer stops the IOP. The bounded guest RPC scheduler now
runs at least four additional 256-instruction R3000 slices. The existing
unknown `WaitSema(0)` diagnostic persists without stopping the guest;
its producer remains unresolved.

**New exact stop:** IOP thread 9, `pc=0x000140E8`, R3000 opcode
`0x0000000C`, service `v0=0x494F1207`, argument `a0=0x24`,
return address `ra=0x0005D55C`, reason `unhandled_syscall=2`,
`pending_calls=1`. The installed HLE stub at `0x14000 + 7 * 0x20 + 8`
resolves to `intrman` ordinal `7` / `DisableIntr` for IOP interrupt
line **36**, i.e. primary DMA channel **4**. The old bridge rejected
all numbers above 31 and required a registered handler.

**Primary reference:** `ps2-web-recomp/SDBZ/runtime/src/iop/libs.cpp`
implements `DisableIntr` by clearing the hardware interrupt enable and
optionally writing the source line to the caller's output pointer. Its
`runtime/src/iop/hw.cpp` maps extended INTRMAN sources **32..38** to
`DICR` bits 16..22 (line 36 = bit 20) and **40..45** to `DICR2`
bits 16..21. The shared DMA IRQ is ordinary IOP `I_STAT/I_MASK` bit 3.
Line 39 and IDs above 45 are not established DMA interrupt sources.
The reference `RegisterIntrHandler`, `ReleaseIntrHandler`,
`EnableIntr`, and `DisableIntr` do not require an already registered
handler before changing a valid source's mask; release is idempotent.

**Coherent parity batch:** expose a bounded hardware `set_interrupt_mask()`
for valid primary and extended DMA sources with explicit DICR/DICR2
channel enables. Enabling a DMA source sets the shared IRQ3 mask and
primary controller master enable; disabling clears only that source's
mask, preserving pending flags and unrelated interrupt control. Update
the four INTRMAN HLE operations to use this hardware mapping and the
reference guest pointer semantics. Keep unknown service ordinals,
reserved line 39, out-of-range IDs, invalid function pointers and
unwritable output pointers fail-closed; do not generate completion,
DMA flags, IRQ delivery or semaphore wake.

Regressions execute real guest R3000 INTRMAN stubs for line 36 and
secondary line 41, verify DICR/DICR2/I_MASK state, no spontaneous IRQ,
optional guest output, registration/release, invalid-source rejection,
and reboot reset. The actual game still needs a matched Windows replay
to verify that this extended DMA4 disable advances method `0x500:1`
toward its genuine guest handler return and SIF0/DMA5 reply.

## 2026-11-20 05:57:06 — ARKD clears extended INTRMAN and reaches SPU2 DMA4 CHCR

The matched `ps2-game-hle(20261120-055706).zip` archive contains the
five diagnostic files and three P6 GS/WebGPU PPM captures. The previous
`DisableIntr(36)` syscall stop is absent, confirming that the extended
DICR interrupt-mask support advances the guest. At **41,025 EE dispatches**
(versus 18,365 previously), the replay reports **16,323,145 EE guest
instruction attempts**, **421 cached LLVM functions**, **29 committed
RPC commands**, and the same six completed original IRX startup threads.
The 512x448 GS display, 1024x64 GS upload and 512x448 WebGPU snapshots
contain only zero RGB values. ARKD `0x500:1` remains a real pending guest
RPC and there is no verified SIF0/DMA5 reply for this method.

**Exact stop:** IOP thread 9, PC `0x0005B450`, opcode `0x8E620000`
(`lw v0, 0(s3)`), guest address `0xBF8010C8` (physical
`0x1F8010C8`), width 4, memory-fault stop reason 4, RA
`0x0005AAE0`, pending guest calls 1. This register is **SPU2 core 0,
primary DMA channel 4 CHCR**. The preserved `WaitSema(0)` diagnostic
still appears; the origin of its invalid ID is unverified.

**Primary reference** `ps2-web-recomp/SDBZ/runtime/src/iop/hw.cpp`
`dma_channel()` maps all primary DMA channel register slots from
`0x1F801080` and secondary slots from `0x1F801500`. In particular,
SPU2 DMA4 uses `0x1F8010C0..0x1F8010CF` and SPU2 DMA7 uses
`0x1F801500..0x1F80150F`, each with MADR, BCR, CHCR, TADR words.
Its `start_dma()` invokes actual `spu2::dma()` with guest memory and
sound-RAM transfers before any completion is scheduled. The existing
LLVM HLE-first model does not yet supply that real transfer producer.

**Coherent batch:** Add explicit independent 32-bit DMA4/DMA7 register
banks with genuine readback, KSEG1 aliasing, reset and CHCR.STR latch
behaviour. Do **not** synthesize sound-RAM copying, clear a requested
CHCR.STR, fabricate DMA4/7 completion flags/IRQ3, or manufacture ARKD
RPC replies just because control registers are programmed. Keep
subword and unallocated MMIO addresses fail-closed. Regress the exact
original guest R3000 `lw v0, 0(s3)` at `0x0005B450` as well as both
DMA banks, separate configuration, no-fake-interrupt and cold reset.

**Next validation:** verify CI and a new matched local Windows replay
that passes the original CHCR read. If ARKD next actually requests a
SPU2 DMA transfer, implement an authentic bounded IOP↔sound-RAM DMA4/7
backend using the guest MADR/BCR/CHCR and original timing/IRQ model;
never clear a requested transfer without moving its real data.

## 2026-11-20 06:03:34 — ARKD method 1 reaches DMA4 BCR halfword write

Matched archive `ps2-game-hle(20261120-060334).zip` contains the five
expected diagnostic reports and three P6 PPM captures from a single
Windows replay. The preceding DMA4 CHCR word-read fault at IOP
`0x0005B450` has cleared. The current replay reaches **41,231 EE
dispatches**, **16,325,205 guest instruction attempts**, reuses
**421 cached LLVM functions**, and commits **29 RPC commands**.
Six original IRX module-start threads complete and guest ARKD
registers servers `0x500..0x503`. All three snapshots (display
512x448, upload 1024x64, WebGPU 512x448) remain fully black.

**Exact stop:** guest IOP thread 9, PC `0x0005B5D4`, opcode
`0xA4A30000` (`sh v1, 0(a1)`), KSEG1 address `0xBF8010C4`
(physical `0x1F8010C4` = primary DMA4 **BCR**), 16-bit write,
`v0=0xBF8010C6` (address of BCR upper halfword), `a0=1`,
RA `0x0005B514`, `memory_fault=4` with **one genuine guest
RPC still pending**. The earlier `WaitSema(0)` diagnostic recurs,
with no verified producer or resolution. No guest ARKD method-`0x500:1`
completion, genuine SIF0/DMA5 reply, nonblack display or game boot
has been observed.

**Primary reference:** `ps2-web-recomp/SDBZ/runtime/src/iop/hw.cpp`
decodes DMA registers into 32-bit MADR/BCR/CHCR/TADR words, extracting
the correct byte or halfword from a read and merging 8-/16-/32-bit
writes using a little-endian lane mask, preserving untouched bits.
DMA4 and DMA7 use independent primary/secondary channel banks.
Thus `SH` at `0x1F8010C4` must preserve BCR high 16 bits;
`0x1F8010C6` addresses the upper halfword of the *same* BCR.

**Coherent batch:** extend only the two previously verified DMA4/DMA7
banks to aligned 8-, 16- and 32-bit access, with per-lane read/write,
unaltered opposite halfword, independent sound-DMA channels, exact
R3000 `SH` guest regression at `0x0005B5D4`, invalid-width/
unaligned/bank-boundary rejection, no-fake-DMA/IRQ and cold reboot
checks. Other MMIO width rules remain unchanged. Programming CHCR.STR
still does **not** copy sound samples, complete a transfer, clear
STR, fabricate DICR flags, raise IRQ3 or release an EE semaphore.
The actual guest DMA sound-data movement and completion producer
remain unimplemented until grounded by a genuine transfer request.

**Next acceptance:** all CI checks on the exact new branch HEAD,
followed by a fresh matched Windows archive confirming this original
BCR write advances and whether the next authentic blocker is a
real IOP↔SPU2 RAM transfer or another guest operation.

## 2026-11-20 06:13:34 — DMA4 BCR store cleared; ARKD method 1 stalls on IOP wait

The matched archive `ps2-game-hle(20261120-061334).zip` contains all
five expected diagnostic files plus three P6 GS/WebGPU captures. The
original guest DMA4 BCR halfword store no longer faults. The EE reaches
the **100,000-dispatch limit** after **16,912,906 guest instruction
attempts** with **421 cached LLVM functions** and **29 committed
RPC commands**. The original six IRX module entry threads completed
and real ARKD servers `0x500..0x503` registered. All three captures
remain entirely black. Guest ARKD method `0x500:1` is **still pending**:
there is no real guest handler finish trampoline/SIF0->EE DMA5 reply.

Unlike the preceding four matched replays, **no new IOP unhandled
syscall or memory fault occurs**. The EE repeatedly visits
`0x00178DE8` and `0x001BD58C` until the dispatch limit. The
EE-cycle-based IOP scheduler continues thread 9 for at least 128
bounded turns of 256 genuine R3000 instructions each, but the
RPC remains pending. `WaitSema(0)` is still reported once during
this run at IOP PC `0x000147A8`; its producer is unverified.
The archived logs do not show the actual DMA4 CHCR/STR register
state at the final stall, so **an uncompleted DMA is a hypothesis,
not yet a proven guest dependency**.

**Primary reference:**
`ps2-web-recomp/SDBZ/runtime/src/iop/hw.cpp` implements IOP DMA4
and DMA7 with MADR/BCR/CHCR, and calls the actual
`spu2::dma()` from `start_dma()`. Its
`runtime/src/iop/spu2.cpp` moves real little-endian halfwords
between primary IOP RAM and shared SPU2 sound RAM at the guest
TSA, wrapping at the 1M-word sound RAM boundary. Direct DMA
completion is then scheduled after `64 + words/4` IOP cycles.
A stopped transfer cannot complete. AutoDMA uses a different
streaming input ring and cannot be faked using the direct path.

**Coherent HLE-first batch:** Connect the real IOP CPU RAM to
the existing guest DMA4/DMA7 register banks only when the IOP
hardware is attached. Read the original BCR count and MADR,
preflight the complete physical RAM span, copy actual guest
bytes to/from the two cores' shared sound RAM, update the
selected TSA, and raise SPU2 IRQ9 only if the original
enabled IRQA is crossed. The direct DMA completion occurs
after the reference-derived IOP instruction-clock interval:
only then can CHCR.STR clear and the actual DICR/DICR2
channel flag and IRQ3 be asserted. A guest CHCR.STR clear
cancels the pending completion. Invalid RAM ranges,
zero-length requests and AutoDMA must remain requested
with no fabricated data, completion or interrupt.

**New regression fixtures** exercise both DMA channels and
directions with known *real guest RAM* data, SPU2 RAM
wraparound, TSA progression, precise delayed STR clearing,
DICR/DICR2 flag creation after actual copying,
cancellation, invalid physical memory and AutoDMA fail
closed. A bounded guest RPC scheduler snapshot now
records thread-PC + actual DMA4/DMA7 register states
every 64 turns and records the thread wait statuses once
if a guest RPC remains pending with no runnable IOP work.
This distinguishes genuine DMA blockage from a thread
waiting on a guest kernel object or missing IRQ dispatch.

**Acceptance:** all CI workflows must pass on the final
branch HEAD. A fresh matched local Windows replay is then
required to identify the next actual guest state and verify
whether this real DMA backend advances ARKD method `0x500:1`.
No sound synthesis/playback or kernel IRQ callback execution
is claimed here.

### DMA completion under an idle IOP kernel

The ordinary direct DMA completion is a **device deadline**, not
a guest thread instruction. If the real transfer has already copied
all bytes but the IOP driver blocks waiting for its completion,
the previously implemented kernel `has_guest_work()` incorrectly
returned false and stopped advancing the IOP clock. The kernel
now includes pending real SPU2 DMA events in `has_guest_work()`,
bounds idle progress by its provided IOP cycle budget, and updates
the physical hardware timer when advancing to the next actual
DMA deadline. The DMA4 regression asserts that a kernel with
no runnable guest thread still clears STR and sets the real
DICR/IRQ3 completion *only* when 64 + words/4 elapsed, and
does not keep the DMA event runnable after completion.
AutoDMA, invalid memory and cancellation remain fail-closed.
This scheduling change still does not invoke guest INTRMAN
callbacks or send a synthetic ARKD SIF reply.


## 2026-11-20 06:35:50 matched retail replay and follow-up runtime parity

The complete matched Windows archive `ps2-game-hle(20261120-063550).zip`
was collected **on prior head `768a9e4c5e084b3f39d03f2adc8782e6e1b59533`**,
after the original DMA4/DMA7 sound-RAM transfer and idle-device scheduling
changes. It is NOT a validation of the later interrupt/scheduler patches.

- The EE reaches 100,000 dispatches, 16,912,906 attempted guest
  instructions, 421 cached LLVM functions and 29 committed SIFRPC commands.
  ARKD registers original guest servers `0x500..0x503`, but method
  `0x500:1` remains genuinely pending. Three GS captures are black.
- Real IOP DMA4 starts at MADR `0x000880ec`, BCR `0x00010010`,
  CHCR `0x01000201` and copies 64 bytes. Its 68-IOP-cycle deadline fires,
  clearing STR to CHCR `0x00000201`, publishing DICR `0x90900000`
  and I_STAT `0x00000008` (IRQ3).
- IOP ARKD thread 9 then blocks at `0x000147ac` with wait type 3
  (semaphore). A previous invalid `WaitSema(0)` at `0x000147a8`,
  RA `0x0007d648`, still returns authentic `-408`.
  The replay alone does not prove whether missing IRQ handler dispatch
  caused the pending semaphore.

### Implementation and source comparison after that replay

All changes remain on the single draft PR #145, not on main:

- `a9f8ef0`: individually track the two SPU2 core IRQ latches;
  assert IRQ9 whenever either core newly latches, even after the other
  core's I_STAT acknowledgement. Accept the reference's ignored
  SPDIF status halfword at `0x1f9007c2` for 32-bit OUT writes.
- `4661c61`: resolve enabled and pending DMA channels through
  DICR/DICR2 before ordinary IOP IRQ lines. Schedule the originally
  registered INTRMAN guest R3000 handler only after a real device
  event, acknowledge its source, preserve an interrupt context for
  QueryIntrContext, and respect the guest return value for ordinary IRQ
  masks. Extend the EE WaitSema guest-work path to pending calls as well
  as deferred binds. No synthetic iSignalSema, EE DMA5 or RPC reply.
- `7c5439e`: add regressions for SPU2 per-core IRQ9, SPDIF word
  stores, DMA handler and QueryIntrContext execution, plus a guest
  RPC handler requiring multiple IOP turns before a genuine response.
- `72c04a3`: keep a running IOP thread inside CpuSuspendIntr/
  CpuResumeIntr critical sections until it resumes, while allowing
  hardware DMA/timer cycles to progress during the masked interval.
- `93b1f23`: replace RPC-only EE-cycle scheduling with a bounded,
  carried-forward IOP clock at the reference's 8:1 EE-to-IOP ratio.
  Guest IOP work now progresses on elapsed EE time even without a
  pending ARKD call. This ratio is reference-compatibility timing,
  not validated per-instruction R5900 cycle accuracy.
- `3bacb44`: derive both IOP VBlank start and end edges from the
  existing GS scanout schedule, using the reference's 22-scanline
  end offset, instead of requiring manual end-edge test injection.

### CI and acceptance

The workflow changes in `3ee9831` run Windows MSVC and SHA256-pinned
Clang 22 independently, keep the combined `native-webgpu-windows` gate,
reuse Linux Lavapipe where installed, and cancel superseded PR runs.
`bced18e` divides the independent Python game-runner scenarios into
four exhaustive parallel CTest entries and retains three fast shards.

**Not yet established:** A full Windows retail replay at the new HEAD,
completion of ARKD method `0x500:1`, any nonblack GS capture, or real
sound output. The IOP critical-section and VBlank end changes also require
current-head CI validation before being regarded as accepted. LOADFILE
still replies when module `_start` is scheduled instead of after its
actual return, and native `timrman` / `sifman#32` coverage remains open.

Do not merge PR #145 without explicit authorization; when authorized,
squash-merge it once into `main`.

## 2026-11-20 — LOADFILE guest-return completion and argument regression

Baseline: PR #145 at `8daae358fdc7edc52723fe6cb1863286e026f030`. Build and test failed only at `CTest (fast-0)` / `iop_sif_rpc_bridge`; ROM0, SIF DMA and native WebGPU checks passed. The argument-bearing fixture expected an EE receive-word sentinel without initializing it. Both receive words are now initialized, and the test guards them and DMA5 until the original guest return.

The IOP kernel records `m_module_entry_returned` and a snapshot of original guest `v0` only when the R3000 module initializer reaches the reserved return BREAK. LOADFILE consumes that event once rather than interpreting a dormant startup thread as a return. Starting or restarting clears the event. A regression stops the startup thread before executing its first guest instruction and verifies that neither EE receive memory nor DMA5 is changed; it then restarts the same real IRX and checks its actual return and delivered reply. Bounded logs differentiate queued starts, guest returns, queued results and delivered EE replies. No guest execution, DMA, IRQ, RPC result or semaphore signal is fabricated.

Validation remains pending on the resulting exact PR head: focused IOP tests plus all required Actions workflows. Only a later matched retail replay can establish IRQ3 guest callback, ARKD semaphore wake, genuine `0x500:1` return, SIF0/DMA5 completion and GS output. The PR remains draft and unmerged.

## 2026-11-20 — 07:39 retail replay: DMA4 completion, interrupt-context parity

The complete `ps2-game-hle(20261120-073909).zip` contains eight matched artifacts (boot, expansion, final stopped report, static IOP catalog, translation cache, and three all-black PPMs). It exhibits the LOADFILE start/return/reply diagnostics introduced in `a58816d`, but the ZIP does **not** independently record the git checkout SHA. The stopped report has 100,000 EE dispatches, 16,912,888 guest instruction attempts, 29 committed RPCs, six returned original IRX initializers, and ARKD servers `0x500..0x503`. ARKD method `0x500:1` is still pending. EE polls `0x00178DE8`/`0x001BD58C` and all GS captures are black.

Real DMA4 uses MADR `0x000890EC`, BCR `0x00010010`, CHCR `0x01000201`; 64 original guest bytes move, the timed completion after 68 IOP cycles clears STR and publishes DICR `0x90900000`, I_STAT `0x00000008`. The log does not expose the guest IRQ handler entry/return or its registration/mask state at completion. The once-only guest `WaitSema(0)` error is still observed, now at thread 11 PC `0x000147A8` RA `0x0007E648`; do not conflate this error with the subsequent valid blocked semaphore without new evidence.

Reference review: `ps2-web-recomp/SDBZ/runtime/src/iop/kernel.cpp` stores interrupt-suspended state on each guest thread and permits delivery after a suspended owner blocks. `runtime/src/iop/libs.cpp` INTRMAN #8/#9/#17/#18 changes the kernel state, **not** hardware I_CTRL. Our native bridge instead wrote I_CTRL=0 for CpuSuspendIntr and retained a global suspended flag even while its owner blocked, preventing DMA IRQ3 from reaching the guest. The parity batch stores suspension with the guest thread, releases the effective global suspension when its owner stops running, restores that thread's state on its next dispatch, and preserves hardware I_CTRL ownership for real MMIO. Real interrupt entry and return are logged; DMA completion includes I_MASK, I_CTRL and next eligible IRQ. The regression uses a genuine direct SPU2 DMA4 transfer, registered guest INTRMAN #36 callback and original R3000 `iSignalSema` to wake a thread that blocked in `WaitSema` after `CpuSuspendIntr`. No fake IRQ, semaphore ID, RPC reply, or ARKD completion is introduced.

**Validation:** All four Actions workflows on the new commit must pass. This proves the synthetic guest critical-section/IRQ chain, not the retail blocker. Request a fresh matched Windows archive only after same-SHA CI is green; locate the first missing guest DMA4 interrupt/handler, ARKD wake, real method return and SIF0/DMA5 reply. PR #145 stays draft and unmerged.

Additional regression: stopping an otherwise runnable thread while inside `CpuSuspendIntr` must clear both the saved thread suspension state and the effective CPU interrupt gate. This prevents a stale global mask after explicit `StopThread` or guest `ExitThread` without injecting an IRQ.

## 2026-11-20 — 08:31:05 matched full replay: IRQ36 proven to be LIBSD

Archive: `ps2-game-hle(20261120-083105).zip`. The `boot.log`, `first-boot.json`, `expansion.log`, IOP catalog and three GS PPM files are from the 08:30:42 full replay; the archive also includes an **older** `quick-probe.json` from 08:00:10. Analyze the stopped full report, not the stale quick probe. The ZIP has no independent `git rev-parse HEAD` record, so exact running source SHA remains unverified.

Full replay: EE dispatches 100,000 (budget reached), 16,912,888 instruction attempts, 29 committed RPC commands; guest ARKD servers `0x500..0x503` register. The original ARKD method `0x500:1` is enqueued and enters its guest RPC worker, but **no original handler return, SIF0 Rend or DMA5 response** for that call is evidenced. GS snapshots are black.

The real 64-byte SPU2 DMA4 completes after 68 IOP cycles; `i_stat=0x8`, `i_mask=0x00020208`, `i_ctrl=1`, and `next_irq=36`. The registered guest IRQ36 handler `0x0005b9e8` is acknowledged at cycle 1,928,701 and returns `v0=1` at cycle 1,928,930. **Crucial distinction:** `0x5b9e8` lies inside the loaded LIBSD.IRX module image (entry `0x592b0`, original `0xb0` entry offset, image ends near `0x5ebb0`), not ARKD_DVD.IRX. DMA4 completion therefore verifies the LIBSD/SPU2 path, not an ARKD semaphore wakeup.

The ARKD guest worker is thread 11 (entry `0x8824c`) and calls `WaitSema(0)` from ARKD address `0x7e648`, returning kernel error -408. Seven valid kernel-HLE semaphore IDs `2..8` exist, all with count 1 when this once-only error is observed. Do **not** assume the unknown semaphore call is the fatal stall without observing the following valid semaphore transitions; equally, do not infer the LIBSD IRQ signals the ARKD worker.

### Next diagnostic batch

Add side-effect-free bounded IOP semaphore traces for genuine create, WaitSema acquire/block and SignalSema increment/wakeup, recording guest thread, ID, caller RA and IOP cycles; log the exact guest RPC queue token, method, queue, handler entry, and genuine return. Add real CDVDMAN read acceptance/completion diagnostics only if a later trace reaches a disc-read path. This will distinguish worker scheduling, incorrect semaphore-object use, absent CDVD/event producer, and genuine handler return. **Never** fabricate a wakeup, timer, DMA completion, guest handler return, or SIF0 reply.

Retain existing draft PR #145 and squash-merge-only policy. Run four required CI workflows on the new head, then request a fresh matched `scripts\run_ps2_game.bat --full` retail replay. CI is not proof the ARKD method returns.


## 2026-11-20 — 08:47:39 full retail replay: ARKD semaphore wakeup is real

Analyzed `ps2-game-hle(20261120-084739).zip`. Matched full `boot.log`,
`first-boot.json`, IOP catalog and GS snapshots timestamped 08:47:20.
The archive also contains an **older** `quick-probe.json` from 08:00:10;
never use it as the final replay. The ZIP does not embed an independently
verified checkout SHA.

At 100,000 EE dispatches, 16,912,888 EE instruction attempts and 29 committed
RPCs, ARKD method `0x500:1` (guest RPC token 7) has genuinely entered thread 11,
at IOP cycle 1,891,889, in guest handler `0x0007df44`, but no guest return
trampoline or method-1 SIF0/DMA5 reply occurred.

New semaphore trace **eliminates the previously suspected first missing
wakeup**:

- Thread 11 creates semaphores 11, 12, 13, 14 and 15 (ID 14 begins with
  count 1), consumes ID 14 once and then blocks again on ID 14 at cycle
  1,930,247 from RA `0x0007e4d8`.
- Thread 25 **genuinely signals and wakes thread 11** on semaphore 14 at
  cycle 1,930,634 from RA `0x0007e3c8`.
- Thread 11 genuinely resumes and **signals semaphore 14 itself** at cycle
  1,930,656 from RA `0x0007e4f0`.
- Threads 18, 19, 23 and 24 block on semaphore IDs 11, 12, 13 and 15
  respectively. There is no further semaphore transition in the bounded
  replay, so their eventual producers and thread 11's later PC remain open.

LIBSD DMA4 IRQ36 still registers, executes and returns; it does not signify
an ARKD completion. The earlier `WaitSema(0)` returns error -408 but must **not**
be labeled the actual cause without following the now-proven valid semaphore
sequence. All three PPMs still contain only black pixels.

**Next bounded diagnostic batch:** when a guest RPC remains outstanding in a
full `--trace --report` replay, print the real ready/blocked/faulted IOP
thread contexts at EE dispatch 50,000, 75,000 and at final stop. Include PC,
next PC, fetched opcode, module-related entry, semaphore wait type, RA, GP,
stack, interrupt-suspension state and IOP cycle count. This is read-only and
does not advance the clock or fabricate any result. Compare snapshots to
distinguish guest busy-polling, scheduler starvation, waiting for a different
producer, or fault. Retain PR #145 and require all four CI gates on one head.


## 2026-11-20 — 09:00:16 full retail replay: active ARKD method 1 loop

Full archive `ps2-game-hle(20261120-090016).zip`: `first-boot.json`,
`boot.log` and the three GS PPMs at 09:00:02, older `quick-probe.json`
from 08:00:10 ignored. No checkout SHA is embedded.

This replay confirms **IOP R3000 execution is progressing** after the
previously established genuine semaphore-14 wakeup. Guest RPC token 7
(server 0x500, method 1, handler 0x0007df44) remains outstanding, but
thread 11 stays **ready** (not blocked/faulted) at all three checkpoints:

| EE dispatches | IOP cycles | Thread 11 PC | v0 |
| ---: | ---: | --- | --- |
| 50,000 | 1,939,921 | 0x0007e0ec | 0x000dd39c |
| 75,000 | 1,971,172 | 0x0007e114 | 0x000dd64f |
| 100,000 | 2,002,422 | 0x0007e13c | 0x000dd903 |

Thread 11 continues through a small guest-code region (PCs differ by
0x28 bytes) and its v0 changes by approximately 692 bytes per 31,250
IOP guest cycles. A genuinely long-running scan/copy/parse loop is
plausible; the instructions available in the ZIP are **insufficient
to confirm its meaning or whether it terminates**. No evidence yet
supports fabricating a semaphore wake or changing R3000 semantics.
At final stop, 4 IOP threads are ready, 13 blocked, and none faulted;
one guest RPC remains pending. EE stopped at its explicit 100,000
dispatch ceiling, 16,912,888 guest instruction attempts. The three
PPMs remain black.

**Follow-up:** configurable per-pass EE ceiling in the Windows launcher,
default 100k for quick probes and 500k for full traces; allow
`MAX_DISPATCHES`/`BOOT_TIMEOUT_SECONDS` overrides. Retain host-only
bounded checkpoint logs, with dense early checkpoints and sparse
long-run checkpoints (250k, 500k, 1M, ...), plus original guest
R3000 instruction words surrounding at most two runnable IOP
thread PCs at final stop. This allows analysis of actual IRX loop
behavior without shipping game binaries, inventing a return,
or making irreversible changes to guest state.

## 2026-11-20 — 09:11:02 long retail full replay: original ARKD byte loop still running

Archive `ps2-game-hle(20261120-091102).zip`: full `boot.log`,
`first-boot.json`, IOP catalog and three GS PPMs timestamped 09:09:42;
its `quick-probe.json` is an older 08:00:10 artifact, **not** this run.
Exact checkout SHA is not embedded.

At **1,000,000 EE dispatches**, 25,912,896 EE guest instruction
attempts and **3,127,424 IOP cycles**, seven genuine ARKD guest RPC
tokens were accepted, first six returned through original R3000
handler trampolines, and token **7 / server 0x500 / method 1**
remains outstanding. IOP thread **11** is ready, not blocked or faulted,
at all checkpoints, in code around `0x7e0ec..0x7e15c`.
Its saved `v0` advances from `0x000dd39c` at 50k dispatches to
`0x000e3a50` at 1M (+26,292 bytes). The final original R3000
instruction window shows a **16-byte inner loop** (a signed comparison
against 16 at `0x7e0bc`) performing real byte loads, shifts,
bitwise operations and stores. There is **no evidence** of a frozen
PC or a missing semaphore wake, but the **outer termination condition**
lies outside the captured window. No guest RPC return was observed.
At final stop 5 threads are ready and 12 blocked, none faulted;
all three PPMs remain uniformly black.

The full `--trace` log is approximately **72.6 MB** and took
approximately 62.4 s for the 1M-dispatch run, so avoid scaling
per-dispatch trace output for an extended byte-processing loop.
Instead, keep the real EE/IOP timing and original guest execution
unchanged but allow the previously bounded RPC checkpoints even in
the existing cached **non-traced** quick probe. Enlarge the final
original-IOP opcode window to 129 aligned 32-bit words and add the
sampled thread's 32 GPR values and real frame-pointer locals.
No synthetic semaphore signal, guest return, MMIO completion or
RPC reply. This supports a faster long-probe workflow
(`set MAX_DISPATCHES=4000000`, then `scripts\run_ps2_game.bat`) with
current `quick-probe.log`/`quick-probe.json` as the authoritative
outputs, while old full logs remain preserved.

When a new probe is available, disassemble the expanded ARKD loop and
inspect its actual counter/bound and supporting data before deciding
whether the repeated work is legitimate or caused by erroneous
guest state. Keep PR #145 draft; require all four CI workflows on
the final head.


## 2026-11-20 — 09:26:39 long replay: real ARKD stdio#4 failure

Archive `ps2-game-hle(20261120-092639).zip` has **current full**
`boot.log`, `first-boot.json`, and GS PPMs at 09:26:12, plus an
older `quick-probe.json` from 08:00:10. The local launcher selected the
full trace instead of a cached quick probe; do not treat the stale
quick-probe report as part of this run. No exact source SHA is embedded.

The replay **crosses the previous ARKD 16-byte inner loop**.
The IOP clock now stops with a real guest `unhandled_syscall` at
`0x000141a8`: original thread **11**, `v0=0x494f1304`,
`a0=0x00088c90` (format string pointer), `a1=0x0001ad40`,
and `ra=0x0007dde4`. Decode `0x494f1304` through the installed
kernel bridge: HLE family 3 / STDIO **`printf #4`**. The existing
formatter supported only a narrow subset of printf formatting.
The reference `ps2-web-recomp/SDBZ` `runtime/src/iop/libs.cpp`
`install_stdio` reuses its more capable guest varargs formatting;
this repo's own `iop_sysclib_strings.cpp` already has an independently
tested bounded implementation for O32 flags, widths, precisions,
h/l/ll length, integer and string specifiers.

Full stopped report: **3,639,679 EE dispatches**, **52,309,904**
guest instruction attempts, IOP cycle **6,427,050**, and
`EE_EXIT_UNSUPPORTED` at `0x001bd58c` only after the original IOP
stdio import was rejected. The last IOP snapshot shows thread 11
**faulted**, 4 ready and 12 blocked, with guest ARKD method
`0x500:1` still pending. This is a **new concrete HLE blocker**,
not a limit or a missing semaphore signal. Three GS snapshots
remain black.

Implemented the narrow architectural fix: factor the already tested
SYSCLIB guest-safe O32 formatter into a read-only `render_stdio_format`
function. `stdio#4` now uses it for real guest printf parameters and
returns the actual output byte count only after console writes succeed.
Unsafe/unknown format conversions remain fail-closed, with bounded
diagnostics including original guest format, argument registers,
stack, RA and IOP cycle count if rejection occurs. Regression tests
exercise width, flags, precision, long and 64-bit aligned O32 varargs,
and unsafe `%n` rejection. No invented guest RPC return, semaphore
signal or DMA5 completion. This is functionally separate from
the older SYSCLIB `sprintf` destination-memory interface.

## 2026-11-20 — Force bounded quick probe after cache invalidation

The 09:26:39 user archive unexpectedly contained a **266.9 MB**
`boot.log` rather than a new `quick-probe.log` because the launcher
selected its full path when saved guest IR could not be reused.
To prevent another huge report while checking the new stdio#4 fix,
the Windows launcher now recognizes an explicit `--quick` argument,
which forces `--quick-probe --no-iop-catalog` even after a guest
translation-cache miss. The ordinary default remains quick only with a
valid cache, and `--full` / WebGPU requests still use the complete
replay. This changes **only diagnostic output selection**, never R3000
execution or guest RPC completion. Regression assertions verify
override ordering.

Next recommended retail run:
```bat
git pull --ff-only
git rev-parse HEAD
set "MAX_DISPATCHES=4000000"
set "BOOT_TIMEOUT_SECONDS=300"
scripts\run_ps2_game.bat --quick
```
Upload current `quick-probe.log`, `quick-probe.json` and any matching
expansion logs; don't confuse preserved full logs with the quick probe.


## 2026-11-20 — 09:45:16 cached probe: original ARKD method-1 completes, next blocker SIFMAN #7

Inspected `ps2-game-hle(20261120-094516).zip`, which contains only
`quick-probe.log` (6,532,005 bytes) and `quick-probe.json` at
09:44:30. The quick log contains **three separate native
auto-expansion replays (0, 1, 2)** and the JSON corresponds to the
final replay, not a single 3x continuous run. Exact local source SHA
is not recorded in the archive.

The STDIO #4 fix enabled real guest progression through the long
original ARKD byte-processing loop: guest RPC **token 7**, server
**0x500**, method **1** returns guest result 0 at **IOP cycle
6,427,070**. Guest RPC **token 8**, server **0x503**, method
**258** enters thread 14, signals semaphore 15 and genuinely
wakes **thread 24** at cycle **6,427,376**, then returns at
6,427,402. Guest RPC **token 9**, server **0x503**, method **2**
returns at 6,427,677. These are original IOP guest trampolines,
not synthetic HLE replies.

The final replay stops with **IOP thread 24**, reason
**`unhandled_syscall`** (enum value 2), at **0x00018048**,
instruction `0x0000000c`. This is the reserved HLE syscall
stub `c_sifman_bridge::k_set_dma_stub + 8`: SIFMAN
**`sceSifSetDma` ordinal 7**, service `0x494f0007`. Unlike
previous replays, **method 0x500:1 is no longer the blocker**.
Final EE state: **3,639,873 dispatches**, **59,312,889
instruction attempts**, `EE_EXIT_UNSUPPORTED`, PC
`0x00178068`, because the IOP producer halted; SDBZ is not
yet booted.

The IOP SIFMAN bridge currently rejects DMA requests if the
EE channel 5 receiver is not armed, descriptor list is malformed,
a descriptor has invalid source/destination/size/attributes,
source bytes cannot be read, or the EE DMA completion cannot
be raised. **The uploaded log does not expose which condition
rejected this call.** The original `ps2-web-recomp/SDBZ`
SIFMAN implementation processes guest descriptor lists, but
this runtime must not guess or silently copy data without its
own existing DMA validation.

Add a **read-only, bounded reason-specific rejection trace**
in the real `sceSifSetDma` HLE handler: original IOP
thread/PC/RA/cycles, list/count, failing descriptor fields,
actual EE channel-5 CHCR and explicit failure reason. This
does not modify any DMA acceptance rule, EE or IOP guest
state, memory or scheduling. Existing tests for valid DMA,
transactional invalid-descriptor rejection, unarmed EE
receiver rejection, and disconnected backend must remain
green. Use the next matched **`--quick`** probe to
determine whether the true blocker is EE receiver state
or a specific guest transfer descriptor before changing
device semantics.




## 2026-11-20 — 09:59 matched cached quick probe: valid 96 KiB SIFMAN descriptor rejected

The uploaded `quick-probe(1).log` and `quick-probe(1).json` represent a
single final `replay=0` (3,639,873 EE dispatches, 59,312,889
instruction attempts, 11 GS fields). This upload contains **no checkout
SHA**, so its exact local commit is not independently established.
All four CI workflows had passed on remote PR #145 HEAD
`8de7a731239d5bda13dc55a5c73876c66b75f1df`.

The newly instrumented SIFMAN rejection is **`invalid_descriptor_fields`**:
original IOP thread 24, PC `0x00018048`, RA `0x0007fba8`,
descriptor list `0x000dd268`, count 1, first descriptor readable,
IOP source `0x000dd2c0`, EE destination `0x01652400`,
size **98,304 bytes (96 KiB)**, attributes 0, EE SIF0 channel-5
CHCR **`0x00000184`** (STR armed), IOP cycle **6,458,047**.
Both addresses are 4-byte aligned. Source plus size ends at
`0x000f52c0` (inside 2 MiB IOP RAM); destination plus size ends
at `0x0166a400` (inside 32 MiB EE RAM).

The sole failing clause for those exact fields is the native bridge's
hardcoded `size > 65536` rejection. The primary reference
`ps2-web-recomp/SDBZ/runtime/src/iop/sif.cpp` has no 64 KiB
`sceSifSetDma` descriptor limit. Remove only that artificial limit;
retain nonzero size, zero-attribute, segment/alignment, IOP source
span, EE destination span, staged reads, armed receiver and actual
DMA5-completion requirements. The existing 2 MiB IOP source span
bounds an individual descriptor; a new unbounded copy is **not**
introduced.

Regression extends the guest-import SIFMAN test with this exact
96 KiB source/destination and payload, checks the complete EE payload
and guards after an armed call, rejects the same call unarmed, and
rejects invalid 96 KiB IOP-source and EE-destination boundary
descriptors transactionally. Failed calls may not modify EE RAM,
raise DMA5, or consume an ID. CI success alone does not prove a
retail SIF0 response or visible frame. A subsequent matched quick
probe is required to observe the next original guest event.


## 2026-11-20 — 10:13 matched quick-probe: PADMAN sifman #32 missing

User-uploaded `quick-probe(2).log`, `quick-probe(2).json`, and
`translation-cache(2).json` form the next Windows cached probe.
The log contains **nine independent auto-expansion replays (0..8)**;
the final JSON is replay 8, not a continuation of earlier dispatch
counts. Neither log nor cache embeds a verified checkout SHA. Remote
PR #145 HEAD before this batch was
`8ac12e4055b9dfbccd749928a1752c38fd007471`.

No `SifSetDma rejected` or IOP syscall stop occurs. The prior 96 KiB
SIFMAN rejection is gone. Actual ARKD guest RPC token 10, server
`0x503`, method 2, returns guest pointer `0x00091bc0` at
IOP cycle 7,303,495. The final replay reaches **4,000,000 EE
dispatches**, 91,541,856 EE guest instruction attempts and 18 GS
fields; it stops at the configured budget with `exit_name=RETURN`,
not an unsupported instruction. No black/nonblack image was supplied.
Observed GS sprite provenance still has five untextured pairs and
zero nonblack sample RGB.

The **new first concrete bootstrap blocker** is repeated LOADFILE
`cdrom0:\\PADMAN.IRX;1`, first request sequence 33, returning
`0xffffffff`, `load_status=7`, `link_status=5`,
`unresolved=sifman#32`. Subsequent failed retries are reported
at sequences 128, 256, ..., 2048. This is a genuine missing IRX
library import, not a reason to increase dispatch limits.

The primary reference `ps2-web-recomp/SDBZ/runtime/src/iop/sif.cpp`
exports `sceSifSetDmaIntr` as SIFMAN ordinal 32. Its first two
arguments use the same DMA descriptor transport as ordinal 7;
arguments 3/4 supply an original IOP guest callback and argument.
Only after the DMA is committed does the reference queue a callback
job for 32 IOP cycles later. Our native kernel already supports
bounded HLE clock events and real one-shot R3000 guest callbacks.

This batch exports only `sifman#32` with a dedicated guest R3000
stub at `0x00018060`; the default extra ordinal mapping would land
at `0x00018b00`, owned by SIFCMD, and must not be used. Both #7
and #32 use the existing staged, range-checked and armed EE SIF0
DMA5 path. A non-null #32 callback is validated as executable
IOP RAM and preflighted for finite event capacity **before** any EE
destination write. Following genuine DMA5 completion, schedule the
provided callback for 32 original IOP guest-clock ticks, preserving
its caller GP and argument; callback dispatch creates no synthetic
EE/IOP readiness or DMA completion. Null callbacks behave exactly
like plain #7. Reset clears pending HLE events.

Regressions cover the newly resolved IRX import, invalid callback,
unarmed receiver, real guest R3000 callback writing its argument
and GP after the delayed event, no early callback, and fail-closed
queue-capacity rejection. A bounded success trace reports the
first eight legitimate SIFMAN DMA commits.

The underlying virtual IOP clock is still approximate and the
callback runs in an existing one-shot guest R3000 thread rather
than bit-exact hardware interrupt nesting. Do not claim first-frame
success before a new retail replay. PR #145 remains draft and open.


### CI correction — stale unknown-import fixture after exporting #32

The first CI on commit `8f5219095f5c0aee37727ec703bb8823465faa74`
passed the dedicated SIF DMA acceptance job and the ROM0 job, but
`CTest (game-1)` failed in
`tests/tools/test_game_runner.py::make_sif_guest_irx(unknown_import=True)`.
The fixture still designated `sifman#32` as deliberately missing.
Since #32 is now correctly installed, the synthetic IRX linked and
executed the new import with an invalid zero descriptor count, stopping
at `0x00018068`. This is **correct fail-closed DMA handling**, not a
new retail regression or evidence that PADMAN failed.

Update the negative fixture to unimplemented `sifman#33` and
assert ordinal 33 remains absent from the SIFMAN import table. The
actual negative test remains meaningful: unresolved guest IRX imports
must fail linkage rather than execute or alter EE readiness. No HLE,
DMA, guest execution, or callback behavior is changed by the fix.



## 2026-11-20 — PADMAN RPC initialization: StartThread(0) and stack allocation parity

User-provided matched `quick-probe(3).log` and `quick-probe(3).json`
show **five distinct auto-expansion replays, 0..4**. The JSON is the
stopped final replay 4: 3,641,108 EE dispatches, 80,330,773 EE guest
instruction attempts, 15 GS fields and a final EE exception vector
`0x80000180` (EPC `0x00174ce4`, SYSCALL). It is **not** an
unsupported EE opcode. The artifacts do not attest their local
checkout SHA. Remote PR HEAD at analysis start was
`15470dde571c9c769938078dbbc0ecfd70e4b844`.

The earlier genuine 96 KiB SIFMAN DMA now logs
`service=0x494f0007, thread=24, bytes=98304, dma_id=1`
at cycle 6,458,047. `PADMAN.IRX` loads with `load_status=0`,
`link_status=0`, `unresolved=(none)#0` as module 7/thread 52.
Its original module entry returns `0` at cycle **9,930,180**.
Original PADMAN RPC server `0x80000100` method 1 returns tokens
11, 12 and 13, with `guest_result=0x001eed80` each. Token 14
enters the original IOP guest handler but never returns in the
last replay.

The new first **guest** failure comes from the original PADMAN
worker: `WaitSema HLE guest IOP stopped: id=8 reason=2
thread=57 pc=0x000142e8 opcode=0x0000000c`.
The recorded context has `v0=0x494f1506`,
`a0=0`, `a1=0`, `ra=0x001e7978`: an imported
**`thbase#6 StartThread(0,0)`**, not a missing library.
Thread 57 is faulted while another newly created PADMAN thread
63 is still ready. Several newly allocated guest stacks sit near
the 2 MiB IOP RAM ceiling. This suggests a preceding failed
CreateThread, but the uploaded logs do **not** capture its
arguments or exact failure reason, so do not assert it as proven.

The reference `ps2-web-recomp/SDBZ/runtime/src/iop/kernel.cpp`
uses `alloc(stack_size,1)` for original R3000 thread stacks:
the **highest** suitable physical RAM gap, with **256-byte** size
rounding. Our native runtime had incorrectly allocated stacks with
low-RAM `allocate()`, the same first-fit path used for IRX
images and IOPHEAP; that is an independently verified allocator
parity discrepancy. Correct only thread stack allocation to a
high-gap, 256-byte-aligned bounded operation, keeping module/heap
allocations low and all owner/free bookkeeping intact. Invalid
overlarge guest stack sizes still fail closed, without wrapping.

The reference `runtime/src/iop/libs.cpp` reports
`KE_UNKNOWN_THID = -407` for an unrecognized `StartThread`
target, rather than faulting the interpreter. The native bridge
now returns that negative guest error for nonexistent/zero IDs,
and does not start any thread. Already active targets cannot be
restarted and produce a negative invalid-state result. A valid
CreateThread descriptor that cannot allocate a real stack returns
`KE_NO_MEMORY = -400`, preserving guest control flow while
**not** fabricating an ID. Bounded diagnostics record the exact
guest entry, requested stack, priority, available heap sizes and
IOP cycles to validate the next retail failure if one remains.

Regression tests cover upper-RAM stack allocation, 256-byte size
rounding, low-heap independence, free-gap reuse, oversized input
failure, correct negative StartThread(0)/unknown-ID results via
original R3000 guest import calls, and finite no-memory
CreateThread returns. No EE DMA, SIF RPC, semaphore, IRQ or
callback success is fabricated. A new retail run is required to
establish whether the PADMAN token-14 guest handler completes.


### CTest fixture correction: SIFCMD guest O32 stack arguments

The first CI for upper-RAM stacks on
`fa05655de197ad12e6c794b951cf4ff0f06a81ed` failed
only `iop_sif_rpc_bridge` in CTest `fast-1`. Its synthetic
SIFCMD #12 guest test wrote fifth and sixth O32 arguments at
`initial_sp + 16/20` without an original guest stack-frame
prologue. When the new reference-parity stack allocator gave the
first thread the highest IOP RAM stack ending at `0x00200000`,
those writes landed outside physical RAM. The downstream SIFCMD
transport correctly rejected the resulting absent stack arguments.

Fix the *test* by adding a real R3000 `addiu sp,sp,-64`
prologue before the import `jal`, place the stacked arguments at
`initial_sp - 64 + 16/20`, and update the intended BREAK PC
from program+20 to program+24. The guest now actually owns its
O32 call frame. Do not weaken bounded IOP memory writes or move
the allocation policy back to low RAM. CI retests all workflows
on the updated commit.


## 2026-11-20 — 11:56 full quick-probe ZIP: CDVDFSV RTC CALL header.opt=1

The uploaded archive `ps2-game-hle(20261120-115659).zip` contains
matching `quick-probe.log` (501,219,902 bytes uncompressed) and
`quick-probe.json`. Log parsing counted **229 distinct EE execution
replays** across expansion passes; `auto-expand: appended` reports
**287 new, genuinely emitted functions**, taking the linked library
from 457 to **744 function modules** by its last successful native
link. This is not 229 independent novel function translations;
table-based batches account for more than one function in some
replays. The final JSON applies exclusively to the **last replay
(replay 31 of pass 3)**: 3,649,155 EE dispatches, 80,493,570
guest instruction attempts, 15 GS fields and EE exception vector
`0x80000180` with Cause=32 (SYSCALL at EPC 0x00174d64).
The final attempt is not a guest screen/frame success.

**Confirmed progress over the preceding log:** PADMAN.IRX loads as
module 7 and its original IOP entry returns 0. Real PADMAN guest
server `0x80000100` RPC token **14 returns** from its R3000
handler at cycle **9,935,640**. A subsequent ARKD guest server
`0x00000501` method 18 RPC is queued as token 15. There are
repeated `thbase#6 StartThread(0)` errors (the native bridge now
returns the genuine negative error -407). The latest guest context
shows three ready threads and zero faulted IOP threads. Do **not**
invent successful thread starts to suppress these errors.

**First concrete new transport blocker:** the genuine retail EE
SIF1 DMA descriptor
`source=0x20561940 destination=0x0001e000 bytes=64 attrs=0x44`
carries SIFRPC CALL words:

```text
00000040 00000000 8000000a 00000001
00010005 20561940 00000025 00464dc0
00000001 00000000 00464440 00000010
00000001 2caca93f 00000000 00000000
```

This decodes as CDVDFSV `0x80000593`, method 1 (RTC),
zero send bytes, 16 reply bytes, normal recv_mode 1, with
**`Header.opt=1`** in packet word 3. The existing
`c_sif_rpc_bridge::prepare_command()` unconditionally rejected
word 3 if nonzero as `invalid_envelope`, even though the
`ps2-web-recomp/SDBZ/runtime/src/iop/sif.cpp` RPC call
dispatcher does not demand `Header.opt==0`. Result:
`unsupported_rpc_packet` rejects the entire descriptor
transaction, which is **not** a legitimate completion.

Implement only the verified narrow compatibility rule: permit
`Header.opt=1` for SIFRPC **CALL** packets, while continuing to
reject unsupported option values and nonzero BIND options; all
other guest address, body-length, method, server, bounded result
buffer and DMA preflight checks remain intact. This permits the
already existing, media-backed CDVD RTC method 1 to execute
through a *real guest SIF1 descriptor* rather than fabricating
its result. A synthetic 64-byte retail-shaped guest packet
regression tests positive end-to-end transaction, while
option 2 CALL and option 1 BIND tests must still fail closed.

The EE exception after that 229-replay sequence is distinct from
this rejected transport. The next retail replay must identify
whether further original ARKD RPC work becomes reachable after
accepting this packet; no first rendered frame has been proven.


## 2026-11-20 — 12:18 native controller transport gap, SIO2 batch

The matched archive `ps2-game-hle(20261120-121854).zip` contains a
115,454,273-byte `quick-probe.log` and `quick-probe.json`. The
driver performed **52 independent replays**, with 51 real IR append
transactions adding **86 modules**, for **830** linked functions.
Final replay reaches the requested **4,000,000 dispatch limit**
(`exit=4, DISPATCH status=1`), 84,007,184 guest instruction
attempts, 16 GS fields. This is **not** an unhandled syscall,
finished boot or first rendered frame.

**Previous fix verified on actual retail transport:** CDVDFSV
`server=0x80000593 method=1` commits as sequence **41**, returns
`1`, and the earlier `invalid_envelope` SIF1 error is absent.
Genuine PADMAN guest RPC 14 still completes. The next ARKD
`server=0x501 method=18`, token **15** is queued on every replay,
but not entered/returned. IOP thread **12** is ready, priority
**50**; PADMAN threads **57 and 63** are ready, priority **46**
(smaller priority wins). The final IOP kernel has 7 ready, 16
blocked, 0 faulted threads. The observed strict priority may
starve the ARKD worker; do not bypass or synthesize its completion.

The missing hardware producer is concrete, independent of possible
guest-script loops: native `src/iop/iop_hw.cpp` only recorded SIO2
TX FIFO, SEND3, DMA11/12 and CTRL registers; it returned no RX bytes,
no completed transfer and no IRQ17. The primary reference
`twist84/ps2-web-recomp`, `SDBZ` branch,
`runtime/src/iop/sio2.cpp`, has a virtual controller in port 0
with neutral (unpressed) input, executes real guest SEND3-described
port commands, performs IOP RAM DMA11/12 and signals IRQ17 when
the guest starts a completed SIO2 exchange. It does not require
fabricating an EE RPC reply or changing priorities.

This batch implements bounded guest-driven SIO2 controller
initialization/protocol/DMA, with no memory-card backend. Controller
responses are produced only for valid actual guest FIFO/DMA bytes;
other ports and card slot remain explicitly absent. DMA channels
complete and IRQ17 raises **only** after an actual CTRL.start event,
never when CHCR/CTRL is merely configured. Invalid memory spans
fail closed. The relevant IOP hardware smoke tests cover genuine
FIFO and DMA exchange as well as no-producer/no-IRQ conditions.

This is a transport gap fix with inferred boot relevance. The next
real SDBZ replay must independently establish whether PADMAN
actually reaches this bus path and whether ARKD token 15 completes;
neither is claimed as already verified.


## 2026-11-20 — 13:06 quick-probe: original StartThread(0) semantics

Matched `ps2-game-hle(20261120-130603).zip` contains
`quick-probe.log` (2,426,359 bytes) and its last-replay
`quick-probe.json`. This is a **single replay**, not an
expansion sequence: zero new IR functions; 830 already-compiled
functions; **4,000,000** EE dispatches; **84,007,184** guest
instruction attempts; 16 GS fields; final DISPATCH/budget stop
at `0x00178de8` (same as preceding 12:18 run).
CDVDFSV RTC RPC sequence 41 still succeeds; PADMAN RPC token
14 returns. ARKD RPC token 15 (`0x501` method 18) queues but
does not enter the guest server's original handler. The final
IOP has 7 ready, 16 blocked and 0 faulted threads.

The first 12 bounded `thbase#6 StartThread(0)` logs, interleaved
from PADMAN threads 57 and 63, all incorrectly returned -407
(`unknown_thread`). **No `CreateThread rejected` lines**
appear; the earlier assumption that zero necessarily resulted
from a failed allocation was unsupported.

Reference inspection:
- `twist84/ps2-web-recomp/SDBZ/runtime/src/iop/kernel.cpp`,
  `Thread* thread(int id)`: **id==0 maps to current()**.
- `runtime/src/iop/libs.cpp`, `thbase#6/#7`: calls
  `thread(arg0)` then `start_thread(t->id,...)` on that
  actual current guest, returning 0 for a found thread.
- `start_thread` resets the guest CPU entry context (PC,
  next PC, args, stack, RA and GP), restores initial priority
  and makes it ready. Thus zero is **not** an unknown thread
  while called inside a real IOP guest thread.

The native kernel now distinguishes an actual self restart
(`id==current_thread_id`, target status RUNNING) from
unknown IDs and attempts to start another non-dormant
thread. It reconstructs the original target's guest
entrypoint/stack context through `start_thread_with_arguments`
and defers loading the newly restarted context into the CPU
until the syscall boundary, preventing the existing native
R3000 run loop from overwriting the restarted context with
the old post-syscall continuation. Missing IDs still return
-407 and non-current non-dormant targets -414. A real R3000
test calls thbase StartThread(0), proves that the subsequent
old-code store never executes, and verifies entry arguments
are restored after self restart. No extra thread or synthetic
guest RPC completion is created.

This is a verified original-semantic discrepancy. Whether
PADMAN's original worker becomes blocked correctly and ARKD
RPC token 15 completes must be verified by a new retail run.


### Bounded SIO2 retail observability

The previous retail log had no SIO2 event trace, so the mere
presence of the new virtual-controller implementation did not
establish that PADMAN actually reached it. Report three
monotonically increasing device counters in the already-bounded
`IOP guest RPC pending snapshot` diagnostic (only when a real
guest RPC is outstanding): `sio2_ctrl_starts` counts actual
guest CTRL.start writes; `sio2_packets` counts successfully
parsed SEND3 packets; `sio2_completed` counts fully completed
controller transactions with IRQ17. The counters reset with
IOP reboot. PIO/DMA/regression tests assert the exact values,
including no phantom completion after invalid DMA source.
Counters do not create a device event, alter timing or log on
every controller request. In the next retail batch, zeros
indicate the device path was never used and nonzero values can
be correlated with PADMAN worker state. Neither outcome is
assumed here.

**CTest fixture clarification:** The real R3000 self-restart was
already functioning: the first `fast-2` diagnostic showed a genuine
`BREAK` at `0x00005034`, 19 executed instructions, persistent
marker 1 (old post-call store skipped), and correct exit RA
`0x00013fc0`. The test incorrectly expected `a1=0x37`, but
`StartThread(thread_id, arg)` passes the second API argument
as the restarted thread's **a0**, with **a1=0**; the implementation
correctly calls `start_thread_with_argument(id,arg)` which stores
`m_gpr[4]=arg`. Corrected the fixture to assert a0=0x37
and a1=0. No runtime semantic change is justified by this
synthetic test failure.


## 2026-11-20 — 13:24 matched retail replay, ARKD token 15 priority diagnosis

The exact uploaded batch `ps2-game-hle(20261120-132425).zip`
contains a `quick-probe.log` (2,424,926 bytes) and matching
`quick-probe.json`. It consists of **one replay**, no new
translations. The final JSON reports 830 linked EE functions,
4,000,000 dispatches, 84,007,184 EE guest instruction attempts,
16 GS fields, `DISPATCH` stop/status 1 and final PC
`0x00178de8`. The EE has exhausted the user-defined budget;
this does not prove rendering or exception handling.

The previous `StartThread(0)` fix took effect: **zero**
`IOP HLE StartThread rejected` and zero
`IOP HLE CreateThread rejected` events. CDVDFSV RTC method
1 still commits (seq 41, result 1), and genuine PADMAN guest
RPC token 14 completes, returning `0x001cfa80`. ARKD guest
RPC token 15 (`0x501`, method 18, worker 12) queues only;
there is no `entered` or `returned` event. Final
IOP state remains 7 ready / 16 blocked / 0 faulted.

The new SIO2 counters resolve the earlier assumption: all nine
bounded snapshots report **sio2_ctrl_starts=0**,
**sio2_packets=0**, **sio2_completed=0**. No executed
controller transaction can explain this specific stall.

At the final snapshot, ARKD worker **12** is ready at priority
**50**, while PADMAN guest workers **57 and 63** are ready
at priority **46**. Native `next_ready()` chooses minimum
priority value, consistent with the primary reference
`ps2-web-recomp/SDBZ/runtime/src/iop/kernel.cpp`
`best_ready()`. The original scheduler therefore allows
these higher-priority guest threads to consume the cycle budget.
Do not adjust their priority, round robin across priority
classes, or fabricate an ARKD reply to force progress.

Need direct evidence to tell whether PADMAN is repeatedly
self-restarting, executing a non-blocking R3000 loop, or
reaching a specific HLE service before it has an opportunity
to block. Add purely observational host-only cumulative
per-thread `guest_insns`, `slices`, `syscalls`,
`self_restarts`, `last_syscall`, `last_syscall_pc`,
`last_syscall_ra`, and `last_syscall_a0` fields to the
existing **bounded** outstanding-RPC thread snapshots, not to
the hot-path console or guest RAM. They are zero-initialized
on creation, naturally reset on kernel reboot, and never
affect guest registers, device status, HLE results, thread
priority or scheduling. The existing real R3000
`StartThread(0)` fixture now asserts that the self-restart
counter increases once, the HLE syscall count is one and
the original callsite/argument were recorded.

Next retail run needs to check the actual increases in
`guest_insns`, `self_restarts`, `last_syscall*` on
57/63 between genuine ARKD token 15 queue and final snapshot.
Only then decide whether a particular HLE behavior requires
a compatibility fix. The first rendered frame and token 15
completion remain unverified.

The final-snapshot opcode-window selection now takes the **lowest
numeric (highest runnable) priority** before sampling at most two
ready/running guest PCs, rather than the two lowest thread IDs.
This ensures PADMAN priority 46 guest code is captured ahead of
ARKD priority 50 when both are ready. The full bounded context
list still includes all non-dormant threads. This changes only
which real RAM words are printed at the final checkpoint, not the
kernel scheduler, guest memory or any device state.


## 2026-11-20 — 13:41 retail replay: PADMAN self-restart evidence

The matched `ps2-game-hle(20261120-134117).zip`
contains one 2,450,442-byte `quick-probe.log` and its
39,423-byte `quick-probe.json`. Zero new IR functions
were appended; final replay again has 830 linked
functions, exactly 4,000,000 EE dispatches, 84,007,184
guest instruction attempts, 16 GS fields and
`DISPATCH` budget stop at `0x00178de8`. CDVDFSV
RTC sequence 41 still returns result 1; PADMAN RPC
14 returns the original guest buffer pointer. ARKD
RPC token 15 (0x501 method 18, worker 12)
queues, but does NOT enter or return.

The new bounded thread counters supply the first concrete
scheduler evidence: PADMAN thread **57**, ready/priority 46,
executed **227,871** R3000 instructions in **177,422**
slices, **2,075** handled syscalls and **259**
original self-restarts. PADMAN thread **63**, same
priority/status, executed **227,429** instructions
in **177,370** slices, **2,073** handled syscalls
and **259** self-restarts. ARKD thread **12**,
ready/priority 50, remains at its earlier
**87 instructions**, 15 slices, 7 syscalls,
0 restarts. Strict native/reference priority
scheduling correctly prevents ARKD worker 12
from getting service while these PADMAN
threads remain ready. The 259 self-restarts
are real guest requests, not evidence that
host scheduler should fabricate RPC or raise
their priority. SIO2 starts/packets/completions
are still all **0**, so SIO2 transport has
not yet been invoked by the original PADMAN.

The last observed syscall on PADMAN 57 is
`0x494f1608` = `thevent#8 ClearEventFlag`.
Thread 63's is `0x494f1516` =
`thbase#22 ReferThreadStatus`. Their original
opcode windows are at `0x001c6de0` and
`0x001c8544` respectively, inside PADMAN
IRX. These observations identify original
guest behavior but do not prove the precise
condition that makes it repeat.

A separate reference parity omission was
confirmed by reading the actual
`ps2-web-recomp/SDBZ` kernel:
`create_thread` persists guest descriptor
`attr` and `option` in its Thread, and
`thbase#22/#23` reports them at offsets
0x00 and 0x04. Our native bridge ignored
both descriptor words and fabricated zeros
in `ReferThreadStatus`. Preserve the two
original guest fields through the thread
creation API (default zero for host-created
threads) and return them in the 80-byte
status record. Validate the whole five-word
guest creation descriptor before reading.
This is an independently justified semantic
fix, not a claim that it alone resolves PADMAN.

A regression now creates a real guest thread
with descriptor attr `0x02000000`
and option `0x13572468`, and verifies
those exact fields are returned by
`thbase#22` along with the original
dormant status and priority. Additional
bounded per-thread diagnostics retain
the original self-restart caller's `ra`
and new argument, to identify the exact
guest callsite on the next retail run.
Never overwrite original guest data,
change priorities, or invent DMA/RPC completion.


## 2026-11-20 — 14:04 matched replay: original ARKD RPC token15 and token16 returned

The uploaded `ps2-game-hle(20261120-140407).zip` has
`quick-probe.log` (106,271,472 uncompressed bytes),
`quick-probe.json` (39,511 bytes), and the translation
cache manifest. **48 separate EE replay entries** appear.
The final JSON applies only to the last replay:
`dispatches=3651691`, `guest_instruction_attempts=80525519`,
`gs_fields_started=15`, `exit_name=UNSUPPORTED`,
`final_pc=0x001069ec`, opcode `0x4bc4212a`.
The expansion footer confirms **78 new functions** in
the final replay (`replay=47`). The previous fixed
budget-stop/idle loop has been escaped.

Crucially, original guest ARKD RPC **token15**, server
`0x501`, method **18**, **queued AND entered** on
IOP worker 12 at handler `0x0006dc78`, and
**returned guest result 0** at cycle 9,952,841.
Original guest RPC **token16**, server `0x502`
method **5**, **queued, entered** handler `0x000712a8`
on IOP thread 13 and **returned 0** at cycle
9,957,343. This is actual guest progress, not an HLE
fabrication. The original PADMAN loop no longer starves
the ARKD worker. The most recent change corrected
`ReferThreadStatus`'s original `attr`/`option`
fields; the coincidence suggests a relation but
does not establish causality by itself.

The next genuine unsupported opcode **0x4bc4212a**
is **VMUL.xyz VF4,VF4,VF4**, COP2 SPECIAL1 funct
`0x2a`. The following original SDBZ US words
are `0x4b042101` VADDy.x,
`0x4b042102` VADDz.x,
`0x4a0403bd` VSQRT,
`0x4a0003bf` VWAITQ,
`0x4b000120` VADDq.x.
These form a scalar vector-length calculation;
they are part of the original EE guest code, not
an HLE service. Implemented all six encodings in
the existing VU0 runtime and LLVM IR builder:
lane mask and source-overlap-safe VMUL; VADDy/z
broadcast; VADDq from the *retired* Q register;
VSQRT using VU float normalization and source
absolute value with invalid status on negative
input; VWAITQ retires the pending result after
the documented **seven VU0 virtual cycles**.
Do not claim these are authoritative EE clock
cycles or synthesize scanout/IRQ events. An
unavailable VU0 microengine and unexpected
SPECIAL2 opcodes remain unsupported.

The real retail opcode regression now executes
this exact sequence starting from vector
(3,4,0,7) with an expected scalar length of
5 and a preserved W lane, checks pending-Q
visibility, original VF0/ACC behavior, flags,
negative VSQRT, invalid SPECIAL2 and concurrent
SQRT fail-closed behavior.

The decoder classification changed; the user's
preserved IR cache needs verification/rebuild
from the updated guest-lowering sources by
`scripts\run_ps2_game.bat --quick`. Avoid forcibly
reusing stale IR against changed COP2 semantics.
No successful game frame has been established.


## 2026-11-20 — 16:28 matched retail run: VRSQRT / VMULq normalization

The uploaded `ps2-game-hle(20261120-162811).zip` contains
`quick-probe.log` (121,890,490 bytes),
`quick-probe.json` (39,495 bytes), and a
120,813-byte translation-cache manifest.
It executed **55 expansion replay passes**, indices
0–54. The final footer reports
`replay=54 new_functions=62 dispatches=3654765
stop=0x00106988 exit=3`. Independently, the
actual uploaded translation-cache manifests contain
**908 IR hash entries** in the preceding 14:04
archive and **1,087** in this 16:28 archive: **180
new keys and one removed key, or +179 net**.
The footer's `new_functions=62` is not a
total cache-delta statistic and must not be
substituted for the manifest counts. Neither
cache inventory alone proves how many functions
executed. The final JSON is paired only with
the last replay; never aggregate its counters
across restart attempts.

The last JSON reports `UNSUPPORTED` at
`0x00106988`, opcode `0x4a6503be` (VU0
SPECIAL2 **VRSQRT**, index 0x3a), after
**3,654,765** EE dispatches and **80,575,633**
guest instruction attempts, with 15 GS fields.
This proves the previous `0x001069ec` VMUL
instruction is now executing, but no displayed
game frame is yet verified. Both genuine guest
RPCs still complete: ARKD server0x501 method18
token **15** entered/returned result zero
(cycle **9,952,841**), and server0x502 method5
token **16** entered/returned result zero
(cycle **9,957,346**). There is no fabricated
guest RPC completion, IRQ or SIO2 activity.

The actual EE function at `0x0010697c`
contains six original words:
`0x4bc4216a` VMUL.xyz VF5,VF4,VF4;
`0x4b052941` VADDy.x;
`0x4b052942` VADDz.x;
`0x4a6503be` VRSQRT Q,VF0.w,VF5.x;
`0x4a0003bf` VWAITQ;
`0x4bc0211c` VMULq.xyz VF4,VF4,Q.
Together they normalize VF4.xyz without changing
the W lane. This is original VU arithmetic,
not an HLE shortcut.

Implemented exact SPECIAL1 VMULq (funct 0x1c)
and SPECIAL2 VRSQRT (index 0x3a) in decoder,
LLVM macro lowering and existing VU0 helpers.
The Q pipeline now holds a *pending* reciprocal
result for **13 VU0 virtual cycles**, versus
7 for VSQRT. VMULq fails closed while Q is
pending, and VWAITQ retires the authentic
result. Zero denominator follows the original
PCSX2 VU interpreter: signed saturated max
or signed zero for 0/0, with current I/D
and sticky IS/DS flags; negative denominator
uses absolute value and sets invalid.
Source operands obey hardwired VF0.w=1 even
if VF0 backing is corrupted. Do not infer EE
hardware timing or scanout from these VU0
virtual cycles.

Smoke regression covers the actual six-word
sequence, vector (3,4,0) -> (0.6,0.8,0),
13-cycle Q, preserved VF4.w and ACC, premature
VMULq fail-closed, negative denominator, signed
zero, 0/0 and saturation; exhaustive decoder
tests and exact original-word lifter tests
include VRSQRT and VMULq. All other unimplemented
COP2 instructions stay UNSUPPORTED.
