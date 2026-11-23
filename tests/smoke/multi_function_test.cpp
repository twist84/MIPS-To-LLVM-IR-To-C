#include "ee_decoder.h"
#include "ee_instruction.h"
#include "ee_opcode.h"
#include "ee_runtime.h"
#include "ee_memory.h"
#include "ee_state.h"
#include "ee_exceptions.h"
#include "ee/mmi_ops.h"
#include "ee/elf_loader.h"
#include "instruction_lifter.h"
#include "native_codegen.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace ee {
void exec_op(const s_lifted_op& op, s_ee_cpu_state& cpu,
             ee::ee_memory& mem, s_ee_runtime* rt, uint32_t& next_pc);
} // namespace ee

namespace {

const uint32_t kRamBase = 0x00200000u;
const uint32_t kTestRet = 0xDEAD0000u;
int failures = 0;

static std::string u32hex(uint32_t value)
{
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08x", value);
    return buffer;
}

static std::string u64hex(uint64_t value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%016llxULL",
                  static_cast<unsigned long long>(value));
    return buffer;
}

void check(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

auto sext32 = [](uint64_t v) -> int64_t {
    return (int64_t)(int32_t)(uint32_t)v;
};

static ee::s_ee_cpu_state ref_op(const ee::s_lifted_op& op,
                                 ee::s_ee_cpu_state start,
                                 ee::ee_memory& mem, ee::s_ee_runtime* rt)
{
    ee::s_ee_cpu_state cpu = start;
    using K = ee::e_lift_op_kind;
    switch (op.kind)
    {
    case K::LOP_ADDIU:
        cpu.m_gpr[op.rt].m_lo = (uint64_t)(sext32(cpu.m_gpr[op.rs].m_lo) + op.immediate);
        cpu.m_gpr[op.rt].m_hi = 0; break;
    case K::LOP_DADDIU:
        cpu.m_gpr[op.rt].m_lo = (uint64_t)((int64_t)cpu.m_gpr[op.rs].m_lo + op.immediate);
        cpu.m_gpr[op.rt].m_hi = 0; break;
    case K::LOP_ADDU:
        cpu.m_gpr[op.rd].m_lo = cpu.m_gpr[op.rs].m_lo + cpu.m_gpr[op.rt].m_lo;
        cpu.m_gpr[op.rd].m_hi = 0; break;
    case K::LOP_SUBU:
        cpu.m_gpr[op.rd].m_lo = cpu.m_gpr[op.rs].m_lo - cpu.m_gpr[op.rt].m_lo;
        cpu.m_gpr[op.rd].m_hi = 0; break;
    case K::LOP_AND:
        cpu.m_gpr[op.rd].m_lo = cpu.m_gpr[op.rs].m_lo & cpu.m_gpr[op.rt].m_lo;
        cpu.m_gpr[op.rd].m_hi = 0; break;
    case K::LOP_OR:
        cpu.m_gpr[op.rd].m_lo = cpu.m_gpr[op.rs].m_lo | cpu.m_gpr[op.rt].m_lo;
        cpu.m_gpr[op.rd].m_hi = 0; break;
    case K::LOP_XOR:
        cpu.m_gpr[op.rd].m_lo = cpu.m_gpr[op.rs].m_lo ^ cpu.m_gpr[op.rt].m_lo;
        cpu.m_gpr[op.rd].m_hi = 0; break;
    case K::LOP_NOR:
        cpu.m_gpr[op.rd].m_lo = ~(cpu.m_gpr[op.rs].m_lo | cpu.m_gpr[op.rt].m_lo);
        cpu.m_gpr[op.rd].m_hi = 0; break;
    case K::LOP_SLT:
        cpu.m_gpr[op.rd].m_lo = (sext32(cpu.m_gpr[op.rs].m_lo) < sext32(cpu.m_gpr[op.rt].m_lo)) ? 1 : 0;
        cpu.m_gpr[op.rd].m_hi = 0; break;
    case K::LOP_SLTU:
        cpu.m_gpr[op.rd].m_lo = (cpu.m_gpr[op.rs].m_lo < cpu.m_gpr[op.rt].m_lo) ? 1 : 0;
        cpu.m_gpr[op.rd].m_hi = 0; break;
    case K::LOP_SLTI:
        cpu.m_gpr[op.rt].m_lo = (sext32(cpu.m_gpr[op.rs].m_lo) < (int64_t)op.immediate) ? 1 : 0;
        cpu.m_gpr[op.rt].m_hi = 0; break;
    case K::LOP_SLTIU:
        cpu.m_gpr[op.rt].m_lo = (cpu.m_gpr[op.rs].m_lo < (uint64_t)op.uimmediate) ? 1 : 0;
        cpu.m_gpr[op.rt].m_hi = 0; break;
    case K::LOP_ANDI:
        cpu.m_gpr[op.rt].m_lo = cpu.m_gpr[op.rs].m_lo & op.uimmediate;
        cpu.m_gpr[op.rt].m_hi = 0; break;
    case K::LOP_ORI:
        cpu.m_gpr[op.rt].m_lo = cpu.m_gpr[op.rs].m_lo | op.uimmediate;
        cpu.m_gpr[op.rt].m_hi = 0; break;
    case K::LOP_XORI:
        cpu.m_gpr[op.rt].m_lo = cpu.m_gpr[op.rs].m_lo ^ op.uimmediate;
        cpu.m_gpr[op.rt].m_hi = 0; break;
    case K::LOP_LUI:
        cpu.m_gpr[op.rt].m_lo = (uint64_t)op.uimmediate << 16;
        cpu.m_gpr[op.rt].m_hi = 0; break;
    case K::LOP_SLL:
        cpu.m_gpr[op.rd].m_lo = cpu.m_gpr[op.rt].m_lo << op.sa;
        cpu.m_gpr[op.rd].m_hi = 0; break;
    case K::LOP_SRL:
        cpu.m_gpr[op.rd].m_lo = cpu.m_gpr[op.rt].m_lo >> op.sa;
        cpu.m_gpr[op.rd].m_hi = 0; break;
    case K::LOP_SRA:
        cpu.m_gpr[op.rd].m_lo = (uint64_t)((int64_t)(int32_t)cpu.m_gpr[op.rt].m_lo >> op.sa);
        cpu.m_gpr[op.rd].m_hi = 0; break;
    case K::LOP_LW:
    {
        uint32_t a = (uint32_t)(sext32(cpu.m_gpr[op.rs].m_lo) + op.immediate);
        cpu.m_gpr[op.rt].m_lo = (uint64_t)(int32_t)ee_mem_read32(rt, a);
        cpu.m_gpr[op.rt].m_hi = 0; break;
    }
    case K::LOP_SW:
    {
        uint32_t a = (uint32_t)(sext32(cpu.m_gpr[op.rs].m_lo) + op.immediate);
        ee_mem_write32(rt, a, (uint32_t)cpu.m_gpr[op.rt].m_lo); break;
    }
    case K::LOP_LBU:
    {
        uint32_t a = (uint32_t)(sext32(cpu.m_gpr[op.rs].m_lo) + op.immediate);
        cpu.m_gpr[op.rt].m_lo = ee_mem_read8(rt, a);
        cpu.m_gpr[op.rt].m_hi = 0; break;
    }
    case K::LOP_LHU:
    {
        uint32_t a = (uint32_t)(sext32(cpu.m_gpr[op.rs].m_lo) + op.immediate);
        cpu.m_gpr[op.rt].m_lo = ee_mem_read16(rt, a);
        cpu.m_gpr[op.rt].m_hi = 0; break;
    }
    case K::LOP_MFC0:
        cpu.m_gpr[op.rt].m_lo = cpu.m_cop0[op.rd];
        cpu.m_gpr[op.rt].m_hi = 0; break;
    case K::LOP_MTC0:
        cpu.m_cop0[op.rd] = (uint32_t)cpu.m_gpr[op.rt].m_lo; break;
    case K::LOP_MFC1:
        cpu.m_gpr[op.rt].m_lo = (uint64_t)(uint32_t)cpu.m_fpr[op.rd];
        cpu.m_gpr[op.rt].m_hi = 0; break;
    case K::LOP_MTC1:
        cpu.m_fpr[op.rd] = (uint64_t)(uint32_t)cpu.m_gpr[op.rt].m_lo; break;
    case K::LOP_MFC2:
        cpu.m_gpr[op.rt].m_lo = (uint64_t)(uint32_t)cpu.m_vu0[op.rd].m_lo;
        cpu.m_gpr[op.rt].m_hi = 0; break;
    case K::LOP_MTC2:
        cpu.m_vu0[op.rd].m_lo = (uint64_t)(uint32_t)cpu.m_gpr[op.rt].m_lo;
        cpu.m_vu0[op.rd].m_hi = 0; break;
    case K::LOP_LQC2:
    {
        uint32_t a = (uint32_t)(sext32(cpu.m_gpr[op.rs].m_lo) + op.immediate);
        cpu.m_vu0[op.rt].m_lo = ee_mem_read64(rt, a);
        cpu.m_vu0[op.rt].m_hi = ee_mem_read64(rt, a + 8); break;
    }
    case K::LOP_SQC2:
    {
        uint32_t a = (uint32_t)(sext32(cpu.m_gpr[op.rs].m_lo) + op.immediate);
        ee_mem_write64(rt, a, cpu.m_vu0[op.rt].m_lo);
        ee_mem_write64(rt, a + 8, cpu.m_vu0[op.rt].m_hi); break;
    }
    case K::LOP_MULT:
    {
        int64_t a64 = sext32(cpu.m_gpr[op.rs].m_lo);
        int64_t b64 = sext32(cpu.m_gpr[op.rt].m_lo);
        int64_t p = a64 * b64;
        cpu.m_lo = (uint64_t)(uint32_t)p;
        cpu.m_hi = (uint64_t)(p >> 32); break;
    }
    case K::LOP_DIV:
    {
        const int64_t lhs = sext32(cpu.m_gpr[op.rs].m_lo);
        const int64_t rhs = sext32(cpu.m_gpr[op.rt].m_lo);
        if (rhs != 0)
        {
            if (lhs == -2147483648LL && rhs == -1)
            {
                cpu.m_lo = 0xffffffff80000000ULL;
                cpu.m_hi = 0;
            }
            else
            {
                cpu.m_lo = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(lhs / rhs)));
                cpu.m_hi = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(lhs % rhs)));
            }
        }
        break;
    }
    case K::LOP_DIVU:
    {
        const uint64_t lhs = static_cast<uint32_t>(cpu.m_gpr[op.rs].m_lo);
        const uint64_t rhs = static_cast<uint32_t>(cpu.m_gpr[op.rt].m_lo);
        if (rhs != 0)
        {
            const uint32_t quotient = static_cast<uint32_t>(lhs / rhs);
            const uint32_t remainder = static_cast<uint32_t>(lhs % rhs);
            cpu.m_lo = (quotient & 0x80000000u) ?
                (0xffffffff00000000ULL | quotient) : quotient;
            cpu.m_hi = (remainder & 0x80000000u) ?
                (0xffffffff00000000ULL | remainder) : remainder;
        }
        break;
    }
    case K::LOP_MMI_ADD_B:
    case K::LOP_MMI_ADD_H:
    case K::LOP_MMI_ADD_W:
    case K::LOP_MMI_ADD_D:
    case K::LOP_MMI_SUB_B:
    case K::LOP_MMI_SUB_H:
    case K::LOP_MMI_SUB_W:
    case K::LOP_MMI_SUB_D:
    case K::LOP_MMI_ADDU_SAT_B:
    case K::LOP_MMI_ADDU_SAT_H:
    case K::LOP_MMI_ADDU_SAT_W:
    case K::LOP_MMI_AND:
    case K::LOP_MMI_OR:
    case K::LOP_MMI_XOR:
    case K::LOP_MMI_NOR:
    case K::LOP_MMI_SLL:
    case K::LOP_MMI_SRL:
    case K::LOP_MMI_SRA:
    case K::LOP_MMI_MAXU_B:
    case K::LOP_MMI_MAXU_H:
    case K::LOP_MMI_MAXU_W:
    case K::LOP_MMI_MAXS_B:
    case K::LOP_MMI_MAXS_H:
    case K::LOP_MMI_MAXS_W:
    case K::LOP_MMI_MINU_B:
    case K::LOP_MMI_MINU_H:
    case K::LOP_MMI_MINU_W:
    case K::LOP_MMI_MINS_B:
    case K::LOP_MMI_MINS_H:
    case K::LOP_MMI_MINS_W:
    case K::LOP_MMI_QFSRV:
    case K::LOP_MMI_SHUFFLE:
    case K::LOP_MMI_PACK:
        cpu.m_gpr[op.rd] = ee::mmi_apply(op.kind, cpu.m_gpr[op.rs], cpu.m_gpr[op.rt], op.sa);
        break;
    default:
        break;
    }
    cpu.m_pc = op.pc + 4;
    return cpu;
}

} // namespace

struct TestFn {
    std::string name;
    std::vector<ee::s_lifted_op> ops;
    uint32_t entry_pc;
    std::map<uint8_t, uint64_t> init_gpr;
    std::map<uint32_t, uint32_t> init_mem;
};

// Generate the lifted-op arrays consumed by the independent reference executor.
static std::string emit_ops_array(const std::string& suffix,
                                  const std::vector<ee::s_lifted_op>& ops)
{
    const std::string var_name = "ops" + suffix;
    const std::string make_name = "mk" + suffix;
    std::string out;
    out += "    std::vector<ee::s_lifted_op> " + var_name + ";\n";
    out += "    auto " + make_name + " = [&](ee::e_lift_op_kind k, uint8_t rs=0, uint8_t rt=0, uint8_t rd=0,\n";
    out += "                  int16_t imm=0, uint16_t uimm=0, uint8_t sa=0) {\n";
    out += "        ee::s_lifted_op o{}; o.pc = 0x00100000; o.kind = k; o.rs = rs; o.rt = rt;\n";
    out += "        o.rd = rd; o.immediate = imm; o.uimmediate = uimm; o.sa = sa; return o;\n";
    out += "    };\n";
    for (const auto& op : ops)
    {
        out += "    " + var_name + ".push_back(" + make_name + "(static_cast<ee::e_lift_op_kind>(" +
               std::to_string(static_cast<int>(op.kind)) + "), " +
               std::to_string(static_cast<int>(op.rs)) + ", " +
               std::to_string(static_cast<int>(op.rt)) + ", " +
               std::to_string(static_cast<int>(op.rd)) + ", " +
               std::to_string(op.immediate) + ", " +
               std::to_string(op.uimmediate) + ", " +
               std::to_string(static_cast<int>(op.sa)) + "));\n";
    }
    return out;
}

static bool build_and_run_multi_test(const std::vector<TestFn>& tests)
{
    std::string cpp_path = "build/recompiled/multi_fn_test.cpp";
    std::ofstream cpp_file(cpp_path);
    if (!cpp_file) {
        std::printf("FAIL: cannot open %s\n", cpp_path.c_str());
        return false;
    }

    cpp_file << "#include <cstdint>\n#include <cstdio>\n#include <vector>\n#include <map>\n";
    cpp_file << "#include \"ee_state.h\"\n#include \"ee_runtime.h\"\n#include \"ee_memory.h\"\n";
    cpp_file << "#include \"instruction_lifter.h\"\n";
    cpp_file << "namespace ee { void exec_op(const s_lifted_op&, s_ee_cpu_state&, ee_memory&, s_ee_runtime*, uint32_t&); }\n\n";

    // Generate all function bodies and the operation lists used by the
    // independent reference executor in the generated translation unit.
    for (size_t i = 0; i < tests.size(); ++i)
    {
        const auto& t = tests[i];
        std::string body = ee::native_codegen().emit_function_body(t.ops, t.entry_pc, t.name.c_str());
        cpp_file << body << "\n\n";
    }

    // Generate main that calls each function and compares against the
    // reference single-instruction executor.
    cpp_file << "int main() {\n";
    cpp_file << "    int failures = 0;\n";
    for (size_t i = 0; i < tests.size(); ++i)
        cpp_file << emit_ops_array(std::to_string(i), tests[i].ops) << "\n";

    for (size_t ti = 0; ti < tests.size(); ++ti) {
        const auto& t = tests[ti];

        cpp_file << "    {\n";
        cpp_file << "        ee::ee_memory mem(16u * 1024u * 1024u);\n";
        cpp_file << "        ee::s_ee_runtime rt = {};\n";
        cpp_file << "        rt.m_user = &mem;\n";
        for (auto& kv : t.init_mem) {
            cpp_file << "        ee_mem_write32(&rt, " << u32hex(kv.first) << ", " << u32hex(kv.second) << "u);\n";
        }
        cpp_file << "        ee::s_ee_cpu_state cpu = {};\n";
        cpp_file << "        cpu.m_hi = 0x1122334455667788ULL;\n";
        cpp_file << "        cpu.m_lo = 0x8877665544332211ULL;\n";
        cpp_file << "        cpu.m_pc = " << u32hex(t.entry_pc) << ";\n";
        cpp_file << "        cpu.m_gpr[31].m_lo = " << u64hex(t.init_gpr.count(31) ? t.init_gpr.at(31) : kTestRet) << ";\n";
        cpp_file << "        cpu.m_gpr[31].m_hi = 0;\n";
        cpp_file << "        cpu.m_gpr[4].m_lo = " << u64hex(t.init_gpr.count(4) ? t.init_gpr.at(4) : kRamBase) << ";\n";
        cpp_file << "        cpu.m_gpr[4].m_hi = 0;\n";
        for (auto& kv : t.init_gpr) {
            if (kv.first != 4 && kv.first != 31) {
                cpp_file << "        cpu.m_gpr[" << (int)kv.first << "].m_lo = " << u64hex(kv.second) << ";\n";
                cpp_file << "        cpu.m_gpr[" << (int)kv.first << "].m_hi = 0;\n";
            }
        }

        cpp_file << "        ee::s_ee_cpu_state ref = cpu;\n";
        cpp_file << "        uint32_t native_exit = " << t.name << "(&cpu, &rt);\n";

        cpp_file << "        ee::ee_memory ref_mem(16u * 1024u * 1024u);\n";
        cpp_file << "        ee::s_ee_runtime ref_rt = {};\n";
        cpp_file << "        ref_rt.m_user = &ref_mem;\n";
        for (auto& kv : t.init_mem) {
            cpp_file << "        ee_mem_write32(&ref_rt, " << u32hex(kv.first) << ", " << u32hex(kv.second) << "u);\n";
        }
        cpp_file << "        uint32_t ref_next_pc = " << u32hex(t.entry_pc) << ";\n";
        cpp_file << "        for (const auto& op : ops" << ti << ") {\n";
        cpp_file << "            ee::exec_op(op, ref, ref_mem, &ref_rt, ref_next_pc);\n";
        cpp_file << "            ref.m_pc = ref_next_pc;\n";
        cpp_file << "        }\n";
        cpp_file << "        ref.m_exit_reason = (uint32_t)ee::e_ee_exit_reason::EE_EXIT_RETURN;\n";
        cpp_file << "        if (native_exit != (uint32_t)ee::e_ee_exit_reason::EE_EXIT_RETURN) { std::printf(\"FAIL: test " << ti << " native exit=%u\\n\", (unsigned)native_exit); ++failures; }\n";

        cpp_file << "        for (int r = 0; r < 32; ++r) {\n";
        cpp_file << "            if (ref.m_gpr[r].m_lo != cpu.m_gpr[r].m_lo || ref.m_gpr[r].m_hi != cpu.m_gpr[r].m_hi) {\n";
        cpp_file << "                std::printf(\"FAIL: test " << ti << " gpr[%d] native=%llx/%llx ref=%llx/%llx\\n\", r,\n";
        cpp_file << "                    (unsigned long long)cpu.m_gpr[r].m_lo, (unsigned long long)cpu.m_gpr[r].m_hi,\n";
        cpp_file << "                    (unsigned long long)ref.m_gpr[r].m_lo, (unsigned long long)ref.m_gpr[r].m_hi);\n";
        cpp_file << "                ++failures;\n";
        cpp_file << "            }\n";
        cpp_file << "        }\n";
        cpp_file << "        if (ref.m_hi != cpu.m_hi || ref.m_lo != cpu.m_lo) {\n";
        cpp_file << "            std::printf(\"FAIL: test " << ti << " HI/LO native=%llx/%llx ref=%llx/%llx\\n\",\n";
        cpp_file << "                (unsigned long long)cpu.m_hi, (unsigned long long)cpu.m_lo,\n";
        cpp_file << "                (unsigned long long)ref.m_hi, (unsigned long long)ref.m_lo);\n";
        cpp_file << "            ++failures;\n";
        cpp_file << "        }\n";
        cpp_file << "        if (ref.m_pc != cpu.m_pc) {\n";
        cpp_file << "            std::printf(\"FAIL: test " << ti << " pc native=%llx ref=%llx\\n\",\n";
        cpp_file << "                (unsigned long long)cpu.m_pc, (unsigned long long)ref.m_pc);\n";
        cpp_file << "            ++failures;\n";
        cpp_file << "        }\n";
        cpp_file << "        if (ref.m_exit_reason != cpu.m_exit_reason) {\n";
        cpp_file << "            std::printf(\"FAIL: test " << ti << " exit native=%u ref=%u\\n\",\n";
        cpp_file << "                (unsigned)cpu.m_exit_reason, (unsigned)ref.m_exit_reason);\n";
        cpp_file << "            ++failures;\n";
        cpp_file << "        }\n";
        std::map<uint32_t, bool> memory_checks;
        for (const auto& kv : t.init_mem) memory_checks[kv.first] = true;
        for (const auto& op : t.ops)
        {
            const bool is_store = op.kind == ee::LOP_SB || op.kind == ee::LOP_SH ||
                                  op.kind == ee::LOP_SW || op.kind == ee::LOP_SD ||
                                  op.kind == ee::LOP_SQ || op.kind == ee::LOP_SWC1 ||
                                  op.kind == ee::LOP_SQC2;
            if (!is_store) continue;
            const uint64_t base = t.init_gpr.count(op.rs) ? t.init_gpr.at(op.rs) : 0;
            memory_checks[(uint32_t)(base + op.immediate)] = true;
            if (op.kind == ee::LOP_SQ || op.kind == ee::LOP_SQC2)
                memory_checks[(uint32_t)(base + op.immediate + 8)] = true;
        }
        for (const auto& kv : memory_checks)
        {
            cpp_file << "        { const uint32_t a = " << u32hex(kv.first) << ";\n";
            cpp_file << "          const uint32_t native_mem = ee_mem_read32(&rt, a);\n";
            cpp_file << "          const uint32_t ref_mem_val = ee_mem_read32(&ref_rt, a);\n";
            cpp_file << "          if (native_mem != ref_mem_val) {\n";
            cpp_file << "            std::printf(\"FAIL: test " << ti << " mem[0x%x] native=0x%x ref=0x%x\\n\", a, native_mem, ref_mem_val); ++failures; } }\n";
        }
        cpp_file << "    }\n";
    }

    cpp_file << "    if (failures == 0) {\n";
    cpp_file << "        std::printf(\"multi-function test: ALL PASS\\n\");\n";
    cpp_file << "        return 0;\n";
    cpp_file << "    } else {\n";
    cpp_file << "        std::printf(\"multi-function test: %d failures\\n\", failures);\n";
    cpp_file << "        return 1;\n";
    cpp_file << "    }\n";
    cpp_file << "}\n";
    cpp_file.close();

    std::string obj_path = "build/recompiled/multi_fn_test.obj";
    std::string compile_cmd =
        "clang++ -O2 -Wall -Wextra -I include -I include/ee -I src/lift -I src/runtime "
        + cpp_path + " tests/smoke/native_smoke.cpp src/runtime/ee_memory.cpp "
        + "src/runtime/ee_exceptions.cpp -o " + obj_path;
    int rc = std::system(compile_cmd.c_str());
    if (rc != 0) {
        std::printf("FAIL: compile multi-function test\n");
        return false;
    }

    std::string run_cmd = obj_path + " > build/recompiled/multi_fn_result.txt 2>&1";
    rc = std::system(run_cmd.c_str());
    if (rc != 0) {
        std::printf("FAIL: run multi-function test\n");
        return false;
    }

    std::string result_path = "build/recompiled/multi_fn_result.txt";
    std::ifstream result_file(result_path);
    std::string result((std::istreambuf_iterator<char>(result_file)),
                       std::istreambuf_iterator<char>());
    if (result.find("ALL PASS") == std::string::npos) {
        std::printf("FAIL: multi-function test did not pass\n%s\n", result.c_str());
        return false;
    }
    std::printf("multi-function test: ALL PASS\n");
    return true;
}

int main()
{
    std::filesystem::create_directories("build/recompiled");

    std::vector<TestFn> tests;

    // Test 1: Simple arithmetic function
    {
        std::vector<ee::s_lifted_op> ops0;
        auto mk = [&](ee::e_lift_op_kind k, uint8_t rs=0, uint8_t rt=0, uint8_t rd=0,
                      int16_t imm=0, uint16_t uimm=0, uint8_t sa=0) {
            ee::s_lifted_op o{}; o.pc = 0x00100000; o.kind = k; o.rs = rs; o.rt = rt;
            o.rd = rd; o.immediate = imm; o.uimmediate = uimm; o.sa = sa; return o;
        };
        ops0.push_back(mk(ee::LOP_ADDIU, 0, 2, 0, 42));
        ops0.push_back(mk(ee::LOP_ORI, 0, 3, 0, 0, 0xFF));
        ops0.push_back(mk(ee::LOP_ANDI, 3, 8, 0, 0, 0x0F));

        std::map<uint8_t, uint64_t> gpr;
        gpr[31] = kTestRet;
        gpr[4] = kRamBase;
        tests.push_back({"fn_arith", ops0, 0x00100000, gpr, {}});
    }

    // Test 2: Load/store function
    {
        std::vector<ee::s_lifted_op> ops1;
        auto mk = [&](ee::e_lift_op_kind k, uint8_t rs=0, uint8_t rt=0, uint8_t rd=0,
                      int16_t imm=0, uint16_t uimm=0, uint8_t sa=0) {
            ee::s_lifted_op o{}; o.pc = 0x00100000; o.kind = k; o.rs = rs; o.rt = rt;
            o.rd = rd; o.immediate = imm; o.uimmediate = uimm; o.sa = sa; return o;
        };
        ops1.push_back(mk(ee::LOP_LW, 4, 8, 0, 0));
        ops1.push_back(mk(ee::LOP_ADDIU, 0, 2, 0, 99));
        ops1.push_back(mk(ee::LOP_SW, 4, 2, 0, 4));

        std::map<uint8_t, uint64_t> gpr;
        gpr[31] = kTestRet;
        gpr[4] = kRamBase;
        std::map<uint32_t, uint32_t> mem;
        mem[kRamBase] = 0x12345678u;
        tests.push_back({"fn_loadstore", ops1, 0x00100000, gpr, mem});
    }

    // Test 3: COP0 MFC0/MTC0 function
    {
        std::vector<ee::s_lifted_op> ops2;
        auto mk = [&](ee::e_lift_op_kind k, uint8_t rs=0, uint8_t rt=0, uint8_t rd=0,
                      int16_t imm=0, uint16_t uimm=0, uint8_t sa=0) {
            ee::s_lifted_op o{}; o.pc = 0x00100000; o.kind = k; o.rs = rs; o.rt = rt;
            o.rd = rd; o.immediate = imm; o.uimmediate = uimm; o.sa = sa; return o;
        };
        ops2.push_back(mk(ee::LOP_MTC0, 0, 4, 12));
        ops2.push_back(mk(ee::LOP_MFC0, 0, 8, 12));

        std::map<uint8_t, uint64_t> gpr;
        gpr[31] = kTestRet;
        gpr[4] = 0xDEADBEEFu;
        tests.push_back({"fn_cop0", ops2, 0x00100000, gpr, {}});
    }

    // The legacy straight-line code generator is still built, so make its
    // DIV/DIVU behaviour agree with the LLVM backend's explicit edge policy.
    struct s_div_case
    {
        const char* m_name;
        ee::e_lift_op_kind m_kind;
        uint64_t m_lhs;
        uint64_t m_rhs;
    };
    const s_div_case div_cases[] =
    {
        {"fn_div_signed_normal", ee::LOP_DIV, 0xffffffffffffffefULL, 5},
        {"fn_div_signed_overflow", ee::LOP_DIV, 0x80000000ULL, 0xffffffffULL},
        {"fn_div_signed_zero", ee::LOP_DIV, 0xfffffffffffffff0ULL, 0},
        {"fn_div_signed_zero_zero", ee::LOP_DIV, 0, 0},
        {"fn_div_unsigned_normal", ee::LOP_DIVU, 0xfffffffeULL, 3},
        {"fn_div_unsigned_highbit", ee::LOP_DIVU, 0xffffffffULL, 1},
        {"fn_div_unsigned_zero", ee::LOP_DIVU, 0xfffffffeULL, 0},
        {"fn_div_unsigned_zero_zero", ee::LOP_DIVU, 0, 0}
    };
    for (const s_div_case& test_case : div_cases)
    {
        ee::s_lifted_op op{};
        op.pc = 0x00100000u;
        op.kind = test_case.m_kind;
        op.rs = 4;
        op.rt = 5;
        std::map<uint8_t, uint64_t> gpr;
        gpr[4] = test_case.m_lhs;
        gpr[5] = test_case.m_rhs;
        tests.push_back({test_case.m_name, {op}, 0x00100000u, gpr, {}});
    }

    std::printf("\n-- multi-function compilation and linking --\n");
    return build_and_run_multi_test(tests) ? 0 : 1;
}
