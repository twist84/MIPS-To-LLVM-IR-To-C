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
    src/runtime/ee_gs.cpp
    src/runtime/ee_exceptions.cpp
    src/runtime/ee_vu0.cpp
)

# Standalone IOP R3000A interpreter, initially isolated from the EE
# runner until module loading, timing and SIF transport are integrated.
add_library(iop_cpu STATIC
    src/iop/iop_cpu.cpp
    src/iop/iop_hw.cpp
    src/iop/irx_loader.cpp
    src/iop/iop_kernel.cpp
    src/iop/iop_sifman_bridge.cpp
    src/iop/iop_sif1_bridge.cpp
    src/iop/iop_sif1_dma_transport.cpp
    src/iop/iop_sif0_dma_transport.cpp
    src/iop/iop_sifcmd_bridge.cpp
    src/iop/iop_disc_loader.cpp
    src/iop/iop_cdvd.cpp
    src/iop/iop_sif_rpc_bridge.cpp
)
target_include_directories(iop_cpu PUBLIC ${CMAKE_SOURCE_DIR}/src/iop)
add_executable(iop_cpu_test tests/smoke/iop_cpu_test.cpp)
target_link_libraries(iop_cpu_test PRIVATE iop_cpu)
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

# VU0 transfer scheduler contract and architectural state regression tests.
add_executable(ee_vu0_runtime_test tests/smoke/vu0_runtime_test.cpp)
target_link_libraries(ee_vu0_runtime_test PRIVATE ee_core)

# Emit a deterministic per-pair VU0 trace from the in-tree candidate runtime.
add_executable(ee_vu0_trace_candidate tests/tools/vu0_trace_candidate.cpp)
target_link_libraries(ee_vu0_trace_candidate PRIVATE ee_core)

# Build the real runtime helper symbols into a loadable library so the LLVM
# MCJIT regression can resolve the helper calls exactly as generated.
add_library(ee_vu0_jit_helpers SHARED src/runtime/ee_vu0.cpp)
set_target_properties(ee_vu0_jit_helpers PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

add_executable(ee_vu0_transfer_ir_fixture
    tests/smoke/vu0_transfer_ir_fixture.cpp
)
target_link_libraries(ee_vu0_transfer_ir_fixture PRIVATE ee_core)

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
add_executable(ee_game_runner src/tools/ee_game_runner_main.cpp)
target_link_libraries(ee_game_runner PRIVATE ee_core iop_cpu ${CMAKE_DL_LIBS})
if(MSVC)
    # This diagnostic runner has a large monolithic dispatch frame. Keep its
    # host stack reserve explicit instead of depending on the 1 MiB PE default.
    target_link_options(ee_game_runner PRIVATE /STACK:8388608)
endif()

add_executable(ee_ram_interpreter_test tests/smoke/ee_ram_interpreter_test.cpp)
target_link_libraries(ee_ram_interpreter_test PRIVATE ee_core)

add_executable(ee_memory_alias_test tests/smoke/ee_memory_alias_test.cpp)
target_link_libraries(ee_memory_alias_test PRIVATE ee_core)

add_executable(ee_physical_bus_test tests/smoke/ee_physical_bus_test.cpp)
target_link_libraries(ee_physical_bus_test PRIVATE ee_core)

add_executable(ee_intc_test tests/smoke/ee_intc_test.cpp)
target_link_libraries(ee_intc_test PRIVATE ee_core)

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

add_executable(ee_gs_state_test tests/smoke/ee_gs_state_test.cpp)
target_link_libraries(ee_gs_state_test PRIVATE ee_core)

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
if(Python3_Interpreter_FOUND)
    add_test(NAME iop_module_path_discovery
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_iop_module_paths.py)
    add_test(NAME iop_disc_catalog
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_iop_disc_catalog.py)
    add_test(NAME boot_session_orchestration
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_boot_session.py)
    add_test(NAME iop_rom0_runner
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_iop_rom0_runner.py
                $<TARGET_FILE:iop_rom0_runner>)
    add_test(NAME recomp_discovery_cache
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_recomp_discovery_cache.py
                $<TARGET_FILE:ee_recomp>)
    if(EE_HAVE_LLVM_JIT_TEST)
        add_test(NAME verify_ir_incremental
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tests/tools/test_verify_ir_incremental.py)
    endif()
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
add_test(NAME iop_kernel COMMAND iop_kernel_test)
add_test(NAME iop_sifman_bridge COMMAND iop_sifman_bridge_test)
add_test(NAME iop_sif_dma COMMAND iop_sif_dma_test)
add_test(NAME iop_sif1_bridge COMMAND iop_sif1_bridge_test)
add_test(NAME iop_sifcmd_bridge COMMAND iop_sifcmd_bridge_test)
add_test(NAME iop_disc_loader COMMAND iop_disc_loader_test)
add_test(NAME iop_cdvd COMMAND iop_cdvd_test)
add_test(NAME iop_sif_rpc_bridge COMMAND iop_sif_rpc_bridge_test)
add_test(NAME decoder COMMAND ee_decoder_test)
add_test(NAME dispatcher COMMAND ee_dispatcher_test)
add_test(NAME ee_dispatch_interrupt COMMAND ee_dispatch_interrupt_test)
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
add_test(NAME ee_scratchpad COMMAND ee_scratchpad_test)
add_test(NAME ee_vu0_memory_map COMMAND ee_vu0_memory_map_test)
add_test(NAME ee_unaligned_memory COMMAND ee_unaligned_memory_test)
add_test(NAME vu0_runtime COMMAND ee_vu0_runtime_test)
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
    add_test(NAME compare_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_compare_ir_runtime.py
                $<TARGET_FILE:ee_compare_ir_fixture>)
    add_test(NAME object_emission_and_dispatch
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_object_emission.py
                $<TARGET_FILE:ee_compare_ir_fixture>
                ${CMAKE_SOURCE_DIR}/tools/verify_ir.py
                ${CMAKE_SOURCE_DIR}/tools/link_native_objects.py
                ${CMAKE_SOURCE_DIR})
    add_test(NAME game_runner_end_to_end
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/tools/test_game_runner.py
                ${CMAKE_SOURCE_DIR}
                ${CMAKE_BINARY_DIR}
                $<TARGET_FILE:ee_game_runner>)
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

    add_test(NAME control_flow_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_control_flow_ir_runtime.py
                $<TARGET_FILE:ee_control_flow_ir_fixture>)
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
