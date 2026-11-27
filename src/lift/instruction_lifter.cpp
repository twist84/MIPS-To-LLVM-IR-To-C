#include "instruction_lifter.h"
#include "instruction_lifter_detail.h"

namespace ee
{
s_lifted_op lift(const s_ee_instruction& insn)
{
    return lifter_detail::lift(insn);
}

std::vector<s_lifted_op> lift_function(const std::vector<s_ee_instruction>& insns)
{
    std::vector<s_lifted_op> ops;
    ops.reserve(insns.size());
    for (const auto& ins : insns) {
        ops.push_back(lift(ins));
    }
    return ops;
}

} // namespace ee
