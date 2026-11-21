#pragma once

// Read-only host provenance: real GS fields, optional window presentations,
// and independent CPU/GPU VRAM readbacks. No guest-visible side effects.

#include "ee_gs_display.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ee
{

enum class e_ee_gs_capture_kind : uint32_t
{
    cpu_display = 0u,
    gpu_display = 1u,
    upload = 2u
};

struct s_ee_gs_capture_metrics
{
    bool m_requested = false;
    bool m_readback_succeeded = false;
    bool m_file_written = false;
    bool m_has_display_source = false;
    uint32_t m_width = 0u;
    uint32_t m_height = 0u;
    uint32_t m_crt_mode = 0u;
    uint64_t m_nonblack_pixels = 0u;
    uint64_t m_checksum = 0u;
    s_ee_gs_display_registers m_display{};
};

// Reporting data is deliberately independent from the CPU GS state, DMA,
// scanout clock and WebGPU renderer. It cannot manufacture any event.
class c_ee_gs_presentation_probe final
{
public:
    void set_window_requested(bool requested)
    {
        m_window_requested = requested;
    }

    void set_gpu_progress(bool enabled, uint64_t rendered_primitives)
    {
        m_gpu_enabled = enabled;
        m_gpu_draws = enabled ? rendered_primitives : 0u;
    }

    // Invoke only after a *real* scanout field drives the window pump.
    // Multiple guest fields may elapse between host polls; one pump is one
    // attempt, not one attempt per elapsed field.
    void record_present(
        uint64_t field_count, bool supported, bool succeeded,
        const s_ee_gs_display_registers& display, uint32_t crt_mode)
    {
        if (field_count == 0u || field_count <= m_last_present_field)
        {
            return;
        }

        m_skipped_window_fields += field_count - m_last_present_field - 1u;
        m_last_present_field = field_count;
        ++m_present_attempts;
        if (supported)
        {
            ++m_valid_scanout_attempts;
        }
        if (supported && succeeded)
        {
            ++m_present_succeeded;
        }
        m_last_present_supported = supported;
        m_last_present_succeeded = succeeded;
        m_last_present_display = display;
        m_last_present_crt_mode = crt_mode;
    }

    void request_capture(e_ee_gs_capture_kind kind)
    {
        capture(kind).m_requested = true;
    }

    // Called only after a successful source-specific VRAM readback.
    // Width/height and buffer length must match before accepting metrics.
    bool record_capture(
        e_ee_gs_capture_kind kind, const std::vector<uint32_t>& pixels,
        uint32_t width, uint32_t height,
        const s_ee_gs_display_registers* display = nullptr,
        uint32_t crt_mode = 0u)
    {
        s_ee_gs_capture_metrics& entry = capture(kind);
        if (!entry.m_requested || width == 0u || height == 0u ||
            pixels.size() != static_cast<size_t>(width) * height ||
            (kind != e_ee_gs_capture_kind::upload && display == nullptr))
        {
            return false;
        }

        // FNV-1a over explicit little-endian RGBA word bytes, stable across
        // host endianness. This is a diagnostic checksum, not proof of equality.
        uint64_t checksum = 14695981039346656037ull;
        uint64_t nonblack = 0u;
        for (uint32_t color : pixels)
        {
            if ((color & 0x00ffffffu) != 0u)
            {
                ++nonblack;
            }
            for (uint32_t shift = 0u; shift < 32u; shift += 8u)
            {
                checksum ^= (color >> shift) & 0xffu;
                checksum *= 1099511628211ull;
            }
        }
        entry.m_width = width;
        entry.m_height = height;
        entry.m_checksum = checksum;
        entry.m_nonblack_pixels = nonblack;
        entry.m_readback_succeeded = true;
        if (display != nullptr && kind != e_ee_gs_capture_kind::upload)
        {
            entry.m_display = *display;
            entry.m_crt_mode = crt_mode;
            entry.m_has_display_source = true;
        }
        return true;
    }

    void record_file_written(e_ee_gs_capture_kind kind, bool success)
    {
        s_ee_gs_capture_metrics& entry = capture(kind);
        entry.m_file_written = entry.m_readback_succeeded && success;
    }

    const s_ee_gs_capture_metrics& metrics(e_ee_gs_capture_kind kind) const
    {
        return m_captures[static_cast<size_t>(kind)];
    }

    uint64_t present_attempts() const
    {
        return m_present_attempts;
    }

    uint64_t present_succeeded() const
    {
        return m_present_succeeded;
    }

    uint64_t skipped_window_fields() const
    {
        return m_skipped_window_fields;
    }

    bool captures_have_same_source() const
    {
        const s_ee_gs_capture_metrics& cpu = metrics(
            e_ee_gs_capture_kind::cpu_display);
        const s_ee_gs_capture_metrics& gpu = metrics(
            e_ee_gs_capture_kind::gpu_display);
        if (!cpu.m_readback_succeeded || !gpu.m_readback_succeeded ||
            !cpu.m_has_display_source || !gpu.m_has_display_source)
        {
            return false;
        }
        const s_ee_gs_display_registers& a = cpu.m_display;
        const s_ee_gs_display_registers& b = gpu.m_display;
        return cpu.m_crt_mode == gpu.m_crt_mode &&
            a.m_pmode == b.m_pmode && a.m_smode2 == b.m_smode2 &&
            a.m_dispfb[0] == b.m_dispfb[0] &&
            a.m_dispfb[1] == b.m_dispfb[1] &&
            a.m_display[0] == b.m_display[0] &&
            a.m_display[1] == b.m_display[1];
    }

    void write_json(FILE* output, uint64_t fields_started) const
    {
        std::fprintf(output,
            "{\n    \"schema\": \"ee-gs-presentation-provenance-v1\","
            "\n    \"window_requested\": %s,"
            "\n    \"gpu_enabled\": %s,"
            "\n    \"gpu_committed_draws\": %llu,"
            "\n    \"guest_fields_started\": %llu,"
            "\n    \"window_present_attempts\": %llu,"
            "\n    \"valid_scanout_attempts\": %llu,"
            "\n    \"window_present_succeeded\": %llu,"
            "\n    \"skipped_window_fields\": %llu,"
            "\n    \"last_attempt_field\": %llu,"
            "\n    \"last_attempt_scanout_supported\": %s,"
            "\n    \"last_attempt_present_succeeded\": %s,"
            "\n    \"last_attempt_crt_mode\": %u,"
            "\n    \"last_attempt_pmode\": \"0x%016llx\","
            "\n    \"last_attempt_smode2\": \"0x%016llx\","
            "\n    \"last_attempt_dispfb1\": \"0x%016llx\","
            "\n    \"last_attempt_display1\": \"0x%016llx\","
            "\n    \"last_attempt_dispfb2\": \"0x%016llx\","
            "\n    \"last_attempt_display2\": \"0x%016llx\",",
            m_window_requested ? "true" : "false",
            m_gpu_enabled ? "true" : "false",
            static_cast<unsigned long long>(m_gpu_draws),
            static_cast<unsigned long long>(fields_started),
            static_cast<unsigned long long>(m_present_attempts),
            static_cast<unsigned long long>(m_valid_scanout_attempts),
            static_cast<unsigned long long>(m_present_succeeded),
            static_cast<unsigned long long>(m_skipped_window_fields),
            static_cast<unsigned long long>(m_last_present_field),
            m_last_present_supported ? "true" : "false",
            m_last_present_succeeded ? "true" : "false",
            m_last_present_crt_mode,
            static_cast<unsigned long long>(m_last_present_display.m_pmode),
            static_cast<unsigned long long>(m_last_present_display.m_smode2),
            static_cast<unsigned long long>(m_last_present_display.m_dispfb[0]),
            static_cast<unsigned long long>(m_last_present_display.m_display[0]),
            static_cast<unsigned long long>(m_last_present_display.m_dispfb[1]),
            static_cast<unsigned long long>(m_last_present_display.m_display[1]));
        std::fputs("\n    \"captures\": {", output);
        static constexpr const char* k_names[3] =
        {
            "cpu_display", "gpu_display", "upload"
        };
        for (size_t i = 0u; i < 3u; ++i)
        {
            const s_ee_gs_capture_metrics& item = m_captures[i];
            std::fprintf(output,
                "%s\n      \"%s\": {"
                "\"requested\": %s, \"readback_succeeded\": %s, "
                "\"file_written\": %s, "
                "\"width\": %u, \"height\": %u, "
                "\"nonblack_pixels\": %llu, "
                "\"fnv1a64_rgba\": \"0x%016llx\"}",
                i != 0u ? "," : "", k_names[i],
                item.m_requested ? "true" : "false",
                item.m_readback_succeeded ? "true" : "false",
                item.m_file_written ? "true" : "false",
                item.m_width, item.m_height,
                static_cast<unsigned long long>(item.m_nonblack_pixels),
                static_cast<unsigned long long>(item.m_checksum));
        }
        const bool comparable = captures_have_same_source();
        std::fputs("\n    },\n    \"cpu_gpu_same_display_source\": ", output);
        std::fputs(comparable ? "true" : "false", output);
        std::fputs(",\n    \"cpu_gpu_digest_match\": ", output);
        if (!comparable)
        {
            std::fputs("null", output);
        }
        else
        {
            const auto& cpu = metrics(e_ee_gs_capture_kind::cpu_display);
            const auto& gpu = metrics(e_ee_gs_capture_kind::gpu_display);
            const bool match = cpu.m_width == gpu.m_width &&
                cpu.m_height == gpu.m_height &&
                cpu.m_checksum == gpu.m_checksum;
            std::fputs(match ? "true" : "false", output);
        }
        std::fputs(
            ",\n    \"limitations\": ["
            "\"gpu_snapshot_does_not_imply_window_present\", "
            "\"black_image_does_not_imply_renderer_defect\", "
            "\"fnv_checksum_is_diagnostic_not_pixel_equality_proof\"]"
            "\n  }", output);
    }

private:
    s_ee_gs_capture_metrics& capture(e_ee_gs_capture_kind kind)
    {
        return m_captures[static_cast<size_t>(kind)];
    }

    s_ee_gs_capture_metrics m_captures[3]{};
    s_ee_gs_display_registers m_last_present_display{};
    uint64_t m_gpu_draws = 0u;
    uint64_t m_last_present_field = 0u;
    uint64_t m_present_attempts = 0u;
    uint64_t m_valid_scanout_attempts = 0u;
    uint64_t m_present_succeeded = 0u;
    uint64_t m_skipped_window_fields = 0u;
    uint32_t m_last_present_crt_mode = 0u;
    bool m_window_requested = false;
    bool m_gpu_enabled = false;
    bool m_last_present_supported = false;
    bool m_last_present_succeeded = false;
};

} // namespace ee
