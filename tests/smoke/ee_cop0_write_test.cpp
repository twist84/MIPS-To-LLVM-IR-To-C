#include "ee_exceptions.h"
#include "native_codegen.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
int s_failures = 0;

void check(bool condition, const char* description)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", description);
        ++s_failures;
    }
}
}

int main()
{
    ee::s_ee_cpu_state cpu{};
    constexpr uint32_t k_timer_ip =
        1u << (ee::CAUSE_IP_SHIFT + ee::INT_TIMER);
    constexpr uint32_t k_intc_ip =
        1u << (ee::CAUSE_IP_SHIFT + ee::INT_INTC);
    constexpr uint32_t k_other_cause = k_intc_ip | 0x24u;
    constexpr uint32_t k_timer_enabled =
        ee::STATUS_IE | ee::STATUS_EIE | k_timer_ip;

    cpu.m_cop0[ee::COP0_Count] = 4u;
    cpu.m_cop0[ee::COP0_Compare] = 6u;
    cpu.m_cop0[ee::COP0_Status] = k_timer_enabled;
    cpu.m_cop0[ee::COP0_Cause] = k_other_cause | k_timer_ip;

    check(ee::ee_interrupts_enabled(&cpu),
          "timer pending and enabled before Compare write");

    ee::ee_cop0_write(&cpu, ee::COP0_Compare, 10u);
    check(cpu.m_cop0[ee::COP0_Compare] == 10u &&
          cpu.m_cop0[ee::COP0_Count] == 4u,
          "Compare write stores the value without advancing Count");
    check(cpu.m_cop0[ee::COP0_Cause] == k_other_cause &&
          !ee::ee_interrupts_enabled(&cpu),
          "Compare write clears only Cause.IP7 and masks timer delivery");

    ee::ee_cop0_write(&cpu, ee::COP0_Compare, 10u);
    check(cpu.m_cop0[ee::COP0_Cause] == k_other_cause,
          "writing the same Compare value does not fabricate an interrupt");

    ee::ee_cop0_write(&cpu, ee::COP0_Count, 7u);
    check(cpu.m_cop0[ee::COP0_Count] == 7u &&
          cpu.m_cop0[ee::COP0_Cause] == k_other_cause,
          "Count write preserves other pending interrupt lines");

    ee::ee_cop0_write(&cpu, ee::COP0_Status, 0x00010001u);
    check(cpu.m_cop0[ee::COP0_Status] == 0x00010001u &&
          cpu.m_cop0[ee::COP0_Cause] == k_other_cause,
          "non-Compare writes retain prior register semantics");

    // The existing explicit timer-tick helper is only a synthetic fixture:
    // no native execution cycle source or GS scheduling is installed.
    cpu.m_cop0[ee::COP0_Count] = 4u;
    cpu.m_cop0[ee::COP0_Compare] = 6u;
    cpu.m_cop0[ee::COP0_Cause] = k_other_cause;
    ee::ee_timer_tick(&cpu, 2u);
    check(cpu.m_cop0[ee::COP0_Count] == 6u &&
          (cpu.m_cop0[ee::COP0_Cause] & k_timer_ip) != 0u,
          "explicit timer-tick fixture can set IP7");

    ee::ee_cop0_write(&cpu, ee::COP0_Compare, 6u);
    check(cpu.m_cop0[ee::COP0_Cause] == k_other_cause &&
          cpu.m_cop0[ee::COP0_Count] == 6u,
          "Compare write acknowledges even when Count equals Compare");

    const ee::s_ee_cpu_state before_invalid = cpu;
    ee::ee_cop0_write(&cpu, 32u, 0xffffffffu);
    ee::ee_cop0_write(&cpu, 0xffffffffu, 0xffffffffu);
    ee::ee_cop0_write(nullptr, ee::COP0_Compare, 0u);
    check(cpu.m_cop0[ee::COP0_Compare] ==
              before_invalid.m_cop0[ee::COP0_Compare] &&
          cpu.m_cop0[ee::COP0_Cause] ==
              before_invalid.m_cop0[ee::COP0_Cause],
          "invalid indices and null CPU do not mutate architectural state");

    // The less frequently used C/C++ emitter must obey the same ABI as
    // generated LLVM, including the callable body's CPU pointer substitution.
    ee::s_lifted_op mtc0{};
    mtc0.kind = ee::LOP_MTC0;
    mtc0.rt = 8u;
    mtc0.rd = ee::COP0_Compare;
    mtc0.pc = 0x00100000u;
    ee::native_codegen generator;
    const std::string callable = generator.emit_function_body(
        std::vector<ee::s_lifted_op>{mtc0},
        mtc0.pc, "cop0_callable"
    );
    check(callable.find("ee_cop0_write(cpu, 11,") != std::string::npos &&
          callable.find("cpu->m_cop0[11] =") == std::string::npos,
          "native callable emitter calls the shared COP0 write helper");

    // Original console performance register semantics, including all 32
    // alias indices, with no dependence on host or diagnostic timing.
    ee::s_ee_runtime runtime{};
    const auto transfer = [&](uint32_t fmt, uint32_t gpr, uint32_t selector)
    {
        return ee::ee_cop0_perf_transfer(&cpu, &runtime,
            (0x10u << 26u) | (fmt << 21u) | (gpr << 16u) |
            (25u << 11u) | selector);
    };
    cpu.m_gpr[4].m_lo = 0x89abcdefu;
    check(transfer(4u, 4u, 1u) == 0u, "MTPC0 full width");
    cpu.m_gpr[4].m_lo = 0x01234567u;
    check(transfer(4u, 4u, 3u) == 0u, "MTPC1 full width");
    cpu.m_gpr[5].m_hi = 0x1234abcdu;
    for (uint32_t i = 0; i < 32u; ++i)
    {
        check(transfer(0u, 5u, (i << 1u) | 1u) == 0u &&
              cpu.m_gpr[5].m_lo == ((i & 1u) ? 0x01234567ull : 0xffffffff89abcdefull) &&
              cpu.m_gpr[5].m_hi == 0x1234abcdu, "MFPC alias and sign extension");
    }
    cpu.m_gpr[4].m_lo = 0x7fffffffu;
    check(transfer(4u, 4u, 0u) == 0u, "MTPS disabled");
    for (uint32_t i = 0; i < 32u; ++i)
    {
        check(transfer(0u, 5u, i << 1u) == 0u &&
              cpu.m_gpr[5].m_lo == 0x000ffbfeu, "MFPS alias and mask");
    }
    check(transfer(4u, 0u, 30u) == 0u &&
          runtime.m_perf_control == 0x000ffbfeu, "nonzero MTPS ignored");
    check(transfer(0u, 0u, 1u) == 0u && cpu.m_gpr[0].m_lo == 0u,
          "MFPC zero destination");
    cpu.m_gpr[4].m_lo = 0x8000003eu;
    check(transfer(4u, 4u, 0u) == 4u &&
          runtime.m_perf_control == 0x000ffbfeu,
          "active cycle event without validated timing fails closed");
    check(runtime.m_ee_cycle_scheduler.install_reference_compatibility_source() &&
          transfer(4u, 0u, 1u) == 0u &&
          transfer(4u, 0u, 3u) == 0u &&
          transfer(4u, 4u, 0u) == 0u &&
          !ee::ee_cop0_perf_advance(&runtime, 0u, 100u),
          "compatibility source enables only its own approximate budget");
    cpu.m_cop0[ee::COP0_Status] = 0u;
    runtime.m_guest_instruction_attempts = 23u;
    check(transfer(0u, 5u, 1u) == 0u && cpu.m_gpr[5].m_lo == 23u &&
          runtime.m_perf_counter[1] == 0u &&
          transfer(0u, 5u, 1u) == 0u && cpu.m_gpr[5].m_lo == 23u,
          "elapsed compatibility budget is charged exactly once");
    runtime.m_cop0_clock_idle = 7u;
    ee::ee_cop0_clock_sync(&cpu, &runtime);
    check(runtime.m_perf_counter[0] == 30u &&
          transfer(4u, 0u, 0u) == 0u,
          "idle units use same clock and stop accounts old control");
    runtime.m_guest_instruction_attempts += 19u;
    check(transfer(0u, 5u, 1u) == 0u && cpu.m_gpr[5].m_lo == 30u,
          "disabled compatibility counter remains unchanged");
    runtime.m_ee_cycle_scheduler.reset();
    check(runtime.m_ee_cycle_scheduler.install_validated_source(), "explicit validated test source");
    check(transfer(4u, 0u, 1u) == 0u && transfer(4u, 0u, 3u) == 0u &&
          transfer(4u, 4u, 0u) == 0u, "enable processor-cycle counter 0");
    check(ee::ee_cop0_perf_advance(&runtime, 0u, 17u) &&
          runtime.m_perf_counter[0] == 17u && runtime.m_perf_counter[1] == 0u,
          "explicit cycle interval updates selected counter only");
    check(ee::ee_cop0_perf_advance(&runtime, ee::STATUS_ERL, 99u) &&
          runtime.m_perf_counter[0] == 17u, "ERL inhibits counting");
    check(!ee::ee_cop0_perf_advance(&runtime, 0u, 0x80000000ull) &&
          runtime.m_perf_counter[0] == 17u, "overflow fails transactionally");
    cpu.m_gpr[4].m_lo = 0x80008020u;
    check(transfer(4u, 4u, 0u) == 0u &&
          ee::ee_cop0_perf_advance(&runtime, 0u, 9u) &&
          runtime.m_perf_counter[0] == 17u && runtime.m_perf_counter[1] == 0u,
          "no mode enables means neither cycle counter counts");

    {
        ee::s_ee_cpu_state a{};
        ee::s_ee_cpu_state b{};
        ee::s_ee_runtime clock{};
        a.m_cop0[ee::COP0_Count] = 0xfffffffcu;
        a.m_cop0[ee::COP0_Compare] = 2u;
        ee::ee_cop0_clock_sync(&a, &clock);
        check(a.m_cop0[ee::COP0_Count] == 0xfffffffcu,
              "unavailable clock leaves Count unchanged");
        check(clock.m_ee_cycle_scheduler.install_reference_compatibility_source(),
              "explicit compatibility source");
        clock.m_guest_instruction_attempts = 6u;
        ee::ee_cop0_clock_sync(&a, &clock);
        check(a.m_cop0[ee::COP0_Count] == 2u &&
              (a.m_cop0[ee::COP0_Cause] & k_timer_ip) != 0u,
              "Count wraps and latches Compare at the elapsed budget");
        ee::ee_cop0_clock_write(&a, &clock, ee::COP0_Compare, 10u);
        clock.m_guest_instruction_attempts = 9u;
        clock.m_cop0_clock_idle = 2u;
        ee::ee_cop0_clock_sync(&b, &clock);
        check(b.m_cop0[ee::COP0_Count] == 7u &&
              b.m_cop0[ee::COP0_Compare] == 10u &&
              (b.m_cop0[ee::COP0_Cause] & k_timer_ip) == 0u,
              "thread context cannot restore stale timer state");
        ee::ee_cop0_clock_write(&b, &clock, ee::COP0_Count, 9u);
        clock.m_guest_instruction_attempts++;
        ee::ee_cop0_clock_sync(&a, &clock);
        check(a.m_cop0[ee::COP0_Count] == 10u &&
              (a.m_cop0[ee::COP0_Cause] & k_timer_ip) != 0u,
              "Count write reanchors without double counting");
    }
    std::printf("ee_cop0_write_test: %d failure(s)\n", s_failures);
    return s_failures != 0 ? 1 : 0;
}
