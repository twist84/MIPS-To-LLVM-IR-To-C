#include "ee_issue_timing_probe.h"

#include <cstdint>
#include <cstdio>
#include <limits>

namespace
{
int g_failures = 0;

void check(bool value, const char* label)
{
    if (!value)
    {
        std::printf("FAIL: %s\n", label);
        ++g_failures;
    }
}
}

int main()
{
    using ee::c_ee_issue_timing_probe;
    using ee::e_ee_probe_pipe;
    using ee::s_ee_probe_instruction;

    c_ee_issue_timing_probe clock{};
    uint64_t at = 99u;
    const s_ee_probe_instruction a{1u << 1u, 1u << 2u};
    const s_ee_probe_instruction b{1u << 3u, 1u << 4u};
    const s_ee_probe_instruction c{1u << 5u, 1u << 6u};

    check(clock.issue(a, &at) && at == 0u &&
          clock.issue(b, &at) && at == 0u &&
          clock.issue(c, &at) && at == 1u &&
          clock.estimated_cycles() == 2u &&
          clock.instructions_issued() == 3u,
          "independent integer instructions can pair twice per cycle");

    clock.reset();
    const s_ee_probe_instruction dependency{1u << 2u, 1u << 7u};
    check(clock.issue(a) && clock.issue(dependency, &at) &&
          at == 1u && clock.estimated_cycles() == 2u,
          "RAW dependency prevents same-cycle dual issue");

    clock.reset();
    const s_ee_probe_instruction load{
        1u << 1u, 1u << 2u, e_ee_probe_pipe::LOAD, 3u
    };
    check(clock.issue(load, &at) && at == 0u &&
          clock.issue(dependency, &at) && at == 3u &&
          clock.estimated_cycles() == 4u,
          "caller-declared load latency stalls a dependent instruction");

    clock.reset();
    const s_ee_probe_instruction branch{
        1u << 1u, 0u, e_ee_probe_pipe::BRANCH, 1u
    };
    check(clock.issue(branch, &at) && at == 0u &&
          clock.reserve_annulled_delay_slot() &&
          clock.issue(b, &at) && at == 2u &&
          clock.instructions_issued() == 2u &&
          clock.annulled_slots_reserved() == 1u,
          "annulled branch-likely slot reserves timing without execution");

    clock.reset();
    check(clock.issue(branch) &&
          clock.issue(a, &at) && at == 0u &&
          clock.estimated_cycles() == 1u &&
          clock.annulled_slots_reserved() == 0u,
          "branch and independent executed delay ALU may dual issue");

    clock.reset();
    check(clock.issue(branch) &&
          clock.charge_hle_overhead(300u) &&
          clock.issue(a, &at) && at == 301u &&
          clock.configured_hle_overhead() == 300u &&
          clock.annulled_slots_reserved() == 0u,
          "configured HLE overhead is separate from issue and annul slots");

    check(clock.estimated_cycles() == 302u &&
          !clock.probe_budget_reached(0u) &&
          !clock.probe_budget_reached(303u) &&
          clock.probe_budget_reached(302u),
          "probe budget is a read-only diagnostic, never a VBlank producer");

    clock.reset();
    check(!clock.issue(s_ee_probe_instruction{
              0u, 1u << 3u, e_ee_probe_pipe::INTEGER, 0u
          }) &&
          clock.estimated_cycles() == 0u &&
          clock.instructions_issued() == 0u,
          "invalid zero-result-latency instruction is atomic");

    check(clock.issue(s_ee_probe_instruction{
              0u, 1u << 3u, e_ee_probe_pipe::INTEGER, 10u
          }) &&
          clock.issue(s_ee_probe_instruction{
              1u << 3u, 0u, e_ee_probe_pipe::INTEGER, 1u
          }, &at) && at == 10u,
          "scoreboard observes explicitly provided long latency");

    clock.reset();
    check(clock.issue(a) &&
          clock.charge_hle_overhead(
              std::numeric_limits<uint32_t>::max()) &&
          clock.configured_hle_overhead() ==
              std::numeric_limits<uint32_t>::max(),
          "large HLE diagnostic costs preserve 64-bit accounting");
    clock.reset();
    check(clock.estimated_cycles() == 0u &&
          clock.instructions_issued() == 0u &&
          clock.configured_hle_overhead() == 0u,
          "reset clears all probe state");

    // MULT can write a GPR result immediately while reserving HI/LO for
    // a separately configured latency. MFLO must observe the long latency.
    clock.reset();
    s_ee_probe_instruction multiply{
        (1u << 4u) | (1u << 5u), 1u << 6u,
        e_ee_probe_pipe::MULT_DIV, 1u
    };
    multiply.m_hilo_write_mask = 0x3u;
    multiply.m_hilo_result_latency = 7u;
    s_ee_probe_instruction mflo{
        0u, 1u << 7u, e_ee_probe_pipe::INTEGER, 1u
    };
    mflo.m_hilo_read_mask = 0x2u;
    check(clock.issue(multiply, &at) && at == 0u &&
          clock.issue(mflo, &at) && at == 7u &&
          clock.estimated_cycles() == 8u,
          "HI/LO dependency waits for caller-supplied result latency");

    clock.reset();
    s_ee_probe_instruction fadd{};
    fadd.m_pipe = e_ee_probe_pipe::COPROCESSOR;
    fadd.m_fpr_read_mask = (1u << 1u) | (1u << 2u);
    fadd.m_fpr_write_mask = 1u << 3u;
    fadd.m_fpr_result_latency = 4u;
    s_ee_probe_instruction fconsumer{};
    fconsumer.m_pipe = e_ee_probe_pipe::COPROCESSOR;
    fconsumer.m_fpr_read_mask = 1u << 3u;
    check(clock.issue(fadd, &at) && at == 0u &&
          clock.issue(fconsumer, &at) && at == 4u,
          "FPR scoreboard tracks explicit producer-to-consumer latency");

    clock.reset();
    s_ee_probe_instruction acc_writer{};
    acc_writer.m_pipe = e_ee_probe_pipe::COPROCESSOR;
    acc_writer.m_acc_write = true;
    acc_writer.m_acc_result_latency = 6u;
    s_ee_probe_instruction acc_reader{};
    acc_reader.m_pipe = e_ee_probe_pipe::COPROCESSOR;
    acc_reader.m_acc_read = true;
    check(clock.issue(acc_writer, &at) && at == 0u &&
          clock.issue(acc_reader, &at) && at == 6u,
          "FPU accumulator hazards use their own readiness clock");

    clock.reset();
    s_ee_probe_instruction invalid_hilo{};
    invalid_hilo.m_hilo_read_mask = 0x10u;
    check(!clock.issue(invalid_hilo) &&
          clock.instructions_issued() == 0u &&
          clock.estimated_cycles() == 0u,
          "invalid special-register masks are rejected atomically");

    clock.reset();
    s_ee_probe_instruction indep_hilo{};
    indep_hilo.m_hilo_write_mask = 0x1u;
    s_ee_probe_instruction dependent_hilo{};
    dependent_hilo.m_hilo_read_mask = 0x1u;
    check(clock.issue(indep_hilo, &at) && at == 0u &&
          clock.issue(dependent_hilo, &at) && at == 1u,
          "HI/LO same-cycle pairing observes RAW hazards");

    // Source: EE Core User's Manual v6.0, Tables 1-2 and 1-3.
    // All timings below remain synthetic candidates.
    clock.reset();
    check(clock.issue(load, &at) && at == 0u &&
          clock.issue(b, &at) && at == 0u &&
          clock.estimated_cycles() == 1u,
          "Pipe1 load and independent Pipe0 ALU can dual issue");

    clock.reset();
    s_ee_probe_instruction mac0{};
    mac0.m_pipe = e_ee_probe_pipe::MULT_DIV;
    s_ee_probe_instruction mac1{};
    mac1.m_pipe = e_ee_probe_pipe::MULT_DIV_1;
    check(clock.issue(mac0, &at) && at == 0u &&
          clock.issue(mac1, &at) && at == 0u,
          "independent MAC0 and MAC1 can use both logical pipes");

    clock.reset();
    check(clock.issue(branch, &at) && at == 0u &&
          clock.issue(branch, &at) && at == 1u,
          "Table 1-3 excludes branch plus branch pairing");

    clock.reset();
    s_ee_probe_instruction fpu_op{};
    fpu_op.m_pipe = e_ee_probe_pipe::COPROCESSOR_OP;
    s_ee_probe_instruction fpu_move{};
    fpu_move.m_pipe = e_ee_probe_pipe::COPROCESSOR_MOVE;
    check(clock.issue(fpu_op, &at) && at == 0u &&
          clock.issue(fpu_move, &at) && at == 1u,
          "COP1 operate/move Y resource-stall pairing stays unmodeled");

    clock.reset();
    s_ee_probe_instruction invalid_pipe{};
    invalid_pipe.m_pipe = static_cast<e_ee_probe_pipe>(255u);
    check(!clock.issue(invalid_pipe) && clock.instructions_issued() == 0u,
          "unrecognized issue class fails atomically");

    // An outstanding write also reserves its architectural destination.
    // Conservative WAW ordering applies across issue cycles and to FCR31.
    clock.reset();
    s_ee_probe_instruction long_gpr{};
    long_gpr.m_gpr_write_mask = 1u << 9u;
    long_gpr.m_result_latency = 8u;
    s_ee_probe_instruction replace_gpr{};
    replace_gpr.m_gpr_write_mask = 1u << 9u;
    check(clock.issue(long_gpr, &at) && at == 0u &&
          clock.issue(replace_gpr, &at) && at == 8u &&
          clock.estimated_cycles() == 9u,
          "long-latency GPR WAW is ordered across cycles");

    clock.reset();
    s_ee_probe_instruction fcsr_writer{};
    fcsr_writer.m_pipe = e_ee_probe_pipe::COPROCESSOR;
    fcsr_writer.m_fcsr_write = true;
    fcsr_writer.m_fcsr_result_latency = 6u;
    s_ee_probe_instruction fcsr_reader{};
    fcsr_reader.m_pipe = e_ee_probe_pipe::BRANCH;
    fcsr_reader.m_fcsr_read = true;
    check(clock.issue(fcsr_writer, &at) && at == 0u &&
          clock.issue(fcsr_reader, &at) && at == 6u &&
          clock.estimated_cycles() == 7u,
          "FCR31 compare-to-condition-branch RAW waits for availability");

    clock.reset();
    check(clock.issue(fcsr_writer, &at) && at == 0u &&
          clock.issue(fcsr_writer, &at) && at == 6u &&
          clock.estimated_cycles() == 7u,
          "FCR31 WAW cannot reorder pending condition/control writes");

    clock.reset();
    check(clock.issue(fcsr_reader, &at) && at == 0u &&
          clock.issue(fcsr_writer, &at) && at == 1u,
          "FCR31 read/write do not pair in the same probe cycle");

    // An initiation interval is not a result latency. In the isolated
    // offline probe, a later independent issue of the same functional unit
    // must wait for its *throughput* reservation, even with no RAW hazard.
    clock.reset();
    s_ee_probe_instruction mac0_unit{};
    mac0_unit.m_pipe = e_ee_probe_pipe::MULT_DIV;
    mac0_unit.m_unit = ee::e_ee_probe_unit::MAC0;
    mac0_unit.m_initiation_interval = 4u;
    s_ee_probe_instruction mac1_unit{};
    mac1_unit.m_pipe = e_ee_probe_pipe::MULT_DIV_1;
    mac1_unit.m_unit = ee::e_ee_probe_unit::MAC1;
    mac1_unit.m_initiation_interval = 2u;
    check(clock.issue(mac0_unit, &at) && at == 0u &&
          clock.issue(mac1_unit, &at) && at == 0u &&
          clock.issue(mac0_unit, &at) && at == 4u &&
          clock.issue(mac1_unit, &at) && at == 4u &&
          clock.instructions_issued() == 4u &&
          clock.estimated_cycles() == 5u,
          "MAC0 and MAC1 have independent initiation reservations");

    clock.reset();
    s_ee_probe_instruction fpu_unit{};
    fpu_unit.m_pipe = e_ee_probe_pipe::COPROCESSOR_OP;
    fpu_unit.m_unit = ee::e_ee_probe_unit::COP1_SCALAR;
    fpu_unit.m_initiation_interval = 9u;
    check(clock.issue(fpu_unit, &at) && at == 0u &&
          clock.issue(fpu_unit, &at) && at == 9u &&
          clock.issue(fpu_unit, &at) && at == 18u,
          "independent COP1 operations share explicit unit initiation interval");

    clock.reset();
    check(clock.issue(fpu_unit, &at) && at == 0u &&
          clock.charge_hle_overhead(3u) &&
          clock.issue(fpu_unit, &at) && at == 9u,
          "explicit HLE host boundary does not discard outstanding occupancy");

    clock.reset();
    s_ee_probe_instruction missing_interval = fpu_unit;
    missing_interval.m_initiation_interval = 0u;
    s_ee_probe_instruction orphan_interval = fpu_unit;
    orphan_interval.m_unit = ee::e_ee_probe_unit::NONE;
    s_ee_probe_instruction invalid_unit = fpu_unit;
    invalid_unit.m_unit = static_cast<ee::e_ee_probe_unit>(255u);
    check(!clock.issue(missing_interval) &&
          !clock.issue(orphan_interval) &&
          !clock.issue(invalid_unit) &&
          clock.instructions_issued() == 0u &&
          clock.estimated_cycles() == 0u &&
          clock.issue(fpu_unit, &at) && at == 0u,
          "invalid or absent unit throughput is rejected atomically");

    clock.reset();
    check(clock.issue(fpu_unit, &at) && at == 0u &&
          clock.issue(fpu_unit, &at) && at == 9u,
          "reset clears all unit reservations");

    // Counterfactual result overlap is opt-in, separate from real EE
    // pipeline/retirement modeling. A second same-register producer may
    // issue at the explicitly supplied unit interval, but its result
    // must not overtake the first producer's completion deadline.
    clock.reset();
    s_ee_probe_instruction overlapping_mac{};
    overlapping_mac.m_pipe = e_ee_probe_pipe::MULT_DIV;
    overlapping_mac.m_unit = ee::e_ee_probe_unit::MAC0;
    overlapping_mac.m_initiation_interval = 2u;
    overlapping_mac.m_hilo_write_mask = 0x3u;
    overlapping_mac.m_hilo_result_latency = 8u;
    uint64_t completion = 99u;
    check(clock.issue(overlapping_mac, &at, &completion) && at == 0u &&
          completion == 8u &&
          clock.issue(overlapping_mac, &at, &completion) && at == 8u &&
          completion == 16u,
          "conservative repeated MAC waits for prior HI/LO completion");

    clock.reset();
    check(clock.configure_waw_policy(
              ee::e_ee_probe_waw_policy::ORDERED_OVERLAP) &&
          clock.issue(overlapping_mac, &at, &completion) && at == 0u &&
          completion == 8u &&
          clock.issue(overlapping_mac, &at, &completion) && at == 2u &&
          completion == 10u &&
          clock.last_completion_cycle() == 10u &&
          clock.max_completion_cycle() == 10u &&
          clock.estimated_cycles() == 3u,
          "ordered-overlap separates MAC throughput from result completion");
    s_ee_probe_instruction read_lo{};
    read_lo.m_pipe = e_ee_probe_pipe::MULT_DIV;
    read_lo.m_hilo_read_mask = 0x2u;
    check(clock.issue(read_lo, &at) && at == 10u,
          "consumer waits for latest overlapping HI/LO result");

    clock.reset();
    s_ee_probe_instruction overlapping_fcsr{};
    overlapping_fcsr.m_pipe = e_ee_probe_pipe::COPROCESSOR_OP;
    overlapping_fcsr.m_unit = ee::e_ee_probe_unit::COP1_SCALAR;
    overlapping_fcsr.m_initiation_interval = 2u;
    overlapping_fcsr.m_fcsr_write = true;
    overlapping_fcsr.m_fcsr_result_latency = 9u;
    check(clock.configure_waw_policy(
              ee::e_ee_probe_waw_policy::ORDERED_OVERLAP) &&
          clock.issue(overlapping_fcsr, &at) && at == 0u &&
          clock.issue(overlapping_fcsr, &at) && at == 2u &&
          clock.last_completion_cycle() == 11u,
          "explicit FCR31 overlap retains in-order candidate completion");
    s_ee_probe_instruction read_fcsr{};
    read_fcsr.m_pipe = e_ee_probe_pipe::BRANCH;
    read_fcsr.m_fcsr_read = true;
    check(clock.issue(read_fcsr, &at) && at == 11u,
          "BC1-like FCR31 read waits for most recent overlapping writer");

    clock.reset();
    overlapping_mac.m_hilo_result_latency = 9u;
    s_ee_probe_instruction overtaking_mac = overlapping_mac;
    overtaking_mac.m_hilo_result_latency = 1u;
    s_ee_probe_instruction simultaneous_mac = overlapping_mac;
    simultaneous_mac.m_hilo_result_latency = 7u;
    check(clock.configure_waw_policy(
              ee::e_ee_probe_waw_policy::ORDERED_OVERLAP) &&
          clock.issue(overlapping_mac, &at, &completion) && at == 0u &&
          completion == 9u &&
          !clock.issue(overtaking_mac, &at, &completion) &&
          !clock.issue(simultaneous_mac, &at, &completion) &&
          completion == 9u &&
          clock.instructions_issued() == 1u &&
          clock.estimated_cycles() == 1u &&
          clock.last_completion_cycle() == 9u &&
          clock.issue(overlapping_mac, &at, &completion) && at == 2u &&
          completion == 11u,
          "overtaking/equal-deadline HI/LO completion rejects atomically");

    clock.reset();
    check(clock.waw_policy() ==
              ee::e_ee_probe_waw_policy::WAIT_FOR_RESULT &&
          !clock.configure_waw_policy(
              static_cast<ee::e_ee_probe_waw_policy>(255u)) &&
          clock.issue(a) &&
          !clock.configure_waw_policy(
              ee::e_ee_probe_waw_policy::ORDERED_OVERLAP),
          "invalid or midstream overlap-mode changes fail closed");

    // COP0 has its own architectural scoreboard. VU0 VF and VI retain
    // different VALUES, but macro hazards alias their register numbers
    // except VF00/VI00 (VU manual 5.4.2). These transfer candidates
    // remain conservatively single-issue with caller-supplied latencies.
    clock.reset();
    s_ee_probe_instruction write_cp0{};
    write_cp0.m_pipe = e_ee_probe_pipe::SYSTEM_CONTROL;
    write_cp0.m_gpr_read_mask = 1u << 8u;
    write_cp0.m_cop0_write_mask = 1u << 12u;
    write_cp0.m_cop0_result_latency = 6u;
    s_ee_probe_instruction read_cp0{};
    read_cp0.m_pipe = e_ee_probe_pipe::SYSTEM_CONTROL;
    read_cp0.m_cop0_read_mask = 1u << 12u;
    read_cp0.m_gpr_write_mask = 1u << 9u;
    check(clock.issue(write_cp0, &at) && at == 0u &&
          clock.issue(read_cp0, &at) && at == 6u,
          "COP0 transfer RAW waits for explicit CP0-register readiness");

    clock.reset();
    read_cp0.m_cop0_read_mask = 1u << 13u;
    check(clock.issue(write_cp0, &at) && at == 0u &&
          clock.issue(read_cp0, &at) && at == 1u,
          "independent COP0 registers do not share false dependencies");

    clock.reset();
    s_ee_probe_instruction write_vf{};
    write_vf.m_pipe = e_ee_probe_pipe::VU0_TRANSFER;
    write_vf.m_gpr_read_mask = 1u << 8u;
    write_vf.m_vu0_vf_write_mask = 1u << 4u;
    write_vf.m_vu0_vf_result_latency = 5u;
    s_ee_probe_instruction read_vf{};
    read_vf.m_pipe = e_ee_probe_pipe::VU0_TRANSFER;
    read_vf.m_vu0_vf_read_mask = 1u << 4u;
    read_vf.m_gpr_write_mask = 1u << 9u;
    check(clock.issue(write_vf, &at) && at == 0u &&
          clock.issue(read_vf, &at) && at == 5u,
          "VU0 VF quadword transfer RAW waits for its own register");

    clock.reset();
    s_ee_probe_instruction read_vi{};
    read_vi.m_pipe = e_ee_probe_pipe::VU0_TRANSFER;
    read_vi.m_vu0_vi_read_mask = 1u << 4u;
    read_vi.m_gpr_write_mask = 1u << 10u;
    check(clock.issue(write_vf, &at) && at == 0u &&
          clock.issue(read_vi, &at) && at == 5u,
          "VU0 macro VF4 write blocks VI4 read by shared index");

    clock.reset();
    s_ee_probe_instruction write_vi{};
    write_vi.m_pipe = e_ee_probe_pipe::VU0_TRANSFER;
    write_vi.m_vu0_vi_write_mask = 1u << 4u;
    write_vi.m_vu0_vi_result_latency = 7u;
    check(clock.issue(write_vi, &at) && at == 0u &&
          clock.issue(read_vf, &at) && at == 7u,
          "VU0 macro VI4 write blocks VF4 read by shared index");

    clock.reset();
    check(clock.issue(write_vf, &at) && at == 0u &&
          clock.issue(write_vi, &at) && at == 5u,
          "VU0 macro VF4 and VI4 writes share WAW hazard identity");

    clock.reset();
    read_vi.m_vu0_vi_read_mask = 1u << 5u;
    check(clock.issue(write_vf, &at) && at == 0u &&
          clock.issue(read_vi, &at) && at == 1u,
          "different VU0 macro register indices have no false alias");

    clock.reset();
    write_vf.m_vu0_vf_write_mask = 1u;
    read_vf.m_vu0_vf_read_mask = 1u;
    read_vi.m_vu0_vi_read_mask = 1u;
    check(clock.issue(write_vf, &at) && at == 0u &&
          clock.issue(read_vf, &at) && at == 1u &&
          clock.issue(read_vi, &at) && at == 2u,
          "VF00 and VI00 do not create macro register dependencies");

    clock.reset();
    write_vi.m_vu0_vi_write_mask = 1u;
    check(clock.issue(write_vi, &at) && at == 0u &&
          clock.issue(read_vf, &at) && at == 1u,
          "VI00 write does not create a false VF00 hazard");

    clock.reset();
    write_cp0.m_cop0_result_latency = 8u;
    s_ee_probe_instruction earlier_cp0 = write_cp0;
    earlier_cp0.m_cop0_result_latency = 1u;
    check(clock.configure_waw_policy(
              ee::e_ee_probe_waw_policy::ORDERED_OVERLAP) &&
          clock.issue(write_cp0, &at) && at == 0u &&
          !clock.issue(earlier_cp0, &at) &&
          clock.instructions_issued() == 1u &&
          clock.estimated_cycles() == 1u,
          "COP0 overlapping writers cannot overtake earlier completion");

    clock.reset();
    write_vf.m_vu0_vf_write_mask = 1u << 4u;
    write_vf.m_vu0_vf_result_latency = 8u;
    s_ee_probe_instruction earlier_vf = write_vf;
    earlier_vf.m_vu0_vf_result_latency = 1u;
    check(clock.configure_waw_policy(
              ee::e_ee_probe_waw_policy::ORDERED_OVERLAP) &&
          clock.issue(write_vf, &at) && at == 0u &&
          !clock.issue(earlier_vf, &at) &&
          clock.instructions_issued() == 1u,
          "VU0 overlapping VF writers cannot overtake earlier completion");

    clock.reset();
    write_vi.m_vu0_vi_write_mask = 1u << 4u;
    write_vi.m_vu0_vi_result_latency = 1u;
    check(clock.configure_waw_policy(
              ee::e_ee_probe_waw_policy::ORDERED_OVERLAP) &&
          clock.issue(write_vf, &at) && at == 0u &&
          !clock.issue(write_vi, &at) &&
          clock.instructions_issued() == 1u &&
          clock.estimated_cycles() == 1u,
          "cross-bank VU0 WAW result overtaking rejects atomically");

    clock.reset();
    read_cp0.m_cop0_read_mask = 1u << 12u;
    check(clock.issue(write_cp0, &at) && at == 0u &&
          clock.issue(write_vf, &at) && at == 1u &&
          clock.issue(read_cp0, &at) && at == 8u,
          "COP0 readiness survives VU0 and host-execution boundaries");

    // Full-width MMI operand coverage must agree with the ordinary GPR
    // scoreboard. The latter is already whole-register and catches
    // dependencies between scalar and packed operations without guessed
    // per-lane forwarding rules.
    clock.reset();
    s_ee_probe_instruction packed{};
    packed.m_pipe = e_ee_probe_pipe::PACKED_MMI;
    packed.m_gpr_read_mask = (1u << 4u) | (1u << 5u);
    packed.m_gpr_write_mask = 1u << 6u;
    packed.m_gpr_128_read_mask = packed.m_gpr_read_mask;
    packed.m_gpr_128_write_mask = packed.m_gpr_write_mask;
    packed.m_result_latency = 5u;
    s_ee_probe_instruction scalar_consumer{};
    scalar_consumer.m_gpr_read_mask = 1u << 6u;
    scalar_consumer.m_gpr_write_mask = 1u << 7u;
    check(clock.issue(packed, &at) && at == 0u &&
          clock.issue(scalar_consumer, &at) && at == 5u,
          "full-128 packed writer blocks later scalar GPR read");

    clock.reset();
    const s_ee_probe_instruction independent_scalar{
        1u << 12u, 1u << 13u
    };
    check(clock.issue(packed, &at) && at == 0u &&
          clock.issue(independent_scalar, &at) && at == 1u,
          "unverified packed-MMI pairing remains conservatively disabled");

    clock.reset();
    s_ee_probe_instruction invalid_packed = packed;
    invalid_packed.m_gpr_128_read_mask |= 1u << 18u;
    check(!clock.issue(invalid_packed) &&
          clock.instructions_issued() == 0u &&
          clock.estimated_cycles() == 0u &&
          clock.issue(packed, &at) && at == 0u,
          "packed full-width read coverage cannot omit dependencies");

    clock.reset();
    invalid_packed = packed;
    invalid_packed.m_gpr_128_write_mask |= 1u << 19u;
    check(!clock.issue(invalid_packed) &&
          clock.instructions_issued() == 0u &&
          clock.issue(packed, &at) && at == 0u,
          "packed full-width write coverage rejects inconsistent masks");

    // Disjoint accounting: pair, register wait, functional-unit wait,
    // annulment and HLE boundary. Artificial intervals exercise bookkeeping
    // only, not measured R5900 latencies.
    clock.reset();
    s_ee_probe_instruction producer{};
    producer.m_pipe = e_ee_probe_pipe::MULT_DIV;
    producer.m_gpr_write_mask = 1u << 2u;
    producer.m_result_latency = 4u;
    producer.m_unit = ee::e_ee_probe_unit::MAC0;
    producer.m_initiation_interval = 7u;
    s_ee_probe_instruction consumer = producer;
    consumer.m_gpr_read_mask = 1u << 2u;
    consumer.m_gpr_write_mask = 1u << 8u;
    check(clock.issue(producer, &at) && at == 0u &&
          clock.issue(b, &at) && at == 0u &&
          clock.issue(consumer, &at) && at == 7u &&
          clock.reserve_annulled_delay_slot() &&
          clock.charge_hle_overhead(3u) &&
          clock.issue(c, &at) && at == 12u,
          "mixed candidate issue-window accounting sequence");
    const auto breakdown = clock.cycle_breakdown();
    check(breakdown.m_single_issue == 2u &&
          breakdown.m_dual_issue == 1u &&
          breakdown.m_annulled == 1u &&
          breakdown.m_register_idle == 3u &&
          breakdown.m_unit_idle == 3u &&
          breakdown.m_hle == 3u &&
          clock.estimated_cycles() == 13u &&
          clock.instructions_issued() == 4u,
          "candidate cycle categories are disjoint and exhaustive");
    check(!clock.issue(s_ee_probe_instruction{
              0u, 0u, e_ee_probe_pipe::INTEGER, 0u
          }) && clock.cycle_breakdown().m_single_issue == 2u &&
          clock.cycle_breakdown().m_register_idle == 3u &&
          clock.cycle_breakdown().m_unit_idle == 3u,
          "failed candidate issue leaves cycle accounting unchanged");
    clock.reset();
    const auto empty = clock.cycle_breakdown();
    check(empty.m_single_issue == 0u && empty.m_dual_issue == 0u &&
          empty.m_annulled == 0u && empty.m_register_idle == 0u &&
          empty.m_unit_idle == 0u && empty.m_hle == 0u,
          "reset clears candidate cycle accounting");

    std::printf("ee_issue_timing_probe_test: %d failure(s)\n", g_failures);
    return g_failures != 0 ? 1 : 0;
}
