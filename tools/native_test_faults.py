"""Checks that tools/native_test.py catches what it is meant to catch.

Usage: native_test_faults.py TESV_unpacked.exe functions.json workdir [runs=2000]

Runs native_test.py (same compiler and launcher: $NATIVE_TEST_CC, $NATIVE_TEST_RUN) on deliberately broken copies
of the natives (--mutate; runtime/ is never written to) and on stand-in natives that call guest code (--identity),
and compares each run's report.json with what must be reported: which pass (default or strict ordering), which
function and which field. A fault that goes unnoticed, or is noticed where it should not be, fails. Needs an
arm64 target, as native_test.py's strict-ordering pass does. Exit status 0 when every case comes out as listed.
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXE, FUNCS, WORK = sys.argv[1:4]
RUNS = sys.argv[4] if len(sys.argv) > 4 else "2000"

# A test setup for 0xB19EB0, which has no native replacement: with --identity its lifted body stands in as one
# that calls a lifted function (0xB1B650) and a native one (0xB1B6C0). It walks a tree of 12-byte nodes (type,
# child A, child B; the dword after a node of type 7 or 8 is its record index) from the root index at +0xA0:
# type 7 tests the object's bound against a record's six planes with 0xB1B650, type 8 with 0xB1B6C0, the result
# picks the child; types 2 and 3 are leaves. Here: up to six inner nodes in the even slots, children always further
# on, eight records of random planes (often far enough out for the bound to be inside) and masks.
FIXTURE = '''ABI = {0xB19EB0: (
    "uint32_t self = 0x10004000u, nodes = 0x10005000u, recs = 0x10006000u + (rnd() % 0x100 & ~3u), "
    "obj = 0x10008000u + (rnd() % 0x200 & ~3u), n = 1 + rnd() % 6; "
    "for (uint32_t k = 0; k < n; k++) { uint32_t q = nodes + 24 * k, r = rnd() % 8; "
    "  M32(q) = r < 3 ? 7 : r < 6 ? 8 : rnd() % 12; M32(q + 12) = rnd() % 8; "
    "  for (int j = 1; j <= 2; j++) { uint32_t ch = k + 1 + rnd() % 3; M32(q + 4 * j) = ch >= n ? 2 * n + (rnd() & 1) : 2 * ch; } } "
    "M32(nodes + 24 * n) = 2; M32(nodes + 24 * n + 12) = 3; "
    "for (uint32_t k = 0; k < 8; k++) { uint32_t q = recs + 0x70 * k; "
    "  for (int j = 0; j < 6; j++) if (rnd() % 4) M32(q + 16 * j + 12) = 0xC47A0000u; "
    "  M32(q + 0x60) = rnd() % 3 ? rnd() & 0x3F : (rnd() & 1) ? 0 : rnd(); } "
    "if (rnd() % 8 == 0) M32(obj + 0x94) = 0; "
    "M32(self) = recs; M32(self + 0xC) = nodes; M32(self + 0xA0) = 0; "
    "M32(self + 0x9C) = rnd() % 8 ? 1 + rnd() % 5 : rnd() % 2 ? 0 : 0xFFFFFFFFu; "
    "c->r[1] = self; M32(esp + 4) = obj;",
    "", STRICT)}
'''

BOTH = ("default", "strict")
# name, what it shows, native_test.py options, {pass: {function: fields reported}} (passes not named: nothing)
CASES = [
    ("calls-clean",
     "stand-in natives that call guest code pass: 0xB1B650 exact (its call of 0x472580 goes to the stub on both "
     "sides), 0xB19EB0 at the calling convention (its calls run lifted 0xB1B650 and native 0xB1B6C0)",
     ["--identity", "B1B650,B19EB0", "--only", "B1B650,B19EB0"], {}),
    ("strict-order-load-after-store",
     "strict ordering: 0x4488B0's aligned path (only compiled with LB_STRICT_ORDER) re-reads in[2] after out[0] "
     "is stored",
     ["--only", "4488B0", "--mutate",
      "__atomic_store_n(q + 0, a0, __ATOMIC_RELEASE);=>__atomic_store_n(q + 0, a0, __ATOMIC_RELEASE); a2 = M32(in + 8);"],
     {"strict": {"4488B0": {"state"}}}),
    ("exact-callee-target",
     "exact test, native calling guest code: the call goes to another function",
     ["--identity", "B1B650", "--only", "B1B650", "--mutate",
      "M32(esp) = 0xB1B685u; SPILL(); f_472580(c);=>M32(esp) = 0xB1B685u; SPILL(); f_4725A0(c);"],
     {o: {"B1B650": {"state"}} for o in BOTH}),
    ("exact-callee-argument",
     "exact test, native calling guest code: the callee's argument is changed for the call and put back after "
     "it (CPU state at the call and final memory are the same)",
     ["--identity", "B1B650", "--only", "B1B650", "--mutate",
      "M32(esp) = 0xB1B685u; SPILL(); f_472580(c); RELOAD();=>"
      "M32(esp) = 0xB1B685u; M32(esp + 4) ^= 4; SPILL(); f_472580(c); RELOAD(); M32(esp) ^= 4;"],
     {o: {"B1B650": {"callees"}} for o in BOTH}),
    ("abi-lifted-callee-argument",
     "ABI test, native calling a lifted function: the record handed to lifted 0xB1B650 is moved by one plane",
     ["--identity", "B19EB0", "--only", "B19EB0", "--mutate",
      "M32(esp) = 0xB19F2Au; SPILL(); f_B1B650(c);=>M32(esp) = 0xB19F2Au; M32(esp + 8) += 0x10; SPILL(); f_B1B650(c);"],
     {o: {"B19EB0": {"state"}} for o in BOTH}),
    ("strict-abi-callee-memory",
     "strict ABI, callees: 0xACE100 puts the saved plane mask back before its virtual call instead of after "
     "(same registers at the call, same final state)",
     ["--only", "ACE100", "--mutate",
      "    uint32_t vt = g8_tl32(obj), t = g8_tl32(vt + 0xC4);=>"
      "    g8_ts32(self + 0x8C, mask);\n    uint32_t vt = g8_tl32(obj), t = g8_tl32(vt + 0xC4);"],
     {o: {"ACE100": {"callees"}} for o in BOTH}),
    ("strict-abi-edx",
     "strict ABI, edx: 0x4488B0 returns in[6] in edx instead of in[7]",
     ["--only", "4488B0", "--mutate", "c->r[2] = a7;=>c->r[2] = a6;"],
     {o: {"4488B0": {"edx"}} for o in BOTH}),
    ("strict-abi-xmm",
     "strict ABI, xmm: 0xF6029C leaves xmm7 as it was instead of the last block's last 16 bytes",
     ["--only", "F6029C", "--mutate",
      "memcpy(c->xmm, MEM + src + bytes - 128, 128);=>memcpy(c->xmm, MEM + src + bytes - 128, 112);"],
     {o: {"F6029C": {"xmm"}} for o in BOTH}),
]


def main():
    os.makedirs(WORK, exist_ok=True)
    fixture = os.path.join(WORK, "fixture_abi.py")
    with open(fixture, "w") as f:
        f.write(FIXTURE)
    failed = False
    for name, what, opts, expect in CASES:
        work = os.path.join(WORK, name)
        res = subprocess.run([sys.executable, os.path.join(HERE, "native_test.py"), EXE, FUNCS, work, RUNS,
                              "--order", "both", "--abi", fixture] + opts, capture_output=True, text=True)
        with open(os.path.join(WORK, name + ".txt"), "w") as f:
            f.write(res.stdout + res.stderr)
        try:
            report = json.load(open(os.path.join(work, "report.json"))) if res.returncode in (0, 1) else {}
        except OSError:
            report = {}
        got = {o: {fa: {k for k, d in r["diffs"].items() if d[2]} for fa, r in report.get(o, {}).items()} for o in BOTH}
        got = {o: {fa: fields for fa, fields in funcs.items() if fields} for o, funcs in got.items()}
        want = {o: expect.get(o, {}) for o in BOTH}
        ok = (set(report) == set(BOTH) and got == want
              and res.returncode == (1 if any(want.values()) else 0))
        failed |= not ok
        print("%s  %s\n    %s" % ("ok    " if ok else "FAILED", name, what))
        for o in BOTH:
            print("    order %-8s" % (o + ":"), "; ".join(
                "%s %s" % (fa, ", ".join("%s (%d of %s runs)" % (k, report[o][fa]["diffs"][k][0], report[o][fa]["runs"])
                                         for k in sorted(fields)))
                for fa, fields in sorted(got[o].items())) or ("nothing reported" if o in report else "not run"))
        if not ok:
            print("    expected:", {o: {fa: sorted(v) for fa, v in fs.items()} for o, fs in want.items()},
                  "exit", res.returncode, "output in", os.path.join(WORK, name + ".txt"))
    sys.exit(1 if failed else 0)


main()
