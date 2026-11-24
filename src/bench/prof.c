/* _GNU_SOURCE: REG_RIP в ucontext_t на x86-64. */
#define _GNU_SOURCE
#include "prof.h"

#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>

#define MAX_SAMPLES (1 << 19)

typedef struct {
    uintptr_t pc, lr;
} sample;

static sample *samples;
static atomic_int sample_count;
static int period;

static void on_sigprof(int sig, siginfo_t *si, void *ucv)
{
    (void)sig;
    (void)si;
    ucontext_t *uc = ucv;
    int i = atomic_fetch_add_explicit(&sample_count, 1, memory_order_relaxed);
    if (i >= MAX_SAMPLES)
        return;
#if defined(__arm__)
    samples[i].pc = uc->uc_mcontext.arm_pc;
    samples[i].lr = uc->uc_mcontext.arm_lr;
#elif defined(__x86_64__)
    samples[i].pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    samples[i].lr = 0;
#else
    (void)uc;
    samples[i].pc = samples[i].lr = 0;
#endif
}

int prof_start(int period_us)
{
    samples = calloc(MAX_SAMPLES, sizeof *samples);
    if (!samples)
        return -1;
    atomic_store(&sample_count, 0);
    period = period_us;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sigprof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    struct itimerval it = { { 0, period_us }, { 0, period_us } };
    if (sigaction(SIGPROF, &sa, NULL) != 0 || setitimer(ITIMER_PROF, &it, NULL) != 0) {
        free(samples);
        samples = NULL;
        return -1;
    }
    return 0;
}

static int cmp_sample(const void *a, const void *b)
{
    const sample *x = a, *y = b;
    if (x->pc != y->pc)
        return x->pc < y->pc ? -1 : 1;
    return x->lr < y->lr ? -1 : x->lr > y->lr;
}

int prof_stop(const char *path)
{
    if (!samples)
        return -1;
    struct itimerval off = { { 0, 0 }, { 0, 0 } };
    setitimer(ITIMER_PROF, &off, NULL);
    signal(SIGPROF, SIG_IGN);

    int n = atomic_load(&sample_count);
    int kept = n < MAX_SAMPLES ? n : MAX_SAMPLES;
    qsort(samples, (size_t)kept, sizeof *samples, cmp_sample);

    FILE *f = fopen(path, "w");
    if (!f) {
        free(samples);
        samples = NULL;
        return -1;
    }
    fprintf(f, "# washi prof: %d сэмплов (сохранено %d), период %d мкс\n# maps\n", n, kept, period);
    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps) {
        char line[512];
        while (fgets(line, sizeof line, maps))
            if (strstr(line, " r-xp "))
                fputs(line, f);
        fclose(maps);
    }
    fprintf(f, "# samples: count pc lr\n");
    for (int i = 0; i < kept;) {
        int j = i;
        while (j < kept && samples[j].pc == samples[i].pc && samples[j].lr == samples[i].lr)
            j++;
        fprintf(f, "%d %lx %lx\n", j - i, (unsigned long)samples[i].pc, (unsigned long)samples[i].lr);
        i = j;
    }
    fclose(f);
    free(samples);
    samples = NULL;
    return n;
}
