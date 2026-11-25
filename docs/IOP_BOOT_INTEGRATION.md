# IOP boot integration for SDBZ (US)

> **Record status (2026-11-25):** Initial firmware/IOP integration record. The HLE-first runner now executes original game IRX modules and has reached training gameplay; the initial SMFLAG stop is no longer the current blocker. See [current status](README.md), [retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md) and [accuracy plan](ACCURACY_PLAN.md). Branch names, open PR instructions, and pending gates in the historical text refer to the documented revision.


Status: staged integration; **the IOP is not yet booted by `ee_game_runner`**.

## Reproduced stop state (2026-11-16)

The native EE runner reaches 6,945 dispatches and stops at guest syscall
`0x001750B4`, which executes `sceSifGetReg(4)` in a tight three-dispatch
loop. `4` denotes IOP-owned SMFLAG. The value remains zero because there
is no IOP producer. With `--sif-poll-limit=256`, the runner now stops with
`EE_EXIT_UNSUPPORTED` and reports the actual stalled syscall PC instead of
silently running to the 100,000-dispatch budget.

**Do not simply change the SMFLAG reset value in `ee_sif.h`.**
That would bypass this synchronization point without implementing the
subsequent IOP-side command buffer, DMA and RPC services.

## Reference implementation

`twist84/ps2-web-recomp`, `SDBZ` branch:

- `runtime/src/iop/cpu.cpp`: MIPS I/R3000 interpreter.
- `runtime/src/iop/kernel.cpp`: IOP thread/event scheduler, IRX module
  loading, and `iop::run(ee_cycles)` (EE:IOP clock conversion of 8:1).
- `runtime/src/iop/sif.cpp`: IOP command handling, DMA and RPC delivery,
  Arika command dispatch; `runtime/src/iop/arika_dvd.cpp` handles game assets.
- `runtime/src/iop/sif.cpp::reset()` explicitly initializes
  `g_smflag = 0x60000`. **This is an HLE bootstrap value**, not evidence
  that a genuine R3000 thread produced the flag. Do not rebrand copying it
  into our EE runner as full IOP emulation.

## Integration milestones

1. **Standalone CPU execution (current PR).** `iop::c_cpu` owns 2 MiB of
   isolated IOP RAM and executes a bounded MIPS I subset with native MIPS
   branch/delay behavior, HI/LO, unaligned loads/stores, and explicit traps.
   Unsupported MMIO, kernel syscalls and instructions fail closed. This does
   not create SIF state or execute any module in the game runner.
2. **IOP kernel and IRX loader (in progress).** The standalone
   `load_irx()` now validates Sony IOP ELF32 modules, copies PT_LOAD into
   isolated IOP RAM, zero-fills BSS, applies the MIPS R_32/R_26/HI16/LO16
   relocation types for IRX v1/v2, resolves import stubs through an explicit
   caller-provided callback, and rejects malformed/unknown imports before
   modifying memory. It takes an explicit preallocated module base. Next:
   the new standalone `c_kernel` supplies first-fit IOP RAM allocation,
   a concrete export registry for IRX import linking, module registration,
   dormant/ready/blocked/faulted threads, deterministic priority/round-robin
   time slicing, and an explicit fail-closed syscall callback. It can run
   linked guest IRX code through `c_cpu`, but does not implement the actual
   PS2 IOP kernel syscall tables, interrupt controller, real IRX services,
   or host-facing EE/SIF integration. Use real game module bytes supplied
   at runtime, not committed copyrighted binaries.
3. **Cycle/event integration.** Advance IOP execution from an explicit,
   deterministic EE-cycle source. Do not derive hardware cycles from native
   dispatch count: one dispatch can execute widely different numbers of
   guest instructions. Make thread/scheduler state separately reportable.
4. **SIF producer connection.** Wire IOP-owned SMCOM/SMFLAG writes into the
   *existing* `c_ee_sif::publish_iop_subaddr()` and
   `publish_iop_smflag()` hooks, and deliver EE-owned MSCOM/MSFLAG
   notifications in the opposite direction. Ensure writes are ordered
   against EE SIF register syscalls and MMIO reads.
5. **SIF DMA/commands/RPC.** Implement real packet transfers and the
   required module services. The completed SMFLAG handshake is only the
   first dependency; the game will need SIFCMD and SIFRPC services.
6. **Validation.** Keep the synthetic CPU and SIF tests, capture an actual
   PCSX2 SIF trace to compare command order and data, and verify SDBZ can
   progress past `0x001750B4` without pre-setting any IOP-owned ready bit.

## Separation of responsibilities

The `ee_recomp` LLVM generator, EE native function metadata and
`verify_ir.py` are unchanged by the first milestone. The new IOP core is
a separate native CMake target and uses no game-specific executable hashes
or function addresses. No unsupported opcode is treated as NOP, and no
successful IOP boot is reported until the IOP kernel actually runs.


## IRX loader contract

`iop::load_irx(cpu, elf_bytes, guest_base, module, resolver)` never starts
a module automatically. The kernel will supply the allocated 16-byte-aligned
base and the import resolver. Missing imports are an error rather than a
fake-returning HLE trampoline. The staging image is fully validated and
relocated before being committed to IOP RAM; invalid inputs do not partially
modify RAM. The first supported format is a single PT_LOAD plus PT_SCE_IOPMOD,
with SHT_REL relocations. Multi-PT_LOAD layouts and unhandled relocation
types are rejected instead of silently approximated.

The `iop_irx_loader` CTest uses synthetic, non-copyrighted ELF fixtures
covering both IRX variants, MIPS relocation, zero-filled BSS, executable
entry code, import resolution and no-write-on-failure behavior. The separate
IOP CPU and loader are still intentionally not linked into the EE runner.

**Boot status as of the 04:20 US run:** 6,945 EE dispatches,
`sceSifGetReg(4)` at `0x001750B4` returns SMFLAG=0. The independent
IOP CPU and IRX parser cannot yet satisfy this wait.

## Kernel module and thread execution milestone

The standalone `src/iop/iop_kernel.{h,cpp}` composes the existing R3000
interpreter and IRX loader. It owns 2 MiB of isolated guest IOP RAM,
allocates module images and stacks inside the user area, resolves only
explicitly registered IOP library exports and offers a host-injected
syscall handler. Without a handler, encountering an R3000 syscall stops
at that syscall's real guest PC with `unhandled_syscall`. There are no
implicit kernel success stubs, synthetic module-start responses or IOP
SMFLAG writes.

Threads have independent PC, GPR and COP0 state. The bounded scheduler
uses lowest-numbered priority first and rotates equal-priority ready
threads between fixed guest-instruction quanta. A thread may be blocked
until an explicit IOP cycle deadline or woken by a kernel event; guest
runtime state is saved at each switch. The IOP scheduler's cycle input
is abstract and explicit at this stage, **not** driven from the EE native
dispatch count. Cycle-accurate EE/IOP stepping and interrupt delivery
remain future integration work.

The new `iop_kernel` CTest covers failed-import rollback, actual IRX
module loading/entry execution, guest register and stack setup, thread
priorities, round-robin scheduling, explicit syscalls, and unsupported
service traps. The standalone kernel is deliberately **not** invoked by
`scripts\run_sdbz_native.bat` yet, so current US boot logs are expected to remain
at the same `0x001750B4` SIF wait.


## Guest-driven IOP SIFMAN register bridge (PR #69)

The new `iop::c_sifman_bridge` registers actual guest-callable MIPS I
import stubs for `sifman` ordinals 23 (`sceSifGetSMFlag`) and 24
(`sceSifSetSMFlag`), matching the service table in
`ps2-web-recomp/SDBZ/runtime/src/iop/sif.cpp`. Only an **executing IOP
guest thread** can invoke these stubs and reach the explicit HLE syscall
handler. A call to SetSMFlag publishes the guest's A0 bitmask into the
existing `ee::c_ee_sif::publish_iop_smflag()`; GetSMFlag reads the same
EE-visible register, so EE acknowledgement/clearing operates on shared
state.

Installation itself writes no ready bits and the IOP reset state remains
SMFLAG=0. Unknown IOP syscalls are still unhandled. This intentionally
models only two documented `sifman` services, **not** SIF0/SIF1 packet
DMA, SIFCMD/RPC, real ROM/IOP boot or full `sifman` coverage.

The `iop_sifman_bridge` CTest runs a synthetic IRX whose actual R3000
instructions call both imported `sifman` functions. The check verifies
that SMFLAG stays zero until those guest instructions run, then observes
the published bits from the EE side and EE-only clear/ack behaviour.
The bridge remains a separate opt-in component, not a synthetic IOP boot
inside `scripts\run_sdbz_native.bat`.

To progress SDBZ beyond `0x001750B4`, load and schedule authentic
IOP boot services/modules at a defined source of EE cycles, then install
this SIFMAN bridge for services those modules actually call. Do not
prepopulate the SMFLAG register on startup merely to satisfy the EE poll.


## Opt-in EE/IOP guest rendezvous (PR #70)

`ee_game_runner` and `tools/run_game.py` now accept
`--iop-irx=FILE` and `--iop-quantum=N` (default 256). When explicitly
requested, the runner loads the provided IOP IRX with the existing
transactional loader, registers its known SIFMAN ordinals, creates and
starts a real R3000 thread, and lets it execute up to N IOP instructions
when the EE reaches `sceSifGetReg(4)` with SMFLAG still zero. The EE then
re-reads the **same** `ee::c_ee_sif` instance that received the IOP
guest's `sceSifSetSMFlag` call. Unknown IOP imports, syscalls and CPU
faults still stop execution instead of inventing completion.

For a synthetic module or a user-supplied independently executable
IOP IRX, run:

```text
python tools/run_game.py PS2_GAME/SLUS_214.42 --build-dir build --work-dir build/sdbz --no-recompile --hle-startup --kseg-ram --sif-poll-limit=256 --iop-irx=path/to/module.irx --iop-quantum=256
```

The SDBZ .bat launcher does not supply an IOP IRX automatically. No
copyrighted module bytes are committed; the option requires a real
local file. Authentic game modules may depend on additional IOP
libraries and services not yet provided by this limited bridge.

**Timing limitation:** this schedules IOP instructions at a *host-visible
EE SIF polling rendezvous*. The native EE function dispatcher reports
function boundaries, not hardware cycles, so **this is not yet
cycle-accurate EE/IOP execution**. An accurate clock/event scheduler and
IOP boot module set are still required. The normal no-IOP path is
unchanged and continues to report SMFLAG=0 if nothing publishes it.

The end-to-end `game_runner_end_to_end` test now includes synthetic
EE and IOP ELF inputs to prove that executed R3000 guest code, rather
than an HLE reset constant, causes the EE to exit its SIF poll.


## Cross-module IOP IRX library exports (PR #71)

The IOP loader now recognizes 0x41C00000 export-library headers in
relocated module memory. The zero-terminated table at header+20 maps
ordinal N to a genuine relocated guest R3000 entry address, analogous
to the export tables used by the `ps2-web-recomp` IOP kernel.
Exports must have a valid library name and point within the module's
loaded RAM range. Duplicate ordinals, malformed pointers and unterminated
tables are rejected before committing the staged image.

`iop::c_kernel::load_module()` checks all discovered entries against
registered HLE and prior module exports before writing guest RAM. After
success, the kernel publishes the entries in its import-resolution
registry, so a *later* module can link against them without a fabricated
HLE return. It remains a strictly ordered, fail-closed module loader;
forward references and recursive dependencies require a future
multi-module linking pass.

The independent `iop_irx_export_link` regression builds two synthetic
IRX images: a provider with an exported real R3000 function and a
consumer importing it. The test checks a missing provider failure,
function-pointer relocation, RAM-safe duplicate/conflict rejection,
successful import resolution, and executing the provider's function
through a real consumer guest JAL/import stub.

Next: accept an ordered group of local IOP IRX paths in the EE runner,
then add the real IOP kernel-service imports and guest clock/event
scheduling required by SDBZ. The reference project's HLE-installed
IOP services remain separate from genuinely executed guest IRX exports.


## Ordered local IRX module list (PR #72)

The native runner accepts **repeatable** `--iop-irx=FILE` flags, or
`tools/run_game.py --iop-irx-list=FILE`, a UTF-8 text manifest containing
one path per line. Relative manifest entries resolve against the manifest's
directory; blank lines and lines beginning with `#` are ignored. The
two forms are mutually exclusive. Modules must appear in **dependency
order**: provider libraries first, consuming/entry IRX last. Missing
imports and conflicting exports fail before any module starts.

A Windows launcher example:

```bat
set "IOP_IRX_LIST=PS2_GAME\\iop-modules.txt"
set "IOP_QUANTUM=256"
scripts\run_sdbz_native.bat
```

Example `PS2_GAME/iop-modules.txt` contents:

```text
# Paths relative to this manifest
IOP/PROVIDER.IRX
IOP/CONSUMER.IRX
```

By default the **final listed module** starts as a guest R3000 thread;
earlier modules are linked so their exported functions can be called,
but their entrypoints are not executed automatically. Set
`IOP_START_ALL=1` in the launcher (or pass `--iop-start-all` to the
Python driver) to start a thread for **each** loaded module. All module
images are validated and linked before any thread starts. Running all
module entrypoints does *not* establish a real PS2 IOP boot lifecycle:
real modules may require kernel imports, initialization ordering, device
handlers, interrupts, IOP hardware MMIO and SIF RPC services that remain
unimplemented. A list is capped at 32 modules, with no automatic
dependency sorting or synthetic success for missing services.

The native end-to-end regression includes synthetic **provider and
consumer** IRXs. A real function exported by the provider returns
`0x60000`, which the consumer obtains through an IRX import and passes
to `sifman` ordinal 24. The EE then observes the guest-published SMFLAG
at its polling syscall. The test checks repeated arguments, manifest
paths with spaces, both start modes, reversed dependencies, duplicate
exports and missing/ambiguous manifests. No game binaries are committed.


## Source-grounded US ELF module-path discovery (PR #73)

Do **not** infer IOP module names or a load order from our existing
`IOP_IRX_LIST` testing manifest. The real game should request modules,
and the load order must ultimately come from runtime execution.

`tools/analyze_iop_module_paths.py` now reads the **local**
`PS2_GAME/SLUS_214.42` as a little-endian MIPS ELF32 executable. It
extracts printable NUL-terminated strings ending with `.IRX` (including
`cdrom0:` and `rom0:` device prefixes and optional `;1` version
suffixes) from file-backed PT_LOAD segments. It records each exact
string, ELF file offset and guest virtual address. An executable
PT_LOAD scan also identifies local `LUI`+`ADDIU/ORI` address
constructions targeting those strings, as well as the known LOADFILE
RPC server identifier `0x80000006` where statically constructed.
Guest pointer aliases are handled conservatively, and branches block
unsound address-construction pairing.

Example manual use:

```bat
python tools\analyze_iop_module_paths.py PS2_GAME\SLUS_214.42 --json build\sdbz\iop-module-paths.json
```

`scripts\run_sdbz_native.bat` generates this JSON automatically on each
local boot unless `SDBZ_IOP_DISCOVERY=0` is set. This scans the user's
own ELF and **does not require uploading or committing game data**.
The CI test generates a non-copyrighted synthetic ELF to verify
filename extraction, both address-construction forms, negative
ADDIU low halves, ELF bounds and refusal to infer cross-branch xrefs.

**Interpretation:** these are *candidate embedded IRX paths*, not a
confirmed set or sequence of `sceSifLoadModule` calls. A path may be
unreferenced, used only for logging, or constructed dynamically. IOP
modules may request other IRX modules, and module calls may be made
through pointers or routines not recognized by this narrow static pass.

The SDBZ reference implementation
`ps2-web-recomp/SDBZ/runtime/src/iop/sif.cpp::install_servers()`
handles LOADFILE SIF RPC server ID `0x80000006`: method `0x00`
reads the module path at request-buffer offset `+8` and argument
length at offset `+0`. Method `0x06` loads a module from an IOP
memory buffer rather than a path. **The actual next milestone is to
observe these RPC requests in the EE SIF transport**, record method,
path/buffer, arguments, guest caller and request ordering, and use
those *observed* requests to drive `c_kernel::load_module()`.
Simply scanning the ELF cannot complete this step; native SIF
DMA/CMD/RPC delivery is still required.


## Guest-triggered IOP to EE SIF0 DMA (PR #75)

The optional IOP R3000 kernel now links another explicit, executable
`sifman` import: ordinal **7** (`sceSifSetDma`). Its guest arguments
are `a0` = IOP address of an array of 16-byte transfer descriptors
(`src, dst, size, attributes`), `a1` = descriptor count. An actual
guest IOP import call must execute before any transfer occurs.

The bridge stages **every** source IOP RAM byte and validates all
guest descriptors and EE RAM destination spans before writing guest
memory; a malformed later descriptor cannot partially commit an
earlier transfer. The current bounded subset accepts 1-64 descriptors,
nonzero sizes no greater than 64 KiB each, zero attributes, word-aligned
guest source/destination addresses and known physical/KSEG aliases.
Unmapped IOP sources, EE destinations, unusual DMA attributes and
oversized transfers are rejected, not silently truncated or wrapped.

The IOP kernel must be explicitly connected to `ee::ee_memory` and
`ee::c_ee_dmac`. An EE SIF0 channel 5 receive chain must already have
`CHCR.STR` armed (the real EE `sceSifSetDChain` does this); only
then does the IOP guest's successful DMA copy its bytes into EE RAM
and latch a real channel-5 `D_STAT` completion. The guest receives a
monotonic DMA transaction ID; failure does not consume an ID. Existing
SMFLAG register ownership is unchanged, and no module startup
automatically creates packets, interrupts or readiness.

The new `iop_sif_dma` CTest executes a synthetic R3000 IRX calling
sifman #7 via the actual import resolver. It checks two IOP RAM-to-EE
RAM transfers, completion status, absence of a synthetic SMFLAG,
all-or-nothing rejection of invalid descriptors, an unarmed EE receive
channel, and an absent EE DMA backend.

**Remaining critical limits:** The bridge is synchronous, not
cycle-accurate and does not implement IOP boot ROM, SIF command framing,
packet queues, RPC transport or LOADFILE server handling. It cannot
advance the real US game past `0x001750B4` unless authentic IOP
initialization code is supplied and executed. Do not interpret the
successful synthetic transfer as completion of SIFCMD/RPC boot.
