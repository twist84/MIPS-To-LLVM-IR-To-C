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
matches. --report-only makes the CI suite observational rather than
turning unimplemented services into an all-PR gate. Each matrix runner
uses two guest workers and one emit worker per test.
The standalone **Original ps2autotests** workflow builds only
`ee_recomp` and `ee_game_runner` (and their CMake dependencies), once
per workflow. It distributes the two executables and a revision/SHA-256
manifest, preserving executable permissions with tar. Each shard installs
llvmlite independently and verifies the tools using `--prebuilt-manifest`;
it does not rebuild them or depend on the CTest build. Local harness runs
continue to rebuild by default.
Every push to main, PR to main and manual dispatch runs all 55 original
ELFs in six nonoverlapping groups. Each shard uploads ps2autotests-<group>
with transcripts and a summary. A final job verifies exact manifest
coverage, matching revision/tool hashes and reports a combined score,
per-test status, stop PC and reason. The combined summary is published
in the GitHub Actions job summary and ps2autotests-summary artifact.
Missing or duplicate tests and inconsistent provenance fail aggregation;
nonmatching transcripts remain observational. Guest timeout excludes
compilation; CI job time bounds the total.

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

## COP2 FBRST control

VU manual v6.0 sections 1.3 and 5.1.3 distinguish Ready, Run and Stop,
independent FB/RS strobes and DE/TE settings. Apply reset after break,
clear the selected unit's debug enables and status on reset, and set its
force-break stop bit otherwise. VU0 cancels pending modeled execution
reservations; committed VF/VI data and memories survive. VU1 debug stop
prevents VIF launches and CMSAR1 or reset releases it. A pending VU1
worker transaction is explicitly rejected rather than pretending to
interrupt its unpublished work. Exact mid-pipeline break effects and
indeterminate macro execution in Stop remain unvalidated. No original
pass claimed before CI.

## COP0 Count progression

Count and Compare are core-global with IP7, not restored per thread.
Synchronize before native MFC0/MTC0 Count/Compare and at runner device
safepoints. Count wraps modulo 2^32, Compare crossing latches IP7,
Compare write clears it, and Count write reanchors the elapsed interval.
Validated sources use the published cycle; the existing explicitly
approximate reference-compatible source uses reached instruction budget
plus idle units, including access inside a translated block. Unavailable
source retains static register semantics. This does not calibrate R5900
cycles, authorize performance counters, or change original test scoring.
Legacy source emission and RAM fallback timing still require audit.

## Standalone pilot and VU0 micro scalar Q

Standalone workflow 45634230156 at 42c9202 completed successfully:
EE 6/7, VU 2/4. Original vu/lower/integer and vu/upper/clip now match
exactly after Count progression. Combined distinct matches 25/55;
not a fresh full suite. Transfer advances to 0x00100544, EFU remains
HOST_YIELD at 0x001128e0, performance remains unsupported at 0x00100728.

Micro DIV/SQRT/RSQRT/WAITQ use the shared signed-digit scalar primitive,
7/7/13 cycle FDIV reservations, lane-specific VF interlocks, delayed Q
and I/D/IS/DS publication, serialization and E drain. Macro timing
remains separate. The pending flags occupy former tail padding; ABI
size and existing offsets remain unchanged. No new pass claimed for
this implementation before CI.

### Upper Q arithmetic

Added ADDq/SUBq/MULq/MADDq/MSUBq and their ACC-writing forms.
Q is sampled after pair interlocks and matured FDIV retirement. A normal
Q consumer does not stall on an in-flight result; a paired WAITQ does.
No fictitious Ft dependency for scalar Q forms. Tests cover all ten
encodings, previous-Q reads, paired WAITQ, ACC forwarding and masks.
At c07f476 the standalone original pilot remained 8/11 exact; the
four non-CTest workflows passed, CTest was still running when checked.

### VU0 lower random register operations

RINIT/RXOR sample the selected VF component after its FMAC dependency;
RGET/RNEXT write enabled lanes, with R maintained as a 23-bit mantissa
and implicit exponent. Shared LFSR step uses taps 4 and 22. RNEXT to VF0
leaves R unchanged in the PCSX2 reference micro path. Regression uses
raw words from the pinned upstream random sequence, with masked writes,
zero destination, dependency and E drain checks. The original random ELF
executes VU1, so this VU0 change alone does not claim its transcript.

### VU0 register-addressed integer memory

ILWR and ISWR use the unmodified VI base in quadword units, not the
instruction's low immediate-looking opcode bits. Reuse ILW/ISW lane
semantics (last enabled lane wins for ILWR, zero-extended low VI for
ISWR), 4 KB wrap, VI0 and four-cycle IALU load reservations. The VU1
register window still fails closed. Regression checks masking, wrap,
zero extension and E drain. Macro ILWR behavior is unchanged.

### VU0 flag setters

FCSET queues a 24-bit CLIP history replacement in the FMAC pipe, with
same-pair lower priority over upper CLIP. FSSET replaces sticky bits
10:6 and 11 (mask 0xfc0) while retaining current flags; same-pair upper
MAC/current flags still retire. The existing upper FMAC issue sidecar
encodes explicit FSSET using an otherwise-unused high bit in its sticky
field, so the state ABI is unchanged. Flag readers observe the previous
values until the four-cycle reservation matures. A synthetic pair test
covers both setters and E drain; more mixed FDIV/FSSET differential
coverage remains necessary.

## Full Linux CI baseline (4ea67e5, 2026-11-25 14:46 UTC)

[Workflow run 45637108081](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45637108081),
[combined summary artifact 15034111577](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45637108081/artifacts/15034111577).
PR head `4ea67e57ac1b42e50d53d51f46dcdcc6ab3d1f4c`; actual tested checkout is the PR merge commit
`6fa80c5c366c9d427474c172684642ec00f1f256`.
All five PR workflows and all eight original-autotest jobs succeeded.
The following rows are taken from the aggregation job log, not inferred
from earlier pilots. This replaces cumulative historical counts as the
current full-suite baseline.

### Recorded outcomes

Revision: `6fa80c5c366c9d427474c172684642ec00f1f256`

**27/55 exact transcripts (49.09%)**

PASS: 27 | DIFF: 12 | INCOMPLETE: 16 | NO_OUTPUT: 0 | ERROR: 0

| Test | Status | Stop | PC | Dispatches |
|---|---|---|---|---:|
| cpu/ee/alu | PASS | EXCEPTION | 0x80000180 | 278578 |
| cpu/ee/branch | PASS | EXCEPTION | 0x80000180 | 113978 |
| cpu/ee/branchdelay | DIFF | EXCEPTION | 0x80000180 | 2831 |
| cpu/ee/lsu | PASS | EXCEPTION | 0x80000180 | 67077 |
| cpu/ee/lsudelay | PASS | EXCEPTION | 0x80000180 | 1947 |
| cpu/ee/muldiv | PASS | EXCEPTION | 0x80000180 | 435078 |
| cpu/ee_cop0/performance | INCOMPLETE | UNSUPPORTED | 0x00100728 | 8193 |
| cpu/ee_fpu/arithmetic | PASS | EXCEPTION | 0x80000180 | 605920 |
| cpu/ee_fpu/branch | PASS | EXCEPTION | 0x80000180 | 8197 |
| cpu/ee_fpu/branchdelay | DIFF | EXCEPTION | 0x80000180 | 1981 |
| cpu/ee_fpu/compare | PASS | EXCEPTION | 0x80000180 | 247981 |
| cpu/ee_fpu/convert | PASS | EXCEPTION | 0x80000180 | 94875 |
| cpu/ee_fpu/fcr | PASS | EXCEPTION | 0x80000180 | 27695 |
| cpu/ee_fpu/muldiv | INCOMPLETE | DISPATCH | 0x00129830 | 1000000 |
| cpu/ee_fpu/sqrt | PASS | EXCEPTION | 0x80000180 | 120636 |
| cpu/ee_fpu/transfer | PASS | EXCEPTION | 0x80000180 | 13569 |
| cpu/ee_simd/arithmetic | PASS | EXCEPTION | 0x80000180 | 277556 |
| cpu/ee_simd/compare | PASS | EXCEPTION | 0x80000180 | 68679 |
| cpu/ee_simd/funnel | PASS | EXCEPTION | 0x80000180 | 18953 |
| cpu/ee_simd/logic | PASS | EXCEPTION | 0x80000180 | 144834 |
| cpu/ee_simd/muldiv | PASS | EXCEPTION | 0x80000180 | 679774 |
| cpu/ee_simd/shuffle | PASS | EXCEPTION | 0x80000180 | 237006 |
| cpu/vu0_macro/integer | PASS | EXCEPTION | 0x80000180 | 38275 |
| cpu/vu0_macro/transfer | INCOMPLETE | UNSUPPORTED | 0x00100544 | 8545 |
| dma/dmac/cpcond | PASS | UNSUPPORTED | 0xffffffe0 | 1851 |
| dma/dmac/tagintr | PASS | UNSUPPORTED | 0xffffffe0 | 2999 |
| dma/spr/chain | DIFF | UNSUPPORTED | 0xffffffe0 | 5149 |
| dma/spr/interleave | PASS | UNSUPPORTED | 0xffffffe0 | 141287 |
| dma/spr/normal | DIFF | UNSUPPORTED | 0xffffffe0 | 10494 |
| dma/vif/basic | DIFF | UNSUPPORTED | 0xffffffe0 | 3598 |
| dma/vif/intstall | DIFF | UNSUPPORTED | 0xffffffe0 | 5912 |
| dma/vif/mskpath3 | DIFF | UNSUPPORTED | 0xffffffe0 | 2186 |
| dma/vif/stcycl | INCOMPLETE | RETURN | 0x0010ef60 | 1000000 |
| dma/vif/stmod | DIFF | UNSUPPORTED | 0xffffffe0 | 31166 |
| dma/vif/unpack | DIFF | UNSUPPORTED | 0xffffffe0 | 26748 |
| gs/label | INCOMPLETE | EXCEPTION | 0x80000180 | 3449 |
| gs/signal | INCOMPLETE | EXCEPTION | 0x80000180 | 3470 |
| kernel/ee/interrupt/dmac_order | PASS | EXCEPTION | 0x80000180 | 10001 |
| kernel/ee/interrupt/intc_order | INCOMPLETE | EXCEPTION | 0x80000180 | 1276 |
| kernel/ee/thread/delete | INCOMPLETE | EXCEPTION | 0x80000180 | 1198 |
| kernel/ee/thread/exit | INCOMPLETE | EXCEPTION | 0x80000180 | 1211 |
| kernel/ee/thread/resume | INCOMPLETE | EXCEPTION | 0x80000180 | 1856 |
| kernel/ee/thread/sleep | INCOMPLETE | EXCEPTION | 0x80000180 | 1195 |
| kernel/ee/thread/stat | INCOMPLETE | EXCEPTION | 0x80000180 | 2081 |
| kernel/ee/thread/suspend | INCOMPLETE | UNSUPPORTED | 0xffffffe0 | 1785 |
| kernel/ee/thread/terminate | INCOMPLETE | EXCEPTION | 0x80000180 | 1198 |
| kernel/ee/thread/wakeup | INCOMPLETE | EXCEPTION | 0x80000180 | 2012 |
| memory/ee/default | DIFF | UNSUPPORTED | 0xffffffe0 | 2575 |
| vu/games/triace | PASS | EXCEPTION | 0x80000180 | 6710 |
| vu/lower/branch | PASS | UNSUPPORTED | 0xffffffe0 | 4322 |
| vu/lower/efu | INCOMPLETE | HOST_YIELD | 0x001128e0 | 76763 |
| vu/lower/fdivdelay | DIFF | UNSUPPORTED | 0xffffffe0 | 630717 |
| vu/lower/integer | PASS | EXCEPTION | 0x80000180 | 162309 |
| vu/lower/random | DIFF | UNSUPPORTED | 0xffffffe0 | 112360 |
| vu/upper/clip | PASS | UNSUPPORTED | 0xffffffe0 | 22464 |

### Reproducing CI inspection

From the repository root with authenticated GitHub CLI (Windows CMD):

```bat
gh pr view 156 --json headRefOid,headRefName,state,mergeable,statusCheckRollup
gh pr checks 156
gh run list --commit 4ea67e57ac1b42e50d53d51f46dcdcc6ab3d1f4c --event pull_request --limit 20
gh run view 45637108081 --json jobs,headSha,status,conclusion
gh run view 45637108081 --job 139287320007 --log
gh run download 45637108081 --name ps2autotests-summary --dir build/ci-autotests/45637108081
type build\ci-autotests\45637108081\summary.md
```

For a later revision, substitute the new PR head, workflow run and job
IDs; use `gh run view <run-id> --log-failed` for infrastructure failures.
Download all shard artifacts with `gh run download <run-id> --pattern "ps2autotests-*"`
into a separate run-specific directory. Never aggregate the combined
summary together with the six shard summaries.

### Result-recording contract

Keep the unmodified combined summary.json as the canonical machine-readable
record: runtime_revision (actual checkout), reference_revision,
native_tool_sha256, suite_total, selected, counts and results. Record PR
head, CI run ID/URL, run attempt if available, platform, and UTC timestamp
alongside it. Each result retains its original name, status, exit_name,
final_pc, dispatches and all other harness fields. Preserve the per-test
guest output, diff and runner report as evidence; do not replace them with
the compact table. Each of the 55 pinned names must appear exactly once.
Report all five status counts even when zero; a selected run or cumulative
historical match list is not a full-suite score. Compare two complete
runs by test name and report both newly passing and regressed names.
Do not conflate a PASS transcript with a successful guest exit.

### Next failure investigation

VU branch and triace already match at this revision. The remaining VU
failures are macro transfer (CMSAR1), EFU, FDIV delay and random. VU0
dual-issue arbitration remains an independently motivated accuracy audit,
not a proven blocker for these four original ELFs. DMA/VIF and kernel
lifecycle account for the largest clusters of remaining failures; inspect
actual guest diffs and stop reports before changing device behavior.
COP1 muldiv and VIF STCYCL hit the dispatch limit and require targeted
budget/loop investigation before attributing their failures to arithmetic.

### VU0 VF destination arbitration

VU User's Manual v6.0 section 3.4.3 and LQI remarks establish upper
priority at register granularity, including disjoint masks. The pinned
PCSX2 VU0microInterp.cpp skips the entire conflicting lower handler
but retains both FMAC reservations (VUops.cpp). Implement this for
supported VF-writing lower loads, transfers and RGET/RNEXT. This
suppresses lower index/R side effects, retains both lane dependencies,
and leaves nonconflicting old-value reads and VF0 behavior intact.
The existing I, flag and FDIV paths are unchanged. Candidate regression
and a mirrored PCSX2 interpreter probe cover LQI/LQD, disjoint and
overlapping masks, RNEXT, old-value MOVE and VF0. The manual differential
workflow now includes the focused reference probe. Physical-console
confirmation of all side effects and wider flag/FDIV interaction remain
outstanding. No original transcript pass is claimed before execution.

### COP2 transfer width audit and VU output ordering

At the 27/55 baseline, `cpu/vu0_macro/transfer` reaches CTC2 CMSAR1 at `0x00100544` (`0x48c8f800`) but already disagrees on several earlier register reads. The documented VI00–VI15 widths, STATUS sticky-bit write mask, CLIP width and CMSAR widths are handled at the CFC2/CTC2 boundary (VU User's Manual v6.0, sections 5.1.2–5.1.3). The successful CMSAR1 callback's stored address is returned by CFC2. Reserved registers are not assigned observed transcript values. No CMSAR1 execution success or original ELF pass is claimed here.

In the VU shard of run 45637108081 (artifact 15034356263), all 40 changed lines of `vu/lower/random` and all 321 changed lines of `vu/lower/fdivdelay` agree after removing whitespace **for diagnosis only**. Raw FILEIO writes already contain the misplaced spaces. The original test runner prints vector components with separate `printf` calls; the ELF compiles the separating space calls to `putchar`. Investigate guest stdio/EE/FILEIO ordering; exact comparison is unchanged. EFU stops while formatting EEXP CVF_MIN immediately after `7f7fffff/`. Reserved COP2 values, CMSAR1 resident microcode execution and the EFU formatting loop remain open.

### SPR normal DMA MADR.SPR bit

Full suite at PR head 17c83a5 (run 45640115217) remains 27/55, with
12 DIFF, 16 INCOMPLETE and zero NO_OUTPUT/ERROR. The macro transfer
now stops at the same CMSAR1 instruction after 8425 dispatches; VI01,
STATUS, CLIP and CMSAR0 match their original transcript. The VU1 start
report records 100000 zero microinstruction pairs without E, so it is
not legitimate to report synchronous completion of this launch.

The original dma/spr/normal diff (artifact 15035172734) first diverges
when the guest writes MADR bit 31 on channel 9, which reads back set and
causes a rejected transfer; subsequent transfers time out with STR
still set. The original hardware rejects the bit itself, not the DMA.
The pinned PCSX2 Dmac.h (82cac38) documents this bit as ineffective on
toSPR/fromSPR MADR. Clear bit 31 on channel 8/9 MADR writes only; retain
alignment and physical backing validation and all other channel rules.
The smoke test checks both directions, readback, QWC, STR, D_STAT and
payload wrapping. The original chain mismatch appears to involve the
source payload itself; investigate allocation/guest memory writes before
changing DMA tag handling. No new exact transcript is claimed yet.

### Kernel lifecycle dispatch

The pinned original thread/delete ELF stops at syscall 0x21 (V1=0x21,
EPC 0x00104964), but the runner reports a SetSyscall handler rejection.
0x21 was in the HLE allowlist without a matching branch; it fell through
to the final unconditional SetSyscall handler. Restrict SetSyscall to
0x74 and fail closed for unimplemented recognized services. Implement
ordinary DeleteThread (0x21) and TerminateThread (0x25), according to
PS2SDK syscallnr.h and original hardware thread/delete and thread/terminate
transcripts: self and invalid targets fail; delete requires dormant;
terminate accepts non-current ready/wait/suspend/waitsuspend and removes
semaphore queue membership. Reset current priority to initial on dormant
transition, including ordinary exit and return; avoid saving a dormant
CPU context during reschedule. Add status and synthetic guest dispatch
coverage for failed/valid operations, restart and ID reuse. Interrupt-safe
termination and additional scheduling behavior remain separate audits.

### Confirmed lifecycle baseline and invalid-state calls

PR head `b998426bbf84deb477ee96db2ae2227d52f2a2fd`, tested merge
`42d9f98c166fe7c5c00efbc8b0cb3b8432db195a`: [full suite run
45641696399](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45641696399),
[summary artifact 15034979929](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45641696399/artifacts/15034979929).
33/55 exact transcripts (60%): PASS 33, DIFF 13, INCOMPLETE 9,
NO_OUTPUT 0, ERROR 0. Compared with the 27/55 baseline, spr/normal and
thread/delete, exit, sleep, stat and terminate now pass, with no lost
passes. Resume and wakeup now complete but differ on invalid-state calls.

The pinned original thread/resume and thread/suspend transcripts require
-1 when the requested status transition is not possible. The original
thread/wakeup transcript requires -1 for self and dormant targets; valid
ready/wait/suspended cases already agree. Reject these before mutating
wakeup credits; keep interrupt callback semantics unchanged for active
threads. The synthetic sleep/wakeup program now obtains a real credit
from a second guest thread instead of expecting an invalid self wakeup
to succeed. The self-suspend rescheduling discrepancy remains open;
no scheduler timing or original ELF/expected output changes are made.

### Confirmed 35/55 and process-wide heap boundary

PR head `d324bb029e3e180760569663d13e6a2987399f26`, tested merge
`0a571bd18be1c17dbc15c1608fbf34ac3ad1dccb`: [full suite run
45642745870](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45642745870),
[summary artifact 15037570594](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45642745870/artifacts/15037570594).
All five workflows passed. PASS 35, DIFF 11, INCOMPLETE 9, NO_OUTPUT 0,
ERROR 0. Thread resume and wakeup are newly exact; no previous pass lost.

The memory/ee/default ELF initializes the heap from its startup thread
(SetupHeap at 0x00100150) but its harness creates a second EE thread
(entry 0x00100650, CreateThread call 0x001006fc, StartThread call
0x00100708). The real libc _sbrk implementation at 0x00109f10 calls
EndOfHeap at 0x00109f54 and rejects the requested new break if it exceeds
that limit. The previous HLE stored the limit on the startup thread only;
new threads saw zero. The reference OpenPS2BIOS
(`893b9fa96678f89c8e56cde28ffda00330cf35b1`, src/ee/kernel.c) uses a single
heap_end for SetupHeap/EndOfHeap, consistent with PS2SDK's shared libc
allocator. Store the limit at executable scope, keep existing range and
startup-context validation, and test an actual worker query and return to
the root. The allocation failure is a plausible shared cause of zero DMA
addresses and overlapping SPR chain buffers; exact effects on each test
must be confirmed by execution. No original binaries or transcripts changed.

### Self-suspend at the EE scheduling boundary

The original thread/suspend transcript (run 45642745870, kernel artifact
15036466734) matches the immediate self-SuspendThread status 08 but the
subsequent status after rescheduling is 08 instead of 01. The guest
continues executing until the scheduler excludes its current owner and
later stops at the worker return sentinel. Track the transient self-suspend
only for the ordinary syscall on the running thread. Preserve immediate
status readback, restore RUN at actual scheduling/IRQ-return boundaries,
and clear the marker on restart/dormancy/explicit resume. External
suspension and WAIT/DORMANT are not converted to RUN. The synthetic guest
calls SuspendThread, ReferThreadStatus, RotateThreadReadyQueue and
ReferThreadStatus, requiring ID/08/01. The exact original transcript
remains the acceptance criterion; this does not generate synthetic IRQs.

### Full-suite heap correction result (794e630)

Actual tested merge `4bbc5ba46189d4973dca22d18470ffeb66017d2b`;
[run 45644589028](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45644589028),
[summary artifact 15037646903](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45644589028/artifacts/15037646903).
**39/55 exact (70.91%)**: PASS 39, DIFF 11, INCOMPLETE 5,
NO_OUTPUT 0, ERROR 0. Newly passing: memory/ee/default, dma/spr/chain,
vu/lower/random and vu/lower/fdivdelay. No lost passes. EFU, VIF STCYCL,
GS label and GS signal now finish but retain differences. VIF1 basic
normal/chain DMA payloads now match; both direct FIFOs and VIF0 DMA
still need work. GS SIGLBLID access and GIF SIGNAL/LABEL commands are
unimplemented, independent of allocation. The guest allocator correction
also resolved the whitespace differences without any transcript changes.

### Confirmed 40/55 and GS SIGNAL/LABEL

PR head `dbc3506704d6bb81cd87d4c571d9551730c00150`, tested merge
`2aeb7ba17b714760d8b7cf080207e74ca2bb3f04`, [full suite run
45645532852](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45645532852),
[summary artifact 15036324591](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45645532852/artifacts/15036324591).
All five workflows passed. 40/55 exact (72.73%): PASS 40, DIFF 11,
INCOMPLETE 4, NO_OUTPUT 0, ERROR 0. All eight thread tests now pass.

Original GS label/signal packets previously failed because A+D 0x62/0x60
were rejected and SIGLBLID was unmapped. Based on PCSX2 `82cac38`
(Gif_Unit.cpp and GS.cpp), merge the masked 32-bit payload into the
selected SIGLBLID lane; SIGNAL sets CSR bit zero and raises INTC GS if
unmasked. Privileged 32/64-bit MMIO accesses share the same transactional
GS state. CSR write-one-to-clear acknowledges SIGNAL, and unmasking a
pending SIGNAL raises the IRQ. Commit IRQ only after full GIF validation.
Second unacknowledged SIGNAL still fails closed: automatic queued SIGNAL
and resumable partial GIF stalls, FINISH timing, and general GS reset
semantics are not implemented in this batch. Regression coverage checks
masked lane writes, CSR, DMA, IRQ masking/unmasking, acknowledgement,
and rollback of rejected transactions. No original transcript changed.

### Native EE quadword FIFO transaction

The original dma/vif/basic test (artifact 15036869035, head dbc3506)
uses SQ to write two VIF FIFO quadwords. VIF1 ordinary and chain DMA
both match but the FIFO output is zero. The LLVM slow SQ/SQC2 path
split every quadword into two ee_mem_write64 calls; c_ee_vif1 accepts
only 16-byte FIFO accesses. Preserve the hot RAM probe and paired
scalar compatibility API, and classify physical/KSEG-alias VIF0,
VIF1, GIF and IPU FIFO addresses on the slow path. Emit one full-width
write, with both trace lanes recorded before that indivisible write;
LQ/LQC2 use a guest helper with the same full-width FIFO read contract.
All existing non-FIFO paired scalar accesses retain their order.
New runtime helpers are additive; previously generated IR remains
linkable but must be regenerated to use the corrected FIFO behavior.
Regression checks the physical bus and native LLVM fixture, including
tracing, aliases and untraced execution. VIF0 is still unimplemented;
this change does not yet claim the entire dma/vif/basic transcript.

### Confirmed GS transcript result (3c265c8)

Tested merge `6a1db052493a18c7f18e4bf584ffce650bdb5eae`,
[run 45646776251](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45646776251),
[summary artifact 15039100663](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45646776251/artifacts/15039100663).
42/55 exact (76.36%): PASS 42, DIFF 9, INCOMPLETE 4, NO_OUTPUT 0,
ERROR 0. Newly passing gs/label and gs/signal. No previous exact
transcripts lost. Native WebGPU Linux smoke exposed a remaining
rollback fixture using SIGNAL as its invalid command; update it to
still-unsupported FINISH without removing the rollback/retry assertions.

### VIF0 command and DMA transport

Head `9597f97f166fcde8e3d6fc8d1bd06d01d4e500de`, tested merge
`908c8c2af89b700d7ec66b87c9b3e34608ea75e2`, [run 45647631828](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45647631828),
[summary artifact 15038027885](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45647631828/artifacts/15038027885).
All five workflows passed; 42/55 exact, 9 DIFF, 4 INCOMPLETE, no lost passes.
The original VIF1 direct FIFO path now transfers real quadwords.

VIF0 uses an independent instance of the common VIF parser, with its
transactional scratch buffers seeded from the live EE VU0 data/micro
banks and published only after acceptance. Address wrap is 256 data
quadwords / 512 microinstruction pairs; VIF1 TOPS addressing is not
applied. Route VIF0 registers/FIFO and channel 0 normal/source-chain
DMA, using the existing validated chain walker. VIF0 launches, IRQ
stalls, partial command readback and other UNPACK edge semantics remain
incomplete; unsupported commands still fail closed, and the shared
walker retains its existing TTE packing limitation. Regression checks
FIFO, normal DMA, REFE, VU0 wrap, MPG, reset and transaction rollback.

### VIF1 MSKPATH3 and GIF DMA mask

PCSX2 `82cac38` (Vif_Codes.cpp, vifCode_MskPath3) sets GIF_STAT.M3P
from immediate bit 15 and triggers GIF DMA when clearing it. Publish
this mask only after VIF transaction acceptance, and gate new PATH3 DMA
starts while masked. A genuine unmask retries only a transfer blocked
by that mask; explicit CHCR stop cancels the wait. GIF_STAT.M3P is now
observable via EE MMIO. This is a synchronous transfer-boundary model,
not cycle-accurate mid-packet GIF arbitration. FBRST preserves the mask
pending a full controller reset or another MSKPATH3; its detailed reset
semantics and other GIF_STAT bits require additional hardware validation.
Regression covers mask readback, preserved STR/MADR/QWC, transactional
rollback and real resumed GIF consumption/DMAC IRQ.

### Confirmed VIF0 transport result (72180fb)

Tested merge `fc04d5fa034c4a6dace720e45f35c7fed6cdc5f1`,
[run 45648797577](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45648797577),
[summary artifact 15039601757](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45648797577/artifacts/15039601757).
43/55 exact (78.18%): PASS 43, DIFF 8, INCOMPLETE 4, NO_OUTPUT 0,
ERROR 0. Newly passing dma/vif/basic; both VIF units now match FIFO,
normal DMA and source-chain uploads. No previous passes lost. The DMA
artifact 15036939897 shows stmod differs only in mode 3 ROW updates,
and unpack differs only in WL=0 and incomplete-command readback for
both units.

### UNPACK STMOD mode 3

The original stmod transcript on 72180fb differs exclusively in mode 3
ROW updates for VIF0 and VIF1 (artifact 15036939897). PCSX2 `82cac38`
Vif_Unpack.cpp::writeXYZW writes raw incoming data into ROW for mode 3,
only for data-selected lanes; V4-5 bypasses MODE. Apply this in the
common decoder and keep mode 3 off the memcpy fast path. Regression
checks both VIF units, masked ROW reads and packed V4-5 bypass.

### STCYCL WL=0 and incomplete UNPACK readback

On 72180fb, all 46 changed STCYCL lines are WL=0 cases; UNPACK differs
only in the WL=0 masked 256-vector fill and header-only register readback.
PCSX2 `82cac38` Vif_Unpack.cpp decodes WL=0 as 256 (CL remains raw).
Apply this consistently to payload word counting and destination cycling,
leaving CYCLE register readback unchanged. Expose committed pending
header as CODE, VPS=datawait, and the eight-bit NUM when no staged
vectors have retired. Whole-command transactions do not yet implement
progressive partial-payload writes or decrement NUM for individually
retired vectors. Regressions cover both units, mask/column fill through
vector 255, and header-only NUM 1/255/256 encodings.
