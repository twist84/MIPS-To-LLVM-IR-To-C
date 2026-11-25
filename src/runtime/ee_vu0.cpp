#include "ee_runtime.h"
#include "ee_vu_scalar_fp.h"
#include "ee_vu_fmac_fp.h"
#include <cmath>
#include <cstring>

namespace ee
{

e_ee_vu0_sync_mode ee_vu0_transfer_sync_mode(
    e_ee_vu0_transfer_direction direction,
    bool in_delay_slot
)
{
    if (!in_delay_slot)
    {
        return e_ee_vu0_sync_mode::CATCH_UP_TO_EE_CYCLE;
    }

    return direction == e_ee_vu0_transfer_direction::FROM_VU0
        ? e_ee_vu0_sync_mode::FINISH_TO_E
        : e_ee_vu0_sync_mode::WAIT_TO_E_OR_M;
}

extern "C" uint32_t ee_vu0_sync(
    s_ee_cpu_state* cpu,
    s_ee_runtime* rt,
    uint32_t mode,
    uint32_t pc
)
{
    if (!cpu || !rt ||
        mode > static_cast<uint32_t>(e_ee_vu0_sync_mode::WAIT_TO_E_OR_M))
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    if (!rt->m_vu0_state || !rt->m_vu0_sync)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE);
    }

    const uint32_t result = rt->m_vu0_sync(
        rt->m_vu0_user, cpu, rt->m_vu0_state, mode, pc
    );

    if (result > static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED))
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
    }

    return result;
}

namespace
{
uint32_t validate_vu0_callback_result(uint32_t result)
{
    return result <= static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED)
        ? result
        : static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
}

bool vu0_is_running(const s_ee_vu0_state* vu0)
{
    return (vu0->m_vpu_status & 1u) != 0;
}
} // namespace

uint32_t ee_vu0_scheduler_sync_callback(
    void* user,
    s_ee_cpu_state* cpu,
    s_ee_vu0_state* vu0,
    uint32_t mode,
    uint32_t pc
)
{
    if (!user || !cpu || !vu0 ||
        mode > static_cast<uint32_t>(e_ee_vu0_sync_mode::WAIT_TO_E_OR_M))
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    auto* scheduler = static_cast<s_ee_vu0_scheduler*>(user);
    if (!scheduler->m_execute)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE);
    }

    const auto sync_mode = static_cast<e_ee_vu0_sync_mode>(mode);
    const uint32_t stop_at_e = EE_VU0_EXECUTE_STOP_AT_E;
    const uint32_t exact_cycles = EE_VU0_EXECUTE_EXACT_CYCLES;
    const uint32_t max_run_calls = scheduler->m_max_run_calls != 0
        ? scheduler->m_max_run_calls : 65536u;

    if (!vu0_is_running(vu0))
    {
        vu0->m_running = 0;
        return static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
    }

    if (sync_mode == e_ee_vu0_sync_mode::CATCH_UP_TO_EE_CYCLE)
    {
        // vu0Sync() is catch-up-only: VU0 ahead of the EE means no work.
        if (vu0->m_ee_cycle < vu0->m_cycle)
        {
            return static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
        }

        const uint64_t cycle_budget = vu0->m_ee_cycle - vu0->m_cycle;
        const uint64_t previous_cycle = vu0->m_cycle;
        const uint32_t result = validate_vu0_callback_result(
            scheduler->m_execute(
                scheduler->m_user, cpu, vu0, cycle_budget,
                stop_at_e | exact_cycles, pc
            )
        );
        if (result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
        {
            return result;
        }

        vu0->m_running = vu0_is_running(vu0) ? 1u : 0u;
        if (vu0_is_running(vu0) && cycle_budget != 0 &&
            vu0->m_cycle <= previous_cycle)
        {
            return static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
        }
        return static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
    }

    // _vu0FinishMicro() and _vu0WaitMicro() both account the VU0 elapsed
    // cycles on the EE and then catch VU1 up. Require those host services
    // before executing anything so an absent service cannot cause a partial
    // synchronization followed by an apparent success.
    if (!scheduler->m_vu1_catch_up || !scheduler->m_schedule_event)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE);
    }

    const uint64_t start_vu0_cycle = vu0->m_cycle;
    const uint64_t start_ee_cycle = vu0->m_ee_cycle;
    const bool wait_for_m = sync_mode == e_ee_vu0_sync_mode::WAIT_TO_E_OR_M;
    if (wait_for_m && vu0->m_m_bit != 0 &&
        vu0->m_ee_cycle <= vu0->m_cycle)
    {
        // PCSX2's M-bit fast path catches the EE up to VU0 and returns before
        // the normal VU1 catch-up/event-scheduling epilogue.
        vu0->m_ee_cycle = vu0->m_cycle;
        vu0->m_running = vu0_is_running(vu0) ? 1u : 0u;
        return static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
    }

    const uint32_t execute_flags = stop_at_e |
        (wait_for_m ? EE_VU0_EXECUTE_STOP_AT_M : 0u);

    bool terminated = false;
    uint32_t run_calls = 0;
    while (run_calls < max_run_calls && vu0_is_running(vu0))
    {
        const uint64_t previous_cycle = vu0->m_cycle;
        const uint32_t result = validate_vu0_callback_result(
            scheduler->m_execute(
                scheduler->m_user, cpu, vu0, 0x7fffffffu,
                execute_flags, pc
            )
        );
        if (result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
        {
            return result;
        }

        ++run_calls;
        vu0->m_running = vu0_is_running(vu0) ? 1u : 0u;
        if (!vu0_is_running(vu0))
        {
            terminated = true;
            break;
        }

        if (wait_for_m &&
            (vu0->m_m_bit != 0 || vu0->m_ee_cycle <= vu0->m_cycle))
        {
            terminated = true;
            break;
        }

        // A host engine that reports success without advancing or stopping
        // would otherwise make a malformed VU program spin forever here.
        if (vu0->m_cycle <= previous_cycle)
        {
            return static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
        }
    }

    if (!terminated)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
    }

    // PCSX2's addCycles epilogue advances EE time by the amount of VU0
    // execution consumed in this call. Do not assign EE time to VU0 time:
    // VU0 may already have been ahead when the interlocked wait began.
    if (vu0->m_cycle < start_vu0_cycle)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
    }
    vu0->m_ee_cycle = start_ee_cycle + (vu0->m_cycle - start_vu0_cycle);

    const uint32_t vu1_result = validate_vu0_callback_result(
        scheduler->m_vu1_catch_up(scheduler->m_user, cpu, vu0, pc)
    );
    if (vu1_result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
    {
        return vu1_result;
    }

    if (vu0_is_running(vu0))
    {
        scheduler->m_schedule_event(
            scheduler->m_user, cpu, vu0, 4u, pc
        );
    }

    return static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
}

namespace
{
uint32_t read_le32(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

e_ee_vu0_micro_opcode decode_lower_secondary(uint32_t word)
{
    using op = e_ee_vu0_micro_opcode;
    const uint32_t secondary = word & 0x3fu;

    switch (secondary)
    {
    case 0x30: return op::IADD;
    case 0x31: return op::ISUB;
    case 0x32: return op::IADDI;
    case 0x34: return op::IAND;
    case 0x35: return op::IOR;
    default: break;
    }

    if (secondary < 0x3cu || secondary > 0x3fu)
    {
        return op::UNKNOWN;
    }

    const uint32_t table = secondary - 0x3cu;
    const uint32_t index = (word >> 6) & 0x1fu;

    switch (table)
    {
    case 0:
        switch (index)
        {
        case 12: return op::MOVE;
        case 13: return op::LQI;
        case 14: return op::DIV;
        case 15: return op::MTIR;
        case 16: return op::RNEXT;
        case 25: return op::MFP;
        case 26: return op::XTOP;
        case 27: return op::XGKICK;
        case 28: return op::ESADD;
        case 29: return op::EATAN_XY;
        case 30: return op::ESQRT;
        case 31: return op::ESIN;
        default: return op::UNKNOWN;
        }
    case 1:
        switch (index)
        {
        case 12: return op::MR32;
        case 13: return op::SQI;
        case 14: return op::SQRT;
        case 15: return op::MFIR;
        case 16: return op::RGET;
        case 26: return op::XITOP;
        case 28: return op::ERSADD;
        case 29: return op::EATAN_XZ;
        case 30: return op::ERSQRT;
        case 31: return op::EATAN;
        default: return op::UNKNOWN;
        }
    case 2:
        switch (index)
        {
        case 13: return op::LQD;
        case 14: return op::RSQRT;
        case 15: return op::ILWR;
        case 16: return op::RINIT;
        case 28: return op::ELENG;
        case 29: return op::ESUM;
        case 30: return op::ERCPR;
        case 31: return op::EEXP;
        default: return op::UNKNOWN;
        }
    case 3:
        switch (index)
        {
        case 13: return op::SQD;
        case 14: return op::WAITQ;
        case 15: return op::ISWR;
        case 16: return op::RXOR;
        case 28: return op::ERLENG;
        case 30: return op::WAITP;
        default: return op::UNKNOWN;
        }
    default:
        return op::UNKNOWN;
    }
}

e_ee_vu0_micro_opcode decode_lower_micro_opcode(uint32_t word)
{
    using op = e_ee_vu0_micro_opcode;
    const uint32_t primary = word >> 25;

    switch (primary)
    {
    case 0x00: return op::LQ;
    case 0x01: return op::SQ;
    case 0x04: return op::ILW;
    case 0x05: return op::ISW;
    case 0x08: return op::IADDIU;
    case 0x09: return op::ISUBIU;

    case 0x10: return op::FCEQ;
    case 0x11: return op::FCSET;
    case 0x12: return op::FCAND;
    case 0x13: return op::FCOR;
    case 0x14: return op::FSEQ;
    case 0x15: return op::FSSET;
    case 0x16: return op::FSAND;
    case 0x17: return op::FSOR;
    case 0x18: return op::FMEQ;
    case 0x1a: return op::FMAND;
    case 0x1b: return op::FMOR;
    case 0x1c: return op::FCGET;

    case 0x20: return op::B;
    case 0x21: return op::BAL;
    case 0x24: return op::JR;
    case 0x25: return op::JALR;
    case 0x28: return op::IBEQ;
    case 0x29: return op::IBNE;
    case 0x2c: return op::IBLTZ;
    case 0x2d: return op::IBGTZ;
    case 0x2e: return op::IBLEZ;
    case 0x2f: return op::IBGEZ;

    // Lower1 uses the top-seven-bit selector 0x40 (bit 31 set), then
    // dispatches the operation through the low six bits.
    case 0x40:
        return decode_lower_secondary(word);

    default:
        return op::UNKNOWN;
    }
}

e_ee_vu0_micro_opcode decode_upper_secondary(
    uint32_t word,
    uint32_t table
)
{
    using op = e_ee_vu0_micro_opcode;
    const uint32_t index = (word >> 6) & 0x1fu;

    switch (table)
    {
    case 0:
        switch (index)
        {
        case 0: return op::ADDA_X;
        case 1: return op::SUBA_X;
        case 2: return op::MADDA_X;
        case 3: return op::MSUBA_X;
        case 4: return op::ITOF0;
        case 5: return op::FTOI0;
        case 6: return op::MULA_X;
        case 7: return op::MULA_Q;
        case 8: return op::ADDA_Q;
        case 9: return op::SUBA_Q;
        case 10: return op::ADDA;
        case 11: return op::SUBA;
        default: return op::UNKNOWN;
        }
    case 1:
        switch (index)
        {
        case 0: return op::ADDA_Y;
        case 1: return op::SUBA_Y;
        case 2: return op::MADDA_Y;
        case 3: return op::MSUBA_Y;
        case 4: return op::ITOF4;
        case 5: return op::FTOI4;
        case 6: return op::MULA_Y;
        case 7: return op::ABS;
        case 8: return op::MADDA_Q;
        case 9: return op::MSUBA_Q;
        case 10: return op::MADDA;
        case 11: return op::MSUBA;
        default: return op::UNKNOWN;
        }
    case 2:
        switch (index)
        {
        case 0: return op::ADDA_Z;
        case 1: return op::SUBA_Z;
        case 2: return op::MADDA_Z;
        case 3: return op::MSUBA_Z;
        case 4: return op::ITOF12;
        case 5: return op::FTOI12;
        case 6: return op::MULA_Z;
        case 7: return op::MULA_I;
        case 8: return op::ADDA_I;
        case 9: return op::SUBA_I;
        case 10: return op::MULA;
        case 11: return op::OPMULA;
        default: return op::UNKNOWN;
        }
    case 3:
        switch (index)
        {
        case 0: return op::ADDA_W;
        case 1: return op::SUBA_W;
        case 2: return op::MADDA_W;
        case 3: return op::MSUBA_W;
        case 4: return op::ITOF15;
        case 5: return op::FTOI15;
        case 6: return op::MULA_W;
        case 7: return op::CLIP;
        case 8: return op::MADDA_I;
        case 9: return op::MSUBA_I;
        case 11: return op::NOP;
        default: return op::UNKNOWN;
        }
    default:
        return op::UNKNOWN;
    }
}

e_ee_vu0_micro_opcode decode_upper_micro_opcode(uint32_t word)
{
    using op = e_ee_vu0_micro_opcode;
    const uint32_t primary = word & 0x3fu;

    if (primary >= 0x3cu)
    {
        return decode_upper_secondary(word, primary - 0x3cu);
    }

    switch (primary)
    {
    case 0x00: return op::ADD_X;
    case 0x01: return op::ADD_Y;
    case 0x02: return op::ADD_Z;
    case 0x03: return op::ADD_W;
    case 0x04: return op::SUB_X;
    case 0x05: return op::SUB_Y;
    case 0x06: return op::SUB_Z;
    case 0x07: return op::SUB_W;
    case 0x08: return op::MADD_X;
    case 0x09: return op::MADD_Y;
    case 0x0a: return op::MADD_Z;
    case 0x0b: return op::MADD_W;
    case 0x0c: return op::MSUB_X;
    case 0x0d: return op::MSUB_Y;
    case 0x0e: return op::MSUB_Z;
    case 0x0f: return op::MSUB_W;
    case 0x10: return op::MAX_X;
    case 0x11: return op::MAX_Y;
    case 0x12: return op::MAX_Z;
    case 0x13: return op::MAX_W;
    case 0x14: return op::MINI_X;
    case 0x15: return op::MINI_Y;
    case 0x16: return op::MINI_Z;
    case 0x17: return op::MINI_W;
    case 0x18: return op::MUL_X;
    case 0x19: return op::MUL_Y;
    case 0x1a: return op::MUL_Z;
    case 0x1b: return op::MUL_W;
    case 0x1c: return op::MUL_Q;
    case 0x1d: return op::MAX_I;
    case 0x1e: return op::MUL_I;
    case 0x1f: return op::MINI_I;
    case 0x20: return op::ADD_Q;
    case 0x21: return op::MADD_Q;
    case 0x22: return op::ADD_I;
    case 0x23: return op::MADD_I;
    case 0x24: return op::SUB_Q;
    case 0x25: return op::MSUB_Q;
    case 0x26: return op::SUB_I;
    case 0x27: return op::MSUB_I;
    case 0x28: return op::ADD;
    case 0x29: return op::MADD;
    case 0x2a: return op::MUL;
    case 0x2b: return op::MAX;
    case 0x2c: return op::SUB;
    case 0x2d: return op::MSUB;
    case 0x2e: return op::OPMSUB;
    case 0x2f: return op::MINI;
    default: return op::UNKNOWN;
    }
}

s_ee_vu0_micro_op_fields decode_micro_op_fields(
    uint32_t word,
    bool upper
)
{
    const uint8_t ft = static_cast<uint8_t>((word >> 16) & 0x1fu);
    const uint8_t fs = static_cast<uint8_t>((word >> 11) & 0x1fu);
    const uint8_t fd = static_cast<uint8_t>((word >> 6) & 0x1fu);

    return {
        static_cast<uint8_t>(upper ? (word & 0x3fu) : (word >> 25)),
        ft,
        fs,
        fd,
        static_cast<uint8_t>(ft & 0x0fu),
        static_cast<uint8_t>(fs & 0x0fu),
        static_cast<uint8_t>(fd & 0x0fu),
        static_cast<uint8_t>((word >> 21) & 0x0fu),
        upper ? decode_upper_micro_opcode(word) : decode_lower_micro_opcode(word)
    };
}} // namespace

extern "C" uint32_t ee_vu0_fetch_micro_instruction(
    s_ee_vu0_state* vu0,
    s_ee_vu0_micro_instruction* out
)
{
    if (!vu0 || !out)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    const uint32_t pc = vu0->m_micro_pc & EE_VU0_MICRO_MEMORY_MASK;
    if ((pc & 7u) != 0)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    const uint8_t* const bytes = &vu0->m_micro_memory[pc];
    const uint32_t lower = read_le32(bytes);
    const uint32_t upper = read_le32(bytes + 4);

    uint32_t flags = 0;
    if ((upper & 0x80000000u) != 0) flags |= EE_VU0_MICRO_FLAG_I;
    if ((upper & 0x40000000u) != 0) flags |= EE_VU0_MICRO_FLAG_E;
    if ((upper & 0x20000000u) != 0) flags |= EE_VU0_MICRO_FLAG_M;
    if ((upper & 0x10000000u) != 0) flags |= EE_VU0_MICRO_FLAG_D;
    if ((upper & 0x08000000u) != 0) flags |= EE_VU0_MICRO_FLAG_T;

    const s_ee_vu0_micro_op_fields lower_fields =
        decode_micro_op_fields(lower, false);
    const s_ee_vu0_micro_op_fields upper_fields =
        decode_micro_op_fields(upper, true);

    *out = {
        pc, lower, upper, flags, lower_fields, upper_fields
    };
    vu0->m_micro_pc = (pc + 8u) & EE_VU0_MICRO_MEMORY_MASK;
    return static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
}

namespace
{
bool ialu_completion_cycle(
    const s_ee_vu0_state::s_ialu_pipe_entry& entry,
    uint64_t* completion_cycle
)
{
    if (!completion_cycle ||
        entry.m_start_cycle > UINT64_MAX - entry.m_latency)
    {
        return false;
    }

    *completion_cycle = entry.m_start_cycle + entry.m_latency;
    return true;
}

void ialu_pop_front(s_ee_vu0_state* vu0)
{
    vu0->m_ialu[vu0->m_ialu_read_pos] = {};
    vu0->m_ialu_read_pos = (vu0->m_ialu_read_pos + 1u) & 3u;
    --vu0->m_ialu_count;
}
} // namespace

extern "C" uint32_t ee_vu0_ialu_schedule_write(
    s_ee_vu0_state* vu0,
    uint32_t register_mask,
    uint32_t latency
)
{
    if (!vu0)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    if (register_mask == 0 || latency == 0)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
    }

    if (vu0->m_ialu_count >= 4 ||
        vu0->m_ialu_read_pos >= 4 ||
        vu0->m_ialu_write_pos >= 4)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
    }

    auto& entry = vu0->m_ialu[vu0->m_ialu_write_pos];
    entry.m_register_mask = register_mask;
    entry.m_latency = latency;
    entry.m_start_cycle = vu0->m_cycle;

    vu0->m_ialu_write_pos = (vu0->m_ialu_write_pos + 1u) & 3u;
    ++vu0->m_ialu_count;
    return static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
}

extern "C" uint32_t ee_vu0_ialu_retire_matured(
    s_ee_vu0_state* vu0,
    uint32_t* retired_count
)
{
    if (!vu0 || !retired_count)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    if (vu0->m_ialu_count > 4 ||
        vu0->m_ialu_read_pos >= 4 ||
        vu0->m_ialu_write_pos >= 4)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
    }

    uint32_t retired = 0;
    while (vu0->m_ialu_count != 0)
    {
        const auto& entry = vu0->m_ialu[vu0->m_ialu_read_pos];
        uint64_t completion_cycle = 0;
        if (!ialu_completion_cycle(entry, &completion_cycle))
        {
            return static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
        }

        if (vu0->m_cycle < completion_cycle)
        {
            break;
        }

        ialu_pop_front(vu0);
        ++retired;
    }

    *retired_count = retired;
    return static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
}

extern "C" uint32_t ee_vu0_ialu_wait_for_reads(
    s_ee_vu0_state* vu0,
    uint32_t read_mask
)
{
    if (!vu0)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    if (vu0->m_ialu_count > 4 ||
        vu0->m_ialu_read_pos >= 4 ||
        vu0->m_ialu_write_pos >= 4)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
    }

    if (read_mask == 0)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
    }

    uint64_t stall_until = vu0->m_cycle;
    uint32_t index = vu0->m_ialu_read_pos;
    for (uint32_t i = 0; i < vu0->m_ialu_count; ++i)
    {
        const auto& entry = vu0->m_ialu[index];
        if ((entry.m_register_mask & read_mask) != 0)
        {
            uint64_t completion_cycle = 0;
            if (!ialu_completion_cycle(entry, &completion_cycle))
            {
                return static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
            }

            if (completion_cycle > stall_until)
            {
                stall_until = completion_cycle;
            }
        }

        index = (index + 1u) & 3u;
    }

    vu0->m_cycle = stall_until;
    return static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
}

extern "C" uint32_t ee_vu0_ialu_flush_all(s_ee_vu0_state* vu0)
{
    if (!vu0)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    if (vu0->m_ialu_count > 4 ||
        vu0->m_ialu_read_pos >= 4 ||
        vu0->m_ialu_write_pos >= 4)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
    }

    while (vu0->m_ialu_count != 0)
    {
        const auto& entry = vu0->m_ialu[vu0->m_ialu_read_pos];
        uint64_t completion_cycle = 0;
        if (!ialu_completion_cycle(entry, &completion_cycle))
        {
            return static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
        }

        if (completion_cycle > vu0->m_cycle)
        {
            vu0->m_cycle = completion_cycle;
        }

        ialu_pop_front(vu0);
    }

    return static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
}

namespace
{
uint16_t vu0_read_ialu_vi(const s_ee_vu0_state* vu0, uint32_t index)
{
    if (index == 0 || index >= 16)
    {
        return 0;
    }
    return static_cast<uint16_t>(vu0->m_vi[index]);
}

uint16_t vu0_read_branch_vi(const s_ee_vu0_state* vu0, uint32_t index)
{
    if (index == 0 || index >= 16)
    {
        return 0;
    }

    if (vu0->m_vi_backup_cycles != 0 &&
        vu0->m_vi_backup_register == index)
    {
        return static_cast<uint16_t>(vu0->m_vi_backup_old_value);
    }

    return vu0_read_ialu_vi(vu0, index);
}

void vu0_backup_ialu_vi(s_ee_vu0_state* vu0, uint32_t index)
{
    if (index == 0 || index >= 16)
    {
        return;
    }

    // Consecutive writes to the same VI preserve the value from before the
    // chain of writes, while refreshing the two-cycle visibility window.
    if (vu0->m_vi_backup_cycles != 0 &&
        vu0->m_vi_backup_register == index)
    {
        vu0->m_vi_backup_cycles = 2;
        return;
    }

    vu0->m_vi_backup_cycles = 2;
    vu0->m_vi_backup_register = index;
    vu0->m_vi_backup_old_value = static_cast<uint16_t>(vu0->m_vi[index]);
}

void vu0_advance_vi_backup(s_ee_vu0_state* vu0, uint64_t elapsed_cycles)
{
    if (vu0->m_vi_backup_cycles == 0)
    {
        return;
    }

    if (elapsed_cycles >= vu0->m_vi_backup_cycles)
    {
        vu0->m_vi_backup_cycles = 0;
    }
    else
    {
        vu0->m_vi_backup_cycles -= static_cast<uint32_t>(elapsed_cycles);
    }
}

void vu0_write_ialu_vi(s_ee_vu0_state* vu0, uint32_t index, uint16_t value)
{
    if (index == 0 || index >= 16)
    {
        return;
    }

    // The integer ALU writes only VI[reg].SS[0]; preserve the neighbouring
    // signed halfword and the rest of the sidecar word.
    vu0->m_vi[index] = (vu0->m_vi[index] & 0xffff0000u) | value;
}

bool vu0_is_supported_lower_ialu(const s_ee_vu0_micro_instruction& instruction)
{
    using op = e_ee_vu0_micro_opcode;
    switch (instruction.m_lower_fields.m_operation)
    {
    case op::IADD:
    case op::ISUB:
    case op::IAND:
    case op::IOR:
    case op::IADDI:
    case op::IADDIU:
    case op::ISUBIU:
        return true;
    default:
        return false;
    }
}

bool vu0_is_supported_lower_flag_read(const s_ee_vu0_micro_instruction& instruction)
{
    using op = e_ee_vu0_micro_opcode;
    switch (instruction.m_lower_fields.m_operation)
    {
    case op::FCEQ:
    case op::FCAND:
    case op::FCOR:
    case op::FSEQ:
    case op::FSAND:
    case op::FSOR:
    case op::FMEQ:
    case op::FMAND:
    case op::FMOR:
    case op::FCGET:
        return true;
    default:
        return false;
    }
}

void vu0_execute_lower_flag_read(
    s_ee_vu0_state* vu0, const s_ee_vu0_micro_instruction& instruction)
{
    using op = e_ee_vu0_micro_opcode;
    const auto& fields = instruction.m_lower_fields;
    const uint32_t clip = vu0->m_clip_flags & 0xffffffu;
    const uint32_t status = vu0->m_status_flags & 0xfffu;
    const uint32_t mac = vu0->m_mac_flags & 0xffffu;
    const uint32_t source = vu0_read_ialu_vi(vu0, fields.m_is);
    const uint32_t imm24 = instruction.m_lower & 0xffffffu;
    const uint32_t imm12 = (instruction.m_lower & 0x7ffu) |
        ((instruction.m_lower >> 10u) & 0x800u);
    uint32_t destination = fields.m_it;
    uint32_t value = 0;
    switch (fields.m_operation)
    {
    case op::FCEQ: destination = 1; value = clip == imm24; break;
    case op::FCAND: destination = 1; value = (clip & imm24) != 0; break;
    case op::FCOR: destination = 1; value = (clip | imm24) == 0xffffffu; break;
    case op::FSEQ: value = status == imm12; break;
    case op::FSAND: value = status & imm12; break;
    case op::FSOR: value = status | imm12; break;
    case op::FMEQ: value = mac == source; break;
    case op::FMAND: value = mac & source; break;
    case op::FMOR: value = mac | source; break;
    case op::FCGET: value = clip & 0xfffu; break;
    default: return;
    }
    vu0_write_ialu_vi(vu0, destination, static_cast<uint16_t>(value));
}

void vu0_record_lower_fmac(s_ee_vu0_state* vu0, uint32_t reg, uint32_t lanes)
{
    const uint64_t ready = vu0->m_cycle + 4u;
    vu0->m_vf_memory_ready_cycle = ready;
    auto& issue = vu0->m_vf_memory_issues[vu0->m_vf_memory_issue_index & 3u];
    ++vu0->m_vf_memory_issue_index;
    issue.m_ready_cycle = ready;
    issue.m_register = reg;
    issue.m_lane_mask = reg != 0 ? lanes : 0u;
}

bool vu0_is_supported_lower_memory(const s_ee_vu0_micro_instruction& instruction)
{
    using op = e_ee_vu0_micro_opcode;
    switch (instruction.m_lower_fields.m_operation)
    {
    case op::ILW:
    case op::ISW:
    case op::LQ:
    case op::SQ:
    case op::LQI:
    case op::SQI:
    case op::LQD:
    case op::SQD:
        return true;
    default:
        return false;
    }
}

int32_t vu0_signed_memory_immediate(uint32_t word)
{
    const uint32_t value = word & 0x7ffu;
    return (value & 0x400u) != 0
        ? static_cast<int32_t>(value) - 0x800
        : static_cast<int32_t>(value);
}

uint32_t vu0_memory_byte_address(
    const s_ee_vu0_state* vu0,
    const s_ee_vu0_micro_instruction& instruction
)
{
    // PCSX2 computes the quadword address through a 16-bit intermediate.
    using op = e_ee_vu0_micro_opcode;
    const op operation = instruction.m_lower_fields.m_operation;
    const bool indexed = operation == op::LQI || operation == op::SQI ||
                         operation == op::LQD || operation == op::SQD;
    const bool store = operation == op::SQ || operation == op::SQI ||
                       operation == op::SQD;
    const uint32_t base_index = store
        ? instruction.m_lower_fields.m_it
        : instruction.m_lower_fields.m_is;
    const int32_t base = static_cast<int16_t>(vu0_read_ialu_vi(vu0, base_index));
    const int32_t offset = indexed
        ? (operation == op::LQD || operation == op::SQD ? -1 : 0)
        : vu0_signed_memory_immediate(instruction.m_lower);
    return (static_cast<uint32_t>((base + offset) * 16) & 0xffffu);
}

bool vu0_execute_lower_memory(
    s_ee_cpu_state* cpu,
    s_ee_vu0_state* vu0,
    const s_ee_vu0_micro_instruction& instruction,
    uint32_t address,
    uint32_t* write_mask
)
{
    const auto& fields = instruction.m_lower_fields;
    *write_mask = 0;
    const uint32_t xyzw = fields.m_xyzw;
    const uint32_t base = address & (EE_VU0_DATA_MEMORY_SIZE - 1u);
    using op = e_ee_vu0_micro_opcode;
    const op operation = fields.m_operation;
    const bool indexed_load = operation == op::LQI || operation == op::LQD;
    const bool indexed_store = operation == op::SQI || operation == op::SQD;
    const bool predecrement = operation == op::LQD || operation == op::SQD;
    const bool postincrement = operation == op::LQI || operation == op::SQI;
    if (operation == op::LQ || operation == op::SQ ||
        indexed_load || indexed_store)
    {
        // VF registers live in the shared EE COP2 bank. Read and write
        // individual 32-bit lanes without assuming host endianness.
        const bool load = operation == op::LQ || indexed_load;
        const uint32_t register_index = load ? fields.m_ft : fields.m_fs;
        const uint32_t base_index = load ? fields.m_is : fields.m_it;
        if (predecrement || postincrement)
        {
            // _vuBackupVI is issued for indexed loads/stores even if the
            // destination is VF0 or no vector lanes are selected.
            if (base_index != 0)
            {
                vu0_backup_ialu_vi(vu0, base_index);
                if (predecrement)
                {
                    vu0_write_ialu_vi(
                        vu0, base_index,
                        static_cast<uint16_t>(
                            vu0_read_ialu_vi(vu0, base_index) - 1u
                        )
                    );
                }
            }
        }
        if (!load || register_index != 0)
        {
            s_ee_u128& vector = cpu->m_vu0[register_index];
            for (uint32_t component = 0; component < 4; ++component)
            {
                if ((xyzw & (8u >> component)) == 0)
                {
                    continue;
                }
                const uint32_t offset = base + component * 4u;
                uint64_t& half = component < 2 ? vector.m_lo : vector.m_hi;
                const uint32_t shift = (component & 1u) * 32u;
                if (load)
                {
                    const uint32_t value =
                        static_cast<uint32_t>(vu0->m_data_memory[offset]) |
                        (static_cast<uint32_t>(vu0->m_data_memory[offset + 1u]) << 8) |
                        (static_cast<uint32_t>(vu0->m_data_memory[offset + 2u]) << 16) |
                        (static_cast<uint32_t>(vu0->m_data_memory[offset + 3u]) << 24);
                    half = (half & ~(uint64_t{0xffffffffu} << shift)) |
                           (static_cast<uint64_t>(value) << shift);
                }
                else
                {
                    const uint32_t value = static_cast<uint32_t>(half >> shift);
                    for (uint32_t byte = 0; byte < 4; ++byte)
                    {
                        vu0->m_data_memory[offset + byte] =
                            static_cast<uint8_t>(value >> (byte * 8u));
                    }
                }
            }
        }
        if (postincrement && base_index != 0)
        {
            vu0_write_ialu_vi(
                vu0, base_index,
                static_cast<uint16_t>(vu0_read_ialu_vi(vu0, base_index) + 1u)
            );
        }
        return true;
    }

    if (fields.m_operation == e_ee_vu0_micro_opcode::ILW)
    {
        // For multiple selected components, PCSX2 overwrites VI in X,Y,Z,W
        // order, leaving the last selected component as the final value.
        if (fields.m_it == 0)
        {
            return true;
        }
        uint16_t loaded = static_cast<uint16_t>(vu0->m_vi[fields.m_it]);
        for (uint32_t component = 0; component < 4; ++component)
        {
            if ((xyzw & (8u >> component)) == 0)
            {
                continue;
            }
            const uint32_t offset = base + component * 4u;
            loaded = static_cast<uint16_t>(
                vu0->m_data_memory[offset] |
                (static_cast<uint16_t>(vu0->m_data_memory[offset + 1u]) << 8)
            );
        }
        if (xyzw != 0)
        {
            vu0_write_ialu_vi(vu0, fields.m_it, loaded);
            *write_mask = 1u << fields.m_it;
        }
        return true;
    }

    if (fields.m_operation == e_ee_vu0_micro_opcode::ISW)
    {
        const uint16_t value = vu0_read_ialu_vi(vu0, fields.m_it);
        for (uint32_t component = 0; component < 4; ++component)
        {
            if ((xyzw & (8u >> component)) == 0)
            {
                continue;
            }
            const uint32_t offset = base + component * 4u;
            vu0->m_data_memory[offset] = static_cast<uint8_t>(value);
            vu0->m_data_memory[offset + 1u] = static_cast<uint8_t>(value >> 8);
            vu0->m_data_memory[offset + 2u] = 0;
            vu0->m_data_memory[offset + 3u] = 0;
        }
        return true;
    }
    return false;
}

bool vu0_is_supported_lower_branch(const s_ee_vu0_micro_instruction& instruction)
{
    using op = e_ee_vu0_micro_opcode;
    switch (instruction.m_lower_fields.m_operation)
    {
    case op::B:
    case op::BAL:
    case op::JR:
    case op::JALR:
    case op::IBEQ:
    case op::IBNE:
    case op::IBLTZ:
    case op::IBGTZ:
    case op::IBLEZ:
    case op::IBGEZ:
        return true;
    default:
        return false;
    }
}

uint32_t vu0_lower_branch_read_mask(
    const s_ee_vu0_micro_instruction& instruction
)
{
    using op = e_ee_vu0_micro_opcode;
    const auto& fields = instruction.m_lower_fields;
    uint32_t read_mask = 0;

    switch (fields.m_operation)
    {
    case op::JR:
    case op::JALR:
        if (fields.m_is != 0) read_mask |= 1u << fields.m_is;
        break;

    case op::IBEQ:
    case op::IBNE:
        if (fields.m_it != 0) read_mask |= 1u << fields.m_it;
        if (fields.m_is != 0) read_mask |= 1u << fields.m_is;
        break;

    case op::IBLTZ:
    case op::IBGTZ:
    case op::IBLEZ:
    case op::IBGEZ:
        if (fields.m_is != 0) read_mask |= 1u << fields.m_is;
        break;

    default:
        break;
    }

    return read_mask;
}

int32_t vu0_sign_extend_branch_immediate(uint32_t lower_word)
{
    const uint32_t immediate = lower_word & 0x07ffu;
    return (immediate & 0x0400u) != 0
        ? static_cast<int32_t>(immediate | 0xfffff800u)
        : static_cast<int32_t>(immediate);
}

uint32_t vu0_relative_branch_target(
    const s_ee_vu0_state* vu0,
    const s_ee_vu0_micro_instruction& instruction
)
{
    // PCSX2's _branchAddr uses the already-advanced TPC plus signed imm10
    // (the sign bit is instruction bit 10; bit 11 is not part of the offset).
    const int64_t target = static_cast<int64_t>(vu0->m_micro_pc) +
        (static_cast<int64_t>(vu0_sign_extend_branch_immediate(instruction.m_lower)) * 8);
    return static_cast<uint32_t>(target) & EE_VU0_MICRO_MEMORY_MASK;
}

void vu0_set_branch(s_ee_vu0_state* vu0, uint32_t target)
{
    if (vu0->m_branch == 1)
    {
        // A branch issued in the current branch's delay pair is deferred until
        // after the first target instruction has executed.
        vu0->m_delay_branch_pc = target & EE_VU0_MICRO_MEMORY_MASK;
        vu0->m_take_delay_branch = 1;
    }
    else
    {
        vu0->m_branch = 2;
        vu0->m_branch_pc = target & EE_VU0_MICRO_MEMORY_MASK;
    }
}

void vu0_advance_branch_pipeline(s_ee_vu0_state* vu0)
{
    if (vu0->m_branch == 0)
    {
        return;
    }

    if (vu0->m_branch > 1)
    {
        --vu0->m_branch;
        return;
    }

    vu0->m_micro_pc = vu0->m_branch_pc & EE_VU0_MICRO_MEMORY_MASK;
    if (vu0->m_take_delay_branch != 0)
    {
        vu0->m_branch = 1;
        vu0->m_branch_pc = vu0->m_delay_branch_pc & EE_VU0_MICRO_MEMORY_MASK;
        vu0->m_take_delay_branch = 0;
    }
    else
    {
        vu0->m_branch = 0;
    }
}

struct s_vu0_branch_action
{
    bool m_taken = false;
    uint32_t m_target = 0;
    uint32_t m_link_register = 0;
    uint16_t m_link_value = 0;
};

bool vu0_prepare_lower_branch(
    const s_ee_vu0_state* vu0,
    const s_ee_vu0_micro_instruction& instruction,
    s_vu0_branch_action* action
)
{
    if (!vu0 || !action)
    {
        return false;
    }

    *action = {};
    using op = e_ee_vu0_micro_opcode;
    const auto& fields = instruction.m_lower_fields;
    const int16_t source_s = static_cast<int16_t>(
        vu0_read_branch_vi(vu0, fields.m_is)
    );
    const int16_t source_t = static_cast<int16_t>(
        vu0_read_branch_vi(vu0, fields.m_it)
    );
    bool absolute_target = false;
    uint32_t target = 0;

    switch (fields.m_operation)
    {
    case op::B:
        action->m_taken = true;
        break;

    case op::BAL:
        action->m_taken = true;
        if (fields.m_it != 0)
        {
            const uint32_t link_pc = vu0->m_branch == 1
                ? (vu0->m_branch_pc + 8u)
                : (vu0->m_micro_pc + 8u);
            action->m_link_register = fields.m_it;
            action->m_link_value =
                static_cast<uint16_t>((link_pc >> 3) & 0xffffu);
        }
        break;

    case op::JR:
        action->m_taken = true;
        absolute_target = true;
        target = static_cast<uint32_t>(
            vu0_read_ialu_vi(vu0, fields.m_is)
        ) << 3;
        break;

    case op::JALR:
        action->m_taken = true;
        absolute_target = true;
        // Sample the jump target before the link write. This also handles the
        // legal Is == It overlap without corrupting the sampled target.
        target = static_cast<uint32_t>(
            vu0_read_ialu_vi(vu0, fields.m_is)
        ) << 3;
        if (fields.m_it != 0)
        {
            const uint32_t link_pc = vu0->m_branch == 1
                ? (vu0->m_branch_pc + 8u)
                : (vu0->m_micro_pc + 8u);
            action->m_link_register = fields.m_it;
            action->m_link_value =
                static_cast<uint16_t>((link_pc >> 3) & 0xffffu);
        }
        break;

    case op::IBEQ:
        action->m_taken = source_t == source_s;
        break;

    case op::IBNE:
        action->m_taken = source_t != source_s;
        break;

    case op::IBLTZ:
        action->m_taken = source_s < 0;
        break;

    case op::IBGTZ:
        action->m_taken = source_s > 0;
        break;

    case op::IBLEZ:
        action->m_taken = source_s <= 0;
        break;

    case op::IBGEZ:
        action->m_taken = source_s >= 0;
        break;

    default:
        return false;
    }

    if (action->m_taken)
    {
        action->m_target = absolute_target
            ? (target & EE_VU0_MICRO_MEMORY_MASK)
            : vu0_relative_branch_target(vu0, instruction);
    }

    return true;
}

void vu0_apply_lower_branch(
    s_ee_vu0_state* vu0,
    const s_vu0_branch_action& action
)
{
    if (action.m_link_register != 0)
    {
        vu0_write_ialu_vi(
            vu0, action.m_link_register, action.m_link_value
        );
    }

    if (action.m_taken)
    {
        vu0_set_branch(vu0, action.m_target);
    }
}

bool vu0_execute_lower_ialu(
    s_ee_vu0_state* vu0,
    const s_ee_vu0_micro_instruction& instruction,
    uint32_t* write_mask
)
{
    if (!vu0 || !write_mask)
    {
        return false;
    }

    *write_mask = 0;
    using op = e_ee_vu0_micro_opcode;
    const s_ee_vu0_micro_op_fields& fields = instruction.m_lower_fields;
    const uint16_t source_s = vu0_read_ialu_vi(vu0, fields.m_is);
    const uint16_t source_t = vu0_read_ialu_vi(vu0, fields.m_it);
    uint16_t result = 0;
    uint32_t destination = 0;

    switch (fields.m_operation)
    {
    case op::IADD:
        destination = fields.m_id;
        result = static_cast<uint16_t>(
            static_cast<int16_t>(source_s) + static_cast<int16_t>(source_t)
        );
        break;

    case op::ISUB:
        destination = fields.m_id;
        result = static_cast<uint16_t>(
            static_cast<int16_t>(source_s) - static_cast<int16_t>(source_t)
        );
        break;

    case op::IAND:
        destination = fields.m_id;
        result = static_cast<uint16_t>(source_s & source_t);
        break;

    case op::IOR:
        destination = fields.m_id;
        result = static_cast<uint16_t>(source_s | source_t);
        break;

    case op::IADDI:
    {
        destination = fields.m_it;
        int32_t immediate = static_cast<int32_t>((instruction.m_lower >> 6) & 0x1fu);
        if ((immediate & 0x10) != 0)
        {
            immediate -= 0x20;
        }
        result = static_cast<uint16_t>(
            static_cast<int16_t>(source_s) + immediate
        );
        break;
    }

    case op::IADDIU:
    {
        destination = fields.m_it;
        const uint32_t immediate =
            ((instruction.m_lower >> 10) & 0x7800u) |
            (instruction.m_lower & 0x07ffu);
        result = static_cast<uint16_t>(
            static_cast<int16_t>(source_s) + static_cast<int32_t>(immediate)
        );
        break;
    }

    case op::ISUBIU:
    {
        destination = fields.m_it;
        const uint32_t immediate =
            ((instruction.m_lower >> 10) & 0x7800u) |
            (instruction.m_lower & 0x07ffu);
        result = static_cast<uint16_t>(
            static_cast<int16_t>(source_s) - static_cast<int32_t>(immediate)
        );
        break;
    }

    default:
        return false;
    }

    if (destination != 0)
    {
        vu0_backup_ialu_vi(vu0, destination);
    }
    vu0_write_ialu_vi(vu0, destination, result);
    if (destination != 0)
    {
        *write_mask = 1u << destination;
    }
    return true;
}
} // namespace

uint32_t ee_vu0_execute_ialu_callback(
    void* user,
    s_ee_cpu_state* cpu,
    s_ee_vu0_state* vu0,
    uint64_t max_cycles,
    uint32_t flags,
    uint32_t
)
{
    if (!user || !cpu || !vu0 ||
        (flags & ~(EE_VU0_EXECUTE_STOP_AT_E |
                   EE_VU0_EXECUTE_STOP_AT_M |
                   EE_VU0_EXECUTE_EXACT_CYCLES)) != 0)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    auto* engine = static_cast<s_ee_vu0_ialu_engine*>(user);
    const uint32_t instruction_limit = engine->m_max_instructions_per_call != 0
        ? engine->m_max_instructions_per_call : 65536u;

    if ((vu0->m_vpu_status & 1u) == 0)
    {
        vu0->m_running = 0;
        return static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
    }

    // PCSX2 clears the per-run M-bit marker whenever it enters the VU engine.
    vu0->m_m_bit = 0;

    const uint64_t slice_start_cycle = vu0->m_cycle;
    uint64_t executed = 0;
    while ((vu0->m_cycle - slice_start_cycle) < max_cycles &&
           executed < instruction_limit &&
           (vu0->m_vpu_status & 1u) != 0)
    {
        if (vu0->m_cycle == UINT64_MAX)
        {
            return static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
        }

        const uint32_t pair_pc = vu0->m_micro_pc;
        s_ee_vu0_micro_instruction instruction{};
        const uint32_t fetch_result =
            ee_vu0_fetch_micro_instruction(vu0, &instruction);
        if (fetch_result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
        {
            return fetch_result;
        }

        const bool lower_ialu = vu0_is_supported_lower_ialu(instruction);
        const bool lower_branch = vu0_is_supported_lower_branch(instruction);
        const bool lower_flag_read = vu0_is_supported_lower_flag_read(instruction);
        // Canonical lower NOP is MOVE to VF0 with no enabled lanes.
        const bool lower_nop = instruction.m_lower == 0x8000033cu;
        const bool lower_memory = vu0_is_supported_lower_memory(instruction);
        const uint32_t memory_address = lower_memory
            ? vu0_memory_byte_address(vu0, instruction) : 0;

        // Only ordinary 4 KB VU0 data memory is supported. PCSX2 maps
        // addresses with bit 14 set into VU1 registers; those must be handled
        // by the host rather than aliased into the ordinary VU0 data array.
        // I/D/T flags and non-NOP upper operations remain unsupported.
        if ((instruction.m_flags &
             (EE_VU0_MICRO_FLAG_I | EE_VU0_MICRO_FLAG_D | EE_VU0_MICRO_FLAG_T)) != 0 ||
            instruction.m_upper_fields.m_operation != e_ee_vu0_micro_opcode::NOP ||
            (!lower_ialu && !lower_branch && !lower_memory && !lower_flag_read && !lower_nop) ||
            (lower_memory && (memory_address & 0x4000u) != 0))
        {
            vu0->m_micro_pc = pair_pc;
            return static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE);
        }

        // PCSX2 resolves lower-op metadata first, then advances the issue
        // cycle, applies lower-pipe stalls, retires matured pipes, ages the
        // VI-backup window, and only then executes the lower operation.
        // Branch predicates, jump targets, and links must observe that
        // post-stall state rather than values sampled before the interlock.
        const uint64_t cycles_before_pair = vu0->m_cycle;
        ++vu0->m_cycle;
        // PCSX2 tests lower FMAC read dependencies before pipe retirement.
        // SQ reads its FS vector, and stalls only when an earlier LQ
        // destination matches both the register and an enabled lane.
        if (lower_memory &&
            (instruction.m_lower_fields.m_operation ==
                 e_ee_vu0_micro_opcode::SQ ||
             instruction.m_lower_fields.m_operation ==
                 e_ee_vu0_micro_opcode::SQI ||
             instruction.m_lower_fields.m_operation ==
                 e_ee_vu0_micro_opcode::SQD))
        {
            const uint32_t source = instruction.m_lower_fields.m_fs;
            const uint32_t lanes = instruction.m_lower_fields.m_xyzw;
            if (source != 0 && lanes != 0)
            {
                for (const auto& issue : vu0->m_vf_memory_issues)
                {
                    if (issue.m_register == source &&
                        (issue.m_lane_mask & lanes) != 0 &&
                        issue.m_ready_cycle > vu0->m_cycle)
                    {
                        vu0->m_cycle = issue.m_ready_cycle;
                    }
                }
            }
        }
        // PCSX2 only tests IALU read dependencies for VUPIPE_BRANCH.
        // ILW/ISW are VUPIPE_IALU: loads reserve their destination for
        // four cycles, but an adjacent store does not stall on that pipe.
        if (lower_branch)
        {
            const uint32_t wait_result = ee_vu0_ialu_wait_for_reads(
                vu0, vu0_lower_branch_read_mask(instruction)
            );
            if (wait_result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
            {
                vu0->m_micro_pc = pair_pc;
                return wait_result;
            }
        }

        uint32_t retired_count = 0;
        const uint32_t retire_result =
            ee_vu0_ialu_retire_matured(vu0, &retired_count);
        if (retire_result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
        {
            vu0->m_micro_pc = pair_pc;
            return retire_result;
        }

        // Match _vu0Exec: VIBackupCycles is aged after lower-pipe stalls and
        // pipe retirement, but before the actual lower-op handler. A stalled
        // branch whose dependency outlives the backup therefore reads the
        // current VI value, not the expired backup value.
        vu0_advance_vi_backup(vu0, vu0->m_cycle - cycles_before_pair);

        if (lower_ialu)
        {
            uint32_t write_mask = 0;
            if (!vu0_execute_lower_ialu(vu0, instruction, &write_mask))
            {
                vu0->m_micro_pc = pair_pc;
                return static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE);
            }

            // The scalar integer ALU operations in this slice have zero
            // IALU-pipeline latency in PCSX2's lower-op metadata.
            const uint32_t schedule_result =
                ee_vu0_ialu_schedule_write(vu0, write_mask, 0);
            if (schedule_result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
            {
                return schedule_result;
            }
        }
        else if (lower_memory)
        {
            uint32_t write_mask = 0;
            if (!vu0_execute_lower_memory(
                    cpu, vu0, instruction, memory_address, &write_mask
                ))
            {
                vu0->m_micro_pc = pair_pc;
                return static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE);
            }

            // PCSX2 declares a four-cycle IALU reservation for ILW.
            // LQ/SQ use the separate four-cycle FMAC pipeline. Even SQ
            // contributes an issue, so pending memory work may extend E.
            const auto memory_op = instruction.m_lower_fields.m_operation;
            if (memory_op == e_ee_vu0_micro_opcode::LQ ||
                memory_op == e_ee_vu0_micro_opcode::SQ ||
                memory_op == e_ee_vu0_micro_opcode::LQI ||
                memory_op == e_ee_vu0_micro_opcode::SQI ||
                memory_op == e_ee_vu0_micro_opcode::LQD ||
                memory_op == e_ee_vu0_micro_opcode::SQD)
            {
                const uint32_t reg =
                    (memory_op == e_ee_vu0_micro_opcode::LQ ||
                     memory_op == e_ee_vu0_micro_opcode::LQI ||
                     memory_op == e_ee_vu0_micro_opcode::LQD)
                        ? instruction.m_lower_fields.m_ft : 0u;
                vu0_record_lower_fmac(vu0, reg, instruction.m_lower_fields.m_xyzw);
            }
            const uint32_t schedule_result = ee_vu0_ialu_schedule_write(
                vu0, write_mask, 4
            );
            if (schedule_result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
            {
                return schedule_result;
            }
        }
        else if (lower_flag_read)
        {
            // Flag reads occupy the lower FMAC pipe but write no VF lane.
            vu0_execute_lower_flag_read(vu0, instruction);
            vu0_record_lower_fmac(vu0, 0u, 0u);
        }
        else if (lower_branch)
        {
            s_vu0_branch_action branch_action{};
            if (!vu0_prepare_lower_branch(vu0, instruction, &branch_action))
            {
                vu0->m_micro_pc = pair_pc;
                return static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE);
            }

            vu0_apply_lower_branch(vu0, branch_action);
        }

        ++executed;

        // Branch issue/retirement is a two-pair pipeline. A branch in an
        // existing delay pair is deferred until after the first target pair.
        vu0_advance_branch_pipeline(vu0);
        vu0->m_tpc = (vu0->m_micro_pc >> 3) & 0x1ffu;

        if ((instruction.m_flags & EE_VU0_MICRO_FLAG_M) != 0)
        {
            vu0->m_m_bit = 1;
        }

        if ((instruction.m_flags & EE_VU0_MICRO_FLAG_E) != 0)
        {
            vu0->m_e_bit = 2;
        }

        if (vu0->m_e_bit != 0)
        {
            --vu0->m_e_bit;
            if (vu0->m_e_bit == 0)
            {
                vu0->m_vi_backup_cycles = 0;
                vu0->m_vpu_status &= ~1u;
                // The pinned PCSX2 interpreter flushes lower FMAC memory
                // reservations even after the E delay pair has executed.
                if (vu0->m_vf_memory_ready_cycle > vu0->m_cycle)
                {
                    vu0->m_cycle = vu0->m_vf_memory_ready_cycle;
                }
                vu0->m_vf_memory_ready_cycle = 0;
                const uint32_t flush_result = ee_vu0_ialu_flush_all(vu0);
                if (flush_result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
                {
                    return flush_result;
                }
            }
        }

        vu0->m_running = (vu0->m_vpu_status & 1u) != 0 ? 1u : 0u;

        // PCSX2's micro-engine returns from each Execute() slice on M,
        // regardless of the caller's wait policy. FINISH_TO_E is implemented
        // by the outer scheduler invoking another slice after M.
        if (vu0->m_m_bit != 0)
        {
            break;
        }
    }

    // If E stopped execution while a branch was still pending, PCSX2 applies
    // that branch target before returning from InterpVU0::Execute().
    if ((vu0->m_vpu_status & 1u) == 0 && vu0->m_branch != 0)
    {
        vu0->m_micro_pc = vu0->m_branch_pc & EE_VU0_MICRO_MEMORY_MASK;
        vu0->m_branch = 0;
        vu0->m_take_delay_branch = 0;
        vu0->m_tpc = (vu0->m_micro_pc >> 3) & 0x1ffu;
    }

    const uint64_t cycles_consumed = vu0->m_cycle - slice_start_cycle;
    if (executed >= instruction_limit &&
        cycles_consumed < max_cycles &&
        (vu0->m_vpu_status & 1u) != 0 &&
        vu0->m_m_bit == 0)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
    }

    if ((flags & EE_VU0_EXECUTE_EXACT_CYCLES) == 0 &&
        cycles_consumed < max_cycles &&
        (vu0->m_vpu_status & 1u) != 0 &&
        vu0->m_m_bit == 0)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
    }

    return static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
}

uint32_t ee_vu0_start_ialu_callback(
    void* user, s_ee_cpu_state* cpu, s_ee_vu0_state* vu0,
    uint32_t address, uint32_t pc)
{
    using result = e_ee_vu0_sync_result;
    if (!user || !cpu || !vu0)
    {
        return static_cast<uint32_t>(result::INVALID_ARGUMENT);
    }
    if (vu0_is_running(vu0) || vu0->m_q_pending != 0u)
    {
        return static_cast<uint32_t>(result::UNAVAILABLE);
    }
    // The micro-engine currently accepts only a subset of VU0 pairs.
    // A rejected first pair must not publish a partial launch.
    const auto previous_vu = *vu0;
    const auto previous_cpu = *cpu;
    vu0->m_tpc = address & 0x1ffu;
    vu0->m_micro_pc = vu0->m_tpc * 8u;
    vu0->m_vpu_status = (vu0->m_vpu_status & ~0xffu) | 1u;
    vu0->m_running = 1u;
    vu0->m_cycle = vu0->m_ee_cycle;
    vu0->m_e_bit = 0u;
    vu0->m_m_bit = 0u;
    vu0->m_branch = 0u;
    vu0->m_take_delay_branch = 0u;
    vu0->m_vi_backup_cycles = 0u;
    const uint32_t started = ee_vu0_execute_ialu_callback(
        user, cpu, vu0, 1u,
        EE_VU0_EXECUTE_STOP_AT_E | EE_VU0_EXECUTE_EXACT_CYCLES, pc);
    if (started != static_cast<uint32_t>(result::COMPLETE))
    {
        *vu0 = previous_vu;
        *cpu = previous_cpu;
    }
    return started;
}

extern "C" uint32_t ee_vu0_read_vi(
    const s_ee_vu0_state* vu0,
    uint32_t index
)
{
    if (!vu0 || index >= 32 || index == 0)
    {
        return 0;
    }

    switch (index)
    {
    case 16: return vu0->m_status_flags;
    case 17: return vu0->m_mac_flags;
    case 18: return vu0->m_clip_flags;
    case 26: return vu0->m_tpc;
    case 28: return vu0->m_fbrst;
    case 29: return vu0->m_vpu_status;
    default: return vu0->m_vi[index];
    }
}

extern "C" void ee_vu0_write_vi(
    s_ee_vu0_state* vu0,
    uint32_t index,
    uint32_t value
)
{
    if (!vu0 || index >= 32 || index == 0)
    {
        return;
    }

    vu0->m_vi[index] = value;
    switch (index)
    {
    case 16: vu0->m_status_flags = value; break;
    case 17: vu0->m_mac_flags = value; break;
    case 18: vu0->m_clip_flags = value; break;
    case 26: vu0->m_tpc = value; break;
    case 28: vu0->m_fbrst = value; break;
    case 29: vu0->m_vpu_status = value; break;
    default: break;
    }
}


namespace
{
constexpr uint32_t VU0_REG_R = 20;
constexpr uint32_t VU0_R_MASK = 0x007fffffu;
} // namespace

extern "C" uint32_t ee_vu0_qmfc2(
    s_ee_cpu_state* cpu,
    s_ee_runtime* rt,
    uint32_t gpr_index,
    uint32_t vf_index,
    uint32_t mode,
    uint32_t pc
)
{
    if (!cpu || gpr_index >= 32 || vf_index >= 32)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    const uint32_t result = ee_vu0_sync(cpu, rt, mode, pc);
    if (result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
    {
        return result;
    }

    if (gpr_index != 0)
    {
        // VF0 is architecturally constant: (0, 0, 0, 1.0f). Do not trust
        // the backing array for register zero, which must never be written.
        cpu->m_gpr[gpr_index] = vf_index == 0
            ? s_ee_u128{0, 0x3f80000000000000ULL}
            : cpu->m_vu0[vf_index];
    }

    return result;
}

extern "C" uint32_t ee_vu0_qmtc2(
    s_ee_cpu_state* cpu,
    s_ee_runtime* rt,
    uint32_t gpr_index,
    uint32_t vf_index,
    uint32_t mode,
    uint32_t pc
)
{
    if (!cpu || gpr_index >= 32 || vf_index >= 32)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    const uint32_t result = ee_vu0_sync(cpu, rt, mode, pc);
    if (result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
    {
        return result;
    }

    if (vf_index != 0)
    {
        cpu->m_vu0[vf_index] = gpr_index == 0
            ? s_ee_u128{}
            : cpu->m_gpr[gpr_index];
    }

    return result;
}

extern "C" uint32_t ee_vu0_cfc2(
    s_ee_cpu_state* cpu,
    s_ee_runtime* rt,
    uint32_t gpr_index,
    uint32_t vi_index,
    uint32_t mode,
    uint32_t pc
)
{
    if (!cpu || gpr_index >= 32 || vi_index >= 32)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    const uint32_t result = ee_vu0_sync(cpu, rt, mode, pc);
    if (result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
    {
        return result;
    }

    if (gpr_index != 0)
    {
        uint32_t value = ee_vu0_read_vi(rt->m_vu0_state, vi_index);
        if (vi_index == VU0_REG_R)
        {
            // PCSX2 writes only UL[0] on this special path. Preserve UL[1]
            // rather than sign-extending or zero-extending the 23-bit value.
            cpu->m_gpr[gpr_index].m_lo =
                (cpu->m_gpr[gpr_index].m_lo & 0xffffffff00000000ULL) |
                (static_cast<uint64_t>(value) & VU0_R_MASK);
        }
        else
        {
            // All ordinary VI reads sign-extend UL[0] into UL[1].
            cpu->m_gpr[gpr_index].m_lo =
                static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value)));
        }
    }

    return result;
}


extern "C" uint32_t ee_vu0_ctc2(
    s_ee_cpu_state* cpu,
    s_ee_runtime* rt,
    uint32_t gpr_index,
    uint32_t vi_index,
    uint32_t mode,
    uint32_t pc
)
{
    if (!cpu || gpr_index >= 32 || vi_index >= 32)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    const uint32_t sync_result = ee_vu0_sync(cpu, rt, mode, pc);
    if (sync_result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
    {
        return sync_result;
    }

    if (vi_index == 0)
    {
        return sync_result;
    }

    // The source is the low 32-bit word; GPR0 is hardwired to zero even if a
    // malformed host state has non-zero backing storage for it.
    const uint32_t value = gpr_index == 0
        ? 0
        : static_cast<uint32_t>(cpu->m_gpr[gpr_index].m_lo);

    s_ee_vu0_state* vu0 = rt->m_vu0_state;
    switch (vi_index)
    {
    case 17: // MAC_FLAG is read-only.
    case 26: // TPC is read-only.
    case 29: // VPU_STAT is read-only.
        return sync_result;

    case 20: // R: preserve the implicit 1.0 exponent.
        ee_vu0_write_vi(vu0, vi_index, (value & VU0_R_MASK) | 0x3f800000u);
        return sync_result;

    case 28: // FBRST: mask stored bits and route reset/break actions to host.
    {
        constexpr uint32_t FBRST_ACTION_MASK = 0x00000303u;
        if ((value & FBRST_ACTION_MASK) != 0)
        {
            if (!rt->m_vu0_control)
            {
                return static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE);
            }

            const uint32_t control_result = rt->m_vu0_control(
                rt->m_vu0_control_user, cpu, vu0, vi_index, value, pc
            );
            if (control_result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
            {
                return control_result <= static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED)
                    ? control_result
                    : static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
            }
        }

        ee_vu0_write_vi(vu0, vi_index, value & 0x00000c0cu);
        return sync_result;
    }

    case 31: // CMSAR1 starts a VU1 micro subroutine after VU1 is finished.
    {
        if (!rt->m_vu0_control)
        {
            return static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE);
        }

        const uint32_t control_result = rt->m_vu0_control(
            rt->m_vu0_control_user, cpu, vu0, vi_index, value & 0xffffu, pc
        );
        if (control_result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
        {
            return control_result <= static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED)
                ? control_result
                : static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
        }

        vu0->m_cmsar1 = value & 0xffffu;
        return sync_result;
    }

    case 18: // CLIP_FLAG also updates the separately tracked clip flags.
        vu0->m_clip_flags = value;
        ee_vu0_write_vi(vu0, vi_index, value);
        return sync_result;

    default:
        ee_vu0_write_vi(vu0, vi_index, value);
        return sync_result;
    }
}

namespace
{
// VU0 stores four IEEE-style bit patterns in each 128-bit VF/ACC register.
// Its register zero is (0, 0, 0, 1), regardless of the CPU backing bytes.
uint32_t vu0_fmac_lane(const s_ee_u128& vector, uint32_t lane)
{
    const uint64_t half = lane < 2 ? vector.m_lo : vector.m_hi;
    return static_cast<uint32_t>(half >> (32u * (lane & 1u)));
}

void vu0_fmac_set_lane(s_ee_u128* vector, uint32_t lane, uint32_t bits)
{
    uint64_t* half = lane < 2 ? &vector->m_lo : &vector->m_hi;
    const uint32_t shift = 32u * (lane & 1u);
    *half = (*half & ~(0xffffffffull << shift)) |
        (static_cast<uint64_t>(bits) << shift);
}

uint32_t vu0_fmac_read_vf(const s_ee_cpu_state* cpu, uint32_t index, uint32_t lane)
{
    return index == 0 ? (lane == 3 ? 0x3f800000u : 0u) :
        vu0_fmac_lane(cpu->m_vu0[index], lane);
}

float vu0_fmac_operand(uint32_t bits)
{
    // VU arithmetic flushes subnormal operands and interprets exponent
    // 255 as a finite, signed maximum rather than host IEEE infinity/NaN.
    const uint32_t exponent = bits & 0x7f800000u;
    if (exponent == 0)
    {
        bits &= 0x80000000u;
    }
    else if (exponent == 0x7f800000u)
    {
        bits = (bits & 0x80000000u) | 0x7f7fffffu;
    }
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void vu0_fmac_integer_flags(uint32_t bits, bool overflow, bool underflow,
                            uint32_t lane, uint32_t* flags)
{
    const uint32_t shift = 3u - lane;
    *flags &= ~(0x1111u << shift);
    if ((bits & 0x80000000u) != 0u)
    {
        *flags |= 0x10u << shift;
    }
    if ((bits & 0x7f800000u) == 0u)
    {
        *flags |= 1u << shift;
    }
    if (underflow)
    {
        *flags |= 0x100u << shift;
    }
    if (overflow)
    {
        *flags |= 0x1000u << shift;
    }
}

} // namespace

namespace
{
enum class e_vu0_macro_arith : uint8_t
{
    add, sub, mul, madd, msub, max, mini
};

enum class e_vu0_macro_source : uint8_t
{
    vector, broadcast, q, i, outer
};

struct s_vu0_macro_fmac_op
{
    e_vu0_macro_arith m_arith;
    e_vu0_macro_source m_source;
    uint32_t m_broadcast;
    bool m_to_acc;
};

// VU User's Manual v6.0 chapter 5 upper-instruction encodings shared by
// COP2 macro mode. SPECIAL1 writes fd; SPECIAL2 accumulator forms write ACC.
bool vu0_macro_decode_fmac(uint32_t funct, uint32_t special2, bool special,
                           s_vu0_macro_fmac_op* op)
{
    using arith = e_vu0_macro_arith;
    using source = e_vu0_macro_source;
    const uint32_t code = special ? special2 : funct;
    const uint32_t bc = code & 3u;
    if (code < 0x10u || (code >= 0x18u && code < 0x1cu) ||
        (!special && code >= 0x10u && code < 0x18u))
    {
        static constexpr arith k_groups[8] = {
            arith::add, arith::sub, arith::madd, arith::msub,
            arith::max, arith::mini, arith::mul, arith::mul
        };
        const arith kind = k_groups[(code >> 2u) & 7u];
        if (special && (kind == arith::max || kind == arith::mini))
        {
            return false; // SPECIAL2 0x10..0x17 are ITOF/FTOI.
        }
        *op = {kind, source::broadcast, bc, special};
        return true;
    }
    switch (code)
    {
    case 0x1cu: *op = {arith::mul, source::q, 0u, special}; return true;
    case 0x1du:
        if (special) return false; // VABS
        *op = {arith::max, source::i, 0u, false}; return true;
    case 0x1eu: *op = {arith::mul, source::i, 0u, special}; return true;
    case 0x1fu:
        if (special) return false; // VCLIP
        *op = {arith::mini, source::i, 0u, false}; return true;
    case 0x20u: *op = {arith::add, source::q, 0u, special}; return true;
    case 0x21u: *op = {arith::madd, source::q, 0u, special}; return true;
    case 0x22u: *op = {arith::add, source::i, 0u, special}; return true;
    case 0x23u: *op = {arith::madd, source::i, 0u, special}; return true;
    case 0x24u: *op = {arith::sub, source::q, 0u, special}; return true;
    case 0x25u: *op = {arith::msub, source::q, 0u, special}; return true;
    case 0x26u: *op = {arith::sub, source::i, 0u, special}; return true;
    case 0x27u: *op = {arith::msub, source::i, 0u, special}; return true;
    case 0x28u: *op = {arith::add, source::vector, 0u, special}; return true;
    case 0x29u: *op = {arith::madd, source::vector, 0u, special}; return true;
    case 0x2au: *op = {arith::mul, source::vector, 0u, special}; return true;
    case 0x2bu:
        if (special) return false;
        *op = {arith::max, source::vector, 0u, false}; return true;
    case 0x2cu: *op = {arith::sub, source::vector, 0u, special}; return true;
    case 0x2du: *op = {arith::msub, source::vector, 0u, special}; return true;
    case 0x2eu: // VOPMULA (ACC) / VOPMSUB (fd)
        *op = {special ? arith::mul : arith::msub, source::outer, 0u, special};
        return true;
    case 0x2fu:
        if (special) return false; // VNOP
        *op = {arith::mini, source::vector, 0u, false}; return true;
    default:
        return false;
    }
}

void vu0_macro_update_status(s_ee_vu0_state* vu0, uint32_t mac_flags)
{
    vu0->m_mac_flags = mac_flags;
    vu0->m_vi[17] = mac_flags;
    const uint32_t current = ((mac_flags & 0x000fu) ? 1u : 0u) |
        ((mac_flags & 0x00f0u) ? 2u : 0u) |
        ((mac_flags & 0x0f00u) ? 4u : 0u) |
        ((mac_flags & 0xf000u) ? 8u : 0u);
    // VU User's Manual v6.0 section 3.3.2: SF[3:0] reflects the
    // latest FMAC result; SF[9:6] accumulates sticky Z/S/U/O by OR.
    // Retain existing I/D (bits 4/5) and all sticky flags, including
    // IS/DS at 10/11.
    vu0->m_status_flags = (vu0->m_status_flags & 0x0ff0u) |
        current | (current << 6u);
    vu0->m_vi[16] = vu0->m_status_flags;
}

uint32_t vu0_macro_bits(float value)
{
    uint32_t bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// MAX/MINI compare the original VU words, not normalized host floats.
// The sign-magnitude ordering distinguishes signed zeros, denormals and
// exponent-255 payloads. Reference: PCSX2 VUops.cpp fp_max/fp_min,
// 4e56d430595164106eab9606cb46c94c902802cd.
uint32_t vu0_macro_order_key(uint32_t bits)
{
    return (bits & 0x80000000u) != 0u ? ~bits : bits ^ 0x80000000u;
}

uint32_t vu0_macro_ftoi(uint32_t bits, double scale)
{
    const double value = static_cast<double>(vu0_fmac_operand(bits)) * scale;
    if (value >= 2147483647.0) return 0x7fffffffu;
    if (value <= -2147483648.0) return 0x80000000u;
    return static_cast<uint32_t>(static_cast<int32_t>(value));
}

void vu0_macro_write_vi16(s_ee_vu0_state* vu0, uint32_t index, uint32_t value)
{
    if ((index & 15u) != 0u)
    {
        ee_vu0_write_vi(vu0, index & 15u, value & 0xffffu);
    }
}

uint32_t vu0_macro_read_vi16(const s_ee_vu0_state* vu0, uint32_t index)
{
    return ee_vu0_read_vi(vu0, index & 15u) & 0xffffu;
}

uint32_t vu0_macro_memory_address(const s_ee_vu0_state* vu0, uint32_t is)
{
    return (vu0_macro_read_vi16(vu0, is) << 4u) &
        (EE_VU0_DATA_MEMORY_SIZE - 1u);
}

uint32_t vu0_macro_load32(const s_ee_vu0_state* vu0, uint32_t address)
{
    uint32_t bits = 0u;
    for (uint32_t byte = 0u; byte < 4u; ++byte)
    {
        bits |= static_cast<uint32_t>(vu0->m_data_memory[
            (address + byte) & (EE_VU0_DATA_MEMORY_SIZE - 1u)]) << (byte * 8u);
    }
    return bits;
}

void vu0_macro_store32(s_ee_vu0_state* vu0, uint32_t address, uint32_t bits)
{
    for (uint32_t byte = 0u; byte < 4u; ++byte)
    {
        vu0->m_data_memory[(address + byte) & (EE_VU0_DATA_MEMORY_SIZE - 1u)] =
            static_cast<uint8_t>(bits >> (byte * 8u));
    }
}

// R is a 23-bit mantissa with an implicit 1.0 exponent (VU manual 4.2.5).
uint32_t vu0_macro_r(uint32_t value)
{
    return 0x3f800000u | (value & 0x007fffffu);
}

bool vu0_macro_is_supported(uint32_t funct, uint32_t special2)
{
    if (funct < 0x3cu)
    {
        s_vu0_macro_fmac_op op{};
        return vu0_macro_decode_fmac(funct, special2, false, &op) ||
            funct == 0x30u || funct == 0x31u || funct == 0x32u ||
            funct == 0x34u || funct == 0x35u;
    }
    s_vu0_macro_fmac_op op{};
    return vu0_macro_decode_fmac(funct, special2, true, &op) ||
        (special2 >= 0x10u && special2 <= 0x17u) ||
        special2 == 0x1du || special2 == 0x1fu || special2 == 0x2fu ||
        special2 == 0x30u || special2 == 0x31u || special2 == 0x38u ||
        (special2 >= 0x3cu && special2 <= 0x43u);
}
} // namespace

// Executes every COP2 macro-mode SPECIAL1/SPECIAL2 VU0 operation except
// LQI/SQI/LQD/SQD (ee_vu0_macro_indexed_memory) and
// SQRT/RSQRT/WAITQ (ee_vu0_macro_scalar). Floating-point semantics follow
// the VU: subnormal operands flush to zero, exponent 255 is a finite max,
// products are rounded before accumulation, and MAC/status flags update only
// for FMAC arithmetic. A rejected encoding commits no architectural state.
extern "C" uint32_t ee_vu0_macro_fmac(
    s_ee_cpu_state* cpu,
    s_ee_runtime* rt,
    uint32_t word,
    uint32_t pc
)
{
    if (!cpu || !rt || (word >> 26) != 0x12u ||
        ((word >> 21) & 31u) < 0x10u)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    const uint32_t funct = word & 63u;
    const uint32_t special2 = (word & 3u) | ((word >> 4) & 0x7cu);
    if (funct == 0x38u || funct == 0x39u)
    {
        if (!rt->m_vu0_start || !rt->m_vu0_state)
        {
            return static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE);
        }
        const uint32_t synced = ee_vu0_sync(cpu, rt,
            static_cast<uint32_t>(e_ee_vu0_sync_mode::FINISH_TO_E), pc);
        if (synced != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
        {
            return synced;
        }
        const uint32_t address = funct == 0x38u
            ? ((word >> 6u) & 0x7fffu)
            : (ee_vu0_read_vi(rt->m_vu0_state, 27u) & 0xffffu);
        return validate_vu0_callback_result(
            rt->m_vu0_start(rt->m_vu0_user, cpu, rt->m_vu0_state, address, pc));
    }
    if (!vu0_macro_is_supported(funct, special2))
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    // COP2 macro operations wait for an active VU0 microprogram to
    // terminate. If the host cannot do this, do not touch VF/ACC/flags.
    const uint32_t sync_result = ee_vu0_sync(
        cpu, rt, static_cast<uint32_t>(e_ee_vu0_sync_mode::FINISH_TO_E), pc
    );
    if (sync_result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
    {
        return sync_result;
    }

    const uint32_t fs = (word >> 11) & 31u;
    const uint32_t ft = (word >> 16) & 31u;
    const uint32_t fd = (word >> 6) & 31u;
    const uint32_t mask = (word >> 21) & 15u;
    s_ee_vu0_state* vu0 = rt->m_vu0_state;
    const auto complete = static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
    const auto write_vf = [cpu](uint32_t index, const s_ee_u128& value)
    {
        if (index != 0u)
        {
            cpu->m_vu0[index] = value;
        }
    };
    const auto lanes_of = [cpu](uint32_t index)
    {
        return index == 0u
            ? s_ee_u128{0u, 0x3f80000000000000ull} : cpu->m_vu0[index];
    };

    const bool special = funct >= 0x3cu;
    s_vu0_macro_fmac_op op{};
    if (vu0_macro_decode_fmac(funct, special2, special, &op))
    {
        const bool reads_q = op.m_source == e_vu0_macro_source::q;
        if (reads_q && vu0->m_q_pending != 0u)
        {
            // Q readers cannot consume an in-flight DIV/SQRT/RSQRT result.
            // The original retail guest executes VWAITQ first.
            return static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE);
        }
        const uint32_t scalar_bits =
            reads_q ? static_cast<uint32_t>(vu0->m_q.m_lo) :
            op.m_source == e_vu0_macro_source::i ? vu0->m_vi[21] : 0u;
        const bool flags = op.m_arith != e_vu0_macro_arith::max &&
            op.m_arith != e_vu0_macro_arith::mini;
        // Stage a separate result: fs==ft==fd must read original lanes.
        s_ee_u128 output = op.m_to_acc ? vu0->m_acc : lanes_of(fd);
        uint32_t mac_flags = vu0->m_mac_flags;
        uint32_t product_sticky = 0u;
        uint32_t acc_overflow_mask = vu0->m_acc_overflow_mask;
        for (uint32_t lane = 0; lane < 4; ++lane)
        {
            if (!(mask & (8u >> lane)))
            {
                if (flags)
                {
                    mac_flags &= ~(0x1111u << (3u - lane));
                }
                continue;
            }
            uint32_t fs_lane = lane;
            uint32_t ft_lane = lane;
            if (op.m_source == e_vu0_macro_source::outer)
            {
                if (lane == 3u)
                {
                    continue; // OPMULA/OPMSUB define x, y and z only.
                }
                fs_lane = (lane + 1u) % 3u;
                ft_lane = (lane + 2u) % 3u;
            }
            else if (op.m_source == e_vu0_macro_source::broadcast)
            {
                ft_lane = op.m_broadcast;
            }
            const uint32_t a_bits = vu0_fmac_read_vf(cpu, fs, fs_lane);
            const uint32_t b_bits = op.m_source == e_vu0_macro_source::q ||
                op.m_source == e_vu0_macro_source::i
                    ? scalar_bits : vu0_fmac_read_vf(cpu, ft, ft_lane);
            if (!flags)
            {
                const uint32_t a_key = vu0_macro_order_key(a_bits);
                const uint32_t b_key = vu0_macro_order_key(b_bits);
                const bool take_a = op.m_arith == e_vu0_macro_arith::max
                    ? a_key >= b_key : a_key <= b_key;
                vu0_fmac_set_lane(&output, lane, take_a ? a_bits : b_bits);
                continue;
            }
            vu_fmac_fp::s_fmac_result result{};
            const uint32_t lane_bit = 8u >> lane;
            if (op.m_arith == e_vu0_macro_arith::add ||
                op.m_arith == e_vu0_macro_arith::sub)
            {
                result = vu_fmac_fp::add(a_bits, b_bits,
                    op.m_arith == e_vu0_macro_arith::sub);
            }
            else if (op.m_arith == e_vu0_macro_arith::mul)
            {
                result = vu_fmac_fp::mul(a_bits, b_bits);
            }
            else
            {
                const auto mac = vu_fmac_fp::mac(vu0_fmac_lane(vu0->m_acc, lane),
                    a_bits, b_bits, op.m_arith == e_vu0_macro_arith::msub,
                    (vu0->m_acc_overflow_mask & lane_bit) != 0u);
                result = mac.m_final;
                product_sticky |= mac.m_sticky;
            }
            if (op.m_to_acc)
            {
                acc_overflow_mask = (acc_overflow_mask & ~lane_bit) |
                    (result.m_overflow ? lane_bit : 0u);
            }
            vu0_fmac_integer_flags(result.m_bits, result.m_overflow,
                result.m_underflow, lane, &mac_flags);
            vu0_fmac_set_lane(&output, lane, result.m_bits);
        }
        if (op.m_to_acc)
        {
            vu0->m_acc = output;
            vu0->m_acc_overflow_mask = acc_overflow_mask;
        }
        else
        {
            write_vf(fd, output);
        }
        if (flags)
        {
            vu0->m_status_flags |= product_sticky;
            vu0_macro_update_status(vu0, mac_flags);
        }
        return complete;
    }

    if (!special)
    {
        // SPECIAL1 integer ALU: VIADD/VISUB/VIAND/VIOR id,is,it; VIADDI.
        const uint32_t is = vu0_macro_read_vi16(vu0, fs);
        const uint32_t it = vu0_macro_read_vi16(vu0, ft);
        switch (funct)
        {
        case 0x30u: vu0_macro_write_vi16(vu0, fd, is + it); break;
        case 0x31u: vu0_macro_write_vi16(vu0, fd, is - it); break;
        case 0x32u:
        {
            const uint32_t imm = (fd & 0x10u) ? (fd | 0xffffffe0u) : fd;
            vu0_macro_write_vi16(vu0, ft, is + imm);
            break;
        }
        case 0x34u: vu0_macro_write_vi16(vu0, fd, is & it); break;
        case 0x35u: vu0_macro_write_vi16(vu0, fd, is | it); break;
        default:
            return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
        }
        return complete;
    }

    const s_ee_u128 source = lanes_of(fs);
    s_ee_u128 output = lanes_of(ft);
    const uint32_t fsf = (word >> 21) & 3u;
    const uint32_t ftf = (word >> 23) & 3u;
    switch (special2)
    {
    case 0x10u: case 0x11u: case 0x12u: case 0x13u: // VITOF0/4/12/15
    case 0x14u: case 0x15u: case 0x16u: case 0x17u: // VFTOI0/4/12/15
    {
        static constexpr double k_scale[4] = {1.0, 16.0, 4096.0, 32768.0};
        const double scale = k_scale[special2 & 3u];
        for (uint32_t lane = 0; lane < 4; ++lane)
        {
            if (!(mask & (8u >> lane)))
            {
                continue;
            }
            const uint32_t bits = vu0_fmac_lane(source, lane);
            vu0_fmac_set_lane(&output, lane, special2 < 0x14u
                ? vu0_macro_bits(static_cast<float>(
                      static_cast<double>(static_cast<int32_t>(bits)) / scale))
                : vu0_macro_ftoi(bits, scale));
        }
        write_vf(ft, output);
        return complete;
    }
    case 0x1du: // VABS
    case 0x30u: // VMOVE
    case 0x31u: // VMR32
        for (uint32_t lane = 0; lane < 4; ++lane)
        {
            if (!(mask & (8u >> lane)))
            {
                continue;
            }
            uint32_t bits = vu0_fmac_lane(
                source, special2 == 0x31u ? (lane + 1u) & 3u : lane);
            if (special2 == 0x1du)
            {
                bits &= 0x7fffffffu; // VABS clears only the raw sign bit.
            }
            vu0_fmac_set_lane(&output, lane, bits);
        }
        write_vf(ft, output);
        return complete;
    case 0x1fu: // VCLIPw.xyz fs, ft: six judgement bits per call.
    {
        const float limit = std::fabs(
            vu0_fmac_operand(vu0_fmac_read_vf(cpu, ft, 3u)));
        uint32_t clip = (vu0->m_clip_flags << 6u) & 0x00ffffffu;
        for (uint32_t lane = 0; lane < 3; ++lane)
        {
            const float value = vu0_fmac_operand(vu0_fmac_lane(source, lane));
            if (value > limit) clip |= 1u << (lane * 2u);
            if (value < -limit) clip |= 2u << (lane * 2u);
        }
        vu0->m_clip_flags = clip;
        vu0->m_vi[18] = clip;
        return complete;
    }
    case 0x2fu: // VNOP
        return complete;
    case 0x38u: // VDIV Q, fs.fsf, ft.ftf (seven-cycle Q latency)
    {
        if (vu0->m_q_pending != 0u || vu0->m_cycle > UINT64_MAX - 7u)
        {
            return static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE);
        }
        const uint32_t fs_bits = vu0_fmac_lane(source, fsf);
        const uint32_t ft_bits = vu0_fmac_read_vf(cpu, ft, ftf);
        const uint32_t current_flags =
            vu_scalar_fp::current_flags(fs_bits, ft_bits, false, false);
        const uint32_t result_bits = vu_scalar_fp::quotient(fs_bits, ft_bits);
        vu0->m_q_pending_bits = result_bits;
        vu0->m_q_ready_cycle = vu0->m_cycle + 7u;
        vu0->m_q_pending = 1u;
        vu0->m_status_flags = (vu0->m_status_flags & ~0x30u) |
            current_flags | (current_flags << 6u);
        vu0->m_vi[16] = vu0->m_status_flags;
        return complete;
    }
    case 0x3cu: // VMTIR it, fs.fsf
        vu0_macro_write_vi16(vu0, ft, vu0_fmac_lane(source, fsf));
        return complete;
    case 0x3du: // VMFIR ft, is: sign-extended VI into each masked lane.
    {
        const uint32_t value = static_cast<uint32_t>(static_cast<int32_t>(
            static_cast<int16_t>(vu0_macro_read_vi16(vu0, fs))));
        for (uint32_t lane = 0; lane < 4; ++lane)
        {
            if (mask & (8u >> lane))
            {
                vu0_fmac_set_lane(&output, lane, value);
            }
        }
        write_vf(ft, output);
        return complete;
    }
    case 0x3eu: // VILWR it, (is): the single selected lane's low 16 bits.
    {
        const uint32_t address = vu0_macro_memory_address(vu0, fs);
        for (uint32_t lane = 0; lane < 4; ++lane)
        {
            if (mask & (8u >> lane))
            {
                vu0_macro_write_vi16(vu0, ft,
                    vu0_macro_load32(vu0, address + lane * 4u));
                break;
            }
        }
        return complete;
    }
    case 0x3fu: // VISWR it, (is): zero-extended VI into each masked lane.
    {
        const uint32_t address = vu0_macro_memory_address(vu0, fs);
        const uint32_t value = vu0_macro_read_vi16(vu0, ft);
        for (uint32_t lane = 0; lane < 4; ++lane)
        {
            if (mask & (8u >> lane))
            {
                vu0_macro_store32(vu0, address + lane * 4u, value);
            }
        }
        return complete;
    }
    case 0x40u: // VRNEXT ft: advance the 23-bit LFSR, then read R.
    case 0x41u: // VRGET ft
    {
        uint32_t r = vu0->m_vi[20];
        if (special2 == 0x40u)
        {
            const uint32_t feedback = ((r >> 4u) ^ (r >> 22u)) & 1u;
            r = vu0_macro_r((r << 1u) ^ feedback);
            vu0->m_vi[20] = r;
        }
        for (uint32_t lane = 0; lane < 4; ++lane)
        {
            if (mask & (8u >> lane))
            {
                vu0_fmac_set_lane(&output, lane, r);
            }
        }
        write_vf(ft, output);
        return complete;
    }
    case 0x42u: // VRINIT R, fs.fsf
        vu0->m_vi[20] = vu0_macro_r(vu0_fmac_lane(source, fsf));
        return complete;
    case 0x43u: // VRXOR R, fs.fsf
        vu0->m_vi[20] = vu0_macro_r(
            vu0->m_vi[20] ^ vu0_fmac_lane(source, fsf));
        return complete;
    default:
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }
}


extern "C" uint32_t ee_vu0_macro_scalar(
    s_ee_cpu_state* cpu,
    s_ee_runtime* rt,
    uint32_t word,
    uint32_t pc
)
{
    if (cpu == nullptr || rt == nullptr ||
        (word >> 26u) != 0x12u ||
        ((word >> 21u) & 31u) < 0x10u ||
        (word & 0x3cu) != 0x3cu)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }
    const uint32_t special2 = (word & 3u) | ((word >> 4u) & 0x7cu);
    if (special2 != 0x39u && special2 != 0x3au && special2 != 0x3bu)
    {
        return static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    // Macro-mode operations require a completed VU0 micro-subroutine.
    // An unavailable host sync callback cannot imply a completed run.
    const uint32_t sync_result = ee_vu0_sync(
        cpu, rt, static_cast<uint32_t>(e_ee_vu0_sync_mode::FINISH_TO_E), pc);
    if (sync_result != static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE))
    {
        return sync_result;
    }

    s_ee_vu0_state* vu0 = rt->m_vu0_state;
    if (special2 == 0x3bu) // VWAITQ
    {
        if (vu0->m_q_pending != 0u)
        {
            // SQRT has a seven-cycle Q latency, RSQRT thirteen cycles.
            // Retire in VU0 virtual time only, never from EE wall time.
            if (vu0->m_cycle < vu0->m_q_ready_cycle)
            {
                vu0->m_cycle = vu0->m_q_ready_cycle;
            }
            const uint32_t value = vu0->m_q_pending_bits;
            vu0->m_q.m_lo = (vu0->m_q.m_lo & 0xffffffff00000000ull) |
                static_cast<uint64_t>(value);
            vu0->m_vi[22] = value;
            vu0->m_q_pending = 0u;
            vu0->m_q_ready_cycle = 0u;
        }
        return static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
    }

    const bool reciprocal = special2 == 0x3au;
    const uint32_t latency = reciprocal ? 13u : 7u;
    if (vu0->m_q_pending != 0u ||
        vu0->m_cycle > UINT64_MAX - latency)
    {
        // Reject overlapping scalar producers rather than overwriting
        // an in-flight Q result or wrapping its readiness deadline.
        return static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE);
    }

    // The selectors are separate: fsf uses bits 22:21, ftf bits 24:23.
    // This retail RSQRT uses VF0.w / sqrt(VF5.x).
    const uint32_t ft = (word >> 16u) & 31u;
    const uint32_t fs = (word >> 11u) & 31u;
    const uint32_t ft_lane = (word >> 23u) & 3u;
    const uint32_t fs_lane = (word >> 21u) & 3u;
    const uint32_t ft_bits = vu0_fmac_read_vf(cpu, ft, ft_lane);
    const uint32_t fs_bits = vu0_fmac_read_vf(cpu, fs, fs_lane);
    // Raw denominator sign raises I even when exponent zero; RSQRT
    // divides by the absolute root and can raise I and D together.
    const uint32_t current_flags =
        vu_scalar_fp::current_flags(fs_bits, ft_bits, !reciprocal, reciprocal);
    const uint32_t root = vu_scalar_fp::root(ft_bits);
    const uint32_t result_bits = reciprocal ?
        vu_scalar_fp::quotient(fs_bits, root) : root;

    vu0->m_q_pending_bits = result_bits;
    vu0->m_q_ready_cycle = vu0->m_cycle + latency;
    vu0->m_q_pending = 1u;
    // I/D are current flags; IS/DS must remain sticky.
    vu0->m_status_flags =
        (vu0->m_status_flags & ~0x30u) |
        current_flags | (current_flags << 6u);
    vu0->m_vi[16] = vu0->m_status_flags;
    return static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
}

// R5900 COP2 SPECIAL2 indexed VU0 vector-memory macros. These access
// VU0's separate 4 KiB data memory (VI address in 16-byte units), not
// EE main memory, and preserve VF0/VI0 hardwired register semantics.
// This is ordinary translated guest execution, not an HLE shortcut.
extern "C" uint32_t ee_vu0_macro_indexed_memory(
    s_ee_cpu_state* cpu,
    s_ee_runtime* rt,
    uint32_t word,
    uint32_t pc
)
{
    if (cpu == nullptr || rt == nullptr || rt->m_vu0_state == nullptr ||
        (word >> 26u) != 0x12u ||
        ((word >> 21u) & 31u) < 0x10u ||
        (word & 0x3cu) != 0x3cu)
    {
        return static_cast<uint32_t>(
            e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    const uint32_t secondary = (word & 3u) | ((word >> 4u) & 0x7cu);
    if (secondary < 0x34u || secondary > 0x37u)
    {
        return static_cast<uint32_t>(
            e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    const bool load = (secondary & 1u) == 0u;
    const bool predecrement = (secondary & 2u) != 0u;
    const uint32_t fs = (word >> 11u) & 31u;
    const uint32_t ft = (word >> 16u) & 31u;
    const uint32_t base_index = load ? fs : ft;
    const uint32_t vector_index = load ? ft : fs;
    const uint32_t mask = (word >> 21u) & 15u;
    if (base_index >= 16u)
    {
        // VI16..31 are control registers, not memory address registers.
        return static_cast<uint32_t>(
            e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }

    // The macro is not allowed to race an active VU0 microprogram.
    // A failed synchronization commits no VI/VF/memory changes.
    const uint32_t sync_result = ee_vu0_sync(
        cpu, rt, static_cast<uint32_t>(e_ee_vu0_sync_mode::FINISH_TO_E), pc
    );
    if (sync_result != static_cast<uint32_t>(
            e_ee_vu0_sync_result::COMPLETE))
    {
        return sync_result;
    }

    s_ee_vu0_state* vu0 = rt->m_vu0_state;
    uint16_t vi_value = static_cast<uint16_t>(
        ee_vu0_read_vi(vu0, base_index));
    if (predecrement && base_index != 0u)
    {
        vi_value = static_cast<uint16_t>(vi_value - 1u);
    }
    const uint32_t address = (static_cast<uint32_t>(vi_value) << 4u) &
        (EE_VU0_DATA_MEMORY_SIZE - 1u);
    const s_ee_u128 original = vector_index == 0u
        ? s_ee_u128{0u, 0x3f80000000000000ull}
        : cpu->m_vu0[vector_index];
    s_ee_u128 vector = original;

    for (uint32_t lane = 0u; lane < 4u; ++lane)
    {
        if ((mask & (8u >> lane)) == 0u)
        {
            continue;
        }
        const uint32_t offset = address + lane * 4u;
        const uint32_t shift = (lane & 1u) * 32u;
        uint64_t& half = lane < 2u ? vector.m_lo : vector.m_hi;
        if (load)
        {
            uint32_t bits = 0u;
            for (uint32_t byte = 0u; byte < 4u; ++byte)
            {
                bits |= static_cast<uint32_t>(
                    vu0->m_data_memory[offset + byte]) << (byte * 8u);
            }
            half = (half & ~(0xffffffffull << shift)) |
                (static_cast<uint64_t>(bits) << shift);
        }
        else
        {
            const uint32_t bits = static_cast<uint32_t>(half >> shift);
            for (uint32_t byte = 0u; byte < 4u; ++byte)
            {
                vu0->m_data_memory[offset + byte] =
                    static_cast<uint8_t>(bits >> (byte * 8u));
            }
        }
    }

    if (load && vector_index != 0u)
    {
        cpu->m_vu0[vector_index] = vector;
    }
    // VI0 never changes; all other indexed memory operations update their
    // base even if the VF target is VF0 or the destination mask is empty.
    if (base_index != 0u)
    {
        ee_vu0_write_vi(vu0, base_index,
            predecrement ? vi_value
            : static_cast<uint16_t>(vi_value + 1u));
    }
    return static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
}

extern "C" uint32_t ee_vu0_get_branch_condition(const s_ee_runtime* rt)
{
    if (!rt || !rt->m_vu0_state)
    {
        // 0 and 1 are valid branch predicates. 2 is the fail-closed sentinel.
        return 2;
    }

    // PCSX2's BC2 predicate is VU0.VI[REG_VPU_STAT].US[0] bit 8.
    return (rt->m_vu0_state->m_vpu_status & (1u << 8)) != 0 ? 1u : 0u;
}


} // namespace ee
