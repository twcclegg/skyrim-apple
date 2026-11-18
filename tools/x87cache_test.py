"""Whole-function test of the lifter's x87 register cache (X87CACHE in lift.py).

Usage: x87cache_test.py TESV_unpacked.exe functions.json workdir [functions=400] [runs=6]

difftest.py checks single instructions, each in one block. The cache's risks are elsewhere: values carried
between blocks, and state handed to callees and helpers. So this lifts real multi-block game functions that use
x87 twice, with the cache off (the reference) and on, builds one native harness from each, runs both on the same
random CPU and memory states, and compares everything a caller could observe: the whole CPU struct (all eight
x87 registers, the top index, the condition codes), the scratch memory the registers point into, and what every
callee saw. Callees are replaced by a stub that hashes the CPU state it is handed and then acts like a function
that sometimes returns a float (pushes one) and clobbers eax, so a missing write-back or reload shows up.
"""
import hashlib
import os
import random
import re
import subprocess
import sys

EXE, FUNCS, WORK = sys.argv[1:4]
NFUNCS = int(sys.argv[4]) if len(sys.argv) > 4 else 400
RUNS = int(sys.argv[5]) if len(sys.argv) > 5 else 6
sys.argv = [sys.argv[0], EXE, FUNCS, WORK]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lift  # noqa: E402

CALL = re.compile(r"\bf_([0-9A-F]+)\(c\)")

HARNESS = r'''
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include "x86.h"
#include "lift_prelude.h"
uint8_t *MEM;
static sigjmp_buf g_jb; static const char *g_why; static uint64_t g_seen;
static uint64_t mix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p; while (n--) { h ^= *b++; h *= 0x100000001B3ull; } return h;
}
/* every callee: record the CPU state it was handed, return like a function that may return a float */
static int g_trace;
static void dump(const char *what, CPU *c) {
    printf("\n  %s: top %u c %u%u%u%u r", what, c->top, c->c0, c->c1, c->c2, c->c3);
    for (int k = 0; k < 8; k++) printf(" %08x", c->r[k]);
    printf(" st");
    for (int k = 0; k < 8; k++) printf(" %.17g", c->st[k]);
}
#define SCR 0x10000000u
#define SCR_SIZE 0x20000u
#ifdef NATIVE_TEST
/* native_test.py only. A second callee hash, of the guest memory a callee can see: in an exact test the whole
 * scratch area; in a calling-convention test (g_abi_esp = the tested function's stack pointer at entry) the
 * scratch area outside the stack below that pointer (the tested function's own frames, which a replacement need
 * not write), plus the callee's return address and its first g_abi_args argument dwords. Also what a
 * calling-convention test clears before the comparison (ecx, edx, the XMM registers), as the call left it. */
#define NT_STACK 0x10018000u  /* calling-convention tests keep their buffers out of [NT_STACK, entry esp) */
static uint64_t g_seen2, g_xmm;
static uint32_t g_abi_esp, g_abi_args, g_ecx, g_edx;
static int nt_stubbed;  /* 1: a replacement's direct calls go to the stub, as the guest's do in an exact test */
static uint64_t mixw(uint64_t h, uint32_t a, uint32_t end) {
    for (; end - a >= 8; a += 8) { uint64_t v; memcpy(&v, MEM + a, 8); h = (h ^ v) * 0x100000001B3ull; h ^= h >> 29; }
    for (; a != end; a += 4) { uint32_t v; memcpy(&v, MEM + a, 4); h = (h ^ v) * 0x100000001B3ull; h ^= h >> 29; }
    return h;
}
static void nt_callee_memory(CPU *c) {
    uint64_t h = (g_seen2 ^ g_seen) * 0x100000001B3ull;
    if (!g_abi_esp) h = mixw(h, SCR, SCR + SCR_SIZE);
    else {
        h = mixw(mixw(h, SCR, NT_STACK), g_abi_esp, SCR + SCR_SIZE);
        for (uint32_t k = 0; k <= g_abi_args; k++) {
            uint32_t a = c->r[4] + 4 * k;
            if (a <= 0xFFFFFFFCu) h = mixw(h, a, a + 4);
        }
    }
    g_seen2 = h;
}
#define NT_KEEP(c) (g_ecx = (c)->r[1], g_edx = (c)->r[2], g_xmm = mix(0xCBF29CE484222325ull, (c)->xmm, sizeof (c)->xmm))
#endif
static void stub_call(CPU *c) {
    if (g_trace) dump("callee sees", c);
    g_seen = mix(g_seen, c, offsetof(CPU, thread));
#ifdef NATIVE_TEST
    nt_callee_memory(c);
#endif
    c->r[4] += 4;                                   /* ret */
    c->r[0] = (uint32_t)(g_seen >> 7);              /* eax, and edx/ecx are the callee's to clobber */
    c->r[2] = (uint32_t)(g_seen >> 23);
    if (g_seen & 1) { c->top = (c->top - 1) & 7; c->st[c->top] = (double)(int32_t)(g_seen >> 11) / 64.0; }
    if (g_seen & 2) c->c0 ^= 1;
}
void rt_call(CPU *c, uint32_t t) { g_seen = mix(g_seen, &t, 4); stub_call(c); }
void rt_call_ic(CPU *c, uint32_t t, const FnEntry **cache) { (void)cache; rt_call(c, t); }
/* The import cache's lookup (lift.py's indirect call sites): no entry, so every call site falls back to rt_call. */
const FnEntry *rt_ic_lookup(uint32_t t, const FnEntry **cache) { (void)t; (void)cache; return 0; }
void rt_unimpl(CPU *c, uint32_t a, const char *w) { (void)c; (void)a; g_why = w; siglongjmp(g_jb, 1); }
void rt_int3(CPU *c, uint32_t a) { (void)c; (void)a; g_why = "int3"; siglongjmp(g_jb, 1); }
void rt_cpuid(CPU *c, uint32_t *a, uint32_t *b, uint32_t *cc, uint32_t *d) { (void)c; *a = *b = *cc = *d = 0; }
uint64_t rt_rdtsc(void) { return 0; }
jmp_buf *rt_setjmp_slot(CPU *c, uint32_t g) { static jmp_buf jb; (void)c; (void)g; return &jb; }
void rt_setjmp3(CPU *c) { stub_call(c); }
static void on_signal(int sig) { g_why = sig == SIGALRM ? "timeout" : "fault"; siglongjmp(g_jb, 1); }
#include "body.c"
static uint64_t g_rng;
static uint32_t rnd(void) { g_rng = g_rng * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(g_rng >> 33); }
static double rnd_double(void) {
    switch (rnd() % 5) {
    case 0: return (double)(int32_t)rnd() / 65536.0;
    case 1: return (double)((int)(rnd() % 200) - 100);
    case 2: return (double)(rnd() % 1000) / 1000.0;
    case 3: return (rnd() & 1) ? 0.0 : -1.0;
    default: return (double)(float)((double)(int32_t)rnd() / 1024.0);
    }
}
int main(int argc, char **argv) {
    int runs = atoi(argv[2]);
    uint32_t only = argc > 4 ? (uint32_t)strtoul(argv[3], NULL, 16) : 0;  /* trace one function's one run */
    int only_run = argc > 4 ? atoi(argv[4]) : -1;
    uint32_t pick = argc == 4 ? (uint32_t)strtoul(argv[3], NULL, 16) : 0;  /* run one function only */
    MEM = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    FILE *f = fopen(argv[1], "rb");                 /* the module's bytes, where the lifter read them */
    uint32_t base; fread(&base, 4, 1, f); size_t n = fread(MEM + base, 1, 64u << 20, f); (void)n; fclose(f);
    signal(SIGBUS, on_signal); signal(SIGSEGV, on_signal); signal(SIGALRM, on_signal); signal(SIGFPE, on_signal);
    for (unsigned t = 0; t < sizeof g_tests / sizeof *g_tests; t++) {
        /* one process per function: guest memory outside the scratch area (the module's own data) starts the
         * same for every function, whatever earlier ones wrote or wherever a timeout stopped them */
        if (pick && g_addrs[t] != pick) continue;
        fflush(stdout);
        pid_t child = fork();
        if (child) { int status; waitpid(child, &status, 0); continue; }
        for (int r = 0; r < runs; r++) {
            if (only && (g_addrs[t] != only || r != only_run)) continue;
            g_trace = only != 0;
            g_rng = 0x9E3779B97F4A7C15ull * (t + 1) + (uint64_t)r * 7919; g_seen = 0xCBF29CE484222325ull; g_why = NULL;
#ifdef NATIVE_TEST
            g_seen2 = g_seen; g_abi_esp = g_abi_args = g_ecx = g_edx = 0; g_xmm = 0; nt_stubbed = 1;
#endif
            for (uint32_t o = 0; o < SCR_SIZE; o += 4) {  /* floats and pointers back into the scratch area */
                uint32_t v = rnd(), k = v % 4;
                if (k == 0) { float x = (float)rnd_double(); memcpy(&v, &x, 4); if (rnd() % 24 == 0) v = 0x7FC00000u; }
                else if (k == 1) v = SCR + 0x4000 + (rnd() % 0x18000 & ~3u);
                else if (k == 2) v = rnd() % 64;
                memcpy(MEM + SCR + o, &v, 4);
            }
            CPU c; memset(&c, 0, sizeof c);
            for (int k = 0; k < 8; k++) c.r[k] = SCR + 0x4000 + (rnd() % 0x18000 & ~3u);
            if (rnd() & 1) c.r[0] = rnd() % 256;
            c.fs_base = SCR + 0x1000; c.fpu_cw = 0x27F; c.mxcsr = 0x1F80;
            c.top = rnd() & 7;
            for (int k = 0; k < 8; k++) c.st[k] = rnd_double();
            c.c0 = rnd() & 1; c.c1 = rnd() & 1; c.c2 = rnd() & 1; c.c3 = rnd() & 1;
            for (int k = 0; k < 8; k++) for (int j = 0; j < 4; j++) c.xmm[k].f[j] = (float)rnd_double();
            printf("%08x %d ", g_addrs[t], r);
            struct itimerval limit = {{0, 0}, {0, 200000}}, off = {{0, 0}, {0, 0}};
            /* after a trap the module's data is in an unknown state: this function's later runs don't count */
            if (sigsetjmp(g_jb, 1)) { setitimer(ITIMER_REAL, &off, NULL); printf("TRAP %s\n", g_why); break; }
            setitimer(ITIMER_REAL, &limit, NULL);
            g_tests[t](&c);
            setitimer(ITIMER_REAL, &off, NULL);
            if (g_trace) { dump("returns", &c); printf("\n"); }
            printf("cpu %016llx mem %016llx callees %016llx",
                   (unsigned long long)mix(0xCBF29CE484222325ull, &c, offsetof(CPU, thread)),
                   (unsigned long long)mix(0xCBF29CE484222325ull, MEM + SCR, SCR_SIZE), (unsigned long long)g_seen);
#ifdef NATIVE_TEST
            printf(" | ecx %08x edx %08x xmm %016llx callees %016llx", g_ecx, g_edx, (unsigned long long)g_xmm,
                   (unsigned long long)g_seen2);
#endif
            printf("\n");
        }
        fflush(stdout);
        _exit(0);
    }
    return 0;
}
'''


def lift_one(fa, blocks, all_funcs, cache, stub="stub_call(c)"):
    """The lifted function as a test, static void t_<addr>; its direct calls become stub (a re.sub replacement)."""
    lift.X87CACHE = cache
    lines = lift.Lifter(fa, blocks, all_funcs).emit()
    head, body = lines[0], lines[1:]
    head = re.sub(r"^void f_[0-9A-F]+(_guest)?\(", "static void t_%X(" % fa, head)
    return [head] + [CALL.sub(stub, l) for l in body]


def build(name, picked, variants, rt, cc=("cc",), flags=(), head="", tail=""):
    """Builds WORK/<name>/harness from the tests in variants; head and tail go before and after them in body.c."""
    work = os.path.join(WORK, name)
    os.makedirs(work, exist_ok=True)
    text = head + "".join("\n".join(variants[fa]) + "\n\n" for fa in picked) + tail
    text += "typedef void (*TF)(CPU *);\nstatic TF g_tests[] = {%s};\n" % ", ".join("t_%X" % fa for fa in picked)
    text += "static uint32_t g_addrs[] = {%s};\n" % ", ".join("0x%Xu" % fa for fa in picked)
    open(os.path.join(work, "body.c"), "w").write(text)
    open(os.path.join(work, "harness.c"), "w").write(HARNESS)
    exe = os.path.join(work, "harness")
    subprocess.check_call([*cc, "-O2", "-w", "-fno-strict-aliasing", "-fwrapv", "-fsigned-char", *flags, "-I", rt,
                           "-I", work, "-o", exe, os.path.join(work, "harness.c"), os.path.join(rt, "fpu.c"), "-lm"])
    return exe


def main():
    os.makedirs(WORK, exist_ok=True)
    funcs = {int(k, 16): [tuple(b) for b in v] for k, v in lift.data["functions"].items()}
    all_funcs = set(funcs)
    rng = random.Random(87)
    order = sorted(funcs)
    rng.shuffle(order)
    ref, new, with_calls = {}, {}, 0
    for fa in order:
        if len(ref) >= NFUNCS:
            break
        blocks = funcs[fa]
        if len(blocks) < 2 or len(blocks) > 60:
            continue
        try:
            cached = lift_one(fa, blocks, all_funcs, True)
        except lift.Unsupported:
            continue
        if lift.X87_LOCALS not in cached:
            continue
        calls = any("stub_call(c)" in l or "rt_call" in l for l in cached)
        if not calls and len(ref) - with_calls >= NFUNCS // 2:
            continue  # keep half of the sample for functions that call out
        with_calls += calls
        ref[fa] = lift_one(fa, blocks, all_funcs, False)
        new[fa] = cached
    picked = sorted(ref)
    print("functions", len(picked), "with calls", with_calls)
    rt = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "runtime")
    image = os.path.join(WORK, "image.bin")
    with open(image, "wb") as f:
        f.write(lift.BASE.to_bytes(4, "little") + bytes(lift.img))
    out = {}
    for name, variants in (("plain", ref), ("cached", new)):
        exe = build(name, picked, variants, rt)
        res = subprocess.run([exe, image, str(RUNS)], capture_output=True, text=True)
        if res.returncode:
            sys.exit("%s harness exit %d: %s" % (name, res.returncode, res.stderr[-400:]))
        out[name] = res.stdout.splitlines()
        open(os.path.join(WORK, name + ".txt"), "w").write(res.stdout)
    res = {name: {l[:11]: l for l in lines if len(l) > 11} for name, lines in out.items()}
    bad, traps, timeouts, compared = [], 0, 0, 0
    for key in sorted(set(res["plain"]) | set(res["cached"])):
        a, b = res["plain"].get(key, key + "(no result)"), res["cached"].get(key, key + "(no result)")
        if "timeout" in a or "timeout" in b:
            timeouts += 1  # where a 200 ms limit lands isn't repeatable
            continue
        traps += "TRAP" in a
        compared += 1
        if a != b:
            bad.append((a, b))
    print("runs compared", compared, "faulted in the reference", traps, "timed out", timeouts, "different", len(bad))
    print("results sha256", hashlib.sha256("\n".join(out["plain"]).encode()).hexdigest()[:16])
    for a, b in bad[:20]:
        print("  plain  ", a)
        print("  cached ", b)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
