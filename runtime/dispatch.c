#define _GNU_SOURCE
/* Indirect call/jump dispatch, host<->guest call bridging, and traps. */
#include "rt.h"
#include "rt_table.h"
#include "host_portability.h"
#include <string.h>
#include <time.h>
#include <unistd.h>

unsigned rt_module_count(unsigned m);

#define MAX_THUNKS 8192
static struct { ShimFn fn; const char *name; } g_thunks[MAX_THUNKS];
/* One immutable table entry per thunk, so call-site caches can hold imports too (rt_call_ic). */
static FnEntry g_thunk_entries[MAX_THUNKS];
static uint32_t g_nthunks;
static pthread_mutex_t g_thunk_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t rt_thunk(ShimFn fn, const char *name) {
    pthread_mutex_lock(&g_thunk_lock);
    if (g_nthunks >= MAX_THUNKS) rt_fatal("out of thunks");
    uint32_t i = g_nthunks;
    g_thunks[i].fn = fn;
    g_thunks[i].name = name;
    g_thunk_entries[i].addr = GUEST_THUNK_BASE + i * 4;
    g_thunk_entries[i].fn = fn;
    __atomic_store_n(&g_nthunks, i + 1, __ATOMIC_RELEASE);  /* entry visible before the count */
    pthread_mutex_unlock(&g_thunk_lock);
    return GUEST_THUNK_BASE + i * 4;
}

const char *rt_thunk_name(uint32_t a) {
    if (a >= GUEST_THUNK_BASE && (a - GUEST_THUNK_BASE) / 4 < __atomic_load_n(&g_nthunks, __ATOMIC_ACQUIRE)) return g_thunks[(a - GUEST_THUNK_BASE) / 4].name;
    return NULL;
}

/* Small direct-mapped cache in front of the sorted function table. A slot
 * holds one pointer to an immutable table entry, read and written atomically:
 * a separate address and function pointer could be seen half updated by
 * another thread (a call to the wrong function). */
#define CACHE_BITS 14
static const FnEntry *g_cache[1 << CACHE_BITS];

static const FnEntry *lookup_entry(uint32_t a) {
    unsigned h = (a ^ (a >> 13)) & ((1 << CACHE_BITS) - 1);
    const FnEntry *e = __atomic_load_n(&g_cache[h], __ATOMIC_ACQUIRE);
    if (e && e->addr == a) return e;
    for (unsigned m = 0; m < g_nmodules; m++) {
        const FnEntry *t = g_modules[m].table;
        unsigned n = rt_module_count(m);
        if (!n || a < t[0].addr || a > t[n - 1].addr) continue;
        unsigned lo = 0, hi = n;
        while (lo < hi) {
            unsigned mid = (lo + hi) / 2;
            if (t[mid].addr < a) lo = mid + 1;
            else hi = mid;
        }
        if (lo < n && t[lo].addr == a) {
            __atomic_store_n(&g_cache[h], &t[lo], __ATOMIC_RELEASE);
            return &t[lo];
        }
    }
    return NULL;
}
static GuestFn lookup(uint32_t a) {
    const FnEntry *e = lookup_entry(a);
    return e ? e->fn : NULL;
}

/* Watchdog bookkeeping: the last import each thread called, its guest return
 * address and a call counter, so a stall can be located from the log. */
#define WD_MAX 64
static struct { const char *name; uint32_t ret; volatile uint32_t calls; uint64_t tid; uint32_t gtid, esi, ebx, esp; pthread_t th; } g_wd[WD_MAX];
static int g_wd_n;
static __thread int t_wd = -1;

static void wd_note(CPU *c, const char *name) {
    if (t_wd < 0) {
        t_wd = __atomic_fetch_add(&g_wd_n, 1, __ATOMIC_RELAXED);
        if (t_wd >= WD_MAX) t_wd = WD_MAX - 1;
        g_wd[t_wd].tid = rt_host_thread_id();
        g_wd[t_wd].th = pthread_self();
    }
    g_wd[t_wd].name = name;
    g_wd[t_wd].ret = M32(c->r[4]);
    g_wd[t_wd].esi = c->r[6];
    g_wd[t_wd].ebx = c->r[3];
    g_wd[t_wd].esp = c->r[4];
    g_wd[t_wd].gtid = M32(c->fs_base + 0x24);
    g_wd[t_wd].calls++;
}

/* MSVC RTTI class name of a guest object, or "?" */
static const char *rtti_name(uint32_t obj) {
    if (obj < 0x10000 || obj >= 0xFFFF0000u) return "?";
    uint32_t vt = M32(obj);
    if (vt < 0x400000 || vt >= 0x2000000) return "?";
    uint32_t col = M32(vt - 4);
    if (col < 0x400000 || col >= 0x2000000) return "?";
    uint32_t td = M32(col + 12);
    if (td < 0x400000 || td >= 0x2000000) return "?";
    return (const char *)(MEM + td + 8);
}

#ifndef __ANDROID__
#include <execinfo.h>
#include <signal.h>
/* desktop: SIGUSR2 makes a thread print its host backtrace (guest call chain) */
static void wd_bt(int sig) {
    (void)sig;
    void *bt[48];
    int n = backtrace(bt, 48);
    char hdr[64];
    int l = snprintf(hdr, sizeof hdr, "--- backtrace tid %llu\n", (unsigned long long)rt_host_thread_id());
    write(2, hdr, l);
    backtrace_symbols_fd(bt, n, 2);
}
#endif

/* LB_IMPORT_COUNT=1: calls per import, reported with the watchdog every 30 s (busiest first), to pick imports
 * worth emitting inline in the lifter. Off by default: the counter is a shared atomic on every import call. */
static int g_import_count = -1;
static uint64_t g_thunk_calls[MAX_THUNKS], g_thunk_last[MAX_THUNKS];
static void import_report(int seconds) {
    uint32_t n = __atomic_load_n(&g_nthunks, __ATOMIC_ACQUIRE), top[16] = {0};
    uint64_t d[16] = {0}, total = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t k = __atomic_load_n(&g_thunk_calls[i], __ATOMIC_RELAXED), delta = k - g_thunk_last[i];
        g_thunk_last[i] = k;
        total += delta;
        for (int j = 0; j < 16; j++)
            if (delta > d[j]) {
                memmove(&d[j + 1], &d[j], sizeof d[0] * (15 - j));
                memmove(&top[j + 1], &top[j], sizeof top[0] * (15 - j));
                d[j] = delta; top[j] = i;
                break;
            }
    }
    LOG("imports: %llu calls in %d s (%.0f/s)", (unsigned long long)total, seconds, (double)total / seconds);
    for (int j = 0; j < 16 && d[j]; j++)
        LOG("  %10.0f/s %5.1f%%  %s", (double)d[j] / seconds, 100.0 * d[j] / (total ? total : 1), g_thunks[top[j]].name);
}

static void *wd_thread(void *arg) {
    (void)arg;
    uint32_t last[WD_MAX] = {0}, still[WD_MAX] = {0};
    extern int platform_frame_count(void);
    int lastf = -1;
    for (int tick = 1;; tick++) {
        sleep(5);
        int n = g_wd_n < WD_MAX ? g_wd_n : WD_MAX, f = platform_frame_count();
        int report = tick % 6 == 0 || f == lastf;  /* every 30 s, or every 5 s while no frame is presented */
        for (int i = 0; i < n; i++) {
            uint32_t k = g_wd[i].calls;
            still[i] = k == last[i] ? still[i] + 5 : 0;
            last[i] = k;
        }
        if (report && (f == lastf || tick % 6 == 0)) {
            LOG("watchdog %ds: frames %d%s", tick * 5, f, f == lastf ? " (no new frames)" : "");
            if (tick % 6 == 0) { extern void prof_report(void), cpu_report(void); prof_report(); cpu_report(); }
            if (tick % 6 == 0 && g_import_count > 0) import_report(30);
            for (int i = 0; i < n; i++)
            {
                uint32_t e = g_wd[i].esi, ok = e >= 0x10000 && e < 0xFFFFFFF0u;
                LOG("  thread %d (tid %llu, guest %u): last %s from %08x, %u calls%s; esi %08x [%08x %08x]",
                    i, (unsigned long long)g_wd[i].tid, g_wd[i].gtid, g_wd[i].name ? g_wd[i].name : "-", g_wd[i].ret, g_wd[i].calls,
                    still[i] >= 10 ? " (idle)" : "", e, ok ? M32(e) : 0, ok ? M32(e + 4) : 0);
#ifdef GAME_SKYRIM
                if (g_wd[i].ret == 0x5A1A30u) {  /* task wait loop: arg0 -> task, ebx = manager */
                    uint32_t fr = g_wd[i].esp + 8, pp = M32(fr + 0x18), t = pp >= 0x10000 ? M32(pp) : 0;
                    LOG("    waits on task %08x (%s) state %08x, arg1 %d, manager %08x (%s), flag %d", t, rtti_name(t),
                        t >= 0x10000 ? M32(t + 0x10) : 0, M8(fr + 0x1C), g_wd[i].ebx, rtti_name(g_wd[i].ebx), M8(0x1B41078));
                }
#endif
            }
        }
#ifndef __ANDROID__
        if (tick == 26) {
            LOG("io guard 0x1B97ED8 = %d", M8(0x1B97ED8));
            signal(SIGUSR2, wd_bt);
            for (int i = 0; i < n; i++)
                if (still[i] < 10) { pthread_kill(g_wd[i].th, SIGUSR2); usleep(200000); }
        }
#endif
        lastf = f;
    }
    return NULL;
}

void rt_watchdog_start(void) {
    const char *ic = getenv("LB_IMPORT_COUNT");
    g_import_count = ic && *ic == '1';
    pthread_t th;
    pthread_create(&th, NULL, wd_thread, NULL);
    pthread_detach(th);
}

void rt_call(CPU *c, uint32_t t) {
    if (t >= GUEST_THUNK_BASE) {
        uint32_t i = (t - GUEST_THUNK_BASE) / 4;
        if (i < __atomic_load_n(&g_nthunks, __ATOMIC_ACQUIRE)) {
            wd_note(c, g_thunks[i].name);
            if (g_import_count > 0) __atomic_fetch_add(&g_thunk_calls[i], 1, __ATOMIC_RELAXED);
            TRACE("-> %s", g_thunks[i].name);
            if (!g_thunks[i].fn)
                rt_fatal("unimplemented import %s (called from %08x)", g_thunks[i].name, M32(c->r[4]));
            g_thunks[i].fn(c);
            return;
        }
    }
    GuestFn fn = lookup(t);
    if (!fn) {
        uint32_t ret = M32(c->r[4]), esp = c->r[4];
        uint32_t a0 = M32(esp + 4);
        fprintf(stderr, "bad call: esp=%08x args %08x %08x %08x %08x\n", esp, a0, M32(esp + 8), M32(esp + 12), M32(esp + 16));
        if (a0 >= 0x10000) {
            uint32_t vt = M32(a0);
            fprintf(stderr, "  [arg0] = %08x %08x %08x %08x\n", vt, M32(a0 + 4), M32(a0 + 8), M32(a0 + 12));
            if (vt >= 0x10000) fprintf(stderr, "  vtbl: %08x %08x %08x ... [19]=%08x\n", M32(vt), M32(vt + 4), M32(vt + 8), M32(vt + 0x4c));
        }
        rt_fatal("no recompiled code at %08x (return address %08x)", t, ret);
    }
    fn(c);
}

/* Indirect call from recompiled code with a per-call-site cache: remember the
 * target's table entry (immutable, so one pointer is a consistent pair).
 * Imports are cached too (LB_IMPORT_CACHE=0 turns that off): later calls from
 * the site go straight to the shim, so the watchdog's last-import note and
 * call count only see a site's first call. Unknown targets take the normal
 * path every time. */
static int g_import_cache = -1;
void rt_call_ic(CPU *c, uint32_t t, const FnEntry **cache) {
    if (t >= GUEST_THUNK_BASE) {
        if (g_import_cache < 0) {
            const char *v = getenv("LB_IMPORT_CACHE");
            g_import_cache = !(v && *v == '0');
        }
        uint32_t i = (t - GUEST_THUNK_BASE) / 4;
        if (g_import_cache && g_import_count <= 0 && !(t & 3) &&
            i < __atomic_load_n(&g_nthunks, __ATOMIC_ACQUIRE) && g_thunk_entries[i].fn)
            __atomic_store_n(cache, &g_thunk_entries[i], __ATOMIC_RELAXED);
    } else {
        const FnEntry *e = lookup_entry(t);
        if (e) {
            __atomic_store_n(cache, e, __ATOMIC_RELAXED);
            e->fn(c);
            return;
        }
    }
    rt_call(c, t);
}

uint32_t rt_invoke(CPU *c, uint32_t fn, int nargs, const uint32_t *args) {
    uint32_t saved_esp = c->r[4];
    for (int i = nargs - 1; i >= 0; i--) {
        c->r[4] -= 4;
        M32(c->r[4]) = args[i];
    }
    c->r[4] -= 4;
    M32(c->r[4]) = 0xFEEDF00Du; /* fake return address */
    rt_call(c, fn);
    /* works for both stdcall (callee popped) and cdecl (we restore) */
    c->r[4] = saved_esp;
    return c->r[0];
}

void rt_unimpl(CPU *c, uint32_t addr, const char *what) {
    rt_fatal("unimplemented instruction '%s' at %08x", what, addr);
}

void rt_int3(CPU *c, uint32_t addr) {
    rt_fatal("int3 at %08x", addr);
}

void rt_cpuid(CPU *c, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    uint32_t leaf = *eax;
    if (leaf == 0) {
        *eax = 1;
        memcpy(ebx, "Genu", 4);
        memcpy(edx, "ineI", 4);
        memcpy(ecx, "ntel", 4);
    } else if (leaf == 1) {
        *eax = 0x00000F29; /* a Pentium 4 */
        *ebx = 0;
        *ecx = 0;
        *edx = (1u << 0) | (1u << 4) | (1u << 15) | (1u << 23) | (1u << 25) | (1u << 26); /* fpu tsc cmov mmx sse sse2 */
    } else {
        *eax = *ebx = *ecx = *edx = 0;
    }
}

uint64_t rt_rdtsc(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
