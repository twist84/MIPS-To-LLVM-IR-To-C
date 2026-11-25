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
