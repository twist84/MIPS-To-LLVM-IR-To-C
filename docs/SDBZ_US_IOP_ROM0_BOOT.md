# SDBZ US: authentic IOP ROM0 reset-vector bootstrap (experimental)

> **Record status (2026-11-25):** Historical opt-in ROM0 boot investigation; the current normal target is HLE-first, without ROM0. See [current status](README.md), [retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md) and [accuracy plan](ACCURACY_PLAN.md). Any branch, PR, pending replay or feature-status claims below refer to the documented stage.


## Why this exists

The SDBZ US EE has consistently stalled at `0x001750B4` polling
IOP-owned SMFLAG `0x00020000` while no IOP producer is running.
The game ELF's seven IRX path references **cannot** initialize a
PS2 IOP by themselves: its resident kernel is bootstrapped from
the PS2 BIOS. Previous IRX/SIF bridges have no firmware source.

PCSX2's IOP reset starts at `0xBFC00000` with COP0 Status
`0x10900000` and PRid `0x1F`. Its ROM0 is 4 MiB mapped at physical
`0x1FC00000` and KSEG0/KSEG1 aliases.

References:

- https://sources.debian.org/src/pcsx2/1.5.0~gfc1d9aef0%2Bdfsg-1/pcsx2/R3000A.cpp
- https://github.com/PCSX2/pcsx2/blob/master/pcsx2/Memory.cpp

## Implementation

- The R3000 CPU can map a caller-provided 4 MiB ROM0 image **read-only**
  at physical `0x1FC00000`, `0x9FC00000`, and `0xBFC00000`.
- The IOP ROM reset starts at `0xBFC00000`, resets IOP RAM and registers,
  and sets the verified COP0 Status and PRid.
- The IOP kernel executes ROM instructions directly in a bounded
  instruction budget. No invented module thread, HLE call result,
  readiness bit, or synthetic IOP init is involved.
- Unsupported MMIO, syscalls, and instructions remain hard stops
  with guest PC and opcode. Read-only ROM writes are rejected.
- ROM0 bootstrap and the existing `--iop-irx` experimental module
  bootstrap are deliberately mutually exclusive.

## Running with automatically managed OpenPS2BIOS

No BIOS data is bundled with this project. By default,
\`scripts\run_sdbz_native.bat\` checks the official
[TamaribaEmu/OpenPS2BIOS releases](https://github.com/TamaribaEmu/OpenPS2BIOS/releases)
for its latest **stable** release and downloads the matching 4 MiB ROM0
image to \`build/openps2bios/rom0.bin\`. It verifies the downloaded bytes
using the release's \`.sha256\` sidecar (or GitHub's SHA256 asset digest),
then writes a local version/asset/sha256 manifest. On later launches it
checks for a newer release and reuses the cached image if unchanged.
A verified cached image can also be used if the release API is unavailable.
A missing or corrupted cache is never executed. No Sony ROM is downloaded.

**Standard run (no manual ROM path):**

\`\`\`bat
scripts\run_sdbz_native.bat
\`\`\`

**Manual override (takes precedence, no automatic download):**

\`\`\`bat
set "SDBZ_IOP_ROM0=C:\path\to\your\bios-rom0.bin"
scripts\run_sdbz_native.bat
\`\`\`

Explicit \`IOP_IRX\` / \`IOP_IRX_LIST\` compatibility mode skips the
automatic ROM0 download unless the caller explicitly sets
\`SDBZ_IOP_ROM0\`; ROM0 and those legacy IRX bootstrap options are
otherwise mutually exclusive.

The environment variable is passed directly to \`ee_game_runner\` via
the unchanged \`tools/run_boot_session.py\` and \`run_game.py\`, so
downloaded BIOS version changes **do not invalidate** the existing
native EE guest translation cache. The direct \`ee_game_runner\`
executable also accepts \`--iop-rom0=FILE\`.

This is an **emulator-only** firmware image and is not suitable for
flashing to physical PS2 hardware.

## Real-game ROM0 scheduling and reproducible SIF readiness (PR #103)

The game runner now selects a 65,536-instruction *ROM0* quantum by
default (versus 256 for the explicit IRX/HLE mode). The EE's 256-read
SMFLAG stall guard previously permitted only 65,536 IOP instructions total,
whereas the standalone OpenPS2BIOS hardware integration grants 10 million
instructions before asserting guest firmware readiness. This is a bounded
deterministic rendezvous, **not** EE/IOP cycle-accurate scheduling.

The firmware guest runs its own `sifcmd` / `sifman` logic. Unlike explicit
IRX/HLE mode, authentic ROM0 mode does **not** install their compatibility
stubs into IOP RAM, does not create compatibility LOADFILE RPC responses, and
does not synthesize SIF readiness. On a failed poll guard it logs the actual
IOP PC, executed instruction count, receive buffer, DMA10 CHCR and hardware
SIF0/SIF1 byte counters to distinguish CPU progress from transport stalls.

The ROM0 guest instruction quantum defaults to 65,536 per unready
SMFLAG poll. To override the scheduler independently of the selected
ROM0 file, set \`SDBZ_IOP_ROM0_QUANTUM=NNN\` in the environment.
The batch script leaves both caller-provided variables intact.

The direct `ee_game_runner` command also accepts
`--iop-quantum=N`; that explicit argument takes precedence over the
environment variable. The existing `run_game.py` script intentionally
does not forward this option for ROM0; use the environment variable instead.

With ROM0 enabled, guest EE `sceSifSetDma` (syscall `0x77`)
now accepts bounded, prevalidated 16-byte descriptor lists. Its host
kernel boundary builds a real EE DMA6 source chain with the IOP DMA10
destination tags and quadword-padded snapshots of guest EE source bytes.
The existing DMA6 registers, FIFO and genuine IOP DMA10 receiver move
the bytes; there is no HLE command response or injected readiness.

Temporary staging borrows EE RAM at `0x01c00000` and restores its original
bytes before guest execution resumes. Each request is bounded to 64
descriptors and 64 KiB of staged packet data; only completed DMA IDs
return `SifDmaStat=-1`. This is synchronous host kernel marshalling,
**not** EE/IOP cycle-accurate scheduling.

The official OpenPS2BIOS v0.6 integration test now sends its genuine
LOADFILE `GET_VERSION` RPC_CALL using this same syscall adapter and
requires a firmware-generated SIF0 result. Smoke tests verify a 20-byte
request with zero-padded QWC, two descriptor transfers, refusal without an
IOP DMA10 receiver, invalid-attribute rejection, interrupt completion and
EE RAM restoration. The SDBZ US first genuine SifSetDma at EE EPC
`0x00175064` uses one 20-byte descriptor (source `0x005616c0`, IOP
receiver `0x00044f00`) with attributes `0x44`, the standard EE
SIFCMD combination `SIF_DMA_ERT | SIF_DMA_INT_O`. The syscall adapter
now accepts this terminal command form, emits distinct DMA10 END (bit 31)
and IRQ-on-tag (bit 30) flags, and still rejects unsupported attributes
and nonterminal command markers. The official-ROM0 EE syscall and firmware
SIFCMD tests exercise `0x44`; synthetic tests cover one and two
descriptors plus rejection without changing DMA state. The game boot must
be rerun locally to identify the next authentic stop; tests alone do not
establish complete boot or actual EE DMA5 interrupt delivery.

**This is still not a full SDBZ boot:** guest EE DMA5 interrupt handler
delivery, complete EE exception and interrupt scheduling, and disc-backed
LOADFILE module requests remain incomplete. Never replace guest firmware
readiness or replies with host-injected flags or synthetic packets.

## Remaining work

- Implement ROM0-visible IOP hardware registers, exception/interrupt
  handling, timers and loadcore/system services as boot traces require.
- Evolve the standalone reset-vector execution into real IOP kernel
  scheduling and firmware module startup without fake guest threads.
- Derive proper EE/IOP execution synchronization from guest cycles.
- Confirm that firmware-originated SIF writes, not host startup,
  publish actual readiness flags.

The runner can also be invoked directly without ROM0 or manual IRXs; that explicit minimal mode is unchanged.
