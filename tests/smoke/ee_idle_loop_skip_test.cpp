// Host-only idle-loop skip: eligibility of retail-shaped poll loops and the
// side-effect-free prediction of their next iteration.

#include "ee_idle_loop_skip.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace
{
int g_pass = 0;
int g_fail = 0;

void check(bool ok, const char* name)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    (ok ? g_pass : g_fail)++;
}

struct s_code
{
    uint32_t base = 0u;
    std::vector<uint32_t> words;

    bool operator()(uint32_t pc, uint32_t* word) const
    {
        if (pc < base || pc >= base + 4u * words.size() || (pc & 3u) != 0u)
        {
            return false;
        }
        *word = words[(pc - base) / 4u];
        return true;
    }
};

// Retail SDBZ VSync wait at 0x00175210:
//   lw v0,0(v1) ; andi v0,v0,4 ; bne v0,zero,exit ; nop
//   lw v0,0(sp) ; beq v0,zero,0x00175210 ; nop
const s_code k_vsync = {0x00175210u, {
    0x8c620000u, 0x30420004u, 0x14400004u, 0x00000000u,
    0x8fa20000u, 0x1040fffau, 0x00000000u}};

struct s_memory
{
    std::map<uint32_t, uint64_t> values;
    bool refuse = false;
    mutable int reads = 0;

    bool operator()(uint32_t address, uint32_t, uint64_t* raw) const
    {
        ++reads;
        if (refuse)
        {
            return false;
        }
        const auto found = values.find(address);
        *raw = found != values.end() ? found->second : 0u;
        return true;
    }
};
} // namespace

int main()
{
    using ee::e_idle_iteration;

    const ee::s_idle_loop_shape vsync = ee::analyze_idle_loop(0x00175210u, k_vsync);
    check(vsync.eligible && vsync.words.size() == 7u &&
              vsync.backedge_pc == 0x00175224u,
          "retail VSync poll loop is an eligible 7-instruction loop");

    uint64_t gpr[32] = {};
    gpr[3] = 0x1000f000u;   // v1: INTC_STAT
    gpr[29] = 0x01ff0000u;  // sp
    gpr[2] = 0x12345678u;   // stale v0 is overwritten before any use

    s_memory memory;
    check(ee::predict_idle_iteration(vsync, gpr, memory) == e_idle_iteration::loops,
          "no VBlank bit and zero flag keeps polling");
    memory.values[0x1000f000u] = 4u;
    check(ee::predict_idle_iteration(vsync, gpr, memory) == e_idle_iteration::exits,
          "INTC_STAT VBlank-start bit leaves the loop");
    memory.values[0x1000f000u] = 0u;
    memory.values[0x01ff0000u] = 1u;
    check(ee::predict_idle_iteration(vsync, gpr, memory) == e_idle_iteration::exits,
          "nonzero stack flag leaves through the backedge fall-through");
    memory.values.clear();
    memory.refuse = true;
    check(ee::predict_idle_iteration(vsync, gpr, memory) == e_idle_iteration::unsupported,
          "a load the host refuses makes the iteration unsupported");
    check(gpr[2] == 0x12345678u, "prediction never writes guest registers");

    // Byte loads are sign extended exactly as LB does.
    const s_code byte_poll = {0x00200000u, {
        0x80820000u,   // lb   v0,0(a0)
        0x0441fffeu,   // bgez v0,0x00200000
        0x00000000u}};
    const ee::s_idle_loop_shape byte_shape = ee::analyze_idle_loop(0x00200000u, byte_poll);
    s_memory byte_memory;
    uint64_t byte_gpr[32] = {};
    byte_gpr[4] = 0x00400000u;
    byte_memory.values[0x00400000u] = 0x7fu;
    check(byte_shape.eligible &&
              ee::predict_idle_iteration(byte_shape, byte_gpr, byte_memory) ==
                  e_idle_iteration::loops,
          "positive LB byte keeps a BGEZ poll looping");
    byte_memory.values[0x00400000u] = 0x80u;
    check(ee::predict_idle_iteration(byte_shape, byte_gpr, byte_memory) ==
              e_idle_iteration::exits,
          "LB sign-extends 0x80 so BGEZ falls through");

    struct s_rejected
    {
        const char* name;
        s_code code;
        const char* reason;
    };
    const s_rejected rejected[] = {
        {"store inside the loop", {0x00300000u, {
            0xac400000u,   // sw   zero,0(v0)
            0x1000fffeu,   // b    0x00300000
            0x00000000u}}, "unsupported instruction"},
        {"loop-carried counter", {0x00300000u, {
            0x25080001u,   // addiu t0,t0,1
            0x1509fffeu,   // bne   t0,t1,0x00300000
            0x00000000u}}, "loop-carried register"},
        {"branch-likely backedge", {0x00300000u, {
            0x8c820000u,   // lw   v0,0(a0)
            0x5040fffeu,   // beql v0,zero,0x00300000
            0x00000000u}}, "unsupported instruction"},
        {"call inside the loop", {0x00300000u, {
            0x0c0c0000u,   // jal  0x00300000
            0x00000000u,
            0x1000fffdu,   // b    0x00300000
            0x00000000u}}, "unsupported instruction"},
        {"branch into the loop body", {0x00300000u, {
            0x8c820000u,   // lw   v0,0(a0)
            0x10400001u,   // beq  v0,zero,0x0030000c (inside)
            0x00000000u,
            0x8c830004u,   // lw   v1,4(a0)
            0x1000fffbu,   // b    0x00300000
            0x00000000u}}, "branch into the loop body"},
        {"syscall inside the loop", {0x00300000u, {
            0x0000000cu,   // syscall
            0x1000fffeu,
            0x00000000u}}, "unsupported instruction"},
    };
    for (const s_rejected& item : rejected)
    {
        const ee::s_idle_loop_shape shape = ee::analyze_idle_loop(item.code.base, item.code);
        const bool ok = !shape.eligible && std::strcmp(shape.reason, item.reason) == 0;
        if (!ok)
        {
            std::printf("  %s: eligible=%d reason=%s\n",
                        item.name, shape.eligible ? 1 : 0, shape.reason);
        }
        check(ok, item.name);
    }

    const s_code no_backedge = {0x00300000u, std::vector<uint32_t>(40u, 0u)};
    check(!ee::analyze_idle_loop(0x00300000u, no_backedge).eligible,
          "straight-line code without a backedge is not a loop");

    std::printf("ee_idle_loop_skip_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
