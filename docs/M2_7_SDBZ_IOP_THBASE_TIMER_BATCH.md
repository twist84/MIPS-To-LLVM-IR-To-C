# SDBZ USA native boot — IOP THBASE timer batch

> **Record status (2026-11-25):** Historical THBASE service and ROM0/IOP boot checkpoint; later retail execution progressed past these module-load stops. See [current status](README.md), [retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md) and [accuracy plan](ACCURACY_PLAN.md). Branch names, open PR instructions, and pending gates in the historical text refer to the documented revision.


## Evidence (Windows 2026-11-19 22:41)

- Final replay: **14,385 dispatches**, versus the preceding 14,240 baseline.
- The installed guest syscall `0x56` (`_SetTLBEntry`) executed 48 indexed TLB writes without the prior `MTC0` unsupported stop.
- `SIO2MAN.IRX` succeeded (`load_status=0`, `link_status=0`, `module_started=true`).
- `MCMAN.IRX` did **not** load; final committed LOADFILE sequence 14 returned `load_status=7`, `link_status=5`, `unresolved=thbase#33`.
- The exit at EE `EPC=0x001748C4` is syscall `v1=4` (`KExit`), not another unsupported COP0 instruction. This follows the failed module load and is not evidence that a new ISA instruction is missing.
- The three supplied 2026-11-19 captures (GS display, texture upload, WebGPU raster) each contain only black pixels. Counters show GS events and one GPU draw, not a successful game frame.

The final replay metadata matches the same USA `SLUS_214.42` executable and disc folder as the static IRX catalog. Intermediate auto-expansion replays are not counted as successful module loads.

## Implemented service family

In addition to the existing `thbase#4`, `#6`, `#20`, the R3000-callable HLE bridge now provides:

| Ordinal | PS2SDK operation | Behavior |
|---|---|---|
| 33 | `DelayThread` | Blocks a real guest IOP thread on the scheduler's virtual clock |
| 34 | `GetSystemTime` | Writes the current modeled clock to a bounded guest `iop_sys_clock_t` |
| 35 | `SetAlarm` | Registers a timed real guest R3000 callback, with duplicate/invalid registrations rejected |
| 37 | `CancelAlarm` | Removes a pending callback or marks a running callback for cancellation |
| 39 | `USec2SysClock` | Converts using the PS2SDK 4608/125 tick ratio |
| 40 | `SysClock2USec` | Converts bounded guest clock data into seconds and microseconds |

Timer callback `$v0 = 0` removes the alarm. A nonzero return re-arms it by the callback's requested virtual tick duration. The code executes actual IOP guest instructions with a bounded callback return trap, and does not synthesize a hardware DMA completion, controller response, module startup, IRQ, or framebuffer.

## Architectural limits (important)

- **Timing is not calibrated to the PS2 IOP timer.** The current IOP scheduler accounts guest instruction slots as its virtual clock; the 4608/125 conversion is the PS2SDK clock-unit conversion, not proof of wall-clock accuracy.
- Alarm callbacks currently run through an isolated scheduled guest execution context. They are **not** real interrupt-context callbacks. They cannot yet model full IOP interrupt nesting, timer hardware, or precise EE/IOP synchronization. This is a functional HLE bridge, not cycle-accurate timing.
- Interrupt-context `thbase#36` (`iSetAlarm`) and `thbase#38` (`iCancelAlarm`) remain unregistered and fail closed.
- Invalid guest pointers, unknown ordinals, duplicate alarm pairs, allocation failures and guest callback faults do not produce successful replies.
- `MCMAN.IRX` linking on the next real Windows boot is **not established** by CI; that must be checked against a newly matched diagnostic batch.

## Regression checks

`iop_kernel_hle_bridge_test` now exercises the MCMAN timer import family, clock conversion, modeled time, delayed thread resume, R3000 callback execution/disarm, and pending alarm cancellation. The original SIO2MAN static-IRX import/link tests remain in place.

Further work: validate MCMAN entrypoint and post-link guest instructions, calibrate IOP time against the EE timing model, implement true interrupt-context callbacks, and preserve fail-closed behavior for unknown hardware.
