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
