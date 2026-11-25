# Native WebGPU GS renderer (SDBZ HLE)

The native WebGPU backend is extracted and adapted from
[twist84/ps2-web-recomp, SDBZ](https://github.com/twist84/ps2-web-recomp/tree/SDBZ)
and uses **wgpu-native**. There is no browser frontend, WebAssembly,
Emscripten, JavaScript, SDL, or window required.

## Implementation

- `src/runtime/ps2_webgpu/gs/gpu/gpu.cpp`: native WebGPU compute
  upload, copy, CLUT, raster, resolve, snapshot, decode, and GS display
  pipelines; native wgpu surface support remains available in the source.
- `src/runtime/ps2_webgpu/gs/gpu/gs.wgsl`: the reference GS compute
  shader, including VRAM swizzle, texture formats and primitive raster.
- `src/runtime/ps2_webgpu/gs/gpu/blit.wgsl`: the reference present/blit
  shader; no window is needed for diagnostic readback.
- `src/runtime/ps2_webgpu/gs/swizzle.cpp`: swizzle tables needed by the
  imported compute renderer.
- `src/runtime/ee_webgpu_bridge.cpp`: adapts the recompiler's **committed**
  GIF events to the reference GS register, primitive and image-transfer API.
  It has no path to fake DMA success, IRQs, VBlank, or guest execution.
- `src/tools/ee_game_runner_main.cpp`: optional native, offscreen
  GPU-processed GS VRAM snapshot. It does not claim that the guest presented
  a frame.
- `tests/smoke/ee_webgpu_native_smoke.cpp`: real GPU upload, raster and
  readback test without game files or a window.
- `.github/workflows/native-webgpu.yml`: native Linux and Windows
  build verification and Linux software-Vulkan smoke testing.

In the default CPU-authoritative mode, the existing CPU GS state and 4-MiB VRAM remain authoritative. The GPU bridge receives committed GS events. The optional GPU-authoritative mode described below instead materializes VRAM on the GPU, with checked readbacks; it cannot silently fall back to stale CPU VRAM.

### Experimental GPU-backed guest readback

Set `SDBZ_GS_GPU_READBACK=1` when running with the native WebGPU renderer.
Reverse GS DMA then reads a checked GPU VRAM snapshot, including rasterized
pixels missing from the CPU reference. A failed map rejects the transfer
without advancing its cursor. Split reads reuse a snapshot until the next
committed GIF batch. Headless CPU-only runs remain unchanged. This does not
yet remove CPU-side swizzling, local copies or palette processing.

### Experimental GPU-authoritative VRAM

`SDBZ_GS_GPU_VRAM=1` selects GPU-only materialization when the renderer
is active. CPU GS validates the entire transaction, tracks upload cursors,
CLD/CBP controls and local-copy bounds, but does not produce pixel journals
or CPU palette words. Checked diagnostic and guest reads retrieve GPU
snapshots. Raw CLUT values in the control-state struct are not authoritative
in this mode; use the checked palette accessor. The existing CPU mode
remains the default. This experimental path rejects aliased/wrapped-coordinate
destination rectangles, layout mutation during upload and copies exceeding
the compute scratch capacity. It does not support switching back to stale
CPU VRAM mid-run.

For performance-only replays, `SDBZ_GIF_IMAGE_DIGEST=0` skips the
per-byte diagnostic FNV-1a digest and nonzero-byte count of accepted IMAGE
payloads. Event/QWC counters, parsing, committed payload bytes and DMA
semantics remain unchanged. Reports mark the digest disabled and emit null
for unavailable byte-level statistics. The default retains full provenance.

For profiling, `SDBZ_WINDOW_PUMP_STRIDE=64` reduces host window clock
checks to one every 64 device-service calls while still calling the window
pump immediately on every observed guest field change. The existing 120 Hz
host input polling rule inside the pump is retained. The default stride is
1; valid values are 1 through 64. A larger stride can delay host input or
close-event detection between guest fields, especially during slow guest
execution. It does not change guest scanout or device-service timing.

## Windows (MSVC / LLVM)

From a Windows command prompt in the repository root:

```bat
git pull
set EE_WEBGPU_SNAPSHOT=1
scripts\run_ps2_game.bat
```

When WebGPU is enabled, the launcher automatically downloads and
installs the pinned `wgpu-native v29.0.1.1` **Windows x86-64 MSVC**
SDK under `build/_deps/wgpu-native/`. It verifies the same SHA-256 as
native CI and checks `include/webgpu/webgpu.h`,
`include/webgpu/wgpu.h`, `lib/wgpu_native.lib`, and the runtime DLL.
A complete installed SDK is reused without network access.
The downloader requires internet access only on the first install.

Direct CMake configuration also provisions the SDK on Windows, without
requiring the launcher:

```bat
cmake -S . -B build -DEE_ENABLE_WEBGPU=ON
```

CMake defaults `EE_WEBGPU_ROOT` to
`${CMAKE_BINARY_DIR}/_deps/wgpu-native`; for `-B build`, that is
`build/_deps/wgpu-native`. Linux CI extracts the same pinned release
for its platform into its own build tree's `_deps` directory.

To use an SDK already installed elsewhere, optionally set
`EE_WEBGPU_ROOT` to its root directory containing `include/` and
`lib/`. For direct CMake, use `-DEE_WEBGPU_ROOT=PATH`; the two overrides
are independent. If an SDK directory exists but is incomplete, the
installer fails explicitly instead of overwriting local files. Repair
or empty that directory before retrying.

This configures `-DEE_ENABLE_WEBGPU=ON`, builds the native renderer,
and requests a GPU snapshot at
`build\ps2-game-hle\gs-webgpu.ppm`.

The existing `gs-display.ppm` (CPU GS VRAM display readback) and
`gs-upload.ppm` (completed host-to-local texture transfer) remain
separate diagnostics. Neither an upload nor a GPU snapshot on its own
proves that a real guest present event occurred.

For a standalone native GPU smoke test:

```bat
cmake --build build --config Release --target ee_webgpu_native_smoke
ctest --test-dir build -C Release -R ee_webgpu_native_smoke --output-on-failure
```

For a CPU-only full replay, explicitly set both `EE_WEBGPU_SNAPSHOT=0` and `EE_WEBGPU_WINDOW=0` before running `scripts\run_ps2_game.bat --full`. Full mode defaults both flags to 1 when unset; quick, matched and recovery modes force both off. The launcher explicitly configures the CMake GPU options for the selected mode.

## Optional raylib Win32 window

raylib provides the native window and host input loop. The existing
wgpu-native GS backend continues to perform compute rasterization, blitting,
and DX12 swapchain presentation. Although raylib creates an OpenGL context
during window setup, no raylib OpenGL rendering or swapping is used.

On Windows, in the normal extracted-disc workflow:

```bat
git pull
set EE_WEBGPU_WINDOW=1
scripts\run_ps2_game.bat
```

The launcher enables `EE_ENABLE_WEBGPU` and `EE_ENABLE_RAYLIB_WINDOW`.
CMake fetches the source for raylib 5.5 at its pinned commit under
`build/_deps/raylib-src`; the wgpu-native SDK remains under
`build/_deps/wgpu-native`. The window opens **only during the final traced
replay**, not the repeated native expansion passes. You may also combine
`EE_WEBGPU_WINDOW=1` with `EE_WEBGPU_SNAPSHOT=1` to retain a PPM
diagnostic. Direct runner and Python tooling accept `--gs-webgpu-window`.

The window is resizable, and physical framebuffer dimensions are used for
the native WebGPU surface. The host polls input without calling raylib's
`BeginDrawing`/`EndDrawing`. Presentation follows real scheduled GS
scanout-field changes and requires a valid guest-configured GS display.
No synthetic guest VBlank, interrupt, DMA completion, or fake frame is
introduced. A blank window is expected until genuine visible GS output exists.

This first integration is Win32-only. Native Linux builds, headless tests,
offscreen WebGPU snapshots, and GPU-independent HLE execution remain available.

## Current acceptance boundaries

Native game-driven rendering and Win32 window presentation have been observed in bounded SDBZ training gameplay. The [retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md) records 2,886 successful presents in the 12:06 run. Matching display captures in GPU-authoritative mode are not independent CPU/GPU raster evidence. Full graphics parity, other modes/titles and a new retail replay on the final PR #156 revision remain unverified. The GPU implementation
cannot invent GS primitives, renderer commands, VBlank events, or valid
framebuffer state that the guest has not produced. Keep the existing
HLE and DMA tests and the strict matched-log workflow.
