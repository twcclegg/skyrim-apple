/* Sampling profiler: SIGPROF every 1 ms of process CPU time records the
 * interrupted PC; the watchdog prints the hottest functions every 30 s, so a
 * player's log.txt shows where the time goes. save/noprof.txt turns it off. */
#define _GNU_SOURCE
#include "rt.h"
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#ifdef __APPLE__
#include <sys/ucontext.h>
#include <mach/mach.h>
#include <TargetConditionals.h>
#else
#include <ucontext.h>
#endif
#include <unistd.h>
#include <dirent.h>
#include <time.h>

#define PROF_N (1u << 20)
static uintptr_t *g_samp;
static volatile uint32_t g_samp_n;
static uint32_t g_samp_done;
static int g_prof_on;

#ifdef __APPLE__
/* Apple platforms: SIGPROF is delivered to whichever thread the kernel picks, in practice one entering or
 * leaving a system call, so its samples said the game spent half its time in pthread_cond_signal when a real
 * stack sample showed about 1%. Sample from a thread of our own instead: every 5 ms, read the program counter
 * of each thread that is on a core right now. The top bit of a sample marks the main thread, which gets its
 * own table in the report, since that thread is the frame. */
#include <pthread.h>
#define PROF_MAIN ((uintptr_t)1 << 63)
#define PROF_TICK_US 5000
static mach_port_t g_prof_main;
static void *prof_sampler(void *arg) {
    (void)arg;
    pthread_setname_np("prof-sampler");
    mach_port_t self = mach_thread_self();
    for (;;) {
        usleep(PROF_TICK_US);
        thread_act_array_t threads;
        mach_msg_type_number_t count;
        if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) continue;
        for (mach_msg_type_number_t i = 0; i < count; i++) {
            thread_basic_info_data_t basic;
            mach_msg_type_number_t nb = THREAD_BASIC_INFO_COUNT;
            if (threads[i] != self && thread_info(threads[i], THREAD_BASIC_INFO, (thread_info_t)&basic, &nb) == KERN_SUCCESS &&
                basic.run_state == TH_STATE_RUNNING) {
#if defined(__aarch64__)
                arm_thread_state64_t state;
                mach_msg_type_number_t ns = ARM_THREAD_STATE64_COUNT;
                if (thread_get_state(threads[i], ARM_THREAD_STATE64, (thread_state_t)&state, &ns) == KERN_SUCCESS) {
                    uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(state) & ~PROF_MAIN;
                    uint32_t k = __atomic_fetch_add(&g_samp_n, 1, __ATOMIC_RELAXED);
                    g_samp[k & (PROF_N - 1)] = pc | (threads[i] == g_prof_main ? PROF_MAIN : 0);
                }
#endif
            }
            mach_port_deallocate(mach_task_self(), threads[i]);
        }
        vm_deallocate(mach_task_self(), (vm_address_t)threads, (vm_size_t)count * sizeof *threads);
    }
    return NULL;
}
#endif

static void on_prof(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si;
    ucontext_t *uc = uc_;
#if defined(__APPLE__) && defined(__aarch64__)
    uintptr_t pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
#elif defined(__APPLE__) && defined(__x86_64__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext->__ss.__rip;
#elif defined(__aarch64__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
#elif defined(__x86_64__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#else
    uintptr_t pc = 0;
#endif
    uint32_t i = __atomic_fetch_add(&g_samp_n, 1, __ATOMIC_RELAXED);
    g_samp[i & (PROF_N - 1)] = pc;
}

void prof_start(void) {
    extern char g_save_dir[];
    char path[1100];
    snprintf(path, sizeof path, "%s/noprof.txt", g_save_dir);
    if (access(path, F_OK) == 0) return;
    /* LB_PROF=0 turns it off, LB_PROF=1 on. On visionOS it is off unless asked for: sampling briefly stops every
     * running thread 200 times a second, which costs frame time and battery on the headset. */
    const char *want = getenv("LB_PROF");
    if (want && *want == '0') return;
#if defined(__APPLE__) && TARGET_OS_VISION
    if (!want || *want != '1') { LOG("profiler off on visionOS (LB_PROF=1 turns it on)"); return; }
#endif
    g_samp = calloc(PROF_N, sizeof *g_samp);
    if (!g_samp) return;
#if defined(__APPLE__) && defined(__aarch64__)
    {
        pthread_t sampler;
        g_prof_main = pthread_mach_thread_np(pthread_self());  /* prof_start runs on the main thread */
        if (pthread_create(&sampler, NULL, prof_sampler, NULL)) return;
        pthread_detach(sampler);
        g_prof_on = 1;
        LOG("profiler on: on-core threads sampled every %d ms (save/noprof.txt turns it off)", PROF_TICK_US / 1000);
        return;
    }
#endif
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    struct itimerval it = {{0, 1000}, {0, 1000}};
    setitimer(ITIMER_PROF, &it, NULL);
    g_prof_on = 1;
    LOG("profiler on (save/noprof.txt turns it off)");
}

typedef struct { uintptr_t key; const char *name; uintptr_t base; uint32_t n; } Bin;

static int bin_cmp(const void *a, const void *b) {
    uint32_t x = ((const Bin *)a)->n, y = ((const Bin *)b)->n;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* dladdr, cached per 256-byte block of code (code never moves): one lookup per block for the life of the process
 * instead of one per sample per report, which cost about half a core during busy stretches. Watchdog thread
 * only. A full cache falls back to calling dladdr. */
typedef struct { uintptr_t block, key, base; const char *name; } SymCache;
static void sym_of(uintptr_t pc, uintptr_t *key, const char **name, uintptr_t *base) {
    enum { NC = 1 << 15 };
    static SymCache *cache;
    if (!cache) cache = calloc(NC, sizeof *cache);
    uintptr_t block = pc & ~(uintptr_t)0xFF;
    uint32_t h = (uint32_t)((block >> 8) * 2654435761u) & (NC - 1);
    for (uint32_t probe = 0; cache && probe < 64; probe++, h = (h + 1) & (NC - 1)) {
        if (cache[h].block == block) { *key = cache[h].key; *name = cache[h].name; *base = cache[h].base; return; }
        if (!cache[h].block) break;
    }
    Dl_info di;
    *key = 0; *name = "?"; *base = 0;
    if (dladdr((void *)pc, &di)) {
        // unexported code: bin by 256-byte block, printed as lib+offset
        *key = di.dli_saddr ? (uintptr_t)di.dli_saddr : block;
        *name = di.dli_sname ? di.dli_sname : di.dli_fname ? di.dli_fname : "?";
        *base = di.dli_sname ? 0 : (uintptr_t)di.dli_fbase;
    }
    /* A function's symbol is cached per block, so a block spanning two functions is credited to the first one
     * seen in it; at 256 bytes that blurs only neighbouring small functions. */
    if (cache && !cache[h].block) cache[h] = (SymCache){block, *key, *base, *name};
}

/* Top functions among samples [start, end); main_only keeps the main thread's (Apple sampler). */
static void prof_table(uint32_t start, uint32_t end, int main_only) {
    uint32_t total = 0;
    enum { NB = 8192 };
    Bin *bins = calloc(NB, sizeof *bins);
    if (!bins) return;
    for (uint32_t k = start; k != end; k++) {
        uintptr_t pc = g_samp[k & (PROF_N - 1)];
#ifdef __APPLE__
        if (main_only && !(pc & PROF_MAIN)) continue;
        pc &= ~PROF_MAIN;
#else
        (void)main_only;
#endif
        total++;
        uintptr_t key = 0;
        const char *name = "?";
        uintptr_t base = 0;
        if (pc) sym_of(pc, &key, &name, &base);
        uint32_t h = (uint32_t)((key >> 2) * 2654435761u) % NB;
        for (uint32_t probe = 0; probe < NB; probe++, h = (h + 1) % NB) {
            if (!bins[h].n) { bins[h].key = key; bins[h].name = name; bins[h].base = base; }
            if (bins[h].key == key) { bins[h].n++; break; }
        }
    }
    qsort(bins, NB, sizeof *bins, bin_cmp);
#if defined(__APPLE__) && defined(__aarch64__)
    LOG("profile, %s: %u samples (%d ms on a core each)", main_only ? "main thread" : "all threads", total, PROF_TICK_US / 1000);
#else
    LOG("profile: %u samples (1 ms CPU each)", total);
#endif
    for (int i = 0; i < (main_only ? 40 : 30) && bins[i].n; i++) {
        const char *b = strrchr(bins[i].name, '/');
        if (bins[i].base)
            LOG("  %5.1f%%  %s+0x%lx", 100.0 * bins[i].n / total, b ? b + 1 : bins[i].name,
                (unsigned long)(bins[i].key - bins[i].base));
        else
            LOG("  %5.1f%%  %s", 100.0 * bins[i].n / total, b ? b + 1 : bins[i].name);
    }
    free(bins);
}

/* Called from the watchdog thread: top functions since the last report. */
void prof_report(void) {
    if (!g_prof_on) return;
    uint32_t end = g_samp_n, start = g_samp_done;
    if (end - start > PROF_N) start = end - PROF_N;
    g_samp_done = end;
    if (end == start) return;
    prof_table(start, end, 0);
#if defined(__APPLE__) && defined(__aarch64__)
    prof_table(start, end, 1);
#endif
}

/* Per-thread CPU use since the last call: the SIGPROF
 * sampler above is limited by the kernel tick, so its sample count is not a
 * measure of how busy the cores are. */
#ifdef __APPLE__
void cpu_report(void) {
    enum { NT = 256 };
    static uint64_t tids[NT], last_us[NT];
    static double last_t;
    uint64_t next_tids[NT] = {0}, next_us[NT] = {0};
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9, dt = now - last_t;
    thread_act_array_t threads;
    mach_msg_type_number_t count;
    kern_return_t kr = task_threads(mach_task_self(), &threads, &count);
    if (kr != KERN_SUCCESS) { LOG("cpu: task_threads failed (%d)", kr); return; }
    char out[1600] = "";
    double total = 0;
    unsigned n = 0;
    for (mach_msg_type_number_t i = 0; i < count; i++) {
        thread_basic_info_data_t basic;
        thread_identifier_info_data_t ident;
        mach_msg_type_number_t nb = THREAD_BASIC_INFO_COUNT, ni = THREAD_IDENTIFIER_INFO_COUNT;
        kern_return_t kb = thread_info(threads[i], THREAD_BASIC_INFO, (thread_info_t)&basic, &nb);
        kern_return_t ki = thread_info(threads[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&ident, &ni);
        mach_port_deallocate(mach_task_self(), threads[i]);
        if (kb != KERN_SUCCESS || ki != KERN_SUCCESS || n == NT) continue;
        uint64_t us = ((uint64_t)basic.user_time.seconds + basic.system_time.seconds) * 1000000ull
                    + basic.user_time.microseconds + basic.system_time.microseconds;
        next_tids[n] = ident.thread_id;
        next_us[n++] = us;
        if (last_t == 0 || dt <= 0) continue;
        for (unsigned k = 0; k < NT; k++) {
            if (tids[k] != ident.thread_id) continue;
            double pct = us >= last_us[k] ? 100.0 * (us - last_us[k]) / 1e6 / dt : 0;
            total += pct;
            if (pct >= 3 && strlen(out) < sizeof out - 64) {
                char tmp[64];
                snprintf(tmp, sizeof tmp, " tid%llu:%.0f%%", (unsigned long long)ident.thread_id, pct);
                strcat(out, tmp);
            }
            break;
        }
    }
    vm_deallocate(mach_task_self(), (vm_address_t)threads, (vm_size_t)count * sizeof *threads);
    if (last_t != 0) LOG("cpu: %.0f%% of one core in total;%s", total, out);
    memcpy(tids, next_tids, sizeof tids);
    memcpy(last_us, next_us, sizeof last_us);
    last_t = now;
}
#else
/* Linux and Android expose per-thread counters in /proc/self/task. */
void cpu_report(void) {
    enum { NT = 256 };
    static int tids[NT];
    static unsigned long long last[NT];
    static double last_t;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9, dt = now - last_t;
    long hz = sysconf(_SC_CLK_TCK);
    DIR *d = opendir("/proc/self/task");
    if (!d) return;
    char line[2048], out[1600];
    int n = 0;
    double total = 0;
    out[0] = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        int tid = atoi(e->d_name);
        if (tid <= 0) continue;
        char path[64];
        snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        size_t len = fread(line, 1, sizeof line - 1, f);
        fclose(f);
        line[len] = 0;
        char *lp = strchr(line, '('), *rp = strrchr(line, ')');
        if (!lp || !rp) continue;
        unsigned long long ut = 0, st = 0;
        int cpu = -1;
        /* fields after ')': state(3) ... utime(14) stime(15) ... processor(39) */
        char *p = rp + 2;
        for (int field = 3; field <= 39 && p && *p; field++) {
            if (field == 14) ut = strtoull(p, NULL, 10);
            if (field == 15) st = strtoull(p, NULL, 10);
            if (field == 39) cpu = atoi(p);
            p = strchr(p, ' ');
            if (p) p++;
        }
        unsigned long long t = ut + st, prev = 0;
        int slot = -1;
        for (int k = 0; k < NT; k++) if (tids[k] == tid) { slot = k; break; }
        if (slot < 0) for (int k = 0; k < NT; k++) if (!tids[k]) { slot = k; tids[k] = tid; last[k] = t; break; }
        if (slot < 0) continue;
        prev = last[slot];
        last[slot] = t;
        if (last_t == 0 || dt <= 0) continue;
        double pct = 100.0 * (double)(t - prev) / hz / dt;
        total += pct;
        if (pct >= 3 && strlen(out) < sizeof out - 64) {
            *rp = 0;
            char tmp[64];
            snprintf(tmp, sizeof tmp, " %.15s/%d:%.0f%%@%d", lp + 1, tid, pct, cpu);
            strcat(out, tmp);
        }
        n++;
    }
    closedir(d);
    if (last_t != 0) LOG("cpu: %.0f%% of one core in total;%s", total, out);
    last_t = now;
}
#endif
