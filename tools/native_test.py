"""Checks the native replacements in runtime/skyrim_native*.c against the guest functions they replace.

Usage: native_test.py TESV_unpacked.exe functions.json workdir [runs=20000] [options]

For every address in lift.HOST_OVERRIDES_SKYRIM["exe"] (plus tools/native_overrides/*.txt), lifts the guest body
(plain x87 model) and runs it and the native function on the same random CPU and memory states, using
x87cache_test.py's harness. One process per function and build, --jobs of them at a time. Exit status 0 when
nothing differs, 1 when something does, 2 when the test could not be run as asked. workdir/report.json holds the
counts per pass, function and field.

Two kinds of test:

  exact   The whole CPU struct, the scratch memory and what callees saw must match on every run. Direct calls,
          the guest's and the native function's f_X(c) alike, go to a stub that hashes the target, the CPU state
          it is handed and the whole scratch memory at that moment.
  ABI     For functions with an entry in tools/native_overrides/*_abi.py (see ABI below): the guest runs with its
          whole lifted call tree, a native function's f_X(c) calls run the real thing (the native X if there is
          one, else X's lifted call tree), and what a caller may not rely on is cleared before the comparison.

The target must be arm64. The natives are written for an arm64 host (its default NaN, clang's fp contract
pragma), and LB_STRICT_ORDER only changes code there; built for x86-64 the test reports differences that are not
bugs. Away from an arm64 machine, cross-compile and run under an emulator, for example:
  --cc "clang --target=aarch64-linux-gnu --sysroot=SYSROOT -static" --run qemu-aarch64-static

Options:
  --order default|strict|both   Memory-ordering build to test (default: both). "strict" builds guest and native
                    with -DLB_STRICT_ORDER (as the macOS, iPad, simulator and visionOS builds do), so the natives'
                    load-acquire / store-release paths are the ones compared. "both" runs the two passes one after the other;
                    when the target is not arm64 it says so and leaves the strict pass out.
  --only A[,A...]   Run only these functions (everything is still built).
  --jobs N          Processes at a time (default: one per CPU).
  --cc CMD          Compiler command (default: $NATIVE_TEST_CC, else cc).
  --run CMD         Command put in front of the test binaries, e.g. an emulator (default: $NATIVE_TEST_RUN).
  --abi FILE        A further file of ABI entries (repeatable), e.g. for --identity functions.

Options for testing the test (tools/native_test_faults.py uses them; runtime/ is never written to):
  --mutate 'OLD=>NEW'   Replace OLD, which must occur exactly once, in the test's copy of the natives' source.
                    A change of behaviour should then be reported as a difference.
  --identity A[,A...]   Take the lifted body of a function that has no native replacement as its "native" one,
                    direct calls and all: an exact test unless it has an ABI entry. Without --mutate it must pass.

Strict ABI comparison. An ABI test clears ecx, edx and the XMM registers before comparing, and the stub's callee
hash covers the CPU struct only. Per entry (third element, see ABI), any of these can be compared as well:
  ecx, edx, xmm   the register(s) as the call left them
  callees[=N]     the guest memory every stubbed callee could see when it was called: the scratch area outside
                  the tested function's own stack (from its stack pointer at entry down to 0x10018000; setups
                  keep their buffers out of there), the callee's return address and its first N argument dwords
                  (default 0; a native function need not write anything else of its own frame)
All four are computed for every ABI function and listed after each pass, flagged or not. Exact tests always
compare everything, callee memory included.
"""
import argparse
import concurrent.futures
import json
import os
import re
import shlex
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def options():
    p = argparse.ArgumentParser(usage=__doc__.split("\n\n")[1].split(": ", 1)[1] + "  (see the file's docstring)")
    p.add_argument("exe")
    p.add_argument("functions")
    p.add_argument("workdir")
    p.add_argument("runs", nargs="?", default="20000")
    p.add_argument("--order", choices=("default", "strict", "both"), default="both")
    p.add_argument("--only", default="")
    p.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    p.add_argument("--cc", default=os.environ.get("NATIVE_TEST_CC") or "cc")
    p.add_argument("--run", default=os.environ.get("NATIVE_TEST_RUN") or "")
    p.add_argument("--abi", action="append", default=[])
    p.add_argument("--mutate", action="append", default=[])
    p.add_argument("--identity", default="")
    return p.parse_args()


OPT = options()
sys.argv = [sys.argv[0], OPT.exe, OPT.functions, OPT.workdir]
sys.path.insert(0, HERE)
import x87cache_test as t  # noqa: E402  (also imports lift with these arguments)

lift = t.lift
HEAD = re.compile(r"^void f_[0-9A-F]+(_guest)?\(")
FIELDS = ("ecx", "edx", "xmm", "callees")
STRICT = " ".join(FIELDS)  # for entries that leave all of it as the guest does

# Replacements of C runtime functions, and others that inline or call guest code, are checked at the calling
# convention instead of instruction for instruction: the guest body runs with its whole lifted call tree, each run
# gets arguments that make sense for the function, and afterwards everything a caller may not rely on is cleared
# in both before the comparison: eax when the result is in ST(0), ecx, edx, the XMM registers, the x87 condition
# codes and registers other than the result, and the callee's stack below the return address.
# address: (argument setup in C, extra clearing in C[, strict fields]); the optional third element is a string
# of the strict ABI fields to compare as well (see the docstring), e.g. "ecx edx xmm callees=1", or STRICT.
ABI = {}
# Per-group ABI entries: tools/native_overrides/<group>_abi.py defines ABI = {address: (setup, extra[, strict])}.
_lists = os.path.join(HERE, "native_overrides")
for _path in ([os.path.join(_lists, n) for n in sorted(os.listdir(_lists)) if n.endswith("_abi.py")]
              if os.path.isdir(_lists) else []) + OPT.abi:
    _scope = {"STRICT": STRICT}
    exec(open(_path).read(), _scope)
    ABI.update(_scope.get("ABI", {}))


def abi_entry(fa):
    """(setup, extra clearing, set of strict fields, callee argument dwords) of an ABI entry."""
    setup, extra, *rest = ABI[fa]
    strict, args = set(), 0
    for word in (rest[0] if rest else "").split():
        name, _, n = word.partition("=")
        if name not in FIELDS or (n and name != "callees"):
            sys.exit("ABI entry %X: unknown strict field %r" % (fa, word))
        strict.add(name)
        args = int(n) if n else args
    return setup, extra, strict, args


_lifted = {}


def lifted(g, funcs):
    if g not in _lifted:
        lift.X87CACHE = False
        _lifted[g] = lift.Lifter(g, funcs[g], set(funcs)).emit()
    return _lifted[g]


def call_tree(root, funcs):
    """The addresses of a function's whole lifted call tree."""
    seen, todo = set(), [root]
    while todo:
        g = todo.pop()
        if g in seen or g not in funcs:
            continue
        seen.add(g)
        todo += [int(m, 16) for m in t.CALL.findall("\n".join(lifted(g, funcs)))]
    return seen


# The lifter's own exits out of lifted code: rt_call with a literal address (a jump or call to an address that has
# no lifted function) or with the jump-table default's jt_. A lifted function's ordinary calls are f_<addr>(c), and
# calls through memory (imports, vtables) rt_call(c, M32(...)); neither matches.
EXIT = re.compile(r"\brt_call\(c, (jt_|0x[0-9A-Fa-f]+u)\)")


def tree_source(addrs, funcs):
    """The lifted functions addrs (whole call trees) as static lt_<addr>; calls out of them go to the stub, and the
    lifter's exits out of lifted code to nt_exit, which marks the run as having left (see compare)."""
    out = ["static uint32_t rnd(void); static double rnd_double(void);"]
    out += ["static void lt_%X(CPU *c);" % g for g in sorted(addrs)]
    for g in sorted(addrs):
        lines = lifted(g, funcs)
        out.append(HEAD.sub("static void lt_%X(" % g, lines[0]))
        body = EXIT.sub(r"nt_exit(c, \1)", "\n".join(lines[1:]))
        out.append(t.CALL.sub(lambda m: "lt_%s(c)" % m[1] if int(m[1], 16) in addrs else "rt_call(c, 0x%su)" % m[1],
                              body))
    return "\n".join(out) + "\n\n"


def abi_wrapper(fa, callee):
    setup, extra, _, args = abi_entry(fa)
    clear = "c->r[1] = c->r[2] = 0; memset(c->xmm, 0, sizeof c->xmm); memset(MEM + esp - 0x100, 0, 0x100); " + extra
    # the stack is kept clear of any buffers; NT_KEEP notes ecx, edx and the XMM registers before they are cleared
    return ("static void t_%X(CPU *c) { uint32_t esp = 0x1001C000u + (rnd() %% 0x800 & ~3u); c->r[4] = esp; "
            "g_abi_esp = esp; g_abi_args = %d; nt_stubbed = 0; " % (fa, args)
            + setup + " %s(c); NT_KEEP(c); " % callee + clear + " }")


def native_source(rt, identity, funcs):
    """The natives' source for the test, f_X(c) calls rewritten to nt_call_X(c), and the set of those X."""
    src = {f: open(os.path.join(rt, f)).read()
           for f in sorted(os.listdir(rt)) if re.fullmatch(r"skyrim_native(_\w+)?\.c", f)}
    for fa in identity:  # the lifted body under the native function's name
        lines = lifted(fa, funcs)
        src["identity_%X.c" % fa] = "\n".join([HEAD.sub("void f_%X(" % fa, lines[0])] + lines[1:]) + "\n"
    for change in OPT.mutate:
        old, sep, new = change.partition("=>")
        hits = [name for name, text in src.items() for _ in range(text.count(old))]
        if not sep or not old or len(hits) != 1:
            sys.exit("--mutate %r: OLD=>NEW, with OLD exactly once in the natives (found %d times)" % (change, len(hits)))
        src[hits[0]] = src[hits[0]].replace(old, new)
    targets, text = set(), ""
    for name, body in src.items():
        targets |= {int(m, 16) for m in t.CALL.findall(body)}
        text += '#line 1 "%s"\n' % os.path.join(rt, name) + t.CALL.sub(lambda m: "nt_call_%s(c)" % m[1], body) + "\n"
    return text, targets


def variants(picked, identity, funcs, rt):
    """Per build (guest, native): the tests, and the text before and after them."""
    all_funcs = set(funcs)
    natives = set(picked)
    trees = set().union(*[call_tree(fa, funcs) for fa in picked if fa in ABI])
    source, targets = native_source(rt, identity, funcs)
    guest, native = {}, {}
    for fa in picked:
        if fa in ABI:
            guest[fa] = [abi_wrapper(fa, "lt_%X" % fa)]
            # a replacement may hand some inputs to f_<addr>_guest
            native[fa] = ([] if fa in identity else ["void f_%X_guest(CPU *c) { lt_%X(c); }" % (fa, fa)]) \
                + [abi_wrapper(fa, "f_%X" % fa)]
            continue
        guest[fa] = t.lift_one(fa, funcs[fa], all_funcs, False, stub=lambda m: "rt_call(c, 0x%su)" % m[1])
        native[fa] = ["static void t_%X(CPU *c) { f_%X(c); }" % (fa, fa)]
        if fa not in identity:  # the lifted body as f_<addr>_guest; its calls go where a native function's do
            body = t.lift_one(fa, funcs[fa], all_funcs, False, stub=lambda m: "nt_call_%s(c)" % m[1])
            targets |= {int(m, 16) for m in t.CALL.findall("\n".join(lifted(fa, funcs)))}
            native[fa] = [re.sub(r"^static void t_%X\(" % fa, "void f_%X_guest(" % fa, body[0])] + body[1:] + native[fa]
    # A native function's (or f_<addr>_guest's) direct call of X: the stub in an exact test, as for the guest;
    # in an ABI test the native X, else X's lifted call tree (always part of the tested guest's own tree).
    decl = "".join("static void nt_call_%X(CPU *c);\n" % x for x in sorted(targets))
    glue = "".join(
        "static void nt_call_%X(CPU *c) { if (nt_stubbed) rt_call(c, 0x%Xu); else %s; }\n"
        % (x, x, "f_%X(c)" % x if x in natives else "lt_%X(c)" % x if x in trees else "rt_call(c, 0x%Xu)" % x)
        for x in sorted(targets))
    tree = tree_source(trees, funcs)
    return {"guest": (guest, tree, ""), "native": (native, "#define GAME_SKYRIM 1\n" + decl + source + tree, glue)}


def is_arm64(cc):
    res = subprocess.run(cc + ["-dM", "-E", "-x", "c", os.devnull], capture_output=True, text=True)
    if res.returncode:
        sys.exit("compiler %r failed: %s" % (" ".join(cc), res.stderr[-300:]))
    return "__aarch64__" in res.stdout


def run_all(exes, image, addrs):
    """{build: {address: output lines}}, one process per build and function."""
    prefix = shlex.split(OPT.run)

    def one(job):
        name, fa = job
        return job, subprocess.run(prefix + [exes[name], image, OPT.runs, "%X" % fa], capture_output=True, text=True)

    out = {name: {} for name in exes}
    with concurrent.futures.ThreadPoolExecutor(max(1, OPT.jobs)) as pool:
        for (name, fa), res in pool.map(one, [(name, fa) for name in exes for fa in addrs]):
            if res.returncode:
                print("%s harness, function %X: exit %d: %s" % (name, fa, res.returncode, res.stderr[-400:]))
                sys.exit(2)
            out[name][fa] = res.stdout.splitlines()
    return out


def compare(fa, guest, native):
    """Counts for one function: runs, traps, runs where the guest left its lifted code, and per differing field
    [runs, first run, enforced]; "state" is the ordinary comparison, "missing" a run without its counterpart."""
    strict = abi_entry(fa)[2] if fa in ABI else set(FIELDS)
    res = {"mode": "abi" if fa in ABI else "exact", "runs": len(guest), "traps": sum("TRAP" in a for a in guest),
           "left": 0, "diffs": {}, "examples": []}

    def note(field, run, enforced, a, b):
        d = res["diffs"].setdefault(field, [0, run, enforced])
        d[0] += 1
        if enforced and len(res["examples"]) < 2:
            res["examples"].append((run, a, b))

    if len(guest) != len(native) or not guest:
        note("missing", min(len(guest), len(native)), True, "%d lines" % len(guest), "%d lines" % len(native))
    empty = "callees cbf29ce484222325"
    for run, (a, b) in enumerate(zip(guest, native)):
        if a == b:
            continue
        (abase, _, aext), (bbase, _, bext) = a.partition(" | "), b.partition(" | ")
        if abase != bbase:
            # In an ABI test the native side makes no stubbed calls unless it hands the run to the guest (or
            # calls guest code as the guest does). A guest that left its lifted tree through one of the lifter's
            # exits (a jump-table target the lifter never found; "left 1") where the native one makes no stubbed
            # call: the game would stop there with "no recompiled code", so the replacement is the correct one.
            # Without "left 1" the native has skipped a call the guest makes, which is a difference.
            if fa in ABI and bbase.endswith(empty) and not abase.endswith(empty) and " left 1" in a:
                res["left"] += 1
            else:
                note("state", run, True, a, b)
            continue
        aext, bext = aext.split(), bext.split()
        for field, x, y in zip(aext[0::2], aext[1::2], bext[1::2]):
            if x != y and field != "left":  # where each side left lifted code is not itself a result
                note(field, run, field in strict, a, b)
    return res


def main():
    funcs = {int(k, 16): [tuple(b) for b in v] for k, v in lift.data["functions"].items()}
    identity = sorted(int(a, 16) for a in OPT.identity.split(",") if a)
    natives = sorted(lift.HOST_OVERRIDES_SKYRIM.get("exe", ()))
    for fa in identity:
        if fa in natives or fa not in funcs:
            sys.exit("--identity %X: %s" % (fa, "has a native replacement" if fa in natives else "not a function"))
    picked = sorted(natives + identity)
    addrs = sorted(int(a, 16) for a in OPT.only.split(",") if a) or picked
    if set(addrs) - set(picked):
        sys.exit("--only: not a native replacement: " + ", ".join("%X" % a for a in sorted(set(addrs) - set(picked))))
    cc = shlex.split(OPT.cc)
    arm64 = is_arm64(cc)
    if not arm64:
        print("note: the target is not arm64: the natives are written for an arm64 host and differences reported "
              "here need not be bugs (see --cc and --run)")
        if OPT.order == "strict":
            sys.exit("--order strict: LB_STRICT_ORDER only changes code on arm64")
    passes = [o for o in ("default", "strict") if OPT.order in (o, "both") and (o == "default" or arm64)]
    rt = os.path.join(HERE, "..", "runtime")
    builds = variants(picked, set(identity), funcs, rt)
    image = os.path.join(t.WORK, "image.bin")
    os.makedirs(t.WORK, exist_ok=True)
    with open(image, "wb") as f:
        f.write(lift.BASE.to_bytes(4, "little") + bytes(lift.img))
    report, failed = {}, False
    for order in passes:
        flags = ["-DNATIVE_TEST"] + (["-DLB_STRICT_ORDER"] if order == "strict" else [])
        exes = {name: t.build(name + ("-strict" if order == "strict" else ""), picked, tests, rt, cc=cc, flags=flags,
                              head=head, tail=tail) for name, (tests, head, tail) in builds.items()}
        out = run_all(exes, image, addrs)
        res = {fa: compare(fa, out["guest"][fa], out["native"][fa]) for fa in addrs}
        report[order] = {"%X" % fa: {k: v for k, v in r.items() if k != "examples"} for fa, r in res.items()}
        bad = {fa: sum(d[0] for d in r["diffs"].values() if d[2]) for fa, r in res.items()}
        failed |= any(bad.values())
        print("order %s: functions" % order, len(addrs), "runs", sum(r["runs"] for r in res.values()),
              "trapped", sum(r["traps"] for r in res.values()), "different", sum(bad.values()),
              "guest left its lifted code", sum(r["left"] for r in res.values()))
        for fa, r in res.items():
            if not bad[fa]:
                continue
            print("  %X (%s) differs:" % (fa, r["mode"]),
                  ", ".join("%s in %d runs (first: run %d)" % (k, d[0], d[1]) for k, d in r["diffs"].items() if d[2]))
            for run, a, b in r["examples"]:
                print("    guest ", a)
                print("    native", b)
            if r["examples"] and r["examples"][0][0] < r["runs"]:
                print("    trace: %s %s %s %X %d  (and the same with %s)" % (
                    " ".join(shlex.split(OPT.run) + [exes["guest"]]), image, OPT.runs, fa, r["examples"][0][0],
                    exes["native"]))
        if any(fa in ABI for fa in addrs):
            print("  strict ABI comparison (* = compared: the entry flags it):")
        for fa, r in res.items():
            if fa in ABI:
                print("    %06X  " % fa + "  ".join(
                    "%s %s%s" % (k, "differs in %d runs (first: run %d)" % tuple(r["diffs"][k][:2])
                                 if k in r["diffs"] else "same", "*" if k in abi_entry(fa)[2] else "") for k in FIELDS))
    if OPT.order == "both" and not arm64:
        print("order strict: NOT RUN (the target is not arm64)")
        report["not run"] = ["strict"]
    with open(os.path.join(t.WORK, "report.json"), "w") as f:
        json.dump(report, f, indent=1, sort_keys=True)
    sys.exit(1 if failed else 0)


main()
