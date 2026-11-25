#pragma once

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <initializer_list>
#include <cstdio>
#include <cstring>

namespace ps2::gs::gpu
{
struct s_native_viewport
{
    int x = 0;
    int y = 0;
    int width = 1;
    int height = 1;
};

// The session file contains exactly one supported ASCII aspect token.
// Return stable string literals, and keep the previous mode on invalid
// or incomplete input (including transient failures during replacement).
inline const char* parse_display_aspect(const char* value, std::size_t count, const char* fallback)
{
    for (const char* aspect : {"4:3", "16:9", "native", "1:1"})
    {
        if (count == std::strlen(aspect) && std::memcmp(value, aspect, count) == 0)
        {
            return aspect;
        }
    }
    return fallback;
}

inline const char* read_display_aspect(FILE* input, const char* fallback)
{
    char value[7]{};
    if (input != nullptr)
    {
        const std::size_t count = std::fread(value, 1u, sizeof(value), input);
        if (!std::ferror(input))
        {
            fallback = parse_display_aspect(value, count, fallback);
        }
    }
    return fallback;
}

inline const char* read_display_aspect(const char* path, const char* fallback)
{
    if (path != nullptr && path[0] != '\0')
    {
        if (FILE* input = std::fopen(path, "rb"))
        {
            fallback = read_display_aspect(input, fallback);
            std::fclose(input);
        }
    }
    return fallback;
}

// GS scanout samples do not necessarily have square pixels. Fit the
// selected display aspect, not the framebuffer dimensions.
inline s_native_viewport window_viewport(int window_width, int window_height,
    int source_width, int source_height, const char* aspect = nullptr)
{
    window_width = std::max(window_width, 1);
    window_height = std::max(window_height, 1);
    int ratio_width = 4;
    int ratio_height = 3;
    if (aspect != nullptr && std::strcmp(aspect, "native") == 0)
    {
        ratio_width = std::max(source_width, 1);
        ratio_height = std::max(source_height, 1);
    }
    else if (aspect != nullptr && std::strcmp(aspect, "16:9") == 0)
    {
        ratio_width = 16;
        ratio_height = 9;
    }
    else if (aspect != nullptr && std::strcmp(aspect, "1:1") == 0)
    {
        ratio_width = 1;
        ratio_height = 1;
    }

    int width = window_width;
    int height = window_height;
    if (int64_t{window_width} * ratio_height > int64_t{window_height} * ratio_width)
    {
        width = std::max(1, static_cast<int>(
            (int64_t{window_height} * ratio_width + ratio_height / 2) / ratio_height));
    }
    else
    {
        height = std::max(1, static_cast<int>(
            (int64_t{window_width} * ratio_height + ratio_width / 2) / ratio_width));
    }
    return {(window_width - width) / 2, (window_height - height) / 2, width, height};
}
}
