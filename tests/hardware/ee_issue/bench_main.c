/* Independent console measurement program. No expected cycle values. */
#include <stdint.h>
#include <stdio.h>
#include "bench_cases.h"

static uint32_t read_pccr(void)
{
    uint32_t value;
    __asm__ volatile("mfps %0,0" : "=r"(value) :: "memory");
    return value;
}

static uint32_t read_pcr0(void)
{
    uint32_t value;
    __asm__ volatile("mfpc %0,0" : "=r"(value) :: "memory");
    return value;
}

static void write_pccr(uint32_t value)
{
    __asm__ volatile("mtps %0,0\n\tsync.p" :: "r"(value) : "memory");
}

static void write_pcr0(uint32_t value)
{
    __asm__ volatile("mtpc %0,0\n\tsync.p" :: "r"(value) : "memory");
}

int main(void)
{
    static const uint32_t data[16] __attribute__((aligned(64))) = {1u};
    /* Event 1, processor cycles, all execution modes, counter enabled.
       Counter programming follows the pinned original performance test.
       Interrupts are left enabled; raw repeated measurements expose noise. */
    const uint32_t processor_cycles = 0x8000003eu;
    puts("EE_ISSUE_COUNTER_V1");
    puts("case,repeat,sample,pcr0_delta");
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        uint32_t samples[BENCH_SAMPLES];
        const uint32_t previous_pccr = read_pccr();
        write_pccr(0u);
        const uint32_t previous_pcr0 = read_pcr0();
        write_pcr0(0u);
        write_pccr(processor_cycles);
        /* Warm the exact same straight-line function and data. */
        for (unsigned warm = 0; warm < 3u; ++warm)
        {
            (void)cases[i].run(data);
        }
        for (unsigned sample = 0; sample < BENCH_SAMPLES; ++sample)
        {
            samples[sample] = cases[i].run(data);
        }
        write_pccr(0u);
        write_pcr0(previous_pcr0);
        write_pccr(previous_pccr);
        for (unsigned sample = 0; sample < BENCH_SAMPLES; ++sample)
        {
            printf("%s,%u,%u,%u\n", cases[i].name, cases[i].repeat,
                   sample, (unsigned)samples[sample]);
        }
    }
    puts("EE_ISSUE_COUNTER_END");
    return 0;
}
