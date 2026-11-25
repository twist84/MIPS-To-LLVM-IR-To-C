#include "ee_ram_interpreter.h"
#include "ee_memory.h"
#include "ee_runtime.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace
{
bool check(bool ok, const char* message)
{
    if (!ok)
    {
        std::fprintf(stderr, "ee_ram_interpreter_test: %s\n", message);
    }
    return ok;
}

void write32(ee::ee_memory& memory, uint32_t address, uint32_t code)
{
    memory.write32(address, code);
}
}

int main()
{
    constexpr uint32_t k_ram_address = 0x80076000u;
    constexpr uint32_t k_table_address = 0x80076700u;
    constexpr uint32_t k_return_address = 0xfffffff0u;
    ee::ee_memory memory(2u * 1024u * 1024u);
    memory.set_kseg_ram_aliases(true);

    // A synthetic RAM routine using real MIPS branch and JR delay slots:
    // return a table value when a0 equals the table key, else return 0.
    // Delay slot on the BNE increments t0 in both paths; JR's delay slot
    // performs the final LW on the matching path.
    write32(memory, k_ram_address + 0x00u, 0x3c098007u); // lui t1, 0x8007
    write32(memory, k_ram_address + 0x04u, 0x25296700u); // addiu t1, t1, 0x6700
    write32(memory, k_ram_address + 0x08u, 0x8d2a0000u); // lw t2, 0(t1)
    write32(memory, k_ram_address + 0x0cu, 0x148a0003u); // bne a0,t2,+3
    write32(memory, k_ram_address + 0x10u, 0x25080001u); // addiu t0,t0,1
    write32(memory, k_ram_address + 0x14u, 0x03e00008u); // jr ra
    write32(memory, k_ram_address + 0x18u, 0x8d220004u); // lw v0,4(t1)
    write32(memory, k_ram_address + 0x1cu, 0x03e00008u); // jr ra
    write32(memory, k_ram_address + 0x20u, 0x0000102du); // daddu v0,zero,zero
    write32(memory, k_table_address, 0xfcu);
    write32(memory, k_table_address + 4u, 0x80076440u);

    ee::s_ee_cpu_state cpu{};
    ee::s_ee_runtime runtime{};
    ee::s_ee_instruction_trace_entry issue_stream[64]{};
    runtime.m_instruction_trace = issue_stream;
    runtime.m_instruction_trace_capacity = 64u;
    cpu.m_gpr[4].m_lo = 0xfcu;
    cpu.m_gpr[31].m_lo = static_cast<uint64_t>(-16ll);
    const ee::s_ee_ram_execution found = ee::ee_execute_ram_callback(
        &cpu, &memory, k_ram_address, k_return_address, 50u, &runtime
    );
    if (!check(found.m_result == ee::e_ee_ram_result::RETURNED,
               "matching key must return") ||
        !check(cpu.m_gpr[2].m_lo == 0xffffffff80076440ull,
               "JR delay slot must load and sign-extend the table pointer") ||
        !check(cpu.m_gpr[8].m_lo == 1u,
               "BNE delay slot must execute even when branch not taken") ||
        !check(found.m_instructions == 7u, "match instruction count") ||
        !check(runtime.m_guest_instruction_attempts == 7u &&
               runtime.m_reference_instruction_slots == 7u,
               "interpreter retirement is included in diagnostics") ||
        !check(runtime.m_instruction_trace_count == 7u &&
               issue_stream[0].m_pc == k_ram_address &&
               issue_stream[0].m_opcode_word == 0x3c098007u &&
               issue_stream[0].m_kind == 2u &&
               issue_stream[0].m_reference_slot == 1u &&
               issue_stream[6].m_reference_slot == 7u,
               "interpreter observes ordered committed opcodes, not clocks") ||
        !check(cpu.m_pc == k_return_address, "return sentinel PC"))
    {
        return 1;
    }

    cpu = {};
    cpu.m_gpr[4].m_lo = 0xfeu;
    cpu.m_gpr[31].m_lo = k_return_address;
    const ee::s_ee_ram_execution not_found = ee::ee_execute_ram_callback(
        &cpu, &memory, k_ram_address, k_return_address, 50u, &runtime
    );
    if (!check(not_found.m_result == ee::e_ee_ram_result::RETURNED,
               "missing key must return") ||
        !check(cpu.m_gpr[2].m_lo == 0u,
               "missing key must return zero") ||
        !check(cpu.m_gpr[8].m_lo == 1u,
               "BNE taken branch must still execute delay slot"))
    {
        return 1;
    }

    // Stop on unknown code; never silently replace it with a NOP.
    write32(memory, k_ram_address, 0x70000000u); // SPECIAL2, unsupported
    cpu = {};
    const ee::s_ee_ram_execution unknown = ee::ee_execute_ram_callback(
        &cpu, &memory, k_ram_address, k_return_address, 50u, &runtime
    );
    if (!check(unknown.m_result == ee::e_ee_ram_result::UNSUPPORTED,
               "unknown instruction must fail closed") ||
        !check(unknown.m_pc == k_ram_address, "unsupported PC retained") ||
        !check(unknown.m_opcode == 0x70000000u, "unsupported opcode retained") ||
        !check(runtime.m_guest_instruction_attempts ==
                   found.m_instructions + not_found.m_instructions,
               "unsupported instructions never retire") ||
        !check(runtime.m_instruction_trace_count ==
                   found.m_instructions + not_found.m_instructions,
               "unsupported instructions never enter issue stream"))
    {
        return 1;
    }

    // Two-instruction infinite branch, including its delay slot; no hang.
    write32(memory, k_ram_address, 0x1000ffffu); // beq zero,zero,-1
    write32(memory, k_ram_address + 4u, 0x25080001u);
    cpu = {};
    const ee::s_ee_ram_execution bounded = ee::ee_execute_ram_callback(
        &cpu, &memory, k_ram_address, k_return_address, 20u, &runtime
    );
    if (!check(bounded.m_result == ee::e_ee_ram_result::INSTRUCTION_BUDGET,
               "infinite branch must consume bounded budget") ||
        !check(bounded.m_instructions == 20u, "bounded instruction count") ||
        !check(cpu.m_gpr[8].m_lo == 10u, "loop delay slot executes each iteration") ||
        !check(runtime.m_guest_instruction_attempts ==
                   found.m_instructions + not_found.m_instructions +
                   bounded.m_instructions,
               "bounded loop instructions retire exactly once") ||
        !check(runtime.m_instruction_trace_count ==
                   runtime.m_guest_instruction_attempts &&
               issue_stream[runtime.m_instruction_trace_count - 1u].m_reference_slot ==
                   runtime.m_reference_instruction_slots,
               "interpreter preserves global trace ordinals across calls"))
    {
        return 1;
    }

    // Unsupported control transfer in a branch delay slot fails closed.
    write32(memory, k_ram_address + 4u, 0x03e00008u);
    cpu = {};
    const ee::s_ee_ram_execution nested = ee::ee_execute_ram_callback(
        &cpu, &memory, k_ram_address, k_return_address, 20u, &runtime
    );
    if (!check(nested.m_result == ee::e_ee_ram_result::BRANCH_IN_DELAY_SLOT,
               "branch in delay slot must not be executed as normal flow"))
    {
        return 1;
    }
    if (!check(runtime.m_instruction_trace_count ==
                   runtime.m_guest_instruction_attempts,
               "rejected branch-in-delay-slot does not enter issue stream"))
    {
        return 1;
    }

    // Trace window and full-buffer indicators are diagnostic only.
    runtime.m_instruction_trace_count = 0u;
    runtime.m_instruction_trace_capacity = 1u;
    runtime.m_instruction_trace_first_pc = k_ram_address;
    runtime.m_instruction_trace_last_pc = k_ram_address + 4u;
    write32(memory, k_ram_address + 4u, 0x25080001u);
    cpu = {};
    const ee::s_ee_ram_execution filtered = ee::ee_execute_ram_callback(
        &cpu, &memory, k_ram_address, k_return_address, 3u, &runtime);
    if (!check(filtered.m_instructions == 3u &&
               runtime.m_instruction_trace_count == 1u &&
               runtime.m_instruction_trace_overflow == 1u &&
               issue_stream[0].m_pc == k_ram_address &&
               issue_stream[0].m_kind == 2u,
               "windowed RAM trace handles capacity without changing execution"))
    {
        return 1;
    }

    // Timing-producer rejection in interpreted RAM must stop at the
    // first committed instruction; no arbitrary interpreter budget burn.
    write32(memory, k_ram_address, 0x3c098007u);
    int rejected_calls = 0;
    const auto reject_observation = [](
        void* user, uint32_t, uint32_t, uint32_t, uint32_t,
        uint64_t, uint64_t) -> bool
    {
        ++*static_cast<int*>(user);
        return false;
    };
    runtime.m_timing_observer = reject_observation;
    runtime.m_timing_observer_user = &rejected_calls;
    runtime.m_timing_observer_failed = 0u;
    cpu = {};
    const ee::s_ee_ram_execution rejected = ee::ee_execute_ram_callback(
        &cpu, &memory, k_ram_address, k_return_address, 20u, &runtime
    );
    if (!check(rejected.m_result ==
                   ee::e_ee_ram_result::TIMING_OBSERVER_REJECTED &&
               rejected.m_instructions == 1u &&
               rejected.m_pc == k_ram_address &&
               rejected_calls == 1 &&
               runtime.m_timing_observer_failed == 1u &&
               runtime.m_ee_cycle_scheduler.source() ==
                   ee::e_ee_cycle_source::UNAVAILABLE,
               "interpreter rejects unknown timing at first committed step"))
    {
        return 1;
    }

    // Original PS2SDK relocated syscall uses BNEL. Cover both
    // outcomes for all six non-link likely branches, checking annul
    // accounting, pre-delay condition and unsupported slot suppression.
    const uint32_t likely_opcodes[] =
    {
        0x50800002u, // BEQL a0,zero,+2
        0x54800002u, // BNEL a0,zero,+2
        0x58800002u, // BLEZL a0,+2
        0x5c800002u, // BGTZL a0,+2
        0x04820002u, // BLTZL a0,+2
        0x04830002u  // BGEZL a0,+2
    };
    const int64_t taken_inputs[] = {0, 1, -1, 1, -1, 0};
    const int64_t annul_inputs[] = {1, 0, 1, 0, 0, -1};
    for (uint32_t i = 0u; i < 6u; ++i)
    {
        for (bool taken : {false, true})
        {
            write32(memory, k_ram_address, likely_opcodes[i]);
            write32(memory, k_ram_address + 4u,
                taken ? 0x25080001u : 0x70000000u);
            write32(memory, k_ram_address + 8u, 0x24090001u);
            write32(memory, k_ram_address + 12u, 0x03e00008u);
            write32(memory, k_ram_address + 16u, 0x00000000u);
            cpu = {};
            cpu.m_gpr[4].m_lo = static_cast<uint64_t>(
                taken ? taken_inputs[i] : annul_inputs[i]);
            cpu.m_gpr[31].m_lo = k_return_address;
            ee::s_ee_runtime likely_runtime{};
            likely_runtime.m_instruction_trace = issue_stream;
            likely_runtime.m_instruction_trace_capacity = 64u;
            const auto execution = ee::ee_execute_ram_callback(
                &cpu, &memory, k_ram_address, k_return_address, 4u, &likely_runtime);
            if (!check(execution.m_result == ee::e_ee_ram_result::RETURNED &&
                       execution.m_instructions == 4u &&
                       cpu.m_gpr[8].m_lo == (taken ? 1u : 0u) &&
                       cpu.m_gpr[9].m_lo == (taken ? 0u : 1u),
                       "branch-likely architectural path") ||
                !check(likely_runtime.m_guest_instruction_attempts == 4u &&
                       likely_runtime.m_reference_instruction_slots == (taken ? 4u : 5u) &&
                       likely_runtime.m_instruction_trace_count == (taken ? 4u : 5u) &&
                       issue_stream[1].m_pc == k_ram_address + 4u &&
                       issue_stream[1].m_kind == (taken ? 2u : 1u) &&
                       issue_stream[1].m_attempt == (taken ? 2u : 1u),
                       "branch-likely annulled reference slot"))
            {
                return 1;
            }
        }
    }

    std::puts("ee_ram_interpreter_test: PASS");
    return 0;
}
