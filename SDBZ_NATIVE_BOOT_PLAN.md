# Super Dragon Ball Z (US) — Native Boot Development Plan

> **Current checkpoint (2026-11-25):** [Retail gameplay, audio and runtime validation](docs/SDBZ_RUNTIME_VALIDATION_20261125.md) supersedes the historical November 18 status below. Bounded training gameplay, native graphics and audible audio have been observed; full-game and hardware parity are not established.

**Repository:** [twist84/MIPS-To-LLVM-IR-To-C](https://github.com/twist84/MIPS-To-LLVM-IR-To-C)  
**Status date:** 2026-11-18  
**Current merged baseline:** `main` at `b3584825e115a6a1072c2fc7a0a960344f26c044` (squash-merged [PR #141](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/pull/141), 95/95 CTests).  
**Goal:** Boot and eventually play **Super Dragon Ball Z (USA, `SLUS_214.42`)** using native recompiled EE code, HLE EE/IOP kernel services, original game-supplied IRX code and a native hardware runtime. **OpenPS2BIOS is an opt-in reference, not the target boot dependency.**

**HLE-first migration decision:** [docs/HLE_FIRST_RUNTIME_PLAN.md](docs/HLE_FIRST_RUNTIME_PLAN.md) supersedes the earlier firmware-first target architecture. Historical ROM0 replays and evidence below remain valid *for that old path*; they are not evidence that a no-BIOS HLE boot already works.

> This is a milestone and acceptance plan, **not** a statement that the game currently boots or renders a usable frame. Update the checkpoint and evidence as new PRs merge. Re-check `main` and any newer work before acting on these instructions.

## 1. Architecture and non-negotiable invariants

The architecture should remain closer to [ps2-web-recomp](https://github.com/snuri00/ps2-web-recomp) than PCSX2:

- **EE/R5900:** statically recompiled via MIPS → LLVM IR → host-native objects; native dispatcher owns each reachable guest entry.
- **IOP/R3000:** emulate IOP BIOS/kernel services with explicit, tested HLE; execute **game-supplied IRX modules** with the existing R3000A interpreter. Keep [OpenPS2BIOS](https://github.com/TamaribaEmu/OpenPS2BIOS) optional for reference and regression, not normal boot.
- **CDVD/SIF/DMA:** use the original disc sectors and actual guest-visible registers, data, callbacks, and interrupts.
- **GIF/GS:** consume original guest GIF packets and maintain real GS state/VRAM; native primitive rendering can follow once demanded by guest execution.
- **PCSX2:** hardware behavior reference and differential oracle, **not** the runtime architecture.

Never fake `CHCR.STR` completion, an IRQ, a semaphore signal, ROM0 readiness, sector contents, GS acceptance, or a frame merely to increase EE dispatches. A dispatch-limit stop, untranslated entry, true hardware stall, and successful boot are different outcomes.

Keep source under Git; **never commit the game ISO/ELF, ROM0 BIOS, copyrighted assets, or bulky generated native objects**.

## 2. Confirmed baseline and evidence

The following were implemented and merged by 2026-11-17:

| PR | Validated work |
|---|---|
| [#121–#124](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/pull/124) | CDVD reset MMIO, N-command parameter FIFO, real DMA3 request state |
| [#125](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/pull/125) | Actual raw-ISO CDVD reads into IOP RAM via DMA3 |
| [#126](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/pull/126) | Genuine ROM0 SIF/WaitSema response; SIF1 DMA10 terminal-tag semantics; R3000/EE startup support; guest-anchored function-pointer discovery; 256-function cached dispatcher chunks; GIF normal DMA → GS |
| [#127](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/pull/127) | GIF channel 2 source-chain **END** tag transfer, real inline QWC delivery, bounds/consumer rejection tests |

**Genuine IOP → EE handshake evidence:** EE DMAC channel 5 IRQ enters guest handler `0x00178068`; `iSignalSema(8)` changes count 0 → 1; blocked `WaitSema(8)` resumes and consumes that token. The SIF readiness blocker is closed.

**Genuine disc reads:** LBA 16 (ISO PVD), LBA 261 (1 sector), LBA 289 (135 sectors), LBA 450 (4 sectors beginning `7f 45 4c 46`, ELF). The source is a real 2048-byte-sector US ISO.

**GIF/GS evidence:** normal `CHCR=0x101` transfers reached the PATH3 parser; 32 GIF tags, 16,384 IMAGE QWC and 65,536 written GS VRAM pixels were recorded. The next observed source-chain transfer used `CHCR=0x105`, `TADR=0x00503310`, a 9-QWC `END` tag (`0x70000009`), and was accepted by the GS consumer.

**Previously confirmed replay:** **12,168 EE dispatches**, final PC **`0x00102E24`**, untranslated native entry. **PR #129 Stage 1 validation replay:** **12,190 EE dispatches**, final PC **`0x00108240`**, reason **`UNSUPPORTED(4)`** on a genuine VU0/COP2 macro instruction `0x4BE821BC`. The `0x00102E24` entry and its two jump-table siblings were recovered/translated. No boot completion or rendered frame is implied. A later `END` tag (19 QWC at `0x005033B0`) was observed in the diagnostic stream; do not infer another missing DMA mode from this without a fresh guest trace.

**Resolved Stage 1 ELF evidence:** a guest `jr $3` at `0x00102E1C` loads its destination through a table based at **`0x004B7000`** (`lw $3,0($3)` at `0x00102E18`). The first six table words are `0x00102E24`, `0x00102E5C`, `0x00102DA4`, then `0x00102E24` repeated three times. This is a **three-distinct-target indirect switch**, whereas the existing generic `pointer_table_siblings()` deliberately requires at least four distinct executable entries before bulk expansion. That conservative generic threshold should be retained; recover small tables only when grounded by the actual guest indirect branch and its dataflow.

Historical local full-library work reached approximately **8,047 LLVM-verified translated modules**. These were scratch build artifacts, not source checked into `main`. The source-controlled pipeline is authoritative.

## 3. Stage 0 — Establish a reproducible, provenance-safe replay

**Objective:** reproduce the merged baseline without overwriting existing work.

1. Fetch `main`; inspect open PRs, changes since `d81a8e2`, and the working tree before creating a feature branch.
2. Locate the original `SLUS_214.42`, US ISO, and genuine OpenPS2BIOS 0.6 ROM0. Check their provenance; do not substitute another region or infer region from a reused ELF filename.
3. Verify the ELF against the known US SHA-256: `1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`. Previously verified OpenPS2BIOS ROM0 SHA-256: `3702baa508ff40498fab4a95429a2ae1f903d11f5bbcdd4e1a87dbe9f5d21a84`.
4. Check build prerequisites (CMake, modern C++, Python, LLVM/llvmlite where used). Read current `--help` rather than assuming an old invocation remains valid.
5. Reuse the existing guest IR/object cache **only when its provenance and runtime fingerprints validate**. After runtime changes, relink/refresh correctly; never disable fingerprint checks to force an old shared library to load.
6. Preserve the ELF hash, ROM hash, ISO file/hash or verified identity, runner flags, commit SHA, stop reason, and at least the last 64 dispatches in structured JSON and text logs.

The runner reads **`SDBZ_IOP_ROM0` from the environment**; `tools/run_game.py` does not currently expose `--iop-rom0`. The direct `ee_game_runner` does accept `--iop-rom0=FILE`.

Example, with paths adapted to the current environment:

```bash
export SDBZ_IOP_ROM0=/path/to/verified/openps2bios-v0.6-rom0.bin
python tools/run_game.py /path/to/SLUS_214.42 \
    --build-dir=build \
    --work-dir=build/sdbz-us-native-boot \
    --functions=all \
    --hle-startup --kseg-ram \
    --disc-image=/path/to/Super_Dragon_Ball_Z_USA.iso \
    --emit-jobs=8 --emit-opt=0 \
    --max-dispatches=100000 --loop-budget=1000000 \
    --sif-poll-limit=2048 --timeout-seconds=120 \
    --auto-expand=128 --auto-expand-table=24 \
    --report=build/sdbz-us-native-boot/first-boot.json
```

If no authentic raw ISO is available, explicitly mark real CDVD/boot acceptance **blocked**. A loose folder or synthetic fixture can test infrastructure but cannot prove the original ISO sector response.

**Acceptance:** the runner demonstrably uses genuine ROM0, original US sector data, and the latest native code. Identify and reproduce either the known `0x00102E24` stop or a properly explained newer stop; do not attribute a different run's count to this build.

## 4. Stage 1 — Close the next native entry gap: `0x00102E24`

**Objective:** pass the observed untranslated PC via actual native translation, rather than changing guest state.

1. Read `tools/run_game.py` and its `append_function()`, `covered_pcs()`, `pointer_table_siblings()` and runtime-stop parsing. Inspect `tools/link_native_objects.py`'s 256-function chunking and runtime cache validation.
2. Confirm the known **six-entry indirect jump table at `0x004B7000`** and its `jr $3` consumer at `0x00102E1C`. Verify the three distinct targets (`0x00102E24`, `0x00102E5C`, `0x00102DA4`) and the provenance of the table address from the real MIPS `lui`/`addiu`/`sll`/`addu`/`lw` dataflow. Distinguish a switch-case entry from a conventional function prologue.
3. Let `--auto-expand` add the genuinely reached target, respecting existing CFG ownership. If repeated runs reveal this three-target switch is a systematic discovery miss, add **branch-anchored small jump-table recovery** with a deterministic synthetic ELF regression. Do not lower the generic `pointer_table_siblings()` threshold from four distinct addresses globally or sweep arbitrary pointer-looking data. Preserve the original MIPS branch/delay-slot semantics.
4. When shared CFG blocks overlap, keep the original owner and publish only new guest entry PCs; preserve the full separately lifted module's correct control flow.
5. Run `verify_ir.py` incrementally, relink with verified chunk caches, and replay. If discovery remains systematically incomplete, improve the discovery algorithm and add a small test fixture proving the omission is fixed.
6. Capture the **first new genuine blocker** and compare against the last boot trace. Do not call a higher count progress if it only consists of a poll loop.

**Acceptance:** a correctly translated/owned `0x00102E24` executes, guest control flow continues, targeted IR/dispatcher tests pass, and the real SDBZ replay advances to a new explained stop (or reaches a genuine runnable state). Keep its exact inputs, PC, stop reason and evidence.

## 5. Stage 2 — Classify the next guest dependency, then implement only what is required

Use this triage order after each replay:

1. **Untranslated EE PC:** fix coverage or ownership in the native recompiler/dispatcher, with a small ELF regression.
2. **Loop or dispatch budget:** inspect the original MIPS instructions, loaded pointers and MMIO register state; identify the actual producer/event. Never clear bits or increase budgets as a substitute for a missing transfer.
3. **Unsupported device/MMIO:** decode register width, transfer direction, source/destination extent, status/IRQ semantics, and the expected consumer. Add the smallest host-backed implementation.
4. **SIF/IOP wait:** confirm the IOP producer is executing, inspect DMA9/DMA10/FIFO, EE DMA5/6 and SMFLAG, and preserve actual interrupt-safe callback scheduling.
5. **Graphics:** inspect the GIF packet and GS state before implementing new GIF chain tags, GS registers, primitive rendering, or presentation.
6. **VU/VIF, input or audio:** implement only after guest execution reaches the corresponding real dependency.

**Acceptance:** a regression reproduces the blocker; the fix preserves state on rejection, never invents an IRQ or successful result, and the same real-game input advances beyond that exact condition.

### Stage 2 blocker at 0x00108240 — verified VU0 matrix macro sequence

The real US executable enters **16 consecutive VU0/COP2 macro instructions** at `0x00108240..0x0010827C`, preceded by eight `LQC2` vector loads at `0x00108220..0x0010823C` and followed by four `SQC2` stores. This is a four-column matrix multiply, not a generic unsupported transfer:

| Instruction | Decode table | Occurrences | Native execution |
|---|---|---:|---|
| `VMULAx` | COP2 SPECIAL2 0x18 | 4 | Unsupported; initializes ACC |
| `VMADDAy` | COP2 SPECIAL2 0x09 | 4 | Unsupported; accumulates into ACC |
| `VMADDAz` | COP2 SPECIAL2 0x0A | 4 | Unsupported; accumulates into ACC |
| `VMADDw` | COP2 SPECIAL1 0x0B | 4 | Unsupported; writes VF destination |

The indexed input VF registers are **8–11**, matrix source rows are **VF4–VF7**, and each output column overwrites the matching input VF register. The mask is XYZW. PR #130 pins the actual 16 guest opcode words to the decoder and ensures the LLVM lifter still reports `UNSUPPORTED`. This is *diagnosis*, not VU0 execution or higher boot progress.

**Required next implementation (do not bypass):** model the FMAC/ACC pipeline for these four instructions, including PS2/VU float multiplication/addition semantics, the full four-lane destination mask, rounding/clamp/denormal behavior, MAC and status flags, operand forwarding and timing interactions with `LQC2` and `SQC2`. Prove the guest word semantics and state before/after each instruction with a controlled differential against original VU0 behavior or a trustworthy independent interpreter. Only then mark those exact four instructions as supported and replay the original US ISO/ROM0. The restricted existing VU0 scalar micro-engine is insufficient evidence to mark macro operations supported.

## 6. Stage 3 — Incremental GIF, GS and visual presentation

Already implemented: normal GIF PATH3, GS register/VRAM sink, and source-chain END tag with inline payload. **Do not rewrite these.**

- If guest transfer evidence requires other source-chain tags (CNT/NEXT/REF/REFS/CALL/RET), implement their actual TADR/MADR/QWC/TIE semantics and appropriate guest-RAM validation. A detected tag is not by itself evidence that all chain modes are required.
- Continue decoding authentic GIF PACKED/REGLIST/IMAGE packets; apply only supported GS state/VRAM writes atomically. Invalid GS events must leave DMA pending and state uncommitted.
- When the game actually submits drawable primitives, implement the smallest native triangle/sprite/line path that consumes original GS state. Preserve scissor, texture/CLUT, blending and z-test behavior to the extent the observed packets demand it.
- Add a framebuffer-present path and frame/vblank scheduling once a real dependency or consistent scene output requires it. **A successful GIF tag count is not a rendered frame.**
- Prefer deterministic capture tests that compare GS registers, VRAM bytes, and eventual produced pixels against ground truth where available.

**Acceptance for the first visual milestone:** an original-game-derived primitive is rasterized from real GS packets into a framebuffer and can be inspected; no substitute textures or fabricated frame completion. A later milestone can require a sustained main-menu render.

## 7. Stage 4 — Menu, input, audio and gameplay

Progress through these only after boot and graphics have credible execution paths:

- Controller: map native gamepad state to the game's actual input polling/configuration flow; verify menu movement, selection and back.
- Audio: feed real game audio requests into a native backend, preserving guest timing and loop behavior as supported.
- Main menu: run the actual game state machine and show reproducible navigation and animation, not an authored imitation.
- Gameplay: load stages/characters, maintain timing and physics, render frames and accept player input; add continuous-session stability tests.

**Acceptance:** the original executable reaches and responds to at least one authentic main-menu interaction; later a playable match, documented per build.

## 8. Tests, CI and PR gates

For each milestone, require both **focused regressions** and (when the original media/toolchain are available) **a real SDBZ US replay**. Existing useful tests include:

- `tests/tools/test_game_runner.py` — linked native execution and auto-expansion.
- `tests/tools/test_function_pointer_tables.py` — guest-anchored callback table discovery.
- `tests/tools/test_chunked_dispatcher.py` — stable 256-function dispatch-cache ownership.
- `tests/smoke/ee_gs_state_test.cpp` and `tests/smoke/ee_gif_path3_test.cpp` — actual GIF/GS acceptance and rejection.
- Existing IOP/CDVD/SIF/DMA and EE/LLVM tests under `tests/smoke/` and `tests/tools/`.

Every runtime change must test the success path, malformed/unsupported input, bounds, reset/retry (when relevant), exact register changes, real data movement, and **non-completion on refusal**. Preserve compatibility across Windows and Linux.

GitHub gates before **squash merge**:

1. `Build and test`
2. `SIF DMA acceptance`
3. `OpenPS2BIOS ROM0 execution`

Create feature branches from up-to-date `main`, commit incremental source/tests, push a PR, monitor and repair all three checks, record the real-game acceptance evidence, then squash-merge only when the milestone is validated. Passing synthetic CI alone cannot prove original-game bootability.

## 9. Checkpoint record (update after each significant replay)

| Field | Verified checkpoint (2026-11-17) |
|---|---|
| Base commit | `c7ca053af2e022bd5709ee6583c7c5427ef75016` (squash PR #135) |
| Latest merged change | PR #135 — exact native EE host yield at fully committed loop headers; runner opt-in candidate follows |
| Original game | Super Dragon Ball Z (US), `SLUS_214.42` |
| IOP firmware | genuine OpenPS2BIOS v0.6 |
| Verified real-CDVD transfer | LBA 16, 261, 289 × 135, 450 × 4 |
| SIF/WaitSema | real DMA5 callback + `iSignalSema(8)` verified |
| GIF/GS | PATH3 normal and nine-QWC END chain accepted; VRAM transfer observed |
| Last focused replay | **12,232 EE dispatches** using authentic USA ELF/ISO and OpenPS2BIOS 0.6; loop budget, not boot |
| Last guest PC | **`0x00175210`** (guest VBlank wait loop), after genuine SetVSyncFlag registration |
| Stop class | **LOOP_BUDGET(5)** while polling EE INTC_STAT VBlank-start bit and registered guest VSync flag; **not boot complete** |
| Next action | Validate optional host-runner loop resume cap separately from accurate EE cycles; then implement differential-verified R5900 timing and GS scheduler integration |

### 2026-11-17 — Stage 0/1 authentic replay evidence (PR #129)

- **Source:** original `SLUS_214.42` SHA-256 `1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`; raw USA ISO **4,259,610,624 bytes**, SHA-256 `d432b3ccaafe6ddf50ed3dedcf683dbac9bf06556d4e843d12fdee1a821231e1`; OpenPS2BIOS **v0.6 ROM0** SHA-256 `3702baa508ff40498fab4a95429a2ae1f903d11f5bbcdd4e1a87dbe9f5d21a84`.
- **Builder:** `ee-recomp-linux` binaries and exact checked-out source from passing PR #129 Build and test workflow; `--functions=all --hle-startup --kseg-ram --disc-image=<original US ISO> --max-dispatches=100000 --loop-budget=1000000 --sif-poll-limit=2048 --timeout-seconds=180 --auto-expand=128 --auto-expand-table=24 --emit-opt=0 --emit-jobs=6` with `SDBZ_IOP_ROM0` pointing to the verified ROM.
- **Compilation:** 7,901 initial LLVM-verified native modules, zero invalid modules. Restarted `--auto-expand=128` using the verified, provenance-checked `--no-recompile` cache after the first expansion limit was reached. The final replay linked **8,046** verified modules and dispatched **12,190** guest entries. `0x00102E24` was actually executed and its small switch at `0x004B7000` supplied **two** additional targets; overlapping CFG block ownership remained with existing native modules.
- **First new genuine dependency:** `0x00108240`, report `exit_reason=4`, `exit_name=UNSUPPORTED`, `dispatch_status=0`, not a DMA/IRQ stall. Original MIPS word is `0x4BE821BC` (COP2/VU0 macro). The game has eight preceding genuine `LQC2` loads and continues with additional COP2 operations. Implement macro execution only when exact ACC/flag semantics can be justified.
- **CI (PR #129, pre-document-update head `7f331070`):** Build and test, SIF DMA acceptance, and OpenPS2BIOS ROM0 execution all passed. Rerun after any further head update before merge. The OpenPS2BIOS CI authentic ROM0 run is a separate IOP test; the focused full-game replay is the evidence above.

### 2026-11-17 — Stage 2 VU0 authentic replay (PR #131 candidate)

- **Provenance:** original USA ELF SHA-256 `1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`, original USA 4,259,610,624-byte raw ISO SHA-256 `d432b3ccaafe6ddf50ed3dedcf683dbac9bf06556d4e843d12fdee1a821231e1`, genuine OpenPS2BIOS v0.6 ROM0 SHA-256 `3702baa508ff40498fab4a95429a2ae1f903d11f5bbcdd4e1a87dbe9f5d21a84`.
- **Tools:** native EE recompiler and runner from PR #131 Build and test artifact; fresh regeneration of the original 7,901 native modules plus 145 guest-anchored prior expansion entries. All **8,046 LLVM modules verified; zero invalid**. No checksum or cached-library validation bypass.
- **Observed continuation:** original game executes through the four `VMULAx → VMADDAy → VMADDAz → VMADDw` matrix chains at `0x00108240..0x0010827C`; full replay reaches **12,226 EE dispatches** and stops at **EE SYSCALL** (`EXCEPTION(1)`), exception-vector PC `0x80000180`, **EPC `0x00175014`**, **Cause `0x20`**, guest service `v1=0x73` (`SetVSyncFlag`). Inputs `a0=0x01ffeeb0`, `a1=0x01ffeeb8`, both in RAM. This is a new dependency and does not mean VSync, native presentation or gameplay works.
- **Next strict dependency:** `SetVSyncFlag` registers guest pointers which the real VSync event handler uses when an actual VSYNC arrives. Do not set either flag, CSR, an interrupt or a frame during registration. Preserve guest memory bounds and alignment validation, and do not substitute fake periodic events to pass polling.
- **CI (PR #131 head `5d97b2b5` before plan update):** all three required checks passed — Build and test (89 CTest cases including `vu0_macro_fmac`), SIF DMA acceptance, OpenPS2BIOS ROM0 execution. Recheck all three after this plan update before squash merge. This replay establishes finite startup-path execution but does not replace a full VU FMAC pipeline/hardware differential.

### 2026-11-17 — Authentic SetVSyncFlag and VBlank-poll replay (PR #132)

- **Input provenance unchanged:** original US ELF SHA-256 `1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`; original USA raw ISO 4,259,610,624 bytes SHA-256 `d432b3ccaafe6ddf50ed3dedcf683dbac9bf06556d4e843d12fdee1a821231e1`; genuine OpenPS2BIOS v0.6 ROM0 SHA-256 `3702baa508ff40498fab4a95429a2ae1f903d11f5bbcdd4e1a87dbe9f5d21a84`.
- **Runner:** CI-built PR #132 native recompiler and runner, validated `--relink-existing` of the 8,046 LLVM-verified modules from the previous fresh generation (no fingerprint bypass), with `--hle-startup --kseg-ram --disc-image=<original ISO> --max-dispatches=100000 --loop-budget=1000000 --sif-poll-limit=2048 --auto-expand=128 --auto-expand-table=24 --trace`. Genuine ROM0 supplied via `SDBZ_IOP_ROM0`.
- **Observed kernel behavior:** at 12,226 dispatches, guest syscall `0x73` registers `a0=0x01ffeeb0` (4-byte VSync flag), `a1=0x01ffeeb8` (8-byte GS_CSR destination); HLE returns with V0=0 without changing either location or issuing VBlank/IRQ. The flag/optional CSR pointer registration is one-shot: the future real VBlank callback writes its data and clears both pointers; a null flag disables that callback. Guest then calls `SetSyscall(0x12c,0x80076488)` which the existing out-of-range policy ignores, separately from this blocker.
- **New verified stop:** **12,232 EE dispatches**, `LOOP_BUDGET(5)`, guest PC **`0x00175210`**. Original code at `0x001751EC` writes `0x4` to EE INTC_STAT `0x1000f000` to acknowledge VBlank-start, then at `0x00175210` reads `INTC_STAT & 0x4`. If that is not asserted, it checks the guest registered flag at `0x01ffeeb0`. Neither is set because **no genuine VBlank event producer** exists in this native runner. Increasing dispatch or loop budgets cannot solve the hardware dependency.
- **Next acceptance milestone:** real scanout/VBlank producer driven by the guest CRT configuration and emulated EE timing, correct EE INTC_STAT start interrupt and enable/mask logic, actual GS CSR on VBlank, and guest flag write only from that event. First add focused timing/edge/acknowledge/reject tests against reliable GS behavior. Do not create an unconditional periodic host callback, forged interrupt or write-at-registration shortcut.
- **CI for PR #132 pre-document head `4aa9ccf2`:** Build and test, SIF DMA acceptance, and OpenPS2BIOS ROM0 execution all succeeded. Recheck the final head after this document change before squash merge. This replay is not a running main menu or a rendered frame.

### 2026-11-17 — Stage 2 scanout timing foundation (PR #133)

- **Why:** The original US game continues through genuine `SetVSyncFlag(0x01ffeeb0,0x01ffeeb8)` and then polls `EE INTC_STAT & 0x4` at `0x00175210`; 12,232 dispatches end in `LOOP_BUDGET`. This is a real missing VBlank producer, not a missing translated entry.
- **Read-only reference:** `SetGsCrt(1,2,0)` from the original startup trace selects interlaced NTSC. PS2 EE runs at **294,912,000 Hz**; NTSC mode 2 uses **60,000/1001 fields per second**, PAL mode 3 uses **50 fields per second**. A field boundary is therefore **4,920,115.2** EE cycles for NTSC and **5,898,240** for PAL. Use rational phase accumulation to prevent drift.
- **Implemented separately:** `c_ee_scanout_clock` accepts **explicit authoritative EE cycle timestamps**, rejects unconfigured/malformed/non-monotonic timing, and emits a scheduled field-start only at the proper boundary. It latches GS `CSR.VSINT`, toggles `CSR.FIELD` for interlaced output, raises INTC cause 2 (with the existing mask logic), and invokes the prior one-shot guest flag/CSR callback. Invalid memory refuses delivery without partially updating GS/INTC state. New `ee_scanout_clock` CTest exercises timing, masks, edge acknowledgements, callback lifetime, and refusal.
- **IMPORTANT: not enabled in the native game runner.** The current EE dispatcher has no trustworthy instruction/cycle clock and no interruptible mid-function guest-loop safepoint. Driving this model with host wall time, dispatch count, or `m_loop_safepoint_remaining` would fabricate VBlank. Do **not** claim new boot progress; the last authentic replay remains **12,232 dispatches, LOOP_BUDGET at 0x00175210**.
- **Next required work:** introduce instrumented accurate guest EE cycle accounting across translated basic blocks, branch-likely annulled slots, delay slots and long loops; add a bounded, resumable timed-event exit; keep scheduler state outside guest CPU context; connect the scanout clock only after differential/reference tests demonstrate the clock is credible. Validate hardware INTC/GS CSR and field semantics with authentic guest tracing before calling native rendering ready.

### 2026-11-17 — Native guest-instruction accounting (PR #134)

- **Architecture:** appended `s_ee_runtime::m_guest_instruction_attempts` diagnostic counter (host-side, separate from `s_ee_cpu_state`). Generated LLVM increments only for executed guest instructions, including reached delay slots, excluding annulled branch-likely delay slots. The internal LLVM helper allows null runtime pointers in existing JIT fixtures. No scanout trigger, IRQ, synthetic VBlank, CPU timing approximation or guest registers change as a result.
- **Focused tests:** original JR, BEQ, JAL, BLTZALL, BGEZALL instruction/delay-slot JIT cases; reported count in `ee_game_runner` JSON and text output; native integration assertion of guest-loop attempts vs dispatches. Existing VU0 JIT fixture needed its runtime allocation expanded to the actual LLVM-exposed appended counter offset, rather than the old six-pointer ABI prefix; this was fixed after first CI exposed a segmentation fault.
- **Authentic replay provenance:** US `SLUS_214.42` SHA-256 `1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`; original US 4,259,610,624-byte ISO SHA-256 `d432b3ccaafe6ddf50ed3dedcf683dbac9bf06556d4e843d12fdee1a821231e1`; genuine OpenPS2BIOS v0.6 SHA-256 `3702baa508ff40498fab4a95429a2ae1f903d11f5bbcdd4e1a87dbe9f5d21a84`. Fresh 7,901 function modules translated and all passed LLVM verification, then original-replay-derived **145 additional** entries were retranslated from the US ELF (no old LLVM or object copying). **8,046/8,046 modules verified**, native library re-linked, new provenance cache produced and verified with `validate_translation_cache`.
- **Native replay result (after instrumentation):** exactly **12,232 dispatches**, **9,009,311 guest instruction attempts** (not EE cycles); final PC `0x00175210`, reason `LOOP_BUDGET(5)`, guest `INTC_STAT & 4` VBlank wait remains unresolved. Real CDVD DMA3, SIF/IOP firmware, GS PATH3, VU0 FMAC and guest SetVSyncFlag registration follow the same tested paths; no guest framebuffer or VBlank event is invented.
- **Remaining hardware blocker:** implement authoritative R5900 cycle accumulation and safe guest-loop preemption; validate timing against reliable reference, then advance real `c_ee_scanout_clock` only with those EE cycles. This accounting PR is diagnostic evidence, **not** a native-boot progress claim. Rerun three GitHub Actions workflows after the final documentation changes before squash merging.

### 2026-11-17 — Host-cooperative native EE loop safepoints (PR #135, squash-merged)

- **Why:** The original SDBZ VBlank wait at `0x00175210` is an *intra-function* polling loop. A genuine timed GS event cannot be serviced if the native guest never yields to the host scheduler. The existing `LOOP_BUDGET` exit is a terminal diagnostic, not a resumable scheduling boundary.
- **Implemented (opt-in only):** a separate `EE_EXIT_HOST_YIELD(6)` reason and ABI-appended `m_loop_safepoint_yield_enabled` host flag. On exact budget exhaustion at an already-instrumented backward-loop header, generated LLVM chooses between unchanged terminal `LOOP_BUDGET(5)` (default) and `HOST_YIELD(6)`. Both set `m_pc` to the guarded block start *after* prior branch delay slots and architectural GPR writes have committed. They do not change guest cause/status or raise interrupts.
- **Regression:** actual MIPS `BEQ zero,zero,-1` with `ADDIU t0,t0,1` delay slot is emitted to real LLVM. Test provides two bounded host yields and re-enters the same guest PC; it compares cumulative GPR128 state and instruction-attempt count to the expected uninterrupted execution. A disabled yield flag must still return `LOOP_BUDGET` with identical guest state.
- **Native runner connection in follow-up candidate:** the original 12,232-dispatch VBlank loop remains a true hardware dependency. Only an explicit bounded diagnostic CLI opt-in may resume a set number of host yields without generating any EE time or GS/INTC event. The host must first gain accurate, independently verified R5900 EE cycles (including pipeline stalls, dual-issue constraints and delay-slot timing). Only then can a scheduler use cooperative yields to drive `c_ee_scanout_clock` and deliver genuine GS VBlank events. Yield count and instruction-attempt count are **not** substitutes for cycles.

### 2026-11-17 — Opt-in bounded host runner resume at native loop safepoints (follow-up to #135)

- **Behavior:** a new `--loop-yield-resumes=N` runner option enables `s_ee_runtime::m_loop_safepoint_yield_enabled` **only when `--loop-budget` is nonzero**. Default remains the unchanged `LOOP_BUDGET(5)` watchdog. Host runner accepts no more than N `HOST_YIELD(6)` exits, resets the already-existing loop guard before each safe reentry, and then fails closed, preserving the exact guest resume PC. The count is explicitly host resumption, not an EE timing estimate.
- **Synthetic acceptance:** real translated MIPS `ADDIU t0,t0,1; BEQ zero,zero,-2; NOP`, with loop budget 4 and resume cap 2, should exit `HOST_YIELD(6)` after **three** dispatched native entries, retaining `t0=12` and guest PC `0x00100008`. The synthetic test checks a preserved COP0 Cause (no artificial interrupt) and rejects `--loop-yield-resumes` without a nonzero loop budget. Existing standard watchdog and one-shot host-yield JIT tests must remain green.
- **Hardware:** this change **does not** call `c_ee_scanout_clock::advance_to`, infer cycles from instruction attempts, acknowledge INTC_STAT, set `CSR.VSINT`, trigger VBlank or create a frame. All original guest DMA, IOP and BIOS behavior stays in the existing runner. This is a scheduler continuation facility, **not native boot completion**.
- **Real-game acceptance (PR #137 candidate):** CI-built Linux native EE runner from Build and test artifact `14368143902` used the unchanged, provenance-verified **8,046 native translated modules** from PR #135 against original US `SLUS_214.42`, US 4,259,610,624-byte ISO, and genuine OpenPS2BIOS v0.6 (same SHA-256 provenance above). With `--max-dispatches=100000 --loop-budget=1000000 --sif-poll-limit=2048 --hle-startup --kseg-ram` and **no** yield option, replay remains **12,232 dispatches**, **9,009,311 instruction attempts**, `LOOP_BUDGET(5)` at `0x00175210`. With **`--loop-yield-resumes=2`** added, the native runner performed precisely two host resumes at the same guest PC and stopped after **12,234 dispatches**, **23,009,311 instruction attempts**, `HOST_YIELD(6)`, final PC still `0x00175210`. Original CDVD DMA3 LBA transfers, guest SIF/IOP, GIF PATH3 and SetVSyncFlag registrations remained intact; no GS VBlank, INTC completion, frame presentation or boot progress occurred. Reports: `/mnt/data/sdbz-yield-runner/first-boot-default.json` and `first-boot-yield.json` (local working artifacts). These reports are local evidence, not checked-in copyrighted game data.
- **CI pre-document revision:** Build and test, SIF DMA acceptance and OpenPS2BIOS ROM0 execution all passed for head `8179eb76`. Re-run all three on final documentation head before squash-merging.
- **Remaining:** prove native EE instruction issue/stall timing and timed interrupt delivery against real PS2 behavior, then feed only authoritative EE cycles to the cycle-driven scanout model. Original-game replay with no yield option must remain **12,232 dispatches at `0x00175210`**, still waiting for VBlank.

### 2026-11-17 — Windows all-native GIF/DMA-readiness coverage replay (PR #139)

- **Source and runtime:** the user ran the original USA `SLUS_214.42` through Windows `ee_game_runner.exe` from `feature/sdbz-all-native-coverage-probe`, rebased upon their active `feature/sdbz-gif-dma-readiness` branch, with the USA 2,079,888-sector ISO and an IOP ROM0 reset. This was a user-local run, not a GitHub Actions job or a claimed framebuffer boot.
- **Initial gap:** the first all-native `--functions=all` DLL had **7,901** discovered entry modules, but its dispatch ownership map did not include every reached indirect entry. `probe_sdbz_all_native.bat` uses provenance-checked `--no-recompile --auto-expand=128 --auto-expand-table=24` instead of concluding that `DISPATCH(3) status=0` is a GIF/DMA hang.
- **Actual expansion:** **two bounded coverage passes**, compiling **145** additional ELF-anchored native modules over **50** relink cycles. At the final link, all **8,046/8,046 LLVM 22 modules verified**, with zero invalid. The final run completed **12,232 native EE dispatches**, **9,009,311 diagnostic guest instruction attempts** and stopped at **`0x00175210`, `LOOP_BUDGET(5)`**. This is the exact prior genuine VBlank-wait checkpoint, not a new game-boot milestone.
- **Hardware evidence retained:** CDVD DMA3 completed LBA **16, 261, 289×135 and 450×4** transfers; host GS PATH3 consumer accepted **33 tags** and **65,536 transferred pixels**. Neither implies native rasterization. The outstanding game-guest event is still hardware INTC_STAT VBlank-start cause 2 / bit `0x4` (and its registered VSync flag) at the exact guest wait loop. Do not replace it with an unconditional periodic interrupt.
- **Probe performance:** the optional `SDBZ_REPORT_SNAPSHOT_MS=2000` used by the Windows run reduced measured JSON report time to **0.5 ms** in the final **12,232-dispatch** replay (`native_ms=427.0`, `elapsed_ms=966.7`). This host-only timer controls snapshot disk writes, not guest EE/IOP/GS time. The 50 native DLL links consumed **~91.8 seconds total** from log-reported link durations, so relink batching is the next build-performance target. The final guest report remains fully written. Earlier incompatible cache metadata (when `ee_functions.json` had changed without a matching cache record) was correctly rejected rather than silently relinked; the subsequent full Windows replay completed successfully.
- **Validation limits:** local synthetic-ELF regression compares final default/timed-snapshot reports and rejects malformed interval settings, but PR #139's three main-branch GitHub Actions gates have **not run** on the feature-to-feature PR. Do not claim CI green or squash merge before validation.
- **Next hardware milestone:** trustworthy reference-validated R5900 instruction issue/stall cycle accounting; only after that should a host scheduler advance the existing `c_ee_scanout_clock` at genuine cycle timestamps and service real GS/EE VBlank callbacks. No invented VBlank, guest cycles, INTC bits or artificial interrupt injection.

### 2026-11-17 — Stage 3: bounded native guest instruction order trace (stacked PR)

- **Purpose:** an R5900 issue/stall cycle model cannot be inferred from the prior 9,009,311 guest instruction **attempts**, CPU native dispatcher count, elapsed host milliseconds, or loop-guard visits. First recover exactly which native MIPS instruction words were executed and in what order (including branch delay slots, excluding annulled branch-likely slots), then compare with an independent timing reference before declaring any `m_ee_cycle` estimate authoritative.
- **Implementation:** extend the existing per-guest-instruction LLVM counter helper to optionally record `(attempt ordinal, guest PC, original opcode word)` in a **65,536-record, host-owned bounded buffer**. The C++ `s_ee_runtime` fields are **ABI-appended**; null trace pointer disables writes. Optional `--trace-instructions=FILE` writes CSV after the guest stops, with an explicit `# truncated=true|false` marker. An inclusive `--trace-instructions-window=START:END` PC interval limits capture to the code under investigation. Nothing writes EE time, CSR/INTC, IOP events or guest memory, and no trace ordinal is a cycle timestamp.
- **PowerShell trace command (after rebuilding the guest DLL with this new emitter):**
  ```powershell
  $env:SDBZ_REPORT_SNAPSHOT_MS = "2000"
  python tools/run_game.py PS2_GAME/SLUS_214.42 --work-dir=build/sdbz-all-native --build-dir=build --config=Release --no-recompile --max-dispatches=100000 --loop-budget=1000000 --sif-poll-limit=2048 --timeout-seconds=120 --hle-startup --kseg-ram --disc-image="D:\\Games\\PS2_ISO\\Super Dragon Ball Z (USA).iso" --report=build/sdbz-all-native/trace-boot.json --trace-instructions=build/sdbz-all-native/vblank-pcs.csv --trace-instructions-window=0x001751ec:0x00175228
  ```
  The existing `build_sdbz_all_native.bat` must regenerate/relink native guest code once after changes to the LLVM emitter or runtime layout; `--no-recompile` cannot safely use the previous hashed IR cache. The replay remains limited to the preexisting `LOOP_BUDGET` guest hardware wait. CSV contains *guest instruction order*, not a reconstructed PS2 cycle trace.
- **Local synthetic original-ELF-independent validation:** Linux C++17 sources compiled; real synthetic MIPS ELF → new LLVM IR → LLVM 20 object → host DLL → native runner succeeded. `JR $ra` branch delay instruction recorded; not-taken `BEQL` annulled slot at `0x00100010` absent while its five executed words were ordered correctly; a 40,000-iteration two-instruction backward loop reached 80,000 diagnostic instruction attempts while its buffer reported precisely 65,536 entries and `truncated=true`. Baseline and instrumented branch-like native runs had identical GPRs, final PC, stop reason, dispatches and attempt count. The full original US-game run and GitHub Actions have **not yet** been executed on this new trace branch.
- **Next acceptance:** obtain independent R5900 instruction issue/stall timing for representative integer/branch-likely, loads/latencies, COP1/2, VU0 transfers, syscall and wait-loop sequences; differential compare *both* instruction order and measured cycle boundaries. Until timing is proven, `c_ee_scanout_clock` remains disconnected from the native runner. Avoid introducing a model calibrated solely to make a single VBlank wait finish.

### 2026-11-17 23:32 UTC — native instruction-order branch replay acceptance

- **User-local original-game Windows replay:** following the trace-emitter change, the all-native builder re-emitted **7,901** discovered functions and the bounded coverage probe reached **8,046/8,046 LLVM 22 verified modules** (two passes, **145** newly translated modules, **50** DLL relinks). The submitted `translation-cache.json` lists 8,046 IR hashes, `emit_opt=0`, the authentic US ELF SHA-256 `1b213a85…`, and linked native DLL/metadata integrity hashes. It is a provenance record, not a substitute for local file re-hashing.
- **Identical guest stop:** exactly **12,232 EE dispatches / 9,009,311 instruction attempts**, `LOOP_BUDGET(5)` at **`0x00175210`**. GS PATH3 still accepted 33 tags / 65,536 pixels; no native frame, scanout field or VBlank event was produced. This is a *compatibility regression pass*, not a new clock result.
- **Trace capture was not activated in these logs:** `probe_sdbz_all_native.bat` did not forward `--trace-instructions`. The absence of `EE instruction trace:` and a CSV does not mean the guest trace is empty or that timing has been validated.
- **New one-command follow-up on this branch:** `trace_sdbz_vblank.bat` checks the original USA ISO, OpenPS2BIOS ROM0 and linked translation artifacts; calls `run_game.py --no-recompile` once; records `build/sdbz-all-native/vblank-pcs.csv` with guest PC range **`0x001751ec:0x00175228`** and a separate `vblank-trace-report.json`. Source/digest mismatch fails closed; the helper does not regenerate 8,046 objects or discard coverage. A guest `LOOP_BUDGET` remains a nonzero diagnostic return, not a success claim.
- **Next data required:** the CSV sequence and whether `# truncated=true`; compare to a reference R5900 instruction/cycle trace before assigning EE cycles or connecting `c_ee_scanout_clock`.

### 2026-11-17 — Authentic US VBlank PC trace and reference validation gate

- **Original Windows inputs:** user uploaded `vblank-pcs.csv` and `vblank-trace-report.json` from the dedicated cached single-pass trace; guest remains at **12,232 native EE dispatches**, **9,009,311 guest instruction attempts**, `LOOP_BUDGET(5)`, `0x00175210`. This is a regression match, not progress through the hardware wait.
- **Precise captured prefix:** exactly **65,536** guest instruction records, with first/last attempt ordinals **2,009,297–2,074,838**. First **9** records show the guest INTC_STAT acknowledge at **`0x001751ec`** (`SW`), `SYNC`, conditional and a `JAL` with delay slot before constructing **`0x1000f000`**. Six guest instructions within the call lie **outside the trace window**; attempt ordinals correctly show that gap.
- **Seven-instruction polling body:** PCs **`0x00175210` LW**, **`0x00175214` ANDI 4**, **`0x00175218` BNE**, **`0x0017521c` NOP delay slot**, **`0x00175220` LW flag**, **`0x00175224` BEQ backward**, **`0x00175228` NOP delay slot**. The remaining **65,527 captured records** are exactly **9,361 complete contiguous seven-instruction loop iterations** without branch deviation. This proves the captured prefix, **not the entire truncated tail**.
- **Truncation versus terminal state:** footer is `# truncated=true range=0x001751ec:0x00175228`; final report has **6,934,473 additional guest instruction attempts** after the last captured record. From the first loop ordinal **2,009,312** through the final total there are **7,000,000 attempts**, **consistent with** the 1,000,000-iteration configured seven-instruction watchdog. Do not misrepresent the unrecorded tail as observed trace data or use attempts as EE cycles.
- **New validated tooling:** `tools/compare_ee_instruction_traces.py` strictly checks numeric monotonicity, alignment, filter boundaries, footer/truncation, terminal report, and an explicit PC sequence; optional independently captured `cycle,pc,opcode_word` reference must match the native instruction sequence and requires a named provenance. Its result explicitly marks `attempts_are_cycles=false` and `external_timing_independently_verified=false`. The reference cycle span is reported only when an independent source is supplied; the tool never invents cycles. `tests/tools/test_compare_ee_instruction_traces.py` covers valid captured loops, gaps, truncated tails, equal-cycle dual-issue timestamps, strict mismatches and bad records. Registered as `ee_instruction_trace_comparator` in CTest.
- **Single-command local analysis (PowerShell, no recompile):**

  ```powershell
  python tools/compare_ee_instruction_traces.py build/sdbz-all-native/vblank-pcs.csv --report build/sdbz-all-native/vblank-trace-report.json --loop-pcs 0x00175210,0x00175214,0x00175218,0x0017521c,0x00175220,0x00175224,0x00175228 --json build/sdbz-all-native/vblank-analysis.json
  ```

- **Timing acceptance remains open:** obtain independent, cycle-stamped R5900 reference records for the **same exact PC filter and start alignment**, with emulator/hardware build/configuration provenance; compare opcode ordering first, then study issue/stall latency and dual issue. No reference cycles were present in these uploaded artifacts. Do not yet feed any count into `c_ee_scanout_clock`, synthesize VBlank/INTC, or claim rendered output.

**Next developer instruction:** The authentic run now reaches the real VBlank polling loop at `0x00175210`. Implement cycle-driven GS VBlank scheduling only after verifying guest CRT timing, EE cycle accounting and INTC masks/callback delivery. The verified SetVSyncFlag registration alone does not create a VSync. Preserve all prior genuine CDVD/SIF/GIF and VMAC state; continue through a new trace-proven blocker.
### 2026-11-18 14:37 — original US HLE-first SIO2 advancement and VBlank dependency

The matched group `boot(20261118-143750).log`,
`expansion(20261118-143750).log`,
`first-boot(20261118-143750).json`,
`iop-disc-catalog(20261118-143752).json` and
`translation-cache(20261118-143749).json` share ELF SHA-256
`1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`.
The final expansion replay and trace agree: **10,491 EE dispatches**,
**9,001,372 actual guest instruction attempts**, GS PATH3 accepts 33
tags and 16,384 image QWC / 65,536 written pixels, and the final EE
reason is **LOOP_BUDGET(5)** at `0x00175210`. Authentic
`SIO2MAN.IRX` loads as module 1, thread 1; its initial IOP quantum
completes 256 real guest instructions without the former memory fault.
The static disc catalog's `runtime_load_count=0` is still not connected
to runtime loading.

The guest's `SetVSyncFlag(0x01ffeeb0,0x01ffeeb8)` succeeds with
no immediate VSync, then the original guest waits for VBlank start
(`INTC_STAT & 4`), matching the earlier verified `0x00175210`
dependency. No later IOP module or actual presented video frame is proven.
The `SetSyscall(0x12c)` diagnostic remains independently unsupported,
but it did not cause this stop.

The next GS/EE integration batch exposes privileged GS_CSR and GS_IMR
through the EE physical bus; accepts W1C acknowledgement without making
up a VBlank, routes SetGsCrt timing configuration into the existing
rational scanout schedule and includes field delivery, interrupt mask,
one-shot flag, register width, and reset tests. The native runner
adds a VBlank-wait snapshot showing actual INTC status, registered
flag and CSR state **without ever advancing the clock**. Guest
instruction attempts, native dispatch count, safepoint visits and host
wall time are *not* treated as EE cycles. This prepares a real
cycle-budget scheduler but **does not fix the VBlank wait** until
architecturally credible EE cycle accounting is implemented.

### 2026-11-18 15:10 — matched HLE-first VBlank wait; reference instruction-slot foundation

The five-file set `boot(20261118-151007).log`,
`expansion(20261118-151007).log`,
`first-boot(20261118-151007).json`,
`iop-disc-catalog(20261118-151008).json` and
`translation-cache(20261118-151007).json` share the USA retail
ELF SHA-256 `1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`.
The final stop is unchanged from 14:37: **10,491 dispatches**, **9,001,372
actual reached-instruction attempts**, `LOOP_BUDGET(5)` at
`0x00175210`. `SIO2MAN.IRX` links/starts correctly, executing a
bounded initial 256 R3000 instructions without a memory fault. GS
PATH3 still accepts 33 tags and 16,384 image QWC. No later module or
presented framebuffer is established.

New runtime diagnostics prove `INTC_STAT=0`, `INTC_MASK=0x804`,
guest flag `[0x01ffeeb0]=0`, CSR destination `[0x01ffeeb8]=0`,
`GS_CSR=0`, `CRT_configured=1`, `fields_started=0`, and
`cycle_source=unavailable`. The missing timed VBlank producer, not
IRX linkage or translation cache, remains the first blocker.
The 126 expansion-run attempts are not separate independent boots.

**Reference-only clock-model step:** `ps2-web-recomp` currently uses a
simplified interpreter budget: 1 slot for ordinary guest instructions,
2 slots for a branch including its reserved delay slot, and separately
configurable syscall overhead (default 300). The generated EE IR now
maintains an ABI-appended, host-only `m_reference_instruction_slots`
distinct from `m_guest_instruction_attempts`, including an extra
reserved slot when a branch-likely annuls a delay instruction. Runtime
MCJIT regressions cover JR, BEQ, JAL and both branch-likely outcomes,
plus resumed native guest loops. No reference slots are promoted to
hardware-accurate R5900 cycles; syscall cost, dual issue, pipeline
stalls, VU timing and event-edge scheduling require further validation.
The runner displays this *reference accounting only* for diagnostics,
and does not feed `c_ee_scanout_clock` or synthesize VBlank. This
phase supplies a measurable timing baseline, not a boot-unblocking
claim.

### 2026-11-18 15:44 — verified VBlank blocker and isolated issue-timing probe

The matched `boot(20261118-154418).log`, `expansion(20261118-154419).log`,
`first-boot(20261118-154418).json`, `iop-disc-catalog(20261118-154418).json`,
and `translation-cache(20261118-154419).json` retain the USA retail ELF SHA-256
`1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`.
The final expansion has **zero newly discovered functions** and agrees with the
boot report: 10,491 dispatches, 9,001,372 guest attempts, 9,001,546
reference slots, `LOOP_BUDGET(5)` at `0x00175210`. SIO2MAN.IRX links,
starts and executes a 256-instruction IOP slice. GS still accepts 33 tags,
16,384 image QWC and 65,536 pixels. VBlank has no authoritative EE
cycle source: INTC_STAT=0, GS_CSR=0, flag [0x01ffeeb0]=0, fields=0.

`ee_issue_timing_probe.h` adds an **isolated experimental** two-wide issue
scoreboard and explicit dependency/result-latency, serialization, branch-delay
reservation and configured HLE overhead hooks, plus CTest regression. It
models timing **candidates**, not validated R5900 issue rules. Latencies are
provided explicitly by the caller; no hardware latency table is presumed.
This probe is not connected to translated EE instruction execution or the
GS/INTC scheduler, and therefore deliberately **does not unblock VBlank**.
Next: obtain and validate instruction-class pairing/latency evidence, connect
reached guest instructions to a separately controlled timing pipeline, verify
boundary preemption against a reference before granting scanout cycle authority.

### 2026-11-18 16:18 — matched unchanged VBlank and start of GS readback

The matched five-file 16:18 batch has byte-identical boot-report JSON
and disc catalog against the prior 15:44 run. It again reaches 10,491
EE dispatches, 9,001,372 attempts, 9,001,546 reference slots and
`LOOP_BUDGET(5)` at `0x00175210`. No new native translations were
discovered. The library cache hash changed after source updates; guest
execution and ELF provenance remain unchanged. GS accepts 33 tags and
65,536 uploaded pixels but **zero vertices**, so there is no observed
rasterized geometry or completed frame.

A separate, side-effect-free `c_ee_gs_display::readback` now decodes
an explicitly supplied guest GS PMODE/SMODE2/DISPFB/DISPLAY register
snapshot using the reference display width, magnification, visible line
limits and swizzled PSMCT32/24/16/16S color pixels. A new regression
uploads pixel data via the actual GS GIF IMAGE consumer and verifies
readback/circuit selection and fail-closed unsupported formats. No
privileged GS display MMIO capture, renderer/presenter, real VBlank
producer or live frame output is claimed by this foundational step.
Next wire actual guest privileged GS display writes to the snapshot,
then add opt-in VRAM image export; primitive rendering remains separate.

The display-readback foundation now also captures six privileged GS
registers via 32/64-bit guest MMIO (PMODE, SMODE2, DISPFB1/2, DISPLAY1/2),
preserving high-half writes and returning exact values on reads. These
are kept separate from GIF register writes, GS_CSR and GS_IMR.
An opt-in `--gs-snapshot=FILE.ppm` option is available both in the
direct EE runner and `tools/run_game.py`; it writes a P6 image from
the **guest-selected display buffer only when configured and readable**
at the eventual stop. No synthetic frame, GS event, rasterizer,
exception or interrupt is ever generated. Failure to obtain a display
image is reported distinctly from guest boot status.

### 2026-11-18 16:58 — matched HLE trace and automatic GS diagnostics

The single 16:58 local session expanded the guest translation coverage by
128 functions in pass one and 5 functions in pass two (133 total new
functions), then replayed the same 10,491-dispatch boot to
`0x00175210`/`LOOP_BUDGET(5)`. 9,001,372 instruction attempts,
33 accepted GS GIF tags, zero vertices and 65,536 VRAM-transfer pixels
remain unchanged. VBlank is still blocked by missing cycle authority;
zero fields started and `INTC_STAT=0`.

The launcher now requests two *diagnostic-only* snapshots automatically:
`build/ps2-game-hle/gs-display.ppm` from a valid configured display
circuit and `build/ps2-game-hle/gs-upload.ppm` from the **fully completed**
current host-to-local GIF IMAGE rectangle. The two images are not
interchangeable: uploaded textures are not final scanout. Neither
snapshot affects guest execution, VBlank, DMA completion, or the stop.
The batch wrapper passes image options **only on its final traced
replay**, clearing previous PPM files beforehand; incomplete/unsupported
transfer or absent display state remains explicitly unavailable.
A regression covers actual GIF IMAGE upload preview, its rejection
before completion and snapshot orchestration.

### 2026-11-18 17:31 — real GS image inspection and zero-FBW display

The five matched files produced by the new launcher show the same
10,491 EE dispatches, 9,001,372 guest instruction attempts and
`0x00175210`/`LOOP_BUDGET` stop, with GS tags=33, vertices=0,
IMAGE QWC=16,384, and transfer pixels=65,536. The uploaded
`gs-upload.ppm` is a genuine 1024 × 64 P6 RGB export from GS VRAM,
entirely black (all 65,536 pixels). This is not a rendered frame.
The captured guest display regs are PMODE=`0x7F27`, SMODE2=1,
DISPFB1/2=`0xA0000070`, DISPLAY1=`0x001BF9FF0203327C` and
DISPLAY2=`0x001BF9FF0203227C`, CRT mode=NTSC.

Before this change `gs-display.ppm` was not generated because both
DISPFB registers decode FBW=0, which readback incorrectly rejected.
The ps2-web-recomp GS `page_address()` calculation instead uses
`max(1, FBW * 64 / page_width)`, i.e., one page per row for FBW=0.
Read-only PSMCT32/24/16/16S display access now matches that reference
without changing the guest register, rasterizing a primitive or
generating VBlank. The selected display circuit/FBP/FBW/PSM and DW/MAGH
are reported separately. The zero-FBW address normalization is tested
against actual nonzero swizzled VRAM pixels.

The known boot blocker remains the absence of a validated EE cycle
source for scanout fields and interrupt delivery. Do not claim the
resulting display PPM is a presented game frame.


### 2026-11-18 17:34 — raise bounded native ELF auto-expansion cap

Default `run_ps2_game.bat` maximum expansion passes is now **8** (previously
2), with up to **128 new guest functions per pass**, for a conditional
maximum of 1,024 new compiled functions. Environment variable
`EXPANSION_PASSES` can override the launcher count, within the existing
`run_boot_session.py` validated range 1–8. The existing per-pass
translation cap and early stop behavior remain unchanged. This is a
*maximum*, not eight unconditional executions: the uploaded 17:31 log
shows pass 1 added 128 new functions, pass 2 only 5, then the same
`0x00175210` VBlank loop. Without new runtime progress the wrapper
still stops after pass 2, preserving the translation cache. All GS PPM
diagnostics remain final-traced-replay only.
