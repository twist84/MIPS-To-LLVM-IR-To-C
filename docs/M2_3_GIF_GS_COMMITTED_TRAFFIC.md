# M2.3 — Committed GIF PATH3 traffic vs current GS primitive state

> **Record status (2026-11-25):** Historical instrumentation checkpoint; the later retail runner renders and presents real game frames. See [current status](README.md), [retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md) and [accuracy plan](ACCURACY_PLAN.md). Branch names, open PR instructions, and pending gates in the historical text refer to the documented revision.


**Status:** instrumented, synthetic-regression-ready; real SDBZ replay pending  
**Scope:** original USA `SLUS_214.42`, HLE-first native GS consumer  
**Primary reference:** `twist84/ps2-web-recomp:SDBZ`:
`runtime/src/gs/gs.cpp` (`write`, `write_packed`),
`runtime/src/gs/gif.cpp` (`walk`).

## Diagnostic correction

The 2026-11-19 07:27 log line
`GS PATH3 accepted: tags=4584 vertices=2 image_qwc=16384 ...`
was misinterpreted as cumulative vertex and image activity:

- `s_ee_gs_state::m_vertex_count` resets to **zero for every PRIM
  write**, then increases on XYZF2/XYZ2/XYZF3/XYZ3. It measures the
  **current primitive**, not the lifetime number of vertex writes.
- `s_ee_gs_state::m_image_qwc` and `m_image_data` reset for each
  accepted `TRXDIR=0` host-to-local transfer. `16384` is a
  **current transfer** counter, not cumulative uploaded image QWC.
- `m_tag_count` did not reset on PRIM/TRXDIR and remains a lifetime
  count of **committed PATH3 GIF tags**.

This matches the original `ps2-web-recomp:SDBZ` model: PRIM resets
`State::count` in `gs::write`, and XYZF2/XYZ2 supply a vertex kick
whereas XYZF3/XYZ3 are ADC/suppressed-kick writes.

Therefore **the prior 4584:2 ratio does not establish that GS only
received two lifetime vertices**. Nor does a black display prove the
GPU bridge is broken.

## New counters and boundaries

The `m2_gif_gs` report object (schema
`ee-m2-gif-gs-v1`) and concise console summary now distinguish:

| Metric | Semantics |
|---|---|
| `accepted_batches` | Completed GS event batches (including split packets), not DMA starts |
| `accepted_tags` | Accepted PATH3 GIF tags across the replay |
| `tag_formats[4]` | PACKED, REGLIST, IMAGE, IMAGE2 tags |
| `prim_writes_total` | Committed PRIM writes, including GIFtag PRE |
| `vertex_writes_total` | Committed GS XYZF2/XYZ2/XYZF3/XYZ3 register writes |
| `vertex_kicks_total` | XYZF2/XYZ2 writes only, following the reference vertex-kick semantics |
| `adc_vertices_total` | XYZF3/XYZ3 writes (ADC/non-kick) |
| `current_primitive_vertex_writes` | Existing state reset by PRIM (formerly ambiguous `vertices`) |
| `packed_events_total`, `packed_registers[16]` | Committed packed event distribution |
| `reglist_events_total` | Committed 64-bit REGLIST entries |
| `ad_events_total`, `ad_registers[128]` | Committed packed A+D GS-address distribution |
| `image_events_total`, `image_qwc_total` | Cumulative accepted IMAGE/IMAGE2 payload events and QWC |
| `image_qwc_current_transfer` | Existing per-transfer upload counter |
| `primitive_types[8]` | Committed PRIM writes by PRIM.prim bits [2:0] |

All cumulative counters update **only on transactionally committed
`c_ee_gs_state::consume` batches**. A failed batch rolls back earlier
tag/PRIM/vertex/image counter changes, GS writes and DMA completion.
The instrumentation does not execute guest instructions, alter
DMAC STR, schedule interrupts/VBlank, submit GPU draws or change
the GS pixel/transfer implementation.

**Limitation:** these counters only include GIF **PATH3**, not
VIF/VU1-generated PATH1/PATH2 packets. A nonzero vertex kick count
does not alone establish a completed GS primitive, rasterization,
display scanout or native WebGPU draw.

## Regression and advance gate

`ee_gs_gif_diagnostics_test` uses actual PATH3 parser + GS consumer
with two PRIM writes, XYZ2 kicks, an ADC XYZ3, REGLIST entries,
real A+D image setup, two successive IMAGE/IMAGE2 transfers,
and a rejected SIGNAL transaction. It asserts cumulative totals
survive PRIM/TRXDIR resets while rejection leaves counters unchanged.

**M2.3 remains partial** until a **new matched** SDBZ boot checks
tag formats, packed register histograms, image totals and true
lifetime vertex kicks. The final runner must actually include
`--gs-webgpu-snapshot` to attribute a GPU readback. If lifetime
vertex kicks are absent, focus on whether the game issued VIF/VU1
traffic; if kicks exist without WebGPU draws, audit the earliest
committed primitive/setup event versus the GPU bridge. Do not add
fake GS draws.
