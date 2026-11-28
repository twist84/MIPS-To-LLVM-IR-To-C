#pragma once

#include "ee_gs_display.h"
#include "ee_performance_overlay.h"

#include <cstdint>

namespace iop { class c_hw; }

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
    bool closed() const { return m_closed; }
    void pump(c_ee_webgpu_bridge& bridge,
        const s_ee_gs_display_registers& registers,
        uint32_t crt_mode, uint64_t field_count,
        c_ee_gs_presentation_probe* probe, iop::c_hw* iop_hardware,
        const s_ee_performance_counters& counters);
    bool consume_pc_profile_toggle();

private:
    c_ee_performance_overlay m_performance_overlay{};
    uint64_t m_presentations = 0u;
    bool m_ready = false;
    bool m_closed = false;
    bool m_gpu_profile_enabled = false;
    bool m_pc_profile_toggle_requested = false;
    uint64_t m_last_fields = 0u;
    double m_last_host_poll = 0.0;
    void* m_parent_window_handle = nullptr;
    void* m_native_window_handle = nullptr;
};
}
