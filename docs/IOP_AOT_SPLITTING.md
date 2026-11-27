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
