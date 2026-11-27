# SDBZ known-function CPU-only matched replay

This is the fast, **zero-auto-expansion** alternative to a full Windows
SDBZ graphics boot. It is for testing host HLE, PADMAN, GS, and other
runtime changes against an already-known set of EE functions.

**It does not discover or add guest functions during either test run.**
A 6,000,000-dispatch replay is still real guest execution; the speedup
comes from eliminating hundreds of replay/relink cycles and disabling the
Windows WebGPU presentation path.

## Use the existing verified Windows cache (recommended)

In the Visual Studio Developer Command Prompt, from the repo root:

    git pull --ff-only
    scripts\run_ps2_game.bat --matched

The launcher still builds the host tools, validates existing guest
translation provenance, and attempts **verified host-only relink** when
required. It **will not** prompt for a guest LLVM IR rebuild or delete
cached functions in this mode. It runs one neutral and one eight-poll
Cross replay against the same translated library.

A successful pair writes separate reports, bounded branch traces and
\`comparison.json\` under:

    build\ps2-game-hle\known-replays\YYYYMMDD-HHMMSS-ffffff\

The JSON contains both dispatch counts and the original game's
instruction execution count at \`0x00109D50\`. It is not an input
observer proxy: these are instructions executed by the native guest.

If the existing cache cannot be validated or safely relinked, this
mode stops without regenerating any guest code.

## Migrate source fingerprints without rebuilding (PR #178)

LF and CRLF source checkouts now share versioned text fingerprints. ELF,
generated IR, object, library and authenticated manifest hashes remain
byte-exact. Legacy source fingerprints are accepted only if each recorded
hash can be reproduced from the corresponding current file (its actual
bytes, LF form or CRLF form). Files with bare CR or NUL are kept byte-exact.
Added, removed, renamed or genuinely changed source inputs fail closed.

For an existing canonical shared cache, authenticate all modules and
migrate only its source provenance:

    python tools\recover_translation_cache.py --elf PS2_GAME\SLUS_214.42 --base-work-dir build\ps2-game-hle --migrate-source-fingerprints

This is not recovery: it takes the shared IR lock, checks metadata and all
module hashes, and atomically publishes the normalized source map while
retaining the previous fingerprints and append history. It never invokes
the recompiler, compiles native objects, relinks, runs the game or deletes
a backup. Repeating the command should report zero fingerprints migrated.
Native profile receipts are migrated on their next verified link/publication;
changing the cache/link scripts themselves can still require a host relink.
Guest objects remain eligible for the existing authenticated reuse path.

Legacy runtime/dispatcher aggregate keys are reused only when an exact
legacy checkout, all-LF or all-CRLF key can be reproduced and the native
object bytes still match. If a historical mixed aggregate cannot be proven,
the host runtime/dispatcher objects must be recompiled; this never requires
re-lifting guest IR. Existing batch CRLF attributes are retained; no
repository-wide source checkout renormalization is required.

## Reconstruct known IR from the archived manifest

The diagnostic ZIP contains **function entry names and SHA-256 values**,
not the actual LLVM IR source. It is sufficient to seed a fresh,
isolated reconstruction using the local original game ELF.

For example:

    cmake --build build --config Release --target ee_recomp ee_game_runner
    python tools\run_known_replays.py --disc-root PS2_GAME --build-dir build --config Release --manifest sdbz-graphics-pr148-20261121-203021.zip --reconstruct --work-dir build\sdbz-known-1275

This uses the cached ordered list of 1,275 addresses. It calls
\`ee_recomp\` for the first entry and for subsequent **128-entry batches**,
reusing the compiled discovery index and the regular ownership checks.
It checks that every expected function exists, verifies its on-disk
SHA-256, then emits objects and links **once**. There is no guest replay
during reconstruction.

The reconstructed IR can differ from the archived IR when generated
with different code inputs or function boundary batches; the earlier
Linux experiment reported **1,273 / 1,275 identical hashes**. This tool
does not assume it can reproduce exact bytes. If any hashes differ, the
strict default stops before linking. For an intentional, isolated
experiment that permits named SHA mismatches, explicitly supply:

    --allow-ir-mismatches

The exact mismatched names are included in
\`ir-comparison.json\` when the rebuild proceeds and the result is never
described as byte-identical. Never use this option to overwrite or
invalidate an existing production guest cache.

**If the isolated reconstruction fails, inspect that directory rather
than pointing \`--reconstruct\` at the original cache.** The option
requires an unused work directory and refuses to overwrite artifacts.

## Cross experiment and output

Each pair uses \`run_game.py --no-recompile --auto-expand=0\`, the
authentic extracted-disc HLE startup, \`--kseg-ram\`, a
1,000,000-instruction loop watchdog, 16 real-progress resumptions and
a 2,048-poll SIF limit. Default limits are:

- 6,000,000 dispatches per run
- 600-second wall-clock timeout per run
- Cross injection at real SIO2 port-0 polls 200 through 207
- An original-EE-PC instruction trace filtered to \`0x00109D50\`

Neutral explicitly clears inherited Cross environment variables for
its subprocess. Cross uses the same library but installs its own
controlled input environment. The Windows launcher runs both without
WebGPU, raylib or PPM capture.

The experiment reports the total original branch executions, including
in the matched input-path reference: **neutral 0, Cross 8**. Those
values are an observed result from the prior run, not a hardcoded
test expectation. The script fails if an expected trace is truncated,
if a replay does not reach the requested dispatch budget, if a report
is missing, if the guest exits unexpectedly, or if translation-cache
metadata changes during the pair.

The script accepts \`--max-dispatches\`, \`--timeout-seconds\`,
\`--emit-jobs\`, and \`--output-dir\` overrides.

## Validation and scope

The \`ee_known_function_replays\` CTest validates the manifest parser,
hash mismatch detection, 128-entry batching, single link, safety on
existing caches, controlled two-profile invocation, full dispatch
requirement and original branch trace interpretation.

**The CTest uses synthetic fixtures.** It is not a substitute for a
real matched game replay. The game ELF and PS2_GAME disc files remain
local to the developer, so end-to-end replay must be performed in the
environment where those assets exist.

The separate \`--full\` path retains normal auto-expansion and GS
graphics diagnostics for when new code needs to be discovered.


## Recovery after translation-cache invalidation (PR #148)

Verified translation caches now create **content-addressed known-function
manifest snapshots** under `build/ee-known-manifests/`. Snapshots contain
the original ELF digest, IR module entry names and digests, their original
ordered discovery/batch sequence, lowering source fingerprints, and other cache
provenance. They do **not** contain copyrighted
game code or the actual IR. Validation of a preexisting cache also creates
a snapshot, so an existing Windows cache is migrated on its first successful
check without triggering translation.

The launcher still tries exact cache validation and the verified host-only
relink/interrupted-append recovery *first*. If neither works, the normal
launcher attempts **strict** reconstruction using the largest compatible
known-function inventory, building each entry without a guest replay.
Strict unattended recovery requires the **same ELF and same guest-lowering
sources and byte-identical reconstructed IR**. It never silently accepts
changed IR, an unknown ELF or incomplete objects. On failure the original
rebuild confirmation remains authoritative; declining preserves the cache.

To request **reconstruction only** without running the game:

    scripts\run_ps2_game.bat --recover-cache

If guest lowering was deliberately changed, or legitimate batch-boundary
differences cause IR hashes to differ, the strict mode fails safely. After
reviewing the code change and understanding that OLD IR equivalence is not
established, explicitly opt in:

    scripts\run_ps2_game.bat --recover-cache --accept-new-ir

This invokes `tools/recover_translation_cache.py --allow-ir-changes`.
It **still requires the identical original ELF**, the exact known
function-entry set, verified generated LLVM IR, a successful object/link
pipeline, a complete translation-cache provenance record, and final cache
validation. The old IR SHA-256 comparison is recorded in
`ir-comparison.json`; it is *not* described as byte-identical or
semantically equivalent. New guest-function discovery remains disabled in
the reconstruction step.

Every attempt builds into a uniquely named child staging directory:

    build/ps2-game-hle/recovery-YYYYMMDD-HHMMSS-ffffff/

Only after successful reconstruction, linking and validation does recovery
move the staged contents into the canonical cache. It retains the old
contents until post-promotion validation succeeds; exceptions restore the
original names. Legacy sibling caches and active pointers are still
recognised but no new active pointer is written.

By default, the old IR, metadata, shared provenance and translation/object
inventory receipts are retained in a SHA-256 inventoried
`backup-<stamp>-ir.7z` (or an uncompressed directory if 7-Zip is missing
or fails). Native profiles already attached to the canonical IR remain in
place as candidates for subsequent authenticated reuse. Displaced native
files and reports are retained uncompressed in `retained-<stamp>/`.
They are not part of the IR archive. Existing backups are never removed.
An IR-only backup requires native validation/rebuild before execution; it
is not an immediate native DLL rollback. To retain the older complete
native backup behavior, opt in:

    scripts\run_ps2_game.bat --recover-cache --full-native-backup

Compression remains synchronous so the completed archive is tested
before deleting the source directory. IR-only backups use level 1;
explicit full native backups retain level 9. The console reports
input/archive size and compression/test elapsed time.

### Windows compression benchmark (2026-11-27)

Measured on the canonical 16,252-module cache with the benchmark at
`41ff520`. Sizes below are decimal MB; times include archive testing.

| Scope | Level | Input MB | Archive MB | Compress + test (s) |
| --- | ---: | ---: | ---: | ---: |
| Full | 9 | 3042.085 | 152.274 | 299.312 |
| IR only | 9 | 1021.734 | 41.372 | 87.016 |
| IR only | 3 | 1021.734 | 81.658 | 10.141 |
| IR only | 1 | 1021.734 | 97.999 | 6.000 |

Level 1 reduces measured compression/test time by 293.312 s (49.89x)
relative to full level 9. Relative to IR level 3 it costs 16.341 MB
more per archive and saves 4.141 s. This benchmark excludes backup
partitioning, inventory authentication, promotion and native builds;
it does not establish an end-to-end recovery speedup. The temporary archive is tested before the old directory is
removed. If partitioning fails, the full displaced tree is retained
uncompressed. Legacy full backups remain readable by the existing
object-seeding path.

To compare compression without changing the active cache or triggering
recovery, run:

    python tools\cache_backup.py --benchmark-compression build\ps2-game-hle --output ee-backup-benchmark.json

The isolated benchmark creates and tests temporary archives for the full
current payload at level 9 and the IR-only payload at levels 9, 3 and 1.
Historical backups and retained data are excluded. The benchmark holds
the shared cache lock and reports the payload sizes and elapsed times;
it does not measure promotion or inventory SHA-256 generation. Temporary
archives are discarded; the JSON report is retained outside the cache.

After extracting a new IR-only archive into an isolated directory,
verify its complete SHA-256 inventory before using its contents:

    python tools\cache_backup.py --verify-ir-backup PATH_TO_EXTRACTED_DIRECTORY

Both the normal launcher's strict fallback and explicit recovery path reuse
the same batched emitter/linker in `run_known_replays.reconstruct` but **do
not execute the neutral/Cross replay pair**. Once a recovered cache is
selected, ordinary boot sessions can resume guest execution and discover
*additional genuinely missing functions* through the unchanged expansion
path. This workflow is not a replacement for a full guest/game/graphics
validation after changing the recompiler's semantics.

`ee_known_cache_recovery` CTest exercises snapshot selection, content
integrity, exact-ELF scoping, changed-source opt-in, safe active pointers,
failure preservation, and activation only after validation. Its fixtures
are synthetic; testing against a real user-owned 1,296-function cache
remains an end-to-end acceptance step.


## Recovery and known-IR refresh timings (R3)

Actual recovery and overwrite-IR operations publish a console stage summary
and a timestamped JSON report beside the canonical cache. The Python tools
also accept `--timings-report PATH` (must be a new file). Stage times are
inclusive, not additive. Child recompiler discovery, coverage and LLVM IR
emission times use the existing native timing output; process wall time
includes startup, ELF loading and other overhead. Native verification reports
cache hits/misses, worker count, total worker time, per-worker parse/verify,
emission and I/O/hash sums, compile wall time, throughput and slowest module.
Link and backup timing records distinguish preparation, actual linker time,
compression and archive testing. Failed operations report failure without
suppressing the original exception. No extra IR scan is performed for timing.
The existing native discovery timing cannot separately resolve ELF loading
or internal metadata serialization; the Python metadata timings cover the
instrumented Python operations. A report is not execution or cache provenance.


## Native object reuse (R4, first slice)

Fresh native object stores can combine matching candidates across multiple
inventories instead of selecting only the largest inventory. Every copied
object must match the current IR digest, LLVM version, target triple, native
CPU target, optimization profile, backend level, pipeline and exact object
size/SHA-256. Corrupt candidates are skipped; the ordinary incremental
emitter and linker still verify the result. Verified IR migration also
checks extant canonical/legacy/retained O0 object stores. The original
candidate files are not modified.

Overwrite-IR can skip native compilation and linking only when the existing
DLL receipt, current shared metadata and hashes, source/native inputs, and
complete native object inventory all validate. Any mismatch follows the
ordinary incremental build path. This does not skip requested IR generation.

Recovery now reconstructs IR separately from native emission. Even with
explicitly changed lowering, the current verified IR hashes are used to
seed native objects from extant canonical, legacy and retained inventories.
Only identical modules with compatible native identity can be reused.
Strict recovery still refuses any change to module bytes or ordered entries.


## Selected-profile recovery (R5)

Recovery accepts `--emit-opt 0..5` and builds only that native profile.
For nonzero profiles the staged canonical IR has independent shared
provenance and the selected child owns the native objects/library/receipt.
The complete selected profile is validated before and after transactional
promotion. Existing unrelated native profiles remain candidates, not
implicitly validated libraries. O0 remains the default for compatibility.

    set SDBZ_EE_OPT=5
    scripts\run_ps2_game.bat --recover-cache --accept-new-ir

Omit `--accept-new-ir` for strict identical-lowering recovery. The launcher
also recognizes a valid shared source without a root O0 translation receipt
when preparing a subsequent optimized full replay. No intermediate O0 build
or retail replay is required. The explicit recovery mode may rebuild a
selected profile even when its current native cache is already valid.
