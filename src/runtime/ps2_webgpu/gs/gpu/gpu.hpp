#pragma once

#include "../gs.hpp"

#include <cstdint>
#include <vector>

namespace ee { struct s_gpu_timestamp_profile; struct s_ee_overlay_text; }

namespace ps2::gs::gpu {

enum class Mode { off, mirror, primary };

struct DisplaySource {
    std::uint32_t fbp, fbw, psm, dbx, dby, width, height;
    bool halved;
    bool interlaced = false;
    std::uint64_t guest_field = 0u;
};

struct SurfaceTarget {
    enum Kind { X11, Wayland, Canvas, Win32, Metal } kind;
    void *display;
    std::uint64_t window;
    void *surface;
    const char *selector;
    std::uint32_t width, height;
};

Mode mode();
bool init(Mode m);
void draw(const State &s, const Vertex *v, int count);
// Bounded GPU atlas cache: registry index is mapped to a reusable physical
// slot; old slots are evicted only after submitting pending draws.
int native_slot(std::uint32_t index, std::uint32_t width,
    std::uint32_t height, const std::uint8_t *rgba);
void invalidate_native_atlases();
void image_begin(const State &s);
void image_data(const std::uint8_t *data, std::size_t bytes);
void local_copy(const State &s);
void load_clut(std::uint64_t tex0, std::uint64_t texclut);
void flush();
void texflush();
void wait_idle();
// Destination is a full GS VRAM buffer; offset and bytes are word-aligned.
// A failed map never reports successful guest-visible data.
bool download_checked(std::uint8_t *memory, std::uint32_t offset = 0, std::uint32_t bytes = kMemorySize);
void download(std::uint8_t *memory, std::uint32_t offset = 0, std::uint32_t bytes = kMemorySize);
bool download_async(std::uint8_t *memory, std::uint32_t offset, std::uint32_t bytes, std::uint32_t serial);
bool finish_download(std::uint32_t serial);
// Read the current palette snapshot, not its backing VRAM.
bool read_clut_checked(std::uint32_t *entries, std::size_t count);
void read_state(std::vector<std::uint32_t> &vram, std::uint32_t &scale, std::vector<std::uint32_t> &clut);
void write_state(const std::vector<std::uint32_t> &vram, std::uint32_t scale, const std::vector<std::uint32_t> &clut);
bool display(const DisplaySource &src, std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);
bool display_async(const DisplaySource &src, std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height);
bool attach_surface(const SurfaceTarget &target);
void resize_surface(std::uint32_t width, std::uint32_t height);
bool has_surface();
bool present(const DisplaySource &src);
// Owner-thread host presentation only. Null disables the overlay.
void set_performance_overlay(const ee::s_ee_overlay_text* text);
// Snapshot can be called by the EE while the GS worker owns the GPU.
// Collection touches the device and is owner-only, after draining work.
ee::s_gpu_timestamp_profile timestamp_profile();
void collect_timestamp_profile();
bool set_command_profile_enabled(bool enabled);
double busy_ms();
double vram_mb();
void stats();

}
