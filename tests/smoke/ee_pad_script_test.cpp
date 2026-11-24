// SDBZ_PAD0_SCRIPT parsing: poll windows and PS2 pad button bits.

#include "ee_pad_script.h"

#include <cstdio>

namespace
{
int g_pass = 0;
int g_fail = 0;

void check(bool ok, const char* name)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    (ok ? g_pass : g_fail)++;
}
} // namespace

int main()
{
    std::vector<ee::s_pad_script_step> steps;
    std::string error;

    check(ee::parse_pad_script("740:8:cross,2900:8:Cross;3500:4:start+up", &steps, &error) &&
              steps.size() == 3u &&
              steps[0].first_poll == 740u && steps[0].poll_count == 8u &&
              steps[0].buttons == (1u << 14u) &&
              steps[1].buttons == (1u << 14u) &&
              steps[2].buttons == ((1u << 3u) | (1u << 4u)),
          "parses Cross/Start/Up steps with comma or semicolon separators");

    check(ee::parse_pad_script("1:1:square+triangle+circle+select+l1+r1+l2+r2+l3+r3+down+left+right",
                               &steps, &error) &&
              steps[0].buttons == 0xbfe7u,
          "maps every named PS2 button to its pad bit");

    const char* invalid[] = {
        "", "740", "740:8", "0:8:cross", "740:0:cross", "740:601:cross",
        "740:8:jump", "abc:8:cross", "740:8:cross+", "-1:8:cross",
    };
    bool all_rejected = true;
    for (const char* text : invalid)
    {
        if (ee::parse_pad_script(text, &steps, &error))
        {
            std::printf("  unexpectedly accepted: '%s'\n", text);
            all_rejected = false;
        }
    }
    check(all_rejected, "rejects malformed scripts with a message");

    std::printf("ee_pad_script_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
