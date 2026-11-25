#include "ee_runtime.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <cstdio>
#include <type_traits>

namespace
{

struct s_mock_scheduler
{
    uint32_t m_calls = 0;
    uint32_t m_last_mode = 0xffffffffu;
    uint32_t m_last_pc = 0;
    ee::s_ee_cpu_state* m_last_cpu = nullptr;
    ee::s_ee_vu0_state* m_last_vu0 = nullptr;
    uint32_t m_result = static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE);
};

uint32_t mock_vu0_sync(
    void* user,
    ee::s_ee_cpu_state* cpu,
    ee::s_ee_vu0_state* vu0,
    uint32_t mode,
    uint32_t pc
)
{
    auto* scheduler = static_cast<s_mock_scheduler*>(user);
    if (!scheduler)
    {
        return static_cast<uint32_t>(ee::e_ee_vu0_sync_result::FAILED);
    }

    ++scheduler->m_calls;
    scheduler->m_last_mode = mode;
    scheduler->m_last_pc = pc;
    scheduler->m_last_cpu = cpu;
    scheduler->m_last_vu0 = vu0;

    // A test scheduler mutation proves that the callback is operating on the
    // exact state objects owned by the caller, not copies or hidden globals.
    vu0->m_cycle += 7;
    cpu->m_pc = pc;
    return scheduler->m_result;
}

struct s_mock_control
{
    uint32_t m_calls = 0;
    uint32_t m_last_register = 0xffffffffu;
    uint32_t m_last_value = 0;
    uint32_t m_last_pc = 0;
    uint32_t m_result = static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE);
};

uint32_t mock_vu0_control(
    void* user,
    ee::s_ee_cpu_state*,
    ee::s_ee_vu0_state*,
    uint32_t register_index,
    uint32_t value,
    uint32_t pc
)
{
    auto* control = static_cast<s_mock_control*>(user);
    if (!control)
    {
        return static_cast<uint32_t>(ee::e_ee_vu0_sync_result::FAILED);
    }

    ++control->m_calls;
    control->m_last_register = register_index;
    control->m_last_value = value;
    control->m_last_pc = pc;
    return control->m_result;
}

struct s_mock_vu0_engine
{
    uint32_t m_run_calls = 0;
    uint32_t m_vu1_calls = 0;
    uint32_t m_event_calls = 0;
    uint64_t m_last_budget = 0;
    uint32_t m_last_flags = 0;
    uint32_t m_last_pc = 0;
    uint32_t m_last_event_delta = 0;
    uint32_t m_clear_e_after_call = 0;
    uint32_t m_set_m_after_call = 0;
    uint64_t m_cycles_per_call = 10;
    uint32_t m_execute_result =
        static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE);
    uint32_t m_vu1_result =
        static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE);
};

uint32_t mock_vu0_execute(
    void* user,
    ee::s_ee_cpu_state*,
    ee::s_ee_vu0_state* vu0,
    uint64_t max_cycles,
    uint32_t flags,
    uint32_t pc
)
{
    auto* engine = static_cast<s_mock_vu0_engine*>(user);
    if (!engine || !vu0)
    {
        return static_cast<uint32_t>(ee::e_ee_vu0_sync_result::FAILED);
    }

    ++engine->m_run_calls;
    engine->m_last_budget = max_cycles;
    engine->m_last_flags = flags;
    engine->m_last_pc = pc;
    if (engine->m_execute_result !=
        static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE))
    {
        return engine->m_execute_result;
    }

    const bool exact = (flags & ee::EE_VU0_EXECUTE_EXACT_CYCLES) != 0;
    vu0->m_cycle += exact ? max_cycles : engine->m_cycles_per_call;
    if (engine->m_set_m_after_call != 0 &&
        engine->m_run_calls >= engine->m_set_m_after_call)
    {
        vu0->m_m_bit = 1;
    }
    if (engine->m_clear_e_after_call != 0 &&
        engine->m_run_calls >= engine->m_clear_e_after_call)
    {
        vu0->m_vpu_status &= ~1u;
    }
    return static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE);
}

uint32_t mock_vu1_catch_up(
    void* user,
    ee::s_ee_cpu_state*,
    ee::s_ee_vu0_state*,
    uint32_t
)
{
    auto* engine = static_cast<s_mock_vu0_engine*>(user);
    if (!engine)
    {
        return static_cast<uint32_t>(ee::e_ee_vu0_sync_result::FAILED);
    }
    ++engine->m_vu1_calls;
    return engine->m_vu1_result;
}

void mock_vu0_schedule_event(
    void* user,
    ee::s_ee_cpu_state*,
    ee::s_ee_vu0_state*,
    uint32_t delta_cycles,
    uint32_t pc
)
{
    auto* engine = static_cast<s_mock_vu0_engine*>(user);
    if (!engine) return;
    ++engine->m_event_calls;
    engine->m_last_event_delta = delta_cycles;
    engine->m_last_pc = pc;
}

ee::s_ee_vu0_scheduler make_mock_vu0_scheduler(s_mock_vu0_engine* engine)
{
    ee::s_ee_vu0_scheduler scheduler{};
    scheduler.m_user = engine;
    scheduler.m_execute = mock_vu0_execute;
    scheduler.m_vu1_catch_up = mock_vu1_catch_up;
    scheduler.m_schedule_event = mock_vu0_schedule_event;
    scheduler.m_max_run_calls = 16;
    return scheduler;
}

uint32_t mock_ialu_vu1_catch_up(
    void*,
    ee::s_ee_cpu_state*,
    ee::s_ee_vu0_state*,
    uint32_t
)
{
    return static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE);
}

void mock_ialu_schedule_event(
    void*,
    ee::s_ee_cpu_state*,
    ee::s_ee_vu0_state*,
    uint32_t,
    uint32_t
)
{
}

void write_le32(uint8_t* destination, uint32_t value)
{
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8);
    destination[2] = static_cast<uint8_t>(value >> 16);
    destination[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t make_vu0_upper_nop(uint32_t raw_flags = 0)
{
    return (0x3fu | (11u << 6) | raw_flags);
}

uint32_t make_vu0_ialu_3r(
    uint32_t secondary,
    uint32_t it,
    uint32_t is,
    uint32_t id
)
{
    return (0x40u << 25) | (it << 16) | (is << 11) |
           (id << 6) | secondary;
}

uint32_t make_vu0_iaddi(uint32_t it, uint32_t is, int32_t immediate)
{
    return (0x40u << 25) | (it << 16) | (is << 11) |
           ((static_cast<uint32_t>(immediate) & 0x1fu) << 6) | 0x32u;
}

uint32_t make_vu0_memory(
    uint32_t primary,
    uint32_t xyzw,
    uint32_t it,
    uint32_t is,
    int32_t immediate
)
{
    return (primary << 25) | ((xyzw & 0x0fu) << 21) |
           ((it & 0x0fu) << 16) | ((is & 0x0fu) << 11) |
           (static_cast<uint32_t>(immediate) & 0x07ffu);
}

uint32_t make_vu0_indexed_memory(
    uint32_t secondary,
    uint32_t xyzw,
    uint32_t ft,
    uint32_t fs
)
{
    return (0x40u << 25) | ((xyzw & 0x0fu) << 21) |
           ((ft & 0x1fu) << 16) | ((fs & 0x1fu) << 11) |
           (13u << 6) | secondary;
}

uint32_t make_vu0_iaddiu(uint32_t it, uint32_t is, uint32_t immediate)
{
    return (0x08u << 25) | (it << 16) | (is << 11) |
           ((immediate & 0x7800u) << 10) | (immediate & 0x07ffu);
}

uint32_t make_vu0_isubiu(uint32_t it, uint32_t is, uint32_t immediate)
{
    return (0x09u << 25) | (it << 16) | (is << 11) |
           ((immediate & 0x7800u) << 10) | (immediate & 0x07ffu);
}

uint32_t make_vu0_branch(
    uint32_t primary,
    uint32_t it,
    uint32_t is,
    uint32_t immediate
)
{
    return (primary << 25) | (it << 16) | (is << 11) |
           (immediate & 0x07ffu);
}

void install_vu0_pair(
    ee::s_ee_vu0_state* vu0,
    uint32_t slot,
    uint32_t lower,
    uint32_t upper
)
{
    const uint32_t byte_pc = slot * 8u;
    write_le32(&vu0->m_micro_memory[byte_pc], lower);
    write_le32(&vu0->m_micro_memory[byte_pc + 4u], upper);
}

int g_pass = 0;
int g_fail = 0;

void check(bool condition, const char* name)
{
    if (condition)
    {
        ++g_pass;
    }
    else
    {
        ++g_fail;
        std::printf("FAIL %s\n", name);
    }
}

} // namespace

static_assert(std::is_standard_layout_v<ee::s_ee_vu0_state>);
static_assert(alignof(ee::s_ee_vu0_state) == 16);
static_assert(offsetof(ee::s_ee_vu0_state, m_vi) == 0);
static_assert(offsetof(ee::s_ee_vu0_state, m_acc) == 128);
static_assert(offsetof(ee::s_ee_vu0_state, m_q) == 144);
static_assert(offsetof(ee::s_ee_vu0_state, m_p) == 160);
static_assert(offsetof(ee::s_ee_vu0_state, m_cycle) == 208);
static_assert(offsetof(ee::s_ee_vu0_state, m_ee_cycle) == 216);
static_assert(offsetof(ee::s_ee_vu0_state, m_running) == 224);
static_assert(offsetof(ee::s_ee_vu0_state, m_data_memory) == 240);
static_assert(offsetof(ee::s_ee_vu0_state, m_micro_memory) == 4336);
static_assert(offsetof(ee::s_ee_vu0_state, m_ialu) == 8432);
static_assert(offsetof(ee::s_ee_vu0_state, m_ialu_read_pos) == 8496);
static_assert(offsetof(ee::s_ee_vu0_state, m_ialu_write_pos) == 8500);
static_assert(offsetof(ee::s_ee_vu0_state, m_ialu_count) == 8504);
static_assert(offsetof(ee::s_ee_vu0_state, m_branch) == 8508);
static_assert(offsetof(ee::s_ee_vu0_state, m_branch_pc) == 8512);
static_assert(offsetof(ee::s_ee_vu0_state, m_delay_branch_pc) == 8516);
static_assert(offsetof(ee::s_ee_vu0_state, m_take_delay_branch) == 8520);
static_assert(offsetof(ee::s_ee_vu0_state, m_vi_backup_cycles) == 8524);
static_assert(offsetof(ee::s_ee_vu0_state, m_vi_backup_register) == 8528);
static_assert(offsetof(ee::s_ee_vu0_state, m_vi_backup_old_value) == 8532);
static_assert(offsetof(ee::s_ee_vu0_state, m_vf_memory_ready_cycle) == 8536);
static_assert(offsetof(ee::s_ee_vu0_state, m_vf_memory_issues) == 8544);
static_assert(offsetof(ee::s_ee_vu0_state, m_vf_memory_issue_index) == 8608);
// Existing VU0 offsets remain ABI-stable; scalar-Q fields are append-only.
static_assert(offsetof(ee::s_ee_vu0_state, m_q_ready_cycle) == 8616);
static_assert(offsetof(ee::s_ee_vu0_state, m_q_pending_bits) == 8624);
static_assert(offsetof(ee::s_ee_vu0_state, m_q_pending) == 8628);
static_assert(offsetof(ee::s_ee_vu0_state, m_clip_issues) == 8632);
static_assert(offsetof(ee::s_ee_vu0_state, m_upper_fmac_issues) == 8696);
static_assert(sizeof(ee::s_ee_vu0_state) == 8800);

int main()
{
    using ee::e_ee_vu0_sync_mode;
    using ee::e_ee_vu0_sync_result;
    using ee::e_ee_vu0_transfer_direction;

    ee::s_ee_cpu_state cpu{};
    ee::s_ee_vu0_state vu0{};
    s_mock_scheduler scheduler{};

    // VCALLMS uses a 15-bit immediate and VCALLMSR reads CMSAR0.
    // Both launch the real micro-engine at a wrapped 9-bit VU0 slot.
    {
        ee::s_ee_cpu_state launch_cpu{};
        ee::s_ee_vu0_state launch_vu{};
        ee::s_ee_vu0_ialu_engine engine{};
        ee::s_ee_runtime rt{};
        rt.m_vu0_state = &launch_vu;
        rt.m_vu0_user = &engine;
        rt.m_vu0_sync = [](void*, ee::s_ee_cpu_state*, ee::s_ee_vu0_state* state,
            uint32_t, uint32_t) -> uint32_t
        {
            return static_cast<uint32_t>((state->m_vpu_status & 1u) != 0u
                ? e_ee_vu0_sync_result::UNAVAILABLE : e_ee_vu0_sync_result::COMPLETE);
        };
        const uint32_t call = 0x4a000038u | (0x203u << 6u);
        const auto complete = static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
        const auto unavailable = static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE);
        install_vu0_pair(&launch_vu, 3u, make_vu0_iaddiu(1u, 0u, 7u),
            make_vu0_upper_nop() | 0x40000000u);
        install_vu0_pair(&launch_vu, 4u, make_vu0_iaddiu(2u, 1u, 4u),
            make_vu0_upper_nop());
        check(ee::ee_vu0_macro_fmac(&launch_cpu, &rt, call, 0u) == unavailable &&
              launch_vu.m_running == 0u, "VCALLMS without engine fails closed");
        rt.m_vu0_start = ee::ee_vu0_start_ialu_callback;
        check(ee::ee_vu0_macro_fmac(&launch_cpu, &rt, call, 0u) == complete &&
              launch_vu.m_running == 1u && launch_vu.m_micro_pc == 32u &&
              launch_vu.m_vi[1] == 7u, "VCALLMS starts real microcode");
        check(ee::ee_vu0_execute_ialu_callback(&engine, &launch_cpu, &launch_vu,
              8u, ee::EE_VU0_EXECUTE_STOP_AT_E, 0u) == complete &&
              launch_vu.m_running == 0u && launch_vu.m_vi[2] == 11u,
              "VCALLMS E delay pair completes");
        launch_vu.m_vi[27] = 0x203u;
        check(ee::ee_vu0_macro_fmac(&launch_cpu, &rt, 0x4a000039u, 0u) == complete &&
              launch_vu.m_micro_pc == 32u && launch_vu.m_running == 1u,
              "VCALLMSR reads CMSAR0");
    }

    // Read the three flag banks using the original microinstruction forms.
    {
        ee::s_ee_cpu_state flag_cpu{};
        ee::s_ee_vu0_state flag_vu{};
        ee::s_ee_vu0_ialu_engine engine{};
        flag_vu.m_vpu_status = 1u;
        flag_vu.m_clip_flags = 0xabc123u;
        flag_vu.m_mac_flags = 0x3456u;
        flag_vu.m_status_flags = 0x245u;
        flag_vu.m_vi[1] = 0xabcd0000u;
        flag_vu.m_vi[4] = 0x80u;
        flag_vu.m_vi[0] = 0xdeadbeefu;
        install_vu0_pair(&flag_vu, 0, (0x1cu << 25u) | (1u << 16u),
            make_vu0_upper_nop());
        install_vu0_pair(&flag_vu, 1, (0x1bu << 25u) | (2u << 16u) | (4u << 11u),
            make_vu0_upper_nop());
        install_vu0_pair(&flag_vu, 2, (0x17u << 25u) | (3u << 16u) | (1u << 21u) | 0x20u,
            make_vu0_upper_nop());
        install_vu0_pair(&flag_vu, 3, (0x1cu << 25u),
            make_vu0_upper_nop(0x40000000u));
        install_vu0_pair(&flag_vu, 4, 0x8000033cu, make_vu0_upper_nop());
        const auto result = ee::ee_vu0_execute_ialu_callback(&engine, &flag_cpu,
            &flag_vu, 20u, ee::EE_VU0_EXECUTE_STOP_AT_E, 0u);
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              flag_vu.m_running == 0u && flag_vu.m_micro_pc == 40u &&
              flag_vu.m_vi[1] == 0xabcd0123u && flag_vu.m_vi[2] == 0x34d6u &&
              flag_vu.m_vi[3] == 0xa65u && flag_vu.m_vi[0] == 0xdeadbeefu &&
              flag_vu.m_cycle == 8u,
              "VU0 flag reads, VI0 and lower NOP/E completion");
    }

    // Four consecutive upper CLIP/lower FCGET pairs and six readbacks.
    // Expected history comes from the original console upper/clip Multi
    // test, with the real four-cycle delay and no transcript substitution.
    {
        ee::s_ee_cpu_state clip_cpu{};
        ee::s_ee_vu0_state clip_vu{};
        ee::s_ee_vu0_ialu_engine engine{};
        clip_vu.m_vpu_status = 1u;
        clip_cpu.m_vu0[1] = {0x4000000040000000ull, 0x4000000040000000ull};
        clip_cpu.m_vu0[2] = {0xc0000000c0000000ull, 0xc0000000c0000000ull};
        clip_cpu.m_vu0[3] = {0xc000000040000000ull, 0xc000000040000000ull};
        clip_cpu.m_vu0[4] = {0x40000000c0000000ull, 0x40000000c0000000ull};
        for (uint32_t i = 0; i < 10u; ++i)
        {
            const uint32_t fcget = (0x1cu << 25u) | ((i + 1u) << 16u);
            const uint32_t upper = i < 4u
                ? (14u << 21u) | ((i + 1u) << 11u) | (7u << 6u) | 0x3fu
                : make_vu0_upper_nop(i == 8u ? 0x40000000u : 0u);
            install_vu0_pair(&clip_vu, i, fcget, upper);
        }
        const uint32_t expected[] = {0, 0, 0, 0, 0x15, 0x56a, 0xa99,
                                      0x666, 0x666, 0x666};
        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &engine, &clip_cpu, &clip_vu, 64u, ee::EE_VU0_EXECUTE_STOP_AT_E, 0u);
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              clip_vu.m_running == 0u && clip_vu.m_cycle == 14u,
              "CLIP pair engine stops and flushes at E");
        for (uint32_t i = 0; i < 10u; ++i)
        {
            check(clip_vu.m_vi[i + 1u] == expected[i],
                  "CLIP/FCGET four-cycle history");
        }
    }

    // Upper arithmetic issues a VF dependency and delayed MAC/status
    // result independently of the paired lower instruction.
    {
        ee::s_ee_cpu_state fmac_cpu{};
        ee::s_ee_vu0_state fmac_vu{};
        ee::s_ee_vu0_ialu_engine engine{};
        fmac_vu.m_vpu_status = 1u;
        fmac_cpu.m_vu0[1] = {0x400000003f800000ull, 0u};
        fmac_cpu.m_vu0[2] = {0x40000000bf800000ull, 0u};
        fmac_cpu.m_vu0[3] = {0x400000003f800000ull, 0u};
        const auto upper_op = [](uint32_t op, uint32_t fd, uint32_t fs,
                                 uint32_t ft, uint32_t mask)
        {
            return (mask << 21u) | (ft << 16u) | (fs << 11u) | (fd << 6u) | op;
        };
        const auto fmor = [](uint32_t dest)
        {
            return (0x1bu << 25u) | (dest << 16u);
        };
        install_vu0_pair(&fmac_vu, 0, fmor(1), upper_op(0x28, 3, 1, 2, 8));
        // Reading the unaffected Y lane must not stall on the X result.
        install_vu0_pair(&fmac_vu, 1, fmor(2), upper_op(0x2a, 4, 3, 2, 4));
        // An overlapping X dependency stalls until ADD matures at cycle 5.
        install_vu0_pair(&fmac_vu, 2, fmor(3), upper_op(0x2c, 5, 3, 1, 8));
        install_vu0_pair(&fmac_vu, 3, fmor(4), make_vu0_upper_nop(0x40000000u));
        install_vu0_pair(&fmac_vu, 4, fmor(5), make_vu0_upper_nop());
        const auto result = ee::ee_vu0_execute_ialu_callback(&engine, &fmac_cpu,
            &fmac_vu, 64u, ee::EE_VU0_EXECUTE_STOP_AT_E, 0u);
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              fmac_vu.m_cycle == 11u && fmac_vu.m_running == 0u,
              "upper FMAC overlap stalls and E flushes");
        check(fmac_cpu.m_vu0[3].m_lo == 0x4000000000000000ull &&
              fmac_cpu.m_vu0[4].m_lo == 0x4080000000000000ull &&
              fmac_cpu.m_vu0[5].m_lo == 0x00000000bf800000ull,
              "upper ADD/MUL/SUB values and lane masks");
        check(fmac_vu.m_vi[1] == 0u && fmac_vu.m_vi[2] == 0u &&
              fmac_vu.m_vi[3] == 8u && fmac_vu.m_vi[4] == 0u &&
              fmac_vu.m_vi[5] == 0u && fmac_vu.m_mac_flags == 0x80u &&
              fmac_vu.m_status_flags == 0xc2u,
              "upper FMAC delayed MAC and sticky status flags");
    }

    // Both halves sample the old VF value even when the upper arithmetic
    // overwrites the vector consumed by the lower store in the same pair.
    {
        ee::s_ee_cpu_state pair_cpu{};
        ee::s_ee_vu0_state pair_vu{};
        ee::s_ee_vu0_ialu_engine engine{};
        pair_vu.m_vpu_status = 1u;
        pair_vu.m_vi[21] = 0x40000000u; // I = 2.0
        pair_cpu.m_vu0[2].m_lo = 0x3f800000u;
        pair_cpu.m_vu0[3].m_lo = 0x40a0000040400000ull; // y=5, x=3
        const uint32_t addx = (12u << 21u) | (2u << 16u) |
            (3u << 11u) | (3u << 6u);
        const uint32_t addi = (8u << 21u) | (3u << 11u) |
            (4u << 6u) | 0x22u;
        install_vu0_pair(&pair_vu, 0, make_vu0_memory(0x01, 8, 0, 3, 2), addx);
        install_vu0_pair(&pair_vu, 1, 0x8000033cu, addi);
        install_vu0_pair(&pair_vu, 2, 0x8000033cu, make_vu0_upper_nop(0x40000000u));
        install_vu0_pair(&pair_vu, 3, 0x8000033cu, make_vu0_upper_nop());
        const auto result = ee::ee_vu0_execute_ialu_callback(&engine, &pair_cpu,
            &pair_vu, 64u, ee::EE_VU0_EXECUTE_STOP_AT_E, 0u);
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              pair_vu.m_cycle == 9u && pair_vu.m_running == 0u,
              "paired ADDx/SQ and dependent ADDi retire");
        check(pair_cpu.m_vu0[3].m_lo == 0x40c0000040800000ull &&
              pair_cpu.m_vu0[4].m_lo == 0x40c00000u,
              "broadcast, I register and aliased destination use original operands");
        check(pair_vu.m_data_memory[32] == 0u &&
              pair_vu.m_data_memory[33] == 0u &&
              pair_vu.m_data_memory[34] == 0x40u &&
              pair_vu.m_data_memory[35] == 0x40u,
              "lower SQ observes the VF value before the paired ADDx");

        // Conflicting simultaneous VF writes deliberately remain unavailable.
        ee::s_ee_vu0_state conflict{};
        conflict.m_vpu_status = 1u;
        const auto previous = pair_cpu.m_vu0[3];
        install_vu0_pair(&conflict, 0, make_vu0_memory(0x00, 8, 3, 0, 2), addx);
        check(ee::ee_vu0_execute_ialu_callback(&engine, &pair_cpu, &conflict, 1u,
                  ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0u) ==
                  static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE) &&
              conflict.m_cycle == 0u && conflict.m_micro_pc == 0u &&
              pair_cpu.m_vu0[3].m_lo == previous.m_lo &&
              pair_cpu.m_vu0[3].m_hi == previous.m_hi,
              "conflicting upper/lower VF writes fail without partial execution");
    }

    // ACC forwarding, masked writes and multiply/accumulate flag timing.
    {
        ee::s_ee_cpu_state acc_cpu{};
        ee::s_ee_vu0_state acc_vu{};
        ee::s_ee_vu0_ialu_engine engine{};
        acc_vu.m_vpu_status = 1u;
        acc_cpu.m_vu0[1].m_lo = 0x400000003f800000ull; // x=1, y=2
        acc_cpu.m_vu0[2].m_lo = 0x4080000040400000ull; // x=3, y=4
        acc_vu.m_acc.m_hi = 0x123456789abcdef0ull;
        const auto upper = [](uint32_t op, uint32_t fd, uint32_t fs,
                               uint32_t ft, uint32_t mask)
        {
            return (mask << 21u) | (ft << 16u) | (fs << 11u) | (fd << 6u) | op;
        };
        const auto special = [&](uint32_t op, uint32_t fs, uint32_t ft, uint32_t mask)
        {
            return upper(0x3cu | (op & 3u), op >> 2u, fs, ft, mask);
        };
        const uint32_t nop = 0x8000033cu;
        // ADDA: ACC=(4,6), MULA.y: ACC=(4,8),
        // MADDA.x: ACC=(7,8), MSUBA.y: ACC=(7,0).
        install_vu0_pair(&acc_vu, 0, nop, special(0x28, 1, 2, 12));
        install_vu0_pair(&acc_vu, 1, nop, special(0x2a, 1, 2, 4));
        install_vu0_pair(&acc_vu, 2, nop, special(0x29, 1, 2, 8));
        install_vu0_pair(&acc_vu, 3, nop, special(0x2d, 1, 2, 4));
        install_vu0_pair(&acc_vu, 4, nop, upper(0x29, 3, 1, 2, 12));
        install_vu0_pair(&acc_vu, 5, nop, upper(0x2d, 4, 1, 2, 12));
        install_vu0_pair(&acc_vu, 6, nop, special(0x2c, 1, 2, 8) | 0x40000000u);
        install_vu0_pair(&acc_vu, 7, nop, make_vu0_upper_nop());
        const auto result = ee::ee_vu0_execute_ialu_callback(&engine, &acc_cpu,
            &acc_vu, 64u, ee::EE_VU0_EXECUTE_STOP_AT_E, 0u);
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              acc_vu.m_cycle == 11u && acc_vu.m_running == 0u,
              "ACC chain forwards without a VF dependency stall");
        check(acc_cpu.m_vu0[3].m_lo == 0x4100000041200000ull &&
              acc_cpu.m_vu0[4].m_lo == 0xc100000040800000ull &&
              acc_vu.m_acc.m_lo == 0x00000000c0000000ull &&
              acc_vu.m_acc.m_hi == 0x123456789abcdef0ull,
              "ADDA/MULA/MADDA/MSUBA/MADD/MSUB/SUBA results");
        check(acc_vu.m_mac_flags == 0x80u &&
              acc_vu.m_status_flags == 0xc2u,
              "ACC flags retire in issue order with sticky zero and sign");
    }

    // I-bit lower words are literals, not opcodes. ADDi in an I pair
    // sees the old I, even when the lower word resembles a branch.
    {
        ee::s_ee_cpu_state imm_cpu{};
        ee::s_ee_vu0_state imm_vu{};
        ee::s_ee_vu0_ialu_engine engine{};
        imm_vu.m_vpu_status = 1u;
        imm_vu.m_vi[21] = 0x3f800000u; // 1.0
        imm_cpu.m_vu0[1].m_lo = 0x40000000u; // 2.0
        const auto addi = [](uint32_t fd)
        {
            return (8u << 21u) | (1u << 11u) | (fd << 6u) | 0x22u;
        };
        install_vu0_pair(&imm_vu, 0, 0x40000000u, addi(2) | 0x80000000u);
        install_vu0_pair(&imm_vu, 1, 0xdeadbeefu, addi(3) | 0x80000000u);
        install_vu0_pair(&imm_vu, 2, 0x7fffffffu,
            make_vu0_upper_nop(0xc0000000u)); // I and E
        install_vu0_pair(&imm_vu, 3, 0x8000033cu, make_vu0_upper_nop());
        const auto result = ee::ee_vu0_execute_ialu_callback(&engine, &imm_cpu,
            &imm_vu, 64u, ee::EE_VU0_EXECUTE_STOP_AT_E, 0u);
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              imm_vu.m_micro_pc == 32u && imm_vu.m_branch == 0u &&
              imm_vu.m_running == 0u && imm_vu.m_cycle == 6u,
              "I-bit literals never execute as lower branch or other instructions");
        check(imm_cpu.m_vu0[2].m_lo == 0x40400000u &&
              imm_cpu.m_vu0[3].m_lo == 0x40800000u &&
              imm_vu.m_vi[21] == 0x7fffffffu,
              "upper ADDi reads previous I and commits exact new literal bits");
    }

    // Non-flag-writing upper operations reserve VF lanes but preserve MAC/SF.
    {
        ee::s_ee_cpu_state unary_cpu{};
        ee::s_ee_vu0_state unary_vu{};
        ee::s_ee_vu0_ialu_engine engine{};
        unary_vu.m_vpu_status = 1u;
        unary_vu.m_mac_flags = 0x1234u;
        unary_vu.m_status_flags = 0x345u;
        unary_cpu.m_vu0[1].m_lo = 0x80000000bfc00000ull; // -0, -1.5
        unary_cpu.m_vu0[2].m_lo = 0xdeadbeef00000000ull;
        unary_cpu.m_vu0[6].m_lo = 0x7fffffff00000000ull;
        const auto unary = [](uint32_t code, uint32_t ft, uint32_t fs, uint32_t mask)
        {
            return (mask << 21u) | (ft << 16u) | (fs << 11u) |
                ((code >> 2u) << 6u) | (0x3cu | (code & 3u));
        };
        const auto binary = [](uint32_t code, uint32_t fd, uint32_t fs, uint32_t ft, uint32_t mask)
        {
            return (mask << 21u) | (ft << 16u) | (fs << 11u) | (fd << 6u) | code;
        };
        const uint32_t nop = 0x8000033cu;
        install_vu0_pair(&unary_vu, 0, nop, unary(0x1d, 2, 1, 8)); // ABS.x
        install_vu0_pair(&unary_vu, 1, nop, unary(0x15, 3, 2, 8)); // FTOI4.x
        install_vu0_pair(&unary_vu, 2, nop, unary(0x11, 4, 3, 8)); // ITOF4.x
        install_vu0_pair(&unary_vu, 3, nop, binary(0x2b, 5, 4, 1, 8)); // MAX.x
        install_vu0_pair(&unary_vu, 4, nop, binary(0x2f, 6, 5, 1, 8)); // MINI.x
        install_vu0_pair(&unary_vu, 5, nop, make_vu0_upper_nop(0x40000000u));
        install_vu0_pair(&unary_vu, 6, nop, make_vu0_upper_nop());
        const auto result = ee::ee_vu0_execute_ialu_callback(&engine, &unary_cpu,
            &unary_vu, 64u, ee::EE_VU0_EXECUTE_STOP_AT_E, 0u);
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              unary_vu.m_running == 0u && unary_vu.m_cycle == 21u,
              "non-flag FMAC chain has four-cycle VF interlocks");
        check(unary_cpu.m_vu0[2].m_lo == 0xdeadbeef3fc00000ull &&
              unary_cpu.m_vu0[3].m_lo == 24u &&
              unary_cpu.m_vu0[4].m_lo == 0x3fc00000u &&
              unary_cpu.m_vu0[5].m_lo == 0x3fc00000u &&
              unary_cpu.m_vu0[6].m_lo == 0x7fffffffbfc00000ull,
              "ABS/FTOI/ITOF/MAX/MINI use correct destinations and preserve masked lanes");
        check(unary_vu.m_mac_flags == 0x1234u && unary_vu.m_status_flags == 0x345u,
              "non-flag FMAC does not clear or alter existing flags");
    }

    // OPMULA/OPMSUB always operate on XYZ, with W left untouched.
    {
        ee::s_ee_cpu_state outer_cpu{};
        ee::s_ee_vu0_state outer_vu{};
        ee::s_ee_vu0_ialu_engine engine{};
        outer_vu.m_vpu_status = 1u;
        outer_cpu.m_vu0[1] = {0x4040000040000000ull, 0x3f80000040a00000ull};
        outer_cpu.m_vu0[2] = {0x4130000040e00000ull, 0x3f80000041500000ull};
        outer_cpu.m_vu0[3].m_hi = 0xdeadbeef00000000ull;
        outer_vu.m_acc.m_hi = 0x1234567800000000ull;
        outer_vu.m_mac_flags = 1u;
        const uint32_t nop = 0x8000033cu;
        const uint32_t opmula = (2u << 16u) | (1u << 11u) | (11u << 6u) | 0x3eu;
        const uint32_t opmsub = (1u << 16u) | (2u << 11u) | (3u << 6u) | 0x2eu;
        install_vu0_pair(&outer_vu, 0, nop, opmula);
        install_vu0_pair(&outer_vu, 1, nop, opmsub | 0x40000000u);
        install_vu0_pair(&outer_vu, 2, nop, make_vu0_upper_nop());
        const auto result = ee::ee_vu0_execute_ialu_callback(&engine, &outer_cpu,
            &outer_vu, 64u, ee::EE_VU0_EXECUTE_STOP_AT_E, 0u);
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              outer_vu.m_cycle == 6u && outer_vu.m_running == 0u,
              "outer product pair executes with ACC forwarding");
        check(outer_vu.m_acc.m_lo == 0x420c0000421c0000ull &&
              outer_vu.m_acc.m_hi == 0x1234567841b00000ull &&
              outer_cpu.m_vu0[3].m_lo == 0x41100000c1800000ull &&
              outer_cpu.m_vu0[3].m_hi == 0xdeadbeef3f800000ull,
              "OPMULA and OPMSUB rotated XYZ operands and preserved W");
        check(outer_vu.m_mac_flags == 0x81u,
              "outer product preserves W MAC bits");
    }

    // Lower MOVE/MR32/MTIR/MFIR operate on raw VF words and VI halfwords.
    {
        ee::s_ee_cpu_state transfer_cpu{};
        ee::s_ee_vu0_state transfer_vu{};
        ee::s_ee_vu0_ialu_engine engine{};
        transfer_vu.m_vpu_status = 1u;
        transfer_vu.m_vi[1] = 0x12340000u;
        transfer_cpu.m_vu0[1] = {0x2222800133331234ull, 0x4444444455555555ull};
        transfer_cpu.m_vu0[3].m_hi = 0xdeadbeefcafebabeull;
        const auto lower = [](uint32_t table, uint32_t index, uint32_t ft,
                              uint32_t fs, uint32_t mask)
        {
            return 0x80000000u | (mask << 21u) | (ft << 16u) |
                (fs << 11u) | (index << 6u) | (0x3cu | table);
        };
        install_vu0_pair(&transfer_vu, 0, lower(0, 12, 2, 1, 15), make_vu0_upper_nop());
        install_vu0_pair(&transfer_vu, 1, lower(1, 12, 2, 2, 15), make_vu0_upper_nop());
        install_vu0_pair(&transfer_vu, 2, lower(0, 15, 1, 2, 0), make_vu0_upper_nop());
        install_vu0_pair(&transfer_vu, 3, lower(1, 15, 3, 1, 10), make_vu0_upper_nop());
        install_vu0_pair(&transfer_vu, 4, lower(0, 12, 0, 3, 15),
            make_vu0_upper_nop(0x40000000u));
        install_vu0_pair(&transfer_vu, 5, 0x8000033cu, make_vu0_upper_nop());
        const auto result = ee::ee_vu0_execute_ialu_callback(&engine, &transfer_cpu,
            &transfer_vu, 64u, ee::EE_VU0_EXECUTE_STOP_AT_E, 0u);
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              transfer_vu.m_running == 0u && transfer_vu.m_cycle == 14u,
              "lower transfer lane dependencies and E drain");
        check(transfer_cpu.m_vu0[2].m_lo == 0x5555555522228001ull &&
              transfer_cpu.m_vu0[2].m_hi == 0x3333123444444444ull &&
              transfer_vu.m_vi[1] == 0x12348001u &&
              transfer_cpu.m_vu0[3].m_lo == 0x00000000ffff8001ull &&
              transfer_cpu.m_vu0[3].m_hi == 0xdeadbeefffff8001ull &&
              transfer_cpu.m_vu0[0].m_lo == 0u && transfer_cpu.m_vu0[0].m_hi == 0u,
              "lower MOVE/MR32 aliasing, MTIR low half and MFIR sign extension");
    }

    // Micro FDIV publishes Q and I/D together, with WAITQ and E drain.
    {
        ee::s_ee_cpu_state scalar_cpu{};
        ee::s_ee_vu0_state scalar_vu{};
        ee::s_ee_vu0_ialu_engine engine{};
        scalar_vu.m_vpu_status = 1u;
        scalar_vu.m_q.m_lo = 0x3f800000u;
        scalar_vu.m_vi[22] = 0x3f800000u;
        scalar_cpu.m_vu0[1].m_lo = 0x41000000u; // 8
        scalar_cpu.m_vu0[2].m_lo = 0x40800000u; // 4
        const auto scalar = [](uint32_t op, uint32_t fs, uint32_t ft)
        {
            return 0x80000000u | (ft << 16u) | (fs << 11u) |
                (14u << 6u) | (0x3cu | op);
        };
        install_vu0_pair(&scalar_vu, 0, scalar(0, 1, 2), make_vu0_upper_nop());
        install_vu0_pair(&scalar_vu, 1, scalar(3, 0, 0), make_vu0_upper_nop());
        install_vu0_pair(&scalar_vu, 2, scalar(1, 0, 2), make_vu0_upper_nop());
        install_vu0_pair(&scalar_vu, 3, scalar(2, 1, 2), make_vu0_upper_nop());
        install_vu0_pair(&scalar_vu, 4, scalar(0, 1, 0),
            make_vu0_upper_nop(0x40000000u)); // divide by zero
        install_vu0_pair(&scalar_vu, 5, 0x8000033cu, make_vu0_upper_nop());
        const auto step = [&]()
        {
            return ee::ee_vu0_execute_ialu_callback(&engine, &scalar_cpu,
                &scalar_vu, 1u, ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0u);
        };
        const auto complete = static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);
        check(step() == complete && scalar_vu.m_q_pending == 2u &&
              scalar_vu.m_cycle == 1u && scalar_vu.m_vi[22] == 0x3f800000u,
              "DIV leaves old Q visible until retirement");
        check(step() == complete && scalar_vu.m_cycle == 8u &&
              scalar_vu.m_vi[22] == 0x40000000u,
              "WAITQ retires DIV at seven cycles");
        check(step() == complete && scalar_vu.m_cycle == 9u &&
              scalar_vu.m_q_ready_cycle == 16u,
              "SQRT schedules seven-cycle Q");
        check(step() == complete && scalar_vu.m_cycle == 16u &&
              scalar_vu.m_q_ready_cycle == 29u && scalar_vu.m_vi[22] == 0x40000000u,
              "RSQRT serializes prior SQRT");
        check(step() == complete && scalar_vu.m_cycle == 29u &&
              scalar_vu.m_vi[22] == 0x40800000u &&
              (scalar_vu.m_status_flags & 0x30u) == 0u,
              "RSQRT retires before new DIV, flags remain old");
        check(step() == complete && scalar_vu.m_cycle == 36u &&
              scalar_vu.m_q_pending == 0u && scalar_vu.m_running == 0u &&
              scalar_vu.m_vi[22] == 0x7fffffffu &&
              (scalar_vu.m_status_flags & 0x830u) == 0x820u,
              "E drains divide-by-zero Q and D/DS flags");
    }

    // Upper Q consumers do not wait unless the lower half interlocks FDIV.
    {
        ee::s_ee_cpu_state q_cpu{};
        ee::s_ee_vu0_state q_vu{};
        ee::s_ee_vu0_ialu_engine engine{};
        q_vu.m_vpu_status = 1u;
        q_vu.m_vi[22] = 0x3f800000u;
        q_vu.m_q.m_lo = 0x3f800000u;
        q_cpu.m_vu0[1].m_lo = 0x41000000u; // 8
        q_cpu.m_vu0[2].m_lo = 0x40800000u; // 4
        q_cpu.m_vu0[3].m_hi = 0x12345678abcdef01ull;
        const auto upper_q = [](uint32_t op, uint32_t fd, bool acc)
        {
            const uint32_t base = (8u << 21u) | (1u << 11u);
            return acc ? base | ((op >> 2u) << 6u) | (0x3cu | (op & 3u)) :
                base | (fd << 6u) | op;
        };
        const uint32_t nop = 0x8000033cu;
        const uint32_t div = 0x80000000u | (2u << 16u) | (1u << 11u) |
            (14u << 6u) | 0x3cu;
        const uint32_t waitq = 0x80000000u | (14u << 6u) | 0x3fu;
        install_vu0_pair(&q_vu, 0, div, upper_q(0x20, 3, false)); // 8+1
        install_vu0_pair(&q_vu, 1, nop, upper_q(0x1c, 4, false)); // 8*1
        install_vu0_pair(&q_vu, 2, waitq, upper_q(0x24, 5, false)); // 8-2
        install_vu0_pair(&q_vu, 3, nop, upper_q(0x20, 0, true)); // ACC=10
        install_vu0_pair(&q_vu, 4, nop, upper_q(0x24, 0, true)); // ACC=6
        install_vu0_pair(&q_vu, 5, nop, upper_q(0x1c, 0, true)); // ACC=16
        install_vu0_pair(&q_vu, 6, nop, upper_q(0x21, 0, true)); // ACC=32
        install_vu0_pair(&q_vu, 7, nop, upper_q(0x25, 0, true)); // ACC=16
        install_vu0_pair(&q_vu, 8, nop, upper_q(0x21, 6, false)); // 32
        install_vu0_pair(&q_vu, 9, nop, upper_q(0x25, 7, false) | 0x40000000u); // 0
        install_vu0_pair(&q_vu, 10, nop, make_vu0_upper_nop());
        const auto result = ee::ee_vu0_execute_ialu_callback(&engine, &q_cpu,
            &q_vu, 64u, ee::EE_VU0_EXECUTE_STOP_AT_E, 0u);
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              q_vu.m_cycle == 19u && q_vu.m_running == 0u,
              "Q consumers do not stall before WAITQ and E drains FMAC");
        check(q_cpu.m_vu0[3].m_lo == 0x41100000u &&
              q_cpu.m_vu0[3].m_hi == 0x12345678abcdef01ull &&
              q_cpu.m_vu0[4].m_lo == 0x41000000u &&
              q_cpu.m_vu0[5].m_lo == 0x40c00000u &&
              q_vu.m_acc.m_lo == 0x41800000u &&
              q_cpu.m_vu0[6].m_lo == 0x42000000u &&
              q_cpu.m_vu0[7].m_lo == 0u,
              "all ten upper Q variants use the visible scalar and preserve masked lanes");
    }

    // VU0 microcode is a 4 KB byte-addressed store of paired
    // lower/upper 32-bit words. The upper word carries I/E/M/D/T flags.
    {
        ee::s_ee_vu0_state micro_state{};
        ee::s_ee_vu0_micro_instruction instruction{};

        write_le32(&micro_state.m_micro_memory[0x20], 0x11223344u);
        write_le32(&micro_state.m_micro_memory[0x24], 0xf8001234u);
        micro_state.m_micro_pc = 0x20u;

        check(
            ee::ee_vu0_fetch_micro_instruction(&micro_state, &instruction) ==
                static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE),
            "VU0 micro fetch succeeds for aligned byte PC"
        );
        check(instruction.m_pc == 0x20u &&
              instruction.m_lower == 0x11223344u &&
              instruction.m_upper == 0xf8001234u,
              "VU0 micro fetch preserves lower/upper word order");
        check(instruction.m_flags ==
              (ee::EE_VU0_MICRO_FLAG_I | ee::EE_VU0_MICRO_FLAG_E |
               ee::EE_VU0_MICRO_FLAG_M | ee::EE_VU0_MICRO_FLAG_D |
               ee::EE_VU0_MICRO_FLAG_T),
              "VU0 micro fetch decodes I/E/M/D/T flag bits");
        check(instruction.m_lower_fields.m_opcode == 8 &&
              instruction.m_lower_fields.m_ft == 2 &&
              instruction.m_lower_fields.m_fs == 6 &&
              instruction.m_lower_fields.m_fd == 13 &&
              instruction.m_lower_fields.m_it == 2 &&
              instruction.m_lower_fields.m_is == 6 &&
              instruction.m_lower_fields.m_id == 13 &&
              instruction.m_lower_fields.m_xyzw == 9,
              "VU0 micro fetch extracts lower primary opcode and operands");
        check(instruction.m_upper_fields.m_opcode == 52 &&
              instruction.m_upper_fields.m_ft == 0 &&
              instruction.m_upper_fields.m_fs == 2 &&
              instruction.m_upper_fields.m_fd == 8 &&
              instruction.m_upper_fields.m_it == 0 &&
              instruction.m_upper_fields.m_is == 2 &&
              instruction.m_upper_fields.m_id == 8 &&
              instruction.m_upper_fields.m_xyzw == 0,
              "VU0 micro fetch extracts upper primary opcode and operands");
        check(
            instruction.m_lower_fields.m_operation ==
                ee::e_ee_vu0_micro_opcode::IADDIU,
            "VU0 lower primary table classifies IADDIU"
        );
        check(
            instruction.m_upper_fields.m_operation ==
                ee::e_ee_vu0_micro_opcode::UNKNOWN,
            "VU0 upper primary table marks an unassigned opcode as unknown"
        );
        check(micro_state.m_micro_pc == 0x28u,
              "VU0 micro fetch advances PC by one 8-byte instruction");

        // Lower primary 0x40 selects the lower-op selector table;
        // sub-op 0x3c within that table selects T3_00.
        const uint32_t lower_iadd = (0x40u << 25) | 0x30u;
        write_le32(&micro_state.m_micro_memory[0], lower_iadd);
        write_le32(&micro_state.m_micro_memory[4], 0);
        micro_state.m_micro_pc = 0;
        check(
            ee::ee_vu0_fetch_micro_instruction(&micro_state, &instruction) ==
                static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE) &&
            instruction.m_lower_fields.m_opcode == 0x40u &&
            instruction.m_lower_fields.m_operation ==
                ee::e_ee_vu0_micro_opcode::IADD,
            "VU0 lower1 selector and secondary table classify IADD"
        );

        const uint32_t lower_lqi =
            (0x40u << 25) | (13u << 6) | 0x3cu;
        write_le32(&micro_state.m_micro_memory[0], lower_lqi);
        micro_state.m_micro_pc = 0;
        check(
            ee::ee_vu0_fetch_micro_instruction(&micro_state, &instruction) ==
                static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE) &&
            instruction.m_lower_fields.m_opcode == 0x40u &&
            instruction.m_lower_fields.m_operation ==
                ee::e_ee_vu0_micro_opcode::LQI,
            "VU0 lower1 T3_00 table classifies LQI"
        );

        write_le32(&micro_state.m_micro_memory[0], 0x7fu << 25);
        micro_state.m_micro_pc = 0;
        check(
            ee::ee_vu0_fetch_micro_instruction(&micro_state, &instruction) ==
                static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE) &&
            instruction.m_lower_fields.m_operation ==
                ee::e_ee_vu0_micro_opcode::UNKNOWN,
            "VU0 lower opcode decoder rejects unassigned primary slot"
        );

        const uint32_t upper_add_x = 0;
        const uint32_t upper_nop = (11u << 6) | 0x3fu;
        write_le32(&micro_state.m_micro_memory[0], 0);
        write_le32(&micro_state.m_micro_memory[4], upper_add_x);
        micro_state.m_micro_pc = 0;
        check(
            ee::ee_vu0_fetch_micro_instruction(&micro_state, &instruction) ==
                static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE) &&
            instruction.m_upper_fields.m_operation ==
                ee::e_ee_vu0_micro_opcode::ADD_X,
            "VU0 upper primary table classifies ADDx"
        );

        write_le32(&micro_state.m_micro_memory[4], 0x22u);
        micro_state.m_micro_pc = 0;
        check(
            ee::ee_vu0_fetch_micro_instruction(&micro_state, &instruction) ==
                static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE) &&
            instruction.m_upper_fields.m_operation ==
                ee::e_ee_vu0_micro_opcode::ADD_I,
            "VU0 upper primary table classifies ADDi"
        );

        write_le32(&micro_state.m_micro_memory[4], upper_nop);
        micro_state.m_micro_pc = 0;
        check(
            ee::ee_vu0_fetch_micro_instruction(&micro_state, &instruction) ==
                static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE) &&
            instruction.m_upper_fields.m_operation ==
                ee::e_ee_vu0_micro_opcode::NOP,
            "VU0 upper FD secondary table classifies NOP"
        );
        write_le32(&micro_state.m_micro_memory[0xff8], 0xaabbccddu);
        write_le32(&micro_state.m_micro_memory[0xffc], 0x40000000u);
        micro_state.m_micro_pc = 0xff8u;
        check(
            ee::ee_vu0_fetch_micro_instruction(&micro_state, &instruction) ==
                static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE),
            "VU0 micro fetch succeeds at final instruction slot"
        );
        check(instruction.m_pc == 0xff8u &&
              instruction.m_lower == 0xaabbccddu &&
              instruction.m_upper == 0x40000000u &&
              instruction.m_flags == ee::EE_VU0_MICRO_FLAG_E,
              "VU0 micro fetch decodes the final program-memory pair");
        check(micro_state.m_micro_pc == 0,
              "VU0 micro PC wraps at the 4 KB program boundary");

        micro_state.m_micro_pc = 0x21u;
        const ee::s_ee_vu0_micro_instruction sentinel{
            0xdeadbeefu, 0x12345678u, 0x87654321u, 0x55u
        };
        instruction = sentinel;
        check(
            ee::ee_vu0_fetch_micro_instruction(&micro_state, &instruction) ==
                static_cast<uint32_t>(ee::e_ee_vu0_sync_result::INVALID_ARGUMENT),
            "VU0 micro fetch rejects an unaligned PC"
        );
        check(micro_state.m_micro_pc == 0x21u &&
              instruction.m_pc == sentinel.m_pc &&
              instruction.m_lower == sentinel.m_lower &&
              instruction.m_upper == sentinel.m_upper &&
              instruction.m_flags == sentinel.m_flags,
              "invalid VU0 micro fetch leaves PC and output unchanged");
        check(
            ee::ee_vu0_fetch_micro_instruction(nullptr, &instruction) ==
                static_cast<uint32_t>(ee::e_ee_vu0_sync_result::INVALID_ARGUMENT),
            "VU0 micro fetch rejects missing state"
        );
        check(
            ee::ee_vu0_fetch_micro_instruction(&micro_state, nullptr) ==
                static_cast<uint32_t>(ee::e_ee_vu0_sync_result::INVALID_ARGUMENT),
            "VU0 micro fetch rejects missing output"
        );
    }

    check(
        ee::ee_vu0_transfer_sync_mode(
            e_ee_vu0_transfer_direction::FROM_VU0, false
        ) == e_ee_vu0_sync_mode::CATCH_UP_TO_EE_CYCLE,
        "read transfer outside delay slot catches VU0 up to EE cycle"
    );
    check(
        ee::ee_vu0_transfer_sync_mode(
            e_ee_vu0_transfer_direction::TO_VU0, false
        ) == e_ee_vu0_sync_mode::CATCH_UP_TO_EE_CYCLE,
        "write transfer outside delay slot catches VU0 up to EE cycle"
    );
    check(
        ee::ee_vu0_transfer_sync_mode(
            e_ee_vu0_transfer_direction::FROM_VU0, true
        ) == e_ee_vu0_sync_mode::FINISH_TO_E,
        "read transfer in delay slot finishes to E"
    );
    check(
        ee::ee_vu0_transfer_sync_mode(
            e_ee_vu0_transfer_direction::TO_VU0, true
        ) == e_ee_vu0_sync_mode::WAIT_TO_E_OR_M,
        "write transfer in delay slot waits to E or M"
    );

    vu0.m_vi[0] = 0xffffffffu;
    vu0.m_vi[7] = 0x81234567u;
    check(ee::ee_vu0_read_vi(&vu0, 0) == 0,
          "VI0 reads as the hardwired zero register");
    check(ee::ee_vu0_read_vi(&vu0, 7) == 0x81234567u,
          "VI register read returns the stored word");
    ee::ee_vu0_write_vi(&vu0, 0, 0x12345678u);
    ee::ee_vu0_write_vi(&vu0, 7, 0x76543210u);
    check(vu0.m_vi[0] == 0xffffffffu,
          "VI0 writes are ignored");
    check(ee::ee_vu0_read_vi(&vu0, 7) == 0x76543210u,
          "VI register write updates the selected register");
    check(ee::ee_vu0_read_vi(&vu0, 32) == 0,
          "out-of-range VI read fails closed");
    ee::ee_vu0_write_vi(&vu0, 32, 0xdeadbeefu);
    check(vu0.m_vi[7] == 0x76543210u,
          "out-of-range VI write is ignored");

    ee::s_ee_runtime runtime{};
    runtime.m_vu0_state = &vu0;
    runtime.m_vu0_user = &scheduler;
    runtime.m_vu0_sync = mock_vu0_sync;
    s_mock_control control{};
    runtime.m_vu0_control_user = &control;
    runtime.m_vu0_control = mock_vu0_control;

    const uint32_t modes[] = {
        static_cast<uint32_t>(e_ee_vu0_sync_mode::CATCH_UP_TO_EE_CYCLE),
        static_cast<uint32_t>(e_ee_vu0_sync_mode::FINISH_TO_E),
        static_cast<uint32_t>(e_ee_vu0_sync_mode::WAIT_TO_E_OR_M)
    };
    for (uint32_t i = 0; i < 3; ++i)
    {
        const uint32_t pc = 0x00100000u + (i * 4);
        const uint32_t result = ee::ee_vu0_sync(&cpu, &runtime, modes[i], pc);
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
              "scheduler completion propagates");
        check(scheduler.m_last_mode == modes[i],
              "scheduler receives requested synchronization mode");
        check(scheduler.m_last_pc == pc,
              "scheduler receives the guest instruction PC");
        check(scheduler.m_last_cpu == &cpu && scheduler.m_last_vu0 == &vu0,
              "scheduler receives the original CPU and VU0 state");
    }
    check(scheduler.m_calls == 3, "all three synchronization modes dispatched");
    check(vu0.m_cycle == 21, "scheduler updates the VU0 cycle state");
    check(cpu.m_pc == 0x00100008u, "scheduler may update the EE state");

    scheduler.m_result = static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
    check(
        ee::ee_vu0_sync(&cpu, &runtime, modes[0], 0x0010000cu) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED),
        "scheduler failure propagates"
    );
    scheduler.m_result = 0xffffffffu;
    check(
        ee::ee_vu0_sync(&cpu, &runtime, modes[0], 0x00100010u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED),
        "unknown scheduler result is converted to failure"
    );

    ee::s_ee_runtime missing_callback{};
    missing_callback.m_vu0_state = &vu0;
    check(
        ee::ee_vu0_sync(&cpu, &missing_callback, modes[0], 0) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE),
        "missing scheduler fails closed"
    );

    ee::s_ee_runtime missing_state{};
    missing_state.m_vu0_sync = mock_vu0_sync;
    missing_state.m_vu0_user = &scheduler;
    check(
        ee::ee_vu0_sync(&cpu, &missing_state, modes[0], 0) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE),
        "missing VU0 state fails closed"
    );
    check(
        ee::ee_vu0_sync(nullptr, &runtime, modes[0], 0) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT),
        "null CPU is rejected"
    );
    check(
        ee::ee_vu0_sync(&cpu, nullptr, modes[0], 0) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT),
        "null runtime is rejected"
    );
    check(
        ee::ee_vu0_sync(&cpu, &runtime, 3, 0) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT),
        "unknown synchronization mode is rejected"
    );

    scheduler.m_result = static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);

    // QMFC2 copies both halves of VF[fs] after synchronization.
    cpu.m_vu0[3] = {0x0123456789abcdefULL, 0xfedcba9876543210ULL};
    cpu.m_gpr[9] = {};
    const uint32_t read_mode = static_cast<uint32_t>(
        ee::ee_vu0_transfer_sync_mode(e_ee_vu0_transfer_direction::FROM_VU0, false)
    );
    check(
        ee::ee_vu0_qmfc2(&cpu, &runtime, 9, 3, read_mode, 0x00100020u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "QMFC2 succeeds after scheduler synchronization"
    );
    check(cpu.m_gpr[9].m_lo == cpu.m_vu0[3].m_lo &&
          cpu.m_gpr[9].m_hi == cpu.m_vu0[3].m_hi,
          "QMFC2 copies both 64-bit halves");
    const ee::s_ee_u128 zero_gpr_sentinel{0x1111222233334444ULL, 0x5555666677778888ULL};
    cpu.m_gpr[0] = zero_gpr_sentinel;
    check(
        ee::ee_vu0_qmfc2(&cpu, &runtime, 0, 3, read_mode, 0x00100024u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "QMFC2 to GPR0 still performs synchronization"
    );
    check(cpu.m_gpr[0].m_lo == zero_gpr_sentinel.m_lo &&
          cpu.m_gpr[0].m_hi == zero_gpr_sentinel.m_hi,
          "QMFC2 never changes GPR0");

    // QMTC2 copies all 128 source bits. GPR0 reads as zero and VF0 is
    // protected from writes; synchronization still happens in both cases.
    cpu.m_gpr[8] = {0x8877665544332211ULL, 0x0123456789abcdefULL};
    const uint32_t write_mode = static_cast<uint32_t>(
        ee::ee_vu0_transfer_sync_mode(e_ee_vu0_transfer_direction::TO_VU0, false)
    );
    check(
        ee::ee_vu0_qmtc2(&cpu, &runtime, 8, 4, write_mode, 0x00100028u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "QMTC2 succeeds after scheduler synchronization"
    );
    check(cpu.m_vu0[4].m_lo == cpu.m_gpr[8].m_lo &&
          cpu.m_vu0[4].m_hi == cpu.m_gpr[8].m_hi,
          "QMTC2 copies both 64-bit halves");
    cpu.m_vu0[5] = {0xaaaaaaaa55555555ULL, 0xcccccccc33333333ULL};
    check(
        ee::ee_vu0_qmtc2(&cpu, &runtime, 0, 5, write_mode, 0x0010002cu) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "QMTC2 from GPR0 succeeds"
    );
    check(cpu.m_vu0[5].m_lo == 0 && cpu.m_vu0[5].m_hi == 0,
          "QMTC2 treats GPR0 as zero");
    const ee::s_ee_u128 zero_vf_sentinel{0xdeadbeef01234567ULL, 0x89abcdef76543210ULL};
    cpu.m_vu0[0] = zero_vf_sentinel;
    check(
        ee::ee_vu0_qmtc2(&cpu, &runtime, 8, 0, write_mode, 0x00100030u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "QMTC2 to VF0 still performs synchronization"
    );
    check(cpu.m_vu0[0].m_lo == zero_vf_sentinel.m_lo &&
          cpu.m_vu0[0].m_hi == zero_vf_sentinel.m_hi,
          "QMTC2 never changes VF0");
    cpu.m_gpr[13] = {};
    check(
        ee::ee_vu0_qmfc2(&cpu, &runtime, 13, 0, read_mode, 0x00100032u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "QMFC2 from VF0 succeeds"
    );
    check(cpu.m_gpr[13].m_lo == 0 &&
          cpu.m_gpr[13].m_hi == 0x3f80000000000000ULL,
          "QMFC2 reads VF0 as (0, 0, 0, 1.0f)");

    // CFC2 sign-extends ordinary VI words; R only replaces UL[0].
    vu0.m_vi[6] = 0x81234567u;
    cpu.m_gpr[10].m_hi = 0x123456789abcdef0ULL;
    check(
        ee::ee_vu0_cfc2(&cpu, &runtime, 10, 6, read_mode, 0x00100034u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "CFC2 succeeds after scheduler synchronization"
    );
    check(cpu.m_gpr[10].m_lo == 0xffffffff81234567ULL,
          "CFC2 sign-extends ordinary VI values");
    check(cpu.m_gpr[10].m_hi == 0x123456789abcdef0ULL,
          "CFC2 preserves the upper 64-bit GPR half");
    vu0.m_vi[20] = 0xffabcdefu;
    cpu.m_gpr[11].m_lo = 0xfeedfacecafebeefULL;
    check(
        ee::ee_vu0_cfc2(&cpu, &runtime, 11, 20, read_mode, 0x00100038u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "CFC2 special R read succeeds"
    );
    check(cpu.m_gpr[11].m_lo == 0xfeedface002bcdefULL,
          "CFC2 R masks UL[0] and preserves UL[1]");
    cpu.m_gpr[0] = zero_gpr_sentinel;
    check(
        ee::ee_vu0_cfc2(&cpu, &runtime, 0, 6, read_mode, 0x0010003cu) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "CFC2 to GPR0 still performs synchronization"
    );
    check(cpu.m_gpr[0].m_lo == zero_gpr_sentinel.m_lo &&
          cpu.m_gpr[0].m_hi == zero_gpr_sentinel.m_hi,
          "CFC2 never changes GPR0");

    // CFC2 reads the dedicated flag/control fields, not stale VI array slots.
    vu0.m_mac_flags = 0x12345678u;
    vu0.m_clip_flags = 0x87654321u;
    vu0.m_status_flags = 0x0badc0deu;
    vu0.m_vpu_status = 0x00000005u;
    vu0.m_tpc = 0x00001234u;
    check(ee::ee_vu0_read_vi(&vu0, 17) == 0x12345678u,
          "CFC2 MAC flag reads tracked MAC flags");
    check(ee::ee_vu0_read_vi(&vu0, 18) == 0x87654321u,
          "CFC2 clip flag reads tracked clip flags");
    check(ee::ee_vu0_read_vi(&vu0, 16) == 0x0badc0deu,
          "CFC2 status flag reads tracked status flags");
    check(ee::ee_vu0_read_vi(&vu0, 29) == 0x00000005u,
          "CFC2 VPU status reads tracked VPU status");
    check(ee::ee_vu0_read_vi(&vu0, 26) == 0x00001234u,
          "CFC2 TPC reads tracked micro-PC");

    // Ordinary CTC2 writes VI, while the dedicated special cases are
    // implemented explicitly and the three read-only registers ignore writes.
    cpu.m_gpr[8].m_lo = 0x89abcdefu;
    check(
        ee::ee_vu0_ctc2(&cpu, &runtime, 8, 7, write_mode, 0x00100048u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "CTC2 ordinary VI write succeeds"
    );
    check(ee::ee_vu0_read_vi(&vu0, 7) == 0x89abcdefu,
          "CTC2 writes the selected VI register");
    vu0.m_mac_flags = 0x12345678u;
    cpu.m_gpr[8].m_lo = 0xffffffffu;
    check(
        ee::ee_vu0_ctc2(&cpu, &runtime, 8, 17, write_mode, 0x0010004cu) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "CTC2 MAC_FLAG read-only write is ignored"
    );
    check(vu0.m_mac_flags == 0x12345678u,
          "CTC2 cannot modify MAC_FLAG");
    vu0.m_tpc = 0x1234u;
    check(
        ee::ee_vu0_ctc2(&cpu, &runtime, 8, 26, write_mode, 0x00100050u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "CTC2 TPC read-only write is ignored"
    );
    check(vu0.m_tpc == 0x1234u, "CTC2 cannot modify TPC");
    vu0.m_vpu_status = 5u;
    check(
        ee::ee_vu0_ctc2(&cpu, &runtime, 8, 29, write_mode, 0x00100054u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "CTC2 VPU_STAT read-only write is ignored"
    );
    check(vu0.m_vpu_status == 5u, "CTC2 cannot modify VPU_STAT");

    cpu.m_gpr[8].m_lo = 0xabcdef01u;
    check(
        ee::ee_vu0_ctc2(&cpu, &runtime, 8, 20, write_mode, 0x00100058u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "CTC2 R write succeeds"
    );
    check(ee::ee_vu0_read_vi(&vu0, 20) == ((0xabcdef01u & 0x007fffffu) | 0x3f800000u),
          "CTC2 R preserves the implicit 1.0 exponent");

    cpu.m_gpr[8].m_lo = 0x12345678u;
    check(
        ee::ee_vu0_ctc2(&cpu, &runtime, 8, 18, write_mode, 0x0010005cu) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "CTC2 CLIP_FLAG write succeeds"
    );
    check(vu0.m_clip_flags == 0x12345678u &&
          ee::ee_vu0_read_vi(&vu0, 18) == 0x12345678u,
          "CTC2 CLIP_FLAG updates VI and the clip flag");

    cpu.m_gpr[8].m_lo = 0x00000c0fu;
    const uint32_t control_calls_before_fbrst = control.m_calls;
    check(
        ee::ee_vu0_ctc2(&cpu, &runtime, 8, 28, write_mode, 0x00100060u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "CTC2 FBRST host action succeeds"
    );
    check(control.m_calls == control_calls_before_fbrst + 1 &&
          control.m_last_register == 28 && control.m_last_value == 0x00000c0fu,
          "CTC2 FBRST forwards reset/break bits to the host");
    check(ee::ee_vu0_read_vi(&vu0, 28) == 0x00000c00u,
          "CTC2 FBRST reset clears only the affected unit debug enables");
    cpu.m_gpr[8].m_lo = 0x0000030fu;
    check(ee::ee_vu0_ctc2(&cpu, &runtime, 8, 28, write_mode, 0x00100060u) ==
          static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
          ee::ee_vu0_read_vi(&vu0, 28) == 0u &&
          (vu0.m_vpu_status & 0xffffu) == 0u,
          "both resets dominate force break and clear their debug status");
    cpu.m_gpr[8].m_lo = 0x00000001u;
    check(ee::ee_vu0_ctc2(&cpu, &runtime, 8, 28, write_mode, 0x00100060u) ==
          static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
          (vu0.m_vpu_status & 0xfu) == 8u && vu0.m_running == 0u,
          "VU0 force break sets its own debug stop status");
    cpu.m_gpr[8].m_lo = 0x00000002u;
    check(ee::ee_vu0_ctc2(&cpu, &runtime, 8, 28, write_mode, 0x00100060u) ==
          static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
          (vu0.m_vpu_status & 0xffu) == 0u,
          "VU0 reset exits debug stop");

    cpu.m_gpr[8].m_lo = 0x12345678u;
    const uint32_t control_calls_before_cmsar = control.m_calls;
    check(
        ee::ee_vu0_ctc2(&cpu, &runtime, 8, 31, write_mode, 0x00100064u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
        "CTC2 CMSAR1 host action succeeds"
    );
    check(control.m_calls == control_calls_before_cmsar + 1 &&
          control.m_last_register == 31 && control.m_last_value == 0x5678u,
          "CTC2 CMSAR1 forwards its low 16-bit micro entry address");

    // Side-effectful writes fail closed without a host action callback.
    ee::s_ee_runtime no_control = runtime;
    no_control.m_vu0_control = nullptr;
    const uint32_t old_fbrst = vu0.m_fbrst;
    cpu.m_gpr[8].m_lo = 0x00000001u;
    check(
        ee::ee_vu0_ctc2(&cpu, &no_control, 8, 28, write_mode, 0x00100068u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE),
        "CTC2 FBRST fails closed without host action callback"
    );
    check(vu0.m_fbrst == old_fbrst,
          "failed FBRST action leaves register state unchanged");
    check(
        ee::ee_vu0_ctc2(&cpu, &no_control, 8, 31, write_mode, 0x0010006cu) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE),
        "CTC2 CMSAR1 fails closed without host action callback"
    );

    control.m_result = static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
    const uint32_t old_cmsar1 = vu0.m_cmsar1;
    check(
        ee::ee_vu0_ctc2(&cpu, &runtime, 8, 31, write_mode, 0x00100070u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED),
        "CTC2 propagates host action failure"
    );
    check(vu0.m_cmsar1 == old_cmsar1,
          "failed CMSAR1 action leaves state unchanged");
    control.m_result = static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE);

    cpu.m_gpr[12] = {0xaaaaaaaaaaaaaaaaULL, 0xbbbbbbbbbbbbbbbbULL};
    scheduler.m_result = static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
    check(
        ee::ee_vu0_qmfc2(&cpu, &runtime, 12, 3, read_mode, 0x00100040u) ==
            static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED),
        "QMFC2 propagates scheduler failure"
    );
    check(cpu.m_gpr[12].m_lo == 0xaaaaaaaaaaaaaaaaULL &&
          cpu.m_gpr[12].m_hi == 0xbbbbbbbbbbbbbbbbULL,
          "failed QMFC2 leaves destination unchanged");

    // The concrete scheduler adapter applies the PCSX2 synchronization
    // policy around a host-provided VU execution callback.
    {
        s_mock_vu0_engine engine{};
        ee::s_ee_vu0_scheduler scheduler = make_mock_vu0_scheduler(&engine);
        ee::s_ee_vu0_state state{};
        state.m_vpu_status = 1u;
        state.m_cycle = 30;
        state.m_ee_cycle = 100;

        const uint32_t result = ee::ee_vu0_scheduler_sync_callback(
            &scheduler, &cpu, &state,
            static_cast<uint32_t>(e_ee_vu0_sync_mode::CATCH_UP_TO_EE_CYCLE),
            0x00101000u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
              "catch-up scheduler completes");
        check(engine.m_run_calls == 1 && engine.m_last_budget == 70,
              "catch-up executes exactly the EE/VU0 cycle difference");
        check((engine.m_last_flags & ee::EE_VU0_EXECUTE_EXACT_CYCLES) != 0 &&
              (engine.m_last_flags & ee::EE_VU0_EXECUTE_STOP_AT_E) != 0 &&
              (engine.m_last_flags & ee::EE_VU0_EXECUTE_STOP_AT_M) == 0,
              "catch-up requests exact cycles and ignores M-bit");
        check(state.m_cycle == 100 && state.m_ee_cycle == 100,
              "catch-up advances VU0 to EE without changing EE time");
        check(engine.m_vu1_calls == 0 && engine.m_event_calls == 0,
              "catch-up does not run the interlocked epilogue");
    }
    {
        s_mock_vu0_engine engine{};
        ee::s_ee_vu0_scheduler scheduler = make_mock_vu0_scheduler(&engine);
        ee::s_ee_vu0_state state{};
        state.m_vpu_status = 1u;
        state.m_cycle = 101;
        state.m_ee_cycle = 100;

        const uint32_t result = ee::ee_vu0_scheduler_sync_callback(
            &scheduler, &cpu, &state,
            static_cast<uint32_t>(e_ee_vu0_sync_mode::CATCH_UP_TO_EE_CYCLE),
            0x00101004u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              engine.m_run_calls == 0,
              "catch-up does not run VU0 that is already ahead");
    }
    {
        s_mock_vu0_engine engine{};
        engine.m_clear_e_after_call = 2;
        engine.m_set_m_after_call = 1;
        ee::s_ee_vu0_scheduler scheduler = make_mock_vu0_scheduler(&engine);
        ee::s_ee_vu0_state state{};
        state.m_vpu_status = 1u;
        // VU0 is already ahead of EE at entry. The interlocked path must
        // add only the cycles consumed during this wait to the EE counter.
        state.m_cycle = 80;
        state.m_ee_cycle = 40;

        const uint32_t result = ee::ee_vu0_scheduler_sync_callback(
            &scheduler, &cpu, &state,
            static_cast<uint32_t>(e_ee_vu0_sync_mode::FINISH_TO_E),
            0x00101008u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              engine.m_run_calls == 2,
              "finish-to-E continues through an M-bit and stops at E");
        check((engine.m_last_flags & ee::EE_VU0_EXECUTE_STOP_AT_M) == 0,
              "finish-to-E does not request M-bit termination");
        check(state.m_cycle == 100 && state.m_ee_cycle == 60,
              "finish-to-E adds consumed cycles without collapsing EE/VU0 offset");
        check(engine.m_vu1_calls == 1 && engine.m_event_calls == 0,
              "finish-to-E catches VU1 up without scheduling a stopped VU0");
    }
    {
        s_mock_vu0_engine engine{};
        engine.m_set_m_after_call = 1;
        ee::s_ee_vu0_scheduler scheduler = make_mock_vu0_scheduler(&engine);
        ee::s_ee_vu0_state state{};
        state.m_vpu_status = 1u;
        state.m_cycle = 20;
        state.m_ee_cycle = 100;

        const uint32_t result = ee::ee_vu0_scheduler_sync_callback(
            &scheduler, &cpu, &state,
            static_cast<uint32_t>(e_ee_vu0_sync_mode::WAIT_TO_E_OR_M),
            0x0010100cu
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              engine.m_run_calls == 1,
              "wait-to-E-or-M stops on the first M-bit");
        check((engine.m_last_flags & ee::EE_VU0_EXECUTE_STOP_AT_M) != 0,
              "wait-to-E-or-M requests M-bit termination");
        check(state.m_ee_cycle == 110 && state.m_cycle == 30,
              "wait-to-E-or-M adds consumed VU0 cycles to the original EE time");
        check(engine.m_vu1_calls == 1 && engine.m_event_calls == 1 &&
              engine.m_last_event_delta == 4,
              "wait-to-E-or-M catches VU1 up and schedules a running VU0");
        check(engine.m_last_pc == 0x0010100cu,
              "VU1 catch-up and event scheduling retain the guest PC");
    }
    {
        s_mock_vu0_engine engine{};
        ee::s_ee_vu0_scheduler scheduler = make_mock_vu0_scheduler(&engine);
        ee::s_ee_vu0_state state{};
        state.m_vpu_status = 1u;
        state.m_m_bit = 1;
        state.m_cycle = 40;
        state.m_ee_cycle = 30;

        const uint32_t result = ee::ee_vu0_scheduler_sync_callback(
            &scheduler, &cpu, &state,
            static_cast<uint32_t>(e_ee_vu0_sync_mode::WAIT_TO_E_OR_M),
            0x00101010u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              engine.m_run_calls == 0,
              "wait-to-E-or-M takes the early M-bit catch-up path");
        check(state.m_ee_cycle == 40 && engine.m_vu1_calls == 0 &&
              engine.m_event_calls == 0,
              "early M-bit path skips the normal VU1/event epilogue");
    }
    {
        s_mock_vu0_engine engine{};
        engine.m_execute_result =
            static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED);
        ee::s_ee_vu0_scheduler scheduler = make_mock_vu0_scheduler(&engine);
        ee::s_ee_vu0_state state{};
        state.m_vpu_status = 1u;
        state.m_cycle = 20;
        state.m_ee_cycle = 40;

        const uint32_t result = ee::ee_vu0_scheduler_sync_callback(
            &scheduler, &cpu, &state,
            static_cast<uint32_t>(e_ee_vu0_sync_mode::FINISH_TO_E),
            0x00101014u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED) &&
              engine.m_vu1_calls == 0 && engine.m_event_calls == 0,
              "VU0 engine failure stops before VU1/event callbacks");
    }
    {
        s_mock_vu0_engine engine{};
        ee::s_ee_vu0_scheduler scheduler = make_mock_vu0_scheduler(&engine);
        scheduler.m_vu1_catch_up = nullptr;
        ee::s_ee_vu0_state state{};
        state.m_vpu_status = 1u;
        state.m_cycle = 20;
        state.m_ee_cycle = 40;

        const uint32_t result = ee::ee_vu0_scheduler_sync_callback(
            &scheduler, &cpu, &state,
            static_cast<uint32_t>(e_ee_vu0_sync_mode::FINISH_TO_E),
            0x00101018u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE) &&
              engine.m_run_calls == 0,
              "finish-to-E fails closed before execution without VU1 catch-up");
    }
    {
        s_mock_vu0_engine engine{};
        engine.m_cycles_per_call = 1;
        ee::s_ee_vu0_scheduler scheduler = make_mock_vu0_scheduler(&engine);
        scheduler.m_max_run_calls = 2;
        ee::s_ee_vu0_state state{};
        state.m_vpu_status = 1u;
        state.m_cycle = 20;
        state.m_ee_cycle = 40;

        const uint32_t result = ee::ee_vu0_scheduler_sync_callback(
            &scheduler, &cpu, &state,
            static_cast<uint32_t>(e_ee_vu0_sync_mode::FINISH_TO_E),
            0x0010101cu
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED) &&
              engine.m_run_calls == 2,
              "finish-to-E enforces the configured execution-call budget");
    }

    // A deterministic scalar oracle exercises mixed register dependencies
    // over a longer program. The oracle is independent of the runtime decoder
    // and models the architectural VI low-halfword and VI-backup rules.
    {
        constexpr uint32_t instruction_count = 256u;
        uint32_t random_state = 0x6d2b79f5u;
        auto next_random = [&random_state]() -> uint32_t
        {
            random_state ^= random_state << 13;
            random_state ^= random_state >> 17;
            random_state ^= random_state << 5;
            return random_state;
        };

        ee::s_ee_vu0_state actual{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        uint32_t reference_vi[16]{};
        uint32_t reference_backup_cycles = 0;
        uint32_t reference_backup_register = 0;
        uint16_t reference_backup_old_value = 0;
        actual.m_vpu_status = 1u;

        for (uint32_t i = 0; i < 16; ++i)
        {
            actual.m_vi[i] = next_random();
            reference_vi[i] = actual.m_vi[i];
        }

        bool reference_matches = true;
        for (uint32_t step = 0; step < instruction_count; ++step)
        {
            const uint32_t operation = next_random() % 7u;
            const uint32_t it = next_random() & 0x0fu;
            const uint32_t is = next_random() & 0x0fu;
            const uint32_t id = next_random() & 0x0fu;
            const uint32_t immediate = next_random() & 0x7fffu;
            uint32_t lower = 0;
            uint32_t destination = 0;
            uint16_t result_value = 0;

            const uint16_t source_s = is == 0
                ? 0u : static_cast<uint16_t>(reference_vi[is]);
            const uint16_t source_t = it == 0
                ? 0u : static_cast<uint16_t>(reference_vi[it]);

            // PCSX2 ages the previous VI-backup window after pipe stalls
            // and before executing the current lower operation.
            if (reference_backup_cycles != 0)
            {
                --reference_backup_cycles;
            }

            switch (operation)
            {
            case 0:
                lower = make_vu0_ialu_3r(0x30u, it, is, id);
                destination = id;
                result_value = static_cast<uint16_t>(
                    static_cast<int16_t>(source_s) +
                    static_cast<int16_t>(source_t)
                );
                break;

            case 1:
                lower = make_vu0_ialu_3r(0x31u, it, is, id);
                destination = id;
                result_value = static_cast<uint16_t>(
                    static_cast<int16_t>(source_s) -
                    static_cast<int16_t>(source_t)
                );
                break;

            case 2:
                lower = make_vu0_ialu_3r(0x34u, it, is, id);
                destination = id;
                result_value = static_cast<uint16_t>(source_s & source_t);
                break;

            case 3:
                lower = make_vu0_ialu_3r(0x35u, it, is, id);
                destination = id;
                result_value = static_cast<uint16_t>(source_s | source_t);
                break;

            case 4:
            {
                const int32_t signed_immediate =
                    static_cast<int32_t>(next_random() & 0x1fu);
                const int32_t immediate_value =
                    (signed_immediate & 0x10) != 0
                        ? signed_immediate - 0x20
                        : signed_immediate;
                lower = make_vu0_iaddi(it, is, immediate_value);
                destination = it;
                result_value = static_cast<uint16_t>(
                    static_cast<int16_t>(source_s) + immediate_value
                );
                break;
            }

            case 5:
                lower = make_vu0_iaddiu(it, is, immediate);
                destination = it;
                result_value = static_cast<uint16_t>(
                    static_cast<int16_t>(source_s) +
                    static_cast<int32_t>(immediate)
                );
                break;

            case 6:
                lower = make_vu0_isubiu(it, is, immediate);
                destination = it;
                result_value = static_cast<uint16_t>(
                    static_cast<int16_t>(source_s) -
                    static_cast<int32_t>(immediate)
                );
                break;

            default:
                reference_matches = false;
                break;
            }

            // The lower operation captures the old low halfword after
            // this pair's previous backup window has aged.
            if (destination != 0)
            {
                if (reference_backup_cycles == 0 ||
                    reference_backup_register != destination)
                {
                    reference_backup_old_value =
                        static_cast<uint16_t>(reference_vi[destination]);
                }
                reference_backup_register = destination;
                reference_backup_cycles = 2;
                reference_vi[destination] =
                    (reference_vi[destination] & 0xffff0000u) | result_value;
            }
            install_vu0_pair(&actual, step, lower, make_vu0_upper_nop());
            const uint32_t execute_result = ee::ee_vu0_execute_ialu_callback(
                &ialu_engine, &cpu, &actual, 1,
                ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x00102000u
            );

            if (execute_result !=
                    static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) ||
                actual.m_cycle != static_cast<uint64_t>(step + 1u) ||
                actual.m_micro_pc != (step + 1u) * 8u ||
                actual.m_tpc != ((step + 1u) & 0x1ffu) ||
                actual.m_ialu_count != 0 ||
                actual.m_vi_backup_cycles != reference_backup_cycles ||
                actual.m_vi_backup_register != reference_backup_register ||
                static_cast<uint16_t>(actual.m_vi_backup_old_value) !=
                    reference_backup_old_value)
            {
                reference_matches = false;
                std::printf(
                    "VU0 scalar oracle mismatch at pair %u: result=%u "
                    "cycle=%llu pc=0x%x backup=%u/%u expected=%u/%u\n",
                    step, execute_result,
                    static_cast<unsigned long long>(actual.m_cycle),
                    actual.m_micro_pc, actual.m_vi_backup_register,
                    actual.m_vi_backup_cycles, reference_backup_register,
                    reference_backup_cycles
                );
                break;
            }

            for (uint32_t reg = 0; reg < 16; ++reg)
            {
                if (actual.m_vi[reg] != reference_vi[reg])
                {
                    reference_matches = false;
                    std::printf(
                        "VU0 scalar oracle VI mismatch at pair %u, VI%u: "
                        "actual=0x%08x expected=0x%08x\n",
                        step, reg, actual.m_vi[reg], reference_vi[reg]
                    );
                    break;
                }
            }

            if (!reference_matches)
            {
                break;
            }
        }

        check(reference_matches,
              "256-pair scalar IALU sequence matches the independent VI/backup oracle");
    }

    // A second test-side model follows PCSX2's lower-op, branch-delay,
    // VI-backup and E/M ordering over a deterministic mixed control-flow
    // program. It decodes raw words independently of the runtime decoder.
    {
        constexpr uint32_t program_slots = 512u;
        constexpr uint32_t pair_count = 384u;
        uint32_t random_state = 0x91e10da5u;
        auto next_random = [&random_state]() -> uint32_t
        {
            random_state ^= random_state << 13;
            random_state ^= random_state >> 17;
            random_state ^= random_state << 5;
            return random_state;
        };

        ee::s_ee_vu0_state actual{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        uint32_t reference_vi[16]{};
        uint32_t program_lower[program_slots]{};
        uint32_t program_upper[program_slots]{};
        uint32_t reference_pc = 0;
        uint32_t reference_branch = 0;
        uint32_t reference_branch_pc = 0;
        uint32_t reference_delay_branch_pc = 0;
        uint32_t reference_take_delay_branch = 0;
        uint32_t reference_backup_cycles = 0;
        uint32_t reference_backup_register = 0;
        uint16_t reference_backup_old_value = 0;
        uint32_t reference_e_bit = 0;
        uint32_t reference_vpu_status = 1;
        uint32_t reference_m_bit = 0;
        uint64_t reference_cycle = 0;
        uint32_t executed_operation_mask = 0;
        actual.m_vpu_status = 1u;

        for (uint32_t reg = 0; reg < 16; ++reg)
        {
            actual.m_vi[reg] = next_random();
            reference_vi[reg] = actual.m_vi[reg];
        }

        // Keep the first path deterministic and linear while visiting every
        // supported lower opcode family. The four unconditional transfers
        // target the instruction after their delay pair; all conditional
        // branches are arranged to fall through. This guarantees that the
        // mixed oracle exercises each of its 17 operation classes before the
        // randomized control-flow tail begins.
        reference_vi[1] = actual.m_vi[1] = 1u;
        reference_vi[2] = actual.m_vi[2] = 2u;
        reference_vi[4] = actual.m_vi[4] = 0x8000u;
        reference_vi[13] = actual.m_vi[13] = 13u;
        reference_vi[15] = actual.m_vi[15] = 15u;

        for (uint32_t slot = 0; slot < program_slots; ++slot)
        {
            const uint32_t operation = slot % 17u;
            const uint32_t it = next_random() & 0x0fu;
            const uint32_t is = next_random() & 0x0fu;
            const uint32_t id = next_random() & 0x0fu;
            const uint32_t immediate = next_random() & 0x07ffu;
            uint32_t lower = 0;

            if (slot < 21u)
            {
                switch (slot)
                {
                case 0: lower = make_vu0_ialu_3r(0x30u, 2, 1, 3); break;
                case 1: lower = make_vu0_ialu_3r(0x31u, 2, 3, 5); break;
                case 2: lower = make_vu0_ialu_3r(0x34u, 2, 1, 6); break;
                case 3: lower = make_vu0_ialu_3r(0x35u, 2, 1, 7); break;
                case 4: lower = make_vu0_iaddi(8, 7, -1); break;
                case 5: lower = make_vu0_iaddiu(9, 8, 3); break;
                case 6: lower = make_vu0_isubiu(10, 9, 1); break;
                case 7: lower = make_vu0_branch(0x20u, 0, 0, 1); break;
                case 8: lower = make_vu0_iaddiu(11, 0, 5); break;
                case 9: lower = make_vu0_branch(0x21u, 12, 0, 1); break;
                case 10: lower = make_vu0_iaddiu(6, 0, 6); break;
                case 11: lower = make_vu0_branch(0x24u, 0, 13, 0); break;
                case 12: lower = make_vu0_iaddiu(7, 0, 7); break;
                case 13: lower = make_vu0_branch(0x25u, 14, 15, 0); break;
                case 14: lower = make_vu0_iaddiu(8, 0, 8); break;
                case 15: lower = make_vu0_branch(0x28u, 1, 2, 1); break;
                case 16: lower = make_vu0_branch(0x29u, 1, 1, 1); break;
                case 17: lower = make_vu0_branch(0x2cu, 0, 1, 1); break;
                case 18: lower = make_vu0_branch(0x2du, 0, 0, 1); break;
                case 19: lower = make_vu0_branch(0x2eu, 0, 1, 1); break;
                case 20: lower = make_vu0_branch(0x2fu, 0, 4, 1); break;
                default: break;
                }
            }
            else
            {
                switch (operation)
                {
                case 0: lower = make_vu0_ialu_3r(0x30u, it, is, id); break;
            case 1: lower = make_vu0_ialu_3r(0x31u, it, is, id); break;
            case 2: lower = make_vu0_ialu_3r(0x34u, it, is, id); break;
            case 3: lower = make_vu0_ialu_3r(0x35u, it, is, id); break;
            case 4:
                lower = make_vu0_iaddi(
                    it, is, static_cast<int32_t>(immediate & 0x1fu)
                );
                break;
            case 5: lower = make_vu0_iaddiu(it, is, immediate); break;
            case 6: lower = make_vu0_isubiu(it, is, immediate); break;
            case 7: lower = make_vu0_branch(0x20u, it, is, immediate); break;
            case 8: lower = make_vu0_branch(0x21u, it, is, immediate); break;
            case 9: lower = make_vu0_branch(0x24u, it, is, immediate); break;
            case 10: lower = make_vu0_branch(0x25u, it, is, immediate); break;
            case 11: lower = make_vu0_branch(0x28u, it, is, immediate); break;
            case 12: lower = make_vu0_branch(0x29u, it, is, immediate); break;
            case 13: lower = make_vu0_branch(0x2cu, it, is, immediate); break;
            case 14: lower = make_vu0_branch(0x2du, it, is, immediate); break;
            case 15: lower = make_vu0_branch(0x2eu, it, is, immediate); break;
            case 16: lower = make_vu0_branch(0x2fu, it, is, immediate); break;
                default: break;
                }
            }

            uint32_t flags = slot >= 21u && (next_random() % 23u) == 0
                ? 0x20000000u : 0u; // M
            if (slot == program_slots - 1u)
            {
                flags |= 0x40000000u; // E, reachable by a sampled jump.
            }

            program_lower[slot] = lower;
            program_upper[slot] = make_vu0_upper_nop(flags);
            install_vu0_pair(&actual, slot, lower, program_upper[slot]);
        }

        bool reference_matches = true;
        for (uint32_t step = 0; step < pair_count; ++step)
        {
            if ((reference_vpu_status & 1u) == 0)
            {
                break;
            }

            reference_m_bit = 0;
            const uint32_t slot = (reference_pc >> 3) & 0x1ffu;
            const uint32_t lower = program_lower[slot];
            const uint32_t upper = program_upper[slot];
            uint64_t pair_elapsed_cycles = 1;

            if (step == 15u)
            {
                // Force the first conditional branch to interlock on VI1.
                // The three-cycle reservation outlasts the one-cycle backup,
                // so the lower handler must compare current VI values after
                // the stall, despite this one-cycle execution budget.
                actual.m_vi[1] =
                    (actual.m_vi[1] & 0xffff0000u) | 0u;
                actual.m_vi[2] =
                    (actual.m_vi[2] & 0xffff0000u) | 1u;
                reference_vi[1] =
                    (reference_vi[1] & 0xffff0000u) | 0u;
                reference_vi[2] =
                    (reference_vi[2] & 0xffff0000u) | 1u;
                actual.m_vi_backup_cycles = 1;
                actual.m_vi_backup_register = 1;
                actual.m_vi_backup_old_value = 1;
                reference_backup_cycles = 1;
                reference_backup_register = 1;
                reference_backup_old_value = 1;

                const uint32_t schedule_result =
                    ee::ee_vu0_ialu_schedule_write(
                        &actual, 1u << 1, 3
                    );
                if (schedule_result != static_cast<uint32_t>(
                        ee::e_ee_vu0_sync_result::COMPLETE))
                {
                    reference_matches = false;
                    break;
                }
                pair_elapsed_cycles = 3;
            }

            reference_pc = (reference_pc + 8u) & 0x0fffu;
            reference_cycle += pair_elapsed_cycles;

            // PCSX2 applies lower-pipe stalls and retires matured pipes
            // before aging VIBackupCycles and invoking the lower handler.
            // Age by the full elapsed issue interval before evaluating a
            // branch predicate or capturing a fresh IALU-write backup.
            if (pair_elapsed_cycles >= reference_backup_cycles)
            {
                reference_backup_cycles = 0;
            }
            else if (reference_backup_cycles != 0)
            {
                reference_backup_cycles -= static_cast<uint32_t>(
                    pair_elapsed_cycles
                );
            }

            const uint32_t primary = lower >> 25;
            const uint32_t it = (lower >> 16) & 0x0fu;
            const uint32_t is = (lower >> 11) & 0x0fu;
            const uint32_t id = (lower >> 6) & 0x0fu;
            const int16_t source_s = is == 0
                ? 0 : static_cast<int16_t>(reference_vi[is]);
            const int16_t source_t = it == 0
                ? 0 : static_cast<int16_t>(reference_vi[it]);

            bool write_vi = false;
            uint32_t destination = 0;
            uint16_t result_value = 0;
            bool branch_taken = false;
            bool relative_branch = false;
            bool link_write = false;
            uint32_t branch_target = 0;
            uint32_t link_register = 0;
            uint16_t link_value = 0;

            switch (primary)
            {
            case 0x40u:
            {
                const uint32_t secondary = lower & 0x3fu;
                switch (secondary)
                {
                case 0x30u:
                    write_vi = true;
                    destination = id;
                    result_value = static_cast<uint16_t>(
                        static_cast<int16_t>(source_s) +
                        static_cast<int16_t>(source_t)
                    );
                    executed_operation_mask |= 1u << 0;
                    break;
                case 0x31u:
                    write_vi = true;
                    destination = id;
                    result_value = static_cast<uint16_t>(
                        static_cast<int16_t>(source_s) -
                        static_cast<int16_t>(source_t)
                    );
                    executed_operation_mask |= 1u << 1;
                    break;
                case 0x34u:
                    write_vi = true;
                    destination = id;
                    result_value = static_cast<uint16_t>(
                        static_cast<uint16_t>(source_s) &
                        static_cast<uint16_t>(source_t)
                    );
                    executed_operation_mask |= 1u << 2;
                    break;
                case 0x35u:
                    write_vi = true;
                    destination = id;
                    result_value = static_cast<uint16_t>(
                        static_cast<uint16_t>(source_s) |
                        static_cast<uint16_t>(source_t)
                    );
                    executed_operation_mask |= 1u << 3;
                    break;
                case 0x32u:
                {
                    int32_t immediate_value =
                        static_cast<int32_t>((lower >> 6) & 0x1fu);
                    if ((immediate_value & 0x10) != 0)
                    {
                        immediate_value -= 0x20;
                    }
                    write_vi = true;
                    destination = it;
                    result_value = static_cast<uint16_t>(
                        static_cast<int16_t>(source_s) + immediate_value
                    );
                    executed_operation_mask |= 1u << 4;
                    break;
                }
                default:
                    reference_matches = false;
                    break;
                }
                break;
            }

            case 0x08u:
            case 0x09u:
            {
                const uint32_t immediate_value =
                    ((lower >> 10) & 0x7800u) | (lower & 0x07ffu);
                write_vi = true;
                destination = it;
                const int32_t arithmetic = primary == 0x08u
                    ? static_cast<int32_t>(static_cast<int16_t>(source_s)) +
                        static_cast<int32_t>(immediate_value)
                    : static_cast<int32_t>(static_cast<int16_t>(source_s)) -
                        static_cast<int32_t>(immediate_value);
                result_value = static_cast<uint16_t>(arithmetic);
                executed_operation_mask |= primary == 0x08u
                    ? (1u << 5) : (1u << 6);
                break;
            }

            case 0x20u: // B
            case 0x21u: // BAL
            case 0x24u: // JR
            case 0x25u: // JALR
            case 0x28u: // IBEQ
            case 0x29u: // IBNE
            case 0x2cu: // IBLTZ
            case 0x2du: // IBGTZ
            case 0x2eu: // IBLEZ
            case 0x2fu: // IBGEZ
            {
                const uint16_t branch_s = is == 0 ? 0u :
                    (reference_backup_cycles != 0 &&
                     reference_backup_register == is
                        ? reference_backup_old_value
                        : static_cast<uint16_t>(reference_vi[is]));
                const uint16_t branch_t = it == 0 ? 0u :
                    (reference_backup_cycles != 0 &&
                     reference_backup_register == it
                        ? reference_backup_old_value
                        : static_cast<uint16_t>(reference_vi[it]));
                const int16_t signed_s = static_cast<int16_t>(branch_s);
                const int16_t signed_t = static_cast<int16_t>(branch_t);
                const uint32_t encoded_immediate = lower & 0x07ffu;
                const int32_t signed_immediate =
                    (encoded_immediate & 0x0400u) != 0
                        ? static_cast<int32_t>(encoded_immediate) - 0x0800
                        : static_cast<int32_t>(encoded_immediate);

                switch (primary)
                {
                case 0x20u:
                    branch_taken = true;
                    relative_branch = true;
                    executed_operation_mask |= 1u << 7;
                    break;
                case 0x21u:
                    branch_taken = true;
                    relative_branch = true;
                    link_write = it != 0;
                    executed_operation_mask |= 1u << 8;
                    break;
                case 0x24u:
                    branch_taken = true;
                    branch_target =
                        (static_cast<uint32_t>(
                            is == 0 ? 0u :
                                static_cast<uint16_t>(reference_vi[is])
                        ) << 3) & 0x0fffu;
                    executed_operation_mask |= 1u << 9;
                    break;
                case 0x25u:
                    branch_taken = true;
                    branch_target =
                        (static_cast<uint32_t>(
                            is == 0 ? 0u :
                                static_cast<uint16_t>(reference_vi[is])
                        ) << 3) & 0x0fffu;
                    link_write = it != 0;
                    executed_operation_mask |= 1u << 10;
                    break;
                case 0x28u:
                    branch_taken = branch_t == branch_s;
                    relative_branch = true;
                    executed_operation_mask |= 1u << 11;
                    break;
                case 0x29u:
                    branch_taken = branch_t != branch_s;
                    relative_branch = true;
                    executed_operation_mask |= 1u << 12;
                    break;
                case 0x2cu:
                    branch_taken = signed_s < 0;
                    relative_branch = true;
                    executed_operation_mask |= 1u << 13;
                    break;
                case 0x2du:
                    branch_taken = signed_s > 0;
                    relative_branch = true;
                    executed_operation_mask |= 1u << 14;
                    break;
                case 0x2eu:
                    branch_taken = signed_s <= 0;
                    relative_branch = true;
                    executed_operation_mask |= 1u << 15;
                    break;
                case 0x2fu:
                    branch_taken = signed_s >= 0;
                    relative_branch = true;
                    executed_operation_mask |= 1u << 16;
                    break;
                default:
                    break;
                }

                if (link_write)
                {
                    const uint32_t link_pc = reference_branch == 1
                        ? reference_branch_pc + 8u
                        : reference_pc + 8u;
                    link_register = it;
                    link_value = static_cast<uint16_t>((link_pc >> 3) & 0xffffu);
                }
                if (branch_taken && relative_branch)
                {
                    branch_target = static_cast<uint32_t>(
                        static_cast<int64_t>(reference_pc) +
                        static_cast<int64_t>(signed_immediate) * 8
                    ) & 0x0fffu;
                }
                break;
            }

            default:
                reference_matches = false;
                break;
            }

            if (!reference_matches)
            {
                break;
            }

            // IALU writes capture the old low halfword before commit. Link
            // writes do not enter the VI-backup mechanism in PCSX2.
            if (write_vi && destination != 0)
            {
                if (reference_backup_cycles == 0 ||
                    reference_backup_register != destination)
                {
                    reference_backup_old_value =
                        static_cast<uint16_t>(reference_vi[destination]);
                }
                reference_backup_register = destination;
                reference_backup_cycles = 2;
                reference_vi[destination] =
                    (reference_vi[destination] & 0xffff0000u) | result_value;
            }

            if (link_register != 0)
            {
                reference_vi[link_register] =
                    (reference_vi[link_register] & 0xffff0000u) | link_value;
            }

            if (branch_taken)
            {
                if (reference_branch == 1)
                {
                    reference_delay_branch_pc = branch_target;
                    reference_take_delay_branch = 1;
                }
                else
                {
                    reference_branch = 2;
                    reference_branch_pc = branch_target;
                }
            }

            // Branch retirement follows the pair's lower operation.
            if (reference_branch != 0)
            {
                if (reference_branch > 1)
                {
                    --reference_branch;
                }
                else
                {
                    reference_pc = reference_branch_pc & 0x0fffu;
                    if (reference_take_delay_branch != 0)
                    {
                        reference_branch = 1;
                        reference_branch_pc =
                            reference_delay_branch_pc & 0x0fffu;
                        reference_take_delay_branch = 0;
                    }
                    else
                    {
                        reference_branch = 0;
                    }
                }
            }

            if ((upper & 0x40000000u) != 0)
            {
                reference_e_bit = 2;
            }
            if ((upper & 0x20000000u) != 0)
            {
                reference_m_bit = 1;
            }

            if (reference_e_bit != 0)
            {
                --reference_e_bit;
                if (reference_e_bit == 0)
                {
                    reference_backup_cycles = 0;
                    reference_vpu_status &= ~1u;
                }
            }

            if ((reference_vpu_status & 1u) == 0 &&
                reference_branch != 0)
            {
                reference_pc = reference_branch_pc & 0x0fffu;
                reference_branch = 0;
                reference_take_delay_branch = 0;
            }

            const uint32_t execute_result = ee::ee_vu0_execute_ialu_callback(
                &ialu_engine, &cpu, &actual, 1,
                ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x00102000u
            );

            bool pair_matches =
                execute_result == static_cast<uint32_t>(
                    ee::e_ee_vu0_sync_result::COMPLETE) &&
                actual.m_cycle == reference_cycle &&
                actual.m_micro_pc == reference_pc &&
                actual.m_tpc == ((reference_pc >> 3) & 0x1ffu) &&
                actual.m_branch == reference_branch &&
                actual.m_branch_pc == reference_branch_pc &&
                actual.m_delay_branch_pc == reference_delay_branch_pc &&
                actual.m_take_delay_branch == reference_take_delay_branch &&
                actual.m_vi_backup_cycles == reference_backup_cycles &&
                actual.m_vi_backup_register == reference_backup_register &&
                static_cast<uint16_t>(actual.m_vi_backup_old_value) ==
                    reference_backup_old_value &&
                actual.m_e_bit == reference_e_bit &&
                actual.m_vpu_status == reference_vpu_status &&
                actual.m_m_bit == reference_m_bit &&
                actual.m_running == (reference_vpu_status & 1u) &&
                actual.m_ialu_count == 0;

            for (uint32_t reg = 0; reg < 16 && pair_matches; ++reg)
            {
                pair_matches = actual.m_vi[reg] == reference_vi[reg];
            }

            if (!pair_matches)
            {
                reference_matches = false;
                std::printf(
                    "VU0 control-flow oracle mismatch at pair %u "
                    "(slot=%u pc=0x%x/0x%x cycle=%llu/%llu "
                    "branch=%u/%u target=0x%x/0x%x deferred=0x%x/0x%x "
                    "take=%u/%u backup=%u/%u reg=%u/%u old=0x%x/0x%x "
                    "e=%u/%u m=%u/%u status=%u/%u running=%u)\n",
                    step, slot, reference_pc, actual.m_micro_pc,
                    static_cast<unsigned long long>(actual.m_cycle),
                    static_cast<unsigned long long>(reference_cycle),
                    actual.m_branch, reference_branch,
                    actual.m_branch_pc, reference_branch_pc,
                    actual.m_delay_branch_pc, reference_delay_branch_pc,
                    actual.m_take_delay_branch, reference_take_delay_branch,
                    actual.m_vi_backup_cycles, reference_backup_cycles,
                    actual.m_vi_backup_register, reference_backup_register,
                    static_cast<uint16_t>(actual.m_vi_backup_old_value),
                    reference_backup_old_value,
                    actual.m_e_bit, reference_e_bit,
                    actual.m_m_bit, reference_m_bit,
                    actual.m_vpu_status, reference_vpu_status,
                    actual.m_running
                );
                for (uint32_t reg = 0; reg < 16; ++reg)
                {
                    if (actual.m_vi[reg] != reference_vi[reg])
                    {
                        std::printf(
                            "  VI%u actual=0x%08x expected=0x%08x\n",
                            reg, actual.m_vi[reg], reference_vi[reg]
                        );
                    }
                }
                break;
            }

            if ((reference_vpu_status & 1u) == 0)
            {
                break;
            }
        }

        check(reference_matches &&
              executed_operation_mask == ((1u << 17) - 1u),
              "384-pair mixed VU0 trace matches the oracle, executes all 17 op classes and covers a forced three-cycle branch interlock");
    }

    // Execute the conservative lower-IALU/upper-NOP subset through the
    // same callback shape used by the VU scheduler.
    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        state.m_vi[1] = 0xabcd0000u; // Preserve VI[1].SS[1] on low-halfword writes.
        state.m_vi[0] = 0xdeadbeefu; // Reads are hardwired to zero; writes ignored.

        install_vu0_pair(&state, 0, make_vu0_iaddiu(1, 0, 0x1234), make_vu0_upper_nop());
        install_vu0_pair(&state, 1, make_vu0_iaddi(2, 1, -3), make_vu0_upper_nop());
        install_vu0_pair(&state, 2, make_vu0_ialu_3r(0x30, 2, 1, 3), make_vu0_upper_nop());
        install_vu0_pair(&state, 3, make_vu0_ialu_3r(0x31, 2, 3, 4), make_vu0_upper_nop());
        install_vu0_pair(&state, 4, make_vu0_ialu_3r(0x34, 1, 3, 5), make_vu0_upper_nop());
        install_vu0_pair(&state, 5, make_vu0_ialu_3r(0x35, 2, 3, 6), make_vu0_upper_nop());
        install_vu0_pair(&state, 6, make_vu0_isubiu(7, 6, 0x0123), make_vu0_upper_nop());
        install_vu0_pair(&state, 7, make_vu0_ialu_3r(0x30, 1, 2, 0), make_vu0_upper_nop());

        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &cpu, &state, 8,
            ee::EE_VU0_EXECUTE_EXACT_CYCLES |
                ee::EE_VU0_EXECUTE_STOP_AT_E,
            0x00102000u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
              "VU0 IALU slice executes an exact instruction budget");
        check(static_cast<uint16_t>(state.m_vi[1]) == 0x1234u &&
              (state.m_vi[1] & 0xffff0000u) == 0xabcd0000u,
              "IADDIU writes low VI halfword and preserves the upper halfword");
        check(static_cast<uint16_t>(state.m_vi[2]) == 0x1231u,
              "IADDI sign-extends its five-bit immediate");
        check(static_cast<uint16_t>(state.m_vi[3]) == 0x2465u,
              "IADD uses the encoded VI source and destination fields");
        check(static_cast<uint16_t>(state.m_vi[4]) == 0x1234u,
              "ISUB computes a wrapped 16-bit result");
        check(static_cast<uint16_t>(state.m_vi[5]) == 0x0024u,
              "IAND computes the low 16-bit integer result");
        check(static_cast<uint16_t>(state.m_vi[6]) == 0x3675u,
              "IOR computes the low 16-bit integer result");
        check(static_cast<uint16_t>(state.m_vi[7]) == 0x3552u,
              "ISUBIU uses the encoded unsigned 15-bit immediate");
        check(state.m_vi[0] == 0xdeadbeefu,
              "VU0 IALU ignores writes to VI0");
        check(state.m_cycle == 8 && state.m_micro_pc == 64 && state.m_tpc == 8 &&
              state.m_ialu_count == 0,
              "IALU slice advances cycles, byte PC, TPC and retires prior reservations");
    }
    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        state.m_vi[1] = 0xabcd0007u;
        install_vu0_pair(&state, 0, make_vu0_iaddiu(1, 0, 0x1234),
                         make_vu0_upper_nop(0x20000000u)); // M

        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &cpu, &state, 1,
            ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x00102000u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_m_bit == 1 &&
              static_cast<uint16_t>(state.m_vi[1]) == 0x1234u &&
              state.m_vi_backup_register == 1 &&
              static_cast<uint16_t>(state.m_vi_backup_old_value) == 7u &&
              state.m_vi_backup_cycles == 2,
              "VU0 IALU write starts a fresh two-cycle VI backup after issue");
    }
    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        install_vu0_pair(&state, 0, make_vu0_branch(0x20, 0, 0, 2),
                         make_vu0_upper_nop());
        install_vu0_pair(&state, 1, make_vu0_iaddiu(2, 0, 2),
                         make_vu0_upper_nop());
        install_vu0_pair(&state, 3, make_vu0_iaddiu(3, 0, 3),
                         make_vu0_upper_nop());

        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &cpu, &state, 2,
            ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x00102000u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_cycle == 2 && state.m_micro_pc == 24 && state.m_tpc == 3 &&
              state.m_branch == 0,
              "VU0 B applies its target after exactly one delay pair");
        check(static_cast<uint16_t>(state.m_vi[2]) == 2 &&
              static_cast<uint16_t>(state.m_vi[3]) == 0,
              "VU0 B executes the delay pair and skips sequential fallthrough");
    }
    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        install_vu0_pair(&state, 0, make_vu0_branch(0x21, 4, 0, 2),
                         make_vu0_upper_nop());
        install_vu0_pair(&state, 1, make_vu0_iaddiu(2, 0, 2),
                         make_vu0_upper_nop());

        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &cpu, &state, 2,
            ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x00102004u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              static_cast<uint16_t>(state.m_vi[4]) == 2 &&
              state.m_micro_pc == 24 && state.m_branch == 0,
              "VU0 BAL writes the instruction-after-delay link slot");
    }
    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        state.m_vi[3] = 5u;
        install_vu0_pair(&state, 0, make_vu0_branch(0x24, 0, 3, 0),
                         make_vu0_upper_nop());
        install_vu0_pair(&state, 1, make_vu0_iaddiu(2, 0, 2),
                         make_vu0_upper_nop());

        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &cpu, &state, 2,
            ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x00102008u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_micro_pc == 40 && state.m_branch == 0 &&
              static_cast<uint16_t>(state.m_vi[2]) == 2,
              "VU0 JR jumps to the VI-supplied micro-program slot after its delay pair");
    }
    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        state.m_vi[3] = 5u;
        install_vu0_pair(&state, 0, make_vu0_branch(0x25, 3, 3, 0),
                         make_vu0_upper_nop());
        install_vu0_pair(&state, 1, make_vu0_iaddiu(2, 0, 2),
                         make_vu0_upper_nop());

        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &cpu, &state, 2,
            ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x0010200cu
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_micro_pc == 40 && state.m_branch == 0 &&
              static_cast<uint16_t>(state.m_vi[3]) == 2,
              "VU0 JALR samples its target before writing an overlapping link register");
    }
    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        state.m_vi[3] = 6u;
        install_vu0_pair(&state, 0, make_vu0_branch(0x20, 0, 0, 3),
                         make_vu0_upper_nop());
        install_vu0_pair(&state, 1, make_vu0_branch(0x25, 5, 3, 0),
                         make_vu0_upper_nop());
        install_vu0_pair(&state, 4, make_vu0_iaddiu(6, 0, 6),
                         make_vu0_upper_nop());
        install_vu0_pair(&state, 6, make_vu0_iaddiu(7, 0, 7),
                         make_vu0_upper_nop());

        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &cpu, &state, 3,
            ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x00102010u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_micro_pc == 48 && state.m_branch == 0 &&
              static_cast<uint16_t>(state.m_vi[5]) == 5 &&
              static_cast<uint16_t>(state.m_vi[6]) == 6 &&
              static_cast<uint16_t>(state.m_vi[7]) == 0,
              "VU0 JALR in a branch delay pair defers its target and links from the first target");
    }
    {
        struct s_branch_condition_case
        {
            uint32_t m_primary;
            uint16_t m_source;
            const char* m_name;
        };

        const s_branch_condition_case cases[] =
        {
            { 0x28u, 0u, "IBEQ" },
            { 0x29u, 1u, "IBNE" },
            { 0x2cu, 0xffffu, "IBLTZ" },
            { 0x2du, 1u, "IBGTZ" },
            { 0x2eu, 0u, "IBLEZ" },
            { 0x2fu, 0u, "IBGEZ" },
        };

        bool all_taken = true;
        for (const s_branch_condition_case& branch_case : cases)
        {
            ee::s_ee_vu0_state state{};
            ee::s_ee_vu0_ialu_engine ialu_engine{};
            state.m_vpu_status = 1u;
            state.m_vi[1] = branch_case.m_source;
            install_vu0_pair(&state, 0,
                make_vu0_branch(branch_case.m_primary, 0, 1, 1),
                make_vu0_upper_nop());

            const uint32_t result = ee::ee_vu0_execute_ialu_callback(
                &ialu_engine, &cpu, &state, 1,
                ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x00102014u
            );
            all_taken = all_taken &&
                result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
                state.m_branch == 1 && state.m_branch_pc == 16;
        }
        check(all_taken,
              "VU0 signed and equality branch conditions take at their true boundaries");
    }
    {
        struct s_branch_condition_case
        {
            uint32_t m_primary;
            uint16_t m_source_s;
            uint16_t m_source_t;
        };

        const s_branch_condition_case cases[] =
        {
            { 0x28u, 1u, 2u },       // IBEQ: unequal
            { 0x29u, 1u, 1u },       // IBNE: equal
            { 0x2cu, 0u, 0u },       // IBLTZ: zero
            { 0x2du, 0u, 0u },       // IBGTZ: zero
            { 0x2eu, 1u, 0u },       // IBLEZ: positive
            { 0x2fu, 0xffffu, 0u },  // IBGEZ: negative
        };

        bool all_not_taken = true;
        for (const s_branch_condition_case& branch_case : cases)
        {
            ee::s_ee_vu0_state state{};
            ee::s_ee_vu0_ialu_engine ialu_engine{};
            state.m_vpu_status = 1u;
            state.m_vi[1] = branch_case.m_source_s;
            state.m_vi[2] = branch_case.m_source_t;
            install_vu0_pair(
                &state,
                0,
                make_vu0_branch(branch_case.m_primary, 2, 1, 3),
                make_vu0_upper_nop()
            );

            const uint32_t result = ee::ee_vu0_execute_ialu_callback(
                &ialu_engine, &cpu, &state, 1,
                ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x00102014u
            );
            all_not_taken = all_not_taken &&
                result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
                state.m_cycle == 1 &&
                state.m_micro_pc == 8 &&
                state.m_tpc == 1 &&
                state.m_branch == 0 &&
                state.m_vpu_status == 1;
        }

        check(all_not_taken,
              "VU0 equality and signed branches fall through at their false boundaries");
    }
    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        install_vu0_pair(&state, 0, make_vu0_branch(0x20, 0, 0, 0x7ffu),
                         make_vu0_upper_nop());
        install_vu0_pair(&state, 1, make_vu0_iaddiu(8, 0, 8),
                         make_vu0_upper_nop());

        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &cpu, &state, 2,
            ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x00102018u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_micro_pc == 0 && state.m_tpc == 0 &&
              static_cast<uint16_t>(state.m_vi[8]) == 8,
              "VU0 negative relative branch offset sign-extends and wraps the 4 KB program");
    }
    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        install_vu0_pair(
            &state, 0, make_vu0_branch(0x20u, 0, 0, 3),
            make_vu0_upper_nop(0x40000000u)
        );
        install_vu0_pair(
            &state, 1, make_vu0_iaddiu(2, 0, 2),
            make_vu0_upper_nop()
        );
        install_vu0_pair(
            &state, 4, make_vu0_iaddiu(3, 0, 3),
            make_vu0_upper_nop()
        );

        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &cpu, &state, 8,
            ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x00102018u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_cycle == 2 &&
              state.m_vpu_status == 0 &&
              state.m_running == 0 &&
              state.m_e_bit == 0 &&
              state.m_micro_pc == 32 &&
              state.m_tpc == 4 &&
              state.m_branch == 0,
              "VU0 E on a branch pair still permits its delay pair before stopping");
        check(static_cast<uint16_t>(state.m_vi[2]) == 2 &&
              static_cast<uint16_t>(state.m_vi[3]) == 0,
              "VU0 E termination follows the branch target and does not execute beyond it");
    }
    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        state.m_vi[1] = 1u;
        install_vu0_pair(&state, 0, make_vu0_iaddiu(1, 0, 0),
                         make_vu0_upper_nop());
        install_vu0_pair(&state, 1, make_vu0_branch(0x28, 1, 0, 2),
                         make_vu0_upper_nop());

        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &cpu, &state, 2,
            ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x00102008u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_cycle == 2 && state.m_micro_pc == 16 &&
              state.m_branch == 0 && static_cast<uint16_t>(state.m_vi[1]) == 0,
              "VU0 conditional branch sees the backed-up VI value in its hazard window");
        check(state.m_vi_backup_cycles == 1 &&
              state.m_vi_backup_register == 1 &&
              static_cast<uint16_t>(state.m_vi_backup_old_value) == 1,
              "VU0 branch consumes one cycle of the backed-up value's two-cycle window");
    }
    {
        // PCSX2 applies the lower-pipe interlock and ages VIBackupCycles
        // before invoking the branch handler. A three-cycle stall therefore
        // expires this one-cycle backup before IBEQ reads its operands.
        ee::s_ee_vu0_state state{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        state.m_cycle = 10;
        state.m_vi[1] = 0;
        state.m_vi_backup_cycles = 1;
        state.m_vi_backup_register = 1;
        state.m_vi_backup_old_value = 1;
        install_vu0_pair(&state, 0, make_vu0_branch(0x28, 1, 0, 2),
                         make_vu0_upper_nop());

        const uint32_t scheduled = ee::ee_vu0_ialu_schedule_write(
            &state, 1u << 1, 3
        );
        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &cpu, &state, 1,
            ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x0010200au
        );
        check(scheduled == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_cycle == 13 && state.m_micro_pc == 8 &&
              state.m_branch == 1 && state.m_branch_pc == 24 &&
              state.m_ialu_count == 0,
              "VU0 branch reads current VI after a three-cycle interlock expires the backup");
        check(state.m_vi_backup_cycles == 0,
              "VU0 VI backup expires before the stalled branch handler executes");
    }
    {
        // Both operands of IBEQ have independent pending reservations with
        // different completion cycles. Execution waits for the later of the
        // two matching reservations, then ages the backup window before the
        // branch handler reads VI. A two-cycle slice budget must still finish
        // the issued pair atomically when the dependency takes longer.
        ee::s_ee_vu0_state state{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        state.m_cycle = 20;
        state.m_vi[1] = 0;
        state.m_vi[2] = 1;
        state.m_vi_backup_cycles = 1;
        state.m_vi_backup_register = 1;
        state.m_vi_backup_old_value = 1;
        install_vu0_pair(&state, 0, make_vu0_branch(0x28, 1, 2, 2),
                         make_vu0_upper_nop());

        const uint32_t scheduled_first = ee::ee_vu0_ialu_schedule_write(
            &state, 1u << 1, 3
        );
        const uint32_t scheduled_second = ee::ee_vu0_ialu_schedule_write(
            &state, 1u << 2, 5
        );
        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &cpu, &state, 2,
            ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x0010200au
        );

        check(scheduled_first == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              scheduled_second == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_cycle == 25 && state.m_micro_pc == 8 && state.m_tpc == 1 &&
              state.m_branch == 0 && state.m_branch_pc == 0 &&
              state.m_ialu_count == 0,
              "VU0 branch waits for the latest matching source reservation beyond a two-cycle budget");
        check(state.m_vi_backup_cycles == 0 &&
              state.m_vi_backup_register == 1 &&
              static_cast<uint16_t>(state.m_vi_backup_old_value) == 1 &&
              static_cast<uint16_t>(state.m_vi[1]) == 0 &&
              static_cast<uint16_t>(state.m_vi[2]) == 1,
              "VU0 branch reads current VI after the five-cycle stall expires the backup");
    }
    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        state.m_vi[1] = 1u;
        install_vu0_pair(&state, 0, make_vu0_iaddiu(1, 0, 0),
                         make_vu0_upper_nop());
        install_vu0_pair(&state, 1, make_vu0_iaddiu(2, 0, 2),
                         make_vu0_upper_nop());
        install_vu0_pair(&state, 2, make_vu0_branch(0x28, 1, 0, 1),
                         make_vu0_upper_nop());

        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &cpu, &state, 3,
            ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x0010200cu
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_branch == 1 && state.m_branch_pc == 32 &&
              state.m_vi_backup_register == 2,
              "an intervening VI write replaces the backup register before a later branch");
    }
    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        state.m_vi[1] = 1u;
        state.m_vi[2] = 1u;
        install_vu0_pair(&state, 0, make_vu0_iaddiu(1, 0, 0),
                         make_vu0_upper_nop());
        install_vu0_pair(&state, 1, make_vu0_iaddiu(1, 0, 5),
                         make_vu0_upper_nop());
        install_vu0_pair(&state, 2, make_vu0_branch(0x28, 1, 2, 1),
                         make_vu0_upper_nop());

        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &cpu, &state, 3,
            ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x00102010u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_branch == 1 && state.m_branch_pc == 32 &&
              static_cast<uint16_t>(state.m_vi_backup_old_value) == 1,
              "consecutive writes to one VI preserve the value from before the write chain");
    }
    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        install_vu0_pair(&state, 0, make_vu0_iaddiu(1, 0, 1),
                         make_vu0_upper_nop(0x20000000u)); // M flag

        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &cpu, &state, 10,
            ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x00102004u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_m_bit == 1 && state.m_cycle == 1 &&
              state.m_micro_pc == 8 && (state.m_vpu_status & 1u) != 0,
              "VU0 engine slice returns at M even without a wait-at-M request");
        check(state.m_ialu_count == 0,
              "zero-latency integer ALU leaves no IALU reservation at M");
    }
    {
        // M ends one interpreter Execute() slice, not a FINISH_TO_E wait.
        // The scheduler resumes a fresh slice, then observes E after its delay.
        ee::s_ee_vu0_state state{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        ee::s_ee_vu0_scheduler scheduler{};
        scheduler.m_user = &ialu_engine;
        scheduler.m_execute = ee::ee_vu0_execute_ialu_callback;
        scheduler.m_vu1_catch_up = mock_ialu_vu1_catch_up;
        scheduler.m_schedule_event = mock_ialu_schedule_event;
        scheduler.m_max_run_calls = 8;

        state.m_vpu_status = 1u;
        state.m_ee_cycle = 20;
        install_vu0_pair(&state, 0, make_vu0_iaddiu(1, 0, 1),
                         make_vu0_upper_nop(0x20000000u)); // M
        install_vu0_pair(&state, 1, make_vu0_iaddiu(2, 0, 2),
                         make_vu0_upper_nop());
        install_vu0_pair(&state, 2, make_vu0_iaddiu(3, 0, 3),
                         make_vu0_upper_nop(0x40000000u)); // E
        install_vu0_pair(&state, 3, make_vu0_iaddiu(4, 0, 4),
                         make_vu0_upper_nop()); // E delay
        install_vu0_pair(&state, 4, make_vu0_iaddiu(5, 0, 5),
                         make_vu0_upper_nop()); // Must not execute.

        const uint32_t result = ee::ee_vu0_scheduler_sync_callback(
            &scheduler, &cpu, &state,
            static_cast<uint32_t>(e_ee_vu0_sync_mode::FINISH_TO_E),
            0x00102006u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
              "finish-to-E resumes after M and completes at E");
        check(state.m_cycle == 4,
              "finish-to-E does not add a flush cycle for zero-latency integer ALU");
        check(state.m_ee_cycle == 24,
              "finish-to-E accounts only the four executed VU0 pairs on the EE");
        check(state.m_micro_pc == 32,
              "finish-to-E preserves the E delay pair's next micro-PC");
        check(static_cast<uint16_t>(state.m_vi[1]) == 1 &&
              static_cast<uint16_t>(state.m_vi[2]) == 2 &&
              static_cast<uint16_t>(state.m_vi[3]) == 3 &&
              static_cast<uint16_t>(state.m_vi[4]) == 4 &&
              static_cast<uint16_t>(state.m_vi[5]) == 0,
              "finish-to-E resumes after M and stops after the E delay");
        check((state.m_vpu_status & 1u) == 0 && state.m_m_bit == 0 &&
              state.m_ialu_count == 0,
              "finish-to-E clears running state and flushes pending IALU writes");
    }
    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        install_vu0_pair(&state, 0, make_vu0_iaddiu(1, 0, 1),
                         make_vu0_upper_nop(0x40000000u)); // E flag
        install_vu0_pair(&state, 1, make_vu0_iaddiu(2, 0, 2),
                         make_vu0_upper_nop());
        install_vu0_pair(&state, 2, make_vu0_iaddiu(3, 0, 3),
                         make_vu0_upper_nop());

        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &cpu, &state, 10,
            ee::EE_VU0_EXECUTE_STOP_AT_E, 0x00102008u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_cycle == 2 && state.m_micro_pc == 16 && state.m_tpc == 2,
              "E flag terminates after its two-pair delay without a spurious ALU flush cycle");
        check((state.m_vpu_status & 1u) == 0 && state.m_running == 0 &&
              state.m_e_bit == 0 && state.m_ialu_count == 0,
              "E termination clears VU0 running state with no pending zero-latency ALU writes");
        check(static_cast<uint16_t>(state.m_vi[1]) == 1 &&
              static_cast<uint16_t>(state.m_vi[2]) == 2 &&
              static_cast<uint16_t>(state.m_vi[3]) == 0,
              "E delay executes one following pair but not the subsequent pair");
    }
    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        install_vu0_pair(&state, 0, (0x40u << 25) | (16u << 6) | 0x3cu,
                         make_vu0_upper_nop()); // RNEXT remains unsupported.

        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &cpu, &state, 4,
            ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x0010200cu
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE) &&
              state.m_micro_pc == 0 && state.m_cycle == 0 &&
              state.m_vi[1] == 0,
              "unsupported lower operation leaves the pair unconsumed");
    }
    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        install_vu0_pair(&state, 0, make_vu0_iaddiu(1, 0, 1), 0x00000030u);

        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &cpu, &state, 1,
            ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x00102010u
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE) &&
              state.m_micro_pc == 0 && state.m_cycle == 0 &&
              state.m_vi[1] == 0,
              "unsupported upper operation leaves the pair unconsumed");
    }

    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_cpu_state vector_cpu{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        state.m_vi[1] = 1u; // Load/store address is one quadword from base.
        vector_cpu.m_vu0[2].m_lo = 0x1122334455667788ull;
        vector_cpu.m_vu0[2].m_hi = 0x99aabbccddeeff00ull;
        vector_cpu.m_vu0[3].m_lo = 0xaaaabbbbccccddddull;
        vector_cpu.m_vu0[3].m_hi = 0xeeeeffff12345678ull;
        // SQ uses FT as the VI base and FS as the VF source, unlike LQ.
        install_vu0_pair(&state, 0, make_vu0_memory(0x01, 0xau, 1, 2, 1),
                         make_vu0_upper_nop()); // SQ X/Z to quadword 2.
        install_vu0_pair(&state, 1, make_vu0_memory(0x00, 0xfu, 3, 1, 1),
                         make_vu0_upper_nop()); // LQ all lanes from quadword 2.
        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &vector_cpu, &state, 2,
            ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x0010200cu
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_cycle == 2 && state.m_micro_pc == 16,
              "LQ/SQ execute as ordinary masked quadword memory operations");
        check(vector_cpu.m_vu0[3].m_lo == 0x0000000055667788ull &&
              vector_cpu.m_vu0[3].m_hi == 0x00000000ddeeff00ull &&
              vector_cpu.m_vu0[2].m_lo == 0x1122334455667788ull,
              "LQ/SQ respect the XYZW mask and preserve source VF");
        check(state.m_data_memory[32] == 0x88u &&
              state.m_data_memory[35] == 0x55u &&
              state.m_data_memory[40] == 0x00u &&
              state.m_data_memory[43] == 0xddu,
              "SQ stores selected VF lanes in little-endian order");
    }

    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_cpu_state vector_cpu{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        install_vu0_pair(&state, 0, make_vu0_memory(0x00, 0xf, 2, 0, 1),
                         make_vu0_upper_nop()); // LQ reserves FMAC for four cycles.
        install_vu0_pair(&state, 1, make_vu0_iaddiu(0, 0, 0),
                         make_vu0_upper_nop(0x40000000u)); // E
        install_vu0_pair(&state, 2, make_vu0_iaddiu(0, 0, 0),
                         make_vu0_upper_nop()); // E delay
        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &vector_cpu, &state, 10,
            ee::EE_VU0_EXECUTE_STOP_AT_E, 0x0010200cu
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_cycle == 5 && state.m_micro_pc == 24 &&
              state.m_vf_memory_ready_cycle == 0,
              "E termination drains outstanding LQ FMAC issue latency");
    }

    // PCSX2 stalls a store reading a lane still reserved by an earlier LQ,
    // but a disjoint SQ lane must not stall on that same destination.
    for (uint32_t store_mask : { 0x8u, 0x4u })
    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_cpu_state vector_cpu{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        state.m_data_memory[16] = 0x78u;
        state.m_data_memory[17] = 0x56u;
        state.m_data_memory[18] = 0x34u;
        state.m_data_memory[19] = 0x12u;
        vector_cpu.m_vu0[2].m_lo = 0xabcdef0011223344ull;
        install_vu0_pair(&state, 0, make_vu0_memory(0x00, 0x8u, 2, 0, 1),
                         make_vu0_upper_nop()); // LQ VF2.x
        install_vu0_pair(&state, 1, make_vu0_memory(0x01, store_mask, 0, 2, 2),
                         make_vu0_upper_nop()); // SQ VF2.x or VF2.y
        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &vector_cpu, &state, 2,
            ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x0010200cu
        );
        const bool overlap = store_mask == 0x8u;
        const uint32_t offset = overlap ? 32u : 36u;
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_cycle == (overlap ? 5u : 2u) &&
              state.m_micro_pc == 16u,
              overlap ? "SQ overlapping LQ VF lane waits for four-cycle FMAC" :
                        "SQ disjoint VF lane bypasses LQ reservation");
        check(state.m_data_memory[offset] == (overlap ? 0x78u : 0x00u) &&
              state.m_data_memory[offset + 1u] == (overlap ? 0x56u : 0xefu),
              overlap ? "dependent SQ writes loaded VF lane after interlock" :
                        "disjoint SQ writes preserved VF lane without stall");
    }

    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_cpu_state vector_cpu{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        state.m_vi[1] = 0xabcd0001u;
        state.m_data_memory[16] = 0x78u;
        state.m_data_memory[17] = 0x56u;
        state.m_data_memory[18] = 0x34u;
        state.m_data_memory[19] = 0x12u;
        install_vu0_pair(
            &state, 0, make_vu0_indexed_memory(0x3cu, 0x8u, 2, 1),
            make_vu0_upper_nop()
        ); // LQI VF2.x, (VI1)++
        install_vu0_pair(
            &state, 1, make_vu0_indexed_memory(0x3du, 0x8u, 1, 2),
            make_vu0_upper_nop()
        ); // SQI VF2.x, (VI1)++
        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &vector_cpu, &state, 2,
            ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x0010200cu
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_cycle == 5 && state.m_micro_pc == 16,
              "LQI to dependent SQI stalls on overlapping VF lane");
        check(vector_cpu.m_vu0[2].m_lo == 0x12345678ull &&
              state.m_vi[1] == 0xabcd0003u &&
              state.m_data_memory[32] == 0x78u &&
              state.m_data_memory[35] == 0x12u,
              "LQI loads before increment and SQI stores before increment");
        check(state.m_vi_backup_register == 1 &&
              state.m_vi_backup_old_value == 2,
              "indexed-memory VI backup refreshes after a dependent FMAC stall");
    }
    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_cpu_state vector_cpu{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        state.m_vi[1] = 2u;
        state.m_vi[2] = 3u;
        state.m_data_memory[16] = 0xabu;
        vector_cpu.m_vu0[4].m_lo = 0x1122334455667788ull;
        install_vu0_pair(
            &state, 0, make_vu0_indexed_memory(0x3eu, 0x8u, 3, 1),
            make_vu0_upper_nop()
        ); // LQD VF3.x, --(VI1)
        install_vu0_pair(
            &state, 1, make_vu0_indexed_memory(0x3fu, 0x4u, 2, 4),
            make_vu0_upper_nop()
        ); // SQD VF4.y, --(VI2)
        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &vector_cpu, &state, 2,
            ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x0010200cu
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_cycle == 2 && state.m_micro_pc == 16,
              "LQD/SQD decrement independent VI bases without VF stall");
        check(state.m_vi[1] == 1u && state.m_vi[2] == 2u &&
              static_cast<uint32_t>(vector_cpu.m_vu0[3].m_lo) == 0xabu &&
              state.m_data_memory[36] == 0x44u &&
              state.m_data_memory[39] == 0x11u,
              "LQD/SQD use decremented quadword addresses");
    }
    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_cpu_state vector_cpu{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        state.m_vi[1] = 0x0401u;
        install_vu0_pair(
            &state, 0, make_vu0_indexed_memory(0x3eu, 0xfu, 3, 1),
            make_vu0_upper_nop()
        ); // LQD would decrement to VU1-mapped quadword 0x0400.
        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &vector_cpu, &state, 1,
            ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x0010200cu
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE) &&
              state.m_vi[1] == 0x0401u && state.m_cycle == 0 &&
              state.m_micro_pc == 0 && state.m_vi_backup_cycles == 0,
              "VU1-mapped indexed-memory address fails before VI update");
    }
    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_cpu_state vector_cpu{};
        ee::s_ee_vu0_ialu_engine ialu_engine{};
        state.m_vpu_status = 1u;
        state.m_vi[1] = 7u;
        install_vu0_pair(
            &state, 0, make_vu0_indexed_memory(0x3cu, 0xfu, 0, 1),
            make_vu0_upper_nop()
        ); // LQI VF0 ignores the load but increments VI1.
        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &ialu_engine, &vector_cpu, &state, 1,
            ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0x0010200cu
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_vi[1] == 8u && vector_cpu.m_vu0[0].m_lo == 0,
              "LQI VF0 suppresses vector writes without suppressing base update");
    }

    // The generic four-entry IALU interlock queue is used by lower
    // operations with nonzero latency (for example ILW/ILWR). The scalar
    // IADD-family operations have zero pipeline latency and bypass it.
    {
        ee::s_ee_vu0_state state{};
        state.m_cycle = 10;

        check(ee::ee_vu0_ialu_schedule_write(&state, 1u << 3, 2) ==
                  static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
              "IALU schedules a VI destination");
        check(ee::ee_vu0_ialu_schedule_write(&state, 1u << 5, 4) ==
                  static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
              "IALU queues a second destination");
        check(ee::ee_vu0_ialu_schedule_write(&state, 1u << 7, 3) ==
                  static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
              "IALU queues a third destination");
        check(state.m_ialu_count == 3 && state.m_ialu_read_pos == 0 &&
              state.m_ialu_write_pos == 3,
              "IALU queue maintains FIFO count and positions");

        check(ee::ee_vu0_ialu_wait_for_reads(&state, 1u << 4) ==
                  static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_cycle == 10,
              "IALU read of an unrelated VI register does not stall");
        check(ee::ee_vu0_ialu_wait_for_reads(&state, 1u << 3) ==
                  static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_cycle == 12,
              "IALU read stall advances to the matching destination ready cycle");
        check(state.m_ialu_count == 3,
              "IALU read stall does not retire queue entries implicitly");

        uint32_t retired = 0xffffffffu;
        check(ee::ee_vu0_ialu_retire_matured(&state, &retired) ==
                  static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              retired == 1 && state.m_ialu_count == 2 &&
              state.m_ialu_read_pos == 1,
              "IALU retirement removes only the matured FIFO head");

        check(ee::ee_vu0_ialu_wait_for_reads(
                  &state, (1u << 5) | (1u << 7)) ==
                  static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_cycle == 14,
              "IALU read stall waits through the latest overlapping reservation");
        check(ee::ee_vu0_ialu_retire_matured(&state, &retired) ==
                  static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              retired == 2 && state.m_ialu_count == 0 &&
              state.m_ialu_read_pos == state.m_ialu_write_pos,
              "IALU retirement drains all matured reservations in FIFO order");

        check(ee::ee_vu0_ialu_schedule_write(&state, 0, 3) ==
                  static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              ee::ee_vu0_ialu_schedule_write(&state, 1u << 8, 0) ==
                  static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_ialu_count == 0,
              "IALU ignores empty masks and zero-cycle reservations");
    }
    {
        ee::s_ee_vu0_state state{};
        state.m_cycle = 20;
        for (uint32_t i = 0; i < 4; ++i)
        {
            check(ee::ee_vu0_ialu_schedule_write(
                      &state, 1u << (i + 1), 2) ==
                      static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
                  "IALU fills each slot in its four-entry ring");
        }

        const uint32_t write_pos = state.m_ialu_write_pos;
        check(state.m_ialu_count == 4 && write_pos == 0,
              "IALU ring wraps write position after four enqueues");
        check(ee::ee_vu0_ialu_schedule_write(&state, 1u << 9, 2) ==
                  static_cast<uint32_t>(e_ee_vu0_sync_result::FAILED) &&
              state.m_ialu_count == 4 && state.m_ialu_write_pos == write_pos,
              "IALU full queue rejects overflow without corrupting ring state");

        state.m_cycle = 22;
        uint32_t retired = 0;
        check(ee::ee_vu0_ialu_retire_matured(&state, &retired) ==
                  static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              retired == 4 && state.m_ialu_count == 0 &&
              state.m_ialu_read_pos == state.m_ialu_write_pos,
              "IALU queue drains correctly across ring wrap");

        check(ee::ee_vu0_ialu_schedule_write(&state, 1u << 10, 3) ==
                  static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_ialu_count == 1 && state.m_ialu_write_pos == 1,
              "IALU ring accepts a new reservation after wrap and drain");
    }
    {
        ee::s_ee_vu0_state state{};
        state.m_cycle = 30;
        check(ee::ee_vu0_ialu_schedule_write(&state, 1u << 2, 3) ==
                  static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              ee::ee_vu0_ialu_schedule_write(&state, 1u << 4, 7) ==
                  static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
              "IALU schedules reservations for flush-all");
        check(ee::ee_vu0_ialu_flush_all(&state) ==
                  static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_cycle == 37 && state.m_ialu_count == 0 &&
              state.m_ialu_read_pos == state.m_ialu_write_pos,
              "IALU flush-all advances to the final ready cycle and empties queue");
        check(ee::ee_vu0_ialu_flush_all(nullptr) ==
                  static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT),
              "IALU flush-all rejects missing state");
        check(ee::ee_vu0_ialu_retire_matured(&state, nullptr) ==
                  static_cast<uint32_t>(e_ee_vu0_sync_result::INVALID_ARGUMENT),
              "IALU retirement rejects missing output counter");
    }

    // Integer VU0 data-memory slice: PCSX2 uses quadword addressing,
    // lane masks, 16-bit integer values and zeroed upper store halfwords.
    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_vu0_ialu_engine engine{};
        state.m_vpu_status = 1;
        state.m_running = 1;
        state.m_vi[1] = 1;                  // quadword base address
        state.m_vi[2] = 0xabcd1234u;       // store source
        state.m_vi[3] = 0xface0000u;       // load destination
        install_vu0_pair(
            &state, 0, make_vu0_memory(0x05u, 0xau, 2, 1, 1),
            make_vu0_upper_nop()
        );
        install_vu0_pair(
            &state, 1, make_vu0_memory(0x04u, 0xau, 3, 1, 1),
            make_vu0_upper_nop()
        );
        const uint32_t result = ee::ee_vu0_execute_ialu_callback(
            &engine, &cpu, &state, 2, ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0
        );
        check(result == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE),
              "VU0 ISW/ILW execute through the bounded micro-engine");
        check(state.m_data_memory[32] == 0x34u &&
              state.m_data_memory[33] == 0x12u &&
              state.m_data_memory[34] == 0 &&
              state.m_data_memory[35] == 0 &&
              state.m_data_memory[36] == 0 &&
              state.m_data_memory[40] == 0x34u &&
              state.m_data_memory[41] == 0x12u &&
              state.m_data_memory[42] == 0 &&
              state.m_data_memory[43] == 0,
              "ISW honors XYZW lane mask and clears each stored word's upper half");
        check(state.m_vi[3] == 0xface1234u,
              "ILW loads the selected 16-bit lane and preserves upper VI bits");
        check(state.m_ialu_count == 1 &&
              state.m_ialu[0].m_register_mask == (1u << 3) &&
              state.m_ialu[0].m_latency == 4,
              "ILW reserves its destination for four cycles");
    }
    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_vu0_ialu_engine engine{};
        state.m_vpu_status = 1;
        state.m_vi[1] = 0x100u;            // 0x100 * 16 wraps to data byte 0
        state.m_data_memory[0] = 0x78u;
        state.m_data_memory[1] = 0x56u;
        install_vu0_pair(
            &state, 0, make_vu0_memory(0x04u, 8u, 2, 1, 0),
            make_vu0_upper_nop()
        );
        check(ee::ee_vu0_execute_ialu_callback(
                  &engine, &cpu, &state, 1, ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0
              ) == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_vi[2] == 0x5678u,
              "ILW wraps ordinary VU0 quadword addresses at 4 KB");

        ee::s_ee_vu0_state mapped{};
        mapped.m_vpu_status = 1;
        mapped.m_vi[1] = 0x400;            // 0x400 * 16 is VU1 register window
        install_vu0_pair(
            &mapped, 0, make_vu0_memory(0x05u, 8u, 2, 1, 0),
            make_vu0_upper_nop()
        );
        check(ee::ee_vu0_execute_ialu_callback(
                  &engine, &cpu, &mapped, 1, ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0
              ) == static_cast<uint32_t>(e_ee_vu0_sync_result::UNAVAILABLE) &&
              mapped.m_micro_pc == 0 && mapped.m_cycle == 0 &&
              mapped.m_data_memory[0] == 0,
              "VU1 mapped register-window stores fail closed without mutation");
    }

    // In PCSX2, VUPIPE_IALU records ILW's four-cycle destination hazard,
    // but only VUPIPE_BRANCH actually waits for that IALU read hazard.
    {
        ee::s_ee_vu0_state state{};
        ee::s_ee_vu0_ialu_engine engine{};
        state.m_vpu_status = 1;
        state.m_data_memory[0] = 0x34;
        state.m_data_memory[1] = 0x12;
        install_vu0_pair(
            &state, 0, make_vu0_memory(0x04u, 8u, 2, 0, 0),
            make_vu0_upper_nop()
        );
        install_vu0_pair(
            &state, 1, make_vu0_memory(0x05u, 8u, 2, 0, 1),
            make_vu0_upper_nop()
        );
        install_vu0_pair(
            &state, 2, make_vu0_branch(0x28u, 2, 0, 1),
            make_vu0_upper_nop()
        );
        check(ee::ee_vu0_execute_ialu_callback(
                  &engine, &cpu, &state, 2,
                  ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0
              ) == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_cycle == 2 && state.m_ialu_count == 1 &&
              state.m_data_memory[16] == 0x34 &&
              state.m_data_memory[17] == 0x12,
              "ISW after ILW reads the loaded VI without an IALU pipe stall");
        check(ee::ee_vu0_execute_ialu_callback(
                  &engine, &cpu, &state, 1,
                  ee::EE_VU0_EXECUTE_EXACT_CYCLES, 0
              ) == static_cast<uint32_t>(e_ee_vu0_sync_result::COMPLETE) &&
              state.m_cycle == 5 && state.m_ialu_count == 0,
              "dependent VU0 integer branch waits for ILW's four-cycle pipe");
    }

    std::printf("vu0_runtime_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
