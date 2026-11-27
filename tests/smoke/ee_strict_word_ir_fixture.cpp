#include "ee_decoder.h"
#include "ee_strict_word_ir.h"
#include <cstdlib>
#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::fprintf(stderr, "usage: ee_strict_word_ir_fixture output.ll pc opcode\n");
        return 2;
    }
    const uint32_t pc = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 0));
    const uint32_t opcode = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 0));
    llvm::llvm_context context("ee_strict_word");
    if (!llvm::emit_strict_word_entry(context, ee::decode(pc, opcode)))
    {
        return 3;
    }
    return context.write_ir_to_file(argv[1]) ? 0 : 4;
}
