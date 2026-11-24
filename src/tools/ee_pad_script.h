#pragma once

// SDBZ_PAD0_SCRIPT: scripted, deterministic port-0 controller holds counted in
// authentic SIO2 0x42 pad polls, e.g. "740:8:cross,2900:8:cross,3500:4:start".
// Each step is FIRST_POLL:HOLD_POLLS:BUTTON[+BUTTON...]. Names use the PS2
// pad button bits (active-high here; the pad model inverts them on the wire).

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace ee
{

struct s_pad_script_step
{
    uint32_t first_poll = 0u;
    uint32_t poll_count = 0u;
    uint16_t buttons = 0u;
};

inline bool pad_button_bit(const std::string& name, uint16_t* bit)
{
    static const struct { const char* name; uint16_t bit; } k_buttons[] = {
        {"select", 1u << 0u}, {"l3", 1u << 1u}, {"r3", 1u << 2u},
        {"start", 1u << 3u}, {"up", 1u << 4u}, {"right", 1u << 5u},
        {"down", 1u << 6u}, {"left", 1u << 7u}, {"l2", 1u << 8u},
        {"r2", 1u << 9u}, {"l1", 1u << 10u}, {"r1", 1u << 11u},
        {"triangle", 1u << 12u}, {"circle", 1u << 13u},
        {"cross", 1u << 14u}, {"square", 1u << 15u},
    };
    std::string lower;
    for (const char c : name)
    {
        lower.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    }
    for (const auto& item : k_buttons)
    {
        if (lower == item.name)
        {
            *bit = item.bit;
            return true;
        }
    }
    return false;
}

inline bool parse_pad_script_number(const std::string& text, uint32_t* value)
{
    if (text.empty() || text.size() > 9u)
    {
        return false;
    }
    for (const char c : text)
    {
        if (c < '0' || c > '9')
        {
            return false;
        }
    }
    *value = static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 10));
    return true;
}

// Returns false with a message for any malformed step. Poll numbers start at
// 1; holds are 1..600 polls; at most 256 steps.
inline bool parse_pad_script(const std::string& script,
                             std::vector<s_pad_script_step>* steps,
                             std::string* error)
{
    steps->clear();
    size_t start = 0u;
    while (start <= script.size())
    {
        const size_t end = script.find_first_of(",;", start);
        const std::string item = script.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? script.size() + 1u : end + 1u;
        if (item.empty())
        {
            continue;
        }
        const size_t first_colon = item.find(':');
        const size_t second_colon = first_colon == std::string::npos
            ? std::string::npos : item.find(':', first_colon + 1u);
        if (second_colon == std::string::npos)
        {
            *error = "expected FIRST_POLL:HOLD_POLLS:BUTTON in '" + item + "'";
            return false;
        }
        s_pad_script_step step{};
        if (!parse_pad_script_number(item.substr(0u, first_colon), &step.first_poll) ||
            !parse_pad_script_number(
                item.substr(first_colon + 1u, second_colon - first_colon - 1u),
                &step.poll_count) ||
            step.first_poll == 0u || step.poll_count == 0u || step.poll_count > 600u)
        {
            *error = "invalid poll numbers in '" + item + "' (first >= 1, hold 1..600)";
            return false;
        }
        const std::string names = item.substr(second_colon + 1u);
        size_t name_start = 0u;
        while (name_start <= names.size())
        {
            const size_t plus = names.find('+', name_start);
            const std::string name = names.substr(
                name_start, plus == std::string::npos ? std::string::npos : plus - name_start);
            uint16_t bit = 0u;
            if (!pad_button_bit(name, &bit))
            {
                *error = "unknown button '" + name + "' in '" + item + "'";
                return false;
            }
            step.buttons = static_cast<uint16_t>(step.buttons | bit);
            if (plus == std::string::npos)
            {
                break;
            }
            name_start = plus + 1u;
        }
        steps->push_back(step);
        if (steps->size() > 256u)
        {
            *error = "more than 256 script steps";
            return false;
        }
    }
    if (steps->empty())
    {
        *error = "empty pad script";
        return false;
    }
    return true;
}

} // namespace ee
