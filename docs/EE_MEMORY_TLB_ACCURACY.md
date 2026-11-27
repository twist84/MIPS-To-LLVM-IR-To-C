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

## Strict memory follow-up M6A: isolated scalar instruction execution

Add an explicit opt-in single-instruction entry point to the bounded
RAM interpreter, not a new default memory mode. It fetches actual guest
instruction bytes via the strict fetch planner, decodes LB/LBU/LH/LHU/
LW/LWU/LD and SB/SH/SW/SD, resolves data translation and permission,
checks physical aperture and actual backing, and commits the register
or RAM update only after every check succeeds. It never reinterprets
a physical address through the HLE alias mapper. Sign extension,
little-endian bytes, GPR zero, and preservation of upper GPR halves
are explicit. A modeled fetch/data fault commits through M5 before
any data write; unsupported or unmodeled paths leave state unchanged.

Scope is deliberately limited to ordinary uncached physical RAM with
explicit caller evidence of an isolated ordinary dispatch boundary.
No pending cache/write-buffer or DMA/device effects, native code
coherence, elapsed-clock or interrupt work may be inferred from that
evidence. Cached (including disabled-cache attributes), accelerated,
scratchpad, ROM, MMIO, delay slots, ERL and other opcodes are deferred.
The caller owns clock publication and real handler dispatch. The old
RAM callback, native LLVM memory helpers and production front ends are
unchanged; no new retail replay or cache rebuild required for this
isolated opt-in path.

Runtime tests execute real little-endian opcodes from TLB-mapped
virtual addresses, all eleven scalar load/store forms, clean-page
store rejection, unaligned load, fetch ASID miss, zero-register load
fault, absent evidence, unsupported opcode, cached mappings, unbacked
physical RAM and HLE-only physical aliases. M1-M5 constexpr/static_assert
fixtures remain. CI pending. Next M6B: quadword and partial transfers
through this isolated executor, then bounded handler/ERET integration
and native strict lowering strategy without changing the default path.

## Strict memory follow-up M6B: isolated wide and partial transfers

Extend ee_execute_strict_ram_step with LQ/SQ and all eight left/right
load/store forms. Quadwords align the effective address before
translation and validate all sixteen bytes before either half commits.
Partial transfers use the M2 original-EA fault plan and M4 enabled-lane
span; a store writes only selected bytes, never an extra guest load.
Loads preserve unaffected register lanes and implement LWL/LWR word
sign extension; quadword zero-register stores source both halves as
zero. Pure byte-mask and merge helpers have static_assert coverage,
including the full-width (no shift by 64) case.

Runtime tests execute 32 quadword and 48 partial offset/direction
combinations, comparing partial results to independently expressed
shift/mask equations from pinned PCSX2 R5900OpcodeImpl.cpp. They also
cover GPR zero, modified-page faults (aligned SQ versus original
partial EA), partial load to zero with a refill, and a truncated
quadword backing that must not commit either half.
https://github.com/PCSX2/pcsx2/blob/34670c6151d28dee8284e90f7957e470fc03f9d3/pcsx2/R5900OpcodeImpl.cpp

The opt-in executor still requires isolated ordinary uncached RAM and
caller-owned dispatch/clock evidence. No delay slots, handler dispatch,
cache contents, scratchpad or MMIO transactions, native lowering or
default execution changes. CI pending; no retail replay needed.
Next: bounded guest exception handler and ERET execution.

## Strict memory follow-up M6C: bounded guest handler and ERET

Extend the isolated executor with explicit guest-handler evidence.
A pending Level 1 memory exception may enter its matching refill or
common vector only with EXL set, ERL clear, ordinary-boundary and
isolated-RAM evidence. Fetch still uses strict translation and physical
backing checks (the test vector is uncached KSEG0 RAM). Rejected first
instructions preserve the pending exception exit. No automatic HLE
dispatch or host-side TLB repair is performed.

Within the opt-in EXL handler, support restricted MFC0/MTC0, TLBWI,
Level 1 ERET, NOP, LUI, ORI and ADDIU. COP0 writes are limited to
validated Index, PageMask, EntryHi, ordinary EntryLo and EPC. No
Status/Cause writes, TLBWR, ERL return, user COP0 success, or general
privileged instruction emulation is implied. ERET uses the existing
architectural Level 1 return helper. PC/EXL checks are not independent
proof of caller ownership or absence of pending effects.

Runtime tests execute actual guest handler instructions for refill,
invalid and modified faults, reading BadVAddr/EntryHi, setting Index,
PageMask and EntryLo through guest MTC0, committing TLBWI and ERET,
and retrying the original LW/SW. The host never repairs the mapping
after the fault. Verify destination/memory preservation before
entry, handler opt-in, unsupported first instruction rollback,
restored user Status, and rejection of user-mode ERET.

Still a bounded isolated RAM pilot, not a general BIOS interpreter.
ROM/BEV1 execution, branch/delay slots, TLB hazards, clocks/interrupts,
cache and device state, production caller integration and native
LLVM lowering remain open. CI pending; no retail replay required.

## Strict memory follow-up M6D1: opt-in native LW/SW pilot

Add a separate single-instruction LLVM emitter for decoded LW/SW, not
a mode of the production module builder. Its distinct context ABI
requires isolated RAM and an ordinary dispatch boundary. The checked
preparation helper translates live instruction fetch, checks the
compiled PC/opcode against current memory, translates the data access
and validates the actual physical backing. M5 delivers faults before
any guest transfer. Stale instructions and unresolved backing reject
without changing CPU/RAM. The helper returns a short-lived validated
span only on success. No interpreter call performs the native transfer:
LLVM emits explicit little-endian byte loads/stores, signed LW result
and PC advance on the success edge. GPR zero still translates and
faults; a successful isolated RAM read to zero can be optimized away.

The MCJIT fixture executes real LLVM at O0 and the normal O2 pipeline,
with actual C++ memory/fault helpers, comparing CPU/COP0/RAM against
the independent strict interpreter. Four opcodes (LW/SW to/from r2 or
zero), twelve scenarios each, two optimization paths: 96 executions.
Cases include clean/invalid/missing mappings, misalignment, modified
stores, cached and missing backing, fetch ASID miss, evidence refusal,
stale opcode and mismatched PC. Unsupported instructions reject at
emission. This is a pilot: native multi-instruction CFG, delay-slot
precision, native handler dispatch, cache/device ordering, invalidation
and integration with the actual BIOS/retail runner are still open.
No production lowering selection or runtime ABI change. CI pending.

## Strict memory follow-up M6D2: remaining native GPR memory forms

Extend the experimental LLVM entry to all 21 integer GPR memory
opcodes: eleven scalar, LQ/SQ and eight left/right transfers. Shared
constexpr decode describes width, direction and transfer kind.
Keep the original word preparation symbol as a restricted wrapper.
The generalized preparation helper returns the first enabled byte
pointer and partial EA lane only after strict fetch, live-code check,
translation, permission and whole selected-span backing validation.

Native LLVM emits scalar byte assembly/sign extension, both quadword
halves, and partial register merge or selected-byte stores. Partial
lane variants are separate fixed-offset blocks: no dynamic shifts
by 64, no unselected-byte access, and no guest RMW read. An invalid
lane fails closed. PC advances only after the transfer. Neither the
interpreter nor a host memory operation substitutes for LLVM execution.

The existing MCJIT test now covers every opcode, register 2 and zero,
O0/O2, the twelve original scenarios plus every offset in the
container. Compare full affected quadword, GPR and COP0 state against
the strict interpreter; independently require modeled fault/success
outcomes and complete quadword transfer. Existing M6A/B directed
fixtures remain. Production CFG lowering, delay slots, BIOS/retail
integration, cache/device state and native invalidation remain open.
No change to default lowering or existing retail ABI. CI pending.

## Strict memory follow-up M6D3A: precise native sequence boundaries

Factor the existing transfer lowering into reusable internal LLVM
functions. Add a bounded straight-line sequence emitter (1–256
contiguous supported GPR memory instructions, no address wrap).
Each instruction calls live preparation and commits its own PC,
GPR and RAM effects before entering the next block. Any nonzero
result exits immediately, preserving the completed prefix; no later
guest instruction is fetched or executed. Internal linkage permits
LLVM inlining without changing the checked helper ABI. Live opcode
comparison is repeated at every boundary, including after stores
to subsequent instruction bytes. The existing single-entry emitter
and retail lowering remain unchanged.

The MCJIT test adds eight three-instruction scenarios at O0/O2:
normal dependency chain across a virtual page, second-instruction
refill/alignment fault, third-instruction fetch invalid, stale second
or third opcode, first store overwriting the next instruction, and
first-instruction modified-page fault. Compare CPU/COP0 and the
entire RAM fixture against sequential strict interpreter execution,
with independent assertions for committed prefix, EPC, BadVAddr,
vector and untouched suffix. Explicitly reject branches, unsupported
opcodes and wrapping sequences.

This is a straight-line native CFG pilot, not general function CFG
integration. Branch/delay-slot precision, instruction/clock accounting,
native handler dispatch, BIOS/retail caller integration, caches and
devices remain open. No production change; CI pending.

## Strict memory follow-up M6D3B: non-linking branch/delay pairs

Add a separate native branch terminator emitter for BEQ/BNE/BEQL/BNEL
with one supported integer GPR memory slot. The branch fetch is checked
against the live translation/opcode before LLVM samples its two 64-bit
low-half operands. The selected target/fallthrough is captured before
the slot writes any GPR. Ordinary branches execute their slot even
when not taken; not-taken likely branches never call the slot fetch or
data helper. No target instruction is fetched here.

The delay helper recomputes the live slot fetch and data access while
the CPU remains at the side-effect-free branch restart PC. M5 receives
the actual slot PC and branch PC, selecting EPC/BD correctly, preserving
nested saved EPC/BD, and retaining actual-slot diagnostics. A rejected
slot leaves the branch restart state intact; a successful slot commits
the transfer before the parent publishes its selected next PC. The
existing ordinary single-step and sequence ABI remains available.

Directed MCJIT tests cover all four branch opcodes, a register or zero
comparison operand, LW/SW slots, 17 scenarios each, O0/O2 (544 cases).
Include low-64-bit comparison, slot overwriting a branch operand,
invalid or stale slot fetch, alignment/refill/modified faults, branch
fetch failure, nested BEV/BD, annulment, stale branch and host refusal.
Reject branch-in-slot, linking branches and unsupported sequence shapes.

This is not general CFG lowering: linking/indirect branches, arbitrary
slot instructions, integrated prefix/branch blocks, timing accounting,
actual BIOS/retail dispatch, cache/device state remain open. No default
path or production runtime ABI change. CI pending.

## Strict memory follow-up M6D3C: restartable jumps and terminated blocks

Extend the isolated branch emitter with J/JAL/JR/JALR. The EE
Instruction Set Manual v6.0, printed pp.65–68, specifies zero-extended
PC+8 links, distinct JALR source/destination registers, and target
alignment exceptions at subsequent fetch rather than at the jump.
https://studylib.net/doc/28989088/ee-insns

Capture indirect targets before the slot. Publish the low-half link
before the slot, preserve it on a guest exception (it is an earlier
restartable architectural effect), restore it on host refusal, and
leave upper halves and register zero untouched. Reject undefined
same-register JALR and reserved fields. No target fetch is invented.
Compose an existing bounded contiguous memory prefix with an existing
branch pair, keeping all per-instruction checked exits. This is
single-terminator block lowering, not arbitrary CFG joins/loops.

Extend directed native MCJIT tests with direct/indirect jumps, links
and zero-link destination, slot consumption/overwrite of the link,
target capture, misaligned target deferral, guest faults and host
refusals, both with and without a memory prefix, at O0/O2. The
existing single instruction, sequence and equality-branch tests
remain. No default lowering or production ABI change. CI pending;
conditional link branches, general CFG, device/cache/ROM handling,
clock accounting and actual BIOS dispatch remain open.

## Strict memory follow-up M6E1: immutable EE ROM0 backing and authentic fetch probe

Existing OpenPS2BIOS CI executes IOP R3000A firmware, not EE reset
code; its EE rendezvous uses synthetic code with authentic IOP
firmware. Keep original assertions and HLE-first retail unchanged.

Add an explicit immutable 4 MiB EE ROM0 view for translated,
uncached instruction fetch only. Strict interpreter and native
preparation consume it; no ROM pointer is exposed as mutable data.
ROM data access, cache/device state and ERL execution remain
unmodeled. Existing callers default to no ROM backing.

Run the existing three guest TLB repair/ERET handler scenarios
also with synthetic BEV1 ROM vectors. Check native preparation
from ROM code with RAM data, ROM store refusal, exact image size,
aperture edge, privilege and cached fetch refusal.

After the existing SHA256 verification, pinned OpenPS2BIOS v0.6
CI runs ee_rom0_fetch_probe on authentic reset/refill/common vector
bytes and archives the observed words/decode classifications.
It retires zero instructions: fetch validation, not firmware boot.
Next: examine observed reset words and introduce bounded reset
execution with verified COP0 initialization. No retail replay
required. CI pending.

## Strict memory follow-up M6E2A: authentic reset-entry retirement

M6E1 CI run 46040114327, job 140645391124, observed the pinned
OpenPS2BIOS v0.6 reset word 0x40806000 (MTC0 zero,Status) at
0xbfc00000, and zero words at the sampled BEV1 refill/common vectors.
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46040114327/job/140645391124

EE Core User's Manual v6.0 section 4.2.1 (p.95) defines reset
PC, Status.ERL/BEV, Random=47, Wired=0 and disabled Config controls.
https://docs.alexrp.com/mips/ee.pdf
The isolated reset-prefix state initializes these relevant fields;
other zero-initialized values are deterministic fixture choices,
not claims of defined hardware reset contents. No full CPU/device,
cache, debug or timer reset is claimed.

A narrowly scoped reset-entry executor fetches real ROM bytes and
retires only the observed MTC0 zero,Status. No other ERL instruction,
unknown reset state, missing ROM or unexpected opcode is accepted.
It clears Status and advances PC, preserving unrelated GPRs. The
existing general strict interpreter/native paths still refuse ERL.
No pipeline hazards, elapsed cycles or IRQs are synthesized.

The pinned ROM0 CI probe now requires this single retirement and
records the next PC/opcode plus fifteen further decoded words
without executing them. The one-instruction budget is an explicit
host stop, not a guest fault or successful boot. Synthetic tests
cover missing backing, wrong opcode, unsafe boundary, zero source
and repeat refusal. Next: use the actual lookahead to establish
the next bounded execution prefix. No retail replay required.

## Strict memory follow-up M6E2B: authentic processor-selection prefix

CI run 46041193709/job 140649051618 confirmed the first MTC0
retirement and exposed the next real firmware instructions.
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46041193709/job/140649051618

Use the reference-compatible R5900 PRId 0x00002e20 from PCSX2
cpuReset (not a universal physical revision claim).
https://sources.debian.org/src/pcsx2/1.6.0%2Bdfsg-2/pcsx2/R5900.cpp/

Add a bounded isolated post-Status-clear ROM register executor:
NOP, MFC0 PRId, ANDI, ORI, LUI, ADDIU, BEQ/BNE with a supported
register-only delay slot. Capture branch operands before the slot,
preserve upper GPR halves, respect architectural zero, count both
retirements, and refuse a slot without committing the branch if
unsupported, unavailable or over budget. No new guest exception,
clock, COP0 hazard, cache, device or arbitrary ERL behavior is
implied. This is interpreted instruction execution, not LLVM.

The authentic pinned firmware probe now requires twelve actual
retirements (including the initial MTC0 and taken BEQ slot),
PC 0xbfc00044, t0/t1 0x2e00, t8 0xffffffffa0010000 and
t7 0xffffffffbfc0012c. It records the actual successor and
sixteen lookahead words for the next batch. The budget is deliberate,
not a firmware fault or boot completion. A synthetic regression
covers this observed prefix, taken/not-taken delay slots, a slot
changing the compared register, budget refusal and unsupported slot.
Production runtime unchanged. CI pending; no retail replay.

## Strict memory follow-up M6E2C: authentic ROM data reads

CI at 1fcb692 confirmed all five workflows and the 12-retirement
processor selection checkpoint (PC bfc00044, PRId 2e20, t8
ffffffffa0010000, t7 ffffffffbfc0012c).
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46042781159/job/140654445133

The next actual firmware words set up pointers and issue LBU at
bfc00054, followed by BEQ and an ADDU-based byte comparison
sequence. Add a const ROM0 data-span accessor using the existing
translated bus/cache plan, canonical ADDU and LBU to the isolated
reset executor. Validate uncached physical ROM backing before the
read, including loads to zero. Preserve upper GPR halves. Reject
non-ROM/cached/unavailable data without inventing guest faults.
Non-linking delay-slot refusal preserves the branch restart state;
the diagnostic now identifies the actual slot opcode/PC and EA.

Keep the exact twelve-retirement checkpoint. Independently require
twenty actual retirements and a first LBU value matching the
unmodified image byte at offset 0x1000; the following BEQ must
select bfc000bc (zero byte) or bfc00064 (nonzero byte). Then explore
until the first host refusal or a 65,536-instruction total budget,
logging the last 32 committed steps, actual stop and GPRs, and
bounded decode lookahead. Reaching a diagnostic stop is not a
successful boot. The probe does not synthesize an MMIO response,
write ROM, or execute RAM instructions. Existing native/HLE
execution remains unchanged.

Directed synthetic tests cover LBU negative displacement, zero
extension, zero destination, ADDU wrapping/sign extension, upper
GPR preservation, cached/RAM/unmapped data refusal, final ROM byte
and successful/refused delay-slot reads. CI pending. No retail
replay required; use actual firmware stop for the next checkpoint.
