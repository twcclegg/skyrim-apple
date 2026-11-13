#!/usr/bin/env python3
"""Two-thread test for LIFT_PLAIN_ACYCLIC (docs/HEAP_ACCESSES.md).

usage: x86threads.py [--cc clang] [--work DIR]

Lifts tests/aot/x86threads/handoff.c and runs it as two host threads on shared guest memory: one spins
until the other sets a flag. Built three ways: the default lifter; LIFT_PLAIN_ACYCLIC=1, which must still
finish; and LIFT_PLAIN_ACYCLIC=all (plain accesses in every function, loops included), which should hang
in the spin that has no call in it: that run shows the test can see the bug it guards against. The
lifted code is built with LB_GUEST_VOLATILE so the default accesses are volatile on any host, as on arm64.
"""
import argparse
import json
import os
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import x86corpus as xc  # noqa: E402

HARNESS = r'''
#include "rt.h"
#include "rt_table.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include "layout.h"
uint8_t *MEM; int g_log_level;
extern const FnEntry g_fn_table_fixture[]; extern const unsigned g_fn_count_fixture;
const FnModule g_modules[] = {{"x86threads", g_fn_table_fixture, 0}}; const unsigned g_nmodules = 1;
unsigned rt_module_count(unsigned i) { return i == 0 ? g_fn_count_fixture : 0; }
void rt_fatal(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); _exit(86); }
int platform_frame_count(void) { return 0; } void prof_report(void) {} void cpu_report(void) {}
struct job { uint32_t stack, fs, args[2], r; volatile int done; };
static void *run(void *p) {
    struct job *j = p;
    CPU c = {0}; c.r[4] = j->stack; c.fs_base = j->fs; c.fpu_cw = 0x37F; c.mxcsr = 0x1F80;
    j->r = rt_invoke(&c, ENTRY, 2, j->args);
    j->done = 1;
    return 0;
}
int main(int argc, char **argv) {
    MEM = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    FILE *f = fopen(argv[1], "rb"); static uint8_t img[IMG_SIZE]; if (fread(img, 1, IMG_SIZE, f) != IMG_SIZE) return 2; fclose(f);
    memcpy(MEM + IMG_BASE, img, IMG_SIZE);
    struct job w = {0x180000, 0x25000, {(uint32_t)atoi(argv[2]), 0}}, s = {0x280000, 0x26000, {2, 0x1234}};
    pthread_t tw, ts;
    pthread_create(&tw, 0, run, &w);
    usleep(100000);                         /* the waiter is spinning by now */
    pthread_create(&ts, 0, run, &s);
    pthread_join(ts, 0);
    for (int k = 0; k < 300 && !w.done; k++) usleep(10000);
    if (!w.done) { printf("HANG\n"); fflush(stdout); _exit(3); }
    printf("DONE %08x\n", w.r);
    return 0;
}
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default="xcrun clang" if sys.platform == "darwin" else "clang")
    ap.add_argument("--work")
    a = ap.parse_args()
    cc = a.cc.split()
    work = Path(a.work or tempfile.mkdtemp(prefix="x86threads-"))
    work.mkdir(parents=True, exist_ok=True)
    src = HERE / "x86threads" / "handoff.c"
    obj = work / "handoff.o"
    p = xc.sh(cc + xc.X86_FLAGS + ["-O2", "-c", src, "-o", obj])
    if p.returncode:
        sys.exit("x86 build failed: " + p.stderr)
    text, data, infos, data_rva = xc.link([obj])
    pe, fmap = work / "prog.exe", work / "functions.json"
    xc.write_pe(pe, text, data, data_rva, infos[0]["entry"])
    env = dict(os.environ, LIFT_GAME="skyrim", LIFT_FILES="1", LIFT_TSO="1", LIFT_SLOTS="0")
    p = xc.sh([sys.executable, xc.ROOT / "tools/discover.py", pe, fmap], env=env, cwd=xc.ROOT)
    if p.returncode:
        sys.exit("discovery failed: " + p.stderr)
    img = bytes(xc.TEXT_RVA) + text + data
    (work / "image.bin").write_bytes(img)
    (work / "layout.h").write_text("#define IMG_BASE 0x%Xu\n#define IMG_SIZE %du\n#define ENTRY 0x%Xu\n"
                                   % (xc.BASE, len(img), infos[0]["entry"]))
    (work / "harness.c").write_text(HARNESS)
    expect = {"0": "DONE", "1": "DONE", "all": "HANG"}
    bad = 0
    for setting in ("0", "1", "all"):
        gen = work / ("generated-" + setting)
        gen.mkdir(exist_ok=True)
        p = xc.sh([sys.executable, xc.ROOT / "tools/lift.py", pe, fmap, gen, "fixture"],
                  env=dict(env, LIFT_PLAIN_ACYCLIC=setting), cwd=xc.ROOT)
        if p.returncode:
            sys.exit("lifter failed: " + p.stderr[-400:])
        if json.loads((gen / "fixture_rejected.json").read_text()):
            sys.exit("lifter rejected a function")
        exe = work / ("run-" + setting)
        common = xc.HOST_ARCH + ["-std=gnu11", "-O2", "-fno-strict-aliasing", "-fwrapv", "-fsigned-char", "-w",
                                 "-DGAME_SKYRIM", "-DLB_GUEST_VOLATILE", "-I", xc.ROOT / "runtime", "-I", gen, "-I", work]
        p = xc.sh(cc + common + [work / "harness.c", xc.ROOT / "runtime/dispatch.c", xc.ROOT / "runtime/fpu.c",
                                 xc.ROOT / "runtime/setjmp.c"] + sorted(gen.glob("*.c")) + xc.HOST_LIBS + ["-lpthread", "-o", exe])
        if p.returncode:
            sys.exit("build failed: " + p.stderr[:400])
        for waiter in ("0", "1"):
            r = xc.sh([exe, work / "image.bin", waiter], timeout=30)
            got = (r.stdout.split() or ["?"])[0]
            want = "DONE" if setting != "all" or waiter == "0" else expect[setting]
            ok = got == want
            bad += not ok
            print("%-4s LIFT_PLAIN_ACYCLIC=%-3s waiter %s: %s%s" % ("OK" if ok else "FAIL", setting, waiter,
                                                                  r.stdout.strip() or r.stderr.strip()[:120],
                                                                  "" if ok else " (expected %s)" % want))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
