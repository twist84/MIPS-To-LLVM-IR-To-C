#include "ee_issue_trace_replay.h"

#include <cstdio>
#include <utility>

namespace
{
int s_failures = 0;
void check(bool success, const char* description)
{
    if (!success)
    {
        std::printf("FAIL: %s\n", description);
        ++s_failures;
    }
}
ee::s_ee_instruction_trace_entry record(
    uint64_t attempt, uint32_t pc, uint32_t opcode,
    uint32_t kind, uint64_t slot)
{
    ee::s_ee_instruction_trace_entry item{};
    item.m_attempt = attempt;
    item.m_pc = pc;
    item.m_opcode_word = opcode;
    item.m_kind = kind;
    item.m_reference_slot = slot;
    return item;
}
}

int main()
{
    using ee::c_ee_issue_trace_replay;
    using ee::e_ee_issue_replay_status;
    c_ee_issue_trace_replay replay{};

    check(replay.observe(record(1u, 0x1000u, 0x24080001u, 0u, 1u)) ==
          e_ee_issue_replay_status::UNCONFIGURED,
          "requires explicit latency profile");
    check(!replay.configure({0u, 3u}) && replay.configure({1u, 3u}),
          "rejects zero latency");

    check(replay.observe(record(1u, 0x1000u, 0x24080001u, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x1004u, 0x24090002u, 2u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.candidate_cycles() == 1u,
          "native and RAM independent instructions can pair");

    check(replay.observe(record(3u, 0x1008u, 0x8d0a0000u, 0u, 3u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(4u, 0x100cu, 0x01495821u, 2u, 4u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.candidate_cycles() == 5u,
          "load stall persists across native/interpreted transition");

    constexpr uint32_t k_beql = (0x14u << 26u) | (11u << 21u) | 1u;
    check(replay.observe(record(5u, 0x1010u, k_beql, 0u, 5u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(5u, 0x1014u, 0x240c0001u, 1u, 6u)) ==
          e_ee_issue_replay_status::ANNULLED &&
          replay.observe(record(6u, 0x1018u, 0x240d0002u, 0u, 7u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.candidate_cycles() == 8u &&
          replay.executed_instructions() == 6u &&
          replay.annulled_slots() == 1u &&
          replay.last_event_candidate_cycle() == 7u,
          "annulled delay slot reserves candidate time without execution");

    replay.configure({1u, 3u});
    check(replay.observe(record(100u, 0x2000u, 0x24080001u, 0u, 100u)) ==
          e_ee_issue_replay_status::INCOMPLETE_STREAM &&
          !replay.usable_prefix() &&
          replay.failure_pc() == 0x2000u &&
          replay.observe(record(1u, 0x1000u, 0x24080001u, 0u, 1u)) ==
          e_ee_issue_replay_status::HALTED,
          "filtered and incomplete streams fail closed");

    replay.configure({1u, 3u});
    check(replay.observe(record(1u, 0x1000u, k_beql, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(1u, 0x1008u, 0x24090002u, 1u, 2u)) ==
          e_ee_issue_replay_status::INVALID_SLOT,
          "annulment requires exact preceding branch-likely delay address");

    replay.configure({1u, 3u});
    check(replay.observe(record(1u, 0x1000u, 0x0000000cu, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "no fabricated HLE syscall latency");
    replay.configure({1u, 3u});
    check(replay.observe(record(1u, 0x1000u, 0x00000018u, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "no fabricated HI/LO pipeline cost");
    replay.configure({1u, 3u});
    check(replay.observe(record(1u, 0x1000u, 0x46000000u, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "no fabricated COP1 pipeline cost");

    // Per-pipeline availability is never inferred from the generic scalar
    // profile. Demand an explicit MUL, DIV, move or accumulate latency.
    replay.configure({1u, 3u});
    const uint32_t mult = (4u << 21u) | (5u << 16u) | (6u << 11u) | 0x18u;
    const uint32_t mflo = (7u << 11u) | 0x12u;
    check(replay.observe(record(1u, 0x1000u, mult, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "HI/LO multiply is unavailable without calibrated latency");

    replay.configure({1u, 3u, 7u, 11u, 4u, 8u});
    check(replay.observe(record(1u, 0x1000u, mult, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x1004u, mflo, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.candidate_cycles() == 8u &&
          replay.last_event_candidate_cycle() == 7u,
          "separate MULT HI0/LO0 latency delays MFLO without GPR aliasing");

    replay.configure({1u, 3u, 7u, 11u, 4u, 8u});
    const uint32_t div1 = (4u << 21u) | (5u << 16u) |
                          (0x1cu << 26u) | 0x1au;
    const uint32_t mflo1 = (0x1cu << 26u) | (7u << 11u) | 0x12u;
    check(replay.observe(record(1u, 0x1000u, div1, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x1004u, mflo1, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.candidate_cycles() == 12u,
          "second HI1/LO1 divide pipeline gets an independent delay");

    replay.configure({1u, 3u, 7u, 11u, 4u, 8u});
    const uint32_t madd1 = (0x1cu << 26u) | (4u << 21u) |
                           (5u << 16u) | 0x20u;
    check(replay.observe(record(1u, 0x1000u, madd1, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x1004u, mflo1, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.candidate_cycles() == 9u,
          "accumulation reads/writes HI1/LO1 with explicit latency");

    // COP1 FPR0 is not a hardwired zero register. Preserve hazards across
    // GPR-to-FPR moves, arithmetic and FPR-to-GPR moves.
    replay.configure({1u, 3u});
    const uint32_t mtc1 = (0x11u << 26u) | (4u << 21u) |
                          (8u << 16u) | (2u << 11u);
    check(replay.observe(record(1u, 0x1000u, mtc1, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "unconfigured COP1 transfer refuses guessed latency");

    replay.configure({1u, 3u, 7u, 11u, 4u, 8u, 2u, 5u, 14u, 9u});
    const uint32_t adds = (0x11u << 26u) | (0x10u << 21u) |
                          (3u << 16u) | (2u << 11u) | (4u << 6u);
    const uint32_t mfc1 = (0x11u << 26u) | (9u << 16u) | (4u << 11u);
    check(replay.observe(record(1u, 0x1000u, mtc1, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x1004u, adds, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(3u, 0x1008u, mfc1, 0u, 3u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.candidate_cycles() == 8u,
          "MTC1 to ADD.S to MFC1 preserves FPR register dependencies");

    replay.configure({1u, 3u, 7u, 11u, 4u, 8u, 2u, 5u, 14u, 9u});
    const uint32_t adda = (0x11u << 26u) | (0x10u << 21u) |
                          (3u << 16u) | (2u << 11u) | 0x18u;
    const uint32_t madds = (0x11u << 26u) | (0x10u << 21u) |
                           (3u << 16u) | (2u << 11u) |
                           (4u << 6u) | 0x1cu;
    check(replay.observe(record(1u, 0x1000u, adda, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x1004u, madds, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.candidate_cycles() == 10u,
          "ADDA.S to MADD.S waits on independent FPU ACC scoreboard");

    replay.configure({1u, 3u, 7u, 11u, 4u, 8u, 2u, 5u, 14u, 9u});
    check(replay.observe(record(1u, 0x1000u, 0x45000001u, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "FCSR-dependent BC1 remains unavailable");

    // Host-side HLE handoffs are not instructions or cycles. The service
    // identity is visible and the issue replay must refuse an unknown cost.
    ee::s_ee_runtime runtime{};
    ee::s_ee_instruction_trace_entry buffer[1]{};
    runtime.m_instruction_trace = buffer;
    runtime.m_instruction_trace_capacity = 1u;
    runtime.m_guest_instruction_attempts = 10u;
    runtime.m_reference_instruction_slots = 11u;
    ee::ee_record_hle_timing_boundary(runtime, 0x2000u, 0x0000000cu, 0x76u);
    check(runtime.m_hle_timing_boundaries == 1u &&
          runtime.m_guest_instruction_attempts == 10u &&
          runtime.m_reference_instruction_slots == 11u &&
          runtime.m_instruction_trace_count == 1u &&
          buffer[0].m_kind == 3u &&
          buffer[0].m_hle_service == 0x76u &&
          buffer[0].m_reference_slot == 11u,
          "HLE event preserves instruction and slot ordinals");

    ee::ee_record_hle_timing_boundary(runtime, 0x2000u, 0x0000000cu, 0x77u);
    check(runtime.m_hle_timing_boundaries == 2u &&
          runtime.m_instruction_trace_count == 1u &&
          runtime.m_instruction_trace_overflow == 1u,
          "HLE boundary on full trace is reported, not silently dropped");

    replay.configure({1u, 3u});
    check(replay.observe(buffer[0]) ==
          e_ee_issue_replay_status::UNMODELED_HLE_BOUNDARY &&
          !replay.usable_prefix(),
          "unknown HLE cost cannot produce a continuous candidate clock");

    // A bound host observer receives explicit observations, including HLE
    // service identity. Rejection must latch without creating EE cycles.
    struct s_observer_sample
    {
        uint32_t m_kind = 0u;
        uint32_t m_service = 0u;
        uint64_t m_attempt = 0u;
        uint64_t m_slot = 0u;
        uint32_t m_calls = 0u;
    } observer_sample;
    const auto observer = [](void* user, uint32_t, uint32_t,
                             uint32_t kind, uint32_t service,
                             uint64_t attempt, uint64_t slot) -> bool
    {
        auto& sample = *static_cast<s_observer_sample*>(user);
        sample.m_kind = kind;
        sample.m_service = service;
        sample.m_attempt = attempt;
        sample.m_slot = slot;
        ++sample.m_calls;
        return false; // Reject: external timing has not been validated.
    };
    ee::s_ee_runtime callback_runtime{};
    callback_runtime.m_timing_observer_user = &observer_sample;
    callback_runtime.m_timing_observer = observer;
    ee::ee_record_hle_timing_boundary(
        callback_runtime, 0x2000u, 0x0000000cu, 0x76u);
    check(callback_runtime.m_timing_observer_failed == 1u &&
          observer_sample.m_kind == 3u &&
          observer_sample.m_service == 0x76u &&
          observer_sample.m_calls == 1u &&
          callback_runtime.m_ee_cycle_scheduler.source() ==
              ee::e_ee_cycle_source::UNAVAILABLE,
          "rejected host HLE observation cannot install an EE cycle source");
    ee::ee_record_hle_timing_boundary(
        callback_runtime, 0x2004u, 0x0000000cu, 0x77u);
    check(observer_sample.m_calls == 1u &&
          callback_runtime.m_hle_timing_boundaries == 2u,
          "timing observation rejection is latched; execution diagnostics continue");

    // FCR31/FCC0 is a shared control/condition destination. None of
    // these test-only latencies establish actual R5900 cycle timing.
    const uint32_t ctc1_fcr31 = (0x11u << 26u) | (0x06u << 21u) |
                                (8u << 16u) | (31u << 11u);
    const uint32_t cfc1_fcr31 = (0x11u << 26u) | (0x02u << 21u) |
                                (9u << 16u) | (31u << 11u);
    const uint32_t bc1t = (0x11u << 26u) | (0x08u << 21u) |
                          (1u << 16u) | 1u;
    const uint32_t ceq_s = (0x11u << 26u) | (0x10u << 21u) |
                           (3u << 16u) | (2u << 11u) | 0x32u;

    replay.configure({1u, 3u});
    check(replay.observe(record(1u, 0x3000u, ceq_s, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "COP1 comparisons require explicit condition latency");

    replay.configure({1u, 3u, 7u, 11u, 4u, 8u,
                      2u, 5u, 14u, 9u, 6u, 7u});
    check(replay.observe(record(1u, 0x3000u, ceq_s, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x3004u, bc1t, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 7u,
          "C.EQ.S to BC1T observes caller-supplied FCR31 latency");

    replay.configure({1u, 3u, 7u, 11u, 4u, 8u,
                      2u, 5u, 14u, 9u, 6u, 7u});
    const uint32_t clt_s = (ceq_s & ~0x3fu) | 0x34u;
    check(replay.observe(record(1u, 0x3000u, clt_s, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x3004u, bc1t, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 7u,
          "EE C.LT.S comparison must use funct 0x34, not MIPS 0x3c");

    replay.configure({1u, 3u, 7u, 11u, 4u, 8u,
                      2u, 5u, 14u, 9u, 6u, 7u});
    check(replay.observe(record(1u, 0x3000u, ctc1_fcr31, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x3004u, bc1t, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(3u, 0x3008u, cfc1_fcr31, 0u, 3u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.candidate_cycles() == 7u,
          "CTC1 to BC1T to CFC1 preserves FCR31 control ordering");

    replay.configure({1u, 3u, 7u, 11u, 4u, 8u,
                      2u, 5u, 14u, 9u, 6u, 7u});
    const uint32_t cfc1_fcr0 = (0x11u << 26u) | (0x02u << 21u) |
                               (9u << 16u);
    check(replay.observe(record(1u, 0x3000u, cfc1_fcr0, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "unmodeled FCR0 control-transfer timing still fails closed");

    replay.configure({1u, 3u});
    const uint32_t addi = (0x08u << 26u) | (8u << 16u) | 1u;
    const uint32_t daddi = (0x18u << 26u) | (9u << 16u) | 1u;
    check(replay.observe(record(1u, 0x3000u, addi, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "overflow-trapping ADDI remains unsupported by the guest decoder");
    replay.configure({1u, 3u});
    check(replay.observe(record(1u, 0x3000u, daddi, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "overflow-trapping DADDI remains unsupported by the guest decoder");
    replay.configure({1u, 3u});
    const uint32_t addiu = (0x09u << 26u) | (8u << 16u) | 1u;
    const uint32_t daddiu = (0x19u << 26u) | (9u << 16u) | 1u;
    check(replay.observe(record(1u, 0x3000u, addiu, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x3004u, daddiu, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED,
          "nontrapping ADDIU and DADDIU use explicit scalar-ALU timing");

    // The EE Core manual lists DIV.S, SQRT.S and RSQRT.S separately.
    // No one class may inherit another's configured candidate latency.
    const uint32_t sqrt_s = (0x11u << 26u) | (0x10u << 21u) |
                            (2u << 11u) | (4u << 6u) | 0x04u;
    const uint32_t rsqrt_s = (0x11u << 26u) | (0x10u << 21u) |
                             (3u << 16u) | (2u << 11u) |
                             (5u << 6u) | 0x16u;
    const uint32_t mfc1_sqrt = (0x11u << 26u) |
                               (8u << 16u) | (4u << 11u);
    const uint32_t mfc1_rsqrt = (0x11u << 26u) |
                                (8u << 16u) | (5u << 11u);

    replay.configure({1u, 3u, 7u, 11u, 4u, 8u,
                      2u, 5u, 8u, 5u, 6u, 7u});
    check(replay.observe(record(1u, 0x3000u, sqrt_s, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "DIV.S latency must not silently cover SQRT.S");

    replay.configure({1u, 3u, 7u, 11u, 4u, 8u,
                      2u, 5u, 8u, 5u, 6u, 7u, 8u, 14u});
    check(replay.observe(record(1u, 0x3000u, sqrt_s, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x3004u, mfc1_sqrt, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 8u,
          "SQRT.S result availability uses separately supplied latency");

    replay.configure({1u, 3u, 7u, 11u, 4u, 8u,
                      2u, 5u, 8u, 5u, 6u, 7u, 8u, 14u});
    check(replay.observe(record(1u, 0x3000u, rsqrt_s, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x3004u, mfc1_rsqrt, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 14u,
          "RSQRT.S result availability is independent of DIV.S and SQRT.S");

    replay.configure({1u, 3u, 7u, 11u, 4u, 8u});
    const uint32_t dependent_add = (6u << 21u) | (7u << 16u) |
                                   (8u << 11u) | 0x21u;
    check(replay.observe(record(1u, 0x3000u, mult, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x3004u, dependent_add, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 7u,
          "MULT rd result cannot inherit unverified fast ALU latency");

    // Dedicated result clocks apply to HI/LO-to-GPR and MAC-to-GPR paths.
    // Otherwise a later GPR consumer would incorrectly see one-cycle ALU
    // availability despite explicit long-latency special-register work.
    replay.configure({1u, 3u, 7u, 11u, 4u, 8u});
    const uint32_t mfhi_rd6 = (6u << 11u) | 0x10u;
    check(replay.observe(record(1u, 0x4000u, mult, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x4004u, mfhi_rd6, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(3u, 0x4008u, dependent_add, 0u, 3u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 11u,
          "MFHI GPR result uses explicit HI/LO transfer availability");

    replay.configure({1u, 3u, 7u, 11u, 4u, 8u});
    const uint32_t madd_rd6 = (0x1cu << 26u) | (4u << 21u) |
                              (5u << 16u) | (6u << 11u);
    check(replay.observe(record(1u, 0x4000u, madd_rd6, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x4004u, dependent_add, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 8u,
          "MADD rd does not inherit fast generic scalar ALU timing");

    // The legacy offline candidate accepts explicit result latency alone
    // for compatibility; strict replay must reject an operation lacking
    // its separately configured functional-unit initiation interval.
    ee::s_ee_issue_replay_profile throughput_profile{
        1u, 3u, 7u, 11u, 4u, 8u,
        2u, 5u, 14u, 9u, 6u, 7u, 8u, 14u
    };
    throughput_profile.m_require_throughput = true;
    replay.configure(throughput_profile);
    check(replay.observe(record(1u, 0x5000u, mult, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "strict throughput rejects MULT with an unavailable interval");

    throughput_profile.m_mac_multiply_interval = 3u;
    replay.configure(throughput_profile);
    check(replay.observe(record(1u, 0x5000u, mult, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 0u,
          "strict throughput admits MULT only with explicit MAC interval");

    replay.configure(throughput_profile);
    check(replay.observe(record(1u, 0x5000u, sqrt_s, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "strict throughput rejects SQRT.S without its initiation interval");

    throughput_profile.m_cop1_sqrt_interval = 9u;
    replay.configure(throughput_profile);
    const uint32_t sqrt_s_fd6 = (sqrt_s & ~(0x1fu << 6u)) | (6u << 6u);
    check(replay.observe(record(1u, 0x5000u, sqrt_s, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x5004u, sqrt_s_fd6, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 9u,
          "COP1 SQRT.S initiation interval is independent of FPR result latency");

    throughput_profile.m_cop1_rsqrt_interval = 13u;
    replay.configure(throughput_profile);
    check(replay.observe(record(1u, 0x5000u, sqrt_s, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x5004u, rsqrt_s, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 9u,
          "SQRT.S and RSQRT.S conservatively share COP1 scalar unit");

    replay.configure(throughput_profile);
    check(replay.observe(record(1u, 0x5000u, 0x24080001u, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x5004u, 0x24090001u, 2u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.candidate_cycles() == 1u,
          "strict throughput still accepts classes without a special unit");

    // Strict replay keeps conservative WAW waiting by default.
    // An explicit counterfactual overlap policy can be used to compare
    // issue intervals and result deadlines without claiming EE hardware
    // retirement semantics.
    throughput_profile.m_mac_multiply_interval = 2u;
    throughput_profile.m_cop1_arithmetic_interval = 2u;
    throughput_profile.m_require_throughput = true;
    throughput_profile.m_allow_ordered_waw_overlap = false;
    replay.configure(throughput_profile);
    check(replay.observe(record(1u, 0x6000u, mult, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x6004u, mult, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 7u,
          "default strict replay conservatively waits for duplicate MULT rd/HI/LO");

    throughput_profile.m_allow_ordered_waw_overlap = true;
    replay.configure(throughput_profile);
    check(replay.observe(record(1u, 0x6000u, mult, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x6004u, mult, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 2u &&
          replay.last_candidate_completion_cycle() == 9u &&
          replay.max_candidate_completion_cycle() == 9u &&
          replay.observe(record(3u, 0x6008u, mflo, 0u, 3u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 9u,
          "opt-in MAC overlap preserves latest HI/LO completion for MFLO");

    replay.configure(throughput_profile);
    check(replay.observe(record(1u, 0x6000u, ceq_s, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x6004u, ceq_s, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 2u &&
          replay.last_candidate_completion_cycle() == 9u &&
          replay.observe(record(3u, 0x6008u, bc1t, 0u, 3u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 9u,
          "overlapping comparisons retain FCC0 dependency before BC1T");

    throughput_profile.m_fpu_control_latency = 2u;
    replay.configure(throughput_profile);
    check(replay.observe(record(1u, 0x6000u, ceq_s, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x6004u, ctc1_fcr31, 0u, 2u)) ==
          e_ee_issue_replay_status::INVALID_SLOT &&
          !replay.usable_prefix() &&
          replay.failure_pc() == 0x6004u,
          "shorter FCR31 producer cannot overtake older result in ordered mode");

    // COP0 is independent of VU0; VF[n]/VI[n] share macro-mode hazard
    // identity by index, but retain different functional values.
    // All result latencies remain caller-supplied and experimental.
    const uint32_t mtc0_status = (0x10u << 26u) | (4u << 21u) |
                                 (8u << 16u) | (12u << 11u);
    const uint32_t mfc0_status = (0x10u << 26u) |
                                 (9u << 16u) | (12u << 11u);
    const uint32_t mfc0_cause = (0x10u << 26u) |
                                (9u << 16u) | (13u << 11u);
    const uint32_t qmtc2_vf4 = (0x12u << 26u) | (5u << 21u) |
                               (8u << 16u) | (4u << 11u);
    const uint32_t qmfc2_vf4 = (0x12u << 26u) | (1u << 21u) |
                               (9u << 16u) | (4u << 11u);
    const uint32_t ctc2_vi4 = (0x12u << 26u) | (6u << 21u) |
                              (8u << 16u) | (4u << 11u);
    const uint32_t cfc2_vi4 = (0x12u << 26u) | (2u << 21u) |
                              (9u << 16u) | (4u << 11u);
    const uint32_t cfc2_vi5 = (0x12u << 26u) | (2u << 21u) |
                              (9u << 16u) | (5u << 11u);
    const uint32_t qmtc2_vf0 = (0x12u << 26u) | (5u << 21u) |
                               (8u << 16u);
    const uint32_t qmfc2_vf0 = (0x12u << 26u) | (1u << 21u) |
                               (9u << 16u);
    const uint32_t ctc2_vi0 = (0x12u << 26u) | (6u << 21u) |
                              (8u << 16u);
    ee::s_ee_issue_replay_profile transfer_profile{1u, 3u};

    replay.configure(transfer_profile);
    check(replay.observe(record(1u, 0x7000u, mtc0_status, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "COP0 transfers require an explicitly configured latency");

    transfer_profile.m_cop0_transfer_latency = 6u;
    replay.configure(transfer_profile);
    check(replay.observe(record(1u, 0x7000u, mtc0_status, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x7004u, mfc0_status, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 6u &&
          replay.last_candidate_completion_cycle() == 12u,
          "MTC0/MFC0 share CP0 status dependencies and explicit latency");

    replay.configure(transfer_profile);
    check(replay.observe(record(1u, 0x7000u, mtc0_status, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x7004u, mfc0_cause, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 1u,
          "different COP0 registers do not inherit status RAW stalls");

    replay.configure(transfer_profile);
    check(replay.observe(record(1u, 0x7000u, qmtc2_vf4, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "VU0 VF transfer cannot borrow COP0 or integer latency");

    transfer_profile.m_vu0_vf_transfer_latency = 5u;
    transfer_profile.m_vu0_vi_transfer_latency = 7u;
    replay.configure(transfer_profile);
    check(replay.observe(record(1u, 0x7000u, qmtc2_vf4, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x7004u, qmfc2_vf4, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 5u &&
          replay.last_candidate_completion_cycle() == 10u,
          "QMTC2/QMFC2 preserve the VF4 register dependency");

    replay.configure(transfer_profile);
    check(replay.observe(record(1u, 0x7000u, ctc2_vi4, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x7004u, cfc2_vi4, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 7u,
          "CTC2/CFC2 preserve independent VI4 register dependencies");

    replay.configure(transfer_profile);
    check(replay.observe(record(1u, 0x7000u, qmtc2_vf4, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x7004u, cfc2_vi4, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 5u,
          "VU0 VF4 writes block VI4 reads by shared macro hazard identity");

    replay.configure(transfer_profile);
    check(replay.observe(record(1u, 0x7000u, ctc2_vi4, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x7004u, qmfc2_vf4, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 7u,
          "VU0 VI4 writes block VF4 reads by shared macro hazard identity");

    replay.configure(transfer_profile);
    check(replay.observe(record(1u, 0x7000u, qmtc2_vf4, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x7004u, cfc2_vi5, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 1u,
          "VU0 VF4 does not falsely alias different-index VI5");

    replay.configure(transfer_profile);
    check(replay.observe(record(1u, 0x7000u, qmtc2_vf0, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x7004u, qmfc2_vf0, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 1u,
          "VF00 register does not create a macro transfer RAW hazard");

    replay.configure(transfer_profile);
    check(replay.observe(record(1u, 0x7000u, ctc2_vi0, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x7004u, qmfc2_vf0, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 1u,
          "VI00 register does not create a macro cross-bank hazard");

    // Packed MMI accesses full 128-bit GPR values. Replaying any of
    // these decoded operations requires an independent, explicit profile.
    // Numeric test latencies below are synthetic, never a game clock.
    const auto mmi = [](uint32_t rs, uint32_t rt, uint32_t rd,
                        uint32_t sa, uint32_t funct) -> uint32_t
    {
        return (0x1cu << 26u) | (rs << 21u) | (rt << 16u) |
               (rd << 11u) | (sa << 6u) | funct;
    };
    const uint32_t mmi_paddw = mmi(8u, 9u, 10u, 0u, 0x08u);
    const uint32_t mmi_psubh = mmi(10u, 11u, 12u, 5u, 0x08u);
    const uint32_t mmi_pabsw = mmi(0u, 10u, 13u, 1u, 0x28u);
    const uint32_t mmi_pextuw = mmi(10u, 11u, 14u, 18u, 0x28u);
    const uint32_t mmi_pcpyld = mmi(10u, 11u, 15u, 14u, 0x09u);
    const uint32_t mmi_pcpyud = mmi(10u, 11u, 16u, 14u, 0x29u);
    const uint32_t mmi_pcpyh = mmi(0u, 10u, 17u, 27u, 0x29u);
    const uint32_t mmi_qfsrv = mmi(10u, 11u, 18u, 27u, 0x28u);
    const uint32_t mmi_pcgtw = mmi(8u, 9u, 10u, 2u, 0x08u);
    const uint32_t mmi_ppacb = mmi(8u, 9u, 10u, 27u, 0x08u);
    const uint32_t mmi_psrlh = mmi(0u, 9u, 10u, 15u, 0x36u);
    const uint32_t mmi_plzcw = mmi(8u, 0u, 10u, 0u, 0x04u);
    const uint32_t mmi_unsupported = mmi(8u, 9u, 10u, 11u, 0x08u);
    ee::s_ee_issue_replay_profile packed_profile{1u, 3u};

    replay.configure(packed_profile);
    check(replay.observe(record(1u, 0x7100u, mmi_paddw, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION &&
          !replay.usable_prefix(),
          "packed MMI cannot use an implicit scalar integer latency");

    packed_profile.m_packed_mmi_latency = 5u;
    replay.configure(packed_profile);
    check(replay.observe(record(1u, 0x7100u, mmi_paddw, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x7104u, mmi_psubh, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 5u &&
          replay.last_candidate_completion_cycle() == 10u,
          "PADDW full-register producer feeds PSUBH on all packed lanes");

    replay.configure(packed_profile);
    const uint32_t addiu_from_packed =
        (0x09u << 26u) | (10u << 21u) | (19u << 16u) | 1u;
    check(replay.observe(record(1u, 0x7100u, mmi_paddw, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x7104u, addiu_from_packed, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 5u,
          "scalar GPR reader respects packed full-128 producer");

    replay.configure(packed_profile);
    check(replay.observe(record(1u, 0x7100u, mmi_paddw, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x7104u, mmi_paddw, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 5u,
          "packed same-destination WAW respects caller result deadline");

    replay.configure(packed_profile);
    const uint32_t packed_families[] = {
        mmi_pabsw, mmi_pextuw, mmi_pcpyld, mmi_pcpyud, mmi_pcpyh,
        mmi_pcgtw, mmi_ppacb, mmi_psrlh, mmi_plzcw
    };
    bool family_covered = true;
    for (const uint32_t opcode : packed_families)
    {
        replay.configure(packed_profile);
        family_covered &= replay.observe(
            record(1u, 0x7100u, opcode, 0u, 1u)) ==
            e_ee_issue_replay_status::ISSUED;
    }
    check(family_covered,
          "decoder-verified MMI, MMI0/1/2/3, shifts and PLZCW use explicit packed profile");

    replay.configure(packed_profile);
    // MMI2 variable-word shifts read both 128-bit sources, write a
    // 128-bit destination, and never reserve the HI/LO banks.
    const uint32_t mmi_psllvw = mmi(8u, 9u, 10u, 2u, 0x09u);
    const uint32_t mmi_psrlvw = mmi(8u, 9u, 10u, 3u, 0x09u);
    const uint32_t mmi_psllvw_read_prior = mmi(10u, 9u, 11u, 2u, 0x09u);
    const uint32_t variable_readers[] = {mmi_psllvw, mmi_psrlvw};
    bool variable_producers_profiled = true;
    for (const uint32_t opcode : variable_readers)
    {
        replay.configure(packed_profile);
        variable_producers_profiled &=
            replay.observe(record(1u, 0x7150u, opcode, 0u, 1u)) ==
                e_ee_issue_replay_status::ISSUED &&
            replay.observe(record(2u, 0x7154u, addiu_from_packed, 0u, 2u)) ==
                e_ee_issue_replay_status::ISSUED &&
            replay.last_event_candidate_cycle() == 5u;
    }
    check(variable_producers_profiled,
          "PSLLVW/PSRLVW publish full-128 GPR result at packed latency");
    replay.configure(packed_profile);
    check(replay.observe(record(1u, 0x7150u, mmi_paddw, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x7154u,
              mmi_psllvw_read_prior, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 5u,
          "PSLLVW reads prior 128-bit rs rather than an immediate shift");
    packed_profile.m_packed_mmi_latency = 0u;
    bool unprofiled_variable_shifts = true;
    for (const uint32_t opcode : variable_readers)
    {
        // An UNMODELED_INSTRUCTION halts the replay until reconfigured.
        // Each unsupported instruction needs an independently fresh trace.
        replay.configure(packed_profile);
        unprofiled_variable_shifts &=
            replay.observe(record(1u, 0x7150u, opcode, 0u, 1u)) ==
                e_ee_issue_replay_status::UNMODELED_INSTRUCTION;
    }
    check(unprofiled_variable_shifts,
          "variable shifts require explicit packed latency");
    packed_profile.m_packed_mmi_latency = 5u;
    replay.configure(packed_profile);

    const uint32_t mmi_zero_dest = mmi(8u, 9u, 0u, 0u, 0x08u);
    check(replay.observe(record(1u, 0x7100u, mmi_zero_dest, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x7104u,
              (0x09u << 26u) | (0u << 21u) | (19u << 16u) | 1u,
              0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 1u,
          "packed GPR0 destination does not create artificial zero hazards");

    // PMTHL modifies the low words of all four HI/LO halves. PMFHL
    // consumes those same halves and produces a full 128-bit GPR value.
    // Both require an explicit transfer latency; never infer hardware
    // cycles from the generic packed or scalar integer candidate.
    const uint32_t mmi_pmthl = mmi(10u, 0u, 0u, 0u, 0x31u);
    const uint32_t mmi_pmfhl = mmi(0u, 0u, 11u, 4u, 0x30u);
    replay.configure(packed_profile);
    check(replay.observe(record(1u, 0x7200u, mmi_pmthl, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "PMTHL cannot invent an HI/LO transfer latency");

    packed_profile.m_hilo_transfer_latency = 7u;
    replay.configure(packed_profile);
    check(replay.observe(record(1u, 0x7200u, mmi_pmthl, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x7204u, mmi_pmfhl, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 7u,
          "PMTHL partial HI/LO producer stalls PMFHL on all four banks");
    packed_profile.m_hilo_transfer_latency = 0u;

    // Full-bank MMI2/MMI3 HI/LO transfers require explicit transfer
    // latency, and packed multiply/divide require a separately provided
    // packed-MMI profile in addition to their HI/LO result latency.
    const uint32_t mmi_pmthi = mmi(10u, 0u, 0u, 8u, 0x29u);
    const uint32_t mmi_pmfhi = mmi(0u, 0u, 11u, 8u, 0x09u);
    const uint32_t mmi_pmtlo = mmi(10u, 0u, 0u, 9u, 0x29u);
    const uint32_t mmi_pmflo = mmi(0u, 0u, 11u, 9u, 0x09u);
    const uint32_t mmi_pmultw = mmi(8u, 9u, 10u, 12u, 0x09u);
    const uint32_t mmi_pmultuw = mmi(8u, 9u, 10u, 12u, 0x29u);
    const uint32_t mmi_pdivw = mmi(8u, 9u, 0u, 13u, 0x09u);
    const uint32_t mmi_pdivuw = mmi(8u, 9u, 0u, 13u, 0x29u);

    packed_profile.m_hilo_transfer_latency = 0u;
    replay.configure(packed_profile);
    check(replay.observe(record(1u, 0x7300u, mmi_pmthi, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "PMTHI requires independently supplied HI/LO transfer latency");

    packed_profile.m_hilo_transfer_latency = 7u;
    for (const auto pair :
        {std::pair<uint32_t,uint32_t>{mmi_pmthi, mmi_pmfhi},
         std::pair<uint32_t,uint32_t>{mmi_pmtlo, mmi_pmflo}})
    {
        replay.configure(packed_profile);
        check(replay.observe(record(1u, 0x7300u, pair.first, 0u, 1u)) ==
              e_ee_issue_replay_status::ISSUED &&
              replay.observe(record(2u, 0x7304u, pair.second, 0u, 2u)) ==
              e_ee_issue_replay_status::ISSUED &&
              replay.last_event_candidate_cycle() == 7u,
              "MMI3 full-bank write delays MMI2 full-bank read");
    }

    packed_profile.m_hilo_multiply_latency = 0u;
    replay.configure(packed_profile);
    check(replay.observe(record(1u, 0x7300u, mmi_pmultw, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "PMULTW has no implicit scalar or packed arithmetic timing");

    packed_profile.m_hilo_multiply_latency = 9u;
    bool multiply_profiled = true;
    for (const uint32_t opcode : {mmi_pmultw, mmi_pmultuw})
    {
        replay.configure(packed_profile);
        multiply_profiled &=
            replay.observe(record(1u, 0x7300u, opcode, 0u, 1u)) ==
                e_ee_issue_replay_status::ISSUED &&
            replay.observe(record(2u, 0x7304u, mmi_pmfhi, 0u, 2u)) ==
                e_ee_issue_replay_status::ISSUED &&
            replay.last_event_candidate_cycle() == 9u;
    }
    check(multiply_profiled,
          "PMULTW/PMULTUW write both HI banks and block PMFHI until ready");

    packed_profile.m_hilo_divide_latency = 11u;
    bool divide_profiled = true;
    for (const uint32_t opcode : {mmi_pdivw, mmi_pdivuw})
    {
        replay.configure(packed_profile);
        divide_profiled &=
            replay.observe(record(1u, 0x7300u, opcode, 0u, 1u)) ==
                e_ee_issue_replay_status::ISSUED &&
            replay.observe(record(2u, 0x7304u, mmi_pmflo, 0u, 2u)) ==
                e_ee_issue_replay_status::ISSUED &&
            replay.last_event_candidate_cycle() == 11u;
    }
    check(divide_profiled,
          "PDIVW/PDIVUW write both LO banks and block PMFLO until ready");
    // PDIVBW produces four independent quotient and remainder words
    // across the full LO/HI banks, has no GPR destination, and requires
    // an explicitly supplied packed/HI-LO division latency.
    const uint32_t mmi_pdivbw = mmi(8u, 9u, 0u, 0x1du, 0x09u);
    packed_profile.m_hilo_divide_latency = 0u;
    replay.configure(packed_profile);
    check(replay.observe(record(1u, 0x7350u, mmi_pdivbw, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "PDIVBW has no inferred divide timing");
    packed_profile.m_hilo_divide_latency = 11u;
    bool divbw_profiled = true;
    for (const uint32_t hilo_read : {mmi_pmflo, mmi_pmfhi})
    {
        replay.configure(packed_profile);
        divbw_profiled &=
            replay.observe(record(1u, 0x7350u, mmi_pdivbw, 0u, 1u)) ==
                e_ee_issue_replay_status::ISSUED &&
            replay.observe(record(2u, 0x7354u, hilo_read, 0u, 2u)) ==
                e_ee_issue_replay_status::ISSUED &&
            replay.last_event_candidate_cycle() == 11u;
    }
    check(divbw_profiled,
          "PDIVBW full-bank quotient/remainder RAW dependencies");
    packed_profile.m_hilo_transfer_latency = 0u;
    packed_profile.m_hilo_multiply_latency = 0u;
    packed_profile.m_hilo_divide_latency = 0u;

    const uint32_t mmi_pmaddw = mmi(8u, 9u, 10u, 0u, 0x09u);
    const uint32_t mmi_pmsubw = mmi(8u, 9u, 10u, 4u, 0x09u);
    const uint32_t mmi_pmadduw = mmi(8u, 9u, 10u, 0u, 0x29u);
    packed_profile.m_hilo_accumulate_latency = 0u;
    replay.configure(packed_profile);
    check(replay.observe(record(1u, 0x7300u, mmi_pmaddw, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "packed MAC cannot infer accumulator timing");
    packed_profile.m_hilo_accumulate_latency = 13u;
    packed_profile.m_hilo_transfer_latency = 7u;
    bool mac_profiled = true;
    for (const uint32_t opcode : {mmi_pmaddw, mmi_pmsubw, mmi_pmadduw})
    {
        replay.configure(packed_profile);
        mac_profiled &=
            replay.observe(record(1u, 0x7300u, opcode, 0u, 1u)) ==
                e_ee_issue_replay_status::ISSUED &&
            replay.observe(record(2u, 0x7304u, mmi_pmfhi, 0u, 2u)) ==
                e_ee_issue_replay_status::ISSUED &&
            replay.last_event_candidate_cycle() == 13u;
    }
    check(mac_profiled, "packed MAC four-bank RAW timing dependencies");
    packed_profile.m_hilo_accumulate_latency = 0u;
    packed_profile.m_hilo_transfer_latency = 0u;

    // Halfword MMI2 variants touch every 32-bit word of both HI/LO
    // banks. The two accumulating forms also read the original banks;
    // horizontal and multiply forms do not.
    const uint32_t halfword_ops[] = {0x10u, 0x11u, 0x14u, 0x15u, 0x1cu};
    packed_profile.m_hilo_transfer_latency = 7u;
    packed_profile.m_hilo_multiply_latency = 0u;
    packed_profile.m_hilo_accumulate_latency = 0u;
    replay.configure(packed_profile);
    check(replay.observe(record(1u, 0x7400u,
          mmi(8u, 9u, 10u, 0x10u, 0x09u), 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "PMADDH cannot guess an accumulator latency");

    packed_profile.m_hilo_multiply_latency = 9u;
    packed_profile.m_hilo_accumulate_latency = 13u;
    bool halfword_profiled = true;
    for (uint32_t subop : halfword_ops)
    {
        const uint32_t opcode = mmi(8u, 9u, 10u, subop, 0x09u);
        const uint32_t latency = (subop == 0x10u || subop == 0x14u)
            ? 13u : 9u;
        replay.configure(packed_profile);
        halfword_profiled &=
            replay.observe(record(1u, 0x7400u, opcode, 0u, 1u)) ==
                e_ee_issue_replay_status::ISSUED &&
            replay.observe(record(2u, 0x7404u, mmi_pmflo, 0u, 2u)) ==
                e_ee_issue_replay_status::ISSUED &&
            replay.last_event_candidate_cycle() == latency;
    }
    check(halfword_profiled,
          "halfword MMI2 writes all HI/LO halves and produces rd");

    bool halfword_read_dependence = true;
    for (uint32_t subop : {0x10u, 0x14u})
    {
        replay.configure(packed_profile);
        halfword_read_dependence &=
            replay.observe(record(1u, 0x7500u, mmi_pmthi, 0u, 1u)) ==
                e_ee_issue_replay_status::ISSUED &&
            replay.observe(record(2u, 0x7504u,
                mmi(8u, 9u, 10u, subop, 0x09u), 0u, 2u)) ==
                e_ee_issue_replay_status::ISSUED &&
            replay.last_event_candidate_cycle() == 7u;
    }
    check(halfword_read_dependence,
          "PMADDH/PMSUBH read four existing HI/LO halves");
    packed_profile.m_hilo_transfer_latency = 0u;
    packed_profile.m_hilo_multiply_latency = 0u;
    packed_profile.m_hilo_accumulate_latency = 0u;

    packed_profile.m_allow_ordered_waw_overlap = true;
    replay.configure(packed_profile);
    check(replay.observe(record(1u, 0x7100u, mmi_paddw, 0u, 1u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.observe(record(2u, 0x7104u, mmi_paddw, 0u, 2u)) ==
          e_ee_issue_replay_status::ISSUED &&
          replay.last_event_candidate_cycle() == 1u,
          "explicit candidate ordered-overlap remains opt-in for packed WAW");
    packed_profile.m_allow_ordered_waw_overlap = false;

    replay.configure(packed_profile);
    check(replay.observe(record(1u, 0x7100u, mmi_qfsrv, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "QFSRV requires independently modeled SA register hazards");
    replay.configure(packed_profile);
    check(replay.observe(record(1u, 0x7100u, mmi_unsupported, 0u, 1u)) ==
          e_ee_issue_replay_status::UNMODELED_INSTRUCTION,
          "unknown packed sub-op does not gain fabricated timing");

    // Known valid system / VU0 macro / unprofiled MMI instructions still
    // require a separate evidence-backed timing class. This extension
    // must not turn them into automatically timed scalar operations.
    const uint32_t eret = 0x42000018u;
    const uint32_t ei = 0x42000038u;
    const uint32_t bc2t = (0x12u << 26u) | (8u << 21u) |
                          (1u << 16u) | 1u;
    const uint32_t vmaddw = (0x12u << 26u) | (0x10u << 21u) | 0x0bu;
    const uint32_t paddw = (0x1cu << 26u) | (4u << 21u) |
                           (5u << 16u) | (6u << 11u) | 0x08u;
    const uint32_t cache_op = (0x2fu << 26u) | (8u << 21u);
    const uint32_t unmodeled_opcodes[] = {
        eret, ei, bc2t, vmaddw, paddw, cache_op
    };
    for (const uint32_t opcode : unmodeled_opcodes)
    {
        replay.configure(transfer_profile);
        check(replay.observe(record(1u, 0x7000u, opcode, 0u, 1u)) ==
              e_ee_issue_replay_status::UNMODELED_INSTRUCTION &&
              !replay.usable_prefix(),
              "system transition, VU0 macro, packed MMI and cache fail closed");
    }

    std::printf("ee_issue_trace_replay_test: %d failure(s)\n", s_failures);
    return s_failures == 0 ? 0 : 1;
}
