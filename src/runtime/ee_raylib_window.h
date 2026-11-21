#pragma once

#include "ee_gs_display.h"

#include <cstdint>

namespace ee
{
class c_ee_webgpu_bridge;
class c_ee_gs_presentation_probe;

// raylib owns the window and host input; WebGPU owns GS rendering and swapchain.
class c_ee_raylib_window final
{
public:
    c_ee_raylib_window() = default;
    ~c_ee_raylib_window();

    c_ee_raylib_window(const c_ee_raylib_window&) = delete;
    c_ee_raylib_window& operator=(const c_ee_raylib_window&) = delete;

    bool initialize();
    void pump(c_ee_webgpu_bridge& bridge,
        const s_ee_gs_display_registers& registers,
        uint32_t crt_mode, uint64_t field_count,
        c_ee_gs_presentation_probe* probe);

private:
    bool m_ready = false;
    bool m_closed = false;
    uint64_t m_last_fields = 0u;
    double m_last_host_poll = 0.0;
};
}
