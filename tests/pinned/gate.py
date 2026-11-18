#!/usr/bin/env python3
"""Gate and differential check for corpus programs (tests/pinned/SPEC.md).

usage: gate.py [--cc "xcrun clang"] [--json OUT] FILE_OR_DIR...

For each .c file:
  REJECT   the program breaks a rule: does not compile warning-free for uninitialised variables, fails or
           reports under the address/undefined-behaviour sanitisers at -O0, runs over 5 s, prints different
           output on two runs, prints fewer than 3 REPORT lines, or has no register write under a branch or loop.
           Fix the program.
  OK       every optimised build with the pinned build's flags prints what -O0 prints.
  MISMATCH an optimised build with the flags differs from -O0. This is a finding about the compiler, not a
           fault in the program: leave the file as it is.
Also notes ("exposes") whether the program shows the known compiler bugs when the flags are left out.
Exit status 1 if any file is REJECT.
"""
import argparse, json, os, re, shlex, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FIXED = ["-ffixed-x%d" % n for n in range(21, 29)]
SAFE = ["-mllvm", "-disable-postra-machine-licm", "-mllvm", "-disable-early-ifcvt", "-mllvm", "-aarch64-enable-ccmp=false"]
CONFIGS = [["-O1"], ["-O2"], ["-O3"], ["-Os"], ["-O2", "-mllvm", "-aarch64-stress-ccmp"]]
REG = r"\b(eax|ecx|edx|ebx|esp|ebp|esi|edi)\b"


def run(cc, flags, src, exe, timeout=5):
    p = subprocess.run(cc + FIXED + ["-I", HERE, "-w"] + flags + [src, "-o", exe], capture_output=True, text=True)
    if p.returncode:
        return None, "compile: " + p.stderr.strip().split("\n")[0][:200]
    try:
        q = subprocess.run([exe], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, "timeout"
    if q.returncode:
        return None, "exit %d: %s" % (q.returncode, (q.stderr.strip().split("\n") or [""])[0][:200])
    return q.stdout, None


def check(cc, src, work):
    text = open(src).read()
    if '#include "pinned_test.h"' not in text:
        return "REJECT", "does not include pinned_test.h", None
    if not re.search(r"\b(if|for|while|switch)\b[^;{]*\{[^}]*" + REG + r"\s*([-+^|&*]|<<|>>)?=[^=]", text, re.S):
        return "REJECT", "no register write under a branch or loop", None
    exe = os.path.join(work, "t")
    p = subprocess.run(cc + FIXED + ["-I", HERE, "-O1", "-fsyntax-only", "-Werror=uninitialized",
                                     "-Werror=sometimes-uninitialized", "-Werror=implicit-function-declaration",
                                     "-Werror=return-type", src], capture_output=True, text=True)
    if p.returncode:
        return "REJECT", "warning: " + p.stderr.strip().split("\n")[0][:200], None
    out, err = run(cc, ["-O0", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"], src, exe, 20)
    if err:
        return "REJECT", "sanitiser build: " + err, None
    ref, err = run(cc, ["-O0"], src, exe)
    if err:
        return "REJECT", "-O0: " + err, None
    again, _ = run(cc, ["-O0"], src, exe)
    if again != ref or out != ref:
        return "REJECT", "output is not deterministic (or differs under the sanitisers)", None
    if len(ref.strip().split("\n")) < 3:
        return "REJECT", "fewer than 3 REPORT lines", None
    bad = [" ".join(c) for c in CONFIGS if run(cc, c + SAFE, src, exe)[0] != ref]
    exposes = [" ".join(c) for c in CONFIGS if run(cc, c, src, exe)[0] != ref]
    if bad:
        return "MISMATCH", "differs from -O0 under: " + "; ".join(bad), exposes
    return "OK", "", exposes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default="xcrun clang" if sys.platform == "darwin" else "cc")
    ap.add_argument("--json")
    ap.add_argument("paths", nargs="+")
    a = ap.parse_args()
    files = []
    for p in a.paths:
        if os.path.isdir(p):
            files += sorted(os.path.join(d, f) for d, _, fs in os.walk(p) for f in fs if f.endswith(".c"))
        else:
            files.append(p)
    cc, results = shlex.split(a.cc), []
    with tempfile.TemporaryDirectory() as work:
        for f in files:
            status, why, exposes = check(cc, f, work)
            results.append({"file": f, "status": status, "why": why, "exposes_known_bugs": bool(exposes)})
            print("%-8s %s%s%s" % (status, f, "  (" + why + ")" if why else "", "  [exposes known bugs]" if exposes else ""))
    n = {s: sum(r["status"] == s for r in results) for s in ("OK", "MISMATCH", "REJECT")}
    print("%d files: %d OK, %d MISMATCH, %d REJECT; %d expose the known bugs without the flags"
          % (len(results), n["OK"], n["MISMATCH"], n["REJECT"], sum(r["exposes_known_bugs"] for r in results)))
    if a.json:
        json.dump(results, open(a.json, "w"), indent=1)
    return 1 if n["REJECT"] else 0


if __name__ == "__main__":
    sys.exit(main())
