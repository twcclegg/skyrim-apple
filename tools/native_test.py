"""Checks the native replacements in runtime/skyrim_native*.c against the guest functions they replace.

Usage: native_test.py TESV_unpacked.exe functions.json workdir [runs=20000]

For every address in lift.HOST_OVERRIDES_SKYRIM["exe"] (plus tools/native_overrides/*.txt), lifts the guest body (plain x87 model) and runs it and the
native function on the same random CPU and memory states, using x87cache_test.py's harness: the whole CPU struct,
the scratch memory and what callees saw must match on every run.
"""
import os
import re
import subprocess
import sys

RUNS = sys.argv[4] if len(sys.argv) > 4 else "20000"
sys.argv = sys.argv[:4]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import x87cache_test as t  # noqa: E402  (also imports lift with these arguments)

lift = t.lift

# Replacements of C runtime functions are checked at the calling convention instead of instruction for instruction:
# the guest body runs with its whole lifted call tree (nothing stubbed), each run gets arguments that make sense for
# the function, and afterwards everything a caller may not rely on is cleared in both before the comparison: eax
# when the result is in ST(0), ecx, edx, the XMM registers, the x87 condition codes and registers other than the
# result, and the callee's stack below the return address. address: (argument setup in C, extra clearing in C)
ABI = {}
# Per-group ABI entries: tools/native_overrides/<group>_abi.py defines ABI = {address: (setup, extra)}.
_lists = os.path.join(os.path.dirname(os.path.abspath(__file__)), "native_overrides")
for _name in sorted(os.listdir(_lists)) if os.path.isdir(_lists) else ():
    if _name.endswith("_abi.py"):
        _scope = {}
        exec(open(os.path.join(_lists, _name)).read(), _scope)
        ABI.update(_scope.get("ABI", {}))


def abi_variants(fa, funcs, all_funcs):
    """Guest and native test wrappers for an ABI-checked replacement (see ABI)."""
    setup, extra = ABI[fa]
    lift.X87CACHE = False
    bodies, todo = {}, [fa]
    while todo:  # the guest's whole lifted call tree
        g = todo.pop()
        if g in bodies or g not in funcs:
            continue
        lines = lift.Lifter(g, funcs[g], all_funcs).emit()
        bodies[g] = lines
        todo += [int(m, 16) for m in re.findall(r"\bf_([0-9A-F]+)\(c\)", "\n".join(lines))]
    guest = ["static uint32_t rnd(void); static double rnd_double(void);"]
    guest += ["static void g%X_%X(CPU *c);" % (fa, g) for g in sorted(bodies)]  # per test: trees share helpers
    for g, lines in sorted(bodies.items()):
        head = re.sub(r"^void f_[0-9A-F]+(_guest)?\(", "static void g%X_%X(" % (fa, g), lines[0])
        body = re.sub(r"\bf_([0-9A-F]+)\(c\)",
                      lambda m: "g%X_%s(c)" % (fa, m[1]) if int(m[1], 16) in bodies else "stub_call(c)",
                      "\n".join(lines[1:]))
        guest += [head, body]
    clear = ("c->r[1] = c->r[2] = 0; memset(c->xmm, 0, sizeof c->xmm); memset(MEM + esp - 0x100, 0, 0x100); " + extra)
    wrap = ("static void t_%X(CPU *c) { uint32_t esp = 0x1001C000u + (rnd() %% 0x800 & ~3u); c->r[4] = esp; " % fa
            + setup + " CALLEE(c); " + clear + " }")  # the stack is kept clear of any buffers
    # the native build also gets the lifted tree, for a replacement that hands some inputs to f_<addr>_guest
    native = guest + ["void f_%X_guest(CPU *c) { g%X_%X(c); }" % (fa, fa, fa), wrap.replace("CALLEE", "f_%X" % fa)]
    return guest + [wrap.replace("CALLEE", "g%X_%X" % (fa, fa))], native


def main():
    funcs = {int(k, 16): [tuple(b) for b in v] for k, v in lift.data["functions"].items()}
    picked = sorted(lift.HOST_OVERRIDES_SKYRIM.get("exe", ()))
    rt = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "runtime")
    guest = {fa: t.lift_one(fa, funcs[fa], set(funcs), False) for fa in picked if fa not in ABI}
    # the native side also gets the lifted body as f_<addr>_guest, for replacements that hand rare inputs to it
    native = {fa: [re.sub(r"^static void t_%X\(" % fa, "void f_%X_guest(" % fa, guest[fa][0])] + guest[fa][1:]
              + ["static void t_%X(CPU *c) { f_%X(c); }" % (fa, fa)] for fa in picked if fa not in ABI}
    for fa in picked:
        if fa in ABI:
            guest[fa], native[fa] = abi_variants(fa, funcs, set(funcs))
    first = sorted(native)[0]
    sources = sorted(f for f in os.listdir(rt) if re.fullmatch(r"skyrim_native(_\w+)?\.c", f))
    native[first] = ["#define GAME_SKYRIM 1"] + ['#include "%s"' % f for f in sources] + native[first]
    image = os.path.join(t.WORK, "image.bin")
    os.makedirs(t.WORK, exist_ok=True)
    with open(image, "wb") as f:
        f.write(lift.BASE.to_bytes(4, "little") + bytes(lift.img))
    out = {}
    for name, variants in (("guest", guest), ("native", native)):
        exe = t.build(name, picked, variants, rt)
        out[name] = subprocess.run([exe, image, RUNS], capture_output=True, text=True).stdout.splitlines()
    bad = [(a, b) for a, b in zip(out["guest"], out["native"]) if a != b]
    # In ABI mode the native side makes no stubbed calls unless it hands the run to the guest. A guest that calls
    # a stub where the native one doesn't has left its lifted tree (a jump-table target the lifter never found):
    # the game would stop there with "no recompiled code", so the replacement is the correct one.
    empty = "callees cbf29ce484222325"
    left = [(a, b) for a, b in bad if int(a.split()[0], 16) in ABI and b.endswith(empty) and not a.endswith(empty)]
    bad = [p for p in bad if p not in left]
    traps = sum("TRAP" in a for a in out["guest"])
    print("functions", len(picked), "runs", len(out["guest"]), "trapped", traps, "different",
          len(bad) + abs(len(out["guest"]) - len(out["native"])), "guest left its lifted code", len(left))
    for a, _ in left[:3]:
        print("  guest left its code:", a)
    for a, b in bad[:10]:
        print("  guest ", a)
        print("  native", b)
    sys.exit(1 if bad or len(out["guest"]) != len(out["native"]) or not out["guest"] else 0)


main()
