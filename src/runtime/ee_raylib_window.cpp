#include "ee_raylib_window.h"

#include "ee_webgpu_bridge.h"
#include "ee_gs_presentation_probe.h"
#include "ps2_webgpu/gs/gpu/gpu.hpp"

#include <raylib.h>

#include <cstdio>
#include <cstdint>

extern "C" void* ee_win32_window_instance(void* window_handle);

namespace ee
{
c_ee_raylib_window::~c_ee_raylib_window()
{
    if (m_ready)
    {
        CloseWindow();
    }
}

bool c_ee_raylib_window::initialize()
{
    if (m_ready)
    {
        return true;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(960, 720, "Super Dragon Ball Z - native WebGPU");
    if (!IsWindowReady())
    {
        std::fprintf(stderr, "raylib: failed to initialize window\n");
        return false;
    }

    void* hwnd = GetWindowHandle();
    const int width = GetRenderWidth();
    const int height = GetRenderHeight();
    if (hwnd == nullptr || width <= 0 || height <= 0)
    {
        std::fprintf(stderr, "raylib: invalid Win32 window or framebuffer\n");
        CloseWindow();
        return false;
    }

    ps2::gs::gpu::SurfaceTarget target{};
    target.kind = ps2::gs::gpu::SurfaceTarget::Win32;
    target.display = ee_win32_window_instance(hwnd);
    target.surface = hwnd;
    target.width = static_cast<uint32_t>(width);
    target.height = static_cast<uint32_t>(height);
    if (target.display == nullptr || !ps2::gs::gpu::attach_surface(target))
    {
        std::fprintf(stderr, "raylib: WebGPU surface creation failed\n");
        CloseWindow();
        return false;
    }

    m_ready = true;
    m_closed = false;
    m_last_host_poll = GetTime();
    std::printf("GS WebGPU window: raylib HWND, wgpu-native DX12 surface\n");
    return true;
}

void c_ee_raylib_window::pump(c_ee_webgpu_bridge& bridge,
    const s_ee_gs_display_registers& registers,
    uint32_t crt_mode, uint64_t field_count,
    c_ee_gs_presentation_probe* probe)
{
    if (!m_ready || m_closed)
    {
        return;
    }

    // Host time only controls event polling; guest scanout drives frames.
    const double now = GetTime();
    const bool advanced = field_count != m_last_fields;
    if (!advanced && now - m_last_host_poll < 1.0 / 120.0)
    {
        return;
    }

    m_last_host_poll = now;
    PollInputEvents();
    if (WindowShouldClose())
    {
        m_closed = true;
        CloseWindow();
        m_ready = false;
        return;
    }

    const int width = GetRenderWidth();
    const int height = GetRenderHeight();
    if (width <= 0 || height <= 0)
    {
        return;
    }
    ps2::gs::gpu::resize_surface(
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height));

    if (advanced)
    {
        m_last_fields = field_count;
        // Count the actual window attempt separately from the guest field
        // count. A native surface/present failure is not a guest event.
        const bool supported = bridge.supports_display(registers, crt_mode);
        const bool presented = bridge.present(registers, crt_mode);
        if (probe != nullptr)
        {
            probe->record_present(
                field_count, supported, presented, registers, crt_mode);
        }
    }
    // Never call BeginDrawing/EndDrawing: these swap raylib's GL buffers.
}
}
