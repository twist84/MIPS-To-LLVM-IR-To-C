#include "ee_dispatcher.h"
#include "ee_exceptions.h"
#include "ee_hw_bus.h"
#include "ee_memory.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace
{
constexpr uint32_t k_entry = 0x00100000u;
constexpr uint32_t k_resume = 0x00100008u;
// Literal architectural addresses, independent of the production selector.
constexpr uint32_t k_interrupt_vector_bev0 = 0x80000200u;
constexpr uint32_t k_interrupt_vector_bev1 = 0xBFC00400u;
constexpr uint32_t k_dmac_ip = 1u << (ee::CAUSE_IP_SHIFT + ee::INT_DMAC);
constexpr uint32_t k_intc_ip = 1u << (ee::CAUSE_IP_SHIFT + ee::INT_INTC);

uint32_t g_function_calls = 0;
uint32_t g_resume_calls = 0;
uint32_t g_handler_calls = 0;

int fail(const char* message)
{
    std::fprintf(stderr, "EE dispatch interrupt boundary: %s\n", message);
    return 1;
}

ee::s_recompiled_fn make_function(
    const char* name, uint32_t pc, std::initializer_list<uint32_t> blocks)
{
    ee::s_recompiled_fn result{};
    result.name = name;
    result.vaddr = pc;
    result.block_pcs.assign(blocks.begin(), blocks.end());
    result.block_count = static_cast<uint32_t>(result.block_pcs.size());
    return result;
}

uint32_t trigger_dmac(ee::s_ee_cpu_state* cpu, ee::s_ee_runtime* runtime)
{
    ++g_function_calls;
    if (cpu->m_pc == k_entry)
    {
        // Simulate a completed transfer only after an explicit host event,
        // not as an effect of a fabricated channel-start register.
        ee::ee_mem_write32(runtime, ee::c_ee_dmac::k_stat_address, 1u << 17u);
        ee::c_ee_hw_bus* hardware =
            static_cast<ee::c_ee_hw_bus*>(runtime->m_mmio_user);
        if (!hardware->dmac().raise(1u))
        {
            return static_cast<uint32_t>(
                ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED
            );
        }
        cpu->m_pc = k_resume;
        return static_cast<uint32_t>(
            ee::e_ee_exit_reason::EE_EXIT_DISPATCH
        );
    }

    ++g_resume_calls;
    cpu->m_pc = 0xdeadcafeu;
    return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
}

uint32_t harmless_return(ee::s_ee_cpu_state* cpu, ee::s_ee_runtime*)
{
    ++g_function_calls;
    cpu->m_pc = 0xdeadbeefu;
    return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_RETURN);
}

uint32_t exception_return_entry(ee::s_ee_cpu_state* cpu, ee::s_ee_runtime*)
{
    ++g_handler_calls;
    ee::ee_exception_return(cpu);
    return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
}

uint32_t resumed_entry(ee::s_ee_cpu_state* cpu, ee::s_ee_runtime*)
{
    ++g_resume_calls;
    cpu->m_pc = 0xdeadbeefu;
    return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_RETURN);
}

bool interrupted(const ee::s_ee_dispatch_result& result,
                 const ee::s_ee_cpu_state& cpu, uint32_t vector,
                 uint32_t epc, uint32_t count)
{
    return result.status == ee::e_ee_dispatch_status::EXITED &&
        result.exit_reason == static_cast<uint32_t>(
            ee::e_ee_exit_reason::EE_EXIT_EXCEPTION) &&
        result.dispatch_count == count &&
        result.guest_pc == vector &&
        cpu.m_pc == vector &&
        cpu.m_cop0[ee::COP0_EPC] == epc &&
        (cpu.m_cop0[ee::COP0_Status] & ee::STATUS_EXL) != 0 &&
        (cpu.m_cop0[ee::COP0_Cause] & ee::CAUSE_BD) == 0 &&
        ((cpu.m_cop0[ee::COP0_Cause] >> 2) & 31u) == ee::EX_Int;
}
} // namespace

int main()
{
    // IRQ vectors differ from general exceptions and TLB refill.
    // A nested entry keeps the general-vector fallback.
    if (ee::ee_exception_vector(0u, ee::EX_Int) != k_interrupt_vector_bev0 ||
        ee::ee_exception_vector(ee::STATUS_BEV, ee::EX_Int) !=
            k_interrupt_vector_bev1 ||
        ee::ee_exception_vector(ee::STATUS_EXL, ee::EX_Int) != 0x80000180u ||
        ee::ee_exception_vector(ee::STATUS_EXL | ee::STATUS_BEV, ee::EX_Int) !=
            0xBFC00380u ||
        ee::ee_exception_vector(0u, ee::EX_Sys) != 0x80000180u ||
        ee::ee_exception_vector(ee::STATUS_BEV, ee::EX_Sys) != 0xBFC00380u ||
        ee::ee_exception_vector(0u, ee::EX_TLBL) != 0x80000000u ||
        ee::ee_exception_vector(ee::STATUS_BEV, ee::EX_TLBS) != 0xBFC00200u ||
        ee::ee_exception_vector(ee::STATUS_EXL, ee::EX_TLBL) != 0x80000180u)
    {
        return fail("architectural interrupt/general/refill vector selection");
    }

    // A DMA completion raised by guest execution becomes visible precisely
    // before the NEXT translated function entry, after the old exit.
    {
        ee::s_recompiled_elf compiled{};
        compiled.functions.push_back(
            make_function("trigger", k_entry, {k_entry, k_resume}));
        const ee::t_ee_translated_entry entries[] = {trigger_dmac};
        const ee::s_ee_dispatch_table table{&compiled, entries, 1};

        ee::s_ee_cpu_state cpu{};
        ee::ee_memory memory(4096);
        ee::c_ee_hw_bus hardware;
        hardware.bind_cpu(&cpu);
        ee::s_ee_runtime runtime{};
        runtime.m_user = &memory;
        runtime.m_mmio_user = &hardware;
        runtime.m_mmio_read = &ee::c_ee_hw_bus::mmio_read;
        runtime.m_mmio_write = &ee::c_ee_hw_bus::mmio_write;

        cpu.m_pc = k_entry;
        cpu.m_cop0[ee::COP0_Status] =
            ee::STATUS_IE | ee::STATUS_EIE | k_dmac_ip;
        g_function_calls = 0;
        g_resume_calls = 0;
        const ee::s_ee_dispatch_result result =
            ee::ee_dispatch_run(&cpu, &runtime, table, 8);

        if (!interrupted(result, cpu, k_interrupt_vector_bev0, k_resume, 1) ||
            g_function_calls != 1 || g_resume_calls != 0 ||
            (cpu.m_cop0[ee::COP0_Cause] & k_dmac_ip) == 0)
        {
            return fail("DMA event was not taken before continuation entry");
        }
    }

    // Pending INTC before the first guest instruction must not execute the
    // mapped function. With BEV=1 the boot exception vector must be chosen.
    {
        ee::s_recompiled_elf compiled{};
        compiled.functions.push_back(
            make_function("entry", k_entry, {k_entry}));
        const ee::t_ee_translated_entry entries[] = {harmless_return};
        const ee::s_ee_dispatch_table table{&compiled, entries, 1};

        ee::s_ee_cpu_state cpu{};
        ee::ee_memory memory(4096);
        ee::c_ee_hw_bus hardware;
        hardware.bind_cpu(&cpu);
        ee::s_ee_runtime runtime{};
        runtime.m_user = &memory;
        runtime.m_mmio_user = &hardware;
        runtime.m_mmio_read = &ee::c_ee_hw_bus::mmio_read;
        runtime.m_mmio_write = &ee::c_ee_hw_bus::mmio_write;

        cpu.m_pc = k_entry;
        cpu.m_cop0[ee::COP0_Status] =
            ee::STATUS_IE | ee::STATUS_EIE | ee::STATUS_BEV | k_intc_ip;
        hardware.intc().raise(3);
        ee::ee_mem_write32(&runtime, ee::c_ee_intc::k_mask_address, 1u << 3);

        g_function_calls = 0;
        const ee::s_ee_dispatch_result result =
            ee::ee_dispatch_run(&cpu, &runtime, table, 8);
        if (!interrupted(result, cpu, k_interrupt_vector_bev1, k_entry, 0) ||
            g_function_calls != 0 ||
            (cpu.m_cop0[ee::COP0_Cause] & k_intc_ip) == 0)
        {
            return fail("pre-dispatch INTC exception vector or EPC incorrect");
        }
    }

    // IE/EIE/IM masking and EXL/ERL suppression must still apply; the
    // dispatcher may not take an IRQ just because a hardware line is high.
    {
        const uint32_t statuses[] = {
            ee::STATUS_IE | ee::STATUS_EIE,                  // IM3 disabled
            ee::STATUS_EIE | k_dmac_ip,                     // IE disabled
            ee::STATUS_IE | k_dmac_ip,                      // EIE disabled
            ee::STATUS_IE | ee::STATUS_EIE | k_dmac_ip |
                ee::STATUS_EXL,
            ee::STATUS_IE | ee::STATUS_EIE | k_dmac_ip |
                ee::STATUS_ERL
        };

        ee::s_recompiled_elf compiled{};
        compiled.functions.push_back(
            make_function("return", k_entry, {k_entry}));
        const ee::t_ee_translated_entry entries[] = {harmless_return};
        const ee::s_ee_dispatch_table table{&compiled, entries, 1};

        for (uint32_t status : statuses)
        {
            ee::s_ee_cpu_state cpu{};
            ee::c_ee_hw_bus hardware;
            hardware.bind_cpu(&cpu);
            cpu.m_pc = k_entry;
            cpu.m_cop0[ee::COP0_Status] = status;
            hardware.dmac().raise(1);
            ee::s_ee_u128 mask{1u << 17u, 0};
            ee::c_ee_dmac::mmio_write(
                &hardware.dmac(), ee::c_ee_dmac::k_stat_address, 4, &mask);

            g_function_calls = 0;
            const ee::s_ee_dispatch_result result =
                ee::ee_dispatch_run(&cpu, nullptr, table, 4);
            if (g_function_calls != 1 ||
                result.dispatch_count != 1 ||
                result.exit_reason != static_cast<uint32_t>(
                    ee::e_ee_exit_reason::EE_EXIT_RETURN) ||
                cpu.m_pc != 0xdeadbeefu)
            {
                return fail("masked/disabled exception interrupted guest code");
            }
        }
    }

    // R5900 ERET selects ErrorEPC and clears ERL in error level;
    // otherwise it selects EPC and clears EXL. Error level takes
    // precedence when both flags are present.
    {
        struct s_eret_case
        {
            uint32_t m_initial_status;
            uint32_t m_expected_status;
            uint32_t m_expected_pc;
        };
        const s_eret_case cases[] = {
            {
                ee::STATUS_ERL | ee::STATUS_EXL | ee::STATUS_IE,
                ee::STATUS_EXL | ee::STATUS_IE, 0x00102000u
            },
            {
                ee::STATUS_ERL | ee::STATUS_IE,
                ee::STATUS_IE, 0x00102000u
            },
            {
                ee::STATUS_EXL | ee::STATUS_IE,
                ee::STATUS_IE, 0x00101000u
            },
            {
                ee::STATUS_IE,
                ee::STATUS_IE, 0x00101000u
            }
        };

        for (const s_eret_case& test : cases)
        {
            ee::s_ee_cpu_state cpu{};
            cpu.m_pc = 0x80000180u;
            cpu.m_cop0[ee::COP0_Status] = test.m_initial_status;
            cpu.m_cop0[ee::COP0_EPC] = 0x00101000u;
            cpu.m_cop0[ee::COP0_ErrorEPC] = 0x00102000u;
            const uint32_t returned = ee::ee_exception_return(&cpu);
            if (returned != test.m_expected_pc ||
                cpu.m_pc != test.m_expected_pc ||
                cpu.m_cop0[ee::COP0_Status] != test.m_expected_status ||
                cpu.m_cop0[ee::COP0_EPC] != 0x00101000u ||
                cpu.m_cop0[ee::COP0_ErrorEPC] != 0x00102000u ||
                cpu.m_exit_reason != static_cast<uint32_t>(
                    ee::e_ee_exit_reason::EE_EXIT_DISPATCH))
            {
                return fail("ERET ErrorEPC/EPC status selection incorrect");
            }
        }
    }

    // ERET clears EXL. A still-pending level interrupt must be sampled
    // immediately at the next translated boundary, before EPC executes.
    {
        ee::s_recompiled_elf compiled{};
        compiled.functions.push_back(
            make_function("resumed", k_entry, {k_entry}));
        compiled.functions.push_back(make_function(
            "handler", k_interrupt_vector_bev0, {k_interrupt_vector_bev0}));
        const ee::t_ee_translated_entry entries[] = {
            resumed_entry, exception_return_entry
        };
        const ee::s_ee_dispatch_table table{&compiled, entries, 2};

        ee::s_ee_cpu_state cpu{};
        ee::c_ee_hw_bus hardware;
        hardware.bind_cpu(&cpu);
        cpu.m_pc = k_interrupt_vector_bev0;
        cpu.m_cop0[ee::COP0_EPC] = k_entry;
        cpu.m_cop0[ee::COP0_Status] =
            ee::STATUS_IE | ee::STATUS_EIE |
            ee::STATUS_EXL | k_dmac_ip;
        hardware.dmac().raise(1);
        ee::s_ee_u128 mask{1u << 17u, 0};
        ee::c_ee_dmac::mmio_write(
            &hardware.dmac(), ee::c_ee_dmac::k_stat_address, 4, &mask);

        g_handler_calls = 0;
        g_resume_calls = 0;
        const ee::s_ee_dispatch_result result =
            ee::ee_dispatch_run(&cpu, nullptr, table, 4);
        if (!interrupted(result, cpu, k_interrupt_vector_bev0, k_entry, 1) ||
            g_handler_calls != 1 || g_resume_calls != 0)
        {
            return fail("ERET failed to resample still-pending interrupt");
        }
    }

    // The dispatch budget is a host policy and may stop just before
    // an interrupt boundary; a subsequent invocation must still deliver it.
    {
        ee::s_recompiled_elf compiled{};
        compiled.functions.push_back(
            make_function("trigger", k_entry, {k_entry, k_resume}));
        const ee::t_ee_translated_entry entries[] = {trigger_dmac};
        const ee::s_ee_dispatch_table table{&compiled, entries, 1};

        ee::s_ee_cpu_state cpu{};
        ee::ee_memory memory(4096);
        ee::c_ee_hw_bus hardware;
        hardware.bind_cpu(&cpu);
        ee::s_ee_runtime runtime{};
        runtime.m_user = &memory;
        runtime.m_mmio_user = &hardware;
        runtime.m_mmio_read = &ee::c_ee_hw_bus::mmio_read;
        runtime.m_mmio_write = &ee::c_ee_hw_bus::mmio_write;
        cpu.m_pc = k_entry;
        cpu.m_cop0[ee::COP0_Status] =
            ee::STATUS_IE | ee::STATUS_EIE | k_dmac_ip;
        g_function_calls = 0;
        g_resume_calls = 0;

        const ee::s_ee_dispatch_result first =
            ee::ee_dispatch_run(&cpu, &runtime, table, 1);
        if (first.status != ee::e_ee_dispatch_status::DISPATCH_LIMIT ||
            first.dispatch_count != 1 ||
            cpu.m_pc != k_resume || g_function_calls != 1)
        {
            return fail("dispatch budget did not stop after first entry");
        }

        const ee::s_ee_dispatch_result second =
            ee::ee_dispatch_run(&cpu, &runtime, table, 4);
        if (!interrupted(second, cpu, k_interrupt_vector_bev0, k_resume, 0) ||
            g_function_calls != 1)
        {
            return fail("interrupt lost across bounded dispatcher calls");
        }
    }

    std::puts("EE interrupt checkpoints at translated function boundaries: PASS");
    return 0;
}
