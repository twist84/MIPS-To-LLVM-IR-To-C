#pragma once

#include "ee_gs.h"

#include <cstdint>
#include <vector>

namespace ee
{

// Explicit privileged GS display-register snapshot. Kept separate from
// GIF A+D registers and all uncalibrated scanout/timing infrastructure.
struct s_ee_gs_display_registers
{
    uint64_t m_pmode = 0u;
    uint64_t m_smode2 = 0u;
    uint64_t m_dispfb[2]{};
    uint64_t m_display[2]{};
    uint64_t m_bgcolor = 0u;

    // MMIO decoder ordinal: PMODE, SMODE2, DISPFB1, DISPLAY1,
    // DISPFB2, DISPLAY2, BGCOLOR. Raw privileged latch snapshots;
    // BGCOLOR RGB is in bits 23:0. No composition or timing effects.
    uint64_t register_value(uint32_t index) const
    {
        switch (index)
        {
        case 0u: return m_pmode;
        case 1u: return m_smode2;
        case 2u: return m_dispfb[0];
        case 3u: return m_display[0];
        case 4u: return m_dispfb[1];
        case 5u: return m_display[1];
        case 6u: return m_bgcolor;
        default: return 0u;
        }
    }

    void set_register(uint32_t index, uint64_t value)
    {
        switch (index)
        {
        case 0u: m_pmode = value; break;
        case 1u: m_smode2 = value; break;
        case 2u: m_dispfb[0] = value; break;
        case 3u: m_display[0] = value; break;
        case 4u: m_dispfb[1] = value; break;
        case 5u: m_display[1] = value; break;
        case 6u: m_bgcolor = value; break;
        default: break;
        }
    }
};

class c_ee_gs_display
{
public:
    // Pure read-only VRAM display extraction, based on the reference
    // ps2-web-recomp display() circuit-selection and magnification rules.
    // The registers MUST come from guest GS writes in production. This
    // method does not synthesize a VBlank, framebuffer or present event.
    // No outputs change when the register state is unsupported.
    static bool readback(
        const c_ee_gs_state& gs,
        const s_ee_gs_display_registers& registers,
        uint32_t crt_mode,
        std::vector<uint32_t>& pixels,
        uint32_t& width,
        uint32_t& height
    );

    // Interpret only a *completed, real* GIF host-to-local transfer's
    // destination rectangle. A texture upload is NOT a rendered frame.
    // The transfer registers and pixel count must agree exactly.
    static bool readback_upload(
        const c_ee_gs_state& gs,
        std::vector<uint32_t>& pixels,
        uint32_t& width,
        uint32_t& height
    );
};
} // namespace ee
