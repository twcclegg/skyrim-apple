#!/usr/bin/env python3
"""Random differential test of the compiler under LB_PINNED_REGS (arm64).

Generates small C programs that read and write eight global register variables in x21-x28 the way lifted
code does (straight-line code, branches, short-circuit conditions with writes inside, loops, calls followed
by the register clobber marker), builds each at -O0 and at the optimised configurations, runs them and
compares the printed final state. -O0 is the reference: it has shown none of the bugs.

usage: fuzz.py [-n PROGRAMS] [-s SEED] [--cc "xcrun clang"] [--raw] [--keep DIR]
  --raw   leave out the three pass-disabling flags, to show the test finds the known compiler bugs
Exit status 1 if any optimised build disagrees with -O0 (never with --raw).
"""
import argparse
import os
import random
import shlex
import subprocess
import sys
import tempfile

REGS = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
FIXED = ["-ffixed-x%d" % n for n in range(21, 29)]
SAFE = ["-mllvm", "-disable-postra-machine-licm", "-mllvm", "-disable-early-ifcvt",
        "-mllvm", "-aarch64-enable-ccmp=false"]
CONFIGS = [["-O1"], ["-O2"], ["-O3"], ["-Os"], ["-O2", "-mllvm", "-aarch64-stress-ccmp"]]
MEM = 16

PRELUDE = """#include <stdio.h>
#include <stdint.h>
register uint32_t eax __asm__("w21"); register uint32_t ecx __asm__("w22");
register uint32_t edx __asm__("w23"); register uint32_t ebx __asm__("w24");
register uint32_t esp __asm__("w25"); register uint32_t ebp __asm__("w26");
register uint32_t esi __asm__("w27"); register uint32_t edi __asm__("w28");
#define CLOB() __asm__ volatile("" : : : "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28")
static uint32_t mem[%d];
""" % MEM


class Gen:
    def __init__(self, rng):
        self.r = rng

    def expr(self, vs, d=0):
        r = self.r
        k = r.random()
        if d > 2 or k < 0.35:
            return r.choice(vs + REGS + REGS)
        if k < 0.45:
            return "%uu" % r.choice([0, 1, 2, 3, 5, 7, 9, 255, 0x80000000, 0xFFFFFFFF, r.getrandbits(32)])
        if k < 0.55:
            return "mem[%d]" % r.randrange(MEM)
        if k < 0.62:
            return "mem[(%s) & %d]" % (self.expr(vs, d + 1), MEM - 1)
        a, b = self.expr(vs, d + 1), self.expr(vs, d + 1)
        if k < 0.70:
            return "(%s >> ((%s) & 31))" % (a, b)
        if k < 0.76:
            return "(%s << ((%s) & 31))" % (a, b)
        return "(%s %s %s)" % (a, r.choice(["+", "-", "^", "&", "|", "*"]), b)

    def cond(self, vs, d=0):
        r = self.r
        k = r.random()
        if d < 2 and k < 0.3:
            return "(%s %s %s)" % (self.cond(vs, d + 1), r.choice(["&&", "||"]), self.cond(vs, d + 1))
        if k < 0.45:  # a write inside the condition, as lifted flag code produces
            return "(%s = %s, %s)" % (r.choice(REGS), self.expr(vs, 1), self.cond(vs, d + 2))
        return "(%s %s %s)" % (self.expr(vs, 1), r.choice(["==", "!=", "<", ">=", ">", "<="]),
                               r.choice([self.expr(vs, 2), "%uu" % r.choice([0, 1, 5, 9])]))

    def stmts(self, vs, fi, depth, n, in_loop=False):
        r, out = self.r, []
        for _ in range(n):
            k = r.random()
            ind = "    " * (depth + 1)
            if k < 0.34:
                out.append("%s%s = %s;" % (ind, r.choice(REGS), self.expr(vs)))
            elif k < 0.42:
                out.append("%s%s %s= %s;" % (ind, r.choice(REGS), r.choice(["+", "^", "-"]), self.expr(vs)))
            elif k < 0.50:
                out.append("%smem[%d] = %s;" % (ind, r.randrange(MEM), self.expr(vs)))
            elif k < 0.56:
                out.append("%s%s = %s;" % (ind, r.choice(vs[:2]), self.expr(vs)))
            elif k < 0.76 and depth < 3:
                out.append("%sif %s {" % (ind, self.cond(vs)))
                out += self.stmts(vs, fi, depth + 1, r.randint(1, 3), in_loop)
                if r.random() < 0.4:
                    out.append("%s} else {" % ind)
                    out += self.stmts(vs, fi, depth + 1, r.randint(1, 3), in_loop)
                out.append("%s}" % ind)
            elif k < 0.88 and depth < 2:
                i = "i%d" % depth
                bound = r.choice(["%d" % r.randint(1, 9), "(%s & 7)" % self.expr(vs, 2), "(a & 15)"])
                out.append("%sfor (uint32_t %s = 0; %s < %s; %s++) {" % (ind, i, i, bound, i))
                out += self.stmts(vs + [i], fi, depth + 1, r.randint(1, 4), True)
                out.append("%s}" % ind)
            elif k < 0.96 and fi > 0:
                callee = r.randrange(fi)
                call = "f%d(%s, %s)" % (callee, self.expr(vs, 1), self.expr(vs, 1))
                if r.random() < 0.5:
                    out.append("%s%s = %s; CLOB();" % (ind, r.choice(vs[:2]), call))
                else:
                    out.append("%s%s; CLOB();" % (ind, call))
            elif in_loop and r.random() < 0.5:
                out.append("%sif %s %s;" % (ind, self.cond(vs), r.choice(["break", "continue"])))
            else:
                out.append("%sreturn %s;" % (ind, self.expr(vs)))
        return out

    def program(self, nfuncs):
        r, src = self.r, [PRELUDE]
        for fi in range(nfuncs):
            attr = r.choice(["__attribute__((noinline)) ", "__attribute__((noinline)) ", "static inline ", "static "])
            src.append("%suint32_t f%d(uint32_t a, uint32_t b) {" % (attr, fi))
            src.append("    uint32_t t = a ^ b, u = 0;")
            src += self.stmts(["t", "u", "a", "b"], fi, 0, r.randint(3, 8))
            src.append("    return t + u;\n}")
        src.append("int main(void) {")
        src.append("    uint32_t save[8] = {eax, ecx, edx, ebx, esp, ebp, esi, edi};")
        for _ in range(6):
            src.append("    " + " ".join("%s = %uu;" % (g, r.choice([0, 1, 5, 9, r.getrandbits(32)])) for g in REGS))
            src.append("    for (int i = 0; i < %d; i++) mem[i] = (uint32_t)i * %uu + %uu;"
                       % (MEM, r.getrandbits(32) | 1, r.getrandbits(8)))
            src.append("    CLOB(); { uint32_t v = f%d(%uu, %uu); CLOB();" % (nfuncs - 1, r.choice([0, 5, r.getrandbits(32)]),
                                                                         r.choice([0, 9, r.getrandbits(32)])))
            src.append("    uint32_t g[8] = {eax, ecx, edx, ebx, esp, ebp, esi, edi}; uint32_t h = v;")
            src.append("    for (int i = 0; i < %d; i++) h = h * 31 + mem[i];" % MEM)
            src.append('    printf("%08x %08x %08x %08x %08x %08x %08x %08x %08x\\n", '
                       "g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], h); }")
        src.append("    eax = save[0]; ecx = save[1]; edx = save[2]; ebx = save[3];")
        src.append("    esp = save[4]; ebp = save[5]; esi = save[6]; edi = save[7]; CLOB();")
        src.append("    return 0;\n}")
        return "\n".join(src) + "\n"


def build_run(cc, flags, src, exe):
    p = subprocess.run(cc + FIXED + ["-w"] + flags + [src, "-o", exe], capture_output=True, text=True)
    if p.returncode:
        return "compile error: " + p.stderr[:300]
    try:
        q = subprocess.run([exe], capture_output=True, text=True, timeout=20)
    except subprocess.TimeoutExpired:
        return "timeout"
    return "exit %d\n%s" % (q.returncode, q.stdout)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", type=int, default=200)
    ap.add_argument("-s", type=int, default=1)
    ap.add_argument("--cc", default="cc")
    ap.add_argument("--raw", action="store_true")
    ap.add_argument("--keep")
    a = ap.parse_args()
    if os.uname().machine not in ("arm64", "aarch64"):
        print("skipped: not arm64")
        return 0
    cc = shlex.split(a.cc)
    work = tempfile.mkdtemp()
    extra = [] if a.raw else SAFE
    bad = 0
    per_cfg = {" ".join(c): 0 for c in CONFIGS}
    for i in range(a.n):
        seed = a.s * 1000003 + i
        src = os.path.join(work, "p.c")
        text = Gen(random.Random(seed)).program(random.Random(seed).randint(2, 5))
        with open(src, "w") as f:
            f.write(text)
        ref = build_run(cc, ["-O0"], src, os.path.join(work, "ref"))
        if not ref.startswith("exit 0"):
            print("program %d: reference build unusable (%s)" % (seed, ref.split("\n")[0]))
            continue
        failed = []
        for c in CONFIGS:
            if build_run(cc, c + extra, src, os.path.join(work, "opt")) != ref:
                failed.append(" ".join(c))
                per_cfg[" ".join(c)] += 1
        if failed:
            bad += 1
            if a.keep:
                os.makedirs(a.keep, exist_ok=True)
                with open(os.path.join(a.keep, "p%d.c" % seed), "w") as f:
                    f.write(text)
            if bad <= 10:
                print("program %d differs from -O0 under: %s" % (seed, "; ".join(failed)))
    print("%d programs, %d differ from -O0%s" % (a.n, bad, " (pass-disabling flags left out)" if a.raw else ""))
    for c, n in per_cfg.items():
        print("  %-40s %d" % (c, n))
    return 0 if a.raw or not bad else 1


if __name__ == "__main__":
    sys.exit(main())
