# IOP AOT splitting and compilation audit (I1)

Tracking: [#174](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/issues/174).
Source baseline 40900f7ce9c8049c184be3f1f3e30ce7ddbe9ea9 (G8D.2
retirement semantics retained). This audit changes no runtime behavior.

## Current pipeline

* `src/tools/ee_game_runner_main.cpp`: module-loaded callback records
  relocated, import-linked .text from IOP RAM, FNV-1a-64, base, text_size,
  entry, gp and export addresses. Current capture also stores source_name
  and first export_library. It does not preserve all ordinal/library pairs,
  relocation records, import stub ranges or a code/data map.
* `src/iop/irx_loader.{h,cpp}`: the live loader has validated entry,
  GP, export library/ordinal/address triples and linked import resolution.
  Capture text alone cannot reconstruct every original relocation or
  distinguish code/data with certainty. Existing entry and export addresses
  are sufficient to start conservative discovery without a new capture.
* `src/tools/iop_recomp_main.cpp`: sorts capture metadata, reads and
  checks full text hash, truncates trailing incomplete words for legacy
  translation, then serially generates one code IR and one table IR per
  capture. It ignores entry/exports for translation. No function discovery.
* `src/iop/iop_ir_lifter.cpp`: a single `iop_aot_fn` has a switch case
  and guest instruction block per aligned text word, irrespective of
  reachability. Branch delay instructions are also emitted on control
  paths. Direct jumps use fixed labels when in range; indirect JR/JALR
  dispatch via the module switch. Unsupported instructions and external
  targets return precise state to the CPU. Thus there is no function
  ownership, code/data discrimination or explicit import/tail analysis.
* `tools/build_iop_aot.py`: stages named captures, invokes one serial
  lifter process, then uses a bounded module-level ThreadPoolExecutor.
  Each worker compiles code and table sequentially and links one library.
  A large code module cannot use multiple compiler processes internally.
  Non-bootstrap mode clears output files; bootstrap uses private staging
  but still compiles the whole supplied inventory. No per-function cache.
  Aggregate build time is printed; per-stage/module build times, native
  code sizes and peak build memory are not established.
* `src/iop/iop_aot.{h,cpp}`: activation checks complete physical text
  base/range/hash; active ranges scanned linearly. Physical alias lookup
  does not imply that an alias is a supported native entry (lifter
  dispatches exact original PC). Do not normalize guest architectural PC
  silently. Preserve original IRX filenames and activation identity.
* `src/iop/iop_aot_abi.h`, `src/iop/iop_cpu.cpp`: one descriptor entry
  per library. Context carries GPR/HI/LO/RAM, callbacks, budget, retired
  count, PC/next PC/delay state/branch PC. CPU admits only ordinary
  instruction boundaries with no active guest exception mode, caps the
  budget at modeled hardware horizons and retires device time before
  interpreter fallback. Non-RAM accesses refuse native callbacks. Any
  split path must preserve these conditions and zero-progress fallback.
* `tests/smoke/iop_aot_test.cpp`: synthetic native library, per-slice
  interpreter comparison for budgets 1–17 and attached Timer5 device
  boundaries. Existing CPU model does not implement R3000 load delay
  slots (see lifter header); splitting is not a reason to change that
  separately. Add split/monolithic/interpreter comparison.

## I2–I4 proposed contracts

1. Start a shadow inventory using only aligned in-range module entry and
   export addresses, recording rejected seeds. Follow reachable static
   successors and direct call targets; never scan arbitrary unvisited
   text for apparent JAL instructions. Track executed delay words
   separately from ordinary entry PCs. Stop at undecoded or ambiguous
   instructions, and record indirect exits and out-of-image targets.
2. A direct call target is a function candidate, not proof of a unique
   owner. Record shared tails, overlapping reachability, independently
   targeted delay words, and unresolved indirect/jump-table paths.
   Use explicit conservative CFG regions or interpreter fallback where
   ownership is not proven. In particular, don't treat all text words
   as functions or all unvisited text as definitely data.
3. Initially retain the descriptor ABI and one library per IRX. A
   generated module dispatcher maps only certified entry/resume PCs to
   split functions. All unowned/ambiguous PCs fall back without progress.
   Start with explicit cross-region exits rather than native recursion;
   preserve delay-state and retirement and test exact budget partitions.
4. Compile independent code/table objects with one shared bounded job
   pool; link a library only after its objects complete. Cache keys must
   bind relocated bytes/base, ownership, lowering inputs, ABI, optimizer,
   host target and LLVM version. Never overwrite accepted G8D.1 outputs.

## Measurements and gates

G8D.2 matched 40M retail replay is the accepted runtime baseline (seven
active IRXs, 67,846,006 native instructions; 41.937 vs 40.000 s wall).
One pair is not a general speedup claim. Capture binaries, complete
per-module coverage, baseline compile stage timings and object sizes
are not available in the present source audit. Collect those with
existing capture inventory and native files when the comparison
requires them; no new capture or replay requested for I1.

Initial I2 should be read-only and independently testable, emit
stable ownership and unresolved-edge receipts, and not alter activation,
native lowering, ABI, hardware events, or guest clocks. I3/I4 should use
fresh build directories, synthetic differential tests, then a separately
pinned split-versus-monolithic G8D.2 replay. Do not combine with a changed
EE timing producer in the same retail experiment.

## I2a: read-only candidate inventory

`tools/audit_iop_function_discovery.py` reads the original capture metadata
and linked bytes, validates the FNV-1a identity, discovers candidate entries
from recorded roots and reachable direct calls, and emits deterministic
ordinary/delay ownership, overlaps, boundary and unresolved-edge records.
The narrow R3000A classifier stops at unsupported encodings and does not
claim that all visited bytes are code or that all unvisited bytes are data.
Call continuations are candidates only. Delay words never seed functions
unless independently reached or explicitly exported. Indirect targets,
unsupported delay slots and external edges are reported, not invented.
The offline Python tool does not feed the C++ lifter; actual native region
certification should share the runtime decoder/lifter semantics. Receipts
are exclusively created and no capture or existing receipt is replaced.

Synthetic CTest `iop_function_discovery` covers direct calls, unreachable
words, shared tails, explicit delay entry, rejected seeds, indirect calls,
unsupported delays and capture identity. Next collect the existing retail
capture receipt before choosing a bounded native region pilot:

```bat
python tools\\audit_iop_function_discovery.py --capture build\\iop-g8d-capture --out iop-g8d-discovery.json
```

No new retail run or AOT rebuild is needed for this inventory.

## I2b: received retail inventory / I3a: synthetic split lowering

Archive SHA-256 744b097e9875c5e2ef1c0a7bd2f9d2d5e46732b9c076da71b203fb65cb59a303.
Seven captures, 413 candidate regions, 17,812 ordinary and 3,755 delay PCs,
35,101 unvisited aligned words (56,668 total); no reported shared ordinary
PCs, ordinary/delay overlap, or rejected seeds. The 574 unresolved records
include returns and external jumps; they are not 574 decoding failures.
The receipt is not raw capture data or a dynamic coverage trace.
SIO2MAN (67 candidates, 438 ordinary / 143 delay PCs, 976 words, four
indirect calls) is the first prospective retail split pilot. MCMAN
(161 candidates, 14,263 discovered words out of 18,148) is the later
compile scalability pilot. Unvisited code must retain interpreter or
legacy native fallback; do not assert it is data.

I3a adds an explicit, validated disjoint-region emitter and one dispatcher
under the existing descriptor ABI. Native direct edges outside a region
exit with the ordinary resume PC; indirect targets stay in the region or
exit to the CPU. Delay slots still read full original linked text and
retain G8D.2 budget/fallback semantics. The dispatcher checks zero budget
and unowned PCs before calling a region. Production iop_recomp is not
connected yet; the synthetic AOT differential compiles four regions plus
router/table into one library and checks budgets 1–17, cross-region calls
and returns, missing ownership, overlapping regions and Timer5/MMIO.
No retail build/replay until CI validates the synthetic split path.

## I3b/I4a: opt-in single-module build pilot

The builder now supports `--split-module SIO2MAN` with a fresh output
folder. It resolves the exact staged canonical stem (case insensitive),
recomputes the discovery from linked bytes, refuses overlaps/rejected
seeds, excludes unsupported boundaries and writes a private base/size/hash
bound ownership plan. The C++ tool rechecks plan identity and validates
all region PCs before generating separate code IR, a dispatcher and a
normal descriptor table. The default whole-module path is unchanged.
One bounded compiler worker pool compiles region, router and table
objects; link once, retaining one library and original identity. Stage
and compile/link timing and byte counts are printed. No incremental cache
or native chaining yet. This builds only the selected pilot IRX, not all
seven libraries, and must not overwrite accepted G8D.1 outputs.

Synthetic tests and CI precede any retail split acceptance. The first
retail comparison should copy the six original non-pilot libraries to a
fresh directory alongside the replacement SIO2MAN library and verify
all seven identities. Use the unchanged G8D.2 runner, EE/VU1 images and
recorded input, not the new EE timing shadow. No EE IR rebuild needed.

## I4b: accepted SIO2MAN split replay

User archive ee-iop-i4b.zip SHA-256
f20352bc384436400021461978de4ed8dca0cbfce69b4fee653976a2a47c8669.
Pinned G8D.2 runner SHA-256
6cb539ba63b4790d1e034eb041d04ce5c42eec7a614e7346057389bec40b977f.
40M dispatches, 3,127/3,127 input polls, 184,700 exact sampled transitions,
7/7 active modules, 65,069,064 native IOP instructions. Report and PPM
byte-identical, input receipts unchanged. Split SIO2MAN build at O3/jobs=8:
67 regions, 69 objects, 0.516s compile, 0.109s link, 125,283 object bytes,
55,808 DLL bytes. Matched interpreter 40.422s / mixed 39.875s. Prior
monolithic 40.000s is a separate observation, not a paired speed claim.

## I4c: opt-in content-addressed object reuse

`--region-cache DIR` is available with a split module and fresh output.
Cache keys bind individual emitted IR, full linked image/metadata and
ownership plan, lifter and clang executable hashes, clang version/target,
optimization flags and platform. Cached object bytes and size are verified;
corrupt entries fail closed. New entries publish object and receipt together
from a private sibling directory. All objects are linked again; default
build behavior remains unchanged. Cache-enabled compilation disables clang
default config files. This intentionally conservative key invalidates all
regions when the linked image, plan or lifter changes; finer reuse across
such changes is not yet claimed. Synthetic tests cover hits, input/option
invalidation and corruption. MCMAN is the next retail split candidate;
validate cold/warm reuse independently of runtime performance.

## I5/I6 measured results and I7a residual coverage

MCMAN cold/warm O3/jobs=8: 161 regions, 11,593 eligible PCs, 163 objects;
3.563s/0.187s compile/cache stage, 0.125s/0.141s link, 4.1s/0.7s total;
163/163 warm objects reused. Both DLLs 1,199,616 bytes (size equality only).
I5b retained split SIO2MAN and MCMAN: 40M dispatches, 3,127 polls,
184,700 exact trace samples, seven active libraries, exact report/PPM,
65,068,824 native instructions and 11,468,684 entry attempts.
I6 all-split archive SHA256 20cd8b1c4ff7f006211149ec23ea730af21bbb02abad1a63cd4ec0d14308466e:
same correctness gates, but only 7,272,474 native instructions and
53,526,827 entry attempts. Matched interpreter 40.750s vs candidate 44.015s;
performance rejected. Only 17,689 aligned PCs remain eligible in the
conservative function plans out of 56,668 captured words. Static coverage
cannot identify which module accounts for most dynamic loss. Keep I5b.

I7a adds opt-in `--split-residual`: preserve candidate regions and append
one disjoint residual region containing every remaining aligned PC,
including independently enterable delay words and classifier stops.
These are dispatch entries, not newly discovered functions or proven code.
The existing C++ lifter retains its own unsupported/exception/MMIO
fallback and reads delay words from the full linked image. No ABI, runner,
EE or VU1 change. The cache plan hash separates conservative and residual
builds. This restores the legacy dispatch domain, not necessarily legacy
batch lengths or native retirement, since region exits still return to
the scheduler. Compare exact retail state and native retirement/entry
counts before considering this default. One residual object per IRX may
still be large; further partitioning or chaining is a separate experiment.

## I8a: extended discovery shadow audit

CRI_ADXI I7 screenshot: dispatcher about 408 KB, region 0 about 131 KB,
residual region 27 about 8,371 KB (Explorer display units). The residual
is a compatibility partition, not satisfactory function discovery.

`audit_iop_function_discovery.py --extended` compares the original seed
inventory against two independent additions: plausible unvisited stack
frames (aligned negative ADDIU SP followed by in-frame stores ending in
an RA save), and possible sequential continuation through CPU-supported
LWL/LWR/SWL/SWR and exact supported COP0 encodings, including delay slots.
Unsupported delay boundaries remain reported. These are candidate-only
heuristics, not a proof of function ownership or native support; linked
text may contain data. Baseline discovery and all existing build/cache
paths are unchanged. The extended receipt records new reached words,
frame seed evidence, interpreter continuations and ownership conflicts.
Do not promote heuristics directly into the builder until audited.

Next: compare receipts for CRI_ADXI/ARKD_DVD; then mine original IRX ELF
symbols and relocations, preserve linked import stubs and code/data
metadata, and resolve indirect targets from bounded register value
analysis and observed guest PCs. Retain residual compatibility; if large
regions remain, test bounded partitions separately from discovery.

## I8b: measured extended discovery / opt-in build pilot

Receipt iop-i8-discovery.zip SHA256
6ba67e3814302da9a679cb2a1bb2f0bbd0512f3584ffbf839963ae0f571310a6.
413 -> 895 candidate roots; 21,567 -> 39,395 reached aligned words
(38.1% -> 69.5%); 189 heuristic stack-frame roots, 60 interpreter-linear
PCs; no shared ordinary PCs, ordinary/delay overlap or rejected seeds.
Remaining unvisited words: 17,273. ARKD_DVD 356 -> 10,671 reached words
out of 11,208, CRI_ADXI 435 -> 5,488 out of 12,508, PADMAN 2,163 ->
2,810 out of 8,008. Largest extended candidate in ARKD_DVD: 1,610
ordinary PCs; CRI_ADXI: 184. These are candidate ownership measurements,
not dynamic execution coverage or proof of ELF function symbols.

`--split-extended` enables this audit's regions for an explicitly opt-in
build and requires `--split-residual`. All aligned PCs remain in the exact
disjoint dispatch partition; interpreter-only PCs and unsupported
boundaries are excluded from candidate function regions and routed
through the residual compatibility region. The C++ emitter, ABI,
exception/MMIO handling and default builder remain unchanged. The
existing ownership-plan hash separates cache entries. The only change
relative to I7 is region assignment, and unsupported opcodes still fall
back through the same lifter. No runtime acceptance until a distinct
retail experiment. I7 remains separately reproducible.

## I8c: original IRX metadata audit

`tools/audit_iop_irx_metadata.py --capture DIR --disc-root PS2_GAME --out FILE`
scans loose .IRX files, validates ELF32 little-endian MIPS Sony IOP
relocatable headers and table bounds, extracts executable STT_FUNC
symbols and REL entries, and compares IOPMOD name, entry, GP and text
size against each linked capture. Those are metadata matches, not a
proof of original ELF identity; ambiguous matches are reported separately.
For text relocations, R_MIPS_26 JAL and R_MIPS_32 values must agree with
the linked word before becoming candidate evidence. Ordinary differing
non-relocation words are recorded (including linked import stubs).
Data pointers are not automatically promoted to function roots. The
receipt reports missing matches, parse errors and coverage of loose files;
embedded/archive IRXs are not searched. No build or runtime integration.
Synthetic tests cover a relocated JAL, metadata mismatch and ELF bounds.

## I7 acceptance / I8c measured evidence / I9a relocation shadow

I7 archive SHA256 0cd89d570773c8f499333f58d6d1aab9603cbe3562d6c1b3332343399c9fdca8:
40M dispatches, 3127 polls, 184700 identical trace samples, seven active
libraries, exact report and PPM, unchanged inputs. Native retirement
67,846,006 equals G8D.2; 11,767,550 entries. Interpreter 45.719s vs native
43.453s, a single pair, not general performance acceptance.

I8c archive SHA256 7824244f812623c5773a01482b31e92c3cf6ddfb3db7c606cc979c237bc3e0e7:
seven loose IRXs parsed, six exact metadata matches, zero function
symbols. 2,617 verified relocated JALs to 773 unique module-scoped
in-text targets; 176 targets not reached by extended discovery. ARKD_DVD
had no metadata match. I8 extended build: ARKD_DVD 9,348 discovered /
1,860 residual PCs, 161 objects, 5.953s compile; CRI_ADXI 4,273 discovered /
8,235 residual PCs, 251 objects, 201.968s compile. The dominant compile
object is not measured in that receipt.

I9a adds `--discover-relocations` to the original ELF audit. For a unique
metadata match, verified relocated JAL targets not already reached and
with both site and target in executable sections seed extended recursive
discovery. No raw R_MIPS_32 pointer promotion. This remains shadow-only,
reports full ownership/conflicts and capture identity, and discards
shadow output for ambiguous original ELF matches. Unmatched metadata
now reports actual differing fields, including ARKD_DVD. No build path,
runner, or existing I7 artifact changes.

## I9b: split build attribution

The experimental split builder reports completion order (rather than
submission order) while retaining deterministic object order at link time.
It writes split-build-receipt.json in the fresh output root with per-object
IR size, object size, elapsed compile/cache time, reuse status, wall-stage
timings and linked library SHA256. The five slowest objects print to the
console. This measures the suspected CRI_ADXI residual bottleneck rather
than assuming it. No emitted IR or runtime changes; old outputs stay intact.

## I9 measured relocation discovery / I10 build integration

Receipt iop-i9-discovery.zip SHA256
bbf3eb37ca71a18aba35747522b27566fa04aff4fe7a886a8b19e1b66f751904.
176 new roots, 9,293 newly reached words across six matched IRXs, no
shared ordinary PCs, ordinary/delay conflicts or rejected seeds. Including
unchanged ARKD_DVD: 1,071 candidates and 48,688/56,668 reached words
(85.9%). CRI_ADXI residual plan projected 4,549 PCs (from 8,235);
PADMAN 3,060, MCMAN 4,589, MCSERV 412, LIBSD 1,465, SIO2MAN 427.
CRI_ADXI largest eligible function 1,745 PCs; no claim that residual is
necessarily the slowest compiler object.

ARKD_DVD only mismatch was sanitized capture name versus original
ARIKA IOP-Control-Driver. Match ASCII punctuation normalization exactly
as the capture callback does, still requiring all numeric fields and
unique match. No runtime activation identity change.

Opt-in `--split-relocation-receipt FILE` requires extended residual
splitting. Match receipt by linked base/size/hash rather than canonical
staged name; revalidate each root's executable section, previously
unreached status, verified JAL site and linked opcode/target. Recompute
CFG and reject ownership conflicts through the existing builder. Never
consume saved ownership lists as authoritative. Existing full dispatch
partition, C++ fallback, cache plan hash and runner remain unchanged.
The receipt is candidate evidence rather than a cryptographic proof of
original ELF provenance. No default build changes or I7 artifact changes.

## I11 retail acceptance / I12a indirect target shadow

I11 archive SHA256 a25632b7da96bf829d1d576f5f6ea8d12428a5ce613d900d42ac06ddfced50fc.
Seven active relocation-backed split libraries; 40M dispatches, 3,127
polls, 184,700 exact trace samples, unchanged inputs, independently checked
byte-identical report/PPM matching the pinned reference. Native retirement
67,846,006 matches monolithic and I7. Native entries 12,611,629 versus
11,767,550 I7; 596.6ms translated / 1.9ms refresh. 40.921s interpreter vs
40.375s native, one matched pair (performance acceptance false). All five
CI workflows passed at 20cd4c0. Preserve I11 as correctness baseline.

I12a `audit_iop_indirect_targets.py --capture DIR --relocation-receipt FILE
--out FILE` reconstructs the I10 candidate CFG from validated relocation
roots. For each unresolved indirect JR/JALR, it walks at most 32 unique
known sequential predecessor words, stopping at other entry roots,
delay words, alternate predecessor edges or non-simple instructions.
Tracks LUI, ADDIU, ORI, ADDU and OR constants, clears tracked values on
unmodeled instructions, and reads the jump register before the delay
slot. No GP, memory contents, unknown incoming indirect edges or return
addresses are assumed. Reports possible constant targets, producer PCs,
unresolved sites and distinct unvisited call/jump targets. JR targets
are not treated as new function roots. This is shadow-only: existing
CFG may omit indirect incoming edges, and no runtime or lowering uses
these observations. Tests cover delay ordering, alternate entry,
unknown loads and jump/call separation.

## I12a measured result / I12b producer shadow

I12a receipt iop-i12-indirect.json SHA256
3acc01e3624c78c0861301f93d76bbee9d00fecc9fbebfe2d07f600af77bf9a0:
zero recovered constant targets, 91 unresolved sites (81 indirect calls,
10 jumps), 85 use v0 and six v1. Calls: CRI_ADXI 61, PADMAN 14,
SIO2MAN 4, LIBSD 2. Jumps: ARKD_DVD 4, MCMAN 2, LIBSD 4.
Do not infer a pointer's source solely from its register number.

I12b adds optional `--audit-producers` to the same offline audit. It
records a bounded flat definition DAG for the same unique sequential
predecessor window, tracks known GPR writes and stores, and classifies
load addresses (constant, GP-relative live-in, scaled index or unknown
register address). Register moves are followed for classification.
No guest memory is dereferenced; no GP value or table bounds assumed.
Unknown incoming paths and producer values outside the window remain
unresolved. This is diagnostic only; existing constant audit results,
I11 build, runner, ABI and EE/VU1 artifacts are unchanged.

## I12b measured result / I13a relocation-backed table shadow

I12b receipt SHA256 d6e766e43278b5c2486996d3c0f82d13f0eec8f389052c7ca0402c50c3dffd0e:
91 unresolved sites; 41 live-in, 38 register-address loads, 11 scaled-index
loads, one constant-address load. ARKD_DVD four scaled-index JR sites:
0x67790 / 0x724a8, 0x683b4 / 0x72548, 0x689c4 / 0x72620,
0x69474 / 0x726c0 (site / table base). All bases beyond captured text.
CRI_ADXI nested pointer loads are consistent with callback/vtable access,
not proof of dynamic targets.

I13a adds audit_iop_jump_tables.py. Reconstructs accepted discovery and
producer slices; reopens the unique original ELF and verifies its SHA256
against I10. Recognizes direct LW of constant plus index<<2 and signed
offset, requires a unique allocated non-executable file-backed PROGBITS
section, and scans at most 256 consecutive uniquely R_MIPS_32-relocated
slots with aligned executable text targets. Reports original addend-based
targets, whether a linked word was available for verification, known
ownership and the first stopping reason. These are pointer-run candidates,
not proven switch bounds or immutable live values. No automatic roots,
CFG changes, builder changes or runtime changes. Missing scale/base
producers and indirect calls remain outside this audit.

## I13a measured result / I13b table-edge shadow

I13a receipt SHA256 36fc70a48053374f76686da93ff89557be178999a596b0889f4ac2c7e829435d.
Six candidate .rodata tables out of ten indirect JR sites; 138 slots,
51 distinct source-target edges, 46 previously unreached destinations.
ARKD_DVD: four tables (18/50/33/8 slots), 30 new destinations.
LIBSD: two tables (9/20 slots), 16 new destinations. Remaining two
MCMAN and two LIBSD sites have unsupported address expressions.
All slots outside linked capture; no live word verification or proven bounds.

I13b adds --discover-targets to the table audit. Recompute candidates
from the same pinned original ELF and capture, deduplicate site-target
pairs, validate aligned in-text indirect jump sources/targets, and add
candidate CFG edges to the existing recursive ownership walk. They are
not function entry roots; any newly found direct call roots are reported
separately. Retain unresolved indirect jumps and report ownership conflicts
without resolving them. Candidate table edges never feed the builder or
runtime. Synthetic tests verify same-function discovery, shared ownership,
invalid source rejection and unchanged default inventory. I11 stays pinned.

## I16a — Global split compilation queue (implementation; validation pending)

The split builder accepts repeated `--split-module STEM` selections or
`--split-all` (mutually exclusive). Each selection uses the same staged
capture naming, independent ownership plan, relocation receipt validation
and module-specific cache provenance as the single-module path. All
generated objects share one bounded `--jobs` compilation pool. Larger IR
files are queued first as a deterministic cost proxy; historical
object timing prioritization remains future work. Linking remains serial
and preserves the original sorted region/router/table order for each IRX.

For the existing relocation-backed capture:

```bat
python tools\build_iop_aot.py ^
    --build-dir build ^
    --capture build\iop-g8d-capture ^
    --out build\iop-native-split-i16a ^
    --split-all ^
    --split-residual ^
    --split-extended ^
    --split-relocation-receipt iop-i10-discovery.json ^
    --region-cache build\iop-region-cache ^
    --opt 3 ^
    --jobs 8
```

Use a fresh output directory. For multiple modules, the aggregate
`split-build-receipt.json` records global compilation wall time, summed
link time, object/cache totals and each library hash. Detailed
`split-receipts/STEM.json` files contain the original single-module
receipt fields; per-module compile time is the interval between its first
object start and last completion (overlapping intervals are not
additive). Single-module builds retain the original receipt location
and shape.

Synthetic regression cases cover case-insensitive exact selection,
duplicate/missing selections, global largest-first scheduling with one
worker, deterministic per-module link order, and cold/warm cache reuse.
Real compiler/cache validation and comparison against accepted I11
library hashes are still required. No ownership, lifter, runtime, EE or
VU1 semantics changed. A retail replay is unnecessary only when all
candidate library artifacts are byte-identical to accepted artifacts.

### I16a local build results

The uploaded cold/warm receipts report all seven libraries, 1,095 objects,
9,126,099 object bytes and 5,820,928 DLL bytes. The global compilation
wall time was 34.172 s cold and 1.265 s warm (all 1,095 objects reused);
linking took 0.953 s and 0.938 s respectively. Per-object IR and object
sizes and per-module DLL sizes matched. All seven DLL SHA-256 values
differ between cold and warm and from I11. PE timestamps are a possible
explanation, not established. Full byte-level comparison is required
before inheriting retail acceptance on artifact identity grounds.

## I13b/I13c — Table-backed ownership

I13b reached 678 additional aligned words (243 ARKD_DVD, 435 LIBSD),
raising static coverage from 48,737/56,668 to 49,415/56,668 (87.20%).
The function candidate count remains 1,074; no shared ordinary ownership,
ordinary/delay overlaps or rejected seeds. Eligible ordinary ownership
increases from 40,352 to 40,868; residual dispatch PCs fall from 16,316
to 15,800 (ARKD_DVD 1,814 -> 1,655, LIBSD 1,465 -> 1,108).
Six non-exhaustive tables supply candidate edges only; four address
expressions remain unsupported. Dynamic jump targets and bounds are
not established.

I13c adds opt-in `--split-table-disc-root PS2_GAME` to the existing
relocation-backed residual splitting path. For every selected module it
matches a unique capture identity, checks the original ELF path is inside
the disc root and verifies its SHA-256 against the relocation receipt.
It checks original ELF metadata, independently reconstructs relocated
call roots, reruns the indirect producer and table analysis, and recomputes
the CFG. Saved shadow ownership is never used as input. Ambiguous
ownership is rejected; all remaining aligned words retain residual
dispatch coverage. No dynamic jump lowering, bound assumptions, or
runtime changes are introduced.

For an isolated I13c build, select ARKD_DVD and LIBSD using repeated
`--split-module` flags, `--split-residual --split-extended`,
`--split-relocation-receipt iop-i10-discovery.json`,
`--split-table-disc-root PS2_GAME`, `--opt 3 --jobs 8` and a fresh output
directory. Use the existing region cache if desired. Compose the candidate
with unchanged I11 libraries for the other five modules and run a matched
retail replay before accepting the ownership change. Regression coverage
includes canonical staged naming, original ELF hash rejection and exact
residual partition preservation. CI and real retail validation pending.

## I13c and I16a acceptance

I13c retail archive `ee-iop-i13c.zip`, SHA-256
`bd182cbfb7021e418c7f496a936dc7e3b83139b60c959982745c9f74a8f7e5f6`,
is accepted for correctness. Candidate:
`build/iop-native-split-i13c-candidate/modules`. All five unchanged
library hashes match I11, and the two replacements match the I13c
build receipt. At 40,000,000 EE dispatches: 184,700 common trace samples,
zero divergent/unpaired samples, 3,127/3,127 polls, 7/7 active IOP modules,
67,846,006 native retired instructions, 12,611,090 entry attempts.
Report and framebuffer independently match byte-for-byte. EE attempts,
cycles, GS fields and VU1 native pairs match I11; inputs unchanged.
Interpreter 40.938 s, native 40.171 s, native execution 590.0 ms,
refresh 1.8 ms. One pair is insufficient for performance acceptance.
I11 remains a rollback baseline.

The two-module I13c compile took 5.782 s with zero cache hits and 0.249 s
linking (244 objects). ARKD_DVD DLL: 1,049,600 bytes; LIBSD: 443,904 bytes.
LIBSD residual IR fell from 781,137 to 528,541 bytes, compile time
4.891 to 2.016 s (different build concurrency). ARKD_DVD residual IR
fell from 753,389 to 632,664 bytes.

I16a acceptance: all 21 uploaded DLLs match their receipt SHA-256 values.
I11 versus both I16a builds differs only at byte offsets 0x80/0x81
inside the COFF TimeDateStamp field (PE signature offset 0x78).
With only the four-byte COFF timestamp normalized, all seven modules'
full-file hashes match across all three builds. No extra retail replay
is required for I16a. I16b adds `-Wl,/timestamp:0` to the Windows
split and monolithic linker commands; compilation and cache keys are
unchanged. A local Clang/LLD cross-target smoke test confirmed the PE
timestamp is zero. Actual Windows full-build raw hash reproducibility
remains to be validated on the next needed link.

## I14a — Cross-block indirect producer shadow

`audit_iop_indirect_targets.py` and `audit_iop_jump_tables.py` accept
`--cross-block`. This opt-in shadow audit walks unique known sequential
and non-linking branch/jump predecessors, includes simple predecessor
delay-slot instructions in execution order and records crossed CFG
edges. It stops at roots, multiple incoming edges, calls/returns,
unsupported instructions, cycles or the existing bounded window limit.
Unknown dynamic incoming edges and R3000A load-delay state prevent
the resulting producer evidence from being exhaustive or authoritative.
No guest memory values are assumed. Default discovery and I13c
production ownership are unchanged. Tests cover a constant formed in
a jump delay slot and barriers at call resumes and ambiguous incoming
edges.

```bat
python tools\audit_iop_indirect_targets.py ^
    --capture build\iop-g8d-capture ^
    --relocation-receipt iop-i10-discovery.json ^
    --audit-producers --cross-block ^
    --out iop-i14a-indirect.json

python tools\audit_iop_jump_tables.py ^
    --capture build\iop-g8d-capture ^
    --disc-root PS2_GAME ^
    --relocation-receipt iop-i10-discovery.json ^
    --discover-targets --cross-block ^
    --out iop-i14a-tables.json
```

Compare against I12/I13b before enabling any new candidate edges in
production. No native rebuild or replay is needed for this audit.

## I14b/c — Adjacent table candidate boundaries

I14a receipt archive `MIPS-To-LLVM-IR-To-C.zip` (SHA-256
`2740935db404a0775ed209236a04a6f2e8181c054f47237767a31b655f8d4800`)
identifies producers for all 91 unresolved indirect sites: 69
register-address loads, 14 scaled-index loads, eight constant-address
loads, no live-in-only producers. No constant dynamic targets are proven.
All four formerly unsupported jump table expressions were recovered.
MCMAN adds two tables (16 and seven slots), 123 reached words and 83
eligible ordinary PCs. LIBSD adds two table expressions (40 and nine
slots), but the first run overlaps the independently recovered table at
0x50330: JR 0x4eb60, start 0x502e0, crosses into the table for JR 0x4ed50.
The uncapped shadow reports 168 shared ordinary PCs between owners
0x4eaec and 0x4ecd8.

Re-traversing the uploaded graph with that run capped at 0x50330
removes 12 candidate edges and all 168 shared ordinary ownership
conflicts without losing any reached ordinary PCs or adding roots.
The expected capped inventory has ten tables, 190 pointer slots,
85 distinct candidate edges, 890 additional reached words relative
to the relocation-only inventory (49,627/56,668 in total), 41,013
eligible ordinary PCs and 15,655 residual dispatch PCs. These are
offline predictions, not a new accepted capture recomputation.

`--cap-adjacent-tables` in the shadow audit caps a contiguous run at a
separately recovered, nonempty, relocation-backed table base. Equal-base
sites and empty candidates do not impose caps. The receipt retains the
uncapped count and stop reason, the candidate boundary and its sites.
Neither switch index bounds nor runtime table contents are proven.
Cross-block audits also record the original instruction window including
branch and delay-slot words for future range-guard analysis.

`--split-cross-block-tables` enables both cross-block producer traversal
and adjacent candidate caps in the builder's existing original-ELF
validation path. The builder still rejects ambiguous ownership and
preserves residual dispatch, dynamic JR/JALR handling and the default
I13c behavior. Synthetic tests cover adjacent table overlap, unchanged
reachability, unresolved dynamic jumps, opt-in cross-block ownership,
instruction context and exact residual partition preservation.

Only MCMAN and LIBSD require rebuilding for the candidate; retain the
other five accepted I13c libraries. Validate the new shadow receipt
with `--discover-targets --cross-block --cap-adjacent-tables` and run
the pinned 40M-dispatch retail replay. I13c remains the accepted
correctness baseline until that validation passes. No EE/VU1/runtime
changes or EE rebuild are needed.

## I14c acceptance and range-guard evidence

Archive `ee-iop-i14c.zip`, SHA-256
`0c898bf802832ca2e928c9aa672e53127bc5d2e06589d60e0a9737ade7b565aa`.
The capped shadow exactly confirms the predicted inventory: 1,074
candidate roots, ten tables, 190 slots, 85 distinct edges, 49,627 reached
words, 41,013 eligible ordinary PCs and 15,655 residual PCs, with no
ownership conflicts, delay overlaps or rejected seeds. No dynamic target
exhaustiveness claim is made.

Retail correctness accepted: `build/iop-native-split-i14c-candidate/modules`.
The 40M-dispatch replay has 184,700 matching samples, zero divergent
or unpaired samples, 3,127/3,127 polls, 7/7 active modules, 67,846,006
retired native IOP instructions and 12,611,090 native entries. Report
and framebuffer independently match byte-for-byte. Inputs unchanged.
Both replacement DLL hashes match the receipt and all five reused hashes
match I13c. Both replacement COFF timestamps are zero, validating the
I16b linker option on Windows (not yet a repeat-link reproducibility test).
Control 41.062 s, candidate 40.172 s, translated execution 591.5 ms,
refresh 1.7 ms. No performance acceptance on a single pair.

The uploaded predecessor contexts also show an SLTIU index comparison
and a BEQ-to-zero guard on the fallthrough path into the index shift
at each of the ten table sites. The immediate bounds match the
capped pointer counts: ARKD_DVD 18,50,33,8; MCMAN 16,7; LIBSD 9,20,20,9.
In particular LIBSD 0x4eb44 compares the index against 20 before the
branch at 0x4eb48, supporting the independently recovered boundary at
0x50330. This is static path evidence, not proof that all dynamic
arrivals obey the guard or that table memory is immutable.

## I16c — Bounded residual dispatch partition pilot

MCMAN and LIBSD I14c compilation: 296 objects, zero cache hits,
17.906 s compile, 0.281 s link. The MCMAN residual object alone took
17.906 s (2,069,643 IR bytes, 727,140 object bytes). Remaining residual
counts: CRI_ADXI 4,549; MCMAN 4,506; PADMAN 3,060; ARKD_DVD 1,655;
LIBSD 1,046; SIO2MAN 427; MCSERV 412.

The opt-in `--split-residual-max-pcs N` requires residual splitting.
It slices the ascending residual PC list into deterministic chunks
of at most N dispatch PCs after the existing function regions, with
no change to function discovery or function-region IDs. The full
aligned dispatch domain is checked for exact coverage without
duplicates. No lifter or runtime change; delay-slot emission remains
the lifter's responsibility. Default plans remain unchanged.
These chunks are dispatch partitions, not discovered functions.
More boundaries may increase dispatcher exits or code size; retail
correctness and compile/run performance must be measured separately.

Initial pilot: 1,024 residual PCs per partition for CRI_ADXI, MCMAN,
PADMAN (5, 5, 3 partitions respectively). Keep the four unaffected
I14c libraries. Retain original ELF/relocation and cross-block table
options. Use a fresh output path and existing region cache; changing
the plan SHA invalidates the current plan-bound object cache even for
unchanged function IR. Do not compare warm and cold times as if
equivalent. Regression fixtures cover limits of 1, 3, 4 and 8, invalid
limits, unchanged function IDs, exact coverage and default behavior.
Latest CI and real candidate validation pending.

## I16c acceptance

Archive `ee-iop-i16c.zip`, SHA-256
`114ae5d4408962cf92b7a0ffe38a64cf6386a2e2ae9b842776dd743b3d0af715`.
Accept `build/iop-native-split-i16c-candidate/modules` as the retail
correctness baseline; I14c remains the rollback baseline.
All pinned inputs and VU1 inventory match I14c. All three replacement
DLL hashes match their build and replay receipts; four unaffected
library hashes match I14c. All three replacement timestamps are zero.
40M EE dispatches, 184,700 matching traces, zero divergent/unpaired,
3,127/3,127 polls, 7/7 active native IRXs, 67,846,006 retired native IOP
instructions, 12,611,093 entry attempts (three more than I14c).
Report and framebuffer independently match byte-for-byte.
Control 40.578 s, candidate 39.938 s, translated execution 575.7 ms,
refresh 1.7 ms. Performance acceptance remains false (one pair).

Three-module cold compile: 703 objects, 0 cache hits, 12.781 s compile,
0.470 s link. MCMAN's slowest residual object fell from 17.906 s to
2.672 s (different queue workloads, not matched whole-build timing).
The largest remaining compile object is CRI_ADXI.region.83.ll, a
discovered function (1,233,831 IR bytes, 8.250 s), not a residual region.
DLL sizes: CRI_ADXI 1,324,032 B; MCMAN 1,743,872 B; PADMAN 821,248 B.
Combined size 3,889,152 B, 188,416 B below I14c (4.62% reduction).
Retain 1,024-PC residual partitioning for these three tested modules;
other IRXs remain unchanged.

I16c CI had a single discovery fixture assertion failure in
CTest fast-2: the test expected "requires" while the actual
error used "require". Corrected in a separate commit; no runtime
or builder semantics changed by that fix.

## I16d — Exact-IR object cache reuse across ownership plans

Opt-in `--region-cache-ir-key` requires `--region-cache`. The schema-2
cache key includes exact IR SHA-256, source filename, capture and
staged metadata identity, lifter and compiler identity, compiler
options/target/platform, and all other existing provenance except
the ownership-plan SHA. The plan remains independently validated
before lifting. Identical object inputs can therefore survive a
changed partition plan; changed IR or compiler/capture inputs still
invalidate entries.

On a schema-2 miss, the builder checks only the current plan's exact
legacy schema-1 key, validates size and SHA-256, and atomically
promotes a hit into the new namespace without modifying the old
entry. Corrupt entries are rejected. No scanning or trust of other
plans' legacy receipts. Default cache behavior remains unchanged.
Synthetic tests cover legacy promotion, cross-plan reuse, unchanged
legacy keys, provenance/source-name invalidation and corruption.

Local acceptance: first rebuild the three I16c modules with the
same 1,024-PC plan and existing cache, with the new flag and fresh
output. Require 703/703 hits and all three raw DLL SHA-256 values
identical to I16c. Then build MCMAN alone with a fresh 2,048-PC plan
and the same schema-2 cache; unchanged function IR should hit
without having to compile it again. The 2,048-PC build is a cache
experiment only, not a new accepted retail library. No retail replay
is needed for raw-identical I16c artifacts.

## I16d acceptance

Archive `iop-i16d-validation.zip` SHA-256
`97cea6e7a95aeb6a028d64ee0abf9bc7e546e88aaae2a0a76477a988470dce63`.
The three-module seed run reused 703/703 objects, compiled in 1.891 s,
linked in 0.609 s and produced all three DLLs byte-identical to the
accepted I16c archive. All four uploaded DLLs match their receipts and
have zero COFF timestamps. All five CI workflows passed at 61c6ea8.
No new retail replay is needed for the seed run.

The MCMAN 2,048-PC cross-plan experiment reused 214/218 objects:
213 discovered function objects and the module table. Only residual
regions 213, 214, 215 and the dispatcher missed. Compile wall time
3.891 s, link 0.156 s; DLL size 1,783,296 B versus 1,743,872 B with
the 1,024-PC plan. Keep the accepted I16c configuration; the 2,048-PC
library is not accepted for retail execution.

## I17a — Compact module dispatcher pilot

The original router emits a switch case for every dispatch PC, even
when thousands of cases share the same region. The opt-in
`--split-compact-router` transforms only the generated module router
IR after the C++ lifter validates the ownership plan. It verifies
the complete original PC switch against the plan, then uses a private
dense i16 region-ID map indexed by the aligned module-relative PC.
The lookup checks unsigned offset range and alignment before accessing
the map; unowned entries use a sentinel and retain the original
fallback. The final switch has one case per region rather than one
per PC. The budget check, region call bodies, context writes and
fallback return reasons remain unchanged. No guest target speculation,
runtime changes, chaining or region ownership changes.

Regression fixtures check plan mismatch, duplicates, misalignment,
holes, unchanged fallback and high unsigned PCs. A Linux/Clang
fixture compiles the original and compact router and compares return
codes and all context bytes across valid, invalid and zero-budget
entries. CI pending at time of writing.

Pilot on CRI_ADXI, MCMAN and PADMAN with the accepted 1,024-PC plans,
`--region-cache-ir-key` and `--split-compact-router`. Expect 700/703
cache hits with only three router objects rebuilt; actual sizes and
retail performance must be measured. Reuse the four unaffected I16c
libraries and run the pinned retail replay. Do not accept the new
router on static size alone. I16c remains the accepted correctness
baseline until validation.

## I17a acceptance

Archive `ee-iop-i17a.zip` SHA-256
`69591dcd5c6fba3089295a64c5ae786dbabf71b49be3faa57922f172be02580d`.
Accept `build/iop-native-split-i17a-candidate/modules` for retail
correctness and size. Pinned input hashes, VU1 inventory and environment
match I16c. Three replacement DLL hashes verified against build and replay
receipts; other four hashes unchanged. Report and framebuffer byte-identical
to control. 184,700 matching trace samples, zero divergent/unpaired,
3,127/3,127 polls, 7/7 active modules, 67,846,006 retired native IOP
instructions and 12,611,093 entry attempts. No runtime speedup established:
control 40.875 s, candidate 40.031 s, translated execution 579.4 ms,
refresh 1.7 ms; one matched pair, performance acceptance false.

700/703 objects reused; only three routers compiled. Compile 0.781 s,
link 0.483 s. Combined router objects 581,056 -> 126,975 B (-78.15%).
Affected DLLs 3,889,152 -> 3,814,400 B (-74,752 B, -1.92%).
CRI_ADXI 1,299,968 B, MCMAN 1,708,544 B, PADMAN 805,888 B.
Object file bytes include relocations and other metadata, so object
size reduction is not directly equivalent to linked DLL reduction.
I16c remains rollback baseline.

## I17b — Narrow region maps and compact router rollout

Optional `--split-narrow-router` requires `--split-compact-router`.
Select i8 region IDs for up to 255 regions (IDs 0..254); reserve
the all-ones sentinel for holes. Larger modules retain i16 and the
existing i16 sentinel. No ownership, ABI or region code changes.
Fixtures cover 255/256 boundary, hole sentinel, range/alignment,
fallback and native equivalence with original switch.

Pilot on MCMAN/PADMAN with accepted 1,024-PC partitions, plus
ARKD_DVD/LIBSD/MCSERV/SIO2MAN with their accepted unpartitioned
residual plans. Preserve CRI_ADXI's I17a compact i16 router. Use
`--region-cache-ir-key` in both builds. Assemble all six replacements
with the unchanged CRI_ADXI DLL and run one pinned retail replay.
This is a router-only experiment; legacy cache promotion on the
four newly compact modules depends on retained cache provenance.
Require unchanged native retirement/entry counts and exact retail
trace/report/framebuffer before adoption; measure linked size separately.

## I17b acceptance

Archive `ee-iop-i17b.zip` SHA-256
`5d95be3837a198bc6b39291c1a6ba0d6d65ce8b6def1eb5a0038b9a5b44784fa`.
Accept `build/iop-native-split-i17b-candidate/modules` for retail
correctness and linked size, not demonstrated performance. All six
replacement DLL hashes independently verified against build/replay
receipts; all six COFF timestamps zero. CRI_ADXI unchanged.
Pinned input hashes, VU1 inventory and environment match I17a.
Report and framebuffer independently match byte-for-byte; 184,700
matching trace samples, zero divergent/unpaired, 3,127/3,127 polls,
7/7 active modules, 67,846,006 native instructions and 12,611,093
entry attempts. Interpreter 40.953 s, candidate 40.610 s, native
execution 613.1 ms, refresh 1.9 ms. Native execution is 5.82% higher
than I17a on a single pair; monitor with repeated matched runs.
I17a remains rollback baseline.

Partitioned build: 391/393 hits, compile 0.453 s, link 0.312 s.
Remaining build: 398/402 hits, compile 1.109 s, link 0.470 s.
Only six routers missed. I17b library sizes: ARKD_DVD 1,016,320 B,
LIBSD 424,960 B, MCMAN 1,690,112 B, MCSERV 119,296 B,
PADMAN 797,696 B, SIO2MAN 91,136 B, CRI_ADXI 1,299,968 B.
All seven total 5,439,488 B; reduction 73,728 B from I17a and
148,480 B from I16c. MCSERV and SIO2MAN linked sizes unchanged.

## I17c — Shared memory helper pilot

Opt-in `--split-shared-memory-helpers` requires split compilation.
After validated lifting, check that region helper prefixes agree,
and rewrite recognized width-constant memory calls to hidden,
width-specialized shared wrappers. Each IRX gets its own helper
object (STEM.helpers.ll) using the original lifter helper definitions.
The wrapper calls the original always-inline implementation with
constant width, preserving RAM mapping/alignment, mirroring, MMIO
callbacks and return values without a runtime width switch after
optimization. Caller guards, instruction retirement and delay/fault
exits are untouched. No guest ABI/runtime or ownership changes.
Unrecognized call shapes are rejected; unaffected region IR remains
byte-identical and eligible for cache reuse. Existing router/table
objects are unchanged.

Pilot MCMAN only with the accepted I17b narrow router and 1,024-PC
residual partition plan, using exact-IR caching. Replace only its DLL
in the I17b inventory. Measure actual linked size, compile time and
native execution time; use a pinned retail replay for correctness.
Outlining may increase hot-path call overhead, so do not infer a
runtime speedup from smaller binaries. Tests cover all three widths,
unchanged memory-free regions, unexpected call shapes and comparison
of compiled inline/shared RAM and callback paths. CI pending.

## I17c acceptance

Archive `ee-iop-i17c.zip`, SHA-256
`09dd102df2e1f2b2abe1112cd3165f5576256afdba4dc2a20cbb839182b6cac8`.
Accept `build/iop-native-split-i17c-candidate/modules` for retail
correctness and linked size. MCMAN's DLL SHA-256 independently matches
the build and replay receipts; the other six inventory hashes, pinned
inputs, VU1 inventory and environment match I17b. COFF timestamp zero.
Report and framebuffer independently byte-identical. 40M dispatches,
184,700 matching trace samples, zero divergent or unpaired, 3,127/3,127
polls, 7/7 native modules, 67,846,006 retired instructions, 12,611,093
entries. Interpreter 41.312 s, candidate 40.296 s, native execution
585.4 ms, refresh 1.7 ms; one matched pair, performance acceptance false.

MCMAN DLL size 1,690,112 -> 1,308,160 B (-381,952 B, -22.60%).
Object bytes 2,215,711 -> 1,903,573. 221 objects, 45 hits, 176 misses,
4.000 s compile, 0.156 s link. Shared helper object 1,720 B.
All seven DLLs now total 5,057,536 B. I17b remains rollback baseline.

## I17c rollout (I17c2)

Apply the existing shared-memory-helper option to the remaining six
IRXs without changing ownership, partitioning or router encoding.
Build CRI_ADXI/PADMAN with 1,024-PC residual partitions and
ARKD_DVD/LIBSD/MCSERV/SIO2MAN with unpartitioned residual regions.
Use compact+narrow router options in both builds (CRI_ADXI retains
i16 because its region count exceeds 255). Preserve the accepted
I17c MCMAN DLL, exact-IR cache, pinned EE/VU1/runner artifacts and
all previous original-ELF validation options.

Compose a fresh seven-library candidate and perform one pinned
40M-dispatch correctness replay before accepting the rollout.
Do not infer runtime performance from linked size or a single pair.
If a performance regression appears, retain rollback and isolate
modules with repeated matched runs before adopting the new lowering.
Cross-region execution attribution and budget-aware chaining remain
subsequent checkpoints; no runtime rebuild is required for this rollout.
