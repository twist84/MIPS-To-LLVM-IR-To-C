#include "ee_decoder.h"
#include "ee_strict_word_ir.h"
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 4 || argc > 259)
    {
        std::fprintf(stderr, "usage: ee_strict_word_ir_fixture output.ll pc opcode [opcode ...]\n");
        return 2;
    }
    const uint32_t pc = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 0));
    std::vector<ee::s_ee_instruction> instructions;
    for (int i = 3; i < argc; ++i)
    {
        const uint64_t address = static_cast<uint64_t>(pc) + static_cast<uint64_t>(i - 3) * 4u;
        if (address > 0xffffffffull)
        {
            return 3;
        }
        const uint32_t opcode = static_cast<uint32_t>(std::strtoul(argv[i], nullptr, 0));
        instructions.push_back(ee::decode(static_cast<uint32_t>(address), opcode));
    }
    llvm::llvm_context context("ee_strict_word");
    const bool emitted = instructions.size() == 1u
        ? llvm::emit_strict_word_entry(context, instructions.front())
        : ee::ee_strict_branch_supported(instructions[instructions.size() - 2u].opcode_word)
            ? llvm::emit_strict_memory_terminated_block(context, instructions)
            : llvm::emit_strict_memory_sequence(context, instructions);
    if (!emitted)
    {
        return 3;
    }
    return context.write_ir_to_file(argv[1]) ? 0 : 4;
}
