#!/usr/bin/env python3
"""Generate x86 corpus programs (tests/aot/X86SPEC.md) that lean on the stack.

usage: x86gen.py OUTDIR [COUNT] [SEED]

Each program is random but reproducible from its number, free of undefined behaviour by construction
(unsigned arithmetic, masked shifts and indexes, constant loop bounds, bounded recursion), and built to make
a compiler use its stack in as many ways as possible: many locals of every width, 64-bit values, more
arguments than fit in registers, locals whose address is passed down, local arrays, structs by value, calls
through tables of function pointers, recursion, variadic callees, and callees that overwrite their own
arguments. They are the test load for the lifter's stack-slot promotion; x86corpus.py runs them.
"""
import random
import sys
from pathlib import Path

T32, T16, T8, T64 = "uint32_t", "uint16_t", "uint8_t", "uint64_t"
CONV = ["", "", "", "__attribute__((stdcall)) ", "__attribute__((fastcall)) ", "__attribute__((regparm(3))) ",
        "__attribute__((regparm(1))) "]


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.out = []
        self.funcs = []     # (name, [param types], cost, conv, kind)
        self.tables = {}    # signature -> table name

    def pick(self, xs):
        return self.r.choice(xs)

    def const(self):
        r = self.r
        return self.pick(["%du" % r.randrange(16), "0x%xu" % r.getrandbits(32), "%du" % r.randrange(1, 255),
                          "0x%xu" % (1 << r.randrange(32)), "0xffffffffu", "0x80000000u", "1u", "0u"])

    def expr(self, vs, depth=0, wide=False):
        """A uint32_t-valued expression over the (name, type) variables in vs."""
        r = self.r
        if depth > 2 or r.random() < 0.25:
            sv = [v for v in vs if v[1] in (T32, T16, T8, T64)]
            if sv and r.random() < 0.8:
                n, t = self.pick(sv)
                return "(uint32_t)%s" % n if t != T32 else n
            return self.const()
        a, b = self.expr(vs, depth + 1), self.expr(vs, depth + 1)
        k = r.randrange(16)
        if k < 5:
            return "(%s %s %s)" % (a, self.pick(["+", "-", "*", "^", "&", "|"]), b)
        if k == 5:
            return "(%s << (%s & 31))" % (a, b)
        if k == 6:
            return "(%s >> (%s & 31))" % (a, b)
        if k == 7:
            return "((uint32_t)((int32_t)%s >> (%s & 31)))" % (a, b)
        if k == 8:
            return "(%s / ((%s & 15) + 1))" % (a, b)
        if k == 9:
            return "(%s %% ((%s & 255) + 3))" % (a, b)
        if k == 10:
            return "(%s %s %s ? %s : %s)" % (a, self.pick(["<", ">", "==", "!=", "<=", ">="]), b,
                                             self.expr(vs, depth + 1), self.expr(vs, depth + 1))
        if k == 11:
            return "((int32_t)%s %s (int32_t)%s ? %s : %s)" % (a, self.pick(["<", ">"]), b, a, b)
        if k == 12:
            return "(uint32_t)(((uint64_t)%s * %s) >> %d)" % (a, b, r.randrange(1, 40))
        if k == 13:
            return "(uint32_t)(%s)%s" % (self.pick([T8, T16, "int8_t", "int16_t"]), a)
        if k == 14:
            g, n = self.pick(self.garrs)
            return "%s[%s & %d]" % (g, a, n - 1)
        return "(~%s + (%s | 1u))" % (a, b)

    def call(self, vs, avail, cost_left):
        """A call expression to an earlier function (or '' if none fits), and its cost."""
        r = self.r
        fits = [f for f in avail if f[2] <= cost_left]
        if not fits:
            return "", 0
        name, params, cost, conv, kind = self.pick(fits)
        args = []
        for t in params or []:
            if t == "ptr":
                locs = [n for n, ty in vs if ty == T32 and n.startswith("v")]
                args.append("&" + self.pick(locs) if locs else "&gscratch")
            elif t == "arr":
                arrs = [n for n, ty in vs if ty == "arr8"]
                args.append(self.pick(arrs) if arrs else "garr8")
            elif t == "pair":
                args.append("(struct pair){%s, %s}" % (self.expr(vs, 1), self.expr(vs, 1)))
            elif t == T64:
                args.append("((uint64_t)%s << 32 | %s)" % (self.expr(vs, 1), self.expr(vs, 1)))
            else:
                args.append("(%s)%s" % (t, self.expr(vs, 1)))
        if kind == "var":
            n = r.randrange(0, 6)
            args = ["%du" % n] + [self.expr(vs, 1) for _ in range(n)]
        if kind == "table":
            sig = tuple(params)
            tab = self.tables[sig]
            return "%s[%s & %d](%s)" % (tab[0], self.expr(vs, 1), len(tab[1]) - 1, ", ".join(args)), tab[2]
        return "%s(%s)" % (name, ", ".join(args)), cost

    def stmts(self, vs, avail, budget, depth, ret):
        """Statements over vs; returns (lines, cost). vs entries of type uint32_t named v* are assignable."""
        r = self.r
        out, cost = [], 1
        for _ in range(r.randrange(2, 6 if depth else 9)):
            lv = [n for n, t in vs if n.startswith("v") and t in (T32, T16, T8, T64)]
            k = r.randrange(14)
            if k < 4 and lv:
                n = self.pick(lv)
                out.append("%s %s (%s)%s;" % (n, self.pick(["=", "+=", "^=", "-=", "|=", "&=", "*="]),
                                               dict(vs)[n], self.expr(vs)))
            elif k == 4 and depth < 2:
                n = r.randrange(1, 9)
                i = "i%d" % depth
                body, c = self.stmts(vs + [(i, T32)], avail, (budget - cost) // n, depth + 1, ret)
                out += ["for (uint32_t %s = 0; %s < %d; %s++) {" % (i, i, n, i)] + ["    " + l for l in body] + ["}"]
                cost += n * c
            elif k == 5 and depth < 3:
                a, c1 = self.stmts(vs, avail, budget - cost, depth + 1, ret)
                b, c2 = self.stmts(vs, avail, budget - cost - c1, depth + 1, ret)
                out += ["if (%s & %d) {" % (self.expr(vs), 1 << r.randrange(6))] + ["    " + l for l in a] + \
                       ["} else {"] + ["    " + l for l in b] + ["}"]
                cost += c1 + c2
            elif k == 6 and depth < 3 and lv:
                n = self.pick(lv)
                out.append("switch (%s & 7) {" % self.expr(vs))
                for case in r.sample(range(8), r.randrange(2, 6)):
                    out.append("case %d: %s += (%s)%s; %s" % (case, n, dict(vs)[n], self.expr(vs, 1),
                                                              self.pick(["break;", "break;", "/* falls through */"])))
                out.append("default: %s ^= 1; break;" % n)
                out.append("}")
            elif k in (7, 8, 9) and lv:
                e, c = self.call(vs, avail, budget - cost)
                if e:
                    n = self.pick(lv)
                    out.append("%s %s (%s)%s;" % (n, self.pick(["=", "+=", "^="]), dict(vs)[n], e))
                    cost += c
            elif k == 10:
                g, n = self.pick(self.garrs)
                out.append("%s[%s & %d] %s %s;" % (g, self.expr(vs, 1), n - 1, self.pick(["=", "+=", "^="]), self.expr(vs)))
            elif k == 11:
                arrs = [n for n, t in vs if t == "arr8"]
                if arrs:
                    a = self.pick(arrs)
                    out.append("%s[%s & 7] %s %s;" % (a, self.expr(vs, 1), self.pick(["=", "+=", "^="]), self.expr(vs)))
                    if lv:
                        out.append("%s += (%s)%s[%s & 7];" % (self.pick(lv), dict(vs)[lv[0]], a, self.expr(vs, 1)))
            elif k == 12 and depth and ret and r.random() < 0.4:
                out.append("if ((%s & 0xff) == %d) return %s;" % (self.expr(vs), r.randrange(256), self.expr(vs)))
            elif k == 13 and lv:
                ps = [n for n, t in vs if t == "ptr"]
                if ps:
                    p = self.pick(ps)
                    out.append("*%s %s %s;" % (p, self.pick(["=", "+=", "^="]), self.expr(vs)))
                    out.append("%s ^= (%s)*%s;" % (lv[0], dict(vs)[lv[0]], p))
            cost += 1
        return out, cost

    def function(self, k, avail):
        r = self.r
        name = "f%d" % k
        kind = self.pick(["plain"] * 6 + ["rec", "var", "argw"])
        conv = self.pick(CONV) if kind != "var" else ""
        nparams = r.randrange(0, 9)
        params = [self.pick([T32] * 5 + [T16, T8, T64, "ptr", "arr", "pair"]) for _ in range(nparams)]
        if kind == "rec":
            params = [T32] + params[:4]
        if "regparm" in conv or "fastcall" in conv:
            params = [p for p in params if p != T64 or r.random() < 0.5]
        vs, decl = [], []
        for n, t in enumerate(params):
            if t == "ptr":
                decl.append("uint32_t *p%d" % n)
                vs.append(("p%d" % n, "ptr"))
            elif t == "arr":
                decl.append("uint32_t *a%d" % n)
                vs.append(("a%d" % n, "arr8"))
            elif t == "pair":
                decl.append("struct pair s%d" % n)
                vs += [("s%d.key" % n, T32), ("s%d.weight" % n, T32)]
            else:
                decl.append("%s %s%d" % (t, "v" if kind == "argw" else "q", n))
                vs.append(("%s%d" % ("v" if kind == "argw" else "q", n), t))
        body = []
        if kind == "var":
            decl = ["uint32_t n", "..."]
            vs = [("n", T32)]
            body += ["__builtin_va_list ap; __builtin_va_start(ap, n);", "uint32_t vsum = n;",
                     "for (uint32_t i9 = 0; i9 < n; i9++) vsum = vsum * 31u + __builtin_va_arg(ap, uint32_t);",
                     "__builtin_va_end(ap);"]
            vs.append(("vsum", T32))
            params = None
        for n in range(r.randrange(1, 10)):
            t = self.pick([T32] * 5 + [T16, T8, T64, T64])
            body.append("%s v%d = (%s)%s;" % (t, 100 + n, t, self.expr(vs, 1)))
            vs.append(("v%d" % (100 + n), t))
        if r.random() < 0.45:
            body.append("uint32_t la[8];")
            body.append("for (uint32_t i8 = 0; i8 < 8; i8++) la[i8] = %s + i8;" % self.expr(vs, 1))
            vs.append(("la", "arr8"))
        budget = 1500
        if kind == "rec":
            self_call = "%s((q0 & 15) - 1%s)" % (name, "".join(
                ", " + ("&gscratch" if t == "ptr" else "garr8" if t == "arr" else "(struct pair){q0, 3u}" if t == "pair"
                        else "(%s)%s" % (t, self.expr(vs, 1))) for t in params[1:]))
            body.append("if ((q0 & 15) == 0) return %s;" % self.expr(vs, 1))
        st, cost = self.stmts(vs, avail, budget, 0, True)
        body += st
        if kind == "rec":
            body.append("v100 += (%s)%s;" % (dict(vs)["v100"], self_call))
            cost = cost * 16 + 16
        fold = " ^ ".join("(uint32_t)(%s >> %d)" % (n, 32 if t == T64 else 0) if t == T64 else "(uint32_t)%s" % n
                          for n, t in vs if t in (T32, T16, T8, T64))
        for n, t in vs:
            if t == "arr8":
                fold += " ^ %s[%d]" % (n, r.randrange(8))
        body.append("return %s;" % (fold or "0u"))
        self.out.append("__attribute__((noinline)) %sstatic uint32_t %s(%s) {" % (conv, name, ", ".join(decl) or "void"))
        self.out += ["    " + l for l in body]
        self.out.append("}")
        f = (name, params, cost, conv, kind)
        self.funcs.append(f)
        return f

    def program(self):
        r = self.r
        o = self.out
        o.append("/* Generated by x86gen.py: a stack-heavy mix of calls, locals and pointers (models nothing). */")
        o.append("struct pair { uint32_t key; uint32_t weight; };")
        self.garrs = [("ga", 16), ("gb", 8), ("garr8", 8)]
        o.append("static uint32_t ga[16], gb[8], garr8[8], gscratch;")
        o.append("static uint8_t gbytes[32]; static uint16_t gwords[8]; static uint64_t gwide[4];")
        avail = []
        for k in range(r.randrange(4, 10)):
            f = self.function(k, avail)
            if f[1] is not None or f[4] == "var":
                avail.append(f)
            # now and then, gather the plain functions of one signature into a table for indirect calls
            if f[4] == "plain" and f[1] is not None and not f[3] and r.random() < 0.5:
                same = [g for g in self.funcs if g[1] == f[1] and g[4] == "plain" and not g[3]]
                same = (same * 4)[:4] if len(same) < 4 else same[:4]
                if len(same) in (2, 4):
                    tab = "tab%d" % len(self.funcs)
                    sig = ", ".join("uint32_t *" if t in ("ptr", "arr") else "struct pair" if t == "pair" else t
                                    for t in f[1]) or "void"
                    o.append("static uint32_t (*const %s[%d])(%s) = {%s};" % (tab, len(same), sig,
                                                                             ", ".join(g[0] for g in same)))
                    self.tables[tuple(f[1])] = (tab, same, max(g[2] for g in same))
                    avail.append((tab, f[1], max(g[2] for g in same), "", "table"))
        vs = [("a", T32), ("b", T32)]
        body = ["uint32_t v1 = a ^ 0x9e3779b9u, v2 = b + 0x7f4a7c15u, v3 = a * 31u + b;", "uint64_t v4 = (uint64_t)a << 17 | b;",
                "uint8_t v5 = (uint8_t)(a >> 3); uint16_t v6 = (uint16_t)(b >> 5);"]
        vs += [("v1", T32), ("v2", T32), ("v3", T32), ("v4", T64), ("v5", T8), ("v6", T16)]
        la = "la" if r.random() < 0.4 else "ga"     # a local array makes the frame's address escape; not always
        if la == "la":
            body += ["uint32_t la[8];", "for (uint32_t i8 = 0; i8 < 8; i8++) la[i8] = a + i8 * b;"]
            vs.append(("la", "arr8"))
        for _ in range(3):
            st, _ = self.stmts(vs, avail, 8000, 0, False)
            body += st
        for f in avail:
            e, _ = self.call(vs, [f], 10 ** 9)
            body.append("v1 = v1 * 33u + %s;" % e)
        body += ["for (uint32_t i7 = 0; i7 < 8; i7++) { gbytes[(v1 + i7) & 31] += (uint8_t)%s[i7];" % la + " gwords[i7] ^= (uint16_t)(v2 >> i7); }",
                 "gwide[v3 & 3] += v4; gscratch ^= v5 + v6;",
                 "return v1 ^ v2 ^ v3 ^ (uint32_t)v4 ^ (uint32_t)(v4 >> 32) ^ v5 ^ v6 ^ %s[v1 & 7] ^ gscratch;" % la]
        o.append("uint32_t test_main(uint32_t a, uint32_t b) {")
        o += ["    " + l for l in body]
        o.append("}")
        return "\n".join(o) + "\n"


def main():
    out = Path(sys.argv[1])
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 100
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    out.mkdir(parents=True, exist_ok=True)
    for n in range(count):
        (out / ("gen%04d.c" % (seed * 10000 + n))).write_text(Gen(seed * 10000 + n).program())


if __name__ == "__main__":
    main()
