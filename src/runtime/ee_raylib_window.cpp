#include "ee_raylib_window.h"

#include "iop_hw.h"

#include "ee_webgpu_bridge.h"
#include "ee_gs_presentation_probe.h"
#include "ps2_webgpu/gs/gpu/gpu.hpp"

#include <raylib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <limits>

// Win32 and raylib both declare CloseWindow/ShowCursor. Rename those
// unused Win32 declarations while reading windows.h and prevent its
// min/max macros from corrupting standard C++ numeric_limits.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef NOGDI
#define NOGDI
#endif
#define CloseWindow Win32CloseWindow
#define ShowCursor Win32ShowCursor
#include <windows.h>
#undef CloseWindow
#undef ShowCursor

extern "C" void* ee_win32_window_instance(void* window_handle);

namespace ee
{
namespace
{
void poll_controller(iop::c_hw& hardware)
{
    int pad = 0;
    for (; pad < 4 && !IsGamepadAvailable(pad); ++pad)
    {
    }
    if (pad == 4)
    {
        hardware.set_sio2_pad0_input(0u, 128u, 128u, 128u, 128u);
        return;
    }

    constexpr int pad_buttons[] = {
        GAMEPAD_BUTTON_MIDDLE_LEFT, GAMEPAD_BUTTON_LEFT_THUMB,
        GAMEPAD_BUTTON_RIGHT_THUMB, GAMEPAD_BUTTON_MIDDLE_RIGHT,
        GAMEPAD_BUTTON_LEFT_FACE_UP, GAMEPAD_BUTTON_LEFT_FACE_RIGHT,
        GAMEPAD_BUTTON_LEFT_FACE_DOWN, GAMEPAD_BUTTON_LEFT_FACE_LEFT,
        GAMEPAD_BUTTON_LEFT_TRIGGER_2, GAMEPAD_BUTTON_RIGHT_TRIGGER_2,
        GAMEPAD_BUTTON_LEFT_TRIGGER_1, GAMEPAD_BUTTON_RIGHT_TRIGGER_1,
        GAMEPAD_BUTTON_RIGHT_FACE_UP, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT,
        GAMEPAD_BUTTON_RIGHT_FACE_DOWN, GAMEPAD_BUTTON_RIGHT_FACE_LEFT
    };
    uint16_t buttons = 0u;
    for (uint32_t bit = 0u; bit < 16u; ++bit)
    {
        if (IsGamepadButtonDown(pad, pad_buttons[bit]))
        {
            buttons |= static_cast<uint16_t>(1u << bit);
        }
    }

    const auto axis = [pad](int index)
    {
        const float position = std::clamp(
            GetGamepadAxisMovement(pad, index), -1.0f, 1.0f);
        if (std::abs(position) <= 0.12f)
        {
            return uint8_t{128u};
        }
        return static_cast<uint8_t>(std::lround((position + 1.0f) * 127.5f));
    };
    hardware.set_sio2_pad0_input(buttons,
        axis(GAMEPAD_AXIS_RIGHT_X), axis(GAMEPAD_AXIS_RIGHT_Y),
        axis(GAMEPAD_AXIS_LEFT_X), axis(GAMEPAD_AXIS_LEFT_Y));
}
}

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

    // The launcher may provide a live Tk Win32 viewport. A missing
    // environment variable retains the original standalone raylib window.
    HWND parent_handle = nullptr;
    int initial_width = 960;
    int initial_height = 720;
    const char* parent_string = std::getenv("SDBZ_GS_PARENT_HWND");
    if (parent_string != nullptr && parent_string[0] != '\0')
    {
        errno = 0;
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(
            parent_string, &end, 10);
        if (errno != 0 || end == parent_string || *end != '\0' ||
            parsed == 0u ||
            parsed > std::numeric_limits<uintptr_t>::max())
        {
            std::fprintf(stderr, "raylib: invalid embedded parent HWND\n");
            return false;
        }
        parent_handle = reinterpret_cast<HWND>(
            static_cast<uintptr_t>(parsed));
        RECT parent_rect{};
        if (!IsWindow(parent_handle) ||
            !GetClientRect(parent_handle, &parent_rect) ||
            parent_rect.right <= parent_rect.left ||
            parent_rect.bottom <= parent_rect.top)
        {
            std::fprintf(stderr,
                "raylib: embedded parent HWND is not a live viewport\n");
            return false;
        }
        initial_width = parent_rect.right - parent_rect.left;
        initial_height = parent_rect.bottom - parent_rect.top;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(initial_width, initial_height,
        "Super Dragon Ball Z - native WebGPU");
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

    if (parent_handle != nullptr)
    {
        HWND child_handle = static_cast<HWND>(hwnd);
        const LONG_PTR style = GetWindowLongPtrW(
            child_handle, GWL_STYLE);
        SetWindowLongPtrW(
            child_handle, GWL_STYLE,
            (style & ~(static_cast<LONG_PTR>(
                WS_POPUP | WS_CAPTION | WS_THICKFRAME |
                WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU))) |
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS);
        SetLastError(ERROR_SUCCESS);
        const HWND old_parent = SetParent(child_handle, parent_handle);
        if (old_parent == nullptr && GetLastError() != ERROR_SUCCESS)
        {
            std::fprintf(stderr, "raylib: SetParent failed error=%lu\n",
                static_cast<unsigned long>(GetLastError()));
            CloseWindow();
            return false;
        }
        RECT parent_rect{};
        if (!GetClientRect(parent_handle, &parent_rect) ||
            !SetWindowPos(
                child_handle, nullptr, 0, 0,
                parent_rect.right - parent_rect.left,
                parent_rect.bottom - parent_rect.top,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED |
                    SWP_SHOWWINDOW))
        {
            std::fprintf(stderr, "raylib: could not size embedded viewport\n");
            CloseWindow();
            return false;
        }
        m_parent_window_handle = parent_handle;
        m_native_window_handle = child_handle;
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
    std::printf(
        "GS WebGPU window: raylib HWND, wgpu-native DX12 surface%s\n",
        m_parent_window_handle != nullptr ? " (embedded in launcher)" : "");
    return true;
}

void c_ee_raylib_window::pump(c_ee_webgpu_bridge& bridge,
    const s_ee_gs_display_registers& registers,
    uint32_t crt_mode, uint64_t field_count,
    c_ee_gs_presentation_probe* probe, iop::c_hw* iop_hardware)
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
    if (m_parent_window_handle != nullptr)
    {
        HWND parent_handle = static_cast<HWND>(m_parent_window_handle);
        HWND child_handle = static_cast<HWND>(m_native_window_handle);
        if (!IsWindow(parent_handle) || !IsWindow(child_handle))
        {
            m_closed = true;
            CloseWindow();
            m_ready = false;
            return;
        }
        RECT parent_rect{};
        RECT child_rect{};
        if (GetClientRect(parent_handle, &parent_rect) &&
            GetClientRect(child_handle, &child_rect))
        {
            const int width = parent_rect.right - parent_rect.left;
            const int height = parent_rect.bottom - parent_rect.top;
            if (width > 0 && height > 0 &&
                (width != child_rect.right - child_rect.left ||
                 height != child_rect.bottom - child_rect.top))
            {
                SetWindowPos(child_handle, nullptr, 0, 0, width, height,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }
    PollInputEvents();
    if (WindowShouldClose())
    {
        if (iop_hardware != nullptr)
        {
            iop_hardware->set_sio2_pad0_input(0u, 128u, 128u, 128u, 128u);
        }
        m_closed = true;
        CloseWindow();
        m_ready = false;
        return;
    }

    if (iop_hardware != nullptr)
    {
        poll_controller(*iop_hardware);
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
        const bool presented = bridge.present(registers, crt_mode, field_count);
        if (probe != nullptr)
        {
            probe->record_present(
                field_count, supported, presented, registers, crt_mode);
        }
    }
    // Never call BeginDrawing/EndDrawing: these swap raylib's GL buffers.
}
}
