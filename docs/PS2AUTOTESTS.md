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
pilot executes alu, muldiv and fcr via the same built runner; inspect the
uploaded ps2autotests-pilot artifact for actual outcomes. Guest timeout
excludes compilation; CI job time bounds the total.

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
