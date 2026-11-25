# M2.3 — 09:09 committed GS sprite provenance

> **Record status (2026-11-25):** Matched historical GS sprite capture; not a current rendering status. See [current status](README.md), [retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md) and [accuracy plan](ACCURACY_PLAN.md). Branch names, open PR instructions, and pending gates in the historical text refer to the documented revision.


**Target:** Super Dragon Ball Z USA, `SLUS_214.42`
**Original ELF SHA-256:** `1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`
**Matched input batch:** 19 November 2026, 09:09
**Reference:** `twist84/ps2-web-recomp:SDBZ`,
`runtime/src/gs/gs.cpp` and `runtime/src/gs/gpu/gpu.cpp`

## Observed results

The final native command really enables
`--gs-webgpu-snapshot`; WebGPU initializes, accepts GIF PATH3 events,
reports `draws=2275`, and returns the requested 512x448 offscreen
GPU-processed VRAM. None of that is proof of GS presentation
or reaching the original game's menu logic.

| Observation | Confirmed value |
|---|---|
| Runtime exit | 100,000 dispatch budget, final PC 0x00172e48 |
| Native reference-compatible fields | 230 (not calibrated EE cycles) |
| Accepted GIF tags | 4,584 |
| Formats | PACKED 4,568; IMAGE 16; REGLIST 0; IMAGE2 0 |
| GS PRIM writes | 2,275, every primitive type is 6 (sprite) |
| Vertex writes | 4,550 XYZ2 kicks, 0 ADC |
| A+D writes | 29,655 |
| Texture state writes | 0 TEX0/TEX1/TEX2; cannot claim textured menu work |
| IMAGE payload | 262,144 cumulative QWC = 4 MiB |
| Completed native WebGPU draws | 2,275 |
| CPU display VRAM 512x448 | 100% black |
| CPU upload VRAM 1024x64 | 100% black |
| WebGPU 512x448 | black except one pixel RGB(254,0,0) at (0,0) |
| GIF channel latest rejection | 0 |
| IOP load evidence | one successful requested SIO2MAN, 13 committed RPCs |

The host-draw count equals the 2,275 observed two-kick sprites.
This demonstrates a real GS sprite stream reaching the native GPU.
It does **not** demonstrate coloured, textured, visible or
correctly targeted scene rendering. The repeated GS state writes,
absence of TEX0/TEX1/TEX2 and black readback make the workload
consistent with clear/setup packets. Do not misdiagnose lack of
visible menu pixels as missing VU1 geometry without more evidence.

The raw register data required to distinguish **intended black
sprite colour**, **offscreen/mismatched FRAME/scanout target**,
**clipped sprite**, and **incorrect WebGPU rendering** was not present
in the 09:09 report. We do not infer which explanation is correct
from the counts alone.

## New bounded follow-up instrumentation

`s_ee_gs_state` captures only *committed* sprite-pair kicks
when two vertex registers have been consumed under PRIM type 6.
Each sample preserves:

- Raw PRIM and effective PRIM (PRMODECONT/PRMODE applied)
- Colour RGBAQ
- Both vertex XYZ register words
- XYOFFSET, SCISSOR, FRAME, ZBUF and TEST for selected GS context
- Monotonic committed sprite index and GS context

The `m2_gif_gs` report adds lifetime sprite-pair,
textured-sprite and nonblack-RGB-sprite counts, and bounded
`sprite_samples_first` and `sprite_samples_last`, retaining
eight records each. The sample format represents 64-bit raw
registers as hexadecimal strings to avoid JSON numeric
precision loss.

The implementation runs inside the pre-existing transactional
`c_ee_gs_state::consume`: if any later event in the same GIF
batch is rejected, no sprite sample or counter is committed.
The GPU mirror callback still observes only accepted batches.
It does not alter guest RAM/DMAC/GS timing or execution semantics.

### Acceptance gate

- Existing GIF/GS regression plus new sprite snapshot tests green.
- Full CI at exact commit green; PR #142 remains draft/unmerged.
- On next matched run, compare FIRST/LAST FRAME, XYOFFSET,
  scissor, RGBAQ and sprite coordinates with DISPFB/PMode
  and the native WebGPU readback.
- Continue IOP application progression research separately;
  do not fake RPC completions, VBlank, GPU draws or draw-visible
  substitutes to mask the actual first dependency.
