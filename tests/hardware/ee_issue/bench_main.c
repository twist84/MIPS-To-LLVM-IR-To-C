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

static uint32_t read_pcr1(void)
{
    uint32_t value;
    __asm__ volatile("mfpc %0,1" : "=r"(value) :: "memory");
    return value;
}

static void write_pcr1(uint32_t value)
{
    __asm__ volatile("mtpc %0,1\n\tsync.p" :: "r"(value) : "memory");
}

static void write_pccr(uint32_t value)
{
    __asm__ volatile("mtps %0,0\n\tsync.p" :: "r"(value) : "memory");
}

static void write_pcr0(uint32_t value)
{
    __asm__ volatile("mtpc %0,0\n\tsync.p" :: "r"(value) : "memory");
}

static void capture(uint32_t control, int issue_pair)
{
    static const uint32_t data[16] __attribute__((aligned(64))) = {1u};
    puts(issue_pair ? "EE_ISSUE_PAIR_COUNTER_V1" : "EE_ISSUE_COUNTER_V1");
    puts(issue_pair ? "case,repeat,sample,single_issue_delta,dual_issue_delta" :
                      "case,repeat,sample,pcr0_delta");
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        uint32_t samples0[BENCH_SAMPLES];
        uint32_t samples1[BENCH_SAMPLES];
        const uint32_t previous_pccr = read_pccr();
        write_pccr(0u);
        const uint32_t previous_pcr0 = read_pcr0();
        const uint32_t previous_pcr1 = read_pcr1();
        write_pcr0(0u);
        write_pcr1(0u);
        write_pccr(control);
        /* Warm the exact same straight-line function and data.
           Interrupts remain enabled; report all raw samples. */
        uint32_t ignored;
        for (unsigned warm = 0; warm < 3u; ++warm)
        {
            (void)cases[i].run(data, &ignored);
        }
        for (unsigned sample = 0; sample < BENCH_SAMPLES; ++sample)
        {
            samples0[sample] = cases[i].run(data, &samples1[sample]);
        }
        write_pccr(0u);
        write_pcr0(previous_pcr0);
        write_pcr1(previous_pcr1);
        write_pccr(previous_pccr);
        for (unsigned sample = 0; sample < BENCH_SAMPLES; ++sample)
        {
            if (issue_pair)
            {
                printf("%s,%u,%u,%u,%u\n", cases[i].name, cases[i].repeat,
                       sample, (unsigned)samples0[sample], (unsigned)samples1[sample]);
            }
            else
            {
                printf("%s,%u,%u,%u\n", cases[i].name, cases[i].repeat,
                       sample, (unsigned)samples0[sample]);
            }
        }
    }
    puts(issue_pair ? "EE_ISSUE_PAIR_COUNTER_END" : "EE_ISSUE_COUNTER_END");
}

int main(void)
{
    /* Manual v6.0 Table 7-1: PCR0 selector 1 = processor cycles;
       selector 2 = single issue, PCR1 selector 2 = dual issue.
       Enable all modes. The two passes use identical aligned bodies. */
    capture(0x8000003eu, 0);
    capture(0x8001785eu, 1);
    return 0;
}
