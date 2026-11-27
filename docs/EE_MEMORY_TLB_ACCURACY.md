# EE memory, TLB and cache accuracy (#161)

## Baseline

The native HLE runtime retains a direct RAM/alias policy. The shared
48-entry TLB records guest TLBWI/TLBR/TLBP state but is not used for
general translated memory accesses. The old boolean translate helper
does not distinguish refill, invalid and modified failures.

## Batch 1: detailed TLB classification

Add an opt-in lookup returning physical address, entry index, page size,
odd/even selection and cache attribute, or a distinct failure category.
Use the existing supported PageMask encodings; both G bits must be set
to ignore ASID. EntryLo0.S identifies scratchpad mapping; defer those
mappings and reserved coherency attributes instead of treating them as RAM.
Multiple matches are rejected as ambiguous, not delivered as a fabricated
hardware exception. Tests cover seven page sizes, both halves and their
boundaries, last entry, ASID, G, valid/dirty, cache attribute and ambiguity.

Manual reference: EE Core User's Manual v6.0, EntryLo0/EntryLo1 p. 65.
https://docs.alexrp.com/mips/ee.pdf

These are directed model tests, not independent console captures. No
production guest memory path, TLB instruction, native ABI, or existing
legacy translate behavior is changed. Fault delivery and cache execution
remain separate tasks; do not automatically activate this lookup in HLE.

## Batch 2: privilege and segment routing

Add a side-effect-free address-space classifier from Figure 5-1 and
sections 5.1.4–5.1.6 (printed pp. 113–117). Normal low addresses and
allowed supervisor/kernel mapped segments require a TLB lookup. KSEG0
and KSEG1 route to the low 512 MiB physical aperture, with Config.K0
and fixed uncached mode respectively. ERL maps low addresses directly
and uncached; EXL/ERL override KSU. Reserved KSU and K0 encodings fail
closed. HLE-specific aliases are not assumed to be architectural mappings.
The classifier does not determine physical bus validity or model cache
contents. Directed tests cover 256 combinations of mode, exception level,
segment and boundary, plus eight Config.K0 values. No guest path uses
this classifier yet; no new retail replay required.

## Batch 3: distinguish Config.K0 and EntryLo.C

The manual's Config table (printed p. 78) explicitly permits K0=0
(cached without writeback or write allocation), unlike EntryLo.C=0.
The classifier preserves that distinction. The TLB lookup rejects nonzero
reserved EntryLo fields separately from EntryLo0.S scratchpad selection;
EntryLo1 bit 31 is reserved, not a second scratchpad selector. Existing raw
TLB register storage remains unchanged. Neither classifier simulates the
instruction/data cache enable bits, cache contents, or bus effects yet.

## Batch 4: unified access and fault decision

Combine segment and TLB classification without performing a memory access.
Distinguish privilege errors, refill, invalid and modified. Unsupported
attributes or ambiguous matches remain unmodeled. For a well-formed entry,
V/D permission checks precede scratchpad/cache selection (manual section 5.2).
Add a pure fault plan with the correct common/refill vector under BEV/EXL,
Cause.ExcCode, BadVAddr, and Context.BadVPN2/EntryHi.VPN2 updates preserving
PTEBase/current ASID. Do not modify state or manufacture Random/EntryLo
values. Sections 4.2.6–4.2.9 and 3.2 specify these fields. Directed tests
cover the composed route, all BEV/EXL combinations, load/store distinctions
and rejected unsupported decisions. The active HLE memory path is unchanged.

## Batch 5: special scratchpad TLB mapping

Section 5.2.5 (p. 125) and section 6.4 specify a single aligned 16 KiB
SPRAM region, MASK=0, equal V/D in both halves, ignored PFN/C, uncached
attribute and 14-bit offset. The strict resolver now classifies valid
SPRAM separately from bus addresses and checks the entire region.
Malformed entries fail closed; overlapping entries remain ambiguous.
Tests cover both halves, boundaries, ASID, permissions, malformed
alignment/V/D and overlapping RAM. This supersedes earlier deferral of
well-formed SPRAM lookup; raw TLB register readback and active guest
memory behavior are unchanged.

## Batch 6: preserve architectural restart state on nested faults

This is the first live runtime behavior change in this PR and a prerequisite
for future precise TLB fault entry (#162). EE Core Manual section 4.1.2
pseudocode (pp. 90–91) and the explicit nested-refill rule (p. 101) preserve
both EPC and BD while EXL is set. Prior code preserved only EPC. Level 1
entry also retains the independent BD2/EXC2 fields. A host-only diagnostic
records the actual trap delay-slot flag beside cpu.m_pad; the two HLE
syscall checks use it in IRQ callback context rather than the saved
architectural BD. The diagnostic is appended in the existing runtime tail.
Tests cross BEV and original/nested delay-slot state, preserve BadVAddr and
ErrorEPC, and verify ERET resumes the original restart. No Level 2 event
producer, TLB access activation or cache execution is added. A fresh bounded
retail replay is required because the live exception/HLE path changed.

## Bounded retail validation — 2026-11-25 19:53

Archive `sdbz-graphics-pr148-neutral-20261125-195345144.zip`: O5,
16,250 cached IR modules, reference_compatibility clock, 40M dispatches,
DISPATCH stop at 0x001a9e20, no IOP guest fault or timing observer failure.
Native process wall 50.031 s; profiled runner 45.2860136 s, 883274.919124
 dispatches/s. 4,989 guest fields, 3,590/3,590 successful presents,
4,156,721 GPU draws. VU1: 84 programs, 360,944 runs, 414,570,669 pairs,
zero fallback, captures, rejected/discarded jobs or host failures; one job
pending at the bounded stop. Display PPMs byte-identical from the same
source. Guest PCM 3,993,490 frames, 1,938,208 nonzero, peak 32766.
SIO2 0 Cross-pressed polls out of 3,497; EE observer 165 Cross-bit-low
candidates and 218 analogue samples outside deadzone. Archive lacks explicit
Git HEAD and is not a matched performance comparison. Bounded retail evidence
only, not proof of strict TLB operation or measured hardware timing.

At code head `e70d1dc`, ROM0, SIF DMA, native WebGPU and original suite
workflows passed. Build/test fast-2 failed the pre-existing checked-IR fixture
because its nested trap expectation assumed BD was overwritten despite EXL.
The fixture now covers both initial BD states, both trap locations, and both
BEV values against the corrected architectural restart rule. No production
code changed in this follow-up; final CI must pass independently.

## Batch 7: read-only Cause and real interrupt lines

EE Core User's Manual v6.0 pp. 74–75 defines the three hardware interrupt
positions IP2/IP3/IP7 and marks all Cause fields read-only. The EE has no
software interrupt inputs. The shared MTC0 helper now leaves Cause unchanged;
legacy raw state with reserved IP bits cannot trigger the interrupt path.
Request/clear APIs accept only the three implemented hardware lines. The
legacy line enum and state layout are retained for compatibility. This does
not normalize raw reserved bits on readback or add CpU.CE generation, and
it does not replace device interrupt routing. Host tests cover every IP
position, out-of-range lines, and read-only Cause. Generated LLVM fixture
executes MTC0 Cause followed by MFC0 and checks exact readback and preserved
upper GPR halves. No new IR helper ABI or clock changes. Live exception and
interrupt semantics changed; a fresh retail replay is required after CI.

## Cause/IRQ retail validation — 2026-11-25 20:00

Archive `sdbz-graphics-pr148-neutral-20261125-200052476.zip`: O5,
16,250 cached IR modules, no-recompile path, reference_compatibility clock,
40M dispatches, budget stop with RETURN at 0x0019b134, no IOP guest fault
or timing observer failure. Native process wall 49.141 s; profiled runner
44.4880167 s, 899118.526001 dispatches/s. 4,572 guest fields, 3,287/3,287
successful presentations, 4,828,506 GPU draws. VU1: 84 programs, 402,657
native runs, 457,727,557 pairs, all submitted jobs collected; no pending,
fallback, capture, reject, discard or host failure. Display PPMs identical
from the same source. Guest PCM 3,659,546 frames, 1,777,917 nonzero, peak
32766; host audio disabled. SIO2 0 Cross-pressed polls out of 3,196;
EE observer 118 Cross-bit-low candidates and 284 analogue samples outside
deadzone. Archive has no explicit Git HEAD and workload is not matched.
This validates bounded compatibility-path execution, not hardware IRQ
latencies or general MMU correctness.

At code head `4ec5d1e`, [ROM0](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45690274802),
[SIF DMA](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45690274835) and
[original suite](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45690274885) succeeded.
Remaining CI pending when recorded. No production changes in this evidence
commit; no new replay needed solely for this documentation.

## Batch 8: read-only COP0 registers and defined Wired writes

EE Core User's Manual v6.0 pp. 64, 68–69 and 77 specifies read-only
Random, BadVAddr and PRId, and resetting Random to 47 on Wired writes.
Protect these registers in the shared guest write helper; hardware fault
entry can still update BadVAddr. For defined Wired values 0–47, reset Random
to 47. Out-of-range Wired behavior is undefined; preserve legacy raw storage
without inventing a reset. Do not install a cycle-based Random progression,
change the existing processor-revision initialization, or enable TLBWR.
Host tests exercise all 48 defined Wired values, read-only writes and real
fault entry. Generated LLVM tests cover MTC0/MFC0 and sign extension before
and after a Wired write. No new lowering or runtime layout changes. Retail
regression validation is required because guest COP0 behavior changed.

## Batch 9: reference compatibility budget alignment

Pinned reference: [ps2-web-recomp SDBZ 985c0ac](https://github.com/twist84/ps2-web-recomp/tree/985c0ac15f2622199f90b206bac3db89268e2f22).
[Codegen](https://github.com/twist84/ps2-web-recomp/blob/985c0ac15f2622199f90b206bac3db89268e2f22/recomp/codegen.cpp)
charges two slots for control transfers including likely annulment;
[kernel](https://github.com/twist84/ps2-web-recomp/blob/985c0ac15f2622199f90b206bac3db89268e2f22/runtime/src/kernel.cpp#L481-L483)
charges a default 300 units per syscall (reference environment override
not yet mirrored). Use one checked slot + idle + recognized HLE cost
budget for COP0 Count/performance, EE timers and scheduler publication,
with IOP catch-up following the published time. Publish HLE/RAM progress
at the next host continuation safepoint. Preserve trace ordinals and
attempt counts. This is a compatibility-policy change, not validated
hardware timing. Remaining differences: ERET zero-charge in reference,
block-local charging and preemption placement, scheduler quantum/idle
cap, syscall override and unrecognized service handling, VU timing,
scanout phase and device event ordering. Those are not claimed equivalent.
Fresh retail replay required; shared runtime header changed and IR source
provenance must be refreshed. Original 55-suite remains the acceptance gate.

## Native performance overlay

F9 toggles a hidden-by-default host performance overlay in the focused
native game window. The WebGPU blit composites a fixed-size glyph panel
inside the displayed viewport, independent of raylib's GL backbuffer,
including embedded and fullscreen presentation. Sample at 4 Hz only when
visible: presented FPS, guest fields/s, compatibility speed/target,
EE instruction attempts/s, dispatches/s, GS committed draws/s and optional
completed exclusive EE/IOP host scope percentages (profile off otherwise).
Rates reset on counter/time discontinuities. The next real guest present
shows/hides the panel; no extra field, swap or readback is requested.
Guest VRAM, GS captures and input state are unaffected. CPU unit tests cover
sampling, rate conversion, glyph packing and toggle/reset. Windows visual
validation is still required; no physical console measurement is implied.

### CI and presentation-counter follow-up

The clock-alignment head `f42255e` passed ROM0, SIF DMA, native WebGPU
and the original suite, but two CTest shards failed. The dispatcher fixture
still accepted reserved IRQ positions; its expected mask now matches the
three defined hardware lines. The IOP rendezvous fixture observed actual
SMFLAG readiness in guest GPR state, but the diagnostic existed only in
the fallback polling path. Trace a genuine zero-to-nonzero transition
around the normal scheduled IOP guest run as well; retain the original
readiness and ordered-module assertions. No SIF flag is synthesized.
The present routine now reports false on acquisition/view/present failure
instead of counting a swapchain recovery as successful FPS. The guest
field schedule and display compute submission on recovery remain intact.

## Strict memory follow-up M1: instruction-specific alignment

The EE Instruction Set Manual v6.0 specifies natural alignment for scalar
LH/LW/LD and SH/SW/SD; LQ/SQ clear the low four effective-address bits
before address translation (printed pp. 141, 287).
https://docs.alexrp.com/mips/ee_insns.pdf

Independent source checks: pinned ps2-web-recomp codegen Lq/Sq masks
the generated address with ~15u; pinned PCSX2 R5900OpcodeImpl.cpp
likewise masks LQ/SQ and checks scalar LH/LW/LD alignment.
https://github.com/twist84/ps2-web-recomp/blob/985c0ac15f2622199f90b206bac3db89268e2f22/recomp/codegen.cpp
https://github.com/PCSX2/pcsx2/blob/34670c6151d28dee8284e90f7957e470fc03f9d3/pcsx2/R5900OpcodeImpl.cpp

Add a constexpr data-access planner composing instruction width,
alignment, segment/TLB decision and fault projection. Preserve original EA
and normalized virtual address separately. Misaligned ordinary scalar
accesses never fall through to a TLB lookup; LQ/SQ translate the aligned
address, including any subsequent fault address. Tests cover all low
nibbles for five widths and both access directions, even/odd page
boundaries, refill versus alignment, modified, high-address alignment
and unsupported request kinds, with static_assert and runtime checks.
This is a pure plan, not a bus transaction or fault delivery. Fetch,
left/right partial transfers, VU transfers, cache operations, physical
apertures and precise state commitment remain separate tasks. No
production caller, ABI change, timing change or retail replay required.

### Next checkpoints

M2: audit partial transfer byte masks and store-only permission checks.
M3: classify instruction-fetch and scratchpad restrictions independently.
M4: establish physical bus span and cache-policy decisions, separating
strict translation from HLE aliases.
M5: compose precise Level 1 fault entry and side-effect ordering.
M6: opt-in native/interpreter integration only after matching directed
tests and bounded retail validation. Keep cache timing and physical
console calibration open; never substitute estimated cycles for evidence.
