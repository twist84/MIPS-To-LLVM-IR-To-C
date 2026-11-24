#pragma once

// Guest-independent, native-only WebGPU mirror of committed GIF/GS events.
// Requires EE_ENABLE_WEBGPU and wgpu-native. Never determines the
// success of guest DMA, interrupts, or GIF transaction commits.

#include "ee_gif.h"
#include "ee_gs_display.h"
#include "ps2_webgpu/gs/gs.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ee
{
class c_ee_webgpu_bridge
{
public:
    bool init();
    bool active() const;
    uint64_t rendered_primitives() const;

    // Called only by the CPU GS transaction sink AFTER an accepted batch.
    // This is a render mirror: it has no return path to the EE/DMAC.
    void consume(
        const s_ee_gif_event* events, size_t count,
        const uint8_t* data, uint32_t qwc
    );
    static void committed_gif_events(
        void* user, const s_ee_gif_event* events, size_t count,
        const uint8_t* data, uint32_t qwc
    );

    // Read-only validation against the same display-source decoder used
    // by present/readback. This does not submit any GPU work.
    bool supports_display(const s_ee_gs_display_registers& registers,
        uint32_t crt_mode) const;

    // Present only when the host observes a genuine guest scanout field.
    // An invalid GS display configuration cannot trigger a presentation.
    bool present(const s_ee_gs_display_registers& registers,
        uint32_t crt_mode, uint64_t guest_field = 0u);

    // GPU raster + GS display compute pass; no browser or window required.
    // A successful readback is a GPU-processed GS VRAM image, NOT proof
    // of a guest presentation/VBlank event.
    bool readback(
        const s_ee_gs_display_registers& registers, uint32_t crt_mode,
        std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height
    );

    // Read a FRAME_1/FRAME_2 drawing target directly from GPU VRAM using
    // the guest's actual DISPLAY crop. Does not alter any GS registers or
    // submit a guest present. Unsupported color formats fail closed.
    bool readback_frame(
        uint64_t frame, const s_ee_gs_display_registers& registers,
        uint32_t crt_mode, std::vector<uint32_t>& pixels,
        uint32_t& width, uint32_t& height
    );

private:
    void register_write(uint32_t reg, uint64_t value);
    void packed_write(uint32_t reg, uint64_t low, uint64_t high);
    void vertex(uint32_t x, uint32_t y, uint32_t z, uint8_t f, bool draw);
    void kick(bool draw);
    bool m_ready = false;
    uint64_t m_draw_count = 0u;
    ps2::gs::State m_state{};
};
}
