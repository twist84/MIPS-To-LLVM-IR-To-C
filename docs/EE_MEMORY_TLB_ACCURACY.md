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

## Strict memory follow-up M2: left/right partial transfers

The EE Instruction Set Manual v6.0 LWL/LWR (pp. 80–83), SDL/SDR
(pp. 99–102), SWL/SWR (pp. 117–120) specifies original-EA translation
without natural-alignment faults, and store translation for the
partial stores. Retain the original address for BadVAddr, aligned
container addresses for physical/scratchpad backing, explicit memory
byte masks and register lane ranges. LWL sign-extends the merged
word; LWR sign-extends only when the entire word is loaded. The upper
64 GPR bits are outside the transfer. No host byte order assumption
or actual load/merge/store is introduced.
https://studylib.net/doc/28989088/ee-insns

Pinned PCSX2 R5900OpcodeImpl.cpp reads an aligned word/doubleword and
merges it for partial stores. That implementation technique is not a
license to issue a guest MMIO read; the strict plan requests only store
permission and enabled write lanes. Pinned ps2-web-recomp codegen
delegates these instructions to left/right helpers. Neither reference
is treated as measured bus timing. C++ constexpr/static_assert and
runtime cases exercise every offset/direction (48 combinations), lane
masks, sign extension eligibility, clean-page reads versus modified
stores, original fault addresses, privilege and scratchpad boundary.

This planner does not issue bus transactions, perform register merges,
change HLE aliases or enable strict memory execution. Cache/bus lane
semantics and precise commitment still require separate validation.
No retail replay required for M2; CI pending.

## Strict memory follow-up M3: instruction fetch classification

EE Core User's Manual v6.0 sections 4.2.7-4.2.9 define word-aligned
instruction addresses and fetch/load TLB fault codes. Config (p.78)
sets ICE at bit 17, overriding the segment/TLB cache attribute when
disabled; DCE is independent. Stage 2Q (p.19) describes instruction
selection via the SPRAM S bit. Sections 5.2.5 and 6.4 establish SPRAM
mapping and data access, not a validated instruction-fetch model.
https://docs.alexrp.com/mips/ee.pdf

Add a constexpr single-instruction fetch planner with explicit
cached-physical, instruction-cache-bypass physical, fault and unmodeled
routes. Dirty is not a fetch permission; retain the original PC and
raw cache attribute. ICE does not override privilege or translation
faults. Do not assume accelerated data-buffer behavior for instruction
fetch, or treat speculative/pair/line fetches as architectural requests.
Valid SPRAM translations are retained separately as unvalidated fetch
routes rather than redirected to RAM or assigned an invented exception.
Unmapped HLE scratchpad aliases do not bypass strict translation.

Tests cover all K0 encodings and ICE/DCE combinations, clean cached
fetches, misalignment before lookup, invalid/refill and nested vectors,
ASID, privilege, ERL, reserved mode and scratchpad edges. Compile-time
and runtime checks share the constexpr fixture. No instruction bytes
are read, no cache lines or ITLB timing simulated, and no production
caller is enabled. No retail replay required for M3; CI pending.

## Strict memory follow-up M4: physical spans and cache policies

EE Core User's Manual v6.0 Config (p.78), 5.2.6 (p.126) and
6.3.8 (p.138) distinguish DCE, K0=0, ordinary uncached, cached
writeback/allocate and accelerated UCAB/WBB behavior. Data policy
classification preserves the mapping attribute when DCE is disabled;
it never equates bypassing the data cache with bypassing all ordering
or buffer effects.
https://docs.alexrp.com/mips/ee.pdf

Add a separate constexpr bus-span planner for an already translated
data access, using a bounded 32 MiB retail main RAM aperture plus
caller-supplied backing size. The map is intentionally conservative:
the reference memory.cpp mirrors RAM throughout the low 256 MiB and
normalizes HLE aliases, neither of which is adopted here. The local
runtime's VU0 apertures and broad device dispatch window are recognized
as candidate regions, not proof that the device/width is implemented.
The reference also recognizes the 4 MiB ROM0 aperture.
https://github.com/twist84/ps2-web-recomp/blob/985c0ac15f2622199f90b206bac3db89268e2f22/runtime/src/memory.cpp
https://psi-rockin.github.io/ps2tek/index.html

A scratchpad offset is separate from any physical address. Unknown
regions, absent host RAM backing, invalid widths, wraparound and
aperture crossings cannot silently become RAM. Partial transfers use
only their enabled contiguous byte lanes. Compile-time/runtime tests
cover cache encodings and enable bits, each modeled aperture boundary,
missing backing, alias rejection and partial transfer spans.

No pointer, bus transaction, read/modify/write, cache contents,
writeback, ordering, device permission or synthesized DBE is provided.
The classification is not a complete physical bus map. No production
memory or ABI changes; no retail replay required for M4. CI pending.

## Strict memory follow-up M5: precise Level 1 fault commitment

The existing code-only ee_exception_vector() maps TLBL/TLBS to the
refill vector for outer entry. It cannot distinguish a TLB miss from
a matched invalid page. The strict path consumes a freshly computed
instruction-specific fault, preserving this distinction (EE Core
User's Manual v6.0, sections 4.1.2, 4.2.6–4.2.9).
https://docs.alexrp.com/mips/ee.pdf

Add a host-only strict fault commit API with explicit caller-supplied
restored-boundary evidence, ordinary or adjacent delay-slot PC and
live dispatch state. Recompute against the current TLB and COP0
registers rather than accepting a stale fault projection. Commit
Status.EXL, Cause.ExcCode, BadVAddr, Context/EntryHi for TLB faults,
PC/vector, and the exception exit. Preserve nested EPC/BD together,
pending IP, independent Level 2 fields, ErrorEPC, other COP0
registers and GPRs. Keep actual trap PC/delay-slot diagnostics.
Misaligned ordinary fetch PCs remain representable. ERL is explicitly
deferred rather than guessing Level 1/Level 2 interactions.

Tests cover 80 outer/nested, BEV, actual/saved delay-slot and
fault-type combinations, plus bad requests, missing boundary evidence,
ERL, duplicate entry, misaligned fetch and live TLB re-evaluation.
Both compile-time and runtime fixtures exercise the same transaction.
No production caller, legacy helper or ABI is changed. This does not
prove caller rollback, compute elapsed clocks, read/write guest memory
or dispatch a handler; the execution front end owns those steps.
M6 must integrate a bounded opt-in path, explicitly route real guest
handlers and prove rejected accesses leave no partial side effects
before any compatibility-path activation. No retail replay required
for this commit. CI pending.
