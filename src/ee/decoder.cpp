#include "ee_decoder.h"
#include "ee_decoder_detail.h"

namespace ee
{
s_ee_instruction decode(uint32_t pc, uint32_t word)
{
    return decoder_detail::decode(pc, word);
}
} // namespace ee
