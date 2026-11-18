#!/usr/bin/env python3
"""Run game-free C programs through the whole translation path and compare three executions.

Each program (tests/aot/X86SPEC.md) is compiled to 32-bit x86 by clang, linked here into a small PE image,
then run three ways for the same inputs:
  reference  the x86 code under the Unicorn emulator
  plain      discover.py + lift.py output, built for arm64, through runtime/dispatch.c
  pinned     the same lifted code built as LB_PINNED_REGS builds the game (guest registers in x21-x28)
The return value and a hash of the program's data must agree. Every program is tried at four x86
optimisation levels, which gives four different instruction mixes from one source.

usage: x86corpus.py [--work DIR] [--json OUT] [--levels O0,O1,O2,Os] FILE_OR_DIR...
Status per program:
  REJECT    the program breaks the spec (does not compile, unresolved symbol, sanitiser report, emulator
            fault or timeout, or the arm64-native build of the same source returns something else).
  UNLIFTED  the lifter or discovery could not handle the generated x86 (coverage gap, not a wrong result).
  OK        reference, plain and pinned agree at every level.
  MISMATCH  plain or pinned differs from the reference. A finding: keep the file unchanged.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
BASE, TEXT_RVA = 0x400000, 0x1000
STACK_TOP, SENTINEL = 0x180000, 0xFEEDF00D
INPUTS = [(0, 0), (1, 2), (5, 9), (17, 3), (255, 256), (0x7FFFFFFF, 1), (0x80000000, 0xFFFFFFFF), (123456789, 987654321)]
X86_FLAGS = ["--target=i686-unknown-linux-gnu", "-march=i686", "-ffreestanding", "-fno-builtin", "-fno-jump-tables",
             "-fno-pic", "-fno-common", "-fno-stack-protector", "-fno-asynchronous-unwind-tables", "-fwrapv",
             "-mno-sse", "-mno-mmx", "-fno-strict-aliasing", "-fno-math-errno", "-w", "-include", str(HERE / "x86prog.h")]
# X86CORPUS_NO_PASS_FLAGS=1 leaves the three pass-disabling flags out of the pinned build: a control run that
# should produce MISMATCH results and so shows the corpus can detect the known compiler bugs.
PINNED = ["-ffixed-x%d" % n for n in range(21, 29)] + ([] if os.environ.get("X86CORPUS_NO_PASS_FLAGS") == "1" else [
    "-mllvm", "-disable-postra-machine-licm", "-mllvm", "-disable-early-ifcvt", "-mllvm", "-aarch64-enable-ccmp=false"])


def sh(argv, **kw):
    return subprocess.run([str(a) for a in argv], capture_output=True, text=True, **kw)


# The generated code is built for the machine this runs on: arm64 on a Mac (as the game is), the native
# architecture elsewhere. Pinned registers exist on arm64 only, so other hosts skip that run.
HOST_ARCH = ["-arch", "arm64"] if sys.platform == "darwin" else []
HOST_LIBS = [] if sys.platform == "darwin" else ["-lm", "-lpthread"]
HAVE_PINNED = sys.platform == "darwin" or os.uname().machine in ("aarch64", "arm64")


class LinkError(Exception):
    pass


def read_elf(path):
    d = Path(path).read_bytes()
    if d[:4] != b"\x7fELF" or d[4] != 1:
        raise LinkError("not an ELF32 object")
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
    secs = [dict(zip(("name", "type", "flags", "addr", "offset", "size", "link", "info", "align", "entsize"),
                     struct.unpack_from("<10I", d, shoff + i * shentsize))) for i in range(shnum)]
    strtab = secs[shstrndx]

    def cstr(tab, off):
        s = tab["offset"] + off
        return d[s:d.index(b"\0", s)].decode()
    for s in secs:
        s["sname"] = cstr(strtab, s["name"])
    return d, secs, cstr


def link(objects):
    """Lay out the allocated sections of each object (text first, then data) and apply i386 relocations.
    Every object must be self-contained. Returns (text bytes, data bytes, per-object info)."""
    text, data, infos, pending = bytearray(), bytearray(), [], []
    for path in objects:
        d, secs, cstr = read_elf(path)
        where = {}
        for kind in ("text", "data"):
            lo = len(text if kind == "text" else data)
            for i, s in enumerate(secs):
                if not s["flags"] & 2 or s["type"] not in (1, 8) or bool(s["flags"] & 4) != (kind == "text"):
                    continue
                buf = text if kind == "text" else data
                while len(buf) % max(s["align"], 1):
                    buf.append(0xCC if kind == "text" else 0)
                where[i] = (kind, len(buf))
                buf += d[s["offset"]:s["offset"] + s["size"]] if s["type"] == 1 else bytes(s["size"])
            if kind == "text":
                while len(text) % 16:
                    text.append(0xCC)
                tr = (lo, len(text))
            else:
                while len(data) % 16:
                    data.append(0)
                dr = (lo, len(data))
        pending.append((path, d, secs, cstr, where))
        infos.append({"object": str(path), "text": tr, "data": dr})
    text_size = (len(text) + 0xFFF) & ~0xFFF
    text += b"\xCC" * (text_size - len(text))
    data_rva = TEXT_RVA + text_size

    def addr(kind, off):
        return BASE + (TEXT_RVA if kind == "text" else data_rva) + off
    for info, (path, d, secs, cstr, where) in zip(infos, pending):
        symtab = next((s for s in secs if s["type"] == 2), None)
        syms = []
        if symtab:
            for k in range(symtab["size"] // 16):
                name, value, size, sinfo, other, shndx = struct.unpack_from("<IIIBBH", d, symtab["offset"] + 16 * k)
                syms.append((cstr(secs[symtab["link"]], name), value, shndx))
        def sym_addr(k):
            name, value, shndx = syms[k]
            if shndx in where:
                return addr(where[shndx][0], where[shndx][1] + value)
            raise LinkError("unresolved symbol %s" % (name or "?"))
        for name, value, shndx in syms:
            if name == "test_main" and shndx in where:
                info["entry"] = addr(where[shndx][0], where[shndx][1] + value)
        if "entry" not in info:
            raise LinkError("no test_main")
        for s in secs:
            if s["type"] == 4:
                raise LinkError("unexpected RELA section")
            if s["type"] != 9 or s["info"] not in where:
                continue
            kind, base_off = where[s["info"]]
            buf = text if kind == "text" else data
            for k in range(s["size"] // 8):
                off, rinfo = struct.unpack_from("<II", d, s["offset"] + 8 * k)
                place = base_off + off
                addend, = struct.unpack_from("<i", buf, place)
                target = sym_addr(rinfo >> 8)
                rtype = rinfo & 0xFF
                if rtype == 1:
                    value = target + addend
                elif rtype in (2, 4):
                    value = target + addend - addr(kind, place)
                else:
                    raise LinkError("unsupported relocation type %d" % rtype)
                struct.pack_into("<I", buf, place, value & 0xFFFFFFFF)
        info["data_abs"] = (addr("data", info["data"][0]), addr("data", info["data"][1]))
        info["text_abs"] = (addr("text", info["text"][0]), addr("text", info["text"][1]))
    # A table of entry points at the end of the data, so discovery finds every program as a root.
    table_at = len(data)
    for info in infos:
        data += struct.pack("<I", info["entry"])
    data += bytes(-len(data) % 0x1000)
    return bytes(text), bytes(data), infos, data_rva


def write_pe(path, text, data, data_rva, entry):
    hdr = bytearray(0x400)
    hdr[:2] = b"MZ"
    struct.pack_into("<I", hdr, 0x3C, 0x80)
    hdr[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", hdr, 0x84, 0x14C, 2, 0, 0, 0, 0xE0, 0x0102)
    opt = 0x98
    struct.pack_into("<H", hdr, opt, 0x10B)
    struct.pack_into("<III", hdr, opt + 4, len(text), len(data), 0)
    struct.pack_into("<III", hdr, opt + 16, entry - BASE, TEXT_RVA, data_rva)
    struct.pack_into("<III", hdr, opt + 28, BASE, 0x1000, 0x200)
    struct.pack_into("<HH", hdr, opt + 40, 6, 0)
    struct.pack_into("<HH", hdr, opt + 48, 6, 0)
    struct.pack_into("<II", hdr, opt + 56, data_rva + len(data), 0x400)
    struct.pack_into("<HH", hdr, opt + 68, 3, 0)
    struct.pack_into("<IIIIII", hdr, opt + 72, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    sec = opt + 0xE0
    for k, (name, rva, blob, chars) in enumerate(((b".text", TEXT_RVA, text, 0x60000020),
                                                  (b".data", data_rva, data, 0xC0000040))):
        at = sec + 40 * k
        hdr[at:at + len(name)] = name
        struct.pack_into("<IIII", hdr, at + 8, len(blob), rva, len(blob), 0x400 + (0 if k == 0 else len(text)))
        struct.pack_into("<I", hdr, at + 36, chars)
    Path(path).write_bytes(bytes(hdr) + text + data)


def fnv(buf):
    h = 2166136261
    for i in range(0, len(buf), 4):
        h = ((h ^ struct.unpack_from("<I", buf, i)[0]) * 16777619) & 0xFFFFFFFF
    return h


def run_reference(text, data, data_rva, infos):
    import unicorn as U
    from unicorn import x86_const as X
    out = {}
    size = data_rva + len(data)
    for k, info in enumerate(infos):
        res = []
        for a, b in INPUTS:
            mu = U.Uc(U.UC_ARCH_X86, U.UC_MODE_32)
            mu.mem_map(BASE, (size + 0xFFF) & ~0xFFF)
            mu.mem_write(BASE + TEXT_RVA, text)
            mu.mem_write(BASE + data_rva, data)
            mu.mem_map(0x80000, 0x101000)
            mu.mem_map(SENTINEL & ~0xFFF, 0x1000)
            esp = STACK_TOP
            for v in (b, a, SENTINEL):
                esp -= 4
                mu.mem_write(esp, struct.pack("<I", v))
            mu.reg_write(X.UC_X86_REG_ESP, esp)
            try:
                mu.emu_start(info["entry"], SENTINEL, count=20_000_000)
                if mu.reg_read(X.UC_X86_REG_EIP) != SENTINEL:
                    res = "timeout (over 20M instructions)"
                    break
            except U.UcError as e:
                res = "emulator fault: %s at %08x" % (e, mu.reg_read(X.UC_X86_REG_EIP))
                break
            lo, hi = info["data_abs"]
            res.append("%08x %08x" % (mu.reg_read(X.UC_X86_REG_EAX), fnv(bytes(mu.mem_read(lo, hi - lo)))))
        out[k] = res
    return out


HARNESS = r'''
#include "rt.h"
#include "rt_table.h"
#include <stdarg.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "layout.h"
uint8_t *MEM; int g_log_level;
extern const FnEntry g_fn_table_fixture[]; extern const unsigned g_fn_count_fixture;
const FnModule g_modules[] = {{"x86corpus", g_fn_table_fixture, 0}}; const unsigned g_nmodules = 1;
unsigned rt_module_count(unsigned i) { return i == 0 ? g_fn_count_fixture : 0; }
void rt_fatal(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); _exit(86); }
int platform_frame_count(void) { return 0; } void prof_report(void) {} void cpu_report(void) {}
static uint32_t fnv(const uint8_t *p, uint32_t n) { uint32_t h = 2166136261u; for (uint32_t i = 0; i < n; i += 4) { uint32_t w; memcpy(&w, p + i, 4); h = (h ^ w) * 16777619u; } return h; }
int main(int argc, char **argv) {
    MEM = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    FILE *f = fopen(argv[1], "rb"); static uint8_t img[IMG_SIZE]; if (fread(img, 1, IMG_SIZE, f) != IMG_SIZE) return 2; fclose(f);
    /* X86CORPUS_REPS=n calls each program n times per input without resetting its data in between: a load
     * for timing the builds against each other (the printed values then differ from the reference's). */
    int reps = getenv("X86CORPUS_REPS") ? atoi(getenv("X86CORPUS_REPS")) : 1;
    for (unsigned p = 0; p < NPROG; p++) {
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            alarm(reps > 1 ? 600 : 20);
            for (unsigned i = 0; i < NINPUT; i++) {
                memcpy(MEM + IMG_BASE, img, IMG_SIZE);
                memset(MEM + 0x80000, 0, 0x101000);
                CPU c = {0}; c.r[4] = 0x180000; c.fs_base = 0x25000; c.fpu_cw = 0x37F; c.mxcsr = 0x1F80;
                uint32_t args[2] = {inputs[i][0], inputs[i][1]};
                uint32_t r = 0;
                for (int k = 0; k < reps; k++) { c.r[4] = 0x180000; r = rt_invoke(&c, progs[p].entry, 2, args); }
                printf("P %u %08x %08x\n", p, r, fnv(MEM + progs[p].data_lo, progs[p].data_hi - progs[p].data_lo));
            }
            fflush(stdout); _exit(0);
        }
        int st = 0; waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st)) printf("F %u %d\n", p, WIFEXITED(st) ? WEXITSTATUS(st) : -WTERMSIG(st));
    }
    return 0;
}
'''


# name: (lifter environment, extra compiler flags, arm64 only). "slots" is the lifter's stack-slot promotion
# (LIFT_SLOTS=1); "slots-check" builds the same code with LB_SLOT_CHECK, where every promoted slot is mirrored
# in guest memory under a key and compared on each read, so a slot that something else reads or writes shows.
MODES = {
    "plain": ({"LIFT_SLOTS": "0"}, [], False),
    "pinned": ({"LIFT_SLOTS": "0"}, ["-DLB_PINNED"] + PINNED, True),
    "slots": ({"LIFT_SLOTS": "1"}, [], False),
    "slots-check": ({"LIFT_SLOTS": "1"}, ["-DLB_SLOT_CHECK"], False),
    "slots-pinned": ({"LIFT_SLOTS": "1"}, ["-DLB_PINNED"] + PINNED, True),
}


def run_lifted(work, text, data, data_rva, infos, cc, modes):
    """Returns ({mode: {index: results or error string}}, error or None)."""
    pe, fmap = work / "prog.exe", work / "functions.json"
    write_pe(pe, text, data, data_rva, infos[0]["entry"])
    env = dict(os.environ, LIFT_GAME="skyrim", LIFT_FILES="2", LIFT_TSO="1")
    p = sh([sys.executable, ROOT / "tools/discover.py", pe, fmap], env=env, cwd=ROOT)
    if p.returncode:
        return None, "discovery failed: " + (p.stderr.strip().split("\n") or ["?"])[-1][:200]
    found = {int(a, 16) for a in json.loads(fmap.read_text())["functions"]}
    img = bytes(TEXT_RVA) + text + data
    (work / "image.bin").write_bytes(img)
    (work / "layout.h").write_text(
        "#define IMG_BASE 0x%Xu\n#define IMG_SIZE %du\n#define NPROG %du\n#define NINPUT %du\n" % (BASE, len(img), len(infos), len(INPUTS))
        + "static const struct { uint32_t entry, data_lo, data_hi; } progs[] = {\n"
        + "".join("  {0x%Xu, 0x%Xu, 0x%Xu},\n" % (i["entry"], *i["data_abs"]) for i in infos) + "};\n"
        + "static const uint32_t inputs[][2] = {" + ", ".join("{0x%Xu, 0x%Xu}" % ab for ab in INPUTS) + "};\n")
    (work / "harness.c").write_text(HARNESS)
    lifted, results = {}, {}
    for mode in modes:
        lift_env, flags, _ = MODES[mode]
        key = "-".join("%s%s" % kv for kv in sorted(lift_env.items()))
        gen = work / ("generated-" + key)
        if key not in lifted:
            gen.mkdir(exist_ok=True)
            p = sh([sys.executable, ROOT / "tools/lift.py", pe, fmap, gen, "fixture"], env=dict(env, **lift_env), cwd=ROOT)
            if p.returncode:
                return None, "lifter failed (%s): %s" % (mode, (p.stderr.strip().split("\n") or ["?"])[-1][:200])
            rejected = json.loads((gen / "fixture_rejected.json").read_text())
            rej_addrs = [int(a, 16) for a in rejected]
            unlifted = {}
            for k, info in enumerate(infos):
                lo, hi = info["text_abs"]
                bad = [a for a in rej_addrs if lo <= a < hi]
                if bad:
                    unlifted[k] = "lifter rejected a function at %08x %s" % (bad[0], str(rejected.get(hex(bad[0]), ""))[:120])
                elif info["entry"] not in found:
                    unlifted[k] = "discovery did not find test_main"
            lifted[key] = unlifted
        unlifted = lifted[key]
        common = HOST_ARCH + ["-std=gnu11", "-O2", "-fno-strict-aliasing", "-fwrapv", "-fsigned-char", "-w",
                              "-DGAME_SKYRIM", "-I", ROOT / "runtime", "-I", gen, "-I", work]
        exe = work / ("run-" + mode)
        objs = []
        for src in sorted(gen.glob("*.c")):
            obj = work / (src.stem + "-" + mode + ".o")
            p = sh(cc + common + flags + ["-c", src, "-o", obj])
            if p.returncode:
                return None, "generated code does not compile (%s): %s" % (mode, p.stderr.strip().split("\n")[0][:200])
            objs.append(obj)
        p = sh(cc + common + [f for f in flags if f.startswith("-D")] +
               [work / "harness.c", ROOT / "runtime/dispatch.c", ROOT / "runtime/fpu.c", ROOT / "runtime/setjmp.c"] + objs
               + HOST_LIBS + ["-o", exe])
        if p.returncode:
            return None, "link failed (%s): %s" % (mode, p.stderr.strip().split("\n")[0][:200])
        p = sh([exe, work / "image.bin"], timeout=600)
        got = {k: [] for k in range(len(infos))}
        for line in p.stdout.splitlines():
            w = line.split()
            if w[0] == "P":
                got[int(w[1])].append("%s %s" % (w[2], w[3]))
            elif w[0] == "F":
                got[int(w[1])] = "run failed with status %s" % w[2]
        for k, why in unlifted.items():
            got[k] = why
        results[mode] = got
    return results, None


NATIVE_MAIN = r'''
#include <stdio.h>
#include <stdint.h>
uint32_t test_main(uint32_t a, uint32_t b);
#include <stdlib.h>
/* One input per process, so every run starts from the program's initial data, as the other executions do. */
int main(int argc, char **argv) { static const uint32_t in[][2] = {%s};
  unsigned i = (unsigned)atoi(argv[1]); printf("%%08x\n", test_main(in[i][0], in[i][1])); return 0; }
'''


def native_check(cc, src, work):
    """The same source built for the host with sanitisers: catches undefined behaviour and gives the
    return values a second, independent time."""
    main = work / "native_main.c"
    main.write_text(NATIVE_MAIN % ", ".join("{0x%Xu, 0x%Xu}" % ab for ab in INPUTS))
    exe = work / "native"
    san = ["-O0", "-fwrapv", "-fno-strict-aliasing", "-w", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    obj = work / "native_prog.o"
    p = sh(cc + san + ["-Werror=uninitialized", "-Werror=implicit-function-declaration", "-Werror=return-type",
                       "-Dmemcpy=x86prog_memcpy", "-Dmemset=x86prog_memset", "-Dmemmove=x86prog_memmove",
                       "-include", HERE / "x86prog.h", "-c", src, "-o", obj])
    if not p.returncode:
        p = sh(cc + san + [obj, main, "-o", exe] + HOST_LIBS)
    if p.returncode:
        return None, "host build: " + (p.stderr.strip().split("\n") or ["?"])[0][:200]
    out = []
    for i in range(len(INPUTS)):
        try:
            q = sh([exe, str(i)], timeout=30)
        except subprocess.TimeoutExpired:
            return None, "host run: timeout"
        if q.returncode:
            return None, "host run (sanitisers): " + (q.stderr.strip().split("\n") or ["exit %d" % q.returncode])[0][:200]
        out += q.stdout.split()
    return out, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default="xcrun clang" if sys.platform == "darwin" else "clang")
    ap.add_argument("--work")
    ap.add_argument("--json")
    ap.add_argument("--levels", default="O0,O1,O2,Os")
    ap.add_argument("--modes", default=",".join(m for m, v in MODES.items() if HAVE_PINNED or not v[2]),
                    help="which lifted builds to run and compare: " + ", ".join(MODES))
    ap.add_argument("--native-cc", help="compiler for the sanitiser build of each program (default: --cc)")
    ap.add_argument("--reference", choices=("emulator", "plain"), default="emulator",
                    help="what the lifted builds must agree with: the x86 code under Unicorn (default), or the "
                         "plain lifted build, for programs with float arithmetic (x87 is emulated in 80 bits, "
                         "lifted in 64): every other mode must then match it bit for bit")
    ap.add_argument("--x86-cc", help="compiler for the 32-bit x86 programs (default: --cc); gcc works too")
    ap.add_argument("--x86-flags", default="", help="extra flags for the x86 build, e.g. -mrtd or -fno-omit-frame-pointer")
    ap.add_argument("paths", nargs="+")
    a = ap.parse_args()
    cc = a.cc.split()
    modes = a.modes.split(",")
    x86_cc = a.x86_cc.split() if a.x86_cc else cc
    # gcc takes -m32 where clang takes a target triple; everything else in X86_FLAGS is common to both
    x86_flags = (["-m32"] + X86_FLAGS[1:] if "gcc" in x86_cc[0] else X86_FLAGS) + a.x86_flags.split()
    files = []
    for p in a.paths:
        files += sorted(Path(p).rglob("*.c")) if Path(p).is_dir() else [Path(p)]
    tmp = tempfile.TemporaryDirectory()
    top = Path(a.work) if a.work else Path(tmp.name)
    top.mkdir(parents=True, exist_ok=True)
    status = {f: ["OK", ""] for f in files}

    def mark(f, s, why):
        order = {"OK": 0, "UNLIFTED": 1, "MISMATCH": 2, "REJECT": 3}
        if order[s] > order[status[f][0]]:
            status[f] = [s, why]
    native = {}
    for f in files:
        native[f], err = native_check(a.native_cc.split() if a.native_cc else cc, f, top)
        if err:
            mark(f, "REJECT", err)
    for level in a.levels.split(","):
        work = top / level
        work.mkdir(exist_ok=True)
        objs, live = [], []
        for n, f in enumerate(files):
            if status[f][0] == "REJECT":
                continue
            obj = work / ("p%04d.o" % n)
            p = sh(x86_cc + x86_flags + ["-" + level, "-c", f, "-o", obj])
            if p.returncode:
                mark(f, "REJECT", "x86 build -%s: %s" % (level, (p.stderr.strip().split("\n") or ["?"])[0][:200]))
                continue
            try:
                link([obj])
            except LinkError as e:
                mark(f, "REJECT", "link -%s: %s" % (level, e))
                continue
            objs.append(obj)
            live.append(f)
        if not objs:
            continue
        text, data, infos, data_rva = link(objs)
        if a.reference == "plain":
            if "plain" not in modes:
                modes = ["plain"] + modes
            runs, err = run_lifted(work, text, data, data_rva, infos, cc, modes)
            ref = runs["plain"] if runs else {k: "no plain build" for k in range(len(live))}
        else:
            ref = run_reference(text, data, data_rva, infos)
            runs, err = run_lifted(work, text, data, data_rva, infos, cc, modes)
        for k, f in enumerate(live):
            if isinstance(ref[k], str):
                mark(f, "UNLIFTED" if a.reference == "plain" else "REJECT", "-%s reference: %s" % (level, ref[k]))
                continue
            if a.reference == "emulator" and [r.split()[0] for r in ref[k]] != native[f]:
                mark(f, "REJECT", "-%s: the x86 build and the arm64-native build of this source return different values "
                                  "(the program depends on undefined or platform-specific behaviour)" % level)
                continue
            if err:
                mark(f, "UNLIFTED", "-%s: %s" % (level, err))
                continue
            for mode, got in ((m, runs[m][k]) for m in modes if not (a.reference == "plain" and m == "plain")):
                if isinstance(got, str):
                    mark(f, "UNLIFTED" if "lifter" in got or "discovery" in got else "MISMATCH", "-%s %s: %s" % (level, mode, got))
                elif got != ref[k]:
                    i = next((i for i in range(len(ref[k])) if i >= len(got) or got[i] != ref[k][i]), 0)
                    mark(f, "MISMATCH", "-%s %s: input %s gives %s, reference %s" % (
                        level, mode, INPUTS[i], got[i] if i < len(got) else "nothing", ref[k][i]))
    counts = {}
    for f in files:
        s, why = status[f]
        counts[s] = counts.get(s, 0) + 1
        print("%-8s %s%s" % (s, f, "  (" + why + ")" if why else ""))
    print("%d programs: " % len(files) + ", ".join("%d %s" % (counts.get(s, 0), s) for s in ("OK", "MISMATCH", "UNLIFTED", "REJECT")))
    if a.json:
        json.dump([{"file": str(f), "status": status[f][0], "why": status[f][1]} for f in files], open(a.json, "w"), indent=1)
    return 1 if counts.get("REJECT") else 0


if __name__ == "__main__":
    sys.exit(main())
