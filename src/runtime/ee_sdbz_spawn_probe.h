#pragma once

#include "ee_memory.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace ee
{
// Read-only USA spawn samples. The six-word signature guards the known
// original PCs; it is not a complete ELF identity verification.
class c_ee_sdbz_spawn_probe
{
public:
    struct s_sample
    {
        const char* m_kind = nullptr;
        uint64_t m_dispatch = 0u;
        uint32_t m_pc = 0u;
        uint32_t m_ra = 0u;
        uint32_t m_object = 0u;
        uint32_t m_address = 0u;
        bool m_valid = false;
        std::array<uint32_t, 4> m_words{};
    };

    void initialize(ee_memory* memory)
    {
        m_enabled = memory != nullptr &&
            match(memory, 0x0023af30u, 0x27bdfe90u) &&
            match(memory, 0x0023b15cu, 0x0c041a54u) &&
            match(memory, 0x0023b264u, 0x0320f809u) &&
            match(memory, 0x0023b280u, 0x0320f809u) &&
            match(memory, 0x0010697cu, 0x4bc4216au) &&
            match(memory, 0x0030e300u, 0x27bdff50u);
    }

    void observe(uint32_t pc, uint64_t dispatch, const s_ee_cpu_state& cpu,
                 ee_memory* memory)
    {
        if (!m_enabled || memory == nullptr)
        {
            return;
        }
        const uint32_t ra = static_cast<uint32_t>(cpu.m_gpr[31].m_lo);
        const uint32_t sp = static_cast<uint32_t>(cpu.m_gpr[29].m_lo);
        const uint32_t a0 = static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
        const uint32_t a1 = static_cast<uint32_t>(cpu.m_gpr[5].m_lo);
        if (pc == 0x0023af30u)
        {
            append("spawn_stage", dispatch, pc, ra, a1, a0, memory);
        }
        else if (pc == 0x00106950u && ra == 0x0023b164u)
        {
            append("normalize_input", dispatch, pc, ra, 0u, a0, memory);
        }
        else if (pc == 0x0023b164u && sp <= 0xfffffebfu)
        {
            append("normalize_output", dispatch, pc, ra, 0u, sp + 0x140u, memory);
            append("spawn_midpoint", dispatch, pc, ra, 0u, sp + 0x130u, memory);
        }
        else if (pc == 0x0023b23cu && sp <= 0xfffffe9fu)
        {
            const uint32_t player = static_cast<uint32_t>(cpu.m_gpr[21].m_lo);
            const uint32_t index = static_cast<uint32_t>(cpu.m_gpr[2].m_lo);
            append(index == 0u ? "spawn_selected_minus" : "spawn_candidate_minus",
                   dispatch, pc, ra, player, sp + 0x160u, memory);
            append(index != 0u ? "spawn_selected_plus" : "spawn_candidate_plus",
                   dispatch, pc, ra, player, sp + 0x150u, memory);
        }
        else if (ra == 0x0023b26cu || ra == 0x0023b288u ||
                 ra == 0x0030e3ecu || ra == 0x0030e438u || ra == 0x0030e488u)
        {
            // A link register alone is not a call boundary: it persists
            // inside a callee. Require PC to equal the virtual slot target.
            if (is_position_setter(pc, a0, memory))
            {
                append(ra == 0x0023b26cu || ra == 0x0023b288u
                           ? "spawn_setter" : "post_spawn_setter",
                       dispatch, pc, ra, a0, a1, memory);
            }
        }
    }

    bool enabled() const { return m_enabled; }
    size_t size() const { return m_size; }
    uint64_t dropped() const { return m_dropped; }
    const s_sample& sample(size_t index) const { return m_samples[index]; }

    void write_json(FILE* output) const
    {
        if (output == nullptr)
        {
            return;
        }
        std::fprintf(output,
            "{\"schema\":\"sdbz-us-spawn-boundaries-v1\",\"signature_match\":%s,"
            "\"dropped\":%llu,\"samples\":[",
            m_enabled ? "true" : "false",
            static_cast<unsigned long long>(m_dropped));
        for (size_t i = 0; i < m_size; ++i)
        {
            const auto& s = m_samples[i];
            std::fprintf(output,
                "%s{\"kind\":\"%s\",\"dispatch\":%llu,\"pc\":%u,\"ra\":%u,"
                "\"object\":%u,\"address\":%u,\"valid\":%s,"
                "\"words\":[%u,%u,%u,%u]}",
                i == 0u ? "" : ",", s.m_kind,
                static_cast<unsigned long long>(s.m_dispatch),
                s.m_pc, s.m_ra, s.m_object, s.m_address,
                s.m_valid ? "true" : "false", s.m_words[0], s.m_words[1],
                s.m_words[2], s.m_words[3]);
        }
        std::fputs("],\"limitations\":[\"dispatch_boundaries_only\","
                   "\"position_arguments_not_velocity\",\"first_128_samples\"]}", output);
    }

private:
    static bool match(ee_memory* memory, uint32_t pc, uint32_t word)
    {
        return memory->contains(pc, 4u) && memory->read32(pc) == word;
    }

    static bool is_position_setter(uint32_t pc, uint32_t object, ee_memory* memory)
    {
        if (object == 0u || !memory->contains(object, 4u))
        {
            return false;
        }
        const uint32_t table = memory->read32(object);
        return table != 0u && table <= 0xffffffbcu &&
            memory->contains(table + 0x40u, 4u) &&
            memory->read32(table + 0x40u) == pc;
    }

    void append(const char* kind, uint64_t dispatch, uint32_t pc, uint32_t ra,
                uint32_t object, uint32_t address, ee_memory* memory)
    {
        if (m_size == m_samples.size())
        {
            ++m_dropped;
            return;
        }
        auto& s = m_samples[m_size++];
        s.m_kind = kind;
        s.m_dispatch = dispatch;
        s.m_pc = pc;
        s.m_ra = ra;
        s.m_object = object;
        s.m_address = address;
        s.m_valid = address != 0u && address <= 0xfffffff0u &&
            memory->contains(address, 16u);
        if (s.m_valid)
        {
            for (uint32_t i = 0u; i < 4u; ++i)
            {
                s.m_words[i] = memory->read32(address + 4u * i);
            }
        }
    }

    bool m_enabled = false;
    size_t m_size = 0u;
    uint64_t m_dropped = 0u;
    std::array<s_sample, 128> m_samples{};
};
} // namespace ee
