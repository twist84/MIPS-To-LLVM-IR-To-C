# Standalone IOP ROM0 diagnostics: OpenPS2BIOS-compatible

> **Record status (2026-11-25):** Historical optional OpenPS2BIOS diagnostic path, not the current BIOS-free launcher. See [current status](README.md), [retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md) and [accuracy plan](ACCURACY_PLAN.md). Any branch, PR, pending replay or feature-status claims below refer to the documented stage.


## Why

The [OpenPS2BIOS](https://github.com/TamaribaEmu/OpenPS2BIOS) project
provides an MIT-licensed, original PS2 BIOS replacement. Release v0.6
(5 November 2026) ships an exact 4,194,304-byte binary with a ROMDIR
containing the IOPBOOT module. Its `src/reset.S` differentiates the
R3000A IOP from the R5900 EE by COP0 PRid and loads the IOPBOOT
payload from ROM0 at the authentic `0xBFC00000` reset address.

The native EE game runner already accepts a caller-supplied ROM0 via
`SDBZ_IOP_ROM0` and executes it at SIF polling rendezvous points.
However, waiting for an EE boot loop makes each firmware investigation
unnecessarily expensive and hides the immediate IOP fault evidence.

This tool executes **only** the IOP R3000 reset-vector firmware. It
does not execute the EE BIOS, require the SDBZ game, imply firmware
readiness, or return success on a firmware stop.

## Obtain firmware

Use a binary from the project's [official v0.6 release page]
(https://github.com/TamaribaEmu/OpenPS2BIOS/releases/tag/v0.6):

- `OpenPS2BIOS-0.6.bin`: 4 MiB ROM0 image.
- `OpenPS2BIOS-0.6.bin.sha256`: matching SHA-256 checksum.

Verify the checksum before running. No downloaded firmware is checked
into this repository, even though the project uses the MIT license.

## Run (Windows CMD)

```bat
cmake --build build --target iop_rom0_runner
build\iop_rom0_runner.exe C:\Dev\OpenPS2BIOS-0.6.bin --instructions=200000 --trace-tail=64 --report=build\openps2bios-iop.json
```

Exit code `3` means the firmware stopped at a breakpoint, unknown
syscall, instruction, arithmetic exception or unmapped guest memory.
Exit code `4` means the instruction budget was reached. Neither code
indicates a successful BIOS boot. Invalid images or arguments return
`2`.

The JSON contains the reset and stopped PC, guest opcode, R3000 GPRs,
COP0 registers, a bounded instruction tail, and (on memory fault)
the **exact guest address, byte width, and read/write direction**.
This is more actionable than diagnosing an EE SMFLAG poll in isolation.

For integrated EE+IOP runs, set the existing environment option:

```bat
set "SDBZ_IOP_ROM0=C:\Dev\OpenPS2BIOS-0.6.bin"
```

Then rerun your existing game boot command without recompiling the EE
translation. This selects the authentic ROM0 reset-vector path, rather
than the experimental preloaded IRX bootstrap.

## What we do not yet emulate

OpenPS2BIOS has been tested mainly in PCSX2. Our R3000 CPU currently
implements only the bounded subset of IOP MMIO from PR #82 (scratchpad,
interrupt status/mask/control and cache-control register). It does
not yet implement the full interrupt exception lifecycle, IOP DMA,
timers, CD/DVD hardware or complete SIF interaction.

The next development target is the first specific unhandled ROM0
instruction or MMIO address in the captured `openps2bios-iop.json`.
Do not set SMFLAG bits or return fake device values to skip it.

## Reproducible test coverage

The automated `iop_rom0_runner` CTest uses only small original
synthetic instructions, not Sony ROM or OpenPS2BIOS binary content:

- Normal ROM0 -> scratchpad load/store -> guest breakpoint.
- Unsupported IOP timer read and write report the exact fault address.
- ROMDIR IOPBOOT metadata detection is informational.
- R3000 infinite boot loops stop at the explicit instruction budget.
- Invalid firmware sizes and options fail without a successful exit.

OpenPS2BIOS reference: https://github.com/TamaribaEmu/OpenPS2BIOS

## Build the patched OpenPS2BIOS v0.7 ROM

With GNU Make, Python, and MIPS-capable Clang, LLD and LLVM objcopy installed:

```sh
cmake -S . -B build -DEE_ENABLE_OPENPS2BIOS_SOURCE=ON
cmake --build build --target openps2bios_rom
```

The target fetches the pinned v0.7 source into `build/_deps/openps2bios-src`
and applies our disc launch patch. It writes the ROM to
`build/_deps/openps2bios-build/OpenPS2BIOS.bin`. Override the LLVM tool paths
with `OPENPS2BIOS_CLANG`, `OPENPS2BIOS_LLD`, and `OPENPS2BIOS_OBJCOPY`
when using versioned executables. The ROM0 workflow publishes the patched ROM
and its SHA-256 checksum as the `openps2bios-v07-patched-rom` artifact.
