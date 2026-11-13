/* Hitch sampler (LB_HITCH_MS=<ms>, off by default; Apple arm64 only): find out what a long frame was doing.
 *
 * The frame-rate average hides the frames that hurt in a headset: every benchmark run has a dozen or more frames
 * over 50 ms that nothing attributes. With this on, a thread checks every 2 ms how long the current frame has been
 * running. Once it is past the threshold, the thread that presents frames (the game's main thread) is suspended
 * for a moment and its stack is walked by frame pointers. When the frame ends, a frame over the threshold gets
 * one log line with its length, and its most common stacks are printed with symbols: lifted guest functions show
 * as f_<address>, so the chain says whether it was a shader or pipeline compile in DXVK, file decompression or
 * cell loading. The main thread often only waits on another (the render or job threads), so every other thread
 * that is running at that moment is sampled too (every 10 ms) and its most common stacks are printed under its name. Every 30 s
 * the functions that appeared in the most hitch samples (inclusively, anywhere on the stack) are summarised.
 *
 * Cost when on: one round of suspends and stack walks per 2 ms of a long frame only; nothing during normal
 * frames. */
#include "rt.h"
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#if defined(__APPLE__) && defined(__aarch64__)
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define HITCH_DEPTH 40
#define HITCH_STACKS 128  /* distinct stacks kept per long frame */
#define HITCH_FUNCS 4096  /* functions counted for the 30 s summary */

typedef struct { uintptr_t pc[HITCH_DEPTH]; int n, hits; char thread[24]; } Stack;  /* thread: "" = main */
static Stack g_stacks[HITCH_STACKS];
static int g_nstacks, g_samples;
static struct { uintptr_t fn; uint32_t hits; } g_funcs[HITCH_FUNCS];
static uint32_t g_func_samples, g_hitches;

static double g_threshold_ms;
static mach_port_t g_thread;
static _Atomic uint64_t g_frame_start_ns;
static _Atomic int g_frame_no;

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

/* Symbol of a return address, cached: dladdr is slow and the same few hundred addresses recur. */
#define SYM_CACHE 8192
static struct { uintptr_t pc, start; const char *name; } g_syms[SYM_CACHE];
static const char *sym(uintptr_t pc, uintptr_t *start) {
    uint32_t i = (uint32_t)(pc >> 2) * 2654435761u % SYM_CACHE;
    if (g_syms[i].pc != pc) {
        Dl_info info;
        int ok = dladdr((void *)pc, &info) && info.dli_sname;
        g_syms[i].pc = pc;
        g_syms[i].start = ok ? (uintptr_t)info.dli_saddr : pc;
        g_syms[i].name = ok ? info.dli_sname : "?";
    }
    *start = g_syms[i].start;
    return g_syms[i].name;
}

/* Walks one suspended thread's stack by frame pointers, within its stack bounds [lo, hi). */
static void walk(mach_port_t th, uintptr_t lo, uintptr_t hi, Stack *s) {
    arm_thread_state64_t st;
    mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(th, ARM_THREAD_STATE64, (thread_state_t)&st, &n) != KERN_SUCCESS) return;
    s->pc[s->n++] = (uintptr_t)arm_thread_state64_get_pc(st);
    uintptr_t lr = (uintptr_t)arm_thread_state64_get_lr(st), fp = (uintptr_t)arm_thread_state64_get_fp(st);
    if (lr) s->pc[s->n++] = lr;
    while (s->n < HITCH_DEPTH && fp >= lo && fp + 16 <= hi && !(fp & 7)) {
        uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];
        if (!ret) break;
        s->pc[s->n++] = ret & 0x0000FFFFFFFFFFFFull;  /* strip pointer authentication bits */
        if (next <= fp) break;
        fp = next;
    }
}

static void keep(const Stack *s) {
    for (int i = 0; i < g_nstacks; i++)
        if (g_stacks[i].n == s->n && !strcmp(g_stacks[i].thread, s->thread) &&
            !memcmp(g_stacks[i].pc, s->pc, sizeof(uintptr_t) * s->n)) { g_stacks[i].hits++; return; }
    if (g_nstacks < HITCH_STACKS) { g_stacks[g_nstacks] = *s; g_stacks[g_nstacks++].hits = 1; }
}

/* One sample: the frame thread's stack and, when others is set, the stack of every other thread running then. Each thread is
 * suspended only while its registers and stack are read; nothing that can take a lock (malloc, dladdr) runs while
 * one is suspended. */
static void sample(int others) {
    thread_act_array_t threads;
    mach_msg_type_number_t count;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) return;
    mach_port_t self = mach_thread_self();
    for (mach_msg_type_number_t i = 0; i < count; i++) {
        mach_port_t th = threads[i];
        int main = th == g_thread;
        if (th != self && !main && !others) th = MACH_PORT_NULL;
        else if (th != self && !main) {
            thread_basic_info_data_t info;
            mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
            if (thread_info(th, THREAD_BASIC_INFO, (thread_info_t)&info, &n) != KERN_SUCCESS ||
                info.run_state != TH_STATE_RUNNING || (info.flags & TH_FLAGS_IDLE)) th = MACH_PORT_NULL;
        }
        if (th != self && th != MACH_PORT_NULL) {
            Stack s = {0};
            uintptr_t lo = 0, hi = 0;
            pthread_t pt = pthread_from_mach_thread_np(th);
            if (pt) {
                hi = (uintptr_t)pthread_get_stackaddr_np(pt);
                lo = hi - pthread_get_stacksize_np(pt);
                if (!main && (pthread_getname_np(pt, s.thread, sizeof s.thread) || !s.thread[0]))
                    snprintf(s.thread, sizeof s.thread, "thread %x", th);
            } else if (!main) {
                snprintf(s.thread, sizeof s.thread, "thread %x", th);
            }
            if (thread_suspend(th) == KERN_SUCCESS) {
                walk(th, lo, hi, &s);
                thread_resume(th);
                if (s.n) keep(&s);
                if (main) g_samples++;
            }
        }
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    mach_port_deallocate(mach_task_self(), self);
    vm_deallocate(mach_task_self(), (vm_address_t)threads, sizeof(thread_t) * count);
}

static void count_function(uintptr_t fn) {
    uint32_t h = (uint32_t)(fn >> 2) * 2654435761u, i = h % HITCH_FUNCS;
    for (int k = 0; k < 64; k++, i = (i + 1) % HITCH_FUNCS) {
        if (g_funcs[i].fn == fn) { g_funcs[i].hits++; return; }
        if (!g_funcs[i].fn) { g_funcs[i].fn = fn; g_funcs[i].hits = 1; return; }
    }
}

static void print_stack(Stack *s) {
    char line[1024];
    int len = snprintf(line, sizeof line, "  %d x%s%s%s", s->hits, s->thread[0] ? " [" : "", s->thread, s->thread[0] ? "]" : "");
    for (int k = 0; k < s->n && k < 12 && len < (int)sizeof line - 80; k++) {
        uintptr_t start;
        const char *name = sym(s->pc[k], &start);
        len += snprintf(line + len, sizeof line - len, " %s%.48s", k ? "< " : "", name);
    }
    LOG("%s", line);
    s->hits = -s->hits;  /* shown */
}

/* The most common stacks not shown yet: of the frame thread (other = 0) or of the others (other = 1). */
static void print_top(int other, int max) {
    for (int shown = 0; shown < max; shown++) {
        int best = -1;
        for (int i = 0; i < g_nstacks; i++)
            if (g_stacks[i].hits > 0 && !g_stacks[i].thread[0] == !other &&
                (best < 0 || g_stacks[i].hits > g_stacks[best].hits)) best = i;
        if (best < 0) break;
        print_stack(&g_stacks[best]);
    }
}

static void report_frame(int frame, double ms) {
    g_hitches++;
    LOG("hitch: frame %d took %.0f ms, %d stack samples", frame, ms, g_samples);
    print_top(0, 3);
    print_top(1, 4);
    /* functions anywhere on the stacks, once per sample, for the summary */
    for (int i = 0; i < g_nstacks; i++) {
        Stack *s = &g_stacks[i];
        if (s->thread[0]) continue;  /* the summary is of the frame thread */
        int hits = s->hits < 0 ? -s->hits : s->hits;
        for (int k = 0; k < s->n; k++) {
            uintptr_t start, seen = 0;
            sym(s->pc[k], &start);
            for (int j = 0; j < k && !seen; j++) { uintptr_t st2; sym(s->pc[j], &st2); seen = st2 == start; }
            if (!seen) for (int h = 0; h < hits; h++) count_function(start);
        }
        g_func_samples += hits;
    }
}

static void report_summary(void) {
    if (!g_func_samples) return;
    LOG("hitch summary: %u long frames, %u stack samples; functions on the most frame-thread samples, leaving out those on 90%% or more:", g_hitches, g_func_samples);
    for (int shown = 0; shown < 15; shown++) {
        int best = -1;
        for (int i = 0; i < HITCH_FUNCS; i++)
            if (g_funcs[i].fn && g_funcs[i].hits && (best < 0 || g_funcs[i].hits > g_funcs[best].hits)) best = i;
        if (best < 0) break;
        uint32_t hits = g_funcs[best].hits;
        g_funcs[best].hits = 0;
        if (hits * 10 >= g_func_samples * 9) { shown--; continue; }  /* on (nearly) every sample: the main loop */
        Dl_info info;
        const char *name = dladdr((void *)g_funcs[best].fn, &info) && info.dli_sname ? info.dli_sname : "?";
        LOG("  %5.1f%%  %s", 100.0 * hits / g_func_samples, name);
    }
    memset(g_funcs, 0, sizeof g_funcs);
    g_func_samples = g_hitches = 0;
}

static void *hitch_thread(void *arg) {
    (void)arg;
    pthread_setname_np("hitch-sampler");
    int frame = -1;
    uint64_t start = 0, last_summary = now_ns();
    for (;;) {
        usleep(2000);
        int f = atomic_load(&g_frame_no);
        uint64_t s = atomic_load(&g_frame_start_ns), t = now_ns();
        if (f != frame) {  /* a new frame began: report the one that ended if it was long */
            if (frame >= 0 && g_samples) report_frame(frame, (double)(s - start) / 1e6);
            frame = f; start = s; g_nstacks = g_samples = 0;
        }
        if (start && (double)(t - start) / 1e6 > g_threshold_ms && g_samples < 500) sample(g_samples % 5 == 0);  /* others every 10 ms */
        if (t - last_summary > 30000000000ull) { report_summary(); last_summary = t; }
    }
    return NULL;
}

/* Called by the thread that presents, once per frame (platform_sdl.c pace_frame). */
void hitch_frame(void) {
    static int init = -1;
    if (init < 0) {
        const char *v = getenv("LB_HITCH_MS");
        init = v && atof(v) > 0;
        if (!init) return;
        g_threshold_ms = atof(v);
        g_thread = pthread_mach_thread_np(pthread_self());
        pthread_t th;
        pthread_create(&th, NULL, hitch_thread, NULL);
        pthread_detach(th);
        LOG("hitch: sampling frames longer than %.0f ms", g_threshold_ms);
    }
    if (!init) return;
    atomic_store(&g_frame_start_ns, now_ns());
    atomic_fetch_add(&g_frame_no, 1);
}
#else
void hitch_frame(void) {}
#endif
