#include <cmath>

// UCRT's corecrt_math.h defines OVERFLOW on some Windows configurations.
// Reproduce the macro before the runtime header's transitive include on
// every host, without altering an existing platform macro.
#ifndef OVERFLOW
#define OVERFLOW 3
#define EE_TEST_DEFINED_OVERFLOW
#endif
#include "ee_exceptions.h"
#include "ee_perf_events.h"
#ifndef OVERFLOW
#error "Performance event headers must not remove platform macros"
#endif
#ifdef EE_TEST_DEFINED_OVERFLOW
#undef OVERFLOW
#undef EE_TEST_DEFINED_OVERFLOW
#endif
#include "native_codegen.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

namespace
{
constexpr bool test_perf_crossing_projection()
{
    using event = ee::e_ee_perf_event;
    using result = ee::e_ee_perf_interval_result;
    using order = ee::e_ee_perf_overflow_order;
    using boundary = ee::e_ee_perf_boundary_result;
    constexpr uint32_t both_cycles = 0x80009024u;
    constexpr uint32_t both_issue = 0x80011044u;
    uint32_t counters[2] = {0x7ffffffdu, 0x7fffffffu};
    ee::s_ee_perf_event_interval sample{};
    if (sample.set(event::RESERVED, 1u) ||
        !sample.set(event::PROCESSOR_CYCLE, 1u))
    {
        return false;
    }
    const auto exact = ee::ee_perf_project_crossing_prefix(
        both_cycles, 0u, sample, counters);
    if (exact.m_result != boundary::EXACT_CROSSING ||
        exact.m_crossing_mask != 2u ||
        exact.m_next[0] != 0x7ffffffeu ||
        exact.m_next[1] != 0x80000000u ||
        ee::ee_perf_apply_interval(both_cycles, 0u, sample, counters) !=
            result::COUNTER_OVERFLOW ||
        counters[0] != 0x7ffffffdu || counters[1] != 0x7fffffffu)
    {
        return false;
    }
    sample.set(event::PROCESSOR_CYCLE, 3u);
    const auto interval = ee::ee_perf_plan_interval(
        both_cycles, 0u, sample, counters);
    if (ee::ee_perf_classify_overflow_order(interval) != order::PCR1_FIRST ||
        ee::ee_perf_project_crossing_prefix(both_cycles, 0u, sample, counters)
            .m_result != boundary::OVERSHOT_BOUNDARY)
    {
        return false;
    }
    counters[1] = counters[0];
    const auto tied = ee::ee_perf_project_crossing_prefix(
        both_cycles, 0u, sample, counters);
    if (tied.m_result != boundary::EXACT_CROSSING ||
        tied.m_crossing_mask != 3u ||
        tied.m_next[0] != 0x80000000u || tied.m_next[1] != 0x80000000u ||
        ee::ee_perf_classify_overflow_order(
            ee::ee_perf_plan_interval(both_cycles, 0u, sample, counters)) !=
                order::SAME_EVENT_ORDINAL)
    {
        return false;
    }
    sample = {};
    sample.set(event::SINGLE_ISSUE, 3u);
    if (ee::ee_perf_project_crossing_prefix(both_issue, 0u, sample, counters)
            .m_result != boundary::INCOMPLETE_EVIDENCE)
    {
        return false;
    }
    sample.set(event::DUAL_ISSUE, 3u);
    if (ee::ee_perf_project_crossing_prefix(both_issue, 0u, sample, counters)
            .m_result != boundary::AMBIGUOUS_ORDER ||
        ee::ee_perf_classify_overflow_order(
            ee::ee_perf_plan_interval(both_issue, 0u, sample, counters)) !=
                order::INDETERMINATE ||
        ee::ee_perf_project_crossing_prefix(both_issue, 4u, sample, counters)
            .m_result != boundary::NO_CROSSING)
    {
        return false;
    }
    counters[0] = 0x80000000u;
    return ee::ee_perf_project_crossing_prefix(both_issue, 0u, sample, counters)
        .m_result == boundary::EXISTING_OVERFLOW;
}

static_assert(test_perf_crossing_projection());

constexpr bool test_perf_restart_boundary()
{
    using kind = ee::e_ee_perf_restart_kind;
    using result = ee::e_ee_perf_entry_result;
    using recognition = ee::e_ee_perf_recognition;
    constexpr uint32_t control = 0x80009024u;
    constexpr uint32_t status = 0x00800002u;
    constexpr uint32_t cause = 0x80008424u;
    constexpr uint32_t error_epc = 0x12345678u;
    constexpr uint32_t pending[2] = {0x80000000u, 0x80000000u};
    constexpr uint32_t clear[2] = {0u, 0u};
    const auto plan_for = [&](const ee::s_ee_perf_restart_boundary& boundary)
    {
        return ee::ee_perf_plan_exception_at_boundary(
            control, status, cause, error_epc, pending, false, false, boundary);
    };
    const auto unchanged = [&](const ee::s_ee_perf_entry_plan& plan)
    {
        return plan.m_entry.m_status == status &&
               plan.m_entry.m_cause == cause &&
               plan.m_entry.m_error_epc == error_epc &&
               plan.m_entry.m_vector == 0u;
    };
    const auto unavailable = plan_for({});
    const auto not_restored = plan_for({kind::ORDINARY, 0x1000u, 0u, false});
    if (unavailable.m_result != result::UNAVAILABLE_BOUNDARY ||
        unavailable.m_recognition.m_overflow_mask != 3u ||
        not_restored.m_result != result::UNAVAILABLE_BOUNDARY ||
        !unchanged(unavailable) || !unchanged(not_restored))
    {
        return false;
    }
    constexpr ee::s_ee_perf_restart_boundary invalid[] =
    {
        {kind::ORDINARY, 0x1002u, 0u, true},
        {kind::ORDINARY, 0x1004u, 0x1000u, true},
        {kind::DELAY_SLOT, 0x1008u, 0x1000u, true},
        {kind::DELAY_SLOT, 0x1002u, 0xffeu, true},
        {static_cast<kind>(255u), 0x1000u, 0u, true}
    };
    for (const auto& boundary : invalid)
    {
        const auto plan = plan_for(boundary);
        if (plan.m_result != result::INCONSISTENT_BOUNDARY || !unchanged(plan))
        {
            return false;
        }
    }
    constexpr ee::s_ee_perf_restart_boundary ordinary =
        {kind::ORDINARY, 0x1000u, 0u, true};
    const auto direct = plan_for(ordinary);
    const auto delay = plan_for({kind::DELAY_SLOT, 0x1004u, 0x1000u, true});
    const auto wrap = plan_for({kind::DELAY_SLOT, 0u, 0xfffffffcu, true});
    if (direct.m_result != result::READY ||
        direct.m_entry.m_status != (status | 4u) ||
        direct.m_entry.m_cause != 0x80028424u ||
        direct.m_entry.m_error_epc != 0x1000u ||
        direct.m_entry.m_vector != 0xbfc00280u ||
        delay.m_result != result::READY ||
        delay.m_entry.m_cause != 0xc0028424u ||
        delay.m_entry.m_error_epc != 0x1000u ||
        wrap.m_result != result::READY ||
        wrap.m_entry.m_error_epc != 0xfffffffcu)
    {
        return false;
    }
    const auto no_pending = ee::ee_perf_plan_exception_at_boundary(
        control, status, cause, error_epc, clear, false, false, ordinary);
    const auto reset = ee::ee_perf_plan_exception_at_boundary(
        control, status, cause, error_epc, pending, true, false, ordinary);
    const auto nmi = ee::ee_perf_plan_exception_at_boundary(
        control, status, cause, error_epc, pending, false, true, ordinary);
    const auto disabled = ee::ee_perf_plan_exception_at_boundary(
        0u, status, cause, error_epc, pending, false, false, ordinary);
    const auto erl = ee::ee_perf_plan_exception_at_boundary(
        control, status | 4u, cause, error_epc, pending, false, false, ordinary);
    return no_pending.m_result == result::NOT_ELIGIBLE &&
           no_pending.m_recognition.m_result == recognition::NO_PENDING_OVERFLOW &&
           reset.m_result == result::NOT_ELIGIBLE &&
           reset.m_recognition.m_result == recognition::SUPERSEDED_RESET &&
           nmi.m_result == result::NOT_ELIGIBLE &&
           nmi.m_recognition.m_result == recognition::DEFERRED_NMI &&
           disabled.m_result == result::NOT_ELIGIBLE &&
           disabled.m_recognition.m_result == recognition::COUNTING_DISABLED &&
           erl.m_result == result::NOT_ELIGIBLE &&
           erl.m_recognition.m_result == recognition::DEFERRED_ERL &&
           erl.m_entry.m_status == (status | 4u) &&
           erl.m_entry.m_error_epc == error_epc &&
           unchanged(no_pending) && unchanged(reset) && unchanged(nmi) &&
           unchanged(disabled);
}

static_assert(test_perf_restart_boundary());

constexpr bool test_perf_crossing_entry_composition()
{
    using event = ee::e_ee_perf_event;
    using result = ee::e_ee_perf_crossing_entry_result;
    using kind = ee::e_ee_perf_restart_kind;
    constexpr uint32_t control = 0x80009024u;
    constexpr uint32_t status = 0x00800000u;
    constexpr uint32_t cause = 0x80008424u;
    constexpr uint32_t error_epc = 0x12345678u;
    constexpr uint32_t counters[2] = {0x7ffffffdu, 0x7fffffffu};
    ee::s_ee_perf_crossing_request request{};
    request.m_control = control;
    request.m_status = status;
    request.m_before[0] = counters[0];
    request.m_before[1] = counters[1];
    request.m_prefix.set(event::PROCESSOR_CYCLE, 1u);
    request.m_boundary = {kind::DELAY_SLOT, 0x1004u, 0x1000u, true};

    const auto plan_for = [&](uint32_t live_control, uint32_t live_status)
    {
        return ee::ee_perf_plan_crossing_entry(
            request, live_control, live_status, cause, error_epc, counters);
    };
    const auto ready = plan_for(control, status);
    if (ready.m_result != result::READY ||
        ready.m_crossing.m_crossing_mask != 2u ||
        ready.m_crossing.m_next[0] != 0x7ffffffeu ||
        ready.m_crossing.m_next[1] != 0x80000000u ||
        ready.m_entry.m_recognition.m_overflow_mask != 2u ||
        ready.m_entry.m_entry.m_error_epc != 0x1000u ||
        ready.m_entry.m_entry.m_status != (status | 4u) ||
        ready.m_entry.m_entry.m_cause != 0xc0028424u ||
        ready.m_entry.m_entry.m_vector != 0xbfc00280u ||
        plan_for(control ^ 4u, status).m_result != result::STALE_SNAPSHOT ||
        plan_for(control, status ^ 2u).m_result != result::STALE_SNAPSHOT)
    {
        return false;
    }
    request.m_before[0] -= 1u;
    if (plan_for(control, status).m_result != result::STALE_SNAPSHOT)
    {
        return false;
    }
    request.m_before[0] = counters[0];
    request.m_boundary.m_architectural_state_restored = false;
    if (plan_for(control, status).m_result != result::ENTRY_REJECTED)
    {
        return false;
    }
    request.m_boundary.m_architectural_state_restored = true;
    request.m_nmi_pending = true;
    if (plan_for(control, status).m_result != result::ENTRY_REJECTED)
    {
        return false;
    }
    request.m_nmi_pending = false;
    request.m_prefix.set(event::PROCESSOR_CYCLE, 2u);
    if (plan_for(control, status).m_result != result::PREFIX_REJECTED)
    {
        return false;
    }
    request.m_prefix.set(event::PROCESSOR_CYCLE, 0u);
    return plan_for(control, status).m_result == result::PREFIX_REJECTED;
}

static_assert(test_perf_crossing_entry_composition());

constexpr bool test_perf_commit_boundary_gate()
{
    using result = ee::e_ee_perf_crossing_entry_result;
    using kind = ee::e_ee_perf_restart_kind;
    constexpr uint32_t control = 0x80009024u;
    constexpr uint32_t counters[2] = {0x7ffffffdu, 0x7fffffffu};
    constexpr uint32_t dispatch = static_cast<uint32_t>(
        ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
    ee::s_ee_perf_crossing_request request{};
    request.m_control = control;
    request.m_before[0] = counters[0];
    request.m_before[1] = counters[1];
    request.m_prefix.set(ee::e_ee_perf_event::PROCESSOR_CYCLE, 1u);
    request.m_boundary = {kind::DELAY_SLOT, 0x1004u, 0x1000u, true};
    const auto delay = ee::ee_perf_plan_crossing_entry(
        request, control, 0u, 0u, 0u, counters);
    if (delay.m_result != result::READY ||
        !ee::ee_perf_crossing_commit_boundary_matches(0x1000u, dispatch, delay) ||
        ee::ee_perf_crossing_commit_boundary_matches(0x1004u, dispatch, delay) ||
        ee::ee_perf_crossing_commit_boundary_matches(0x1000u, 1u, delay) ||
        ee::ee_perf_crossing_commit_boundary_matches(0x1000u, 7u, delay))
    {
        return false;
    }
    request.m_boundary = {kind::ORDINARY, 0x1004u, 0u, true};
    const auto ordinary = ee::ee_perf_plan_crossing_entry(
        request, control, 0u, 0u, 0u, counters);
    request.m_reset_pending = true;
    const auto rejected = ee::ee_perf_plan_crossing_entry(
        request, control, 0u, 0u, 0u, counters);
    return ee::ee_perf_crossing_commit_boundary_matches(0x1004u, dispatch, ordinary) &&
           !ee::ee_perf_crossing_commit_boundary_matches(0x1000u, dispatch, ordinary) &&
           !ee::ee_perf_crossing_commit_boundary_matches(0x1004u, dispatch, rejected);
}

static_assert(test_perf_commit_boundary_gate());

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

    // The R5900 Cause register is read-only and has no software
    // interrupt lines. Exercise all eight IP positions, with genuine
    // hardware events arriving through the host request/clear path.
    {
        ee::s_ee_cpu_state probe{};
        constexpr uint32_t saved_cause = 0xf0028c34u;
        probe.m_cop0[ee::COP0_Cause] = saved_cause;
        ee::ee_cop0_write(&probe, ee::COP0_Cause, 0u);
        check(probe.m_cop0[ee::COP0_Cause] == saved_cause,
              "guest Cause write cannot clear saved exception or pending lines");
        ee::ee_cop0_write(&probe, ee::COP0_Cause, 0xffffffffu);
        check(probe.m_cop0[ee::COP0_Cause] == saved_cause,
              "guest Cause write cannot manufacture exception or pending lines");
        for (uint32_t line = 0u; line < 8u; ++line)
        {
            const uint32_t bit = 1u << (8u + line);
            const bool implemented = line == 2u || line == 3u || line == 7u;
            probe.m_cop0[ee::COP0_Status] = ee::STATUS_IE | ee::STATUS_EIE | bit;
            probe.m_cop0[ee::COP0_Cause] = 0u;
            ee::ee_interrupt_request(&probe, line);
            check(probe.m_cop0[ee::COP0_Cause] == (implemented ? bit : 0u) &&
                  ee::ee_interrupts_enabled(&probe) == implemented &&
                  ee::ee_pending_interrupts(&probe) == (implemented ? bit : 0u),
                  "only real EE hardware lines may be requested");
            // A raw state image may contain legacy/reserved bits, but
            // they must not be eligible for an architectural interrupt.
            probe.m_cop0[ee::COP0_Cause] = bit;
            check(ee::ee_interrupts_enabled(&probe) == implemented &&
                  ee::ee_pending_interrupts(&probe) == (implemented ? bit : 0u),
                  "reserved IP bits cannot dispatch an interrupt");
            ee::ee_interrupt_clear(&probe, line);
            check(probe.m_cop0[ee::COP0_Cause] == (implemented ? 0u : bit),
                  "clear operates only on implemented interrupt lines");
        }
        probe.m_cop0[ee::COP0_Cause] = saved_cause;
        ee::ee_interrupt_request(&probe, 32u);
        ee::ee_interrupt_clear(&probe, 0xffffffffu);
        check(probe.m_cop0[ee::COP0_Cause] == saved_cause,
              "out of range IRQ line is ignored");
    }

    {
        ee::s_ee_cpu_state probe{};
        probe.m_cop0[ee::COP0_Random] = 17u;
        probe.m_cop0[ee::COP0_BadVAddr] = 0x87654321u;
        probe.m_cop0[ee::COP0_PRId] = 0x00002e20u;
        for (uint32_t value : {0u, 0xffffffffu, 0x12345678u})
        {
            ee::ee_cop0_write(&probe, ee::COP0_Random, value);
            ee::ee_cop0_write(&probe, ee::COP0_BadVAddr, value);
            ee::ee_cop0_write(&probe, ee::COP0_PRId, value);
            check(probe.m_cop0[ee::COP0_Random] == 17u &&
                  probe.m_cop0[ee::COP0_BadVAddr] == 0x87654321u &&
                  probe.m_cop0[ee::COP0_PRId] == 0x00002e20u,
                  "guest writes preserve read-only Random, BadVAddr and PRId");
        }
        for (uint32_t wired = 0u; wired < 48u; ++wired)
        {
            probe.m_cop0[ee::COP0_Random] = wired;
            ee::ee_cop0_write(&probe, ee::COP0_Wired, wired);
            check(probe.m_cop0[ee::COP0_Wired] == wired &&
                  probe.m_cop0[ee::COP0_Random] == 47u,
                  "defined Wired writes reset Random to 47");
        }
        probe.m_cop0[ee::COP0_Random] = 17u;
        ee::ee_cop0_write(&probe, ee::COP0_Wired, 48u);
        check(probe.m_cop0[ee::COP0_Wired] == 48u &&
              probe.m_cop0[ee::COP0_Random] == 17u,
              "undefined Wired value retains legacy storage without invented reset");
        ee::ee_raise_exception(&probe, nullptr, ee::EX_AdEL, 0x1000u, 0u, 0x123u);
        check(probe.m_cop0[ee::COP0_BadVAddr] == 0x123u,
              "architectural fault still updates read-only BadVAddr");
    }

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
          "active cycle event without any clock fails closed");
    check(runtime.m_ee_cycle_scheduler.install_reference_compatibility_source() &&
          transfer(4u, 0u, 1u) == 0u &&
          transfer(4u, 0u, 3u) == 0u &&
          transfer(4u, 4u, 0u) == 0u &&
          !ee::ee_cop0_perf_advance(&runtime, 0u, 100u),
          "compatibility source enables only its own approximate budget");
    cpu.m_cop0[ee::COP0_Status] = 0u;
    runtime.m_guest_instruction_attempts = 20u;
    runtime.m_reference_instruction_slots = 23u;
    check(transfer(0u, 5u, 1u) == 0u && cpu.m_gpr[5].m_lo == 23u &&
          runtime.m_perf_counter[1] == 0u &&
          transfer(0u, 5u, 1u) == 0u && cpu.m_gpr[5].m_lo == 23u,
          "elapsed compatibility budget is charged exactly once");
    runtime.m_cop0_clock_idle = 7u;
    ee::ee_cop0_clock_sync(&cpu, &runtime);
    check(runtime.m_perf_counter[0] == 30u &&
          transfer(4u, 0u, 0u) == 0u,
          "idle units use same clock and stop accounts old control");
    runtime.m_reference_instruction_slots += 19u;
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

    // EE manual 7.1.1/7.2.2: the four mode enables are exclusive;
    // EXL overrides KSU and ERL inhibits counting completely.
    for (uint32_t mode = 0u; mode < 4u; ++mode)
    {
        const uint32_t enable = 1u << (mode + 1u);
        const uint32_t status_words[] =
        {
            ee::STATUS_EXL, 0u, 1u << 3u, 2u << 3u
        };
        cpu.m_gpr[4].m_lo = 0x80008020u | enable | (enable << 10u);
        check(transfer(4u, 0u, 1u) == 0u &&
              transfer(4u, 0u, 3u) == 0u &&
              transfer(4u, 4u, 0u) == 0u,
              "enable isolated performance privilege mode");
        for (uint32_t actual = 0u; actual < 4u; ++actual)
        {
            const uint32_t before = runtime.m_perf_counter[0];
            check(ee::ee_cop0_perf_advance(&runtime, status_words[actual], 7u) &&
                  runtime.m_perf_counter[0] == before + (mode == actual ? 7u : 0u) &&
                  runtime.m_perf_counter[1] == runtime.m_perf_counter[0],
                  "performance mode selectors are mutually exclusive");
        }
        const uint32_t before = runtime.m_perf_counter[0];
        check(ee::ee_cop0_perf_advance(&runtime, ee::STATUS_EXL | (2u << 3u), 3u) &&
              runtime.m_perf_counter[0] == before + (mode == 0u ? 3u : 0u),
              "EXL overrides saved user KSU");
        const uint32_t after = runtime.m_perf_counter[0];
        check(ee::ee_cop0_perf_advance(&runtime, ee::STATUS_ERL | ee::STATUS_EXL, 9u) &&
              runtime.m_perf_counter[0] == after,
              "ERL inhibits all performance modes");
    }
    // PCR1 event zero is real hardware branch issue, not the reserved
    // PCR0 selector. Until issue-slot events exist, reject it explicitly.
    const uint32_t old_control = runtime.m_perf_control;
    cpu.m_gpr[4].m_lo = 0x80001000u;
    check(transfer(4u, 4u, 0u) == 4u &&
          runtime.m_perf_control == old_control,
          "unmodeled PCR1 low-order branch issue fails closed");
    cpu.m_gpr[4].m_lo = 0x80000004u;
    check(transfer(4u, 4u, 0u) == 0u,
          "reserved PCR0 event zero remains accepted");

    // Exhaustive selectors for both PCRs, with and without global/mode
    // enables. Reserved encodings retain the existing compatibility
    // policy; do not confuse that policy with documented hardware behavior.
    // The validated-source fixture supplies only synthetic cycle deltas.
    for (uint32_t index = 0u; index < 2u; ++index)
    {
        const uint32_t shift = index * 10u;
        for (uint32_t event = 0u; event < 32u; ++event)
        {
            for (uint32_t enabled = 0u; enabled < 2u; ++enabled)
            {
                for (uint32_t modes = 0u; modes < 16u; ++modes)
                {
                    check(transfer(4u, 0u, 0u) == 0u,
                          "disable before selector fixture");
                    runtime.m_perf_counter[0] = 11u;
                    runtime.m_perf_counter[1] = 19u;
                    const uint32_t control =
                        (enabled != 0u ? 0x80000000u : 0u) |
                        (event << (5u + shift)) |
                        (modes << (1u + shift));
                    cpu.m_gpr[4].m_lo = control;
                    const bool active = enabled != 0u && modes != 0u;
                    const bool supported = !active || event == 1u ||
                        (index == 0u && event == 0u) || event >= 16u;
                    const uint32_t rc = transfer(4u, 4u, 0u);
                    check(rc == (supported ? 0u : 4u) &&
                          runtime.m_perf_control == (supported ? control : 0u),
                          "PCR selector acceptance and transactional PCCR write");
                    if (!supported)
                    {
                        continue;
                    }
                    // Each interval uses one explicit privilege mode.
                    const uint32_t mode_status[] =
                    {
                        ee::STATUS_EXL, 0u, 1u << 3u, 2u << 3u
                    };
                    for (uint32_t mode = 0u; mode < 4u; ++mode)
                    {
                        const uint32_t before[2] =
                        {
                            runtime.m_perf_counter[0], runtime.m_perf_counter[1]
                        };
                        const uint32_t delta = active && event == 1u &&
                            (modes & (1u << mode)) != 0u ? 5u : 0u;
                        check(ee::ee_cop0_perf_advance(&runtime, mode_status[mode], 5u) &&
                              runtime.m_perf_counter[index] == before[index] + delta &&
                              runtime.m_perf_counter[1u - index] == before[1u - index],
                              "only selected supported event and privilege mode counts");
                    }
                }
            }
        }
    }

    // A fault in the second counter must not commit the first counter's
    // increment. No synthetic Level 2 exception or interrupt is generated.
    cpu.m_gpr[4].m_lo = 0x80009024u; // Both PCRs: kernel processor cycles.
    check(transfer(4u, 4u, 0u) == 0u,
          "enable both processor-cycle counters");
    runtime.m_perf_counter[0] = 13u;
    runtime.m_perf_counter[1] = 0x7ffffffeu;
    const uint32_t before_cause = cpu.m_cop0[ee::COP0_Cause];
    check(ee::ee_cop0_perf_advance(&runtime, 0u, 1u) &&
          runtime.m_perf_counter[0] == 14u &&
          runtime.m_perf_counter[1] == 0x7fffffffu,
          "both counters can reach their last non-overflowing values");
    check(!ee::ee_cop0_perf_advance(&runtime, 0u, 1u) &&
          runtime.m_perf_counter[0] == 14u &&
          runtime.m_perf_counter[1] == 0x7fffffffu &&
          cpu.m_cop0[ee::COP0_Cause] == before_cause,
          "second-counter overflow rejects the whole interval without invented IRQ");
    check(ee::ee_cop0_perf_advance(&runtime, ee::STATUS_ERL, 1u) &&
          runtime.m_perf_counter[0] == 14u &&
          runtime.m_perf_counter[1] == 0x7fffffffu,
          "ERL inhibits counting even at the overflow boundary");
    check(transfer(4u, 0u, 0u) == 0u &&
          ee::ee_cop0_perf_advance(&runtime, 0u, 1u) &&
          runtime.m_perf_counter[0] == 14u &&
          runtime.m_perf_counter[1] == 0x7fffffffu,
          "disabled counters do not overflow on elapsed intervals");

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
        clock.m_reference_instruction_slots = 6u;
        ee::ee_cop0_clock_sync(&a, &clock);
        check(a.m_cop0[ee::COP0_Count] == 2u &&
              (a.m_cop0[ee::COP0_Cause] & k_timer_ip) != 0u,
              "Count wraps and latches Compare at the elapsed budget");
        ee::ee_cop0_clock_write(&a, &clock, ee::COP0_Compare, 10u);
        clock.m_reference_instruction_slots = 9u;
        clock.m_cop0_clock_idle = 2u;
        ee::ee_cop0_clock_sync(&b, &clock);
        check(b.m_cop0[ee::COP0_Count] == 7u &&
              b.m_cop0[ee::COP0_Compare] == 10u &&
              (b.m_cop0[ee::COP0_Cause] & k_timer_ip) == 0u,
              "thread context cannot restore stale timer state");
        ee::ee_cop0_clock_write(&b, &clock, ee::COP0_Count, 9u);
        clock.m_reference_instruction_slots++;
        ee::ee_cop0_clock_sync(&a, &clock);
        check(a.m_cop0[ee::COP0_Count] == 10u &&
              (a.m_cop0[ee::COP0_Cause] & k_timer_ip) != 0u,
              "Count write reanchors without double counting");
        ee::ee_record_hle_timing_boundary(clock, 0x1000u, 0xcu, 0x44u);
        ee::ee_cop0_clock_sync(&a, &clock);
        check(a.m_cop0[ee::COP0_Count] == 310u &&
              clock.m_reference_instruction_slots == 10u,
              "recognized syscall charges reference cost without changing slots");
        uint64_t cycle = 0u;
        check(ee::ee_reference_compatibility_cycle(clock, cycle) && cycle == 312u,
              "shared compatibility timeline includes slots, idle and syscall cost");
        clock.m_hle_timing_boundaries = UINT64_MAX;
        check(!ee::ee_reference_compatibility_cycle(clock, cycle) && cycle == 312u,
              "reference budget overflow is rejected without changing output");
    }
    {
        using event = ee::e_ee_perf_event;
        using result = ee::e_ee_perf_interval_result;
        ee::s_ee_perf_event_interval interval{};
        uint32_t counters[2] = {11u, 19u};
        constexpr uint32_t control = 0x80001004u;
        check(ee::ee_perf_event_for_selector(0u, 2u) == event::SINGLE_ISSUE &&
              ee::ee_perf_event_for_selector(1u, 2u) == event::DUAL_ISSUE &&
              ee::ee_perf_event_for_selector(0u, 0u) == event::RESERVED &&
              ee::ee_perf_event_for_selector(1u, 0u) == event::LOW_ORDER_BRANCH_ISSUED &&
              ee::ee_perf_event_for_selector(0u, 14u) == event::COP2_COMPLETED &&
              ee::ee_perf_event_for_selector(1u, 14u) == event::COP1_COMPLETED,
              "selector identities are counter-specific");
        check(!interval.set(event::RESERVED, 0u) &&
              !interval.set(event::NO_EVENT, 0u) && interval.m_available == 0u,
              "non-events cannot be supplied as measurements");
        for (uint32_t i = 0u; i < 2u; ++i)
        {
            for (uint32_t selector = 0u; selector < 32u; ++selector)
            {
                const event id = ee::ee_perf_event_for_selector(i, selector);
                const uint32_t word = 0x80000000u |
                    (4u << (i * 10u)) | (selector << (5u + i * 10u));
                uint32_t values[2] = {0u, 0u};
                ee::s_ee_perf_event_interval sample{};
                const result expected = id == event::RESERVED ? result::RESERVED_EVENT :
                    id == event::NO_EVENT ? result::OK : result::MISSING_EVENT;
                check(ee::ee_perf_apply_interval(word, 0u, sample, values) == expected &&
                      values[0] == 0u && values[1] == 0u,
                      "missing or reserved event fails closed");
                if (sample.set(id, 7u))
                {
                    check(ee::ee_perf_apply_interval(word, 0u, sample, values) == result::OK &&
                          values[i] == 7u && values[1u-i] == 0u,
                          "selected authoritative event counts independently");
                }
            }
        }
        const uint32_t issue_control = control | (2u << 5u) | (2u << 15u);
        check(interval.set(event::SINGLE_ISSUE, 7u) &&
              ee::ee_perf_apply_interval(issue_control, 0u, interval, counters) ==
                  result::MISSING_EVENT && counters[0] == 11u && counters[1] == 19u,
              "missing second event cannot partially increment first counter");
        check(interval.set(event::DUAL_ISSUE, 0u) &&
              ee::ee_perf_apply_interval(issue_control, 0u, interval, counters) ==
                  result::OK && counters[0] == 18u && counters[1] == 19u,
              "explicit zero dual issues is a valid measurement");
        const uint32_t completed_control = control | (12u << 5u) | (12u << 15u);
        check(interval.set(event::INSTRUCTION_COMPLETED, 9u) &&
              ee::ee_perf_apply_interval(completed_control, 0u, interval, counters) ==
                  result::OK && counters[0] == 27u && counters[1] == 28u,
              "shared completion event updates both counters");
        counters[1] = 0x7fffffffu;
        check(ee::ee_perf_apply_interval(completed_control, 0u, interval, counters) ==
                  result::COUNTER_OVERFLOW && counters[0] == 27u && counters[1] == 0x7fffffffu,
              "second overflow is distinct from missing evidence and atomic");
        check(ee::ee_perf_apply_interval(completed_control, ee::STATUS_ERL, interval, counters) ==
                  result::OK && counters[0] == 27u && counters[1] == 0x7fffffffu,
              "Level 2 status inhibits both counters");
        check(ee::ee_perf_apply_interval(completed_control, ee::STATUS_EXL, interval, counters) ==
                  result::OK && counters[0] == 27u && counters[1] == 0x7fffffffu,
              "EXL does not select kernel-only events");
    }
    {
        using event = ee::e_ee_perf_event;
        using result = ee::e_ee_perf_interval_result;
        ee::s_ee_runtime typed{};
        ee::s_ee_cpu_state typed_cpu{};
        ee::s_ee_perf_event_interval sample{};
        sample.set(event::SINGLE_ISSUE, 3u);
        sample.set(event::DUAL_ISSUE, 5u);
        const uint32_t capabilities = sample.m_available;
        const auto write_control = [&](uint32_t value)
        {
            typed_cpu.m_gpr[4].m_lo = value;
            return ee::ee_cop0_perf_transfer(&typed_cpu, &typed,
                (0x10u << 26u) | (4u << 21u) | (4u << 16u) | (25u << 11u));
        };
        constexpr uint32_t both_issue = 0x80011044u;
        check(!ee::ee_cop0_perf_configure_events(&typed, capabilities) &&
              ee::ee_cop0_perf_advance_events(&typed, 0u, &sample) ==
                  result::UNAVAILABLE_SOURCE,
              "typed event delivery rejects unavailable clock");
        check(typed.m_ee_cycle_scheduler.install_reference_compatibility_source() &&
              !ee::ee_cop0_perf_configure_events(&typed, capabilities) &&
              write_control(both_issue) == 4u,
              "compatibility clock cannot enable measured issue events");
        typed.m_ee_cycle_scheduler.reset();
        check(typed.m_ee_cycle_scheduler.install_validated_source() &&
              !ee::ee_cop0_perf_configure_events(&typed, 0x80000000u) &&
              ee::ee_cop0_perf_configure_events(&typed, capabilities) &&
              write_control(0x80000024u) == 4u &&
              write_control(0x80000004u) == 4u &&
              write_control(both_issue) == 0u,
              "typed producer enables only declared and defined events");
        check(!ee::ee_cop0_perf_configure_events(&typed, 0u) &&
              !ee::ee_cop0_perf_advance(&typed, 0u, 99u) &&
              ee::ee_cop0_perf_advance_events(&typed, 0u, &sample) == result::OK &&
              typed.m_perf_counter[0] == 3u && typed.m_perf_counter[1] == 5u,
              "legacy cycle input cannot double-count typed producer intervals");
        ee::s_ee_perf_event_interval wrong = sample;
        wrong.set(event::PROCESSOR_CYCLE, 1u);
        check(ee::ee_cop0_perf_advance_events(&typed, 0u, &wrong) ==
                  result::UNAVAILABLE_SOURCE &&
              typed.m_perf_counter[0] == 3u && typed.m_perf_counter[1] == 5u,
              "undeclared event measurement rejected atomically");
        typed.m_ee_cycle_scheduler.reset();
        check(typed.m_ee_cycle_scheduler.perf_event_capabilities() == 0u &&
              ee::ee_cop0_perf_advance_events(&typed, 0u, &sample) ==
                  result::UNAVAILABLE_SOURCE &&
              typed.m_ee_cycle_scheduler.install_validated_source() &&
              !ee::ee_cop0_perf_advance(&typed, 0u, 99u) &&
              ee::ee_cop0_perf_advance_events(&typed, 0u, &sample) ==
                  result::UNAVAILABLE_SOURCE &&
              !ee::ee_cop0_perf_configure_events(&typed, capabilities) &&
              typed.m_perf_counter[0] == 3u && typed.m_perf_counter[1] == 5u,
              "scheduler reset revokes prior typed producer authorization");
        check(write_control(0u) == 0u &&
              ee::ee_cop0_perf_configure_events(&typed, 0u) &&
              write_control(0x80000024u) == 0u &&
              ee::ee_cop0_perf_advance(&typed, 0u, 2u) &&
              typed.m_perf_counter[0] == 5u,
              "disabled counter permits return to cycle-only source");
    }
    {
        using event = ee::e_ee_perf_event;
        using result = ee::e_ee_perf_interval_result;
        ee::s_ee_runtime typed{};
        ee::s_ee_cpu_state typed_cpu{};
        ee::s_ee_perf_event_interval sample{};
        sample.set(event::PROCESSOR_CYCLE, 5u);
        const auto write_control = [&](uint32_t value)
        {
            typed_cpu.m_gpr[4].m_lo = value;
            return ee::ee_cop0_perf_transfer(&typed_cpu, &typed,
                (0x10u << 26u) | (4u << 21u) | (4u << 16u) | (25u << 11u));
        };
        constexpr uint32_t cycles = 0x80000024u;
        check(typed.m_ee_cycle_scheduler.install_validated_source() &&
              ee::ee_cop0_perf_configure_events(&typed, sample.m_available) &&
              write_control(cycles) == 0u &&
              ee::ee_cop0_perf_advance_events(&typed, 0u, &sample) == result::OK &&
              typed.m_perf_counter[0] == 5u,
              "authorized typed processor-cycle source counts");
        typed.m_ee_cycle_scheduler.reset();
        typed.m_ee_cycle_scheduler.reset();
        check(typed.m_ee_cycle_scheduler.perf_event_authorization_revoked() &&
              typed.m_ee_cycle_scheduler.perf_event_capabilities() == 0u &&
              !typed.m_ee_cycle_scheduler.install_reference_compatibility_source() &&
              typed.m_ee_cycle_scheduler.install_validated_source() &&
              !ee::ee_cop0_perf_advance(&typed, 0u, 99u) &&
              ee::ee_cop0_perf_advance_events(&typed, 0u, &sample) ==
                  result::UNAVAILABLE_SOURCE &&
              write_control(cycles) == 4u &&
              typed.m_perf_counter[0] == 5u,
              "revoked cycle-only typed selection cannot fall back to legacy accounting");
        check(write_control(0u) == 0u &&
              !ee::ee_cop0_perf_advance(&typed, 0u, 99u) &&
              ee::ee_cop0_perf_configure_events(&typed, 0u) &&
              !typed.m_ee_cycle_scheduler.perf_event_authorization_revoked() &&
              write_control(cycles) == 0u &&
              ee::ee_cop0_perf_advance(&typed, 0u, 2u) &&
              typed.m_perf_counter[0] == 7u,
              "explicit disabled-counter configuration restores legacy cycle delivery");
        typed.m_ee_cycle_scheduler.reset();
        check(typed.m_ee_cycle_scheduler.install_reference_compatibility_source(),
              "ordinary legacy reset permits compatibility clock installation");
    }
    {
        using event = ee::e_ee_perf_event;
        using result = ee::e_ee_perf_interval_result;
        constexpr uint32_t both_cycles = 0x80009024u;
        constexpr uint32_t both_issue = 0x80011044u;
        uint32_t values[2] = {0x7ffffffdu, 0x7fffffffu};
        ee::s_ee_perf_event_interval sample{};
        sample.set(event::PROCESSOR_CYCLE, 3u);
        auto plan = ee::ee_perf_plan_interval(both_cycles, 0u, sample, values);
        check(plan.m_result == result::COUNTER_OVERFLOW &&
              plan.m_counters[0].m_result == result::COUNTER_OVERFLOW &&
              plan.m_counters[0].m_events_to_overflow == 3u &&
              plan.m_counters[1].m_result == result::COUNTER_OVERFLOW &&
              plan.m_counters[1].m_events_to_overflow == 1u &&
              plan.m_counters[0].m_event == event::PROCESSOR_CYCLE &&
              plan.m_counters[1].m_event == event::PROCESSOR_CYCLE &&
              ee::ee_perf_apply_interval(both_cycles, 0u, sample, values) ==
                  result::COUNTER_OVERFLOW &&
              values[0] == 0x7ffffffdu && values[1] == 0x7fffffffu,
              "both counter boundaries reported without partial retirement");

        sample = {};
        sample.set(event::DUAL_ISSUE, 1u);
        plan = ee::ee_perf_plan_interval(both_issue, 0u, sample, values);
        check(plan.m_result == result::MISSING_EVENT &&
              plan.m_counters[0].m_result == result::MISSING_EVENT &&
              plan.m_counters[1].m_result == result::COUNTER_OVERFLOW &&
              plan.m_counters[1].m_events_to_overflow == 1u,
              "missing first event does not conceal second counter boundary");
        sample = {};
        sample.set(event::SINGLE_ISSUE, UINT64_MAX);
        plan = ee::ee_perf_plan_interval(both_issue, 0u, sample, values);
        check(plan.m_result == result::COUNTER_OVERFLOW &&
              plan.m_counters[0].m_events_to_overflow == 3u &&
              plan.m_counters[1].m_result == result::MISSING_EVENT,
              "first-failure ordering preserved for very large deltas");

        sample = {};
        sample.set(event::PROCESSOR_CYCLE, 0u);
        values[0] = 0x80000000u;
        plan = ee::ee_perf_plan_interval(both_cycles, 0u, sample, values);
        check(plan.m_result == result::OK &&
              plan.m_counters[0].m_next == values[0] &&
              plan.m_counters[0].m_events_to_overflow == 0u,
              "zero measured events do not trigger a new boundary");
        sample.set(event::PROCESSOR_CYCLE, 1u);
        plan = ee::ee_perf_plan_interval(both_cycles, 0u, sample, values);
        check(plan.m_result == result::COUNTER_OVERFLOW &&
              plan.m_counters[0].m_events_to_overflow == 1u,
              "already-high counter preserves fail-closed policy");
        plan = ee::ee_perf_plan_interval(both_cycles, ee::STATUS_ERL, sample, values);
        check(plan.m_result == result::OK &&
              plan.m_counters[0].m_event == event::NO_EVENT &&
              plan.m_counters[1].m_event == event::NO_EVENT,
              "inhibited interval has no overflow boundary");

        values[0] = 5u;
        values[1] = 7u;
        sample.set(event::PROCESSOR_CYCLE, 9u);
        plan = ee::ee_perf_plan_interval(both_cycles, 0u, sample, values);
        check(plan.m_result == result::OK &&
              plan.m_counters[0].m_next == 14u &&
              plan.m_counters[1].m_next == 16u &&
              values[0] == 5u && values[1] == 7u &&
              ee::ee_perf_apply_interval(both_cycles, 0u, sample, values) == result::OK &&
              values[0] == 14u && values[1] == 16u,
              "planning is read-only and successful delivery commits both counters");
    }
    {
        using event = ee::e_ee_perf_event;
        using order = ee::e_ee_perf_overflow_order;
        constexpr uint32_t both_cycles = 0x80009024u;
        constexpr uint32_t both_issue = 0x80011044u;
        ee::s_ee_perf_event_interval sample{};
        sample.set(event::PROCESSOR_CYCLE, 3u);
        uint32_t values[2] = {0x7ffffffdu, 0x7fffffffu};
        const auto classify = [&](uint32_t control, uint32_t status)
        {
            return ee::ee_perf_classify_overflow_order(
                ee::ee_perf_plan_interval(control, status, sample, values));
        };
        check(classify(both_cycles, 0u) == order::PCR1_FIRST,
              "shared event ordinal orders PCR1 before PCR0 despite first-failure policy");
        values[0] = 0x7fffffffu;
        values[1] = 0x7ffffffdu;
        check(classify(both_cycles, 0u) == order::PCR0_FIRST,
              "shared event ordinal orders PCR0 before PCR1");
        values[1] = values[0];
        check(classify(both_cycles, 0u) == order::SAME_EVENT_ORDINAL,
              "equal shared ordinals do not choose a counter exception priority");
        sample = {};
        sample.set(event::SINGLE_ISSUE, 3u);
        sample.set(event::DUAL_ISSUE, 1u);
        check(classify(both_issue, 0u) == order::INDETERMINATE,
              "different event identities cannot be ordered from aggregate deltas");
        sample = {};
        sample.set(event::SINGLE_ISSUE, 1u);
        check(classify(both_issue, 0u) == order::INDETERMINATE,
              "missing second event prevents an earliest-boundary claim");
        sample.set(event::DUAL_ISSUE, 0u);
        check(classify(both_issue, 0u) == order::PCR0_FIRST,
              "available zero proves the second counter does not cross");
        sample.set(event::SINGLE_ISSUE, 0u);
        sample.set(event::DUAL_ISSUE, 1u);
        check(classify(both_issue, 0u) == order::PCR1_FIRST,
              "available zero proves the first counter does not cross");
        sample.set(event::DUAL_ISSUE, 0u);
        check(classify(both_issue, 0u) == order::NONE,
              "complete zero deltas have no crossing");
        check(classify(both_issue, ee::STATUS_ERL) == order::NONE &&
              classify(both_issue & 0x7fffffffu, 0u) == order::NONE,
              "inhibited and disabled intervals have no crossing");
        constexpr uint32_t reserved_pcr0 = 0x80011004u;
        sample.set(event::DUAL_ISSUE, 1u);
        check(classify(reserved_pcr0, 0u) == order::INDETERMINATE &&
              values[0] == 0x7fffffffu && values[1] == 0x7fffffffu,
              "reserved event prevents order claim and planning preserves counters");
    }
    {
        constexpr uint32_t dev = 1u << 23u;
        constexpr uint32_t bev = 1u << 22u;
        constexpr uint32_t bd2 = 1u << 30u;
        constexpr uint32_t exc2_mask = 7u << 16u;
        constexpr uint32_t unrelated = 0x90008c24u;
        constexpr uint32_t pc = 0x00100004u;
        for (uint32_t vector_flags = 0u; vector_flags < 4u; ++vector_flags)
        {
            for (uint32_t exl = 0u; exl < 2u; ++exl)
            {
                for (uint32_t delay = 0u; delay < 2u; ++delay)
                {
                    for (uint32_t old_exc2 = 0u; old_exc2 < 8u; ++old_exc2)
                    {
                        for (uint32_t old_bd2 = 0u; old_bd2 < 2u; ++old_bd2)
                        {
                            const uint32_t status =
                                ((vector_flags & 1u) != 0u ? dev : 0u) |
                                ((vector_flags & 2u) != 0u ? bev : 0u) |
                                (exl << 1u) | 0x00010011u;
                            const uint32_t cause = unrelated |
                                (old_exc2 << 16u) | (old_bd2 << 30u);
                            const auto entry = ee::ee_perf_project_exception_entry(
                                status, cause, pc, delay != 0u);
                            check(entry.m_status == (status | ee::STATUS_ERL) &&
                                  (entry.m_cause & ~(bd2 | exc2_mask)) == unrelated &&
                                  (entry.m_cause & exc2_mask) == (2u << 16u) &&
                                  (entry.m_cause & bd2) == (delay << 30u) &&
                                  entry.m_error_epc == (delay != 0u ? pc - 4u : pc) &&
                                  entry.m_vector == ((vector_flags & 1u) != 0u
                                      ? 0xbfc00280u : 0x80000080u),
                                  "recognized Level 2 counter entry preserves unrelated state");
                        }
                    }
                }
            }
        }
        constexpr auto ordinary = ee::ee_perf_project_exception_entry(
            0u, 0u, pc, false);
        static_assert(ordinary.m_status == 4u &&
                      ordinary.m_cause == 0x00020000u &&
                      ordinary.m_error_epc == pc &&
                      ordinary.m_vector == 0x80000080u);
    }
    {
        using result = ee::e_ee_perf_recognition;
        constexpr uint32_t cte = 0x80000000u;
        constexpr uint32_t low = 0x7fffffffu;
        constexpr uint32_t high = 0x80000000u;
        for (uint32_t mask = 0u; mask < 4u; ++mask)
        {
            const uint32_t counters[2] =
            {
                (mask & 1u) != 0u ? high : low,
                (mask & 2u) != 0u ? high : low
            };
            for (uint32_t flags = 0u; flags < 16u; ++flags)
            {
                const bool enabled = (flags & 1u) != 0u;
                const bool erl = (flags & 2u) != 0u;
                const bool reset = (flags & 4u) != 0u;
                const bool nmi = (flags & 8u) != 0u;
                const auto expected = mask == 0u ? result::NO_PENDING_OVERFLOW :
                    reset ? result::SUPERSEDED_RESET :
                    !enabled ? result::COUNTING_DISABLED :
                    erl ? result::DEFERRED_ERL :
                    nmi ? result::DEFERRED_NMI : result::ELIGIBLE;
                for (uint32_t mode = 0u; mode < 4u; ++mode)
                {
                    for (uint32_t exl = 0u; exl < 2u; ++exl)
                    {
                        const uint32_t status = (mode << 3u) |
                            (exl << 1u) | (erl ? ee::STATUS_ERL : 0u);
                        const auto plan = ee::ee_perf_plan_recognition(
                            enabled ? cte : 0u, status, counters, reset, nmi);
                        check(plan.m_overflow_mask == mask &&
                              plan.m_result == expected,
                              "candidate recognition preserves raw flags and respects priority");
                        const auto irq_plan = ee::ee_perf_plan_recognition(
                            (enabled ? cte : 0u) | 0x000ffffeu,
                            status | ee::STATUS_IE | ee::STATUS_EIE | 0x0000ff00u,
                            counters, reset, nmi);
                        check(irq_plan.m_overflow_mask == mask &&
                              irq_plan.m_result == expected,
                              "recognition candidate independent of Level 1 masks and count-mode enables");
                    }
                }
            }
        }
        constexpr uint32_t pending[2] = {high, low};
        constexpr auto deferred = ee::ee_perf_plan_recognition(
            cte, ee::STATUS_ERL, pending, false, true);
        constexpr auto after_eret = ee::ee_perf_plan_recognition(
            cte, 0u, pending, false, false);
        static_assert(deferred.m_result == result::DEFERRED_ERL &&
                      deferred.m_overflow_mask == 1u &&
                      after_eret.m_result == result::ELIGIBLE &&
                      after_eret.m_overflow_mask == 1u);
    }
    {
        using event = ee::e_ee_perf_event;
        using boundary = ee::e_ee_perf_boundary_result;
        using interval_result = ee::e_ee_perf_interval_result;
        constexpr uint32_t both_cycles = 0x80009024u;
        constexpr uint32_t both_issue = 0x80011044u;
        uint32_t counters[2] = {0x7ffffffdu, 0x7fffffffu};
        ee::s_ee_perf_event_interval prefix{};
        prefix.set(event::PROCESSOR_CYCLE, 1u);
        const auto project = [&](uint32_t control, uint32_t status = 0u)
        {
            return ee::ee_perf_project_crossing_prefix(control, status, prefix, counters);
        };
        auto plan = project(both_cycles);
        check(plan.m_result == boundary::EXACT_CROSSING &&
              plan.m_crossing_mask == 2u &&
              plan.m_next[0] == 0x7ffffffeu && plan.m_next[1] == 0x80000000u &&
              counters[0] == 0x7ffffffdu && counters[1] == 0x7fffffffu &&
              ee::ee_perf_apply_interval(both_cycles, 0u, prefix, counters) ==
                  interval_result::COUNTER_OVERFLOW &&
              counters[0] == 0x7ffffffdu && counters[1] == 0x7fffffffu,
              "exact prefix projects crossing without altering fail-closed delivery");
        prefix.set(event::PROCESSOR_CYCLE, 3u);
        plan = project(both_cycles);
        check(plan.m_result == boundary::OVERSHOT_BOUNDARY &&
              plan.m_crossing_mask == 0u &&
              plan.m_next[0] == counters[0] && plan.m_next[1] == counters[1],
              "reject prefix overshooting the earlier shared-event boundary");
        counters[1] = counters[0];
        plan = project(both_cycles);
        check(plan.m_result == boundary::EXACT_CROSSING &&
              plan.m_crossing_mask == 3u &&
              plan.m_next[0] == 0x80000000u && plan.m_next[1] == 0x80000000u,
              "equal shared-event crossing projects both flags");
        prefix.set(event::PROCESSOR_CYCLE, UINT64_MAX);
        check(project(both_cycles).m_result == boundary::OVERSHOT_BOUNDARY,
              "large aggregate cannot be truncated to a crossing");
        prefix = {};
        prefix.set(event::SINGLE_ISSUE, 3u);
        check(project(both_issue).m_result == boundary::INCOMPLETE_EVIDENCE,
              "missing second event prevents projection");
        prefix.set(event::DUAL_ISSUE, 1u);
        plan = project(both_issue);
        check(plan.m_result == boundary::EXACT_CROSSING &&
              plan.m_crossing_mask == 1u &&
              plan.m_next[0] == 0x80000000u && plan.m_next[1] == 0x7ffffffeu,
              "complete supplied prefix preserves the other event count");
        prefix.set(event::DUAL_ISSUE, 3u);
        check(project(both_issue).m_result == boundary::AMBIGUOUS_ORDER,
              "distinct event crossings require independent ordering evidence");
        check(project(both_issue, ee::STATUS_ERL).m_result == boundary::NO_CROSSING &&
              project(both_issue & 0x7fffffffu).m_result == boundary::NO_CROSSING,
              "inhibited and disabled intervals do not cross");
        prefix.set(event::SINGLE_ISSUE, 0u);
        prefix.set(event::DUAL_ISSUE, 0u);
        plan = project(both_issue);
        check(plan.m_result == boundary::NO_CROSSING &&
              plan.m_next[0] == counters[0] && plan.m_next[1] == counters[1],
              "no crossing does not produce a commitment");
        counters[0] = 0x80000000u;
        check(project(both_issue).m_result == boundary::EXISTING_OVERFLOW,
              "already pending overflow requires separate recognition");
    }
    check(test_perf_restart_boundary(), "Level 2 restart boundary contract");
    check(test_perf_crossing_entry_composition(),
          "crossing and entry plans compose without mutation");
    {
        using event = ee::e_ee_perf_event;
        using result = ee::e_ee_perf_crossing_entry_result;
        using kind = ee::e_ee_perf_restart_kind;
        ee::s_ee_cpu_state probe{};
        ee::s_ee_runtime typed{};
        probe.m_pc = 0x1004u;
        probe.m_cop0[ee::COP0_Status] = 0x00800000u;
        probe.m_cop0[ee::COP0_Cause] = 0x80008424u;
        probe.m_cop0[ee::COP0_ErrorEPC] = 0x12345678u;
        typed.m_perf_counter[0] = 0x7ffffffdu;
        typed.m_perf_counter[1] = 0x7fffffffu;
        ee::s_ee_perf_crossing_request request{};
        request.m_control = 0x80009024u;
        request.m_status = probe.m_cop0[ee::COP0_Status];
        request.m_before[0] = typed.m_perf_counter[0];
        request.m_before[1] = typed.m_perf_counter[1];
        request.m_prefix.set(event::PROCESSOR_CYCLE, 1u);
        request.m_boundary = {kind::DELAY_SLOT, 0x1004u, 0x1000u, true};
        const auto preflight = [&]()
        {
            return ee::ee_cop0_perf_preflight_crossing(probe, typed, request);
        };
        check(preflight().m_result == result::UNAVAILABLE_SOURCE &&
              typed.m_ee_cycle_scheduler.install_reference_compatibility_source() &&
              preflight().m_result == result::UNAVAILABLE_SOURCE,
              "unavailable and compatibility clocks cannot preflight typed entry");
        typed.m_ee_cycle_scheduler.reset();
        check(typed.m_ee_cycle_scheduler.install_validated_source() &&
              preflight().m_result == result::UNAVAILABLE_SOURCE &&
              ee::ee_cop0_perf_configure_events(&typed, request.m_prefix.m_available),
              "validated source requires explicit typed capability authorization");
        typed.m_perf_control = request.m_control;
        check(preflight().m_result == result::READY &&
              typed.m_perf_counter[0] == request.m_before[0] &&
              typed.m_perf_counter[1] == request.m_before[1] &&
              probe.m_pc == 0x1004u &&
              probe.m_cop0[ee::COP0_Status] == request.m_status &&
              probe.m_cop0[ee::COP0_Cause] == 0x80008424u &&
              probe.m_cop0[ee::COP0_ErrorEPC] == 0x12345678u,
              "authorized preflight does not commit registers or counters");
        request.m_prefix.set(event::SINGLE_ISSUE, 0u);
        check(preflight().m_result == result::UNAUTHORIZED_EVENTS,
              "undeclared event is rejected even when zero");
        request.m_prefix = {};
        request.m_prefix.set(event::PROCESSOR_CYCLE, 1u);
        typed.m_perf_control = 0x80011044u;
        check(preflight().m_result == result::UNAUTHORIZED_EVENTS,
              "unsupported active selectors cannot preflight");
        typed.m_perf_control = request.m_control;
        ++typed.m_perf_counter[0];
        check(preflight().m_result == result::STALE_SNAPSHOT,
              "changed counter rejects previously supplied prefix");
        --typed.m_perf_counter[0];
        typed.m_ee_cycle_scheduler.reset();
        check(typed.m_ee_cycle_scheduler.install_validated_source() &&
              preflight().m_result == result::UNAVAILABLE_SOURCE,
              "reset revokes preflight authorization");
    }
    check(test_perf_commit_boundary_gate(), "Level 2 commit boundary gate");
    {
        using event = ee::e_ee_perf_event;
        using result = ee::e_ee_perf_crossing_entry_result;
        using kind = ee::e_ee_perf_restart_kind;
        constexpr uint32_t dispatch = static_cast<uint32_t>(
            ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
        constexpr uint32_t level2 = static_cast<uint32_t>(
            ee::e_ee_exit_reason::EE_EXIT_LEVEL2_EXCEPTION);
        for (uint32_t delay = 0u; delay < 2u; ++delay)
        {
            ee::s_ee_cpu_state probe{};
            ee::s_ee_runtime typed{};
            const uint32_t initial_status = ee::STATUS_EXL |
                (delay != 0u ? 0x00800000u : 0u);
            constexpr uint32_t initial_cause = 0x80008420u;
            constexpr uint32_t initial_epc = 0x12340000u;
            constexpr uint32_t initial_error_epc = 0x56780000u;
            probe.m_pc = 0x1000u;
            probe.m_exit_reason = dispatch;
            probe.m_cop0[ee::COP0_Status] = initial_status;
            probe.m_cop0[ee::COP0_Cause] = initial_cause;
            probe.m_cop0[ee::COP0_EPC] = initial_epc;
            probe.m_cop0[ee::COP0_ErrorEPC] = initial_error_epc;
            probe.m_cop0[ee::COP0_Count] = 99u;
            probe.m_cop0[ee::COP0_Compare] = 100u;
            probe.m_pad = 0x2000u;
            probe.m_gpr[8].m_lo = 42u;
            typed.m_last_exception_in_bd_slot = true;
            typed.m_perf_counter[0] = 0x7ffffffdu;
            typed.m_perf_counter[1] = 0x7fffffffu;
            ee::s_ee_perf_crossing_request request{};
            // Both PCRs select processor cycles in EXL mode.
            request.m_control = 0x80008822u;
            request.m_status = initial_status;
            request.m_before[0] = typed.m_perf_counter[0];
            request.m_before[1] = typed.m_perf_counter[1];
            request.m_prefix.set(event::PROCESSOR_CYCLE, 1u);
            request.m_boundary = delay != 0u
                ? ee::s_ee_perf_restart_boundary{kind::DELAY_SLOT, 0x1004u, 0x1000u, true}
                : ee::s_ee_perf_restart_boundary{kind::ORDINARY, 0x1000u, 0u, true};
            const auto commit = [&]()
            {
                return ee::ee_cop0_perf_commit_crossing(probe, typed, request);
            };
            const auto unchanged = [&]()
            {
                return typed.m_perf_counter[0] == request.m_before[0] &&
                       typed.m_perf_counter[1] == request.m_before[1] &&
                       probe.m_cop0[ee::COP0_Status] == initial_status &&
                       probe.m_cop0[ee::COP0_Cause] == initial_cause &&
                       probe.m_cop0[ee::COP0_EPC] == initial_epc &&
                       probe.m_cop0[ee::COP0_ErrorEPC] == initial_error_epc &&
                       probe.m_cop0[ee::COP0_Count] == 99u &&
                       probe.m_cop0[ee::COP0_Compare] == 100u &&
                       probe.m_pad == 0x2000u && probe.m_gpr[8].m_lo == 42u &&
                       typed.m_last_exception_in_bd_slot;
            };
            check(commit() == result::UNAVAILABLE_SOURCE && unchanged() &&
                  typed.m_ee_cycle_scheduler.install_reference_compatibility_source() &&
                  commit() == result::UNAVAILABLE_SOURCE && unchanged(),
                  "unavailable and compatibility sources cannot commit Level 2 entry");
            typed.m_ee_cycle_scheduler.reset();
            check(typed.m_ee_cycle_scheduler.install_validated_source() &&
                  commit() == result::UNAVAILABLE_SOURCE && unchanged() &&
                  ee::ee_cop0_perf_configure_events(&typed, request.m_prefix.m_available),
                  "Level 2 commit requires typed authorization");
            typed.m_perf_control = request.m_control;
            request.m_prefix.set(event::SINGLE_ISSUE, 0u);
            check(commit() == result::UNAUTHORIZED_EVENTS && unchanged(),
                  "undeclared event rejects commit without mutation");
            request.m_prefix = {};
            request.m_prefix.set(event::PROCESSOR_CYCLE, 2u);
            check(commit() == result::PREFIX_REJECTED && unchanged(),
                  "overshot prefix rejects commit without mutation");
            request.m_prefix.set(event::PROCESSOR_CYCLE, 1u);
            request.m_nmi_pending = true;
            check(commit() == result::ENTRY_REJECTED && unchanged(),
                  "NMI priority rejects commit without mutation");
            request.m_nmi_pending = false;
            probe.m_pc = 0x1004u;
            check(commit() == result::UNSAFE_LIVE_STATE && unchanged() &&
                  probe.m_pc == 0x1004u && probe.m_exit_reason == dispatch,
                  "wrong live restart PC rejects commit");
            probe.m_pc = 0x1000u;
            probe.m_exit_reason = 1u;
            check(commit() == result::UNSAFE_LIVE_STATE && unchanged() &&
                  probe.m_exit_reason == 1u,
                  "existing exception exit cannot be overwritten");
            probe.m_exit_reason = dispatch;
            --request.m_before[0];
            check(commit() == result::STALE_SNAPSHOT &&
                  typed.m_perf_counter[0] == 0x7ffffffdu &&
                  probe.m_pc == 0x1000u && probe.m_exit_reason == dispatch,
                  "stale PCR snapshot rejects commit");
            ++request.m_before[0];
            const uint32_t vector = delay != 0u ? 0xbfc00280u : 0x80000080u;
            const uint32_t cause = 0x80028420u | (delay != 0u ? 0x40000000u : 0u);
            check(commit() == result::COMMITTED &&
                  typed.m_perf_counter[0] == 0x7ffffffeu &&
                  typed.m_perf_counter[1] == 0x80000000u &&
                  probe.m_cop0[ee::COP0_Status] == (initial_status | ee::STATUS_ERL) &&
                  probe.m_cop0[ee::COP0_Cause] == cause &&
                  probe.m_cop0[ee::COP0_ErrorEPC] == 0x1000u &&
                  probe.m_cop0[ee::COP0_EPC] == initial_epc &&
                  probe.m_cop0[ee::COP0_Count] == 99u &&
                  probe.m_cop0[ee::COP0_Compare] == 100u &&
                  probe.m_pc == vector && probe.m_exit_reason == level2 &&
                  probe.m_pad == 0x2000u && probe.m_gpr[8].m_lo == 42u &&
                  typed.m_last_exception_in_bd_slot,
                  "commit both PCRs and Level 2 state while preserving Level 1 and clock state");
            check(commit() == result::STALE_SNAPSHOT &&
                  typed.m_perf_counter[1] == 0x80000000u &&
                  probe.m_pc == vector && probe.m_exit_reason == level2,
                  "same request cannot commit twice");
            check(ee::ee_exception_return(&probe) == 0x1000u &&
                  probe.m_cop0[ee::COP0_Status] == initial_status &&
                  probe.m_cop0[ee::COP0_EPC] == initial_epc &&
                  probe.m_cop0[ee::COP0_ErrorEPC] == 0x1000u &&
                  probe.m_pc == 0x1000u && probe.m_exit_reason == dispatch &&
                  typed.m_perf_counter[1] == 0x80000000u,
                  "ERET resumes ErrorEPC and preserves EXL and pending counter flag");
            typed.m_ee_cycle_scheduler.reset();
            check(typed.m_ee_cycle_scheduler.install_validated_source() &&
                  commit() == result::UNAVAILABLE_SOURCE &&
                  typed.m_perf_counter[1] == 0x80000000u,
                  "reset revokes commit authorization");
        }
    }
    std::printf("ee_cop0_write_test: %d failure(s)\n", s_failures);
    return s_failures != 0 ? 1 : 0;
}
