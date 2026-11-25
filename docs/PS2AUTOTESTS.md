# Original ps2autotests ELF validation

This milestone runs the unmodified upstream ELF binaries through the
existing EE LLVM translation / native HLE runtime. Reference revision:
[97469ffbed8631277b94e28d01dabd702aa97ef3](https://github.com/unknownbrackets/ps2autotests/tree/97469ffbed8631277b94e28d01dabd702aa97ef3).
The manifest pins the Git blob IDs of all 55 ELF/expected pairs and the
license. The script verifies every download and cache hit. No commercial
game, BIOS, ISO or SDK build is required; the open test binaries remain
under the ignored build directory.

Configure and build the existing tools with Python/llvmlite installed:

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build --target ee_recomp ee_game_runner
    python tools/run_ps2autotests.py --list
    python tools/run_ps2autotests.py cpu/ee/alu cpu/ee/muldiv cpu/ee_fpu/fcr
    python tools/run_ps2autotests.py "cpu/ee/*"
    python tools/run_ps2autotests.py
    python tools/run_ps2autotests.py --jobs 8 --emit-jobs 1
    python tools/run_ps2autotests.py "vu/*" --jobs 4
    python tools/run_ps2autotests.py --jobs 1

The default is four concurrent guest tests with two LLVM emit workers per
test. Both options accept 1–32; use --jobs 1 for sequential comparison or
reduce emit workers to limit CPU/memory contention. The harness builds the
native tools once and verifies selected references before launching
workers. Reusable per-test caches are retained. Separate harness
invocations cannot share an active build, output or reference-cache root:
OS file locks reject conflicts and release on process exit. The lock files
remain on disk. Completion messages are live; summaries remain in manifest
order and report suite wall time, parallel execution wall time and summed
test duration. CPU contention can cause wall-time timeouts; rerun with
fewer workers to diagnose them.

Each test uses an isolated translation cache and a fresh results folder;
the normal native live expansion and bounded guest execution are used.
The script extracts only actual EE KPUTS guest bytes, reverses the
runtime's byte escapes, joins fragmented messages and compares the full
marker-delimited transcript with the console reference. It normalizes
CRLF and trailing empty lines only. A PASS means exact test output,
not necessarily a clean guest process exit; the runner exit, stop
reason and report are kept separately, including failures after the
test end marker. Missing/partial output, differences and infrastructure
errors are separate statuses, not passing tests. No known-difference
exemptions. The denominator is always 55; selected runs also report
their selected count.

Results: build/autotests/runs/<UTC timestamp>/{summary.json,summary.md}
and per-test run.log, guest.raw, test.out, optional test.diff, runner.json
and result.json. Default exit is nonzero unless every selected output
matches. --report-only explicitly makes the initial pilot observational,
rather than turning unimplemented services into an all-PR gate. The CI
pilot uses separate EE and VU matrix runners with two tests and one emit
worker per test; it includes the original six ELFs plus funnel, shuffle, cpcond, performance and clip.
Manually dispatch Build and test with autotests_full enabled to run all
55 in six nonoverlapping groups. Each shard uploads ps2autotests-<group>
with transcripts and a summary; these are observational artifacts,
not an aggregated pass gate. Guest timeout excludes compilation; CI job
time bounds the total.

The first pilot may stop in PS2SDK startup, TLB setup, scheduling or
console transport. Do not bypass the original startup, patch expected
output, or count an unexecuted test as passed. Implement missing services
from independent references, rerun the smallest failing test, expand
the selection, then establish the full-suite baseline. No score is
claimed before actual execution. Peer 54/55 is a reported output
comparison, not a matched timing benchmark.

## Pilot 1 findings (52e3de3, 2026-11-25)

All four workflows and all seven CTest shards passed. The observational
pilot produced 0/3 exact transcripts. ALU and FCR exhausted 1M dispatches
in SifGetReg(4), at EPCs 0x001135f4 and 0x00103d54 respectively. The
runner had no IOP backend: the initial harness invocation omitted
--disc-folder. MUL/DIV instead stopped after 79 dispatches at relocated
SDK kernel syscall code (0x80075024, instruction 0x54a0fff8, BNEL).
Evidence: [pilot artifact](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45612693778/artifacts/15020192710).

Use a minimal per-run extracted disc (unmodified TEST.ELF and SYSTEM.CNF)
to activate the existing HLE IOP bootstrap with installed SIF services.
No invented flag, modified executable or bypassed startup. Bound repeated
zero SMFLAG polls at 1024; optional --trace enables detailed diagnostics.
The original pilot's 0/3 is not a measured arithmetic discrepancy.

## Relocated kernel callback branch-likely support

MUL/DIV's original SDK installs a RAM syscall handler at 0x80075000
through SetSyscall; the actual callback stopped on BNEL at 0x80075024.
Implement BEQL/BNEL, BLEZL/BGTZL and non-link REGIMM BLTZL/BGEZL in the
bounded RAM callback interpreter. Taken branches execute the delay
instruction; false branches annul it without consuming the actual
instruction budget, and emit a separate reference-slot observation
consistent with the native LLVM path. Tests cover all six predicates,
both directions and suppression of an unsupported annulled opcode.
No guest ELF changes or unknown syscall bypasses. Runtime provenance
may require a native relink; no IR or VU1 lowering changed. Pilot 2
will execute the original three ELFs with the corrected IOP mount and
RAM callback path; the score is not yet known.

## Pilot 2 findings (7afadba, 2026-11-25)

[Artifact](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45613832333/artifacts/15021567782).
All three ELFs progress through the original startup to the same actual
SifSetDma rejection, after 94 (ALU), 155 (MUL/DIV) and 94 (FCR)
dispatches. All remain NO_OUTPUT. MUL/DIV has passed its earlier RAM
BNEL stop. The rejected packets have CID 0x80000002, opt=0, size=20,
destination 0x0001e000 and terminal attribute 0x44. Their buffer words
are respectively 0x20118f40, 0x20122cc0 and 0x201096c0.
The command decoder only accepted physical RDRAM; PS2SDK's
[ee/kernel/src/sifcmd.c](https://github.com/ps2dev/ps2sdk/blob/master/ee/kernel/src/sifcmd.c)
explicitly uses UNCACHED_SEG(pktbuf) for INIT_CMD and CHANGE_SADDR.
Normalize the 0x20000000 EE uncached RAM mirror at command validation,
retain the original bytes written to IOP RAM and use the physical
address for SIF0 reply delivery. Preserve zero, alignment and range
rejections. Regression covers both CIDs, transactional validation,
raw alias preservation and original physical command semantics.
This is not an arithmetic fix or new pass; pilot 3 is required.

## Full Windows baseline (7afadba, 2026-11-25 12:28 UTC)

The user's archive `20261125T122818342585Z.zip` contains all 55 original
ELF runs at reference revision 97469ff. 0 PASS, 0 DIFF, 0 INCOMPLETE,
55 NO_OUTPUT; summed per-test wall 535.904 seconds. 21 stopped at the
INIT_CMD receiver pointer in the 0x20xxxxxx uncached EE RAM alias;
34 stopped on 0x54a0fff8 (BNEL) at relocated RAM PC 0x80075024 after
nine RAM callback instructions. The recorded source HEAD 7afadba
already implements BNEL. An old locally built runner is a plausible
explanation, not proven by the old archive (no binary hashes recorded).
The later Linux pilot at 1620cf5 reached FILEIO BIND for all three
selected tests. Rebuild both native tools before each suite invocation
and record their SHA-256 in summary.json, so a source-only pull cannot
silently reuse an older native executable. No retail cache deletion.

## FILEIO console RPC (pilot 4)

Pilot 3 at 1620cf5 reached FILEIO BIND (server 0x80000001) for all
three selected original ELFs. Artifact: https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45614539023/artifacts/15020604277.
Add a scoped built-in FILEIO BIND and method 3 console-write backend
when a sector source is attached and no guest implementation owns the
server. The reference is PS2SDK's synchronous _fio_write_arg at
https://github.com/ps2dev/ps2sdk/blob/d4b9943bd4a58dbe69800bf4ec9d81640df5bc5b/common/include/fileio-common.h
and the server's fileio_rpc_write in iop/fs/fileio/src/fileio.c.
Validate stdout/stderr, signed-size range, physical RAM aliases and
inline unaligned prefix; stage exact bytes during DMA preparation,
emit only after commit, return actual byte count, queue normal SIF0
reply. The shorter three-word legacy form is also accepted. The harness
collects losslessly escaped FILEIO records alongside KPUTS. No generic
file open/read/seek success, arbitrary RPC success or bypass of startup.
The original ELFs have not yet demonstrated a complete transcript.

## Full executable baseline (e59a4f2, 2026-11-25 12:47 UTC)

Archive `20261125T124751140497Z.zip`, original pinned 55 ELFs, rebuilt
Windows tools with SHA-256 recorded, O0, 1M dispatches/test. 19 PASS,
10 DIFF, 26 INCOMPLETE, no NO_OUTPUT or ERROR, summed per-test time
509.343 s. Exact transcript comparison, not clean process-exit count:
all 19 passing tests still stop with runner exit 3 after TEST END.

| Group | PASS | DIFF | INCOMPLETE |
|---|---:|---:|---:|
| EE integer | 4 | 1 | 1 |
| COP0 | 0 | 0 | 1 |
| COP1 | 7 | 1 | 1 |
| MMI/SIMD | 4 | 0 | 2 |
| VU0 macro | 1 | 0 | 1 |
| DMA/VIF | 2 | 7 | 2 |
| GS | 0 | 0 | 2 |
| Kernel | 1 | 0 | 9 |
| Memory | 0 | 1 | 0 |
| VU micro | 0 | 0 | 7 |

Exact matches: cpu/ee/{alu,lsu,lsudelay,muldiv};
cpu/ee_fpu/{arithmetic,branch,compare,convert,fcr,sqrt,transfer};
cpu/ee_simd/{arithmetic,compare,logic,muldiv}; cpu/vu0_macro/integer;
dma/dmac/tagintr; dma/spr/interleave; kernel/ee/interrupt/dmac_order.

Prioritize independent blockers by evidence:

- cpu/ee/branch: Windows PermissionError reading live expansion request
  3 after 323 transcript lines, not an ISA result. Bound retry to 40
  reads with 25 ms delay on PermissionError only; persistent failures
  still fail closed. No change to guest state or sequence validation.
- cpu/ee_fpu/muldiv and dma/vif/stcycl: hit 1M dispatch budget; latter
  also prints genuine DMA timeouts. Retest individually at 5M before
  interpreting incomplete output as an instruction mismatch.
- Unsupported instructions: MTSA (0x01c00029), MMI shuffle
  (0x70108ea9), COP0 performance transfer (0x4084c801), BC0T
  (0x41010003), VCALLMS (0x4a000038; six tests), CTC2
  (0x48c0f800; EFU and random). Inspect decoder/lowering and physical
  reference before implementation; no guessed execution.
- EE/COP1 branchdelay: completed transcripts disagree on branches
  in delay slots; require explicit R5900 behavior.
- DMA/VIF: seven completed DIFFs, including scratchpad tag payload,
  MADR high bit, VIF0/1 register/data and timeout discrepancies.
- GS/INTC: blocking WaitSema with no runnable peer; investigate actual
  event scheduling. Thread tests need syscall/thread lifecycle audit.
- memory/ee/default: large allocations fail and reported address region
  differs. Audit standalone heap bounds and alias semantics.

Next: host handshake retry regression, then ISA and device batches.
No exceptions added to exact-output grading. The 19/55 result is for
this archive only, not yet a cross-platform full-suite result.

### Batch A: direct CMSAR1 dispatch

CTC2 register 31 routes to the resident VU1 interpreter using the low
11-bit microinstruction address and current TOP/ITOP, after retiring
outstanding deferred VIF1 DMA through the DMAC. No MSCAL is synthesized
or VIF double-buffer state changed. Unsupported programs reject
transactionally. This uses the existing synchronous VU1 model; it does
not establish asynchronous VU1 busy-bit timing, cycle accuracy, VU0
VCALLMS support or new exact autotest matches.

VCALLMS and VCALLMSR are decoded and routed through the existing LLVM
macro helper into the partial VU0 micro-engine. The runner starts a real
pair and services continued execution at COP2 interlocks and dispatch
boundaries. Instruction-attempt scheduling remains a reference-compatible
budget, not calibrated EE timing; unsupported upper operations, flags and
other microinstructions continue to reject. Existing EE ABI offsets and
VU1 AOT code are unchanged. Only actual transcript runs establish passes.

### Batch A: VU0 flag readback and lower NOP

The pinned original VU test runner spills CLIP, MAC and STATUS with
FCGET, FMOR and FSOR before its E-bit exit. Implement these and the
other read-only flag predicates in the partial VU0 engine, using the
existing four-cycle FMAC reservation for flag reads. The canonical
lower MOVE-to-VF0 NOP has no FMAC reservation. General MOVE, flag
writes and non-NOP upper execution are not enabled by this change.

## Full Windows baseline (b698e1c, 2026-11-25)

Archive `20261125T130858470210Z.zip` records 20 PASS, 11 DIFF,
24 INCOMPLETE, zero NO_OUTPUT/ERROR; cumulative per-test wall time
708.531 seconds. This is before the parallel harness. The funnel test
stops at MTSA (0x00100978, 0x01c00029), shuffle at PEXCH (0x00102188).
The pinned upstream funnel transcript exposes a four-bit byte count:
MTSA(16) reads zero, MTSAB(15) reads 15, MTSAH(7) reads 14.
Implement MFSA/MTSA while retaining the existing internal bit-count SA
field (no ABI or QFSRV representation change), and MMI3 PEXCH/PEXCW
with exact unary lane permutations. Rerun original funnel/shuffle; no
new passing transcript is claimed by this implementation.

## Confirmed original EE pilot (53392cf, 2026-11-25)

[CI](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45624237736):
all four workflows and seven CTest shards passed. Original funnel and
shuffle now match exactly; EE pilot 5/5, VU pilot 0/3. Combined evidence
with the b698e1c full baseline is 22 distinct matches, not a fresh full
22/55 measurement. EE pilot execution wall 18.017 seconds vs 32.414
summed test seconds (2 workers, 1 emit worker each).

## DMAC COP0 condition branches

The original cpcond test stopped on BC0T (0x41010003). Model the D_PCR
register and its low ten channel condition selectors. The R5900 COP0
predicate is (D_STAT.CIS & D_PCR.CPC) == D_PCR.CPC, independent of CIM
and interrupt enable; no selected channels means true. This agrees with
PCSX2 COP0.cpp and the pinned console cpcond transcript. Lower BC0F/T
and BC0FL/TL through the common pre-delay predicate and annulment CFG.
The helper returns unavailable if either live MMIO read fails. DMA
completion is sourced from the existing device, never manufactured by
the branch. D_PCR arbitration/enabling behavior is not added here.
Test the original cpcond ELF in the observational pilot; no new PASS
claimed before that run.

## Confirmed DMAC pilot (b021e1b, 2026-11-25)

[CI](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45625569495):
all four workflows passed. Original cpcond, funnel and shuffle match
exactly. EE/DMA pilot 6/6, VU pilot 0/3. Combined evidence is 23 distinct
passes across revisions, not a fresh full-suite score.

## COP0 performance register bank

The pinned performance transcript establishes PCCR mask 0x800ffbfe,
even/odd PCR index aliases, all-index MFPS reads and nonzero MTPS writes
as no-ops. The bank is core-global runtime state, appended without moving
existing CPU/IR fields. LLVM uses a checked helper for MFPC/MFPS/MTPC/MTPS.
A validated timing producer can deliver processor-cycle intervals via
 ee_cop0_perf_advance, with the status of the interval. Non-cycle events
and performance overflow exceptions are not implemented. The stock
reference-compatibility scheduler is not a validated R5900 cycle source;
active processor-cycle events fail closed rather than interpreting guest
attempts as cycles. The original performance test is expected to remain
incomplete until that timing producer is wired. No transcript exemption.

## VU0 upper CLIP and delayed readback

The original upper/clip test pairs consecutive CLIP operations with FCGET
and checks a four-cycle visibility delay. Share raw signed-magnitude
CLIP judgement with macro mode, retaining exponent-255 bit patterns
and treating a denormal W threshold as the largest denormal. Micro CLIP
reads VF xyz and ft.w with lane-specific FMAC dependencies; its result
shifts the latest issued history, but FCGET observes only matured flags.
Keep four pending results and retire them in issue order, including E
flush. Sample VF operands before lower writes. Other upper operations
and I/D/T flags remain unsupported. Regression includes original Multi
sequence; original clip ELF added to observational CI. No new pass claimed.

Performance pilot at fbd2464 remained INCOMPLETE, as expected without a
validated timing producer. All four workflows passed; six EE/DMA passes
remain confirmed.

## VU0 upper vector arithmetic

At ec863f8 all four CI workflows passed; upper/clip remains incomplete
in the original ELF pilot. Add vector, broadcast and I-register forms
of upper ADD/SUB/MUL using the existing bit-level VU arithmetic helpers.
Stage results before the lower half runs, then publish vector lanes and
retire MAC/status after four VU cycles. Upper and lower VF dependencies
are tracked independently. E drains pending upper results. Same-register
upper/lower VF write conflicts fail closed pending arbitration audit;
Q forms, accumulator forms, I-bit immediate pairs and other upper ops
remain unsupported. This is a microengine regression slice, not a new
original transcript pass.

## VU0 upper accumulator arithmetic

All four workflows at b77667b passed; original EE/DMA pilot remains 6/7
and VU pilot 0/4. Add vector, broadcast and I-register ADDA/SUBA/MULA,
MADD/MSUB and MADDA/MSUBA, preserving masked ACC lanes and per-lane ACC
overflow latches. Consecutive ACC operations forward without a VF stall
as in the pinned interpreter's FMAC dependency model. Product sticky
flags travel with the delayed MAC result instead of becoming visible
at issue. The upper issue structure reuses its padding word, preserving
all offsets. Q and outer-product forms remain separate work. No new
original ELF pass claimed.

## VU0 I-bit microinstruction pairs

When I is set, treat all 32 lower bits as a literal, irrespective of
what lower opcode they resemble. Execute the upper using the previous
I value, then commit the literal to VI21. No lower dependency, branch,
memory, or integer ALU side effects. Tests cover consecutive ADDi/I
pairs, arbitrary literal bits, and I+E termination. D/T remain unsupported.

## VU0 non-flag-writing upper instructions

All four workflows at 8fd574c passed; original pilot remains six exact
matches. Upper ABS, ITOF0/4/12/15, FTOI0/4/12/15, MAX and MINI (vector,
broadcast, I) share macro arithmetic primitives. Unary forms write Ft,
not the secondary opcode field Fd. Their VF reservations still mature
after four cycles but do not publish a MAC/status update. Test chains
cover masked destinations, exact arithmetic and delayed dependencies.
No new original transcript pass claimed.

## VU0 outer product

OPMULA and OPMSUB use rotated XYZ operands, ACC forwarding, and the
existing bit-level MUL/MSUB primitives. Preserve W data and W MAC bits,
including the newest pending flag result. XYZ reservations are independent
of the raw destination mask. Regression covers the cross-product sequence
and E flush. Original transcript progress remains unverified.

## VU0 lower register transfers

All four workflows at 9462c8e passed, with original pilot still six
exact matches. Implement lower MOVE/MR32/MTIR/MFIR, using old VF values
for rotated aliases and paired operations, lane-specific dependencies,
VI backup on MTIR, low-halfword preservation and MFIR sign extension.
MOVE to VF0 retains NOP timing; other transfers reserve four FMAC
cycles. Upper/lower same-destination writes remain fail-closed.
No new original transcript match claimed.

### Original VU pilot stop evidence at 9462c8e

Inspected the actual CI artifact 15031341955 (Build and test run
45631621143), including unchanged ELF words and marker-delimited output:

| Test | Stop | PC | Guest output |
|---|---|---|---|
| cpu/vu0_macro/transfer | UNSUPPORTED | 0x001004fc (0x48c8e000 CTC2 FBRST) | 28 lines; TPC test completed at 0x1f |
| vu/lower/integer | DISPATCH | 0x00110b88 (0x40024800 MFC0 Count) | 2 lines |
| vu/upper/clip | DISPATCH | 0x001110b8 (0x40024800 MFC0 Count) | 2 lines |
| vu/lower/efu | HOST_YIELD | 0x001128e0 (0x00063040 SLL) | 57 lines; EEXP CVF_MIN float formatting |

The integer and clip guests exhaust the one-million dispatch budget
at the Count reader used by their polling delay. This does not prove
unsupported VU instructions are the remaining blocker. Investigate
Count progress and actual timing source; do not replace hardware timing
with instruction attempts to manufacture transcript passes. FBRST needs
reset/break semantics, and EFU requires a guest float-formatting/loop
investigation. Publish stop PC, reason, dispatch count and output line
count in CI for non-PASS results without changing transcript judgement.
