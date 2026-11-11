#!/usr/bin/env python3
"""Exercise the real x86 lifter with original, game-free PE32 machine code.

The fixture is authored here; it contains no Skyrim instructions or assets.
The only code generator is tools/lift.py, run before clang. The executed
Mach-O contains native ARM64 code and uses the real runtime dispatcher.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import struct
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
BASE = 0x400000
TEXT_RVA = 0x1000
STEP = 0x100


def dword(value: int) -> bytes:
    return struct.pack("<I", value)


def cases() -> list[tuple[str, bytes, list[tuple[int, int]] | None]]:
    """Known x86 encodings, with independent expected results in harness.c."""
    result = [
        # EAX wraps to zero; SETB/SETE materialize carry and zero flags.
        ("arithmetic_flags", bytes.fromhex("b8ffffffff83c0010f92c20f94c10fb6d20fb6c9c3"), None),
        # Round-trip a deliberately unaligned 32-bit guest address.
        ("unaligned_memory", bytes.fromhex("bb44332211891d03000200a1030002000fc8c3"), None),
        # Sum 5 + 4 + 3 + 2 + 1 with a conditional backward branch.
        ("branch_loop", bytes.fromhex("b90500000031c001c84975fbc3"), [(0, 7), (7, 12), (12, 13)]),
        ("cdecl_callee", bytes.fromhex("8b44240483c007c3"), None),
        ("direct_call", b"", None),
        ("indirect_call", b"", None),
        # Four packed single-precision additions through real guest memory.
        ("sse_packed", bytes.fromhex("0f1005001002000f100d101002000f58c10f110520100200c3"), None),
        # x87 stack push/add/pop and a 64-bit store.
        ("x87_add", bytes.fromhex("dd0500200200dd0508200200dec1dd1d10200200c3"), None),
        # x87 default round-to-nearest-even: 2.5 -> 2.
        ("x87_round", bytes.fromhex("dd0508200200db1d20200200c3"), None),
        # LOCK CMPXCHG success and failure are tested with distinct inputs.
        ("atomic_cmpxchg", bytes.fromhex("b805000000ba09000000f00fb115003002000f94c10fb6c9c3"), None),
        # REP MOVSD must preserve x86 forward-copy semantics, even overlapping.
        ("rep_movsd", bytes.fromhex("fcbe00400200bf10400200b904000000f3a5c3"), None),
        ("rep_overlap", bytes.fromhex("fcbe00400200bf04400200b904000000f3a5c3"), None),
        # A Windows-style FS-relative load reads from the CPU's guest TEB.
        ("fs_relative", bytes.fromhex("64a124000000c3"), None),
        # Host-invoked stdcall function pops its one argument.
        ("stdcall_callee", bytes.fromhex("8b44240483c003c20400"), None),
        # Lifted code -> host shim -> lifted code again -> back: registers set before the call must reach the
        # shim, and a register the shim changes must be seen after it. mov ebx,0x11; mov esi,0x22;
        # call [IAT_HOST]; add eax,ebx; add eax,esi; ret
        ("host_roundtrip", bytes.fromhex("bb11000000be22000000ff1500600200" "01d801f0c3"), None),
        ("setjmp_caller", b"", None),
        # push 7; push jmp_buf; call [IAT_LONGJMP]; ret (never reached)
        ("longjmp_thrower", bytes.fromhex("6a076800700200ff1508600200c3"), None),
        # There is intentionally no function at this dispatch target.
        ("unknown_call", bytes.fromhex("b8ff2f4000ffd0c3"), None),
        # A privileged instruction must become the upstream explicit trap.
        ("unimplemented_trap", bytes.fromhex("cd80c3"), None),
    ]
    callee = BASE + TEXT_RVA + 3 * STEP
    direct_address = BASE + TEXT_RVA + 4 * STEP
    direct = b"\x6a\x29\xe8" + struct.pack("<i", callee - (direct_address + 7))
    direct += bytes.fromhex("83c40483c001c3")
    indirect = b"\x6a\x09\xb8" + dword(callee) + bytes.fromhex("ffd083c40483c001c3")
    # mov ebx,5; mov edi,6; push jmp_buf; call [IAT_SETJMP3]; add esp,4; test eax,eax; jnz done;
    # mov ebx,0x77; mov edi,0x99; call longjmp_thrower; done: ret
    names = [name for name, _, _ in result]
    setjmp_address = BASE + TEXT_RVA + names.index("setjmp_caller") * STEP
    thrower = BASE + TEXT_RVA + names.index("longjmp_thrower") * STEP
    setjmp_code = bytes.fromhex("bb05000000bf060000006800700200ff150460020083c40485c0750f"
                                "bb77000000bf99000000") + b"\xe8" + struct.pack("<i", thrower - (setjmp_address + 43))
    setjmp_code += b"\xc3"
    result[names.index("setjmp_caller")] = ("setjmp_caller", setjmp_code, [(0, 28), (28, 43), (43, 44)])
    result[4] = ("direct_call", direct, None)
    result[5] = ("indirect_call", indirect, None)
    return result


def write_pe(path: Path, definitions: list[tuple[str, bytes, list[tuple[int, int]] | None]]) -> dict:
    """Write a minimal valid i386 PE32; pefile performs the production load."""
    section_size = ((len(definitions) * STEP + 0x1FF) // 0x200) * 0x200
    image = bytearray(0x200 + section_size)
    # Real x86 compiler padding, rather than zero bytes that decode as valid
    # instructions spanning unrelated test functions during recursive discovery.
    image[0x200:] = b"\xcc" * section_size
    image[:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, 0x80)
    image[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", image, 0x84, 0x14C, 1, 0, 0, 0, 0xE0, 0x0102)
    optional = 0x98
    struct.pack_into("<H", image, optional, 0x10B)
    struct.pack_into("<III", image, optional + 4, section_size, 0, 0)
    struct.pack_into("<III", image, optional + 16, TEXT_RVA, TEXT_RVA, 0)
    struct.pack_into("<III", image, optional + 28, BASE, 0x1000, 0x200)
    struct.pack_into("<HH", image, optional + 40, 6, 0)
    struct.pack_into("<HH", image, optional + 48, 6, 0)
    struct.pack_into("<II", image, optional + 56, 0x3000, 0x200)
    struct.pack_into("<HH", image, optional + 68, 3, 0)
    struct.pack_into("<IIIIII", image, optional + 72, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    section = optional + 0xE0
    image[section:section + 8] = b".text\0\0\0"
    struct.pack_into("<IIII", image, section + 8, section_size, TEXT_RVA, section_size, 0x200)
    struct.pack_into("<I", image, section + 36, 0x60000020)
    functions = {}
    symbols = {}
    for index, (name, code, blocks) in enumerate(definitions):
        address = BASE + TEXT_RVA + index * STEP
        offset = 0x200 + index * STEP
        image[offset:offset + len(code)] = code
        functions[hex(address)] = [[address + start, address + end] for start, end in
                                   (blocks or [(0, len(code))])]
        symbols[name] = address
    path.write_bytes(image)
    # Import slots live in guest memory outside the image; the harness fills them with host thunks.
    iat = {"0x26000": "fixture.dll!host_roundtrip", "0x26004": "msvcrt.dll!_setjmp3", "0x26008": "msvcrt.dll!longjmp"}
    return {"functions": functions, "iat": iat, "jump_tables": {}, "symbols": symbols}


def command(argv: list[str], *, env: dict | None = None) -> subprocess.CompletedProcess:
    result = subprocess.run(argv, cwd=ROOT, env=env, text=True, capture_output=True, timeout=120)
    if result.returncode:
        sys.stderr.write(result.stdout + result.stderr)
        raise RuntimeError(f"command failed ({result.returncode}): {argv}")
    return result


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "macos/build/aot-fixtures")
    parser.add_argument("--cc", default=os.environ.get("CC", "clang"))
    parser.add_argument("--strict-order", action="store_true", help="Enable runtime acquire/release guest accesses")
    parser.add_argument("--pinned", action="store_true",
                        help="Build the lifted code as LB_PINNED_REGS does (guest registers in x21-x28)")
    parser.add_argument("--discover", action="store_true", help="Run the real recursive function discovery before lifting")
    args = parser.parse_args()
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        parser.error("this proof requires an ARM64 macOS host")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    generated = output / "generated"
    pe = output / "synthetic-pe32.exe"
    data = write_pe(pe, cases())
    symbols = data.pop("symbols")
    function_map = output / "functions.json"
    function_map.write_text(json.dumps(data, indent=2) + "\n")
    header = output / "fixture_addresses.h"
    header.write_text("/* Authored synthetic functions; no game code. */\n#pragma once\n" +
                      "".join(f"#define FIX_{name.upper()} 0x{address:X}u\n" for name, address in symbols.items()))
    env = os.environ.copy()
    env.update({"LIFT_GAME": "skyrim", "LIFT_FILES": "2", "LIFT_TSO": "1"})
    if args.discover:
        discovered = command([sys.executable, str(ROOT / "tools/discover.py"), str(pe), str(function_map)], env=env)
        (output / "discovery.log").write_text(discovered.stdout + discovered.stderr)
        found = json.loads(function_map.read_text())["functions"]
        missing = sorted(set(symbols.values()) - {int(address, 16) for address in found})
        if missing:
            raise RuntimeError(f"authored function roots not found by actual discovery: {missing}")
    lifted = command([sys.executable, str(ROOT / "tools/lift.py"), str(pe), str(function_map),
                      str(generated), "fixture"], env=env)
    (output / "lift.log").write_text(lifted.stdout + lifted.stderr)
    rejected = json.loads((generated / "fixture_rejected.json").read_text())
    if rejected:
        raise RuntimeError(f"synthetic functions rejected by actual lifter: {rejected}")
    executable = output / "aot-fixtures"
    argv = [args.cc, "-arch", "arm64", "-std=gnu11", "-O2", "-fno-strict-aliasing", "-fwrapv",
            "-fsigned-char", "-DGAME_SKYRIM", "-I", str(ROOT / "runtime"), "-I", str(generated),
            "-I", str(output), str(ROOT / "tests/aot/harness.c"),
            str(ROOT / "runtime/dispatch.c"), str(ROOT / "runtime/fpu.c"), str(ROOT / "runtime/setjmp.c"), "-lpthread"]
    if args.strict_order:
        argv += ["-DLB_STRICT_ORDER"]
    if args.pinned:
        # As macos/CMakeLists.txt does: only the generated code reserves the registers and has the three
        # unsafe passes off; the runtime only knows that the mode is on.
        argv += ["-DLB_PINNED"]
        pinned = ["-ffixed-x%d" % n for n in range(21, 29)] + [
            "-mllvm", "-disable-postra-machine-licm", "-mllvm", "-disable-early-ifcvt",
            "-mllvm", "-aarch64-enable-ccmp=false"]
        common = [a for a in argv[1:] if a.startswith(("-D", "-f", "-O", "-std", "-arch")) or a == "arm64"]
        include = ["-I", str(ROOT / "runtime"), "-I", str(generated), "-I", str(output)]
        for path in sorted(generated.glob("*.c")):
            obj = path.with_suffix(".o")
            built = command([args.cc, *common, *include, *pinned, "-c", str(path), "-o", str(obj)])
            (output / "compile.log").write_text(built.stdout + built.stderr)
            argv.append(str(obj))
    else:
        argv += [str(path) for path in sorted(generated.glob("*.c"))]
    argv += ["-o", str(executable)]
    compiled = command(argv)
    (output / "compile.log").write_text(compiled.stdout + compiled.stderr)
    executed = command([str(executable)])
    (output / "run.log").write_text(executed.stdout + executed.stderr)
    observed = json.loads(executed.stdout)
    if observed.get("passed") != observed.get("total") or observed.get("total", 0) < 14:
        raise RuntimeError(f"fixture assertion summary failed: {observed}")
    file_info = command(["file", str(executable)]).stdout.strip()
    if "arm64" not in file_info or "Mach-O" not in file_info:
        raise RuntimeError(f"expected ARM64 Mach-O, found {file_info}")
    negative_results = {}
    for mode, message in [("unknown", "no recompiled code at 00402fff"),
                          ("trap", "unimplemented instruction 'int'")]:
        failed = subprocess.run([str(executable), "--" + mode], cwd=ROOT, text=True,
                                capture_output=True, timeout=15)
        (output / f"negative-{mode}.log").write_text(failed.stdout + failed.stderr)
        if failed.returncode != 86 or message not in failed.stderr:
            raise RuntimeError(f"generated {mode} path did not fail closed: {failed.returncode}, {failed.stderr}")
        negative_results[mode] = {"exit_code": failed.returncode, "expected_error": message}
    receipt = {
        "kind": "synthetic-x86-aot-to-arm64", "game_code": False, "runtime_cpu_jit": False,
        "scope": "Synthetic instruction and call fixtures only; not Skyrim boot or gameplay proof.",
        "strict_order": args.strict_order, "pinned": args.pinned, "host": platform.platform(), "architecture": platform.machine(),
        "recursive_discovery": args.discover,
        "source_revision": command(["git", "rev-parse", "HEAD"]).stdout.strip(),
        "python": sys.version, "compiler_command": argv, "executable": str(executable),
        "file": file_info, "observed": observed, "negative_tests": negative_results,
        "inputs": {str(path.relative_to(ROOT)): sha(path) for path in
                   [ROOT / "tools/lift.py", ROOT / "tools/discover.py", ROOT / "tools/peimg.py",
                    ROOT / "runtime/dispatch.c", ROOT / "runtime/fpu.c",
                    ROOT / "runtime/x86.h", ROOT / "runtime/lift_prelude.h", Path(__file__),
                    ROOT / "tests/aot/harness.c"]},
        "synthetic_pe_sha256": sha(pe), "executable_sha256": sha(executable),
        "generated_c_sha256": {path.name: sha(path) for path in sorted(generated.glob("*.c"))},
    }
    (output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({"passed": observed["passed"], "total": observed["total"],
                      "negative_tests": len(negative_results), "strict_order": args.strict_order,
                      "receipt": str(output / "receipt.json")}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
