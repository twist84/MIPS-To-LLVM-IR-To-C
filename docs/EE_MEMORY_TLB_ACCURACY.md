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

## Strict memory follow-up M6E2D: ROM directory word scan

M6E2C OpenPS2BIOS CI run 46043812695/job 140657921145 passed
the first ROM byte read (0x52 at bfc01000), then stopped after
27 retired instructions at bfc00090 / 8d0a000c (LW t2,12(t0)).
The first compared byte was 0x52, the sought byte 0x45.
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46043812695/job/140657921145

Extend the isolated reset executor with aligned little-endian LW
(sign-extended low 64-bit result), canonical AND and NOR (64-bit
logical operands). Reuse the data access/bus planner and const ROM
span. Misaligned, cached, unmapped or unbacked reads refuse even
for zero destinations; no architectural fault delivery is claimed.
Preserve upper GPR halves and reject nonzero reserved shift fields.

The pinned-image probe now requires 38 retirements through the
first complete directory mismatch/advance iteration, including
the backward BNE and its slot. Independently assemble the image
file-size word at offset 0x100c, verify the 16-byte rounding,
directory and file pointers, remaining entry count and next PC
bfc00054. Then retain the existing 65,536-instruction exploration
budget and diagnostic trace. Add independent synthetic tests for
signed LW, last ROM word, zero destination refusal, logical
width, reserved encoding and six directory file sizes.
The authentic checkpoint is not yet validated; CI pending.
No production native/HLE changes, RAM execution, device response
or retail replay.

## Strict memory follow-up M6E2E: ROM image bounds and isolated RAM stores

M6E2D CI run 46044342913/job 140659711102 validated the first
directory iteration and reached 183 retirements. Guest ROM scan
matched the entry at bfc01040, source bfc01090, length a3a8,
destination a0010000; next unsupported opcode was 014d682b
(SLTU) at bfc000d0.
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46044342913/job/140659711102

Add canonical unsigned 64-bit SLTU and SB to the reset interpreter.
SB requires explicit caller-owned backing and the existing write
translation, uncached canonical RAM bus classification and actual
allocation bounds. Do not accept ROM stores, devices, cached or
unmapped addresses. Preserve all non-written bytes. A rejected
slot leaves the branch restart state intact. No default RAM backing
or production path changes.

The pinned image probe independently requires the observed 183
retirements and matching directory record, then 199 retirements
through the first image byte read and bounds checks. The RAM
allocation is sentinel-initialized, not preloaded with firmware.
Explore up to one million actual retirements with explicit RAM
backing, reporting matched image byte count and contiguous prefix
against the immutable source. A match is a content comparison,
not a store count. No RAM instruction fetch, device access,
cache/clock simulation or synthetic boot result is introduced.

Directed tests cover full-width unsigned comparisons, byte copying,
negative displacement, zero-register stores, missing backing,
wrong regions, physical bounds, and successful/refused/budgeted
delay-slot stores. CI pending; no retail replay.

## Strict memory follow-up M6E2F: authentic EELOAD handoff

M6E2E CI run 46044834525/job 140661376485 verified 251574 retired
instructions, stopping at bfc00124 / 03000008 (JR t8), with all
41896 EELOAD bytes matching the ROM source and both guards a5.
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46044834525/job/140661376485

Share the canonical JR encoding predicate with native branch lowering.
Capture its low 32-bit target before the actual slot; retire the pair
only after a successful slot. A refused slot retains the branch PC.
Check alignment and backing at the next fetch, not speculatively.

The isolated reset executor now permits live instruction fetch from
explicitly supplied canonical uncached KSEG1 RAM via the architectural
fetch planner and physical span classifier. No cached or accelerated
aliases, devices, missing RAM backing, or ROM-source substitution.
Existing ROM data reads and isolated SB stores are unchanged.

The pinned probe requires exactly 251576 retirements at a0010000,
physical 00010000, a final two-instruction JR transaction, full image
comparison and unchanged guards. Decode live RAM entry and bounded
lookahead before a separate diagnostic exploration of up to 4096
additional retirements. This is not native firmware execution or
boot completion. The expected handoff remains pending CI validation.

Directed tests cover target capture before slot mutation, zero GPR,
reserved encodings, budget and refused slots, live guest-written
instruction bytes, missing/short backing, cached/unmapped/device
targets and deferred misalignment. No retail replay required.

## Strict memory follow-up M6E2G1: EELOAD Status and guest RAM clear

M6E2F passed CI run 46045545827/job 140663768635: exact 251576
retirements at a0010000, physical 00010000, matching 41896/41896
bytes and intact guards. The next LUI t0,7000 retired from live
guest-written RAM. Stop: 251577 retirements, a0010004 / 40886000
(MTC0 t0,Status), t0=70000000.
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46045545827/job/140663768635

Allow canonical MTC0 rt,Status with only CU0/CU1/CU2 bits set,
using the actual low 32-bit source and architectural zero. Keep
all other Status bits, other COP0 writes, interrupts, exception
levels, privilege changes and timing hazards outside this isolated
experiment. Permit subsequent strict steps within this subset.
No production runtime changes.

Require the Status checkpoint at 251578 retirements, pc a0010008.
Execute the authentic guest clear loop with the existing BEQ/SB/
ADDIU implementation. Require 653348 retirements at a001003c,
stack a007ffe0 and 80352 zero bytes in [0001a3b0,0002dd90), with
both adjacent sentinels intact and the copied image unchanged.
Check the initial range is entirely sentinel-filled. These values
are predictions from the observed instruction stream, pending CI.
Do not replace the loop with a host memset. Report the next actual
stop and bounded live lookahead. No retail replay required.

M6E2G1 CI run 46045990404/job 140665290554 confirmed the Status
checkpoint at 251578 retirements and reached a001003c, but the
clear assertion counted only 14816/80352 bytes. The independently
predicted addresses incorrectly treated the ADDIU immediates a3b0
and dd90 as unsigned. They are -5c50 and -2270 respectively,
so the actual physical range is [0001a3b0,0002dd90). The observed
14816 bytes equal the overlap of the two ranges. The retirement
count, loop size, stack expectation and all integrity checks
remain unchanged; the interpreter itself is not modified.
The corrected regression still requires authentic CI validation.
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46045990404/job/140665290554

## Strict memory follow-up M6E2G2: first EELOAD subroutine call

Corrected M6E2G1 passed CI run 46046418708/job 140666705245.
653348 retirements at a001003c, Status 70000000, 80352/80352
zero bytes, both guards a5, stack ffffffffa007ffe0, copied image
unchanged. Next blocker: 0c005877 (JAL a00161dc), slot NOP.
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46046418708/job/140666705245

Share direct jump target and zero-extended link address helpers
between isolated reset execution and native LLVM lowering (EE
Core Instruction Set Manual v6.0, J/JAL). The target takes its
upper nibble from PC+4. JAL updates the low 64-bit link before
the slot, leaves the high half unchanged, and restores the old
link on host-only refusal. No premature target fetch or invented
guest exception. J never modifies the link. Directed tests cover
budget, missing and unsupported slots, rejected stores, link
visibility, slot link overwrite, return and unbacked targets.

Require 653350 retirements, pc a00161dc, physical 000161dc,
ra 00000000a0010044 and unchanged stack. Fetch and decode the
live RAM callee, then explore a bounded 4096 further instructions
and report the actual next stop and lookahead. Pending CI; no
retail replay or production dispatch changes.

## Strict memory follow-up M6E2G3: EELOAD stack frame and RAM scalar transfers

M6E2G2 passed CI run 46046905599/job 140668315724: 653350
retirements at a00161dc, RA 00000000a0010044. The callee
allocated 56 bytes and stopped after 653351 retirements at
a00161e0 / afbf0034 (SW ra,52(sp)), SP ffffffffa007ffa8.
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46046905599/job/140668315724

Extend the isolated transfer path to aligned SW and allow LW/LBU
from explicit canonical uncached RAM. Reuse architectural access,
alignment and physical span classification; no cached, accelerated,
unmapped, device or partial backing accesses. SW writes only the
low 32 bits little-endian; LW sign-extends the result; LBU zero-
extends. Preserve register upper halves and architectural zero.
Refuse before modifying RAM or registers, including delay slots.

Require actual guest prologue stores at physical 0007ffd8 (s0=0)
and 0007ffdc (ra=a0010044), all neighboring stack bytes unchanged,
then the nested call and argument-producing slot. Expected at
653356 retirements: PC a0010528, RA 00000000a00161f4,
a0 ffffffffa00197e6, SP ffffffffa007ffa8. These are predicted
from the observed words, pending CI. No host stack preparation,
fake calls, production dispatch changes or retail replay.

## Strict memory follow-up M6E3A: first EE device access (SIO TX)

M6E2G3 passed CI run 46047391615/job 140669955776: stack stores
and nested call at 653356 retirements, then reached 653362 at
a0010540 / a0820000 (SB v0,0(a0)), address b000f180, byte 5b.
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46047391615/job/140669955776

PS2SDK identifies physical 1000f180 as SIO_TXFIFO:
https://ps2dev.github.io/ps2sdk/sio_8h.html
PS2tek also documents the one-byte console output register:
https://psi-rockin.github.io/ps2tek/

Model only ordered diagnostic byte delivery to an explicit bounded
caller-owned sink, not hardware FIFO timing or readiness. Require
canonical uncached translated DEVICE address and width one. Missing
or full sink, other registers, aliases, widths and reads refuse
without effects. No generic MMIO acceptance, fake read values,
interrupts or completion. Add canonical full-low-64-bit OR for the
observed string loop's delay slot.

The probe independently reads the NUL-terminated string from the
pinned ROM image at the copied EELOAD offset 97e6. Require each
guest-emitted byte to match it, exact retirement count 653364 +
5*length, return to a00161f4, and unchanged stack. Report the
actual bytes and subsequent blocker. The string and retirement
count remain pending CI validation. No retail replay required.

## Strict memory follow-up M6E3B: architectural Config fields

M6E3A passed CI run 46047885667/job 140671607827. The first
guest SIO string matched all 36 bytes, including newline:
[openbios:ee] OpenPS2BIOS 0.6 ready
Return at 653544 retirements, then stop at 653546 / a00161fc,
40038000 (MFC0 v1,Config).
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46047885667/job/140671607827

Correct reset Config from zero to fixed IC/DC fields 0x440 (EE
Core User's Manual v6.0 p.78; EC=0, IC=010, DC=001). K0 is
undefined at reset; select zero as in PCSX2. Writable fields
DIE/ICE/DCE/NBE/BPE/K0 are mask 0x73007; fixed fields ignore
writes, reserved bits and undefined K0 modes refuse. Canonical
MFC0/MTC0 use low-word architectural semantics.

Canonical SYNC.P/SYNC.L are admitted ONLY in the isolated,
synchronous executor where no outstanding transfers or pipeline
work exist. The manual requires synchronization around Config
changes; no live pipeline hazards or cache state are modeled.
Enabling Config does not authorize cached RAM, cached fetch,
or accelerated access. These remain refused.

Require guest Config read/modify/write and SYNC.P at 653553
retirements, pc a0016218, Config/v0=73443, v1=440; preserve
Status and console byte count. Pending CI. No retail replay.
Reference: https://docs.alexrp.com/mips/ee.pdf (p.78).

## Strict memory follow-up M6E3C1: guest-generated vector instructions

M6E3B passed CI run 46048372002/job 140673213324: Config changed
from 440 to 73443 at 653553 retirements, pc a0016218. After
a call to a00166f4 the next stop was 653560 / a0016708, word
00010c02 (SRL at,at,16), source ffffffffa0019160.
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46048372002/job/140673213324

Support canonical SLL/SRL/SRA word-immediate operations, low
32-bit input, sign-extended result, preserved upper GPR half,
architectural zero and reserved rs rejection. Explicit unsigned
arithmetic avoids signed-shift or shift-by-width assumptions.

The observed code constructs 3c1aa001 and 375a9160 (LUI/ORI
k0 with handler address a0019160), reads a destination offset
from the copied image at a001a1a4 and stores the first generated
instruction to canonical uncached low RAM. Require 653575
retirements at a0016744, verify the first word and all other
low-RAM sentinel bytes, and derive the destination independently
from the original ROM. Continue bounded execution to identify
the next dependency. This is not instruction cache coherence
or actual exception dispatch yet. Pending authentic CI.

## Strict memory follow-up M6E3C2: complete vectors and Wired initialization

M6E3C1 passed CI run 46048827089/job 140674754490, first vector
word at physical zero, 653575 retirements. The guest returned
from its vector installation routine at 653619 / a0016220 and
stopped at 653634 / a00197ac (40843000 MTC0 a0,Wired), a0=0.
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46048827089/job/140674754490

Require four non-overlapping 16-byte stubs at the offsets in the
pinned ROM table: 3c1aa001,375a9160,03400008,00000000.
Compare all other low RAM bytes with the original sentinel.
This validates installation, not exception entry or cache coherence.

Support canonical MFC0/MTC0 Wired using low-word semantics.
As established in Batch 8, defined Wired values 0–47 reset
Random to 47; refuse undefined values. Test all defined values,
reserved encodings, architectural zero and unchanged state on
refusal. Random reads and TLBWR remain unsupported, as no
cycle-based Random evolution is modeled. No production changes.

Require the actual Wired write, SYNC.P and JR/NOP return at
653638 retirements, pc a001635c, Wired=0, Random=47 and
SP ffffffffa007ff60. Continue bounded exploration from there.
These new acceptance criteria are pending CI; no retail replay.

## OpenPS2BIOS v0.7 parallel diagnostics

Keep the complete v0.6 pinned SHA-256 and all existing EE/IOP,
SIF and RPC acceptance checkpoints unchanged. Add a separate v0.7
job to the same workflow using the official versioned image and
upstream checksum companion, checking the exact 4 MiB size.

The version-independent EE probe mode (--explore) executes from
ROM0 reset with explicit isolated RAM and SIO backing, a two-million
instruction bound, a 64-entry trace tail, actual RAM handoff evidence,
COP0/GPR state, stop opcode/lookahead and captured guest serial bytes.
No v0.6 addresses, directory sizes, or checkpoints are transplanted.
The v0.7 IOP runner independently collects a ten-million instruction
diagnostic report. Expected diagnostic stop/budget exit codes are
not a claim of successful firmware boot. Failed downloads, integrity
checks, unexpected host exits and missing reports fail CI.

First v0.7 run establishes the actual baseline; the observed checksum
and traces must be reviewed before pinning v0.7-specific acceptance
criteria or extending its SIF/RPC integration. v0.6 remains the
strict end-to-end regression target in parallel. No production
runtime changes or retail replay required for this diagnostic batch.
https://github.com/TamaribaEmu/OpenPS2BIOS/releases/tag/v0.7

## M6E3D1: persistent indexed firmware TLB state

Both versioned jobs passed on d8e3a115:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46050170638
v0.6 stopped at 653650 / a0019730 and v0.7 at 653842 /
a0019750, same MTC0 Index opcode 40840000 and arguments:
Index=0, PageMask=0, EntryHi=70000000, EntryLo0=80000007,
EntryLo1=7. v0.7 official SHA-256 confirmed twice and now pinned.

Add caller-owned persistent c_ee_tlb to the isolated executor,
canonical MFC0/MTC0 for five indexed TLB staging registers,
TLBWI and TLBR using the existing table validation. Staging
registers retain the bounded RAM interpreter's raw storage
policy; invalid indices/masks and unwritten TLBR refuse.
The existing table's lookup still rejects unsupported entries.
No TLBWR, Random progression, translated memory execution,
or live cache/exception delivery is enabled by this change.

Require actual v0.6 first TLBWI at 653657 / a001974c,
check all entry fields and scratchpad classification, then
return at 653660 / a0016388. v0.7 independently must emit the
same first TLBWI entry at 653849 / a001976c. These predicted
checkpoints require CI. No production runtime or retail changes.

## M6E3D2: initial TLB table and unsigned immediate comparison

Both v0.6/v0.7 passed M6E3D1 on 3a101fd4:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46050590530
First scratchpad TLBWI matched at 653657 / 653849; the
13-entry initial loop reached SLTIU at 653932 / 654124,
PC a0016470 / a0016490, opcode 2c210001, at=0.
Verify all 13 v0.6 entries against original ROM table at
EELOAD physical 1a0d4, including absent entries 13–47.
Require v0.7 thirteenth guest TLBWI at 654101.

Implement SLTIU using a sign-extended 16-bit immediate and
unsigned low-64-bit comparison, preserve upper GPR half and
architectural zero. Directed tests cover negative immediates,
64-bit boundaries and delay-slot budget refusal. Require v0.6
mapping argument construction at 653943 / a00164a4, stack
physical 7ff40 contains 301f; then bounded exploration.
Predicted new checkpoints require CI. No retail replay.

## M6E3E1: actual VIF0/VIF1 FBRST device boundary

M6E3D2 passed on 7c0559bb; all 13 initial v0.6 TLB entries
matched ROM and the v0.7 count checkpoint passed:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46051012544
v0.6 exhausted its 4096-instruction exploration window at
658039; v0.7 advanced to 671772 / a00166a8, SW a0,0(at)
with a0=1, address b0003810, Wired=31. Subsequent observed
words target b0003c10 and VIF FIFOs.

Add explicit reset-only VIF write transport, canonical KSEG1,
DEVICE and UNCACHED word span, FBRST addresses 10003810 and
10003c10, value 1 only. Route to actual existing c_ee_vif1
instances (VIF0 bound to isolated VU0), with no DMA/IRQ hooks.
Do not accept arbitrary MMIO or fabricate readiness. Tests
seed ERR/ROW and verify reset behavior, independent devices,
refused addresses/widths/values/reads and branch-slot budget.
Extend v0.6 exploration to 32768 retirements and require both
real reset writes and Wired=31; v0.7 requires both resets.
LQ/SQ FIFO upload and later device dependencies remain blocked.
New checkpoints and compilation pending CI. No retail replay.

## M6E3E2: genuine firmware LQ/SQ and VIF FIFO uploads

Both releases passed M6E3E1 on 29d55c1c:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46051791312
VIF0/VIF1 each reset once, Wired=31. v0.6 stopped at
671586 / a00166a0, v0.7 at 671778 / a00166c0, opcode
78880000 (LQ t0,0(a0)). Packet source a001a200/a001a220.

Use existing GPR_QUADWORD access planner (align down before
translation) and full little-endian 128-bit RAM/ROM loads and
RAM stores. Architectural zero reads/stores and load suppression
apply to both halves. Full backing and canonical uncached
access required. Admit SQ only to first canonical VIF FIFO
ports via existing device parser, not generic device writes.
Record accepted packets separately per channel; refused
parser commands do not increment counts. Validate both
v0.6 payloads against the original ROM's 16 bytes and require
one actual accepted upload to each VIF in v0.7. Tests include
RAM alignment, zero, missing backing, device refusal, parser
rejection and real STCYCL state effects. Next anticipated
boundary is INTC register b000f010; no fake IRQ readiness.
No retail replay. New acceptance checks pending CI.

## M6E3E3: real INTC_MASK register initialization

M6E3E2 passed both releases on 63c5dee1:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46052277820
VIF0/VIF1 each accepted exactly one genuine packet with words
01000404,05000000,00000000,00000000. Both stopped at the
INTC_MASK read (b000f010): v0.6 671592 / a00166b8,
v0.7 671784 / a00166d8, opcode 8c610000.

Generalize the isolated reset transport to explicit MMIO read/write
callbacks and admit only the canonical uncached word INTC_MASK
address in addition to existing VIF operations. Reuse c_ee_intc's
real zero-initialized mask and write-one-to-toggle semantics.
No synthetic status, interrupt sources or scheduling; the isolated
controller is not bound to guest interrupt delivery. Test repeated
toggles, load to zero, preserved GPR upper half, refused addresses
and widths, missing transport, and delay-slot budget refusal.
Require first actual transition read=0, write=2, mask=2, status=0
at 671597 / a00166cc and 671789 / a00166ec respectively.
Compilation and predicted checkpoints pending CI. No retail replay.

## M6E3E4: Timer3 register programming and SH

M6E3E3 passed both releases on 03fd9678:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46052671338
Both reached a0014cc4 / ac430000, SW v1,0(v0), address
b0001810, value 0c00, at 672544 (v0.6) / 672736 (v0.7).
The fourth EE timer (index 3) is selected by 0x1800.
Reuse c_ee_host_timers without a clock or hardware bus for isolated
register programming only. Admit canonical uncached Timer3 word
COUNT/MODE/COMP writes; all guest timer reads remain refused until
a clock is available. No timer ticks, IRQs or ready flags invented.
Implement naturally aligned SH to backed RAM and verify two-byte
store behavior, invalid width/address/backing and register semantics.
Require original guest sequence MODE=0c00, COUNT=0, COMP=ffff,
MODE=83; validate post-write MODE/COUNT and exact fourth-write
retirement 672554 / 672746 at a0014cec. v0.6 checks physical
23824 halfword zero. No runtime clock or retail changes. Pending CI.

## M6E3E5: isolated DMAE and genuine SIF MSFLAG programming

M6E3E4 passed both releases on 020f1a2a:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46053200211
Timer3 sequence verified, INTC_MASK=1002; v0.6 exhausted
its 32768-instruction window at 686711, v0.7 reached D_CTRL
read at 690993 / a001419c, b000e000. Following instructions
OR DMAE and write 10000 to b000f220 (SIF MSFLAG, not D_STAT).
Reference: https://github.com/twist84/ps2-web-recomp/blob/SDBZ/runtime/src/dmac.cpp
D_CTRL reset is zero. The live native DMAC does not yet model
D_CTRL global enable; add isolated DMAE register storage only,
reject other bits, channel starts and completion. Reuse c_ee_sif
for MSFLAG's OR-on-write behavior. No IOP-ready state is injected.
Require exact first D_CTRL and MSFLAG guest writes and state at
690805/690806 (v0.6 predicted) and 690997/690998 (v0.7).
Extend v0.6 bound to 65536 instructions and reduce per-step
printing before the new boundary. Tests verify readback,
enable/disable, rejected unsupported mode/width/address and
MSFLAG accumulation. No production scheduler change. Pending CI.

## M6E3E6: isolated cold GS CSR reset

M6E3E5 passed on c7def708:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46053842140
Both versions reached GS CSR b2001000, SW value 200:
v0.6 690820 / a001624c, v0.7 691012 / a001626c.
Bit 9 is RESET (PS2SDK gs_privileged.h). Reuse c_ee_gs_state::reset
only on the disconnected cold GS, with no prior GIF/VRAM/GPU
operation. General CSR reset is not modeled: existing reset clears
VRAM whereas hardware soft reset behavior is more complex
(PCSX2 GSState.cpp). Do not change production GS behavior.
Accept the subsequent reserved upper-word zero write only after
reset. Refuse other CSR operations, repeated resets, status reads
and timing-dependent state. Require exact retirement and next PC
690821/a0016250, 690822/a0016254 and 691013/a0016270,
691014/a0016274 respectively. Directed rejection tests.
Next guest JAL is to a0010234 with ROM0 and 4 MiB arguments;
explore rather than fabricate its return. Pending CI.

## M6E3F1: ROM directory bounds arithmetic

M6E3E6 passed both versions on 1c06515d:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46054248071
GS reset and upper-word writes matched. Both reached a001021c,
opcode 38210001 (XORI), at 690906/691098 retirements.
Implement XORI (64-bit XOR, zero-extended immediate) and canonical
SUBU (32-bit wrapping subtraction, sign-extended result), preserve
GPR high halves and architectural zero. Test actual seven-word
bounds helper with valid and invalid offsets, end-of-ROM boundary,
unsigned underflow rejection, reserved encodings and branch slot
budget refusal. Require genuine helper return at a00102c4 with
v0=1 after 690912/691104 retirements. No ROM parser HLE or synthetic
return. New checkpoints require CI. No retail replay.

## M6E3F2: partial word ROM/RAM operations

M6E3F1 passed both releases on 56636bdb:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46054621684
Both reach a0010080 at 690975/691167, opcode 88820003
(LWL v0,3(a0)), followed by LWR v0,0(a0), SLL v0,v0,0,
JR ra and NOP. Source bfc0100c.
Reuse partial access/bus planners and extract the existing strict
RAM interpreter's constexpr merge into a common header without
changing its arithmetic. Admit LWL/LWR from immutable ROM and
canonical uncached RAM; SWL/SWR only to RAM, selected lanes only.
No partial MMIO, cached aliases or writes to ROM. Tests exercise
all four lanes independently in ROM/RAM, store lane preservation,
cross-word pairs and delay-slot refusal. Require original ROM
word at a00102f0 at 690980/691172, compared against actual image.
No host parser substitution or retail replay. Pending CI.

## M6E3G1: bounded cold-cache indexed maintenance

M6E3F2 passed both releases on 068a333b:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46055160703
ROM word 1000 matched, both reached CACHE 14 (DXWBIN)
at 694874/a00190dc and 695066/a00190fc. Then CACHE 07
(IXIN) loop. EE Core Instruction Set Manual v6.0 pp306,309:
DXWBIN indexes VA[11:6], IXIN VA[12:6], way VA[0].
Both operations are valid regardless of DCE/ICE.
Explicit cold domain only: all backing accesses bypass cache
and there is no cache fill or dirty data. Require opt-in state,
pure bounded index/way addresses, track unique guest invalidations
(128 D lines and 256 I lines). No claim of physical reset tag
contents, writeback for live cache, or translated-code coherency.
Other CACHE operations including IFL refuse. Test both ways and
all indices, repeats, absent state, invalid op/address, slot budget.
Require complete actual sweeps at predicted retirement
695317/696217 and 695509/696409. No runtime cache changes.
Pending CI; no retail replay.

## M6E3G2: explicit interrupt scheduler handoff

M6E3G1 passed both releases on 3578b066:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46055649575
All 128 data and 256 instruction cache lines were visited.
v0.7 emitted "[openbios:ee] starting EELOAD" through guest SIO.
Both reached MFC0 v1,Status at a0014a40 (696390/696582).
Implement canonical Status read and preserve the register high half.
The following OR/SYNC.L produce a real requested Status 70030c01.
Recognize canonical kernel-mode IE+EIE-enabling writes as a distinct
host refusal, INTERRUPT_SCHEDULER_REQUIRED, without retiring the
write, changing Status/Cause, or leaking a JAL link in a delay slot.
Require the exact boundary at a0014a4c, 696393/696585 retirements.
Keep existing disabled-interrupt Status support. No synthetic
pending sources or clock advancement. The explicit next workstream
is to replace this isolated probe domain with scheduler-owned CPU,
RAM/TLB and devices; account elapsed EE/IOP time and pending timer/
INTC/DMAC/SIF sources, implement precise Status commit and interrupt
entry, and verify against real firmware traces. Do not just relax
status_supported. Native LLVM execution and real cache coherence
remain separate required milestones. Pending CI; no retail replay.

## M6E3H1: bounded reset-owned scheduler pilot

M6E3G2 passed both firmware releases on 1a95082c:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46056154163
Both job logs confirm the exact interrupt refusal at a0014a4c.

Introduce c_ee_strict_firmware_pilot as a separate opt-in owner of
reset CPU, immutable ROM, zeroed live RAM, TLB, cold-cache state,
runtime scheduler, hardware bus and production host timers. The
production reference compatibility clock reserves one slot for an
ordinary instruction and two for supported branch+delay-slot
transactions; only committed instructions publish time. This is
not measured R5900 timing. Refused instructions and insufficient
budget do not advance time or service devices. Timers/INTC use
real register programming, not fabricated pending sources.
Service a deadline at the committed boundary in production timer
channel order; refuse a deadline strictly inside an atomic branch.
Also refuse branch-slot MMIO until issue-time ordering is modeled.
The pilot only connects bus-clock timers and INTC, rejects HBlank
and gating configuration, and is limited to 1048576 reference
slots (below the default unconfigured Timer3 HBlank deadline).
No IOP, DMA, GS scanout or guest exception entry is implied.
Status enablement remains refused; the isolated ROM0 checkpoints
and production retail path are unchanged. Directed synthetic ROM
tests cover reset ownership, RAM effects, retirement/budget,
branch refusal and rollback, real compare IRQ/Cause pending state,
and deadline ordering. Pending CI on both firmware jobs.

## M6E3H2: bounded Level 1 interrupt entry and ERET

M6E3H1 passed both v0.6/v0.7 jobs on 53b292df:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46057014681
The new directed scheduler pilot and unchanged isolated stop passed.

Admit canonical kernel MTC0 Status only in the owned scheduler path,
with a bounded CU0/1/2, IE/EIE/EDI, EXL, BEV and hardware IM subset.
Reject ERL, KSU, cache-related and reserved fields. Retain the
isolated reference's original Status restrictions. Synchronize the
existing compatibility COP0 clock at committed boundaries from reset;
Count/Compare transfers and scheduling their deadlines remain H3.
Use the common COP0 write, Level 1 exception entry and ERET helpers.
Service actual due timers before interrupt eligibility at the
retirement boundary; EPC is the next PC, BD clear after a completed
branch+slot. Refuse Status/ERET in slots and deadlines strictly inside
a branch. No physical Status pipeline hazard timing is claimed; the
reference compatibility policy uses the retirement boundary.
Distinguish retirement with/without accepted interrupt via vector.
The pilot preserves exception exit state on refusal and dispatches
BEV1 handlers through the same live ROM. BEV0 entry selects the real
cached vector but execution of that backing still refuses.
ERET from EXL returns to EPC; uncleared pending sources reenter
without executing the resumed instruction. Directed tests cover
same-boundary and prior pending timers, masks, BEV selection,
branch completion, real guest INTC acknowledgement and ERET.
No dual-processor coordination, full firmware boot or native lift
asserted. Pending CI. No production retail path changes.

## M6E3H3A: Count/Compare deadlines and simultaneous sources

M6E3H2 passed both firmware versions on 393f10db:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46057428203
Both directed interrupt regressions and isolated checkpoints passed.

Use existing core-global COP0 compatibility clock helpers for
canonical MTC0 Count/Compare and MFC0 Count/Compare/Cause/EPC.
Read at the committed instruction-start timestamp; write Count or
Compare at that same timestamp, then charge the retired instruction.
No calibrated pipeline timing. Count progresses modulo 2^32;
equality at start schedules the next wrap, Compare writes clear
only IP7, and Count writes do not clear the pending latch.
Add the next unlatched Compare deadline to the same atomic branch
preflight as EE timers. A deadline inside a branch refuses;
a deadline at completion is serviced before interrupt eligibility.
Preserve the independent IP2 and IP7 levels when they occur together.
Directed ROM tests verify simultaneous real timer and COP0 events,
guest acknowledgement of each independently, ERET, wrap, sign
extension, zero register, and deadline refusal/commit. All new
clock transfers refuse in delay slots until finer scheduling exists.
No Random, DMA, IOP or physical issue timing added. Pending CI.

## M6E3H3B: live DMAC scratchpad transfer pilot

M6E3H3A passed both firmware releases on ca7a4c09:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46057843002
The new Count/Compare tests and previous isolated stops passed.

Replace pilot-only RAM vector/TLB with owned production ee_memory
and its persistent TLB. CPU strict RAM access and the actual DMAC
share the same physical backing; the DMA scratchpad is owned by
the same memory object. No mirrored copies or fabricated payload.
Connect real c_ee_dmac normal channels 8/9, D_STAT and IP3.
D_CTRL remains an explicitly limited local DMAE gate because the
production synchronous DMAC does not implement global arbitration.
Refuse disabled starts, unsupported modes and invalid spans before
touching CHCR or time. Limit to 1..1024 QWC, no chains/interleave,
no other channel masks, no MMIO in branch slots.
Stage the validated CHCR start during the store, perform the real
synchronous copy at retirement before device timers/Count and IRQ
sampling, and check that STR cleared. This is a reference-style
transaction, NOT a modeled asynchronous bus transfer or DMA latency.
Directed guest ROM tests round-trip actual RAM bytes through
scratchpad wrap, observe MADR/QWC/SADR, independent D_STAT status
and masks, IP3, BEV1 entry, acknowledgement and ERET. Negative
tests verify disabled/misaligned/unsupported starts are read-only.
SIF/VIF DMA and dual processor coordination remain future work;
isolated firmware and retail execution remain unchanged. Pending CI.

## M6E3H3C1: live SIF mailbox and bounded VIF device integration

H3B passed both firmware versions at d48646f8:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46058238123

Connect existing SIF MSCOM/SMCOM/MSFLAG/SMFLAG MMIO, preserving
EE/IOP ownership, OR and W1C semantics. No IOP flags, readiness,
RPC server or packet completion are fabricated.
Own separate production VIF0 and VIF1 controllers; VIF0 references
the pilot-owned VU0 state. Bind existing hardware INTC callbacks.
Allow explicit register subset, FBRST reset/ack and direct FIFO SQ.
Admit normal forward DMA channels 0/1 only for fully prevalidated
single-word NOP/STCYCL/ITOP/STMOD/MARK commands, including MARK.I
(nonstalling IRQ), with no partial commands or VU/GIF launches.
Use production parsing and DMA completion, not hand-authored
register values. Reject unsupported command packets before DMA
start/retirement. Shared RAM is the real guest source.
Directed ROM tests cover mailbox ownership, both VIF channels,
guest MARK interrupt and D_STAT source, reset and refusal,
plus direct FIFO NOP. No SIF FIFO endpoint yet; genuine paired
SIF DMA is H3C2, with IOP ownership and progress explicit.
No production retail path changes. Pending CI.

## M6E3H3C2: paired SIF FIFO transport

H3C1 passed both firmware releases at 9aa0c3bd:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46058677582

The strict pilot can now own a second ROM0 reset domain (existing
IOP kernel raw firmware path), bind the genuine SIF mailbox link
and both existing 128-word SIF DMA engines to the same physical
EE RAM and DMAC. No HLE service or host guest-RAM prepopulation.
Admit EE DMA5 destination-chain receive with QWC zero and DMA6
bounded normal source, with real STR/FIFO/completion states;
an unpaired receiver is not a synchronous completed transfer.
Reject active register overwrite and DMAE disable with active DMA.
SIF producer/consumer faults are terminal pilot boundaries.
An explicit independent IOP instruction slice runs between EE
transactions. Sample its pending interrupt before the next EE
fetch; no EE time advances for the IOP slice or IRQ entry.
This is not synchronized clock mapping, concurrency, arbitration
or calibrated DMA latency. H3E must establish those ownership and
deadline contracts. Test ROMs build both tags and payloads in
guest instructions, verify pending FIFO without a receiver,
IOP-transformed response bytes, actual D_STAT/IP3 and zero-slot
interrupt entry. SIF1 source chains remain out of this subset.
No production retail path changes. Pending CI.

## M6E3H3D: GS scanout and VBlank edges

H3C2 passed both firmware versions at a38b002a:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46059348419

Explicit reset-time NTSC/PAL machine configuration installs the
production scanout schedule without creating an event or invoking
a guest syscall. Re-anchor reset Timer3 at cycle zero; keep guest
HBlank programming excluded. Extend the bounded reference slot
limit to 16 Mi slots, still below Timer3's reset deadline.
Include scanout in branch deadline preflight. At exact committed
deadlines call the existing cycle scheduler/scanout clock with
owned INTC, RAM and an unregistered vsync flag object. Real
start edges raise INTC 2 and real end edges raise INTC 3 (the
reference hw.cpp VBlank end event). Expose read-only CSR and
VSINT W1C, independent from INTC acknowledgement. No HLE flag
pointers or synthetic field delivery. No GS rendering, VU/GIF,
guest CRT setup, calibrated timing or IOP VBlank propagation
claimed; H3E will coordinate the second clock.
Directed ROM test retires actual instructions through NTSC
start/end boundaries, checks branch EPC, CSR.FIELD/VSINT,
independent acknowledgement, ERET and masked end latch; PAL
reset cadence is separately checked. Pending CI.

## M6E3H3E1: bounded synchronized EE/IOP execution

H3D passed both firmware versions at 3ca7e2cd:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46059745015

An opt-in synchronized reset domain reuses c_ee_iop_clock
(8 EE compatibility slots per IOP instruction), anchored at zero.
Accrue fractional debt only after actual EE retirement and consume
only actual IOP progress. No independent IOP slices in this mode.
Refuse an atomic EE branch crossing the next IOP slot; equality
at the completed branch boundary is accepted. Same-time order:
EE store/DMA, EE timers, scanout, IOP slot, COP0/EE IRQ sampling.
Actual scanout edges raise raw IOP I_STAT IRQ0/IRQ11, following
PCSX2 IopCounters.cpp; no HLE callback or synthetic event.
IOP stop preserves unconsumed debt and blocks future EE progress.
The committed EE transaction is not rolled back after an IOP stop.
Directed ROMs test fractional accounting, exact mailbox visibility,
branch refusal and PAL VBlank start/end in both clock domains.
This is reference timing and serialized ownership, not calibrated
timing or parallel execution. Atomic branch interior deadlines
remain a blocker for unrestricted firmware execution; next
checkpoint must address branch/slot scheduling and real-ROM
dual-domain diagnostics before claiming authentic boot progress.
No retail changes. Pending CI.

## M6E3H3E2: resumable branch/slot deadlines

H3E1 passed both firmware versions at 126876a9:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46060148640

In the synchronized reset domain only, a one-slot budget or
interior device deadline can retire a supported BEQ/BNE/J/JAL/JR
branch independently. Preserve the sampled target and committed
link across the explicit pending delay-slot boundary. Service the
normal scheduler after each committed slot; no invented cycles.
The next slot fetch is live, not a cached copy taken before IOP
progress. Refusal preserves the retired branch and pending slot;
do not roll back external device progress. Status/ERET and nested
branches in slots remain refused. The compatibility interrupt
policy samples asynchronous EE IRQs after the branch's slot,
while device latches and IOP retirement occur at their actual
scheduled timestamps. No calibrated interrupt/issue timing or
synchronous slot fault entry is claimed. Atomic paths without an
interior deadline and isolated probe behavior are retained.
Directed tests cover IOP deadline crossing, budget zero, JAL link,
interior Compare IRQ/slot completion, refusal/retry and sampled JR
target. Pending CI.

## M6E3H3E3: real-ROM dual-domain diagnostic

H3E2 passed both pinned firmware releases at e853f3b1:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46060890164

The separate ee_iop_rom0_diagnostic executable starts both processors
from the same unmodified, verified 4 MiB ROM0, no RAM seeding and
no HLE service. Select explicit PAL reset cadence and a 2 million
EE compatibility-slot diagnostic bound. Record actual first refusal,
EE/IOP clocks and debt, register state, SIF/DMAC/INTC/CSR state, and
last 64 EE retirement transactions. Check retirement accounting and
actual progress on both CPUs, not successful firmware boot. Keep the
existing isolated v0.6/v0.7 checkpoints unchanged; capture independent
logs in each ROM0 workflow artifact. No guessed firmware stop or
version-specific layout assumptions. Pending first real dual-domain
CI evidence; use it to prioritize subsequent blockers.

## M6E3H3E4: connect guest serial TX

H3E3 passed both firmware versions at 977d65aa:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46061304170

Actual synchronized stops (EE/IOP slots):
- v0.6: 653362 / 81670
- v0.7: 653554 / 81694

Both refused SB opcode a0820000 at a0010540, data address b000f180,
byte 5b. Zero IOP debt, no IOP fault. Reuse the existing bounded
s_ee_reset_sio_tx transport in ordinary and resumable delay-slot
execution. Expose immutable captured bytes; emit hex in diagnostic
logs, not raw workflow commands. No synthetic UART state or events.
Test byte writes, pending-slot retirement, zero budget and invalid
word store. Require both pinned diagnostic runs to advance past
their measured prior stop and emit the real initial byte. Pending CI.

## M6E3H3E5: enable configured reference HBlank timers

H3E4 passed both firmware versions at a7d07440:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46061705946

Both emitted their 36-byte EE ready line and advanced 19191 slots.
v0.6 stopped at 672553 EE / 84069 IOP, v0.7 at 672745 / 84093.
Both stopped at a0014ce8, opcode ac430000, writing MODE=83 to
Timer3 (b0001810). Zero IOP debt.

Admit HBlank timer MODE, COUNT and COMP MMIO only when an explicit
fixed CRT mode is configured. Reuse the existing production
integer line cadence and timer scheduler. No gating or reserved
bits, no dynamic CRT mode changes, no new scanline phase model.
Test NTSC and PAL divisors, exact first/second ticks, real compare
INTC12 and the unchanged refusal without scanout. This is the
existing reference approximation (counter-relative integer line
periods), not a hardware-calibrated HBlank edge producer. No
production timer changes. Raise the pinned real-ROM minimum progress
assertions past the observed Timer3 stop. Pending CI.

## M6E3H3E6: cold GS reset in synchronized ROM0

H3E5 passed both firmware releases at d88fa6a2:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46062073575

Both progressed 18267 EE slots, reached DMAE=1/MSFLAG=10000,
then refused CSR.RESET at b2001000 (opcode ac230000, value 200).
v0.6: 690820 EE / 86352 IOP; v0.7: 691012 / 86376.
Connect a real owned c_ee_gs_state to the pilot bus. Admit the
same one-shot cold GS reset and reserved-zero upper word as the
isolated probe, only before any field and with no active DMA.
Do not reset the configured scanout epoch, INTC, timers, SIF or
either processor's clock. No GIF input/GPU is connected; live GS
reset after scanout is outside this scope. Test exact reset counts,
preserved unrelated state and deadline, and duplicate refusal.
Require both pinned dual-domain diagnostics to advance beyond the
measured reset stop and execute the upper-word store. Pending CI.

## M6E3H3E7: JALR and the real EELOAD handoff boundary

H3E6 passed both firmware versions at c372925b:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46062563763

Both reached Status=70030c01, retired the previously isolated
interrupt-enabling MTC0, printed "starting EELOAD", and then
refused JALR opcode 0320f809 with t9=00082000.
v0.6: 696405 EE / 87050 IOP, PC a0019144.
v0.7: 696597 EE / 87074 IOP, PC a0019164.
Zero IOP debt, no fabricated interrupt or field.

Share the native strict JALR encoding predicate, retaining the
native lowering's prior semantics. Add atomic and scheduler-split
JALR retirement with arbitrary nonzero rd, zero rd suppression,
target sampling, zero-extended link and high-half preservation.
Refuse reserved encodings and undefined rs==rd. Atomic slot
refusals roll back the link; split slots retain the committed link.
No change to mapped/cached instruction fetch: diagnose the actual
target boundary before enabling a new fetch domain. Require real
dual-domain execution beyond the observed JALR stops. Pending CI.

## M6E3H3E8A: mapped fetch admission and actual cache-route evidence

H3E7 passed both ROM0 jobs at 9c71c209:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46062958498

v0.6: 696407 EE / 87050 IOP; v0.7: 696599 / 87074.
Both retired JALR and its slot, then refused the live fetch at 00082000.
Prior isolated evidence reports Config=00073443 (ICE and DCE enabled).
The current synchronized diagnostic did not report the actual TLB
translation/cache route at this PC, so do not assume a cache hit or miss.

Opt into low mapped-address RAM fetch only in the synchronized
owner, only if the existing planner proves physical cache bypass,
cache attribute 2 or cache attribute 3 with ICE disabled, aligned
valid backing. No identity assumption, no raw access on TLB fault,
no accelerated or active I-cache route. Preserve isolated default
and all data access restrictions. Tests exercise actual guest TLB
mapping, clean pages, ICE toggles, invalid/missing/backing refusals.
Add passive stop diagnostics for Config, EntryHi, TLB index/status,
cache route and physical backing instruction (not an executed word).
Do not require progress beyond 00082000 until actual fetch evidence
supports the next cache or fault model. Pending CI.

## M6E3H3E8B: cold instruction cache and guest-installed EELOAD

H3E8A passed both pinned versions at a0eaa399:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46063467975

Both reached 00082000, TLB entry 13, 256 KiB page, physical
00082000, cache 3, Config 00073443, no translation fault.
The entry backing word was zero. Upstream source confirms this
is intentional: ob_ee_install_eeload zeros the initial region,
writes JAL at +9c, main at +100, exec at +2b8, marker at +600,
path at +680, then flushes both caches.
https://github.com/TamaribaEmu/OpenPS2BIOS/blob/v0.6/src/ee/kernel.c
https://github.com/TamaribaEmu/OpenPS2BIOS/blob/v0.7/src/ee/kernel.c

Validate passive backing markers in both CI runs. No host installation.
Introduce bounded 16 KiB two-way, 64-byte line cold I-cache with
transactional fills after a complete guest IXIN sweep. Retain hits
despite uncached RAM writes. Refuse replacement, color aliases,
live cache maintenance, non-RAM and unproven mappings; no D-cache,
cache latency or speculative fills. Commit only on retired
instruction/atomic pair; refused slot/budget discards staged fills.
Assert real progress through the intentional NOP sled and JAL.
Pending CI; not a full R5900 cache model.

## M6E3H3E9A: EELOAD signed immediate comparison

H3E8B passed both pinned releases at 516091e9:
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46063890489
Both stopped at cached EELOAD main 00082100, opcode 28880002
(SLTI t0,a0,2), after 696448 / 696640 retirements; three
I-cache lines were resident and the refused fourth fill rolled back.
The actual firmware-installed backing and serial output remain required.

Implement SLTI in the bounded reset register executor using the existing
architecture signed comparison and immediate sign-extension helpers.
The result is 0/1 in the low 64-bit GPR half; upper halves and zero are
preserved. PCSX2 R5900OpcodeImpl.cpp confirms these semantics; the
ps2-web-recomp primary opcode table and existing production decoder/lift
already include SLTI. No production/native lowering is changed.
Directed tests cover signed 64-bit and immediate boundaries, aliases,
zero and slot budget refusal; a mapped cache regression verifies two
transactional fills across a branch/SLTI slot boundary.
The known adjacent EELOAD BNE/BEQ, NOP, ADDU, LUI, ORI and ADDIU
already have executor coverage. LW is supported only within the existing
bounded data contract; do not bypass cached data access or syscall entry.
Require real-ROM progress beyond 696448 / 696640, retaining the original
isolated regressions and all backing assertions. Pending firmware CI.
No retail replay required; do not claim syscall execution or full boot.

## M6E3H3E9B1: genuine Level 1 SYSCALL and cached KSEG0 vector

H3E9A passed both releases at a8816a8e, workflow 46064486569.
Both reached actual SYSCALL 000822c0 with five resident lines and
696458 / 696650 EE retirements, zero IOP debt and 66 serial bytes.
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46064486569

Use the existing ee_raise_exception EX_Sys path for all SPECIAL
SYSCALL code fields, preserving pending interrupt bits, EPC/BD on
nested entry, and the actual BEV-selected vector. A trapping word
is not a successful retirement. Under this pilot's existing
retirement-based compatibility clock, synchronous entry advances
zero slots (no measured exception latency is implied); commit
its successful instruction fetch, and leave the branch target
cancelled. Synchronized branch/slot execution splits before a
trapping slot using a discarded cache probe; non-synchronized
atomic slots remain refused. Test ordinary, nested, BEV0/1,
explicit and forced split entries and zero budget.

Allow cache-3 canonical KSEG0 RAM fetch through the existing cold
transactional I-cache, with identical backing/color/way checks.
No cache bypass, replacement or D-cache model is added. Both
upstream kernel.c versions install the vector via guest uncached
stores (LUI/ORI/JR/NOP), then branch to ob_ee_exception_entry.
The upstream exception.S starts with depth bookkeeping and SQ
register saves before MFHI/MFLO/SD and COP0 frame reads. Further
coverage follows actual stop evidence, not a host syscall shim.
Require actual guest syscall 6 entry with correct Cause/EPC/EXL
and subsequent guest retirement. Pending real-ROM CI; no retail replay.

## M6E3H3E9B2: guest exception context register saves

H3E9B1 passed both releases at 9f9cb90f, workflow 46064876239.
Actual syscall 6 entry matched both asserted COP0 states and the
cached KSEG0 vector, followed by 45 guest retirements and seven
resident I-cache lines. The first refused word is MFHI k1
0000d810 at a0019204 / a0019224, 696503 / 696695 retirements.
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46064876239

Implement MFHI/MFLO/MFHI1/MFLO1/MFSA with reserved-field checks,
low-half writes and zero suppression; SA uses the existing internal
bit-unit representation (guest value is bits 6:3). Reference:
ps2-web-recomp opcode tables, PCSX2 R5900OpcodeImpl.cpp and MMI.cpp,
and the existing native lowering. Add LD/SD using the existing
naturally aligned uncached physical span planner and little-endian
64-bit transfer; cached data remains refused. Existing scheduler
EPC/Status/Cause reads already suffice. Add constexpr checks,
register transfer and doubleword execution regressions and a guest
BEV1 handler COP0 read test. Require at least 13 more retirements
through the authentic context-save sequence, retaining the exact
syscall entry and backing assertions. Pending real-ROM CI.
No retail replay required; no host frame or fabricated syscall result.

H3E9B2 workflow 46065262316 at 4621dda0 compiled successfully,
but both jobs stopped at the existing scheduler-owned strict
firmware pilot regression before real-ROM execution. The old
unsupported sentinel ffffffff is SD ra,-1(ra), now recognized and
correctly refused as an unaligned/unavailable data access. Replace
the opcode sentinel with unsupported SPECIAL funct 1 and preserve
the original word in an explicit data-refusal regression. Both
variants retain identical no-retirement, PC, RAM and clock checks.
Use the same unsupported sentinel in the Count wrap fixture.
No executor changes, no reduced real-ROM thresholds. Pending CI.
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46065262316

H3E9B2 workflow 46065615191 at 513705b2 passed the previous
scheduler pilot check but stopped at the processor-selection
regression. Its delay-slot fc820000 SD was also a stale unsupported
opcode fixture. Preserve both an unsupported SPECIAL word and the
original SD as an explicit data refusal, with no branch/slot
retirement, unchanged PC and both halves of the branch operand.
Reviewed remaining unsupported-opcode checks in both strict pilot
and RAM/TLB test files for collisions with the new opcode families.
No executor or firmware threshold changes. Pending CI.
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46065615191

## M6E3H3E9B3: remaining nontrapping scalar integer operations

H3E9B2 passed both pinned ROMs at 161d8a7, workflow 46065875198.
Both retired 70 further instructions after MFHI, completed initial
context saves, entered the actual C exception handler and stopped
at XOR v0,s4,at 02811026 (a0016d54 / a0016d74), with 696573 /
696765 EE retirements, seven I-cache lines and zero IOP debt.
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46065875198

Complete the remaining pure nontrapping scalar_constant operation
families in the bounded reset executor: XOR, SLT, DADDU, DSUBU,
DADDIU, SLLV/SRLV/SRAV, DSLLV/DSRLV/DSRAV and DSLL/DSRL/DSRA
plus DSLL32/DSRL32/DSRA32. Use the existing scalar evaluator for
masked shifts and signed bit ordering. Opcode assignments match
ps2-web-recomp recomp/r5900.cpp. Preserve upper GPR halves, zero
suppression and reserved-field checks. Add compile-time execution
assertions and directed ordinary/branch-slot regressions, including
budget refusal and boundary shift counts. No trapping arithmetic,
new data routes, HLE service or production lowering changes.
Require both ROMs to advance beyond their observed XOR stop,
retaining all prior entry/backing/clock assertions. Pending CI.

H3E9B3 workflow 46066209290 at 794cd34b compiled and passed the
new scalar execution regression, but both jobs stopped at the
existing JALR rollback fixture. JAL has the same outdated XOR
refusal assumption. Use nonzero reserved shamt encodings for
negative cases, and explicitly execute the original canonical XOR
in both JAL/JALR slots to verify that the newly written link is
visible. Preserve the previous OR, store, invalid target and
rollback coverage. No runtime changes or reduced thresholds.
Reviewed other unsupported-opcode fixtures in the strict tests;
real-ROM progress remains pending CI.
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46066209290

## M6E3H3E9B4: ordinary signed conditional branches

H3E9B3 passed both pinned ROMs at 878cb91c, workflow 46066563349.
Both advanced 185 retirements beyond XOR, stopping on BLEZ s8
1bc00063 at a0018ae0 / a0018b00, 696758 / 696950 retirements,
seven resident I-cache lines and zero IOP debt.
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46066563349

Add ordinary BLEZ/BGTZ and REGIMM BLTZ/BGEZ. Shared constexpr
validation, 64-bit signed decisions and PC-relative targets serve
both atomic reset execution and the synchronized split-slot owner.
Reference ps2-web-recomp recomp/r5900.cpp opcode tables and PCSX2
Interpreter.cpp. Reject reserved rt, likely/annul and link forms.
Test zero, signed 64-bit extremes and values with differing low-word
sign, backwards displacement, refusal rollback, zero-register
semantics, slot overwriting the sampled operand, caller-budget
and IOP deadline splits, unchanged link register. Preserve prior
firmware acceptance assertions and require progress past BLEZ.
No production lowering or HLE changes, no retail replay. Pending CI.

## M6E3H3E9B5A: real privileged GS display writes

H3E9B4 passed both pinned ROMs at e596849e, workflow 46066942045.
Both advanced 101938 EE retirements, to 798696 / 798888, with zero
IOP debt and 104 real serial bytes. Reconstructed serial confirms
[openbios:ee] LoadExecPS2 rom0:OSDSYS. Actual stop is SD t0,0(a2)
fcc80000 at a0018318 / a0018338, data address b2000020 (SMODE2).
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46066942045

Route only the six existing privileged GS display latches through
scheduler-owned canonical uncached 32/64-bit writes to c_ee_hw_bus.
Retain refusal of atomic MMIO slots, allow synchronized split slots,
no opt-in for the isolated probe, no readback claims. Add constexpr
whitelist checks, guest test for exact halves, refused timing write,
unchanged scanout epoch and clocks, and actual display snapshot in
diagnostic output. CI now checks the complete serial LoadExecPS2
line and SMODE2=3 plus advancement beyond the observed SD.
The guest SetGsCrt routine also writes SRFSH/SYNCH1/SYNCH2/SYNCV and
SMODE1. These are not implemented by the existing bus; do not accept
those as inert successful writes. The host reset reference remains
PAL progressive, and SMODE2 latch changes alone do not establish
NTSC timing or reschedule scanout. Next checkpoint must model the
actual guest timing/PLL programming and safe epoch transition.
No HLE, no retail replay. Pending CI.

## M6E3H3E9B5B: bounded guest-driven CRT timing transition

H3E9B5A passed all five workflows at 8fa8de2b, ROM0 run
46067337236. Real SMODE2=3 on both releases; next SD fca70000
at a001836c / a001838c targets SRFSH b2000030, after 798707 /
798899 EE retirements. Both retain 104 serial bytes and zero IOP debt.
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46067337236

Recognize the actual NTSC/PAL register tuples in both pinned
OpenPS2BIOS src/ee/kernel.c set_gs_crt routines. Address assignments
and FIELD interlace behavior agree with PCSX2 Hw.h/GS.cpp/Counters.cpp.
Only canonical SD timing writes, matching tuple and ordered hold /
release SINT accepted; no syscall interception or PC matching.
Commit the existing reference scanout epoch on the actual release
write, before any delivered field and without pending DMA or timer
edge. Re-anchor HBlank counters using their old-divider count,
preserve flags and unaffected timers. No synthetic edges or IRQs.
Reject incomplete/mixed tuples, unsupported modes and live field
transitions. The bounded model uses a new compatibility line phase
at release; no analog PLL lock, oscillator or cycle-calibrated
settle interval is claimed. A hold crossing the old field deadline
refuses rather than inventing scanout. Subsequent CRT reprogramming
requires a broader live timing model. Existing HLE path unchanged.
Add constexpr tuple tests, guest register execution tests and exact
CRT diagnostic assertions for both ROMs. Pending CI; no retail replay.

## M6E3H3E9B6A: guest exception context restoration

H3E9B5B passed both pinned ROMs at 3473c071, workflow 46069373283.
Both advanced 19796 EE retirements, to 818503 / 818695 with
102312 / 102336 IOP retirements and no debt. Genuine NTSC mode
programming completed, and serial includes exec entry=00100000
argc=00000001. This identifies the selected guest entry, not
execution of OSDSYS. Actual stop is MTC0 k1,EPC 409b7000 at
 a00192bc / a00192dc in ob_ee_exception_return.
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46069373283

Admit canonical scheduler-owned MTC0 EPC via common ee_cop0_write.
No Cause/Status/clock mutation or target fetch at EPC write. Keep
existing control delay-slot restrictions. Add MTHI/MTLO/MTHI1/MTLO1
and MTSA, canonical rs-only encodings, architectural zero reads,
full scalar HI/LO values and shared bit-unit SA representation
(native_codegen.cpp; PCSX2 R5900OpcodeImpl.cpp). Add compile-time
and directed round-trip tests plus guest SYSCALL/EPC/ERET test with
synchronized IOP variant and reserved refusal. Require authentic
exec-entry serial and progress through the ten context restore
instructions beyond the EPC stop. The upcoming FPU restore and
actual OSDSYS execution are separate checkpoints. No fabricated
frame, HLE interception, retail changes or replay. Pending CI.

H3E9B6A ROM0 workflow 46069794804 at f530f3d8 failed compilation
in ee_ram_tlb_test.cpp:4420 because the new EPC test was inserted
before rather than inside the initial if expression. Correct the
registration and preserve all test predicates and CI thresholds.
No runtime changes. Real-ROM execution remains pending CI.
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46069794804

## M6E3H3E9B6B: raw COP1 context restoration

H3E9B6A passed both pinned ROMs at 5204c1eb, workflow 46070057699.
Both advanced 21 EE retirements, to 818524 / 818716; EPC=00100000.
Actual stop is CTC1 k1,FCR31 44dbf800 at a0019310 / a0019330.
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46070057699

Admit canonical MFC1/MTC1, CFC1/CTC1 and LWC1/SWC1 with CU1 set.
Use existing module_builder.cpp raw FPR storage and console-verified
FCR read aliases, FCR0 read-only behavior, FCR31 mask 0083c078 and
fixed bits 01000001. All 32 FPRs (including FPR0) are writable.
Preserve upper GPR halves, zero suppression, ACC/overflow state;
no host floating-point conversion. Memory transfers use the existing
strict word plan and canonical uncached backing; never route to MMIO
or serial callbacks. Disabled CU1, reserved selectors, bad alignment
and unbacked/cached/device data refuse with no retirement; this does
not yet implement coprocessor-unusable exception entry. Add constexpr
and full register round-trip tests and real synthetic guest exception
save/restore/ERET regression with synchronized IOP. Require 33 further
retirements (CTC1 + 32 LWC1) in both actual ROMs. No arithmetic or
production LLVM changes, no retail replay. Pending CI.

## M6E3H3E9B7A: cold D-cache write allocation

H3E9B6B passed both pinned ROMs at 9a02799f, workflow 46070407877.
Both genuinely returned from the exception handler and executed
OSDSYS, stopping at PC 00100064, SW ra,01a4(sp) afbf01a4, EA
00106cbc; 818594 / 818786 EE retirements, 102324 / 102348 IOP,
zero debt, nine I-cache lines. Fetch maps through TLB entry 14,
cache 3, 256 KiB page. Status 70030c01, EPC 00100000.
https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/46070407877

Add a bounded physically tagged 8 KiB, 64-byte, two-way D-cache
(existing data bus planner and cold index sweep). Opt in only after
the guest cold data sweep and synchronized ownership. Only translated
cache-3 writeback-allocate RAM spans; no direct RAM writes on cached
stores. Stage one full data line per instruction or branch-slot,
commit fills/dirty data only on retirement. Refuse a third same-set
line; no eviction, writeback, replacement policy, locking, live CACHE,
Config changes with resident data, or calibrated cache latency.
Uncached accesses observe physical backing, not coherent dirty lines.
Apply to ordinary and partial scalar, quadword and raw COP1 data.
Add nonidentity TLB, hit/dirty, uncached alias, refusal, conflict,
and branch-slot tests. Require dirty D-cache, actual OSDSYS entry
retirement, and advancement past the stack SW in CI. No HLE or
production LLVM changes, no retail replay. Pending CI.
