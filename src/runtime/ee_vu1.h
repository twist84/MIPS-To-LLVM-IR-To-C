// M2.7 bounded VU1 interpreter, based on ps2-web-recomp:SDBZ
// runtime/src/vu_micro.cpp (Samir Nuri, 2026, MIT).
// License: src/runtime/ps2_webgpu/gs/LICENSE.upstream
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace ee
{
struct s_ee_vu1_state
{
    std::array<std::array<uint32_t, 4>, 32> m_vf{};
    std::array<uint16_t, 16> m_vi{};
    std::array<uint32_t, 4> m_acc{};
    uint32_t m_i = 0u;
    uint32_t m_q = 0u;
    uint32_t m_p = 0u;
    uint32_t m_r = 0u;
    uint32_t m_mac = 0u;
    uint32_t m_status = 0u;
    uint32_t m_clip = 0u;
    uint32_t m_tpc = 0u;
    bool m_initialized = false;
    uint32_t m_acc_overflow_mask = 0u;
};

// Single value owner for VU1 code, data and execution context.
// Worker requests and staged VIF transactions never borrow guest MMIO pointers.
struct s_ee_vu1_resident
{
    std::array<uint8_t, 16384u> m_data{};
    std::array<uint8_t, 16384u> m_code{};
    s_ee_vu1_state m_state{};
};

enum class e_ee_vu1_kick_reject : uint32_t
{
    none = 0u,
    packet_qwc_limit,
    missing_eop,
    batch_qwc_limit
};

struct s_ee_vu1_issue
{
    uint32_t m_pc = 0u;
    uint32_t m_lower = 0u;
    uint32_t m_upper = 0u;
    uint32_t m_steps = 0u;
    uint32_t m_cycles = 0u;
    e_ee_vu1_kick_reject m_kick_reject = e_ee_vu1_kick_reject::none;
    uint32_t m_kick_address = 0u;
    uint32_t m_kick_tag_address = 0u;
    uint32_t m_kick_packet_qwc = 0u;
    uint32_t m_kick_staged_qwc = 0u;
    uint64_t m_kick_tag = 0u;
};

// Deliberately a strict subset, NOT a general-purpose VU1 interpreter.
// Unsupported microinstructions/hazards reject the entire staged VIF transfer.
// No GS output or resident state becomes visible on a rejected run.
struct s_ee_vu1_execution;

enum class e_ee_vu1_slice
{
    pending,
    completed,
    rejected
};

class c_ee_vu1
{
public:
    static constexpr uint32_t k_memory_size = 16384u;
    static constexpr uint32_t k_max_steps = 100000u;

    // Shared validated packet transfer for both interpreter and AOT.
    static bool kick(const std::array<uint8_t, k_memory_size>& data,
        uint32_t address, std::vector<uint8_t>& output, s_ee_vu1_issue& issue);

    // Shared EFU arithmetic, including host-specific transcendental results.
    static bool efu(const std::array<uint32_t, 4>& source, uint32_t index,
        uint32_t component, uint32_t& value, uint32_t& latency);

    // A direct launch may run indefinitely; preserve the full interpreter
    // pipeline across slices. Synchronous VIF runs keep their E-bit limit.
    static std::shared_ptr<s_ee_vu1_execution> begin_execution(
        s_ee_vu1_state& state, uint32_t address);
    static e_ee_vu1_slice run_slice(
        s_ee_vu1_execution& execution, s_ee_vu1_state& state,
        std::array<uint8_t, k_memory_size>& code,
        std::array<uint8_t, k_memory_size>& data,
        uint32_t top, uint32_t itop, uint32_t pair_budget,
        std::vector<uint8_t>& path1_bytes, s_ee_vu1_issue* issue);

    static bool run(
        s_ee_vu1_state& state,
        std::array<uint8_t, k_memory_size>& code,
        std::array<uint8_t, k_memory_size>& data,
        uint32_t address, uint32_t top, uint32_t itop,
        std::vector<uint8_t>& path1_bytes, s_ee_vu1_issue* issue);
};
} // namespace ee
