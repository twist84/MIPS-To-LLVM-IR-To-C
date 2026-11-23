cmake_minimum_required(VERSION 3.18)
project(ps2_recompiler C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Use the LLVM-provided clang/clang++ and lld-link when available. LLVM is kept
# optional: the core decoder/runtime build without LLVM (PLAN.md section 6).
if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER "clang")
endif()
if(NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER "clang++")
endif()

if(EXISTS "/usr/bin/ld.lld" OR EXISTS "/usr/local/bin/ld.lld")
  set(CMAKE_EXE_LINKER_FLAGS "-fuse-ld=lld")
  set(CMAKE_SHARED_LINKER_FLAGS "-fuse-ld=lld")
  set(CMAKE_MODULE_LINKER_FLAGS "-fuse-ld=lld")
endif()

include_directories(include include/ee src/llvm src/lift src/runtime)

add_library(ee_core STATIC
    src/ee/decoder.cpp
    src/ee/opcode_tables.cpp
    src/ee/function_analyzer.cpp
    src/ee/recompiled_functions.cpp
    src/ee/ee_dispatcher.cpp
    src/elf/elf_loader.cpp
    src/llvm/llvm_context.cpp
    src/llvm/module_builder.cpp
    src/llvm/object_emitter.cpp
    src/lift/instruction_lifter.cpp
    src/lift/native_codegen.cpp
    src/runtime/ee_memory.cpp
    src/runtime/ee_ram_interpreter.cpp
    src/runtime/ee_unaligned.cpp
    src/runtime/ee_intc.cpp
    src/runtime/ee_dmac.cpp
    src/runtime/ee_gif.cpp
    src/runtime/ee_vif1.cpp
    src/runtime/ee_vu1.cpp
    src/runtime/ee_gs.cpp
    src/runtime/ee_gs_display.cpp
    src/runtime/ee_exceptions.cpp
    src/runtime/ee_vu0.cpp
    src/runtime/ee_vsync_flags.cpp
    src/runtime/ee_scanout_clock.cpp
    src/runtime/ee_cycle_scheduler.cpp
)

# Standalone IOP R3000A interpreter, initially isolated from the EE
# runner until module loading, timing and SIF transport are integrated.
add_library(iop_cpu STATIC
    src/iop/iop_cpu.cpp
    src/iop/iop_guest_fault_probe.cpp
    src/iop/iop_hw.cpp
    src/iop/iop_memory_card.cpp
    src/iop/iop_spu2.cpp
    src/iop/irx_loader.cpp
    src/iop/iop_kernel.cpp
    src/iop/iop_kernel_hle_bridge.cpp
    src/iop/iop_sysclib_strings.cpp
    src/iop/iop_sifman_bridge.cpp
    src/iop/iop_sif1_bridge.cpp
    src/iop/iop_sif1_dma_transport.cpp
    src/iop/iop_sif0_dma_transport.cpp
    src/iop/iop_sifcmd_bridge.cpp
    src/iop/iop_disc_loader.cpp
    src/iop/iop_cdvd.cpp
    src/iop/iop_folder_sector_source.cpp
    src/iop/iop_cdvdman_bridge.cpp
    src/iop/iop_sif_rpc_bridge.cpp
)
target_include_directories(iop_cpu PUBLIC ${CMAKE_SOURCE_DIR}/src/iop)
add_executable(iop_cpu_test tests/smoke/iop_cpu_test.cpp)
target_link_libraries(iop_cpu_test PRIVATE iop_cpu)
add_executable(iop_guest_fault_probe_test
    tests/smoke/iop_guest_fault_probe_test.cpp)
target_link_libraries(iop_guest_fault_probe_test PRIVATE iop_cpu)
add_executable(iop_rom0_boot_test tests/smoke/iop_rom0_boot_test.cpp)
target_link_libraries(iop_rom0_boot_test PRIVATE iop_cpu)
add_executable(iop_hw_test tests/smoke/iop_hw_test.cpp)
target_link_libraries(iop_hw_test PRIVATE iop_cpu)
add_executable(iop_timer5_exception_test tests/smoke/iop_timer5_exception_test.cpp)
target_link_libraries(iop_timer5_exception_test PRIVATE iop_cpu)
add_executable(iop_dma_icr_test tests/smoke/iop_dma_icr_test.cpp)
target_link_libraries(iop_dma_icr_test PRIVATE iop_cpu)
add_executable(iop_dma_global_enable_test tests/smoke/iop_dma_global_enable_test.cpp)
target_link_libraries(iop_dma_global_enable_test PRIVATE iop_cpu)
add_executable(iop_dma_sif_channels_test tests/smoke/iop_dma_sif_channels_test.cpp)
target_link_libraries(iop_dma_sif_channels_test PRIVATE iop_cpu)
add_executable(sif1_dma_registers_test tests/smoke/sif1_dma_registers_test.cpp)
target_link_libraries(sif1_dma_registers_test PRIVATE iop_cpu ee_core)
add_executable(sif1_ee_to_iop_test tests/smoke/sif1_ee_to_iop_test.cpp)
target_link_libraries(sif1_ee_to_iop_test PRIVATE iop_cpu ee_core)
add_executable(sif1_ee_chain_test tests/smoke/sif1_ee_chain_test.cpp)
target_link_libraries(sif1_ee_chain_test PRIVATE iop_cpu ee_core)
add_executable(sif0_iop_to_ee_test tests/smoke/sif0_iop_to_ee_test.cpp)
target_link_libraries(sif0_iop_to_ee_test PRIVATE iop_cpu ee_core)
add_executable(sif_dma_roundtrip_test tests/smoke/sif_dma_roundtrip_test.cpp)
target_link_libraries(sif_dma_roundtrip_test PRIVATE iop_cpu ee_core)
add_executable(sif0_dma_invalid_tags_test tests/smoke/sif0_dma_invalid_tags_test.cpp)
target_link_libraries(sif0_dma_invalid_tags_test PRIVATE iop_cpu ee_core)
add_executable(sif1_dma_invalid_tags_test tests/smoke/sif1_dma_invalid_tags_test.cpp)
target_link_libraries(sif1_dma_invalid_tags_test PRIVATE iop_cpu ee_core)
add_executable(iop_sif_handoff_test tests/smoke/iop_sif_handoff_test.cpp)
target_link_libraries(iop_sif_handoff_test PRIVATE iop_cpu)
add_executable(iop_sif_mmio_link_test tests/smoke/iop_sif_mmio_link_test.cpp)
target_link_libraries(iop_sif_mmio_link_test PRIVATE iop_cpu ee_core)
add_executable(iop_rom0_runner src/tools/iop_rom0_runner_main.cpp)
target_link_libraries(iop_rom0_runner PRIVATE iop_cpu)
add_executable(iop_rom0_sif_integration src/tools/iop_rom0_sif_integration_main.cpp)
target_link_libraries(iop_rom0_sif_integration PRIVATE iop_cpu ee_core)
add_executable(iop_irx_loader_test tests/smoke/iop_irx_loader_test.cpp)
target_link_libraries(iop_irx_loader_test PRIVATE iop_cpu)
add_executable(iop_irx_export_link_test tests/smoke/iop_irx_export_link_test.cpp)
target_link_libraries(iop_irx_export_link_test PRIVATE iop_cpu)
add_executable(iop_sysclib_memory_test tests/smoke/iop_sysclib_memory_test.cpp)
target_link_libraries(iop_sysclib_memory_test PRIVATE iop_cpu)
add_executable(iop_sysclib_string_test tests/smoke/iop_sysclib_string_test.cpp)
target_link_libraries(iop_sysclib_string_test PRIVATE iop_cpu)
add_executable(iop_sysclib_ctype_test tests/smoke/iop_sysclib_ctype_test.cpp)
target_link_libraries(iop_sysclib_ctype_test PRIVATE iop_cpu)
add_executable(iop_kernel_hle_bridge_test tests/smoke/iop_kernel_hle_bridge_test.cpp)
target_link_libraries(iop_kernel_hle_bridge_test PRIVATE iop_cpu)
add_executable(iop_kernel_test tests/smoke/iop_kernel_test.cpp)
target_link_libraries(iop_kernel_test PRIVATE iop_cpu)
add_executable(iop_sifman_bridge_test tests/smoke/iop_sifman_bridge_test.cpp)
target_link_libraries(iop_sifman_bridge_test PRIVATE iop_cpu ee_core)
add_executable(iop_sif_dma_test tests/smoke/iop_sif_dma_test.cpp)
target_link_libraries(iop_sif_dma_test PRIVATE iop_cpu ee_core)
add_executable(iop_sif1_bridge_test tests/smoke/iop_sif1_bridge_test.cpp)
target_link_libraries(iop_sif1_bridge_test PRIVATE iop_cpu ee_core)
add_executable(iop_sifcmd_bridge_test tests/smoke/iop_sifcmd_bridge_test.cpp)
target_link_libraries(iop_sifcmd_bridge_test PRIVATE iop_cpu ee_core)
add_executable(iop_disc_loader_test tests/smoke/iop_disc_loader_test.cpp)
target_link_libraries(iop_disc_loader_test PRIVATE iop_cpu)
add_executable(iop_cdvd_test tests/smoke/iop_cdvd_test.cpp)
target_link_libraries(iop_cdvd_test PRIVATE iop_cpu)
add_executable(iop_folder_sector_source_test tests/smoke/iop_folder_sector_source_test.cpp)
target_link_libraries(iop_folder_sector_source_test PRIVATE iop_cpu)
add_executable(iop_cdvdman_bridge_test tests/smoke/iop_cdvdman_bridge_test.cpp)
target_link_libraries(iop_cdvdman_bridge_test PRIVATE iop_cpu ee_core)
add_executable(iop_sif_rpc_bridge_test tests/smoke/iop_sif_rpc_bridge_test.cpp)
target_link_libraries(iop_sif_rpc_bridge_test PRIVATE iop_cpu ee_core)

add_executable(ee_smoke_test
    tests/smoke/smoke_test.cpp
    tests/smoke/native_smoke.cpp
)
target_link_libraries(ee_smoke_test PRIVATE ee_core)

# Build a native object for the reference/interpreter smoke harness. This is
# deliberately not described as LLVM IR object emission: the actual LLVM text
# path is verified separately by tools/verify_ir.py when llvmlite is available.
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/recompiled/guest_fn_0x100000.obj
    COMMAND ${CMAKE_CXX_COMPILER} -O2 -Wall -Wextra -c
        -I${CMAKE_SOURCE_DIR}/include/ee
        -I${CMAKE_SOURCE_DIR}/src/runtime
        ${CMAKE_SOURCE_DIR}/tests/smoke/native_smoke.cpp
        -o ${CMAKE_BINARY_DIR}/recompiled/guest_fn_0x100000.obj
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Emitting native object for lifted guest function"
    VERBATIM
)
add_custom_target(ee_emit_obj DEPENDS
    ${CMAKE_BINARY_DIR}/recompiled/guest_fn_0x100000.obj
)

add_executable(ee_multi_test
    tests/smoke/multi_function_test.cpp
)
target_link_libraries(ee_multi_test PRIVATE ee_core)

# Function entry discovery, including conservative CFG constant propagation
# for static JALR call targets.
add_executable(ee_function_discovery_test
    tests/smoke/function_discovery_test.cpp
)
target_link_libraries(ee_function_discovery_test PRIVATE ee_core)

# Host-side PC-based dispatcher regression tests.
add_executable(ee_dispatcher_test tests/smoke/dispatcher_test.cpp)
target_link_libraries(ee_dispatcher_test PRIVATE ee_core)

add_executable(ee_dispatch_interrupt_test tests/smoke/ee_dispatch_interrupt_test.cpp)
target_link_libraries(ee_dispatch_interrupt_test PRIVATE ee_core)
add_executable(ee_cop0_write_test tests/smoke/ee_cop0_write_test.cpp)
target_link_libraries(ee_cop0_write_test PRIVATE ee_core)

# VU0 transfer scheduler contract and architectural state regression tests.
add_executable(ee_vu0_runtime_test tests/smoke/vu0_runtime_test.cpp)
target_link_libraries(ee_vu0_runtime_test PRIVATE ee_core)

# Authentic SDBZ US COP2 matrix macro and VU0 FMAC regression.
add_executable(ee_vu0_macro_fmac_test tests/smoke/vu0_macro_fmac_test.cpp)
target_link_libraries(ee_vu0_macro_fmac_test PRIVATE ee_core)

# Native-lowered VU0 indexed memory macros and fail-closed edge cases.
add_executable(ee_vu0_indexed_macro_test tests/smoke/ee_vu0_indexed_macro_test.cpp)
target_link_libraries(ee_vu0_indexed_macro_test PRIVATE ee_core)

add_executable(ee_vsync_flag_test tests/smoke/ee_vsync_flag_test.cpp)
target_link_libraries(ee_vsync_flag_test PRIVATE ee_core)

# Cycle-driven GS field events, without a synthetic runtime VBlank producer.
add_executable(ee_scanout_clock_test tests/smoke/ee_scanout_clock_test.cpp)
target_link_libraries(ee_scanout_clock_test PRIVATE ee_core)

# Isolated, uncalibrated R5900 issue-timing probe; never drives GS events.
add_executable(ee_issue_timing_probe_test tests/smoke/ee_issue_timing_probe_test.cpp)
target_link_libraries(ee_issue_timing_probe_test PRIVATE ee_core)

add_executable(ee_cycle_scheduler_test tests/smoke/ee_cycle_scheduler_test.cpp)
target_link_libraries(ee_cycle_scheduler_test PRIVATE ee_core)

add_executable(ee_issue_trace_replay_test tests/smoke/ee_issue_trace_replay_test.cpp)
target_link_libraries(ee_issue_trace_replay_test PRIVATE ee_core)

add_executable(ee_issue_trace_candidate src/tools/ee_issue_trace_candidate_main.cpp)
target_link_libraries(ee_issue_trace_candidate PRIVATE ee_core)

# Emit a deterministic per-pair VU0 trace from the in-tree candidate runtime.
add_executable(ee_vu0_trace_candidate tests/tools/vu0_trace_candidate.cpp)
target_link_libraries(ee_vu0_trace_candidate PRIVATE ee_core)

# Build the real runtime helper symbols into a loadable library so the LLVM
# MCJIT regression can resolve the helper calls exactly as generated.
add_library(ee_vu0_jit_helpers SHARED src/runtime/ee_vu0.cpp)
set_target_properties(ee_vu0_jit_helpers PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

# Expose the same runtime COP0 side-effect helper to the emitted LLVM JIT
# regression; this is real C++ guest state behavior, not a Python mock.
add_library(ee_cop0_jit_helpers SHARED src/runtime/ee_exceptions.cpp)
set_target_properties(ee_cop0_jit_helpers PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

add_executable(ee_vu0_transfer_ir_fixture
    tests/smoke/vu0_transfer_ir_fixture.cpp
)
target_link_libraries(ee_vu0_transfer_ir_fixture PRIVATE ee_core)

# Real decoded R5900 signed arithmetic and integer trap JIT fixture.
add_executable(ee_checked_ir_fixture
    tests/smoke/ee_checked_ir_fixture.cpp)
target_link_libraries(ee_checked_ir_fixture PRIVATE ee_core)

add_executable(ee_min_test
    tests/smoke/min_test.cpp
)
target_link_libraries(ee_min_test PRIVATE ee_core)

# Full-game static recompiler: load an EE ELF, discover functions, build CFGs,
# lift every instruction, emit per-function LLVM IR text + coverage metadata.
add_executable(ee_recomp
    src/tools/ee_recomp_main.cpp
    src/ee/ee_recomp.cpp
)
target_link_libraries(ee_recomp PRIVATE ee_core)

# Standalone host for translated EE functions and optional, explicit
# IOP IRX execution at SIF polling rendezvous points.
add_executable(ee_hot_loop_diagnostics_test tests/smoke/ee_hot_loop_diagnostics_test.cpp)
add_test(NAME ee_hot_loop_diagnostics COMMAND ee_hot_loop_diagnostics_test)

# Pure host watchdog gate: no guest IR, EE runtime or GPU dependency.
add_executable(ee_host_loop_yield_budget_test
    tests/smoke/ee_host_loop_yield_budget_test.cpp)
target_include_directories(ee_host_loop_yield_budget_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/tools)
add_test(NAME ee_host_loop_yield_budget COMMAND ee_host_loop_yield_budget_test)

# Regression for Windows CRLF cache-miss replies: tests the same native parser
# used by ee_game_runner with no game binaries or Python/Tk dependency.
add_executable(ee_hot_expand_protocol_test
    tests/smoke/ee_hot_expand_protocol_test.cpp)
target_include_directories(ee_hot_expand_protocol_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/tools)
add_test(NAME ee_hot_expand_protocol COMMAND ee_hot_expand_protocol_test)

add_executable(ee_kernel_thread_status_test
    tests/smoke/ee_kernel_thread_status_test.cpp)
target_include_directories(ee_kernel_thread_status_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/tools)
add_test(NAME ee_kernel_thread_status COMMAND ee_kernel_thread_status_test)

add_executable(ee_irq_syscall_services_test
    tests/smoke/ee_irq_syscall_services_test.cpp)
target_include_directories(ee_irq_syscall_services_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/tools)
add_test(NAME ee_irq_syscall_services COMMAND ee_irq_syscall_services_test)

add_executable(ee_isa_inventory_test tests/smoke/ee_isa_inventory_test.cpp)
target_link_libraries(ee_isa_inventory_test PRIVATE ee_core)
add_test(NAME ee_isa_inventory COMMAND ee_isa_inventory_test)

add_executable(ee_reached_isa_inventory_test
    tests/smoke/ee_reached_isa_inventory_test.cpp)
target_link_libraries(ee_reached_isa_inventory_test PRIVATE ee_core)
add_test(NAME ee_reached_isa_inventory COMMAND ee_reached_isa_inventory_test)

add_executable(ee_game_runner src/tools/ee_game_runner_main.cpp)
target_link_libraries(ee_game_runner PRIVATE ee_core iop_cpu ${CMAKE_DL_LIBS})

# Original native WebGPU compute renderer, extracted from ps2-web-recomp:SDBZ.
# No Emscripten, browser host, game code, or SDL; headless builds unchanged.
option(EE_ENABLE_WEBGPU "Enable native wgpu-native GS rendering" OFF)
option(EE_ENABLE_RAYLIB_WINDOW "Use raylib window with native WebGPU GS presentation (Windows)" OFF)
if(EE_ENABLE_RAYLIB_WINDOW AND NOT EE_ENABLE_WEBGPU)
    message(FATAL_ERROR "EE_ENABLE_RAYLIB_WINDOW requires EE_ENABLE_WEBGPU=ON")
endif()
if(EE_ENABLE_RAYLIB_WINDOW AND NOT WIN32)
    message(FATAL_ERROR "The raylib WebGPU window currently supports Win32 only")
endif()
# Keep the SDK under the CMake build tree, never in the source checkout.
# The usual Windows launcher uses build/_deps/wgpu-native.
set(EE_WEBGPU_ROOT "${CMAKE_BINARY_DIR}/_deps/wgpu-native" CACHE PATH
    "wgpu-native SDK root (include/ and lib/); defaults to build/_deps")
# Migrate the old empty cached default without overriding a custom SDK root.
if("${EE_WEBGPU_ROOT}" STREQUAL "")
    set(EE_WEBGPU_ROOT "${CMAKE_BINARY_DIR}/_deps/wgpu-native"
        CACHE PATH "wgpu-native SDK root (include/ and lib/)" FORCE)
endif()
if(EE_ENABLE_WEBGPU)
    if(WIN32)
        # A direct CMake configure (not only run_ps2_game.bat) must also
        # provision the pinned SDK. Existing complete installs are reused.
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        execute_process(
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/tools/ensure_wgpu_native.py"
                    --root "${EE_WEBGPU_ROOT}"
            RESULT_VARIABLE EE_WEBGPU_BOOTSTRAP_RESULT
            OUTPUT_VARIABLE EE_WEBGPU_BOOTSTRAP_OUTPUT
            ERROR_VARIABLE EE_WEBGPU_BOOTSTRAP_ERROR)
        if(NOT EE_WEBGPU_BOOTSTRAP_RESULT EQUAL 0)
            message(FATAL_ERROR
                "wgpu-native SDK bootstrap failed for ${EE_WEBGPU_ROOT}:\n"
                "${EE_WEBGPU_BOOTSTRAP_OUTPUT}${EE_WEBGPU_BOOTSTRAP_ERROR}")
        endif()
        string(STRIP "${EE_WEBGPU_BOOTSTRAP_OUTPUT}"
            EE_WEBGPU_BOOTSTRAP_MESSAGE)
        if(EE_WEBGPU_BOOTSTRAP_MESSAGE)
            message(STATUS "${EE_WEBGPU_BOOTSTRAP_MESSAGE}")
        endif()
    endif()
    # Do not reuse cached paths from a previous SDK root (e.g. third_party).
    # All three dependencies must come from the selected build/_deps tree.
    unset(EE_WEBGPU_INCLUDE_DIR CACHE)
    unset(EE_WGPU_EXTRAS_INCLUDE_DIR CACHE)
    unset(EE_WGPU_NATIVE_LIBRARY CACHE)
    unset(EE_WGPU_NATIVE_DLL CACHE)
    find_path(EE_WEBGPU_INCLUDE_DIR NAMES webgpu/webgpu.h
        HINTS "${EE_WEBGPU_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
    find_path(EE_WGPU_EXTRAS_INCLUDE_DIR NAMES webgpu/wgpu.h
        HINTS "${EE_WEBGPU_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
    find_library(EE_WGPU_NATIVE_LIBRARY NAMES wgpu_native
        HINTS "${EE_WEBGPU_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    set(EE_NATIVE_WGSL_ROOT
        "${CMAKE_SOURCE_DIR}/src/runtime/ps2_webgpu/gs/gpu")
    foreach(shader IN ITEMS gs blit)
        set(out "${CMAKE_CURRENT_BINARY_DIR}/ee_webgpu_${shader}_wgsl.cpp")
        set(path "${EE_NATIVE_WGSL_ROOT}/${shader}.wgsl")
        if(shader STREQUAL "gs")
            set(symbol "kGsShader")
        else()
            set(symbol "kBlitShader")
        endif()
        add_custom_command(OUTPUT "${out}"
            COMMAND "${CMAKE_COMMAND}"
                "-DSRC=${path}" "-DOUT=${out}" "-DNAME=${symbol}"
                -P "${CMAKE_SOURCE_DIR}/cmake/embed_native_wgsl.cmake"
            DEPENDS "${path}" "${CMAKE_SOURCE_DIR}/cmake/embed_native_wgsl.cmake"
            VERBATIM)
        list(APPEND EE_NATIVE_WGSL_SOURCES "${out}")
    endforeach()
    add_library(ee_webgpu_native STATIC
        src/runtime/ps2_webgpu/gs/gpu/gpu.cpp
        src/runtime/ps2_webgpu/gs/swizzle.cpp
        src/runtime/ee_webgpu_bridge.cpp
        ${EE_NATIVE_WGSL_SOURCES})
    target_compile_features(ee_webgpu_native PRIVATE cxx_std_20)
    target_include_directories(ee_webgpu_native PRIVATE
        "${EE_WEBGPU_INCLUDE_DIR}"
        "${EE_WGPU_EXTRAS_INCLUDE_DIR}"
        "${CMAKE_SOURCE_DIR}/src/runtime")
    target_link_libraries(ee_webgpu_native PUBLIC "${EE_WGPU_NATIVE_LIBRARY}")
    if(WIN32)
        # The pinned MSVC wgpu-native SDK includes Rust std objects in
        # wgpu_native.lib. clang++/lld-link does not infer their Windows
        # system imports from Rust link metadata; link the required SDK
        # providers explicitly and propagate them to runner/smoke targets.
        # ws2_32: getaddrinfo, WSA*; ntdll: NtReadFile/NtOpenFile;
        # userenv: GetUserProfileDirectoryW.
        target_link_libraries(ee_webgpu_native PUBLIC
            ws2_32 ntdll userenv)
    endif()
    if(UNIX AND NOT APPLE)
        target_link_libraries(ee_webgpu_native PUBLIC ${CMAKE_DL_LIBS} pthread m)
    endif()
    target_compile_definitions(ee_game_runner PRIVATE EE_ENABLE_WEBGPU=1)
    target_link_libraries(ee_game_runner PRIVATE ee_webgpu_native)
    if(EE_ENABLE_RAYLIB_WINDOW)
        include(FetchContent)
        # Pin raylib 5.5 to a specific commit under build/_deps.
        set(BUILD_EXAMPLES OFF CACHE BOOL "Build raylib examples" FORCE)
        FetchContent_Declare(raylib
            GIT_REPOSITORY https://github.com/raysan5/raylib.git
            GIT_TAG c1ab645ca298a2801097931d1079b10ff7eb9df8)
        FetchContent_MakeAvailable(raylib)
        target_sources(ee_game_runner PRIVATE
            src/runtime/ee_raylib_window.cpp
            src/runtime/ee_raylib_win32.cpp)
        target_compile_definitions(ee_game_runner PRIVATE EE_ENABLE_RAYLIB_WINDOW=1)
        target_link_libraries(ee_game_runner PRIVATE raylib)
    endif()
    if(WIN32)
        find_file(EE_WGPU_NATIVE_DLL NAMES wgpu_native.dll
            HINTS "${EE_WEBGPU_ROOT}/lib" "${EE_WEBGPU_ROOT}/bin")
        if(EE_WGPU_NATIVE_DLL)
            add_custom_command(TARGET ee_game_runner POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${EE_WGPU_NATIVE_DLL}" "$<TARGET_FILE_DIR:ee_game_runner>"
                VERBATIM)
        endif()
    endif()
endif()

if(WIN32)
    # The native runner holds its large EE/IOP diagnostic state in a
    # monolithic main() frame. Both MSVC and clang++/lld-link need an
    # explicit 8 MiB stack reserve, not the default 1 MiB PE reserve.
    # if(MSVC) misses clang++ with GNU-style driver flags on Windows.
    # LINKER: forwards the identical /STACK value for both drivers.
    target_link_options(ee_game_runner PRIVATE "LINKER:/STACK:8388608")
endif()

add_executable(ee_ram_interpreter_test tests/smoke/ee_ram_interpreter_test.cpp)
target_link_libraries(ee_ram_interpreter_test PRIVATE ee_core)
add_executable(ee_ram_tlb_test tests/smoke/ee_ram_tlb_test.cpp)
target_link_libraries(ee_ram_tlb_test PRIVATE ee_core)

add_executable(ee_memory_alias_test tests/smoke/ee_memory_alias_test.cpp)
target_link_libraries(ee_memory_alias_test PRIVATE ee_core)

add_executable(ee_physical_bus_test tests/smoke/ee_physical_bus_test.cpp)
target_link_libraries(ee_physical_bus_test PRIVATE ee_core)

add_executable(ee_intc_test tests/smoke/ee_intc_test.cpp)
target_link_libraries(ee_intc_test PRIVATE ee_core)

add_executable(ee_dmac_gif_observer_test tests/smoke/ee_dmac_gif_observer_test.cpp)
target_link_libraries(ee_dmac_gif_observer_test PRIVATE ee_core)
add_test(NAME ee_dmac_gif_observer COMMAND ee_dmac_gif_observer_test)

add_executable(ee_dmac_test tests/smoke/ee_dmac_test.cpp)
target_link_libraries(ee_dmac_test PRIVATE ee_core)

add_executable(ee_spr_dma_test tests/smoke/ee_spr_dma_test.cpp)
target_link_libraries(ee_spr_dma_test PRIVATE ee_core)

add_executable(ee_fromspr_chain_test tests/smoke/ee_fromspr_chain_test.cpp)
target_link_libraries(ee_fromspr_chain_test PRIVATE ee_core)

add_executable(ee_spr_interleave_test tests/smoke/ee_spr_interleave_test.cpp)
target_link_libraries(ee_spr_interleave_test PRIVATE ee_core)

add_executable(ee_tospr_chain_test tests/smoke/ee_tospr_chain_test.cpp)
target_link_libraries(ee_tospr_chain_test PRIVATE ee_core)

add_executable(ee_device_dma_test tests/smoke/ee_device_dma_test.cpp)
target_link_libraries(ee_device_dma_test PRIVATE ee_core)

add_executable(ee_gif_path3_test tests/smoke/ee_gif_path3_test.cpp)
target_link_libraries(ee_gif_path3_test PRIVATE ee_core)

add_executable(ee_vif1_chain_test tests/smoke/ee_vif1_chain_test.cpp)
target_link_libraries(ee_vif1_chain_test PRIVATE ee_core)
add_test(NAME ee_vif1_chain COMMAND ee_vif1_chain_test)

add_executable(ee_vif1_device_test tests/smoke/ee_vif1_device_test.cpp)
target_link_libraries(ee_vif1_device_test PRIVATE ee_core)
add_test(NAME ee_vif1_device COMMAND ee_vif1_device_test)

add_executable(ee_vu1_pipeline_test tests/smoke/ee_vu1_pipeline_test.cpp)
target_link_libraries(ee_vu1_pipeline_test PRIVATE ee_core)
add_test(NAME ee_vu1_pipeline COMMAND ee_vu1_pipeline_test)

add_executable(ee_sdbz_dma_wait_probe_test tests/smoke/ee_sdbz_dma_wait_probe_test.cpp)
target_link_libraries(ee_sdbz_dma_wait_probe_test PRIVATE ee_core)
add_test(NAME ee_sdbz_dma_wait_probe COMMAND ee_sdbz_dma_wait_probe_test)

add_executable(ee_sdbz_pad_return_probe_test tests/smoke/ee_sdbz_pad_return_probe_test.cpp)
target_link_libraries(ee_sdbz_pad_return_probe_test PRIVATE ee_core)
add_test(NAME ee_sdbz_pad_return_probe COMMAND ee_sdbz_pad_return_probe_test)

add_executable(ee_gs_gif_diagnostics_test tests/smoke/ee_gs_gif_diagnostics_test.cpp)
target_link_libraries(ee_gs_gif_diagnostics_test PRIVATE ee_core)
add_test(NAME ee_gs_gif_diagnostics COMMAND ee_gs_gif_diagnostics_test)

add_executable(ee_gs_presentation_probe_test
    tests/smoke/ee_gs_presentation_probe_test.cpp)
target_link_libraries(ee_gs_presentation_probe_test PRIVATE ee_core)
add_test(NAME ee_gs_presentation_probe COMMAND ee_gs_presentation_probe_test)

add_executable(ee_gs_state_test tests/smoke/ee_gs_state_test.cpp)
target_link_libraries(ee_gs_state_test PRIVATE ee_core)

# Compile the native GS event bridge with a fake GPU for CI,
# without an adapter or wgpu-native.
add_executable(ee_webgpu_bridge_test
    tests/smoke/ee_webgpu_bridge_test.cpp
    src/runtime/ee_webgpu_bridge.cpp)
target_link_libraries(ee_webgpu_bridge_test PRIVATE ee_core)

add_executable(ee_gs_display_test tests/smoke/ee_gs_display_test.cpp)
target_link_libraries(ee_gs_display_test PRIVATE ee_core)

add_executable(ee_gs_vram_test tests/smoke/ee_gs_vram_test.cpp)
target_link_libraries(ee_gs_vram_test PRIVATE ee_core)

add_executable(ee_gs_psmct24_test tests/smoke/ee_gs_psmct24_test.cpp)
target_link_libraries(ee_gs_psmct24_test PRIVATE ee_core)

add_executable(ee_gs_psmct16_test tests/smoke/ee_gs_psmct16_test.cpp)
target_link_libraries(ee_gs_psmct16_test PRIVATE ee_core)

add_executable(ee_gs_psmct16s_test tests/smoke/ee_gs_psmct16s_test.cpp)
target_link_libraries(ee_gs_psmct16s_test PRIVATE ee_core)

add_executable(ee_gs_psmt8_test tests/smoke/ee_gs_psmt8_test.cpp)
target_link_libraries(ee_gs_psmt8_test PRIVATE ee_core)

add_executable(ee_gs_psmt4_test tests/smoke/ee_gs_psmt4_test.cpp)
target_link_libraries(ee_gs_psmt4_test PRIVATE ee_core)

add_executable(ee_gs_clut_test tests/smoke/ee_gs_clut_test.cpp)
target_link_libraries(ee_gs_clut_test PRIVATE ee_core)

add_executable(ee_gs_psmt_high_test tests/smoke/ee_gs_psmt_high_test.cpp)
target_link_libraries(ee_gs_psmt_high_test PRIVATE ee_core)

add_executable(ee_gs_depth_test tests/smoke/ee_gs_depth_test.cpp)
target_link_libraries(ee_gs_depth_test PRIVATE ee_core)

add_executable(ee_gs_local_copy_test tests/smoke/ee_gs_local_copy_test.cpp)
target_link_libraries(ee_gs_local_copy_test PRIVATE ee_core)

add_executable(ee_gs_download_test tests/smoke/ee_gs_download_test.cpp)
target_link_libraries(ee_gs_download_test PRIVATE ee_core)

add_executable(ee_gs_download_dma_test tests/smoke/ee_gs_download_dma_test.cpp)
target_link_libraries(ee_gs_download_dma_test PRIVATE ee_core)

add_executable(ee_gs_hwreg_test tests/smoke/ee_gs_hwreg_test.cpp)
target_link_libraries(ee_gs_hwreg_test PRIVATE ee_core)

add_executable(ee_gs_hwreg_gif_dma_test tests/smoke/ee_gs_hwreg_gif_dma_test.cpp)
target_link_libraries(ee_gs_hwreg_gif_dma_test PRIVATE ee_core)

add_executable(ee_scratchpad_test tests/smoke/ee_scratchpad_test.cpp)
target_link_libraries(ee_scratchpad_test PRIVATE ee_core)

add_executable(ee_vu0_memory_map_test tests/smoke/ee_vu0_memory_map_test.cpp)
target_link_libraries(ee_vu0_memory_map_test PRIVATE ee_core)

add_executable(ee_unaligned_memory_test tests/smoke/ee_unaligned_memory_test.cpp)
target_link_libraries(ee_unaligned_memory_test PRIVATE ee_core)

# Decoder regression test (MMI classification, VALID/UNSUPPORTED contract).
add_executable(ee_decoder_test tests/decoder/decoder_test.cpp)
target_link_libraries(ee_decoder_test PRIVATE ee_core)

# Emit a real LLVM module for a JIT-backed runtime test of HI1/LO1 transfers
# and the four supported MMI logical operations. llvmlite is optional so the
# core C++ build remains usable without the Python LLVM binding.
add_executable(ee_mmi_ir_fixture tests/smoke/mmi_ir_fixture.cpp)
target_link_libraries(ee_mmi_ir_fixture PRIVATE ee_core)

# Dedicated JIT runtime coverage for the EE's second divide pipeline.
add_executable(ee_div1_ir_fixture tests/smoke/div1_ir_fixture.cpp)
target_link_libraries(ee_div1_ir_fixture PRIVATE ee_core)

# Dedicated JIT runtime coverage for exactly lowered MMI1 packed operations.
add_executable(ee_mmi1_ir_fixture tests/smoke/mmi1_ir_fixture.cpp)
target_link_libraries(ee_mmi1_ir_fixture PRIVATE ee_core)

add_executable(ee_mmi_ops_test tests/smoke/mmi_ops_test.cpp)
target_link_libraries(ee_mmi_ops_test PRIVATE ee_core)

# JIT regressions for JAL/JALR/JR dispatch, link timing, and architectural delay slots.
add_executable(ee_control_flow_ir_fixture tests/smoke/control_flow_ir_fixture.cpp)
target_link_libraries(ee_control_flow_ir_fixture PRIVATE ee_core)

# Scalar 64-bit signed/unsigned comparison lowering regression.
add_executable(ee_compare_ir_fixture tests/smoke/compare_ir_fixture.cpp)
target_link_libraries(ee_compare_ir_fixture PRIVATE ee_core)

add_executable(ee_cop0_write_ir_fixture tests/smoke/cop0_write_ir_fixture.cpp)
target_link_libraries(ee_cop0_write_ir_fixture PRIVATE ee_core)

# R5900 single-precision FCC0 comparison normalization regression.
add_executable(ee_fpu_compare_ir_fixture tests/smoke/fpu_compare_ir_fixture.cpp)
target_link_libraries(ee_fpu_compare_ir_fixture PRIVATE ee_core)

find_package(Python3 COMPONENTS Interpreter QUIET)
set(EE_HAVE_LLVM_JIT_TEST FALSE)
if(Python3_Interpreter_FOUND)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "import llvmlite.binding"
        RESULT_VARIABLE EE_LLVM_BINDING_RESULT
        OUTPUT_QUIET ERROR_QUIET
    )
    if(EE_LLVM_BINDING_RESULT EQUAL 0)
        set(EE_HAVE_LLVM_JIT_TEST TRUE)
    endif()
endif()

enable_testing()

# Offline synthetic bootstrap test: verifies pin, ZIP layout, cache reuse
# and path safety; CI never downloads the SDK during this test.
if(Python3_Interpreter_FOUND)
    add_test(NAME ee_m2_dispatch_trace_analysis
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_analyze_m2_dispatch_trace.py)
    add_test(NAME ee_sdbz_elf_hotspot_xrefs
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_inspect_sdbz_elf_hotspots.py)
    add_test(NAME ee_wgpu_native_sdk_bootstrap
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_ensure_wgpu_native.py)
endif()

# Real wgpu-native compute execution, independent of browser or window.
if(EE_ENABLE_WEBGPU)
    add_executable(ee_webgpu_native_smoke
        tests/smoke/ee_webgpu_native_smoke.cpp)
    target_link_libraries(ee_webgpu_native_smoke PRIVATE ee_webgpu_native)
    add_test(NAME ee_webgpu_native_smoke COMMAND ee_webgpu_native_smoke)
    set_tests_properties(ee_webgpu_native_smoke
        PROPERTIES SKIP_RETURN_CODE 77)
endif()

if(Python3_Interpreter_FOUND)
    add_test(NAME ee_timing_differential
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_ee_timing_differential.py)
    add_test(NAME ee_issue_trace_export
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_ee_issue_trace_export.py
                $<TARGET_FILE:ee_issue_trace_candidate>
                ${CMAKE_SOURCE_DIR}/tests/fixtures/ee_issue_trace_candidate.csv)
    add_test(NAME iop_module_path_discovery
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_iop_module_paths.py)
    add_test(NAME iop_disc_catalog
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_iop_disc_catalog.py)
    add_test(NAME iop_irx_import_scan
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_iop_irx_imports.py)
    add_test(NAME ps2_system_cnf_boot
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_ps2_boot_elf.py)
    add_test(NAME boot_session_orchestration
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_boot_session.py)
    # Rebuilding host-only probe headers must relink existing guest
    # objects, never force a fresh ELF-to-LLVM translation.
    add_test(NAME ee_translation_cache_input_split
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_translation_cache_input_split.py)
    add_test(NAME ee_interrupted_append_recovery
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_interrupted_append_recovery.py)
    add_test(NAME ee_known_function_replays
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_known_replays.py)
    add_test(NAME ee_live_cache_miss
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_live_cache_miss.py)
    add_test(NAME ee_launcher_cache_resume
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_launch_ps2_gui.py)
    add_test(NAME ee_known_cache_recovery
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_cache_recovery.py)
    add_test(NAME iop_rom0_runner
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_iop_rom0_runner.py
                $<TARGET_FILE:iop_rom0_runner>)
    add_test(NAME recomp_discovery_cache
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_recomp_discovery_cache.py
                $<TARGET_FILE:ee_recomp>)
    add_test(NAME ee_pointer_table_expansion
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_function_pointer_tables.py)
    add_test(NAME ee_chunked_dispatcher
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_chunked_dispatcher.py)
    if(EE_HAVE_LLVM_JIT_TEST)
        add_test(NAME verify_ir_incremental
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tests/tools/test_verify_ir_incremental.py)
    endif()
    add_test(NAME ee_instruction_trace_comparator
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_compare_ee_instruction_traces.py
                ${CMAKE_SOURCE_DIR}/tools/compare_ee_instruction_traces.py)
    add_test(NAME vu0_trace_comparator
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_compare_vu0_traces.py)
    add_test(NAME vu0_candidate_trace
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_vu0_trace_candidate.py
                $<TARGET_FILE:ee_vu0_trace_candidate>
                ${CMAKE_SOURCE_DIR}/tools/compare_vu0_traces.py)
    add_test(NAME vu0_differential_driver_help
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tools/vu0_diff/run_differential.py
                --help)
    add_test(NAME pcsx2_vu0_installer
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_pcsx2_vu0_installer.py)
    add_test(NAME vu0_differential_x11_config
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_vu0_differential_x11_config.py)
endif()
add_test(NAME iop_cpu COMMAND iop_cpu_test)
add_test(NAME iop_guest_fault_probe COMMAND iop_guest_fault_probe_test)
add_test(NAME iop_rom0_boot COMMAND iop_rom0_boot_test)
add_test(NAME iop_hw COMMAND iop_hw_test)
add_test(NAME iop_timer5_exception COMMAND iop_timer5_exception_test)
add_test(NAME iop_dma_icr COMMAND iop_dma_icr_test)
add_test(NAME iop_dma_global_enable COMMAND iop_dma_global_enable_test)
add_test(NAME iop_dma_sif_channels COMMAND iop_dma_sif_channels_test)
add_test(NAME sif1_dma_registers COMMAND sif1_dma_registers_test)
add_test(NAME sif1_ee_to_iop COMMAND sif1_ee_to_iop_test)
add_test(NAME sif1_ee_chain COMMAND sif1_ee_chain_test)
add_test(NAME sif0_iop_to_ee COMMAND sif0_iop_to_ee_test)
add_test(NAME sif_dma_roundtrip COMMAND sif_dma_roundtrip_test)
add_test(NAME sif0_dma_invalid_tags COMMAND sif0_dma_invalid_tags_test)
add_test(NAME sif1_dma_invalid_tags COMMAND sif1_dma_invalid_tags_test)
add_test(NAME iop_sif_handoff COMMAND iop_sif_handoff_test)
add_test(NAME iop_sif_mmio_link COMMAND iop_sif_mmio_link_test)
add_test(NAME iop_irx_loader COMMAND iop_irx_loader_test)
add_test(NAME iop_irx_export_link COMMAND iop_irx_export_link_test)
add_test(NAME iop_sysclib_memory COMMAND iop_sysclib_memory_test)
add_test(NAME iop_sysclib_string COMMAND iop_sysclib_string_test)
add_test(NAME iop_sysclib_ctype COMMAND iop_sysclib_ctype_test)
add_test(NAME iop_kernel COMMAND iop_kernel_test)
add_test(NAME iop_kernel_hle_bridge COMMAND iop_kernel_hle_bridge_test)
add_test(NAME iop_sifman_bridge COMMAND iop_sifman_bridge_test)
add_test(NAME iop_sif_dma COMMAND iop_sif_dma_test)
add_test(NAME iop_sif1_bridge COMMAND iop_sif1_bridge_test)
add_test(NAME iop_sifcmd_bridge COMMAND iop_sifcmd_bridge_test)
add_test(NAME iop_disc_loader COMMAND iop_disc_loader_test)
add_test(NAME iop_cdvd COMMAND iop_cdvd_test)
add_test(NAME iop_folder_sector_source COMMAND iop_folder_sector_source_test)
add_test(NAME iop_cdvdman_bridge COMMAND iop_cdvdman_bridge_test)
add_test(NAME iop_sif_rpc_bridge COMMAND iop_sif_rpc_bridge_test)
add_test(NAME decoder COMMAND ee_decoder_test)
add_test(NAME dispatcher COMMAND ee_dispatcher_test)
add_test(NAME ee_dispatch_interrupt COMMAND ee_dispatch_interrupt_test)
add_test(NAME ee_cop0_write COMMAND ee_cop0_write_test)
add_test(NAME ee_memory_alias COMMAND ee_memory_alias_test)
add_test(NAME ee_physical_bus COMMAND ee_physical_bus_test)
add_test(NAME ee_intc COMMAND ee_intc_test)
add_test(NAME ee_dmac COMMAND ee_dmac_test)
add_test(NAME ee_spr_dma COMMAND ee_spr_dma_test)
add_test(NAME ee_fromspr_chain COMMAND ee_fromspr_chain_test)
add_test(NAME ee_spr_interleave COMMAND ee_spr_interleave_test)
add_test(NAME ee_tospr_chain COMMAND ee_tospr_chain_test)
add_test(NAME ee_device_dma COMMAND ee_device_dma_test)
add_test(NAME ee_gif_path3 COMMAND ee_gif_path3_test)
add_test(NAME ee_gs_state COMMAND ee_gs_state_test)
add_test(NAME ee_webgpu_bridge COMMAND ee_webgpu_bridge_test)
add_test(NAME ee_gs_display COMMAND ee_gs_display_test)
add_test(NAME ee_gs_vram COMMAND ee_gs_vram_test)
add_test(NAME ee_gs_psmct24 COMMAND ee_gs_psmct24_test)
add_test(NAME ee_gs_psmct16 COMMAND ee_gs_psmct16_test)
add_test(NAME ee_gs_psmct16s COMMAND ee_gs_psmct16s_test)
add_test(NAME ee_gs_psmt8 COMMAND ee_gs_psmt8_test)
add_test(NAME ee_gs_psmt4 COMMAND ee_gs_psmt4_test)
add_test(NAME ee_gs_clut COMMAND ee_gs_clut_test)
add_test(NAME ee_gs_psmt_high COMMAND ee_gs_psmt_high_test)
add_test(NAME ee_gs_depth COMMAND ee_gs_depth_test)
add_test(NAME ee_gs_local_copy COMMAND ee_gs_local_copy_test)
add_test(NAME ee_gs_download COMMAND ee_gs_download_test)
add_test(NAME ee_gs_download_dma COMMAND ee_gs_download_dma_test)
add_test(NAME ee_gs_hwreg COMMAND ee_gs_hwreg_test)
add_test(NAME ee_gs_hwreg_gif_dma COMMAND ee_gs_hwreg_gif_dma_test)
add_test(NAME ee_ram_interpreter COMMAND ee_ram_interpreter_test)
add_test(NAME ee_ram_tlb COMMAND ee_ram_tlb_test)
add_test(NAME ee_scratchpad COMMAND ee_scratchpad_test)
add_test(NAME ee_vu0_memory_map COMMAND ee_vu0_memory_map_test)
add_test(NAME ee_unaligned_memory COMMAND ee_unaligned_memory_test)
add_test(NAME vu0_runtime COMMAND ee_vu0_runtime_test)
add_test(NAME vu0_macro_fmac COMMAND ee_vu0_macro_fmac_test)
add_test(NAME ee_vu0_indexed_macro COMMAND ee_vu0_indexed_macro_test)
add_test(NAME ee_vsync_flag COMMAND ee_vsync_flag_test)
add_test(NAME ee_scanout_clock COMMAND ee_scanout_clock_test)
add_test(NAME ee_issue_timing_probe COMMAND ee_issue_timing_probe_test)
add_test(NAME ee_cycle_scheduler COMMAND ee_cycle_scheduler_test)
add_test(NAME ee_issue_trace_replay COMMAND ee_issue_trace_replay_test)
add_test(NAME ee_issue_trace_candidate COMMAND ee_issue_trace_candidate ${CMAKE_SOURCE_DIR}/tests/fixtures/ee_issue_trace_candidate.csv 1 3)
set_tests_properties(ee_issue_trace_candidate PROPERTIES PASS_REGULAR_EXPRESSION "candidate_issue_cycles=8")
add_test(NAME multi_function COMMAND ee_multi_test)
add_test(NAME function_discovery COMMAND ee_function_discovery_test)
add_test(NAME smoke COMMAND ee_smoke_test)
add_test(NAME mmi_ops COMMAND ee_mmi_ops_test)
if(EE_HAVE_LLVM_JIT_TEST)
    add_test(NAME fpu_compare_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_fpu_compare_ir_runtime.py
                $<TARGET_FILE:ee_fpu_compare_ir_fixture>)
    add_test(NAME vu0_transfer_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_vu0_transfer_ir_runtime.py
                $<TARGET_FILE:ee_vu0_transfer_ir_fixture>
                $<TARGET_FILE:ee_vu0_jit_helpers>)
    add_test(NAME vu0_quadword_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_vu0_quadword_ir_runtime.py
                $<TARGET_FILE:ee_vu0_transfer_ir_fixture>
                $<TARGET_FILE:ee_cop0_jit_helpers>)
    add_test(NAME ee_alignment_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_ee_alignment_ir_runtime.py
                $<TARGET_FILE:ee_vu0_transfer_ir_fixture>
                $<TARGET_FILE:ee_cop0_jit_helpers>)
    add_test(NAME ee_checked_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_ee_checked_ir_runtime.py
                $<TARGET_FILE:ee_checked_ir_fixture>
                $<TARGET_FILE:ee_cop0_jit_helpers>)
    add_test(NAME compare_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_compare_ir_runtime.py
                $<TARGET_FILE:ee_compare_ir_fixture>)
    add_test(NAME cop0_write_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_cop0_write_ir_runtime.py
                $<TARGET_FILE:ee_cop0_write_ir_fixture>
                $<TARGET_FILE:ee_cop0_jit_helpers>)
    add_test(NAME object_emission_and_dispatch
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_object_emission.py
                $<TARGET_FILE:ee_compare_ir_fixture>
                ${CMAKE_SOURCE_DIR}/tools/verify_ir.py
                ${CMAKE_SOURCE_DIR}/tools/link_native_objects.py
                ${CMAKE_SOURCE_DIR})
    add_test(NAME ee_timing_observer_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_ee_timing_observer_ir_runtime.py
                $<TARGET_FILE:ee_control_flow_ir_fixture>)
    # Independent synthetic ELF scenarios run in four parallel CI jobs;
    # omitting the optional argument still executes the complete suite.
    foreach(game_shard RANGE 0 3)
        add_test(NAME game_runner_end_to_end_${game_shard}
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tests/tools/test_game_runner.py
                    ${CMAKE_SOURCE_DIR}
                    ${CMAKE_BINARY_DIR}
                    $<TARGET_FILE:ee_game_runner>
                    ${game_shard})
    endforeach()
    add_test(NAME ee_native_thread_scheduler
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_ee_thread_scheduler.py
                --build-dir ${CMAKE_BINARY_DIR}
                --work-dir ${CMAKE_BINARY_DIR}/ee-thread-scheduler-test)
    set_tests_properties(ee_native_thread_scheduler PROPERTIES TIMEOUT 180)
    add_test(NAME ee_r5900_mult_gpr
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_ee_mult_gpr.py
                --build-dir ${CMAKE_BINARY_DIR}
                --work-dir ${CMAKE_BINARY_DIR}/ee-mult-gpr-test)
    set_tests_properties(ee_r5900_mult_gpr PROPERTIES TIMEOUT 180)
    add_test(NAME ee_sif0_stop_dma
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_ee_sif0_stop_dma.py
                --build-dir ${CMAKE_BINARY_DIR}
                --work-dir ${CMAKE_BINARY_DIR}/ee-sif0-stop-test)
    set_tests_properties(ee_sif0_stop_dma PROPERTIES TIMEOUT 180)
    add_test(NAME ee_memory_size_hle
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_ee_memory_size.py
                --build-dir ${CMAKE_BINARY_DIR}
                --work-dir ${CMAKE_BINARY_DIR}/ee-memory-size-test)
    set_tests_properties(ee_memory_size_hle PROPERTIES TIMEOUT 180)

    # Native Python integration tests select their own compatible linker:
    # Windows LLVM clang++ accepts the generated GNU-style driver flags,
    # while the configured CMake compiler may be MSVC cl.exe. Never force
    # CMAKE_CXX_COMPILER into the test environment, since it can override
    # an explicit caller CXX and make link_native_objects.py invoke cl.exe.
    # See select_native_link_compiler() in tools/link_native_objects.py.

    add_test(NAME control_flow_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_control_flow_ir_runtime.py
                $<TARGET_FILE:ee_control_flow_ir_fixture>)
    add_test(NAME guest_instruction_attempts_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_guest_instruction_attempts_ir_runtime.py
                $<TARGET_FILE:ee_control_flow_ir_fixture>)
    add_test(NAME guest_loop_yield_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_guest_loop_yield_ir_runtime.py
                $<TARGET_FILE:ee_control_flow_ir_fixture>)
    add_test(NAME ee_trace_helper_optimizer
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_trace_helper_optimizer.py
                $<TARGET_FILE:ee_mmi_ir_fixture>)
    add_test(NAME mmi_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_mmi_ir_runtime.py
                $<TARGET_FILE:ee_mmi_ir_fixture>)
else()
    message(STATUS "llvmlite not found: omitting mmi_ir_runtime CTest")
endif()
if(EE_HAVE_LLVM_JIT_TEST)
    add_test(NAME div1_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_div1_ir_runtime.py
                $<TARGET_FILE:ee_div1_ir_fixture>)
    add_test(NAME mmi1_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_mmi1_ir_runtime.py
                $<TARGET_FILE:ee_mmi1_ir_fixture>)
endif()
set_tests_properties(multi_function smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
