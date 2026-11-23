#include "ee_memory.h"
#include "ee_runtime.h"

#include <cstdint>
#include <cstdio>

int main()
{
    ee::ee_memory memory(4096);
    memory.write32(0x20u, 0x12345678u);
    if (memory.contains(0x80000020u, 4u) ||
        memory.read32(0xa0000020u) != 0u)
    {
        std::fprintf(stderr, "KSEG RAM aliases must be opt-in\n");
        return 1;
    }

    memory.set_kseg_ram_aliases(true);
    if (!memory.contains(0x80000020u, 4u) ||
        !memory.contains(0xa0000020u, 4u) ||
        memory.read32(0x80000020u) != 0x12345678u ||
        memory.read32(0xa0000020u) != 0x12345678u)
    {
        std::fprintf(stderr, "KSEG RAM mirrors do not map to EE physical RAM\n");
        return 1;
    }

    memory.write64(0xa0000030u, 0x1122334455667788ull);
    if (memory.read64(0x80000030u) != 0x1122334455667788ull ||
        memory.read64(0x30u) != 0x1122334455667788ull)
    {
        std::fprintf(stderr, "KSEG writes do not alias normal EE RAM\n");
        return 1;
    }

    ee::s_ee_u128 quad{};
    quad.m_lo = 0xabcdef0123456789ull;
    quad.m_hi = 0xfedcba9876543210ull;
    memory.write128(0x80000040u, &quad);
    ee::s_ee_u128 result{};
    memory.read128(0xa0000040u, &result);
    if (result.m_lo != quad.m_lo || result.m_hi != quad.m_hi)
    {
        std::fprintf(stderr, "KSEG 128-bit memory aliases failed\n");
        return 1;
    }

    if (memory.contains(0x80000ffcu, 8u) ||
        memory.contains(0x80100000u, 4u) ||
        memory.contains(0xb0000000u, 4u) ||
        memory.contains(0xffffffffu, 4u))
    {
        std::fprintf(stderr, "invalid KSEG RAM address accepted\n");
        return 1;
    }

    memory.set_kseg_ram_aliases(false);
    if (memory.contains(0x80000020u, 4u))
    {
        std::fprintf(stderr, "KSEG aliases cannot be disabled\n");
        return 1;
    }
    // Reaching the initial console-print limit must not stop the recent
    // host-side guest STORE ring. Its entries are pre-store SSA operands.
    ee::s_ee_guest_store_trace_entry recent[3]{};
    ee::s_ee_runtime runtime{};
    runtime.m_guest_store_trace_enabled = true;
    runtime.m_guest_store_trace_word = 0x80u;
    runtime.m_guest_store_trace_remaining = 0u;
    runtime.m_guest_store_trace_recent = recent;
    runtime.m_guest_store_trace_capacity = 3u;
    for (uint32_t index = 0u; index < 5u; ++index)
    {
        ee::ee_trace_guest_store(
            &runtime, 0x1000u + index * 4u, 0xac88000cu,
            0x80u, 0x80u, 4u, index + 1u, 0u,
            0x80u, 0x2000u, 0x3000u, 0x4000u
        );
    }
    ee::ee_trace_guest_store(
        &runtime, 0x2000u, 0xac88000cu,
        0x100u, 0x100u, 4u, 99u, 0u,
        0x100u, 0x2000u, 0x3000u, 0x4000u
    );
    if (runtime.m_guest_store_trace_count != 3u ||
        runtime.m_guest_store_trace_next != 2u ||
        runtime.m_guest_store_trace_remaining != 0u ||
        recent[2].m_pc != 0x1008u ||
        recent[0].m_pc != 0x100cu ||
        recent[1].m_pc != 0x1010u ||
        recent[1].m_source_value != 5u ||
        recent[1].m_ra != 0x4000u)
    {
        std::fprintf(stderr, "guest STORE recent ring did not wrap correctly\n");
        return 1;
    }
    std::printf("EE KSEG0/KSEG1 RAM alias and guest-store ring regression: PASS\n");
    return 0;
}
