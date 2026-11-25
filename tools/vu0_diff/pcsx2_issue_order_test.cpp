#include "Common.h"
#include "VUmicro.h"
#include "vu0_trace_fixture.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <fstream>
#include <string>

namespace
{

constexpr u32 VU0_LOWER_IBEQ = 0x28u;
constexpr u32 VU0_LOWER_IADDIU = 0x08u;
constexpr u32 VU0_UPPER_NOP = 0x3fu | (11u << 6);
constexpr u32 VU0_MICRO_PC_MASK = 0x0fffu;

void store_le32(u8* destination, u32 value)
{
    destination[0] = static_cast<u8>(value);
    destination[1] = static_cast<u8>(value >> 8);
    destination[2] = static_cast<u8>(value >> 16);
    destination[3] = static_cast<u8>(value >> 24);
}

u32 load_le32(const u8* source)
{
    return static_cast<u32>(source[0]) |
           (static_cast<u32>(source[1]) << 8) |
           (static_cast<u32>(source[2]) << 16) |
           (static_cast<u32>(source[3]) << 24);
}

u32 make_branch(u32 primary, u32 it, u32 is, u32 immediate)
{
    return (primary << 25) | (it << 16) | (is << 11) |
           (immediate & 0x07ffu);
}

u32 make_iaddiu(u32 it, u32 is, u32 immediate)
{
    return (VU0_LOWER_IADDIU << 25) | (it << 16) | (is << 11) |
           ((immediate & 0x7800u) << 10) | (immediate & 0x07ffu);
}

void write_vi(std::ostream& output)
{
    output << "\"vi\":[";
    for (u32 index = 0; index < 32; ++index)
    {
        if (index != 0)
            output << ',';
        output << VU0.VI[index].UL;
    }
    output << ']';
}

void write_vf(std::ostream& output)
{
    output << "\"vf\":[";
    for (u32 index = 0; index < 32; ++index)
    {
        if (index != 0)
            output << ',';
        output << '[' << VU0.VF[index].UL[0]
               << ',' << VU0.VF[index].UL[1]
               << ',' << VU0.VF[index].UL[2]
               << ',' << VU0.VF[index].UL[3] << ']';
    }
    output << ']';
}

void write_state(std::ostream& output)
{
    const u32 micro_pc = VU0.VI[REG_TPC].UL & VU0_MICRO_PC_MASK;
    write_vi(output);
    output << ',';
    write_vf(output);
    output << ",\"cycle\":" << VU0.cycle
           << ",\"micro_pc\":" << micro_pc
           << ",\"tpc\":" << ((micro_pc >> 3) & 0x1ffu)
           << ",\"branch\":" << VU0.branch
           << ",\"branch_pc\":" << VU0.branchpc
           << ",\"delay_branch_pc\":" << VU0.delaybranchpc
           << ",\"take_delay_branch\":"
           << (VU0.takedelaybranch ? "true" : "false")
           << ",\"vi_backup_cycles\":" << static_cast<u32>(VU0.VIBackupCycles)
           << ",\"vi_backup_register\":" << VU0.VIRegNumber
           << ",\"vi_backup_old_value\":" << VU0.VIOldValue
           << ",\"e_bit\":" << VU0.ebit
           << ",\"m_bit\":"
           << ((VU0.flags & VUFLAG_MFLAGSET) != 0 ? "true" : "false")
           << ",\"vpu_status\":" << VU0.VI[REG_VPU_STAT].UL
           << ",\"running\":"
           << ((VU0.VI[REG_VPU_STAT].UL & 1u) != 0 ? "true" : "false")
           << ",\"data_sha256\":\""
           << vu0_trace_fixture::sha256_hex(VU0.Mem, VU0_MEMSIZE)
           << "\"";
}

void write_header(
    std::ostream& output,
    const std::string& program_sha256
)
{
    output << "{\"schema\":\"mips-to-llvm-vu0-trace-v1\""
           << ",\"kind\":\"header\""
           << ",\"engine\":\"pcsx2-interpreter\""
           << ",\"revision\":\"82cac38b2af19d593dedfd9ddaa63448e7ec493c\""
           << ",\"program_sha256\":\"" << program_sha256
           << "\",\"initial_state\":{";
    write_state(output);
    output << "}}\n";
}

void write_step(
    std::ostream& output,
    u32 step,
    u32 pair_pc,
    u32 lower,
    u32 upper
)
{
    output << "{\"kind\":\"step\",\"step\":" << step
           << ",\"pc\":" << pair_pc
           << ",\"lower\":" << lower
           << ",\"upper\":" << upper
           << ",\"state\":{";
    write_state(output);
    output << "}}\n";
}

class VU0IssueOrder : public ::testing::Test
{
protected:
    std::array<u8, VU0_PROGSIZE> m_micro{};
    std::array<u8, VU0_MEMSIZE> m_data{};

    void SetUp() override
    {
        static_assert(std::is_trivially_copyable_v<VURegs>);
        // VURegs has a user-provided constructor that only clears Micro/Mem.
        // Value-initializing it does not reset VI/VF banks or pipeline state.
        // Clear the complete trivially-copyable state before assigning the
        // fixture memories so the PCSX2 trace starts from the candidate state.
        std::memset(&VU0, 0, sizeof(VU0));
        VU0.Micro = m_micro.data();
        VU0.Mem = m_data.data();
        VU0.VF[0].f.x = 0.0f;
        VU0.VF[0].f.y = 0.0f;
        VU0.VF[0].f.z = 0.0f;
        VU0.VF[0].f.w = 1.0f;
        VU0.VI[0].UL = 0;
        VU0.VI[REG_TPC].UL = 0;
        VU0.VI[REG_VPU_STAT].UL = 1;
    }

    void TearDown() override
    {
        VU0.Micro = nullptr;
        VU0.Mem = nullptr;
    }

    void install_pair(u32 byte_pc, u32 lower, u32 upper = VU0_UPPER_NOP)
    {
        store_le32(&m_micro[byte_pc], lower);
        store_le32(&m_micro[byte_pc + 4u], upper);
    }

    void seed_ialu_pipe(u32 index, u32 register_mask, u64 start_cycle, u32 latency)
    {
        VU0.ialu[index].reg = static_cast<int>(register_mask);
        VU0.ialu[index].sCycle = start_cycle;
        VU0.ialu[index].Cycle = latency;
    }

    void execute_one_pair()
    {
        VU0.flags &= ~VUFLAG_MFLAGSET;
        vu0Exec(&VU0);
    }
};

} // namespace

TEST_F(VU0IssueOrder, BranchInterlockExpiresBackupBeforePredicateRead)
{
    VU0.cycle = 10;
    VU0.VI[1].UL = 0;
    VU0.VI[0].UL = 0;
    VU0.VIBackupCycles = 1;
    VU0.VIRegNumber = 1;
    VU0.VIOldValue = 1;

    seed_ialu_pipe(0, 1u << 1, 10, 3);
    VU0.ialureadpos = 0;
    VU0.ialuwritepos = 1;
    VU0.ialucount = 1;

    install_pair(0, make_branch(VU0_LOWER_IBEQ, 1, 0, 2));

    execute_one_pair();

    EXPECT_EQ(VU0.cycle, 13u);
    EXPECT_EQ(VU0.VI[REG_TPC].UL, 8u);
    EXPECT_EQ(VU0.branch, 1u);
    EXPECT_EQ(VU0.branchpc, 24u);
    EXPECT_EQ(VU0.VIBackupCycles, 0u);
    EXPECT_EQ(VU0.ialucount, 0u);
}

TEST_F(VU0IssueOrder, TwoSourceInterlockWaitsForLatestCompletion)
{
    VU0.cycle = 20;
    VU0.VI[1].UL = 0;
    VU0.VI[2].UL = 1;
    VU0.VIBackupCycles = 1;
    VU0.VIRegNumber = 1;
    VU0.VIOldValue = 1;

    seed_ialu_pipe(0, 1u << 1, 20, 3);
    seed_ialu_pipe(1, 1u << 2, 20, 5);
    VU0.ialureadpos = 0;
    VU0.ialuwritepos = 2;
    VU0.ialucount = 2;

    install_pair(0, make_branch(VU0_LOWER_IBEQ, 1, 2, 2));

    execute_one_pair();

    EXPECT_EQ(VU0.cycle, 25u);
    EXPECT_EQ(VU0.VI[REG_TPC].UL, 8u);
    EXPECT_EQ(VU0.branch, 0u);
    EXPECT_EQ(VU0.VIBackupCycles, 0u);
    EXPECT_EQ(VU0.ialucount, 0u);
}

TEST_F(VU0IssueOrder, IALUWriteStartsFreshTwoCycleBackup)
{
    VU0.cycle = 30;
    VU0.VI[1].UL = 0xabcd0007u;

    install_pair(0, make_iaddiu(1, 0, 0x1234u));

    execute_one_pair();

    EXPECT_EQ(VU0.cycle, 31u);
    EXPECT_EQ(VU0.VI[1].US[0], 0x1234u);
    EXPECT_EQ(VU0.VIBackupCycles, 2u);
    EXPECT_EQ(VU0.VIRegNumber, 1u);
    EXPECT_EQ(VU0.VIOldValue, 7u);
}

TEST_F(VU0IssueOrder, DualIssueDestinationPriority)
{
    VU0.VI[1].UL = 3u;
    VU0.VI[REG_R].UL = 0x3f812345u;
    VU0.VF[1].UL[0] = 0xc0000000u;
    VU0.VF[2].UL[0] = 0x3f800000u;
    VU0.VF[2].UL[1] = 0x40400000u;
    VU0.VF[4].UL[0] = 0xc0800000u;
    const auto abs_x = [](uint32_t ft, uint32_t fs)
    {
        return (8u << 21u) | (ft << 16u) | (fs << 11u) |
            (7u << 6u) | 0x3du;
    };
    const auto lq_indexed = [](uint32_t secondary, uint32_t mask, uint32_t ft)
    {
        return 0x80000000u | (mask << 21u) | (ft << 16u) |
            (1u << 11u) | (13u << 6u) | secondary;
    };
    const uint32_t sq_y = (1u << 25u) | (4u << 21u) | (2u << 11u) | 1u;
    const uint32_t move_x = 0x80000000u | (8u << 21u) |
        (3u << 16u) | (2u << 11u) | (12u << 6u) | 0x3cu;
    const uint32_t rnext_x = 0x80000000u | (8u << 21u) |
        (2u << 16u) | (16u << 6u) | 0x3cu;
    install_pair(0, lq_indexed(0x3cu, 4u, 2u), abs_x(2, 1));
    install_pair(8, sq_y);
    install_pair(16, move_x, abs_x(2, 4));
    install_pair(24, rnext_x, abs_x(2, 1));
    install_pair(32, lq_indexed(0x3eu, 8u, 2u), abs_x(2, 4));
    install_pair(40, lq_indexed(0x3cu, 8u, 0u), abs_x(0, 1));

    execute_one_pair();
    EXPECT_EQ(VU0.cycle, 1u);
    EXPECT_EQ(VU0.VI[1].UL, 3u);
    EXPECT_EQ(VU0.VF[2].UL[0], 0x40000000u);
    EXPECT_EQ(VU0.VF[2].UL[1], 0x40400000u);
    execute_one_pair();
    EXPECT_EQ(VU0.cycle, 5u);
    EXPECT_EQ(load_le32(&m_data[20]), 0x40400000u);
    execute_one_pair();
    EXPECT_EQ(VU0.VF[3].UL[0], 0x40000000u);
    EXPECT_EQ(VU0.VF[2].UL[0], 0x40800000u);
    execute_one_pair();
    EXPECT_EQ(VU0.VI[REG_R].UL, 0x3f812345u);
    EXPECT_EQ(VU0.VF[2].UL[0], 0x40000000u);
    execute_one_pair();
    EXPECT_EQ(VU0.VI[1].UL, 3u);
    EXPECT_EQ(VU0.VF[2].UL[0], 0x40800000u);
    execute_one_pair();
    EXPECT_EQ(VU0.VI[1].UL, 4u);
    EXPECT_EQ(VU0.VF[0].UL[0], 0u);
    EXPECT_EQ(VU0.VF[0].UL[3], 0x3f800000u);
}

TEST_F(VU0IssueOrder, EmitDeterministicPerPairTrace)
{
    const char* trace_path = std::getenv("VU0_TRACE_OUTPUT");
    if (trace_path == nullptr || trace_path[0] == '\0')
    {
        GTEST_SKIP() << "Set VU0_TRACE_OUTPUT to write a PCSX2 trace file.";
    }

    std::ofstream output(trace_path, std::ios::out | std::ios::trunc);
    ASSERT_TRUE(output.good()) << "Cannot open VU0_TRACE_OUTPUT=" << trace_path;

    vu0_trace_fixture::install_program(m_micro.data(), static_cast<u32>(m_micro.size()));
    VU0.VI[REG_TPC].UL = 0;
    VU0.VI[REG_VPU_STAT].UL = 1;
    VU0.cycle = 0;
    VU0.branch = 0;
    VU0.branchpc = 0;
    VU0.delaybranchpc = 0;
    VU0.takedelaybranch = false;
    VU0.ebit = 0;
    VU0.VIBackupCycles = 0;
    VU0.VIRegNumber = 0;
    VU0.VIOldValue = 0;
    VU0.flags &= ~VUFLAG_MFLAGSET;
    const u32 vf2[4] = { 0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u };
    const u32 vf3[4] = { 0x01020304u, 0x05060708u, 0x090a0b0cu, 0x0d0e0f10u };
    for (u32 lane = 0; lane < 4; ++lane)
    {
        VU0.VF[2].UL[lane] = vf2[lane];
        VU0.VF[3].UL[lane] = vf3[lane];
    }

    const std::string program_sha256 = vu0_trace_fixture::sha256_hex(
        m_micro.data(), m_micro.size()
    );
    write_header(output, program_sha256);

    u32 step = 0;
    while ((VU0.VI[REG_VPU_STAT].UL & 1u) != 0 &&
           step < vu0_trace_fixture::k_step_limit)
    {
        const u32 pair_pc = VU0.VI[REG_TPC].UL & VU0_MICRO_PC_MASK;
        const u32 lower = load_le32(&m_micro[pair_pc]);
        const u32 upper = load_le32(&m_micro[pair_pc + 4u]);

        // A call to vu0Exec is one interpreter pair. Clear the M marker at
        // each boundary just as InterpVU0::Execute does before a new slice.
        VU0.flags &= ~VUFLAG_MFLAGSET;
        vu0Exec(&VU0);

        write_step(output, step, pair_pc, lower, upper);
        ++step;
    }

    output.flush();
    ASSERT_TRUE(output.good()) << "Failed writing PCSX2 trace " << trace_path;
    EXPECT_EQ(step, vu0_trace_fixture::k_expected_step_count);
    EXPECT_EQ(VU0.VI[REG_VPU_STAT].UL & 1u, 0u);
}
