# Native WebGPU GS raster (no browser)

## Provenance and scope

The native GS compute backend, VRAM/CLUT/upload/raster/display WGSL, and
swizzle tables are adapted from
[twist84/ps2-web-recomp:SDBZ](https://github.com/twist84/ps2-web-recomp/tree/SDBZ).
See `src/runtime/ps2_webgpu/gs/LICENSE.upstream` for its MIT license.

There is **no** Emscripten, JavaScript, browser window, SDL, resource-pack UI,
or FFON text overlay. The port uses the original native wgpu-native API.

## Build and run on Windows

Download **wgpu-native v29.0.1.1** from
https://github.com/gfx-rs/wgpu-native/releases/tag/v29.0.1.1 and unpack the
**windows-x86_64-msvc** release (not the incompatible MinGW .dll.a release).
Its root must contain `include/webgpu/webgpu.h`,
`include/webgpu/wgpu.h` and the MSVC import library in `lib`.

~~~bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DEE_ENABLE_WEBGPU=ON -DEE_WEBGPU_ROOT=C:\Dev\wgpu-native
cmake --build build --target ee_game_runner ee_webgpu_native_smoke
~~~

For the normal `scripts\run_ps2_game.bat` workflow:

~~~bat
set "EE_WEBGPU_ROOT=C:\Dev\wgpu-native"
set "EE_WEBGPU_SNAPSHOT=1"
scripts\run_ps2_game.bat
~~~

The launcher configures the optional GPU target and produces
`build/ps2-game-hle/gs-webgpu.ppm` during its **final traced replay**,
alongside existing `gs-display.ppm` and `gs-upload.ppm` diagnostics.
To opt out, clear `EE_WEBGPU_SNAPSHOT` and reconfigure with
`-DEE_ENABLE_WEBGPU=OFF` if you want the wgpu dependency removed.
Direct runs also accept `--gs-webgpu-snapshot=PATH` in `ee_game_runner`;
`tools/run_game.py` and `tools/run_boot_session.py` forward it.

## Linux

Use the wgpu-native Linux x86-64 release and a Vulkan driver:

~~~sh
cmake -S . -B build -G Ninja -DEE_ENABLE_WEBGPU=ON -DEE_WEBGPU_ROOT=/opt/wgpu-native
cmake --build build --target ee_game_runner ee_webgpu_native_smoke
ctest --test-dir build -R ee_webgpu_native_smoke --output-on-failure
~~~

The default CMake option remains OFF, preserving the existing headless boot.

## Runtime

The CPU `c_ee_gs_state` remains the authoritative, atomic GIF PATH3
consumer and 4 MiB VRAM model. Its optional observer is called **only after
a successful batch commit**. `c_ee_webgpu_bridge` mirrors PACKED/REGLIST
registers, PRE/PRIM, XYZ vertices, IMAGE/HWREG transfers, CLUT, local copies,
and TEXFLUSH to the native compute backend, with an offscreen display
readback from actual guest GS display registers.

The renderer cannot approve/reject guest DMA, clear STR, raise IRQs,
synthesize VBlank, or mutate EE/IOP state. A GPU readback is **not** proof
of a guest present event, and a black image can still be correct if the guest
has not issued visible primitives. PATH1/PATH2, VIF/VU-driven rendering,
native PNG replacements, real-time Win32 presentation, and pixel-perfect
differential validation remain future integration work.

## Tests

The ordinary CTest suite runs a GPU-independent adapter regression and tests
that rejected GIF/GS transactions produce **no** GPU observer callbacks.
Additional native CI builds against checksum-pinned wgpu-native SDKs on
Linux and Windows; Linux also runs a real offscreen compute upload/display
test through the Mesa software Vulkan adapter.
