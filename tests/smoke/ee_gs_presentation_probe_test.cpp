#include "ee_gs_presentation_probe.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(expression) \
    do \
    { \
        if (!(expression)) \
        { \
            std::fprintf(stderr, "GS provenance test failed at line %d: %s\\n", \
                __LINE__, #expression); \
            return 1; \
        } \
    } while (false)

int main()
{
    ee::c_ee_gs_presentation_probe probe{};
    ee::s_ee_gs_display_registers display{};
    display.m_pmode = 1u;
    display.m_dispfb[0] = 1ull << 9u;
    display.m_display[0] = (63ull << 32u) | (31ull << 44u);

    // Only explicit host presentation attempts on genuine field advances
    // are counted; a failed/unsupported display is never a success.
    probe.set_window_requested(true);
    probe.record_present(0u, true, true, display, 2u);
    probe.record_present(1u, false, false, display, 2u);
    probe.record_present(1u, true, true, display, 2u);
    probe.record_present(4u, true, true, display, 2u);
    CHECK(probe.present_attempts() == 2u);
    CHECK(probe.present_succeeded() == 1u);
    CHECK(probe.skipped_window_fields() == 2u);

    std::vector<uint32_t> pixels
    {
        0xff000000u, 0xff112233u, 0x00000000u, 0x00010203u
    };
    const auto cpu = ee::e_ee_gs_capture_kind::cpu_display;
    const auto gpu = ee::e_ee_gs_capture_kind::gpu_display;
    const auto upload = ee::e_ee_gs_capture_kind::upload;

    // Failed readbacks and malformed buffers cannot be reported as
    // successful captures, and no file is assumed to have been written.
    CHECK(!probe.record_capture(cpu, pixels, 2u, 2u, &display, 2u));
    probe.request_capture(cpu);
    probe.request_capture(gpu);
    probe.request_capture(upload);
    CHECK(!probe.record_capture(cpu, pixels, 3u, 2u, &display, 2u));
    CHECK(!probe.record_capture(cpu, pixels, 2u, 2u));
    CHECK(probe.record_capture(cpu, pixels, 2u, 2u, &display, 2u));
    CHECK(probe.metrics(cpu).m_nonblack_pixels == 2u);
    CHECK(!probe.metrics(cpu).m_file_written);
    probe.record_file_written(cpu, true);
    CHECK(probe.metrics(cpu).m_file_written);

    CHECK(probe.record_capture(gpu, pixels, 2u, 2u, &display, 2u));
    CHECK(probe.captures_have_same_source());
    CHECK(probe.metrics(cpu).m_checksum == probe.metrics(gpu).m_checksum);
    CHECK(probe.record_capture(upload, pixels, 2u, 2u));
    CHECK(!probe.metrics(upload).m_has_display_source);

    // A GPU readback from a different original DISPFB configuration must
    // not be compared to the CPU's digest, even if pixels happen to match.
    ee::s_ee_gs_display_registers different = display;
    different.m_dispfb[0] += 1u;
    CHECK(probe.record_capture(gpu, pixels, 2u, 2u, &different, 2u));
    CHECK(!probe.captures_have_same_source());
    CHECK(probe.record_capture(gpu, pixels, 2u, 2u, &display, 2u));
    CHECK(probe.captures_have_same_source());

    FILE* file = std::tmpfile();
    CHECK(file != nullptr);
    probe.set_gpu_progress(true, 11u);
    probe.write_json(file, 9u);
    std::fflush(file);
    std::rewind(file);
    char buffer[8192]{};
    const size_t length = std::fread(buffer, 1u, sizeof(buffer) - 1u, file);
    std::fclose(file);
    const std::string json(buffer, length);
    CHECK(json.find("\"guest_fields_started\": 9") != std::string::npos);
    CHECK(json.find("\"window_present_attempts\": 2") != std::string::npos);
    CHECK(json.find("\"valid_scanout_attempts\": 1") != std::string::npos);
    CHECK(json.find("\"window_present_succeeded\": 1") != std::string::npos);
    CHECK(json.find("\"skipped_window_fields\": 2") != std::string::npos);
    CHECK(json.find("\"gpu_committed_draws\": 11") != std::string::npos);
    CHECK(json.find("\"cpu_gpu_same_display_source\": true") != std::string::npos);
    CHECK(json.find("\"cpu_gpu_digest_match\": true") != std::string::npos);
    CHECK(json.find("\"nonblack_pixels\": 2") != std::string::npos);

    std::puts("EE GS presentation provenance: PASS");
    return 0;
}
