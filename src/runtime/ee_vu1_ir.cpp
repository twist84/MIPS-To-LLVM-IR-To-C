#include "ee_vu1_ir.h"
#include "ee_vu1_dependency.h"
#include "ee_vu1_aot.h"
#include "../llvm/ee_cop1_rsqrt_ir.h"
#include "../llvm/ee_cop1_add_ir.h"
#include "../llvm/ee_cop1_mul_ir.h"
#include "../llvm/ee_vu_fmac_ir.h"
#include "ee_vu_fmac4_ir.h"

#include <utility>
#include <bitset>
#include <set>
#include <tuple>

namespace ee
{
namespace
{
uint32_t read_word(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8u) |
        (static_cast<uint32_t>(bytes[2]) << 16u) |
        (static_cast<uint32_t>(bytes[3]) << 24u);
}

// Integer-only VU arithmetic matching ee_vu1_fp.h. No host FP,
// fast-math, or out-of-range shifts. These helpers are module-local.
constexpr const char* k_fp_ir = R"IR(
declare i32 @llvm.ctlz.i32(i32, i1 immarg)
declare i32 @llvm.vector.reduce.or.v4i32(<4 x i32>)

; Each ring stores the last eight writes in issue order, not timestamp
; order (FCSET and FSSET can publish retroactively visible values).
define internal void @vu_flag_push(ptr %head, ptr %times, ptr %values,
    i32 %time, i32 %value) alwaysinline {
  %h = load i32, ptr %head, align 4
  %index = and i32 %h, 7
  %tp = getelementptr i32, ptr %times, i32 %index
  %vp = getelementptr i32, ptr %values, i32 %index
  store i32 %time, ptr %tp, align 4
  store i32 %value, ptr %vp, align 4
  %next = add i32 %h, 1
  store i32 %next, ptr %head, align 4
  ret void
}

define internal i32 @vu_flag_read(ptr %head, ptr %times, ptr %values,
    i32 %now) alwaysinline {
entry:
  %h = load i32, ptr %head, align 4
  br label %scan
scan:
  %k = phi i32 [1, %entry], [%next, %scan]
  %offset = sub i32 %h, %k
  %index = and i32 %offset, 7
  %tp = getelementptr i32, ptr %times, i32 %index
  %ts = load i32, ptr %tp, align 4
  %ready = add i32 %ts, 4
  %visible = icmp ule i32 %ready, %now
  %oldest = icmp eq i32 %k, 8
  %done = or i1 %visible, %oldest
  %next = add i32 %k, 1
  br i1 %done, label %result, label %scan
result:
  %vp = getelementptr i32, ptr %values, i32 %index
  %value = load i32, ptr %vp, align 4
  ret i32 %value
}

; Q-unit arithmetic calls the module-local signed-digit recurrence.
define internal i32 @vu_div(i32 %a, i32 %b) alwaysinline {
  %r = call i32 @ee_cop1_radix_quotient(i32 %a, i32 %b)
  ret i32 %r
}

define internal i32 @vu_sqrt(i32 %a) alwaysinline {
  %r = call i32 @ee_cop1_radix_root(i32 %a)
  ret i32 %r
}

define internal i32 @vu_rsqrt(i32 %a, i32 %b) alwaysinline {
  %r = call i32 @ee_cop1_rsqrt_bits(i32 %a, i32 %b)
  ret i32 %r
}

; The same scalar pipeline machinery can be used for Q and P. Ready=0
; means no pending value; wait advances the total cycle counter only.
define internal void @vu_scalar_sync(ptr %current, ptr %ready, ptr %value,
    i32 %now) alwaysinline {
entry:
  %r = load i32, ptr %ready, align 4
  %pending = icmp ne i32 %r, 0
  %due = icmp uge i32 %now, %r
  %commit = and i1 %pending, %due
  br i1 %commit, label %publish, label %done
publish:
  %v = load i32, ptr %value, align 4
  store i32 %v, ptr %current, align 4
  store i32 0, ptr %ready, align 4
  br label %done
done:
  ret void
}

define internal i32 @vu_scalar_wait(ptr %current, ptr %ready, ptr %value,
    ptr %cycle) alwaysinline {
entry:
  %now = load i32, ptr %cycle, align 4
  %r = load i32, ptr %ready, align 4
  %stall = icmp ugt i32 %r, %now
  %elapsed = select i1 %stall, i32 %r, i32 %now
  store i32 %elapsed, ptr %cycle, align 4
  %busy = icmp ne i32 %r, 0
  br i1 %busy, label %publish, label %done
publish:
  %v = load i32, ptr %value, align 4
  store i32 %v, ptr %current, align 4
  store i32 0, ptr %ready, align 4
  br label %done
done:
  ret i32 %elapsed
}

define internal void @vu_scalar_schedule(ptr %current, ptr %ready, ptr %value,
    ptr %cycle, i32 %new_value, i32 %latency) alwaysinline {
  %now = call i32 @vu_scalar_wait(ptr %current, ptr %ready, ptr %value, ptr %cycle)
  %at = add i32 %now, %latency
  store i32 %new_value, ptr %value, align 4
  store i32 %at, ptr %ready, align 4
  ret void
}
; FDIV status is a separate retirement event, not an FMAC history
; entry. Publish Q and I/D together, preserving intervening sticky flags.
define internal void @vu_q_sync(ptr %q, ptr %ready, ptr %pending,
    ptr %status, ptr %flags, i32 %now) alwaysinline {
entry:
  %r = load i32, ptr %ready, align 4
  %busy = icmp ne i32 %r, 0
  %due = icmp uge i32 %now, %r
  %commit = and i1 %busy, %due
  br i1 %commit, label %publish, label %done
publish:
  %v = load i32, ptr %pending, align 4
  store i32 %v, ptr %q, align 4
  %f = load i32, ptr %flags, align 4
  %old = load i32, ptr %status, align 4
  %cleared = and i32 %old, -49
  %merged = or i32 %cleared, %f
  store i32 %merged, ptr %status, align 4
  store i32 0, ptr %ready, align 4
  br label %done
done:
  ret void
}

define internal void @vu_q_wait(ptr %q, ptr %ready, ptr %pending,
    ptr %status, ptr %flags, ptr %cycle) alwaysinline {
  %now = load i32, ptr %cycle, align 4
  %r = load i32, ptr %ready, align 4
  %stall = icmp ugt i32 %r, %now
  %elapsed = select i1 %stall, i32 %r, i32 %now
  store i32 %elapsed, ptr %cycle, align 4
  %busy = icmp ne i32 %r, 0
  br i1 %busy, label %publish, label %done
publish:
  %v = load i32, ptr %pending, align 4
  store i32 %v, ptr %q, align 4
  %f = load i32, ptr %flags, align 4
  %old = load i32, ptr %status, align 4
  %cleared = and i32 %old, -49
  %merged = or i32 %cleared, %f
  store i32 %merged, ptr %status, align 4
  store i32 0, ptr %ready, align 4
  br label %done
done:
  ret void
}
)IR";

constexpr uint32_t k_unknown_pc = 0xffffffffu;
constexpr uint32_t k_mac_history = 1u;
constexpr uint32_t k_status_history = 2u;
constexpr uint32_t k_clip_history = 4u;

// Only lower flag readers observe historical values. Flag producers
// always update the live architectural registers, even without readers.
uint32_t flag_history_reads(uint32_t lo, uint32_t hi)
{
    if ((hi & 0x80000000u) != 0u)
    {
        return 0u;
    }
    switch (lo >> 25u)
    {
    case 0x10u: case 0x12u: case 0x13u: case 0x1cu:
        return k_clip_history;
    case 0x14u: case 0x16u: case 0x17u:
        return k_status_history;
    case 0x18u: case 0x1au: case 0x1bu:
        return k_mac_history;
    default:
        return 0u;
    }
}

bool is_branch(uint32_t lo, uint32_t hi)
{
    if ((hi & 0x80000000u) != 0u)
    {
        return false;
    }
    switch (lo >> 25u)
    {
    case 0x20u: case 0x21u: case 0x24u: case 0x25u:
    case 0x28u: case 0x29u: case 0x2cu: case 0x2du:
    case 0x2eu: case 0x2fu:
        return true;
    default:
        return false;
    }
}

bool is_flag_pair(uint32_t lo, uint32_t hi)
{
    const uint32_t funct = hi & 63u;
    const uint32_t index = (((hi >> 6u) & 31u) << 2u) | (funct & 3u);
    if (funct >= 0x3cu && index == 0x1fu)
    {
        return true;
    }
    if ((hi & 0x80000000u) != 0u)
    {
        return false;
    }
    const uint32_t major = lo >> 25u;
    return major >= 0x10u && major <= 0x1cu && major != 0x19u;
}

bool is_q_pair(uint32_t lo, uint32_t hi)
{
    if ((hi & 0x80000000u) != 0u || lo >> 25u != 0x40u ||
        (lo & 0x3cu) != 0x3cu)
    {
        return false;
    }
    const uint32_t index = (((lo >> 6u) & 31u) << 2u) | (lo & 3u);
    return index >= 0x38u && index <= 0x3bu;
}

bool is_efu_index(uint32_t index)
{
    return (index >= 0x70u && index <= 0x76u) ||
        (index >= 0x78u && index <= 0x7au) ||
        (index >= 0x7cu && index <= 0x7eu);
}

bool is_p_pair(uint32_t lo, uint32_t hi)
{
    if ((hi & 0x80000000u) != 0u || lo >> 25u != 0x40u ||
        (lo & 0x3cu) != 0x3cu)
    {
        return false;
    }
    const uint32_t index = (((lo >> 6u) & 31u) << 2u) | (lo & 3u);
    return is_efu_index(index) || index == 0x7bu || index == 0x64u;
}

bool is_kick_pair(uint32_t lo, uint32_t hi)
{
    return (hi & 0x80000000u) == 0u && lo >> 25u == 0x40u &&
        (lo & 0x3cu) == 0x3cu &&
        ((((lo >> 6u) & 31u) << 2u) | (lo & 3u)) == 0x6cu;
}

bool branch_delay_exempt(uint32_t lo)
{
    switch (lo >> 25u)
    {
    case 0x10u: case 0x12u: case 0x13u: case 0x14u:
    case 0x16u: case 0x17u: case 0x18u: case 0x1au:
    case 0x1bu: case 0x1cu: case 0x21u: case 0x25u:
        return true;
    default: return false;
    }
}

uint32_t relative_target(uint32_t pc, uint32_t lo)
{
    const uint32_t raw = lo & 0x7ffu;
    const int32_t offset = raw >= 1024u ?
        static_cast<int32_t>(raw) - 2048 : static_cast<int32_t>(raw);
    return (pc + 8u + static_cast<uint32_t>(offset) * 8u) & 0x3fffu;
}

class c_straight_line_emitter
{
public:
    c_straight_line_emitter()
    {
        m_ir = llvm::k_ee_cop1_rsqrt_ir;
        m_ir += llvm::k_ee_cop1_add_ir;
        m_ir += llvm::k_ee_cop1_mul_ir;
        m_ir += llvm::k_vu_fmac_ir;
        m_ir += llvm::k_vu_fmac4_ir;
        m_ir += llvm::k_vu_q_status_ir;
        m_ir += llvm::k_vu_fixed_ir;
        m_ir += k_fp_ir;
        m_ir += "define dso_local i32 @ee_vu1_program(ptr %vi, ptr %i, "
            "ptr %tpc, i32 %top, i32 %itop, ptr %vf, ptr %q, "
            "ptr %mac, ptr %status, ptr %data, ptr %cycles_out, "
            "ptr %last_pc_out, ptr %acc, ptr %clip, ptr %kick, "
            "ptr %kick_context, ptr %p, ptr %r, ptr %efu, ptr %acc_overflow) {\nentry:\n";
    }

    // The dynamic path uses the same per-pair arithmetic emitter as the
    // straight-line path. PC and delayed NPC are independent values, so
    // branches in delay slots and E termination obey interpreter order.
    void begin_dynamic(uint32_t start, const std::bitset<2048>& reached,
        bool flags, bool q, bool p, uint32_t branch_vi_mask, uint32_t flag_history_mask)
    {
        m_dynamic = true;
        m_dynamic_local_offset = m_ir.size();
        m_branch_vi_mask = branch_vi_mask;
        m_flag_history_mask = flag_history_mask;
        m_flag_pipeline = flags;
        m_q_pipeline = q;
        m_p_pipeline = p;
        if (p)
        {
            init_scalar("p");
        }
        if (q)
        {
            init_scalar("q");
            line("%q_pending_flags = alloca i32, align 4");
            line("store i32 0, ptr %q_pending_flags, align 4");
        }
        if ((m_flag_history_mask & k_mac_history) != 0u)
        {
            init_ring("mac");
        }
        if ((m_flag_history_mask & k_status_history) != 0u)
        {
            init_ring("status");
        }
        if ((m_flag_history_mask & k_clip_history) != 0u)
        {
            init_ring("clip");
        }
        line("%pc = alloca i32, align 4");
        line("%npc = alloca i32, align 4");
        line("%step_count = alloca i32, align 4");
        line("%cycle = alloca i32, align 4");
        line("%ending = alloca i1, align 1");
        if (m_branch_vi_mask != 0u)
        {
            line("%previous_vi_mask = alloca i32, align 4");
            line("%previous_vi = alloca [16 x i16], align 2");
            line("store [16 x i16] zeroinitializer, ptr %previous_vi, align 2");
            line("store i32 0, ptr %previous_vi_mask, align 4");
        }
        line("%ready = alloca [144 x i32], align 4");
        line("store [144 x i32] zeroinitializer, ptr %ready, align 4");
        line("store i32 " + std::to_string(start) + ", ptr %pc, align 4");
        line("store i32 " + std::to_string((start + 8u) & 0x3fffu) + ", ptr %npc, align 4");
        line("store i32 0, ptr %step_count, align 4");
        line("store i32 16, ptr %cycle, align 4");
        line("store i1 false, ptr %ending, align 1");
        line("br label %dispatch");
        m_ir += "dispatch:\n";
        line("%old_steps = load i32, ptr %step_count, align 4");
        line("%within_budget = icmp ult i32 %old_steps, " +
            std::to_string(c_ee_vu1::k_max_steps));
        line("br i1 %within_budget, label %select_pc, label %fail");
        m_ir += "select_pc:\n";
        line("%steps = add i32 %old_steps, 1");
        line("store i32 %steps, ptr %step_count, align 4");
        line("%current_pc = load i32, ptr %pc, align 4");
        line("%npc_now = load i32, ptr %npc, align 4");
        line("%ending_now = load i1, ptr %ending, align 1");
        if (m_branch_vi_mask != 0u)
        {
            line("%previous_vi_mask_now = load i32, ptr %previous_vi_mask, align 4");
        }
        line("%sequential_raw = add i32 %npc_now, 8");
        line("%sequential = and i32 %sequential_raw, 16383");
        line("switch i32 %current_pc, label %fail [");
        for (uint32_t index = 0u; index < reached.size(); ++index)
        {
            if (reached.test(index))
            {
                line("  i32 " + std::to_string(index * 8u) +
                    ", label %pair_" + std::to_string(index * 8u));
            }
        }
        line("]");
    }

    bool emit_dynamic_pair(uint32_t pc, uint32_t lo, uint32_t hi)
    {
        m_ir += "pair_" + std::to_string(pc) + ":\n";
        m_current_pc = pc;
        // Every dynamic pair is a switch target, including loop backedges.
        // SSA values from a different pair do not dominate this entry.
        m_vf_lanes = {};
        m_vf_vectors = {};
        m_acc_lanes = {};
        m_acc_vector.clear();
        m_next_target = "%sequential";
        m_delay_vi_writes = (hi & 0x80000000u) == 0u && !branch_delay_exempt(lo);
        m_delay_vi_mask = 0u;

        // Identical dependency lists and four-cycle retire semantics to
        // s_ee_vu1_dependency. Readiness is local to this invocation.
        const auto use = s_ee_vu1_dependency::decode(lo, hi);
        const auto old = operation("load i32, ptr %cycle, align 4");
        auto cycle = operation("add i32 " + old + ", 1");
        for (uint32_t index = 0u; index < use.m_read_lane_count; ++index)
        {
            cycle = max_ready(cycle, use.m_read_lanes[index]);
        }
        for (uint32_t index = 0u; index < use.m_vi_index_count; ++index)
        {
            cycle = max_ready(cycle, 128u + use.m_vi_indices[index]);
        }
        line("store i32 " + cycle + ", ptr %cycle, align 4");
        if (m_q_pipeline)
        {
            sync_scalar("q", cycle);
        }
        if (m_p_pipeline)
        {
            sync_scalar("p", cycle);
        }
        if (m_flag_pipeline)
        {
            m_issue_cycle = cycle;
            m_flag_time = operation("add i32 %steps, 16");
        }
        if (!emit(lo, hi))
        {
            return false;
        }
        if (use.m_write_lane_count != 0u || use.m_vi_load != 0u)
        {
            const auto ready = operation("add i32 " + cycle + ", 4");
            for (uint32_t index = 0u; index < use.m_write_lane_count; ++index)
            {
                line("store i32 " + ready + ", ptr " +
                    ready_pointer(use.m_write_lanes[index]) + ", align 4");
            }
            if (use.m_vi_load != 0u)
            {
                line("store i32 " + ready + ", ptr " +
                    ready_pointer(128u + use.m_vi_load) + ", align 4");
            }
        }
        if (m_branch_vi_mask != 0u)
        {
            line("store i32 " + std::to_string(m_delay_vi_mask) +
                ", ptr %previous_vi_mask, align 4");
        }
        line("store i32 %npc_now, ptr %pc, align 4");
        line("store i32 " + m_next_target + ", ptr %npc, align 4");
        line(std::string("store i1 ") + ((hi & 0x40000000u) ? "true" : "false") +
            ", ptr %ending, align 1");
        line("br i1 %ending_now, label %done, label %dispatch");
        return true;
    }

    std::string finish_dynamic()
    {
        m_ir += "done:\n";
        if (m_q_pipeline)
        {
            wait_scalar("q");
        }
        if (m_p_pipeline)
        {
            wait_scalar("p");
        }
        const auto cycle = operation("load i32, ptr %cycle, align 4");
        const auto elapsed = operation("sub i32 " + cycle + ", 16");
        line("store i32 " + elapsed + ", ptr %cycles_out, align 4");
        line("store i32 %current_pc, ptr %last_pc_out, align 4");
        line("store i32 %npc_now, ptr %tpc, align 4");
        flush_dynamic_vectors();
        line("ret i32 %steps");
        m_ir += "fail:\n";
        // Preserve even failed invocation's staged register writes. The
        // runtime discards the staged state on failure.
        flush_dynamic_vectors();
        line("ret i32 0");
        m_ir += "}\n";
        initialize_dynamic_vectors();
        return m_ir;
    }

    bool emit(uint32_t lo, uint32_t hi)
    {
        // Compute upper operands before the lower executes, then
        // retire upper writes after lower writeback, as in the interpreter.
        m_upper_mask = 0u;
        m_upper_flags = false;
        m_upper_acc = false;
        m_upper_vector.clear();
        m_upper_over_vector.clear();
        m_upper_under_vector.clear();
        m_upper_sticky_vector.clear();
        m_pending_clip.clear();
        m_fsset = -1;
        if ((hi & 0x38000000u) != 0u || !emit_upper(hi) ||
            !emit_lower(lo, hi))
        {
            return false;
        }
        commit_upper();
        if (m_fsset >= 0)
        {
            const auto old = operation("load i32, ptr %status, align 4");
            const auto current = operation("and i32 " + old + ", 63");
            const auto status = operation("or i32 " + current + ", " +
                std::to_string(m_fsset));
            line("store i32 " + status + ", ptr %status, align 4");
            if ((m_flag_history_mask & (k_status_history | k_mac_history)) != 0u)
            {
                const auto time = operation("sub i32 " + m_flag_time + ", 4");
                if ((m_flag_history_mask & k_status_history) != 0u)
                {
                    push_ring("status", time, status);
                }
                if ((m_flag_history_mask & k_mac_history) != 0u)
                {
                    push_ring("mac", time, operation("load i32, ptr %mac, align 4"));
                }
            }
        }
        if (!m_pending_clip.empty())
        {
            line("store i32 " + m_pending_clip + ", ptr %clip, align 4");
            if ((m_flag_history_mask & k_clip_history) != 0u)
            {
                const auto now = (m_q_pipeline || m_p_pipeline) ?
                    operation("load i32, ptr %cycle, align 4") : m_issue_cycle;
                push_ring("clip", now, m_pending_clip);
            }
        }
        return true;
    }

    bool emit_lower(uint32_t lo, uint32_t hi)
    {
        if ((hi & 0x80000000u) != 0u)
        {
            line("store i32 " + std::to_string(lo) + ", ptr %i, align 4");
            return true;
        }
        if (lo == 0x8000033cu)
        {
            return true;
        }

        const uint32_t major = lo >> 25u;
        const uint32_t it = (lo >> 16u) & 15u;
        const uint32_t is = (lo >> 11u) & 15u;
        const uint32_t id = (lo >> 6u) & 15u;
        const uint32_t funct = lo & 63u;
        const uint32_t ft = (lo >> 16u) & 31u;
        const uint32_t fs = (lo >> 11u) & 31u;
        const uint32_t mask = (lo >> 21u) & 15u;
        const int32_t raw = static_cast<int32_t>(lo & 0x7ffu);
        const int32_t offset = raw >= 1024 ? raw - 2048 : raw;
        if (major == 0x00u || major == 0x01u ||
            major == 0x04u || major == 0x05u)
        {
            const auto address = data_address(major == 0x01u ? it : is, offset);
            switch (major)
            {
            case 0x00u: load_vector(ft, mask, address); break;
            case 0x01u: store_vector(fs, mask, address); break;
            case 0x04u: load_integer(it, mask, address, false); break;
            case 0x05u: store_integer(it, mask, address); break;
            }
            return true;
        }
        if (m_flag_pipeline && major >= 0x10u && major <= 0x1cu)
        {
            return emit_flag_lower(lo, major, is, it);
        }
        if (m_dynamic && is_branch(lo, hi))
        {
            return emit_branch(lo, major, is, it);
        }
        if (major == 0x08u || major == 0x09u)
        {
            const uint32_t imm = ((lo >> 10u) & 0x7800u) | (lo & 0x7ffu);
            binary(major == 0x08u ? "add" : "sub", it, is,
                std::to_string(imm));
            return true;
        }
        if (major != 0x40u)
        {
            return false;
        }
        switch (funct)
        {
        case 0x30u: binary("add", id, is, reg(it)); return true;
        case 0x31u: binary("sub", id, is, reg(it)); return true;
        case 0x34u: binary("and", id, is, reg(it)); return true;
        case 0x35u: binary("or", id, is, reg(it)); return true;
        case 0x32u:
        {
            const int32_t raw = static_cast<int32_t>((lo >> 6u) & 31u);
            binary("add", it, is, std::to_string(raw >= 16 ? raw - 32 : raw));
            return true;
        }
        default:
            break;
        }
        if (funct >= 0x3cu)
        {
            const uint32_t index = (((lo >> 6u) & 31u) << 2u) | (funct & 3u);
            if (index == 0x34u || index == 0x36u)
            {
                // LQD pre-decrements; LQI post-increments, including
                // suppressed VI0 updates and 16-bit integer wrapping.
                if (index == 0x36u)
                {
                    binary("sub", is, is, "1");
                }
                load_vector(ft, mask, data_address(is, 0));
                if (index == 0x34u)
                {
                    binary("add", is, is, "1");
                }
                return true;
            }
            if (index == 0x35u || index == 0x37u)
            {
                if (index == 0x37u)
                {
                    binary("sub", it, it, "1");
                }
                store_vector(fs, mask, data_address(it, 0));
                if (index == 0x35u)
                {
                    binary("add", it, it, "1");
                }
                return true;
            }
            if (m_q_pipeline && index >= 0x38u && index <= 0x3bu)
            {
                return emit_q_lower(lo, index, fs, ft);
            }
            if (index == 0x3eu)
            {
                load_integer(it, mask, data_address(is, 0), true);
                return true;
            }
            if (index == 0x3fu)
            {
                store_integer(it, mask, data_address(is, 0));
                return true;
            }
            if (index == 0x30u || index == 0x31u)
            {
                std::array<std::string, 4> values{};
                for (uint32_t lane = 0u; lane < 4u; ++lane)
                {
                    if ((mask & (8u >> lane)) != 0u)
                    {
                        values[lane] = vf(fs, index == 0x30u ? lane : (lane + 1u) & 3u);
                    }
                }
                write_vector(ft, mask, values);
                return true;
            }
            if (index == 0x3cu) // MTIR
            {
                store(it, operation("trunc i32 " + vf(fs, (lo >> 21u) & 3u) + " to i16"));
                return true;
            }
            if (index == 0x3du) // MFIR
            {
                const auto value = operation("sext i16 " + reg(is) + " to i32");
                const std::array<std::string, 4> values{value, value, value, value};
                write_vector(ft, mask, values);
                return true;
            }
            if (index >= 0x40u && index <= 0x43u)
            {
                return emit_random_lower(lo, index, fs, ft, mask);
            }
            if (m_p_pipeline && (is_efu_index(index) || index == 0x64u || index == 0x7bu))
            {
                return emit_p_lower(lo, index, fs, ft, mask);
            }
            if (m_dynamic && index == 0x6cu)
            {
                const auto address = operation("zext i16 " + reg(is) + " to i32");
                const auto result = operation("call i32 %kick(ptr %kick_context, i32 " +
                    address + ")");
                const auto ok = operation("icmp ne i32 " + result + ", 0");
                const auto label = "kick_ok_" + std::to_string(m_counter++);
                line("br i1 " + ok + ", label %" + label + ", label %fail");
                m_ir += label + ":\n";
                return true;
            }
            if (index == 0x68u || index == 0x69u)
            {
                const auto value = tmp();
                line(value + " = trunc i32 " +
                    (index == 0x68u ? "%top" : "%itop") + " to i16");
                store(it, value);
                return true;
            }
        }
        return false;
    }

    bool writes_data() const { return m_writes_data; }

    std::string finish(uint32_t steps, uint32_t cycles, uint32_t next_pc)
    {
        line("store i32 " + std::to_string(cycles) + ", ptr %cycles_out, align 4");
        line("store i32 " + std::to_string((next_pc - 8u) & 0x3fffu) +
            ", ptr %last_pc_out, align 4");
        line("store i32 " + std::to_string(next_pc) + ", ptr %tpc, align 4");
        line("ret i32 " + std::to_string(steps));
        m_ir += "}\n";
        return m_ir;
    }

private:
    void init_scalar(const std::string& name)
    {
        line("%" + name + "_ready = alloca i32, align 4");
        line("%" + name + "_pending = alloca i32, align 4");
        line("store i32 0, ptr %" + name + "_ready, align 4");
        line("store i32 0, ptr %" + name + "_pending, align 4");
    }

    std::string scalar_args(const std::string& name)
    {
        return "ptr %" + name + ", ptr %" + name + "_ready, ptr %" + name + "_pending";
    }

    void sync_scalar(const std::string& name, const std::string& cycle)
    {
        if (name == "q")
        {
            line("call void @vu_q_sync(" + scalar_args(name) +
                ", ptr %status, ptr %q_pending_flags, i32 " + cycle + ")");
        }
        else
        {
            line("call void @vu_scalar_sync(" + scalar_args(name) + ", i32 " + cycle + ")");
        }
    }

    void wait_scalar(const std::string& name)
    {
        if (name == "q")
        {
            line("call void @vu_q_wait(" + scalar_args(name) +
                ", ptr %status, ptr %q_pending_flags, ptr %cycle)");
        }
        else
        {
            line("call i32 @vu_scalar_wait(" + scalar_args(name) + ", ptr %cycle)");
        }
    }

    void schedule_scalar(const std::string& name, const std::string& value, uint32_t latency)
    {
        line("call void @vu_scalar_schedule(" + scalar_args(name) + ", ptr %cycle, i32 " +
            value + ", i32 " + std::to_string(latency) + ")");
    }

    bool emit_random_lower(uint32_t lo, uint32_t index, uint32_t fs,
        uint32_t ft, uint32_t mask)
    {
        auto current = operation("load i32, ptr %r, align 4");
        if (index == 0x42u || index == 0x43u)
        {
            const auto source = vf(fs, (lo >> 21u) & 3u);
            const auto value = index == 0x42u ? source :
                operation("xor i32 " + current + ", " + source);
            const auto bits = operation("and i32 " + value + ", 8388607");
            line("store i32 " + bits + ", ptr %r, align 4");
            return true;
        }
        if (index == 0x40u)
        {
            const auto a = operation("lshr i32 " + current + ", 4");
            const auto b = operation("lshr i32 " + current + ", 22");
            const auto mix = operation("xor i32 " + a + ", " + b);
            const auto bit = operation("and i32 " + mix + ", 1");
            const auto shifted = operation("shl i32 " + current + ", 1");
            const auto combined = operation("or i32 " + shifted + ", " + bit);
            current = operation("and i32 " + combined + ", 8388607");
            line("store i32 " + current + ", ptr %r, align 4");
        }
        const auto bits = operation("and i32 " + current + ", 8388607");
        const auto value = operation("or i32 " + bits + ", 1065353216");
        const std::array<std::string, 4> values{value, value, value, value};
        write_vector(ft, mask, values);
        return true;
    }

    bool emit_p_lower(uint32_t lo, uint32_t index, uint32_t fs,
        uint32_t ft, uint32_t mask)
    {
        if (index == 0x64u)
        {
            const auto value = operation("load i32, ptr %p, align 4");
            const std::array<std::string, 4> values{value, value, value, value};
            write_vector(ft, mask, values);
            return true;
        }
        if (index == 0x7bu)
        {
            wait_scalar("p");
            return true;
        }
        // EFU math is a shared host callback, preserving the interpreter's
        // float normalization and platform math implementation. The P
        // dependency pipeline and destination writeback remain native IR.
        std::string call = "call i64 %efu(i32 " + std::to_string(index) +
            ", i32 " + std::to_string((lo >> 21u) & 3u);
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            call += ", i32 " + vf(fs, lane);
        }
        const auto result = operation(call + ")");
        const auto value = operation("trunc i64 " + result + " to i32");
        const auto latency64 = operation("lshr i64 " + result + ", 32");
        const auto latency = operation("trunc i64 " + latency64 + " to i32");
        const auto valid = operation("icmp ne i32 " + latency + ", 0");
        const auto label = "efu_ok_" + std::to_string(m_counter++);
        line("br i1 " + valid + ", label %" + label + ", label %fail");
        m_ir += label + ":\n";
        line("call void @vu_scalar_schedule(" + scalar_args("p") +
            ", ptr %cycle, i32 " + value + ", i32 " + latency + ")");
        return true;
    }

    bool emit_q_lower(uint32_t lo, uint32_t index, uint32_t fs, uint32_t ft)
    {
        if (index == 0x3bu)
        {
            wait_scalar("q");
            return true;
        }
        const auto rhs = vf(ft, (lo >> 23u) & 3u);
        const bool is_sqrt = index == 0x39u;
        const bool is_rsqrt = index == 0x3au;
        const auto lhs = is_sqrt ? std::string("0") : vf(fs, (lo >> 21u) & 3u);
        const auto value = is_sqrt
            ? operation("call i32 @vu_sqrt(i32 " + rhs + ")")
            : fp_binary(is_rsqrt ? "vu_rsqrt" : "vu_div", lhs, rhs);
        // Drain the previous producer before replacing the pending tuple.
        wait_scalar("q");
        const auto now = operation("load i32, ptr %cycle, align 4");
        const auto ready = operation("add i32 " + now + ", " +
            std::to_string(is_rsqrt ? 13u : 7u));
        const auto flags = operation("call i32 @vu_q_status(i32 0, i32 " +
            lhs + ", i32 " + rhs + ", i1 " +
            (is_sqrt ? "true" : "false") + ", i1 " +
            (is_rsqrt ? "true" : "false") + ")");
        line("store i32 " + value + ", ptr %q_pending, align 4");
        line("store i32 " + ready + ", ptr %q_ready, align 4");
        line("store i32 " + flags + ", ptr %q_pending_flags, align 4");
        return true;
    }

    void init_ring(const std::string& name)
    {
        const auto prefix = "%" + name + "_ring_";
        line(prefix + "head = alloca i32, align 4");
        line(prefix + "times = alloca [8 x i32], align 4");
        line(prefix + "values = alloca [8 x i32], align 4");
        line("store i32 0, ptr " + prefix + "head, align 4");
        line("store [8 x i32] zeroinitializer, ptr " + prefix + "times, align 4");
        const auto initial = operation("load i32, ptr %" + name + ", align 4");
        const auto lane = operation("insertelement <8 x i32> poison, i32 " + initial + ", i32 0");
        const auto all = operation("shufflevector <8 x i32> " + lane +
            ", <8 x i32> poison, <8 x i32> zeroinitializer");
        line("store <8 x i32> " + all + ", ptr " + prefix + "values, align 4");
    }

    void push_ring(const std::string& name, const std::string& time,
        const std::string& value)
    {
        const auto prefix = "%" + name + "_ring_";
        line("call void @vu_flag_push(ptr " + prefix + "head, ptr " +
            prefix + "times, ptr " + prefix + "values, i32 " + time +
            ", i32 " + value + ")");
    }

    std::string read_ring(const std::string& name)
    {
        const auto prefix = "%" + name + "_ring_";
        return operation("call i32 @vu_flag_read(ptr " + prefix +
            "head, ptr " + prefix + "times, ptr " + prefix +
            "values, i32 " + (name == "clip" ? m_issue_cycle : m_flag_time) + ")");
    }

    void store_flag_result(uint32_t target, const std::string& value)
    {
        store(target, operation("trunc i32 " + value + " to i16"));
    }

    void store_flag_predicate(uint32_t target, const std::string& predicate)
    {
        store(target, operation("zext i1 " + predicate + " to i16"));
    }

    bool emit_flag_lower(uint32_t lo, uint32_t major, uint32_t is, uint32_t it)
    {
        const auto imm24 = std::to_string(lo & 0xffffffu);
        const uint32_t imm12 = ((lo >> 10u) & 0x800u) | (lo & 0x7ffu);
        const auto immediate = std::to_string(imm12);
        if (major == 0x11u)
        {
            line("store i32 " + imm24 + ", ptr %clip, align 4");
            if ((m_flag_history_mask & k_clip_history) != 0u)
            {
                push_ring("clip", operation("sub i32 " + m_flag_time + ", 4"), imm24);
            }
            return true;
        }
        if (major == 0x15u)
        {
            m_fsset = static_cast<int32_t>(imm12 & 0xfc0u);
            return true;
        }
        if (major == 0x10u || major == 0x12u || major == 0x13u || major == 0x1cu)
        {
            const auto clip = operation("and i32 " + read_ring("clip") + ", 16777215");
            if (major == 0x10u)
            {
                store_flag_predicate(1u, operation("icmp eq i32 " + clip + ", " + imm24));
            }
            else if (major == 0x12u)
            {
                const auto bits = operation("and i32 " + clip + ", " + imm24);
                store_flag_predicate(1u, operation("icmp ne i32 " + bits + ", 0"));
            }
            else if (major == 0x13u)
            {
                const auto bits = operation("or i32 " + clip + ", " + imm24);
                store_flag_predicate(1u, operation("icmp eq i32 " + bits + ", 16777215"));
            }
            else
            {
                store_flag_result(it, operation("and i32 " + clip + ", 4095"));
            }
            return true;
        }
        if (major == 0x14u || major == 0x16u || major == 0x17u)
        {
            // FDIV current/sticky flags are visible on Q retirement,
            // independently of the four-pair FMAC flag history.
            const auto fmac = operation("and i32 " + read_ring("status") + ", 975");
            const auto live = operation("load i32, ptr %status, align 4");
            const auto fdiv = operation("and i32 " + live + ", 3120");
            const auto status = operation("or i32 " + fmac + ", " + fdiv);
            if (major == 0x14u)
            {
                store_flag_predicate(it, operation("icmp eq i32 " + status + ", " + immediate));
            }
            else
            {
                store_flag_result(it, operation(std::string(major == 0x16u ? "and" : "or") +
                    " i32 " + status + ", " + immediate));
            }
            return true;
        }
        if (major == 0x18u || major == 0x1au || major == 0x1bu)
        {
            const auto mac = operation("and i32 " + read_ring("mac") + ", 65535");
            const auto vi = operation("zext i16 " + reg(is) + " to i32");
            if (major == 0x18u)
            {
                store_flag_predicate(it, operation("icmp eq i32 " + mac + ", " + vi));
            }
            else
            {
                store_flag_result(it, operation(std::string(major == 0x1au ? "and" : "or") +
                    " i32 " + mac + ", " + vi));
            }
            return true;
        }
        return false;
    }

    std::string ready_pointer(uint32_t index)
    {
        return operation("getelementptr i32, ptr %ready, i32 " +
            std::to_string(index));
    }

    std::string max_ready(const std::string& cycle, uint32_t index)
    {
        const auto ready = operation("load i32, ptr " + ready_pointer(index) +
            ", align 4");
        const auto wait = operation("icmp ugt i32 " + ready + ", " + cycle);
        return operation("select i1 " + wait + ", i32 " + ready +
            ", i32 " + cycle);
    }

    std::string branch_vi(uint32_t index)
    {
        if (index == 0u)
        {
            return "0";
        }
        // Only a VI actually written by the preceding nonexempt lower
        // can differ from the current value. All upper writes target VF,
        // ACC or flags, never VI. Conditional branches do not write VI.
        const auto saved = operation("getelementptr i16, ptr %previous_vi, i32 " +
            std::to_string(index));
        const auto previous = operation("load i16, ptr " + saved + ", align 2");
        const auto current = reg(index);
        const auto bit = operation("and i32 %previous_vi_mask_now, " +
            std::to_string(1u << index));
        const auto delayed = operation("icmp ne i32 " + bit + ", 0");
        return operation("select i1 " + delayed + ", i16 " + previous +
            ", i16 " + current);
    }

    bool emit_branch(uint32_t lo, uint32_t major, uint32_t is, uint32_t it)
    {
        if (major == 0x24u || major == 0x25u)
        {
            // JR/JALR read the current VI, not the delayed conditional
            // branch value. The switch guards unknown indirect targets.
            const auto target = operation("zext i16 " + reg(is) + " to i32");
            const auto bytes = operation("shl i32 " + target + ", 3");
            m_next_target = operation("and i32 " + bytes + ", 16383");
        }
        else
        {
            const auto target = std::to_string(relative_target(m_current_pc, lo));
            if (major == 0x20u || major == 0x21u)
            {
                m_next_target = target;
            }
            else
            {
                const char* predicate = nullptr;
                switch (major)
                {
                case 0x28u: predicate = "eq"; break;
                case 0x29u: predicate = "ne"; break;
                case 0x2cu: predicate = "slt"; break;
                case 0x2du: predicate = "sgt"; break;
                case 0x2eu: predicate = "sle"; break;
                case 0x2fu: predicate = "sge"; break;
                default: return false;
                }
                const auto lhs = branch_vi(is);
                const auto rhs = major == 0x28u || major == 0x29u ?
                    branch_vi(it) : "0";
                const auto taken = operation(std::string("icmp ") + predicate +
                    " i16 " + lhs + ", " + rhs);
                m_next_target = operation("select i1 " + taken + ", i32 " +
                    target + ", i32 %sequential");
            }
        }
        if (major == 0x21u || major == 0x25u)
        {
            store(it, std::to_string((m_current_pc + 16u) / 8u));
        }
        return true;
    }

    std::string operation(const std::string& instruction)
    {
        const auto value = tmp();
        line(value + " = " + instruction);
        return value;
    }

    // Only the VF/ACC bank used by the emitted microprogram is staged
    // here. AOT passes separate VF/ACC storage; its kick callback reads
    // VU data and its EFU callback receives raw scalar operands, never VF.
    // Non-escaping fixed-size allocas allow SROA/mem2reg across switch
    // targets and loops without changing the dispatcher or pair timing.
    std::string local_vf(uint32_t reg_index)
    {
        m_dynamic_vf_accessed.set(reg_index);
        return "%vf_local_" + std::to_string(reg_index);
    }

    std::string acc_base()
    {
        if (!m_dynamic)
        {
            return "%acc";
        }
        m_dynamic_acc_accessed = true;
        return "%acc_local";
    }

    void initialize_dynamic_vectors()
    {
        std::string prologue;
        for (uint32_t reg_index = 1u; reg_index < 32u; ++reg_index)
        {
            if (!m_dynamic_vf_accessed.test(reg_index))
            {
                continue;
            }
            const auto suffix = std::to_string(reg_index);
            const auto backing = "%vf_backing_" + suffix;
            const auto initial = "%vf_initial_" + suffix;
            const auto local = "%vf_local_" + suffix;
            prologue += "  " + backing + " = getelementptr i32, ptr %vf, i32 " +
                std::to_string(reg_index * 4u) + "\n";
            prologue += "  " + initial + " = load <4 x i32>, ptr " + backing + ", align 4\n";
            prologue += "  " + local + " = alloca <4 x i32>, align 16\n";
            prologue += "  store <4 x i32> " + initial + ", ptr " + local + ", align 16\n";
        }
        if (m_dynamic_acc_accessed)
        {
            prologue += "  %acc_initial = load <4 x i32>, ptr %acc, align 4\n"
                "  %acc_local = alloca <4 x i32>, align 16\n"
                "  store <4 x i32> %acc_initial, ptr %acc_local, align 16\n";
        }
        m_ir.insert(m_dynamic_local_offset, prologue);
    }

    void flush_dynamic_vectors()
    {
        for (uint32_t reg_index = 1u; reg_index < 32u; ++reg_index)
        {
            if (m_dynamic_vf_written.test(reg_index))
            {
                const auto value = operation("load <4 x i32>, ptr " +
                    local_vf(reg_index) + ", align 16");
                line("store <4 x i32> " + value + ", ptr %vf_backing_" +
                    std::to_string(reg_index) + ", align 4");
            }
        }
        if (m_dynamic_acc_written)
        {
            const auto value = operation("load <4 x i32>, ptr " + acc_base() + ", align 16");
            line("store <4 x i32> " + value + ", ptr %acc, align 4");
        }
    }

    std::string vf_pointer(uint32_t reg_index, uint32_t lane)
    {
        if (m_dynamic)
        {
            const auto base = local_vf(reg_index);
            return lane == 0u ? base : operation("getelementptr i32, ptr " +
                base + ", i32 " + std::to_string(lane));
        }
        return operation("getelementptr i32, ptr %vf, i32 " +
            std::to_string(reg_index * 4u + lane));
    }

    std::string vf(uint32_t reg_index, uint32_t lane)
    {
        if (reg_index == 0u)
        {
            return lane == 3u ? "1065353216" : "0";
        }
        auto& cached = m_vf_lanes[reg_index][lane];
        if (cached.empty())
        {
            const auto& vector = m_vf_vectors[reg_index];
            cached = vector.empty()
                ? operation("load i32, ptr " + vf_pointer(reg_index, lane) + ", align 4")
                : operation("extractelement <4 x i32> " + vector + ", i32 " + std::to_string(lane));
        }
        return cached;
    }

    std::string vf_vector(uint32_t reg_index)
    {
        if (reg_index == 0u)
        {
            return "<i32 0, i32 0, i32 0, i32 1065353216>";
        }
        auto& cached = m_vf_vectors[reg_index];
        if (cached.empty())
        {
            // Writes remain published at the original architectural point.
            // A full load therefore also sees any earlier partial writes.
            cached = operation("load <4 x i32>, ptr " + vf_pointer(reg_index, 0u) +
                ", align 4");
        }
        return cached;
    }

    std::string splat(const std::string& value)
    {
        const auto first = operation("insertelement <4 x i32> poison, i32 " +
            value + ", i32 0");
        return operation("shufflevector <4 x i32> " + first +
            ", <4 x i32> poison, <4 x i32> zeroinitializer");
    }

    std::string data_address(uint32_t reg_index, int32_t offset)
    {
        const auto base = operation("zext i16 " + reg(reg_index) + " to i32");
        const auto sum = operation("add i32 " + base + ", " + std::to_string(offset));
        const auto wrapped = operation("and i32 " + sum + ", 1023");
        return operation("shl i32 " + wrapped + ", 4");
    }

    std::string data_pointer(const std::string& address, uint32_t lane)
    {
        const auto offset = operation("add i32 " + address + ", " +
            std::to_string(lane * 4u));
        return operation("getelementptr i8, ptr %data, i32 " + offset);
    }

    std::string data_lane(const std::string& address, uint32_t lane)
    {
        return operation("load i32, ptr " + data_pointer(address, lane) + ", align 1");
    }

    void store_data_lane(const std::string& address, uint32_t lane,
        const std::string& value)
    {
        line("store i32 " + value + ", ptr " + data_pointer(address, lane) +
            ", align 1");
        m_writes_data = true;
    }

    void store_vector(uint32_t source, uint32_t mask, const std::string& address)
    {
        if (mask == 15u)
        {
            // VU data addresses wrap in quadwords, not individual lanes.
            // VF and VU data are disjoint buffers in the native ABI.
            const auto value = vf_vector(source);
            line("store <4 x i32> " + value + ", ptr " +
                data_pointer(address, 0u) + ", align 1");
            m_writes_data = true;
            return;
        }
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            if ((mask & (8u >> lane)) != 0u)
            {
                store_data_lane(address, lane, vf(source, lane));
            }
        }
    }

    void store_integer(uint32_t source, uint32_t mask, const std::string& address)
    {
        const auto value = operation("zext i16 " + reg(source) + " to i32");
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            if ((mask & (8u >> lane)) != 0u)
            {
                store_data_lane(address, lane, value);
            }
        }
    }

    std::string assemble_vector(const std::array<std::string, 4>& values)
    {
        std::string result = "poison";
        for (uint32_t lane = 0; lane < 4u; ++lane)
        {
            result = operation("insertelement <4 x i32> " + result + ", i32 " +
                values[lane] + ", i32 " + std::to_string(lane));
        }
        return result;
    }

    void write_full_vf(uint32_t target, const std::string& value)
    {
        if (target != 0u)
        {
            if (m_dynamic)
            {
                m_dynamic_vf_written.set(target);
            }
            line("store <4 x i32> " + value + ", ptr " +
                vf_pointer(target, 0u) + ", align 4");
            m_vf_vectors[target] = value;
            m_vf_lanes[target] = {};
        }
    }

    void write_vector(uint32_t target, uint32_t mask,
        const std::array<std::string, 4>& values,
        const std::string& vector = {})
    {
        if (target == 0u || mask == 0u)
        {
            return;
        }
        if (mask == 15u)
        {
            write_full_vf(target, vector.empty() ? assemble_vector(values) : vector);
            return;
        }
        if (m_dynamic)
        {
            m_dynamic_vf_written.set(target);
        }
        m_vf_vectors[target].clear();
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            if ((mask & (8u >> lane)) != 0u)
            {
                line("store i32 " + values[lane] + ", ptr " +
                    vf_pointer(target, lane) + ", align 4");
                m_vf_lanes[target][lane] = values[lane];
            }
        }
    }

    void load_vector(uint32_t target, uint32_t mask, const std::string& address)
    {
        if (target != 0u && mask == 15u)
        {
            // A full quadword is contiguous even at the last wrapped
            // address (0x3ff0). Do not assume host data alignment.
            const auto value = operation("load <4 x i32>, ptr " +
                data_pointer(address, 0u) + ", align 1");
            write_full_vf(target, value);
            return;
        }
        std::array<std::string, 4> values{};
        if (target != 0u)
        {
            for (uint32_t lane = 0u; lane < 4u; ++lane)
            {
                if ((mask & (8u >> lane)) != 0u)
                {
                    values[lane] = data_lane(address, lane);
                }
            }
            write_vector(target, mask, values);
        }
    }

    void load_integer(uint32_t target, uint32_t mask, const std::string& address,
        bool first_lane)
    {
        // ILW uses the last selected lane, ILWR the first selected lane.
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            if ((mask & (8u >> lane)) != 0u)
            {
                store(target, operation("trunc i32 " + data_lane(address, lane) + " to i16"));
                if (first_lane)
                {
                    break;
                }
            }
        }
    }

    void commit_upper()
    {
        if (m_upper_acc)
        {
            if (m_dynamic && m_upper_mask != 0u)
            {
                m_dynamic_acc_written = true;
            }
            if (m_upper_mask == 15u)
            {
                const auto value = m_upper_vector.empty()
                    ? assemble_vector(m_upper_values) : m_upper_vector;
                line("store <4 x i32> " + value + ", ptr " + acc_base() + ", align 4");
                m_acc_vector = value;
                m_acc_lanes = {};
            }
            else if (m_upper_mask != 0u)
            {
                m_acc_vector.clear();
                for (uint32_t lane = 0u; lane < 4u; ++lane)
                {
                    if ((m_upper_mask & (8u >> lane)) != 0u)
                    {
                        line("store i32 " + m_upper_values[lane] + ", ptr " +
                            acc_pointer(lane) + ", align 4");
                        m_acc_lanes[lane] = m_upper_values[lane];
                    }
                }
            }
            if (m_upper_flags && m_upper_mask != 0u)
            {
                const auto latch = operation("load i32, ptr %acc_overflow, align 4");
                auto next = operation("and i32 " + latch + ", " + std::to_string(~m_upper_mask));
                if (!m_upper_over_vector.empty())
                {
                    next = operation("or i32 " + next + ", " +
                        pack_flags(m_upper_over_vector, m_upper_mask));
                }
                else
                {
                    for (uint32_t lane = 0u; lane < 4u; ++lane)
                    {
                        const uint32_t bit = 8u >> lane;
                        if ((m_upper_mask & bit) != 0u)
                        {
                            const auto flag = operation("select i1 " + m_upper_overflow[lane] +
                                ", i32 " + std::to_string(bit) + ", i32 0");
                            next = operation("or i32 " + next + ", " + flag);
                        }
                    }
                }
                line("store i32 " + next + ", ptr %acc_overflow, align 4");
            }
        }
        else
        {
            write_vector(m_upper_target, m_upper_mask, m_upper_values, m_upper_vector);
        }
        if (m_upper_flags)
        {
            emit_flags(m_upper_values, m_upper_mask);
        }
    }

    std::string pack_flags(const std::string& lanes, uint32_t mask)
    {
        // x,y,z,w map to guest MAC bits 8,4,2,1 respectively.
        const auto reversed = operation("shufflevector <4 x i1> " + lanes +
            ", <4 x i1> poison, <4 x i32> <i32 3, i32 2, i32 1, i32 0>");
        const auto packed = operation("bitcast <4 x i1> " + reversed + " to i4");
        const auto bits = operation("zext i4 " + packed + " to i32");
        return mask == 15u ? bits :
            operation("and i32 " + bits + ", " + std::to_string(mask));
    }

    void emit_flags(const std::array<std::string, 4>& values, uint32_t mask)
    {
        std::string mac = "0";
        if (!m_upper_vector.empty())
        {
            const auto exp = operation("and <4 x i32> " + m_upper_vector +
                ", <i32 2139095040, i32 2139095040, i32 2139095040, i32 2139095040>");
            const auto zero = operation("icmp eq <4 x i32> " + exp + ", zeroinitializer");
            const auto sign = operation("icmp slt <4 x i32> " + m_upper_vector + ", zeroinitializer");
            mac = pack_flags(zero, mask);
            for (const auto& group : {std::make_pair(sign, 4u),
                                      std::make_pair(m_upper_under_vector, 8u),
                                      std::make_pair(m_upper_over_vector, 12u)})
            {
                const auto bits = pack_flags(group.first, mask);
                const auto shifted = operation("shl i32 " + bits + ", " + std::to_string(group.second));
                mac = operation("or i32 " + mac + ", " + shifted);
            }
        }
        else for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            const uint32_t bit = 8u >> lane;
            if ((mask & bit) == 0u)
            {
                continue;
            }
            const auto exp = operation("and i32 " + values[lane] + ", 2139095040");
            const auto zero = operation("icmp eq i32 " + exp + ", 0");
            const auto sign = operation("icmp slt i32 " + values[lane] + ", 0");
            const auto& overflow = m_upper_overflow[lane];
            const auto& underflow = m_upper_underflow[lane];
            for (const auto& test : {std::make_pair(zero, bit),
                                     std::make_pair(sign, bit << 4u),
                                     std::make_pair(underflow, bit << 8u),
                                     std::make_pair(overflow, bit << 12u)})
            {
                const auto flag = operation("select i1 " + test.first +
                    ", i32 " + std::to_string(test.second) + ", i32 0");
                mac = operation("or i32 " + mac + ", " + flag);
            }
        }
        line("store i32 " + mac + ", ptr %mac, align 4");
        const auto old = operation("load i32, ptr %status, align 4");
        auto status = operation("and i32 " + old + ", 4080");
        if (!m_upper_sticky_vector.empty() && mask == 15u)
        {
            const auto sticky = operation("call i32 @llvm.vector.reduce.or.v4i32(<4 x i32> " +
                m_upper_sticky_vector + ")");
            status = operation("or i32 " + status + ", " + sticky);
        }
        else
        {
            for (uint32_t lane = 0u; lane < 4u; ++lane)
            {
                if ((mask & (8u >> lane)) != 0u)
                {
                    status = operation("or i32 " + status + ", " + m_upper_sticky[lane]);
                }
            }
        }
        for (uint32_t group = 0u; group < 4u; ++group)
        {
            const auto bits = operation("and i32 " + mac + ", " +
                std::to_string(15u << (group * 4u)));
            const auto any = operation("icmp ne i32 " + bits + ", 0");
            const auto flag = operation("select i1 " + any + ", i32 " +
                std::to_string((1u << group) | (1u << (group + 6u))) + ", i32 0");
            status = operation("or i32 " + status + ", " + flag);
        }
        line("store i32 " + status + ", ptr %status, align 4");
        if ((m_flag_history_mask & k_mac_history) != 0u)
        {
            push_ring("mac", m_flag_time, mac);
        }
        if ((m_flag_history_mask & k_status_history) != 0u)
        {
            push_ring("status", m_flag_time, status);
        }
    }

    std::string acc_pointer(uint32_t lane)
    {
        const auto base = acc_base();
        return lane == 0u ? base : operation("getelementptr i32, ptr " +
            base + ", i32 " + std::to_string(lane));
    }

    std::string acc(uint32_t lane)
    {
        auto& value = m_acc_lanes[lane];
        if (value.empty())
        {
            value = m_acc_vector.empty()
                ? operation("load i32, ptr " + acc_pointer(lane) + ", align 4")
                : operation("extractelement <4 x i32> " + m_acc_vector + ", i32 " +
                    std::to_string(lane));
        }
        return value;
    }

    std::string acc_vector()
    {
        if (m_acc_vector.empty())
        {
            m_acc_vector = operation("load <4 x i32>, ptr " + acc_base() + ", align 4");
        }
        return m_acc_vector;
    }

    std::string fp_binary(const char* helper, const std::string& lhs,
        const std::string& rhs)
    {
        return operation(std::string("call i32 @") + helper +
            "(i32 " + lhs + ", i32 " + rhs + ")");
    }

    bool emit_upper(uint32_t hi)
    {
        const uint32_t funct = hi & 63u;
        const bool special = funct >= 0x3cu;
        const uint32_t op = special ?
            (((hi >> 6u) & 31u) << 2u) | (funct & 3u) : funct;
        const uint32_t mask = (hi >> 21u) & 15u;
        const uint32_t fs = (hi >> 11u) & 31u;
        const uint32_t ft = (hi >> 16u) & 31u;
        const uint32_t fd = (hi >> 6u) & 31u;
        if (special && op == 0x2fu)
        {
            return true;
        }

        const bool abs = special && op == 0x1du;
        const bool cross = op == 0x2eu;
        const bool conversion = special && op >= 0x10u && op <= 0x17u;
        if (special && op == 0x1fu)
        {
            if (!m_flag_pipeline)
            {
                return false;
            }
            const auto abs_w = operation("and i32 " + vf(ft, 3u) + ", 2147483647");
            const auto limit = operation("call i32 @vu_key(i32 " + abs_w + ")");
            const auto neg = operation("sub i32 0, " + limit);
            std::string bits = "0";
            for (uint32_t lane = 0u; lane < 3u; ++lane)
            {
                const auto x = operation("call i32 @vu_key(i32 " + vf(fs, lane) + ")");
                const auto above = operation("icmp sgt i32 " + x + ", " + limit);
                const auto below = operation("icmp slt i32 " + x + ", " + neg);
                const auto p = operation("select i1 " + above + ", i32 " +
                    std::to_string(1u << (lane * 2u)) + ", i32 0");
                const auto n = operation("select i1 " + below + ", i32 " +
                    std::to_string(2u << (lane * 2u)) + ", i32 0");
                bits = operation("or i32 " + bits + ", " + p);
                bits = operation("or i32 " + bits + ", " + n);
            }
            const auto old = operation("load i32, ptr %clip, align 4");
            const auto shifted = operation("shl i32 " + old + ", 6");
            const auto combined = operation("or i32 " + shifted + ", " + bits);
            m_pending_clip = operation("and i32 " + combined + ", 16777215");
            return true;
        }
        const bool madd = (op >= 0x08u && op <= 0x0bu) ||
            op == 0x21u || op == 0x23u || op == 0x29u;
        const bool msub = (op >= 0x0cu && op <= 0x0fu) ||
            op == 0x25u || op == 0x27u || op == 0x2du;
        const bool mul = (op >= 0x18u && op <= 0x1cu) ||
            op == 0x1eu || op == 0x2au;
        const char* helper = nullptr;
        if (op <= 0x03u || op == 0x20u || op == 0x22u || op == 0x28u)
        {
            helper = "vu_add";
        }
        else if ((op >= 0x04u && op <= 0x07u) ||
            op == 0x24u || op == 0x26u || op == 0x2cu)
        {
            helper = "vu_sub";
        }
        else if (mul)
        {
            helper = "vu_mul";
        }
        else if (!special && ((op >= 0x10u && op <= 0x13u) ||
            op == 0x1du || op == 0x2bu))
        {
            helper = "vu_max";
        }
        else if (!special && ((op >= 0x14u && op <= 0x17u) ||
            op == 0x1fu || op == 0x2fu))
        {
            helper = "vu_min";
        }
        if (!conversion && !abs && !cross && !madd && !msub && helper == nullptr)
        {
            return false;
        }
        const bool flags = !conversion && !abs && !(op >= 0x10u && op <= 0x17u) &&
            op != 0x1du && op != 0x1fu && op != 0x2bu && op != 0x2fu;
        const bool uses_q = op == 0x1cu || op == 0x20u || op == 0x21u ||
            op == 0x24u || op == 0x25u;
        const bool uses_i = op == 0x1du || op == 0x1eu || op == 0x1fu ||
            op == 0x22u || op == 0x23u || op == 0x26u || op == 0x27u;
        const uint32_t write_mask = cross ? 0xeu : mask;
        std::array<std::string, 4> values{};
        // Stage all active lanes before lower execution. Single-lane FMAC
        // keeps scalar lowering; multi-lane FMAC uses integer vectors.
        if (flags && (write_mask & (write_mask - 1u)) != 0u)
        {
            const uint32_t kind = (cross && special) || mul ? 2u :
                madd ? 3u : (msub || cross) ? 4u :
                std::string(helper) == "vu_sub" ? 1u : 0u;
            const bool mac = kind >= 3u;
            auto lhs = vf_vector(fs);
            std::string rhs;
            if (cross)
            {
                lhs = operation("shufflevector <4 x i32> " + lhs +
                    ", <4 x i32> poison, <4 x i32> <i32 1, i32 2, i32 0, i32 0>");
                rhs = operation("shufflevector <4 x i32> " + vf_vector(ft) +
                    ", <4 x i32> poison, <4 x i32> <i32 2, i32 0, i32 1, i32 0>");
            }
            else if (op <= 0x1bu)
            {
                rhs = splat(vf(ft, funct & 3u));
            }
            else if (uses_q || uses_i)
            {
                rhs = splat(operation(uses_q ? "load i32, ptr %q, align 4" :
                    "load i32, ptr %i, align 4"));
            }
            else
            {
                rhs = vf_vector(ft);
            }
            if (write_mask != 15u)
            {
                std::string active = "<";
                for (uint32_t lane = 0u; lane < 4u; ++lane)
                {
                    if (lane != 0u)
                    {
                        active += ", ";
                    }
                    active += (write_mask & (8u >> lane)) != 0u ? "i1 true" : "i1 false";
                }
                active += ">";
                // Inactive lanes cannot enter the Booth correction path.
                lhs = operation("select <4 x i1> " + active + ", <4 x i32> " +
                    lhs + ", <4 x i32> zeroinitializer");
            }
            const auto acc_value = mac ? acc_vector() : std::string("zeroinitializer");
            std::string latched = "zeroinitializer";
            if (mac)
            {
                const auto latch = splat(operation("load i32, ptr %acc_overflow, align 4"));
                const auto bits = operation("and <4 x i32> " + latch +
                    ", <i32 8, i32 4, i32 2, i32 1>");
                latched = operation("icmp ne <4 x i32> " + bits + ", zeroinitializer");
            }
            const std::string type = "{ <4 x i32>, <4 x i1>, <4 x i1>, <4 x i32> }";
            const auto result = operation("call " + type + " @vu_fmac4(i32 " +
                std::to_string(kind) + ", <4 x i32> " + acc_value +
                ", <4 x i32> " + lhs + ", <4 x i32> " + rhs +
                ", <4 x i1> " + latched + ")");
            const auto bits = operation("extractvalue " + type + " " + result + ", 0");
            const auto over = operation("extractvalue " + type + " " + result + ", 1");
            const auto under = operation("extractvalue " + type + " " + result + ", 2");
            const auto sticky = operation("extractvalue " + type + " " + result + ", 3");
            m_upper_vector = bits;
            m_upper_over_vector = over;
            m_upper_under_vector = under;
            m_upper_sticky_vector = sticky;
            for (uint32_t lane = 0u; lane < 4u; ++lane)
            {
                if ((write_mask & (8u >> lane)) != 0u)
                {
                    const auto index = ", i32 " + std::to_string(lane);
                    values[lane] = operation("extractelement <4 x i32> " + bits + index);
                    m_upper_overflow[lane] = operation("extractelement <4 x i1> " + over + index);
                    m_upper_underflow[lane] = operation("extractelement <4 x i1> " + under + index);
                    m_upper_sticky[lane] = operation("extractelement <4 x i32> " + sticky + index);
                }
            }
        }
        else
        {
            for (uint32_t lane = 0u; lane < 4u; ++lane)
            {
                if ((write_mask & (8u >> lane)) == 0u)
                {
                    continue;
                }
                const auto lhs = vf(fs, cross ? (lane + 1u) % 3u : lane);
                if (conversion)
                {
                    constexpr uint32_t fractions[]{0u, 4u, 12u, 15u};
                    values[lane] = fp_binary(op <= 0x13u ? "vu_from_fixed" : "vu_to_fixed",
                        lhs, std::to_string(fractions[op & 3u]));
                }
                else if (abs)
                {
                    values[lane] = operation("and i32 " + lhs + ", 2147483647");
                }
                else
                {
                    const auto rhs = cross ? vf(ft, (lane + 2u) % 3u) :
                        op <= 0x1bu ? vf(ft, funct & 3u) :
                        uses_q ? operation("load i32, ptr %q, align 4") : uses_i ?
                        operation("load i32, ptr %i, align 4") : vf(ft, lane);
                    if (flags)
                    {
                        const uint32_t kind = (cross && special) || mul ? 2u :
                            madd ? 3u : (msub || cross) ? 4u :
                            std::string(helper) == "vu_sub" ? 1u : 0u;
                        const bool mac = kind >= 3u;
                        const auto acc_value = mac ? acc(lane) : std::string("0");
                        std::string latched = "false";
                        if (mac)
                        {
                            const auto latch = operation("load i32, ptr %acc_overflow, align 4");
                            const auto bit = operation("and i32 " + latch + ", " + std::to_string(8u >> lane));
                            latched = operation("icmp ne i32 " + bit + ", 0");
                        }
                        const auto tuple = operation("call { i32, i1, i1, i32 } @vu_fmac(i32 " +
                            std::to_string(kind) + ", i32 " + acc_value + ", i32 " + lhs +
                            ", i32 " + rhs + ", i1 " + latched + ")");
                        const std::string type = "extractvalue { i32, i1, i1, i32 } ";
                        values[lane] = operation(type + tuple + ", 0");
                        m_upper_overflow[lane] = operation(type + tuple + ", 1");
                        m_upper_underflow[lane] = operation(type + tuple + ", 2");
                        m_upper_sticky[lane] = operation(type + tuple + ", 3");
                    }
                    else
                    {
                        values[lane] = fp_binary(helper, lhs, rhs);
                    }
                }
            }
        }
        m_upper_target = conversion || abs ? ft : fd;
        m_upper_mask = write_mask;
        m_upper_values = std::move(values);
        m_upper_flags = flags;
        m_upper_acc = special && !abs && !conversion;
        return true;
    }

    std::string tmp()
    {
        return "%v" + std::to_string(m_counter++);
    }

    void line(const std::string& text)
    {
        m_ir += "  " + text + "\n";
    }

    std::string pointer(uint32_t reg_index)
    {
        const auto result = tmp();
        line(result + " = getelementptr i16, ptr %vi, i32 " +
            std::to_string(reg_index));
        return result;
    }

    std::string reg(uint32_t reg_index)
    {
        if (reg_index == 0u)
        {
            return "0";
        }
        const auto address = pointer(reg_index);
        const auto value = tmp();
        line(value + " = load i16, ptr " + address + ", align 2");
        return value;
    }

    void store(uint32_t reg_index, const std::string& value)
    {
        if (reg_index != 0u)
        {
            const auto address = pointer(reg_index);
            const uint32_t bit = 1u << reg_index;
            if (m_dynamic && m_delay_vi_writes &&
                (m_branch_vi_mask & bit) != 0u && (m_delay_vi_mask & bit) == 0u)
            {
                // Capture the pre-pair value once, even when ILW writes
                // several selected lanes to the same integer register.
                const auto previous = operation("load i16, ptr " + address + ", align 2");
                const auto saved = operation("getelementptr i16, ptr %previous_vi, i32 " +
                    std::to_string(reg_index));
                line("store i16 " + previous + ", ptr " + saved + ", align 2");
                m_delay_vi_mask |= bit;
            }
            line("store i16 " + value + ", ptr " + address + ", align 2");
        }
    }

    void binary(const char* op, uint32_t target, uint32_t source,
        const std::string& rhs)
    {
        if (target != 0u)
        {
            const auto lhs = reg(source);
            const auto result = tmp();
            // No nsw/nuw: VI arithmetic wraps at 16 bits.
            line(result + " = " + op + " i16 " + lhs + ", " + rhs);
            store(target, result);
        }
    }

    std::string m_ir;
    // Write-through SSA cache: within a dynamic pair or across the proven
    // straight-line path. Lower writes update cache before upper retirement.
    size_t m_dynamic_local_offset = 0u;
    std::bitset<32> m_dynamic_vf_accessed{}, m_dynamic_vf_written{};
    bool m_dynamic_acc_accessed = false;
    bool m_dynamic_acc_written = false;
    std::array<std::array<std::string, 4>, 32> m_vf_lanes{};
    std::array<std::string, 32> m_vf_vectors{};
    std::array<std::string, 4> m_acc_lanes{};
    std::string m_acc_vector;
    std::string m_upper_vector, m_upper_over_vector, m_upper_under_vector, m_upper_sticky_vector;
    std::array<std::string, 4> m_upper_values{};
    std::array<std::string, 4> m_upper_overflow{};
    std::array<std::string, 4> m_upper_underflow{};
    std::array<std::string, 4> m_upper_sticky{};
    uint32_t m_upper_target = 0u;
    uint32_t m_upper_mask = 0u;
    bool m_upper_flags = false;
    bool m_upper_acc = false;
    bool m_writes_data = false;
    bool m_dynamic = false;
    bool m_delay_vi_writes = false;
    uint32_t m_delay_vi_mask = 0u;
    uint32_t m_branch_vi_mask = 0u;
    bool m_flag_pipeline = false;
    uint32_t m_flag_history_mask = 0u;
    bool m_q_pipeline = false;
    bool m_p_pipeline = false;
    std::string m_flag_time;
    std::string m_issue_cycle;
    std::string m_pending_clip;
    int32_t m_fsset = -1;
    uint32_t m_current_pc = 0u;
    std::string m_next_target;
    uint32_t m_counter = 0u;
};
} // namespace

s_ee_vu1_ir_program vu1_lift_straight_line_ir(
    const std::array<uint8_t, c_ee_vu1::k_memory_size>& code,
    uint32_t address)
{
    if ((address & 7u) != 0u)
    {
        return {};
    }
    c_straight_line_emitter emitter;
    s_ee_vu1_dependency dependencies{};
    uint32_t cycle = 16u;
    uint32_t pc = address & 0x3ff8u;
    bool ending = false;
    // A straight-line program must encounter E within one revolution
    // of micro memory, followed by its actual delay-slot pair.
    for (uint32_t steps = 1u; steps <= 2049u; ++steps)
    {
        const uint32_t lo = read_word(code.data() + pc);
        const uint32_t hi = read_word(code.data() + pc + 4u);
        if (is_flag_pair(lo, hi) || is_q_pair(lo, hi) ||
            is_p_pair(lo, hi) || is_kick_pair(lo, hi) || !emitter.emit(lo, hi))
        {
            return {};
        }
        // With no branches, Q/P producers or dynamic waits, the
        // same scoreboard can calculate all issue cycles at lift time.
        const auto use = s_ee_vu1_dependency::decode(lo, hi);
        cycle = dependencies.issue(use, cycle + 1u);
        dependencies.retire(use, cycle);
        pc = (pc + 8u) & 0x3fffu;
        if (ending)
        {
            return {emitter.finish(steps, cycle - 16u, pc), steps, cycle - 16u, pc,
                emitter.writes_data()};
        }
        ending = (hi & 0x40000000u) != 0u;
    }
    return {};
}
s_ee_vu1_ir_program vu1_lift_program_ir(
    const std::array<uint8_t, c_ee_vu1::k_memory_size>& code,
    uint32_t address)
{
    auto straight = vu1_lift_straight_line_ir(code, address);
    if (!straight.m_ir.empty() || (address & 7u) != 0u)
    {
        return straight;
    }
    // Discover direct control-flow with independent PC/NPC and E-delay
    // state. For indirect branches compile the known return continuations
    // and delay slots; an unknown target exits through the native guard.
    using t_visit = std::tuple<uint32_t, uint32_t, bool>;
    std::set<t_visit> visited;
    std::vector<t_visit> queue;
    std::bitset<2048> reached;
    const uint32_t start = address & 0x3ff8u;
    const auto enqueue = [&](uint32_t pc, uint32_t npc, bool ending)
    {
        if (pc != k_unknown_pc && visited.emplace(pc, npc, ending).second)
        {
            queue.emplace_back(pc, npc, ending);
        }
    };
    enqueue(start, (start + 8u) & 0x3fffu, false);
    bool has_end = false;
    bool has_branch = false;
    bool has_flags = false;
    bool has_q = false;
    bool has_kick = false;
    bool has_p = false;
    uint32_t branch_vi_mask = 0u;
    uint32_t flag_history_mask = 0u;
    for (size_t index = 0u; index < queue.size(); ++index)
    {
        // Avoid pathological branch-in-delay graphs at lift time.
        if (queue.size() > 16384u)
        {
            return {};
        }
        const auto [pc, npc, ending] = queue[index];
        reached.set(pc >> 3u);
        const uint32_t lo = read_word(code.data() + pc);
        const uint32_t hi = read_word(code.data() + pc + 4u);
        const bool branch = is_branch(lo, hi);
        has_branch |= branch;
        // Include every emitted conditional branch, even an E-delay
        // pair. Indirect transfers can only target emitted switch cases.
        if (branch)
        {
            const uint32_t major = lo >> 25u;
            if (major == 0x28u || major == 0x29u ||
                (major >= 0x2cu && major <= 0x2fu))
            {
                branch_vi_mask |= 1u << ((lo >> 11u) & 15u);
                if (major == 0x28u || major == 0x29u)
                {
                    branch_vi_mask |= 1u << ((lo >> 16u) & 15u);
                }
            }
        }
        has_flags |= is_flag_pair(lo, hi);
        flag_history_mask |= flag_history_reads(lo, hi);
        has_q |= is_q_pair(lo, hi);
        has_kick |= is_kick_pair(lo, hi);
        has_p |= is_p_pair(lo, hi);
        if (ending)
        {
            has_end = true;
            continue;
        }
        const bool next_end = (hi & 0x40000000u) != 0u;
        const uint32_t sequential = npc == k_unknown_pc ?
            k_unknown_pc : (npc + 8u) & 0x3fffu;
        if (!branch)
        {
            enqueue(npc, sequential, next_end);
            continue;
        }
        const uint32_t major = lo >> 25u;
        if (major == 0x21u || major == 0x25u)
        {
            // Link continuation is a possible indirect return target.
            enqueue((pc + 16u) & 0x3fffu, (pc + 24u) & 0x3fffu, false);
        }
        if (major == 0x24u || major == 0x25u)
        {
            enqueue(npc, k_unknown_pc, next_end);
            continue;
        }
        enqueue(npc, relative_target(pc, lo), next_end);
        if (major != 0x20u && major != 0x21u)
        {
            enqueue(npc, sequential, next_end);
        }
    }
    if ((!has_branch && !has_flags && !has_q && !has_kick && !has_p) || !has_end)
    {
        return {};
    }
    c_straight_line_emitter emitter;
    // VI0 is constant, and JR/JALR consume live VI values.
    emitter.begin_dynamic(start, reached, has_flags, has_q, has_p,
        branch_vi_mask & ~1u, flag_history_mask);
    for (uint32_t index = 0u; index < reached.size(); ++index)
    {
        if (!reached.test(index))
        {
            continue;
        }
        const uint32_t pc = index * 8u;
        if (!emitter.emit_dynamic_pair(pc, read_word(code.data() + pc),
                read_word(code.data() + pc + 4u)))
        {
            return {};
        }
    }
    // The full image remains in library metadata, but the loader only
    // needs to compare instruction bytes for emitted switch cases.
    return {emitter.finish_dynamic(), 0u, 0u, 0u, emitter.writes_data(), reached};
}

std::string vu1_aot_module_ir(
    const std::array<uint8_t, c_ee_vu1::k_memory_size>& code,
    uint32_t address, bool windows_exports)
{
    auto program = vu1_lift_program_ir(code, address);
    if (program.m_ir.empty())
    {
        return {};
    }
    const uint32_t start = address & 0x3ff8u;
    const std::string linkage = windows_exports ? "dso_local dllexport " : "dso_local ";
    if (windows_exports)
    {
        const auto at = program.m_ir.find("define dso_local i32 @ee_vu1_program");
        program.m_ir.replace(at, sizeof("define dso_local ") - 1u,
            "define " + linkage);
        // COFF codegen references the MSVC floating-point marker when
        // DIV/SQRT/RSQRT lower to host FP. These DLLs intentionally have
        // no CRT or entry point; supply the marker locally in each DLL.
        program.m_ir += "\n@_fltused = dso_local global i32 9875, align 4\n";
    }
    program.m_ir += "\n@vu1_metadata = private constant [5 x i32] [i32 " +
        std::to_string(k_vu1_aot_abi) + ", i32 " + std::to_string(start) +
        ", i32 " + std::to_string(program.m_steps) + ", i32 " +
        std::to_string(program.m_cycles) + ", i32 " +
        std::to_string(program.m_next_pc) + "]\n";
    const uint32_t size = program.m_steps == 0u ?
        c_ee_vu1::k_memory_size : program.m_steps * 8u;
    program.m_ir += "@vu1_code = private constant [" +
        std::to_string(size) + " x i8] c\"";
    constexpr char hex[] = "0123456789abcdef";
    for (uint32_t i = 0u; i < size; ++i)
    {
        const uint8_t byte = code[(start + i) & 0x3fffu];
        program.m_ir += '\\';
        program.m_ir += hex[byte >> 4u];
        program.m_ir += hex[byte & 15u];
    }
    program.m_ir += "\"\n";
    program.m_ir += "define " + linkage +
        "ptr @ee_vu1_aot_info() { ret ptr @vu1_metadata }\n";
    program.m_ir += "define " + linkage +
        "ptr @ee_vu1_aot_code() { ret ptr @vu1_code }\n";
    return program.m_ir;
}
} // namespace ee
