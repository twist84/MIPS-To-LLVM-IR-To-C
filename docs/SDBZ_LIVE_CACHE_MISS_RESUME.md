# Live EE translation-cache miss recovery

## Goal

When native execution reaches an executable guest PC absent from the linked
LLVM-IR translation set, keep the *same* guest process alive and resume from
that exact PC when the missing function has been compiled. This avoids repeating
menus or controller inputs in each auto-expansion pass.

This is a **live in-process checkpoint**, **not a disk savestate**. It retains
all previously initialized objects in memory (EE/IOP CPU and RAM, kernel
threads, pending SIF/RPC/DMA/interrupt work, GS/WebGPU state, controller, and
in-memory memory-card changes) rather than attempting to serialize and rebuild
their pointer graphs. An OS/process crash or an exhausted expansion budget
still requires a new replay. No cross-process restoration is claimed.

## How it works

1. The native runner executes at one-function dispatch granularity whenever
   live expansion is enabled.
2. A missing function returns `DISPATCH` or `RETURN`, `status=0` and
   `count=0` at the already-committed guest PC. The runner refuses recovery if
   the PC changed or any guest instruction was attempted during that invocation.
3. It publishes a uniquely numbered request in an isolated temporary
   directory, using an atomic rename. While it waits, guest threads, clocks,
   DMA, VBlank, input polls, GPU and memory-card services are not advanced.
4. `tools/run_game.py` validates that the guest PC is in an executable ELF
   segment and absent from the current compiled PC map, and verifies that the
   original ELF and translation-source fingerprints are unchanged.
5. It generates the missing function (with optional nearby anchored jump-table
   siblings), verifies appended LLVM IR, emits host objects, and links a new,
   uniquely named DLL/shared library. The stable dispatcher/runtime *object
   caches* remain reusable.
6. The runner opens the new library, confirms the `ee_game_run` export,
   swaps the loaded module, restores the pre-dispatch EE CPU snapshot, and
   invokes the new dispatcher **inside the same scheduler boundary**. No guest
   callback, CPU instruction, VBlank or controller input is repeated.
7. Once the process exits, the driver atomically publishes the latest library
   (plus a matching PDB when enabled) and updates the normal SHA-256-verified
   translation cache. Previously generated IR modules are not regenerated.

A compiler error, unexpected guest PC, stale input, malformed response or
invalid new library fails closed. A separate 30-minute request timeout guards
a compiler that disappears while the process is paused.

## Using it

The default auto-expansion mode now uses live resume:

```bat
git pull --ff-only
set "EXPANSION_PER_PASS=512"
set "MAX_DISPATCHES=6000000"
set "EE_WEBGPU_SNAPSHOT=1"
set "EE_WEBGPU_WINDOW=1"
scripts\run_ps2_game.bat --full
```

`EXPANSION_PER_PASS` ranges from 1 to 1024 (default 128). The expansion
budget is still bounded: when the configured limit is reached at another cache
miss, the runner stops and the boot-session controller can schedule a new pass,
which replays guest input. Use a larger budget when testing long menu sequences.

For direct Python runs, `--auto-expand=N` enables live mode; add
`--replay-after-expand` to opt back into the original restart-on-miss mode.
The native runner's `--hot-expand-dir=DIR` is an internal request/response
transport for the Python driver, not a standalone code compiler.

### Protocol

The native runner publishes `request-<N>.txt` atomically with two lines:
request number and guest PC (hexadecimal). The Python driver replies with
`response-<N>.txt`: the same number and an absolute compiled-library path,
`STOP` at the configured expansion limit, or `ERROR: ...` on failure.

The request directory is unique for each run and is deleted only after the
native process exits and unloads its hot libraries. The protocol never
contains a guest-state snapshot; the running process owns that state.

## Regression coverage

- `ee_live_cache_miss` tests two requests in one native child process,
  retention of simulated consumed input, bounded-stop behaviour and
  fail-closed compiler errors.
- `game_runner_end_to_end` exercises real LLVM emission and native hot
  relinking on a synthetic caller/callee ELF. It asserts that the guest entry
  PC executes once and the new function runs without launching a second
  process.
- **Retail SDBZ acceptance target:** Cross reaches the Auto-Save notice, hits
  missing function `0x00348570`, compiles it, then resumes into the
  health-warning screen without rebooting or requiring Cross again. This
  specific disc replay must still be validated on a user's original game
  files and the compiled Windows runtime.

## Persistent savestates are separate work

A durable save/load file needs versioned serialization for all EE and IOP
threads, EE/IOP RAM and scratchpad, pending interrupt/DMA/timer queues, SIF
RPCs, disc state, in-memory memory-card writes, gamepad polling state, and GS
VRAM/register state sufficient to rebuild WebGPU resources. It must also check
the original ELF digest, compiler ABI and translation-cache provenance.

Do not label the live in-process handoff as a persistent savestate or assume a
partial EE RAM snapshot is sufficient to restore a retail PS2 game.
