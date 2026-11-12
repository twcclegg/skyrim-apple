"""Stack-frame analysis for the lifter's slot promotion (LIFT_SLOTS=1 in tools/lift.py).

The plain translation keeps the guest stack in guest memory: every [esp+N] or [ebp-N] operand, push and pop is
a load or store through MEM, and the C compiler has to assume that any other guest store may hit it. This
module works out, per function, which stack slots can be C locals instead.

Three steps:

1. frame_ir() reduces a function to the events that matter for its stack: changes of esp, the frame-pointer
   set-up, stack-relative operands, calls, exits, and anything that lets the stack address get away.
2. analyze() runs those events over the function's blocks and gives the height of esp (and of ebp, while it is
   a frame pointer) relative to esp on entry, at every instruction that touches the stack. What a callee adds
   to esp on return is taken from the callee's own `ret N` when it is a lifted function, and otherwise solved
   from the function's own balance (esp must be back at the return address at every ret, and agree where
   paths join). Heights that cannot be established reject the function.
3. classify() takes the stack accesses the lifter recorded while translating the function once, groups them
   into cells, and picks the cells that become locals.

What is proven and what is assumed is spelled out in docs/STACK_SLOTS.md.
"""
from fractions import Fraction

from capstone import x86

MASK = 0xFFFFFFFF
CALLEE_SAVED = ("ebx", "esi", "edi", "ebp")
ENV_OPS = ("fnstenv", "fstenv", "fldenv", "fnsave", "fsave", "frstor", "fxsave", "fxrstor")

# set by lift.py
decode_block = None
jump_tables = {}
iat = {}
is_setjmp = None


def s32(v):
    v &= MASK
    return v - (1 << 32) if v & 0x80000000 else v


def insn_events(i):
    """The stack-relevant events of one instruction, in the order they take effect."""
    mn, ops, A = i.mnemonic, i.operands, i.address
    if mn.startswith("lock "):
        mn = mn[5:]
    name = i.reg_name
    ev = []
    popadj = 4 if mn == "pop" else 0  # pop [esp+N] computes its address after esp has moved
    for op in ops:
        if op.type != x86.X86_OP_MEM:
            continue
        m = op.mem
        b = name(m.base) if m.base else None
        x = name(m.index) if m.index else None
        if b not in ("esp", "ebp") and x not in ("esp", "ebp"):
            continue
        fs = bool(m.segment) and name(m.segment) not in ("ss", "ds", "es", "cs")
        if mn == "lea":
            dst = name(ops[0].reg)
            if dst == "esp" and b == "esp" and not x:
                ev.append(("adj", A, s32(m.disp)))
            elif dst == "esp" and b == "ebp" and not x:
                ev.append(("sp_fp", A, s32(m.disp)))
            else:
                if dst == "esp":
                    ev.append(("badsp", A, "lea esp"))
                if "esp" in (b, x):
                    ev.append(("esc_sp", A))
                if "ebp" in (b, x):
                    ev.append(("esc_fp", A))
            continue
        if x == "ebp" or (b == "ebp" and (x or fs)):
            ev.append(("esc_fp", A))        # indexed off the frame pointer: the bytes reached are not known
        if "esp" in (b, x) and (x or fs):
            ev.append(("esc_sp", A))
        if not x and not fs:
            if (mn in ("bt", "bts", "btr", "btc") and ops[1].type == x86.X86_OP_REG) or mn in ENV_OPS \
                    or not 0 < op.size <= 16:
                ev.append(("wide", A, b))   # an access whose extent the operand does not give
            ev.append(("mem", A, b, s32(m.disp) + (popadj if b == "esp" else 0), op.size))
    regs = [name(op.reg) for op in ops if op.type == x86.X86_OP_REG]
    try:
        _, written = i.regs_access()
        written = {name(r) for r in written}
    except Exception:
        written = {"esp", "ebp"}
    sp_written = bool(written & {"esp", "sp", "spl"})
    fp_written = bool(written & {"ebp", "bp", "bpl"})
    done_sp = False
    if mn == "push":
        if regs and regs[0] in ("esp", "sp"):
            ev.append(("esc_sp", A))
        if regs and regs[0] in ("ebp", "bp"):
            ev.append(("esc_fp", A))
        ev.append(("push", A, 2 if ops[0].size == 2 and ops[0].type != x86.X86_OP_IMM and
                   not (regs and regs[0] in ("cs", "ds", "es", "fs", "gs", "ss")) else 4))
        done_sp = True
    elif mn == "pop":
        if regs and regs[0] in ("esp", "sp"):
            ev.append(("badsp", A, "pop esp"))
        ev.append(("pop", A, 2 if ops[0].size == 2 else 4))
        done_sp = True
    elif mn in ("pushal", "pushfd", "popal", "popfd"):
        ev.append(("nopromo", A, mn))
        ev.append(("adj", A, {"pushal": -32, "pushfd": -4, "popal": 32, "popfd": 4}[mn]))
        done_sp = True
    elif mn in ("add", "sub") and regs[:1] == ["esp"] and ops[1].type == x86.X86_OP_IMM:
        k = s32(ops[1].imm)
        ev.append(("adj", A, k if mn == "add" else -k))
        done_sp = True
    elif mn == "mov" and regs == ["ebp", "esp"]:
        ev.append(("setfp", A))
        return ev
    elif mn == "mov" and regs == ["esp", "ebp"]:
        ev.append(("sp_fp", A, 0))
        return ev
    elif mn == "leave":
        ev.append(("leave", A))
        return ev
    elif mn == "lea":
        done_sp = any(e[0] in ("adj", "sp_fp", "badsp") for e in ev)
    elif mn == "call":
        op = ops[0]
        direct = op.type == x86.X86_OP_IMM
        tgt = (op.imm if direct else op.mem.disp if op.type == x86.X86_OP_MEM else 0) & MASK
        if (direct or (op.type == x86.X86_OP_MEM and not op.mem.base and not op.mem.index)) and is_setjmp(tgt):
            ev.append(("nopromo", A, "setjmp"))
            ev.append(("call", A, None))
        else:
            ev.append(("call", A, tgt if direct else None))
        done_sp = True
    elif x86.X86_GRP_RET in i.groups:
        ev.append(("ret", A, ops[0].imm if ops else 0))
        return ev
    if sp_written and not done_sp:
        ev.append(("badsp", A, mn + " writes esp"))
    elif not done_sp and any(r in ("esp", "sp", "spl") for r in regs):
        ev.append(("esc_sp", A))
    if mn != "push" and any(op.type == x86.X86_OP_REG and name(op.reg) in ("ebp", "bp", "bpl") and op.access & 1
                            for op in ops):
        ev.append(("esc_fp", A))    # ebp read as a value (only matters while it is the frame pointer)
    if fp_written:
        ev.append(("killfp", A))
    return ev


class IR:
    """blocks: start -> (events, successors). An event is a tuple (kind, insn address, ...)."""
    __slots__ = ("blocks", "entry", "rets", "tails", "wild")


def frame_ir(fa, blocks, all_blocks=None):
    starts = {a for a, _ in blocks}
    ir = IR()
    ir.blocks, ir.entry, ir.rets, ir.tails, ir.wild = {}, fa, set(), set(), False
    for a, b in blocks:
        insns = decode_block(a, b)
        if not insns or insns[-1].address + insns[-1].size != b:
            return None
        ev, succ = [], []
        for i in insns:
            ev += insn_events(i)
        last = insns[-1]
        mn, g = last.mnemonic, set(last.groups)

        def go(t, ev=ev, succ=succ, last=last):
            t &= MASK
            if t in starts:
                if t not in succ:
                    succ.append(t)
            else:
                ev.append(("tail", last.address, t))
                ir.tails.add(t)
        if x86.X86_GRP_RET in g:
            ir.rets.add(last.operands[0].imm if last.operands else 0)
        elif mn == "jmp":
            op = last.operands[0]
            if op.type == x86.X86_OP_IMM:
                go(op.imm)
            elif last.address in jump_tables:
                for t in jump_tables[last.address]:
                    go(t)
            elif op.type == x86.X86_OP_MEM and not op.mem.base and not op.mem.index and (op.mem.disp & MASK) in iat:
                ev.append(("tail", last.address, None))
                ir.tails.add(None)
            else:
                ir.wild = True      # a computed jump with no table: it may land on any block
        elif mn in ("loop", "jecxz") or (x86.X86_GRP_JUMP in g and mn != "call"):
            go(last.operands[0].imm)
            go(b)
        elif mn in ("int3", "int"):
            pass                    # lifted as a call that does not return
        else:
            go(b)
        ir.blocks[a] = (ev, succ)
    if fa not in ir.blocks:
        return None
    return ir


# ---------------------------------------------------------------------------------------------------------
# heights

class Lin:
    """constant + sum(coef * var): the height of esp while some callee pops are still unknown"""
    __slots__ = ("c", "v")

    def __init__(self, c=0, v=None):
        self.c, self.v = c, v or {}

    def add(self, k):
        return Lin(self.c + k, self.v)

    def addvar(self, x):
        v = dict(self.v)
        v[x] = v.get(x, 0) + 1
        return Lin(self.c, v)

    def sub(self, o):
        v = dict(self.v)
        for x, k in o.v.items():
            v[x] = v.get(x, 0) - k
            if not v[x]:
                del v[x]
        return Lin(self.c - o.c, v)

    def same(self, o):
        return self.c == o.c and self.v == o.v


def solve(eqs):
    """Each Lin in eqs is zero. Returns ({var: value}, ok). A callee pops a non-negative multiple of 4."""
    rows = [(dict((x, Fraction(k)) for x, k in e.v.items()), Fraction(e.c)) for e in eqs]
    known = {}
    progress = True
    while progress:
        progress = False
        # Gaussian elimination: a row with one variable settles it
        for n, (v, c) in enumerate(rows):
            v = {x: k for x, k in v.items() if x not in known}
            c = c + sum(k * known[x] for x, k in rows[n][0].items() if x in known)
            rows[n] = (v, c)
        piv = {}
        for v, c in rows:
            v, c = dict(v), c
            for x, (pv, pc) in piv.items():
                if x in v:
                    k = v.pop(x)
                    for y, q in pv.items():
                        if y != x:
                            v[y] = v.get(y, 0) - k * q
                            if not v[y]:
                                del v[y]
                    c -= k * pc
            if not v:
                if c:
                    return known, False
                continue
            x = next(iter(v))
            k = v[x]
            piv[x] = ({y: q / k for y, q in v.items()}, c / k)
        for x, (pv, pc) in piv.items():
            rest = {y: q for y, q in pv.items() if y != x}
            if not rest:
                val = -pc
            elif pc == 0 and (all(q > 0 for q in rest.values())):
                val = 0             # x + (positive combination) = 0 with nothing negative: all are zero
                for y in rest:
                    if y not in known:
                        known[y] = Fraction(0)
                        progress = True
            else:
                continue
            if x not in known:
                known[x] = Fraction(val)
                progress = True
    out = {}
    for x, val in known.items():
        if val.denominator != 1 or val < 0 or val % 4 or val > 4096:
            return out, False
        out[x] = int(val)
    return out, True


class Frame:
    """The result of analyze() for one function."""
    __slots__ = ("ok", "why", "at", "calls", "succs", "balanced", "clash", "unproven", "ext", "cells", "stats", "fps")

    def __init__(self):
        self.ok, self.why = False, ""
        self.at = {}        # insn address -> (esp height, ebp height or None) before the instruction
        self.calls = {}     # call insn address -> (callee or None, height after return, guard needed)
        self.succs = {}
        self.balanced = False   # every exit was reached with esp at the return address
        self.clash = False      # the heights contradict each other: nothing about this function's esp is known
        self.unproven = False   # the result leans on a solved or assumed callee pop
        self.ext = None         # (lo, hi) of the bytes at or above the return address it touches, or None
        self.cells = {}
        self.stats = {}
        self.fps = set()    # heights ebp was set to by mov ebp, esp: the saved ebp sits there


def analyze(ir, pops, verified, known_funcs):
    """pops: lifted function -> bytes its ret removes beyond the return address (None: not known).
    verified: functions whose pop is proven rather than read off their ret instructions."""
    fr = Frame()
    if ir is None or ir.wild:
        fr.why = "computed jump" if ir is not None else "decode"
        fr.clash = True
        return fr
    fr.succs = {a: s for a, (_, s) in ir.blocks.items()}

    def callee_pop(t):
        return pops.get(t) if t is not None and t in known_funcs else None

    # pass 1: heights with unknown callee pops as variables; collect what must be zero
    eqs, state, work = [], {ir.entry: Lin()}, [ir.entry]
    seen = set()
    while work:
        a = work.pop()
        if a in seen:
            continue
        seen.add(a)
        h = state[a]
        fp = None
        ev, succ = ir.blocks[a]
        for e in ev:
            k = e[0]
            if k == "adj":
                h = h.add(e[2])
            elif k == "push":
                h = h.add(-e[2])
            elif k == "pop":
                h = h.add(e[2])
            elif k == "call":
                p = callee_pop(e[2])
                h = h.add(p) if p is not None else h.addvar(e[1])
            elif k in ("ret", "tail"):
                eqs.append(h)
            elif k == "badsp":
                fr.why, fr.clash = e[2], True
                return fr
            elif k in ("sp_fp", "leave"):
                h = None            # resolved in pass 2, where ebp's height is tracked
                break
        if h is None:
            continue
        for s in succ:
            if s in state:
                if not state[s].same(h):
                    eqs.append(state[s].sub(h))
            else:
                state[s] = h
                work.append(s)
    sol, ok = solve([e for e in eqs if e.v or e.c])
    # A function that restores esp from ebp gives no equations past that point; pass 2 decides.
    # pass 2: concrete heights. h is an int, or None after a call whose pop is still unknown.
    state, work, fpstate = {ir.entry: 0}, [ir.entry], {ir.entry: None}
    rejects = []
    lo, hi, ext_ok = 0, 0, True
    balanced = True
    done = {}
    while work:
        a = work.pop()
        h, fp = state[a], fpstate[a]
        if done.get(a) == (h, fp):
            continue
        done[a] = (h, fp)
        ev, succ = ir.blocks[a]
        last = None
        for e in ev:
            k, A = e[0], e[1]
            if A != last:
                fr.at[A] = (h, fp)
                last = A
            if k == "adj":
                h = None if h is None else h + e[2]
            elif k in ("push", "pop"):
                if h is None:
                    rejects.append("callee pop unknown")
                elif k == "push":
                    if h > 0:
                        lo, hi = min(lo, h - e[2]), max(hi, h)
                    h -= e[2]
                else:
                    if h + e[2] > 0:
                        lo, hi = min(lo, h), max(hi, h + e[2])
                    h += e[2]
            elif k == "mem":
                base = e[2]
                if base == "esp":
                    if h is None:
                        rejects.append("callee pop unknown")
                        continue
                    off = h + e[3]
                elif fp is not None:
                    off = fp + e[3]
                else:
                    continue
                if off + e[4] > 0:
                    lo, hi = min(lo, off), max(hi, off + e[4])
            elif k == "wide":
                if e[2] == "esp" or fp is not None:
                    rejects.append("stack access of unknown extent")
                    ext_ok = False
            elif k == "setfp":
                if h is None:
                    rejects.append("callee pop unknown")
                fp = h
                if h is not None:
                    fr.fps.add(h)
            elif k == "sp_fp":
                if fp is None:
                    fr.why, fr.clash = "esp restored from an untracked ebp", True
                    return fr
                h = fp + e[2]
            elif k == "leave":
                if fp is None:
                    fr.why, fr.clash = "leave with an untracked ebp", True
                    return fr
                h, fp = fp + 4, None
            elif k == "killfp":
                fp = None
            elif k == "esc_sp":
                rejects.append("stack address taken")
                ext_ok = False
            elif k == "esc_fp":
                if fp is not None:
                    rejects.append("frame address taken")
                    ext_ok = False
            elif k == "nopromo":
                rejects.append(e[2])
                ext_ok = False
            elif k == "call":
                t = e[2] if e[2] is not None and e[2] in known_funcs else None
                p = callee_pop(t)
                proven = p is not None and t in verified
                if p is None:
                    p = sol.get(A) if ok else None
                if not proven:
                    fr.unproven = True
                if h is None:
                    rejects.append("callee pop unknown")
                after = None if (h is None or p is None) else h + p
                fr.calls[A] = (t, after, not proven, fp)
                h = after
            elif k == "ret":
                if h is None:
                    balanced = False
                elif h != 0:
                    fr.why, fr.clash = "ret with esp %+d from the return address" % h, True
                    return fr
            elif k == "tail":
                if h is None:
                    balanced = False
                elif h != 0:
                    # control leaves for other code with this frame still on the stack (a shared tail)
                    rejects.append("leaves with a live frame")
                    balanced = False
                    ext_ok = False
        for s in succ:
            if s not in state:
                state[s], fpstate[s] = h, fp
                work.append(s)
            else:
                if state[s] != h:
                    if state[s] is None or h is None:
                        if state[s] is not None:
                            state[s] = None
                            work.append(s)
                    else:
                        fr.why, fr.clash = "paths join with different stack heights", True
                        return fr
                if fpstate[s] != fp:
                    # ebp is the frame pointer on one path and something else on another: an [ebp+N] operand
                    # after the join could be either, so nothing here can be promoted
                    rejects.append("frame pointer differs between paths")
                    if fpstate[s] is not None:
                        fpstate[s] = None
                        work.append(s)
    if len(state) != len(ir.blocks):
        rejects.append("unreached block")
    if not ok:
        rejects.append("callee pops do not solve")
    fr.balanced = balanced and not rejects.count("callee pop unknown") and ok
    if ext_ok and fr.balanced:
        fr.ext = (lo, hi)
    if rejects:
        fr.why = rejects[0]
    else:
        fr.ok = True
    return fr


def analyze_all(funcs, known_funcs, overrides, irs):
    """Frames for every function. funcs: address -> blocks; irs: a cache of frame_ir results."""
    known = {f for f in known_funcs if f not in overrides}
    for fa in funcs:
        if fa not in irs:
            irs[fa] = frame_ir(fa, funcs[fa])
    TOP = object()
    # what a function's ret instructions say it pops, following tail jumps
    syn = {}
    for fa in known:
        ir = irs.get(fa)
        syn[fa] = None if ir is None or ir.wild or len(ir.rets) > 1 else (next(iter(ir.rets)) if ir.rets else TOP)
    changed = True
    while changed:
        changed = False
        for fa in known:
            ir = irs.get(fa)
            if ir is None or syn[fa] is None:
                continue
            v = syn[fa]
            for t in ir.tails:
                tv = syn.get(t) if t in known else None
                if tv is None or (v is not TOP and tv is not TOP and tv != v):
                    v = None
                    break
                if v is TOP:
                    v = tv
            if v is not syn[fa]:
                syn[fa], changed = v, True
    pops = {fa: (None if v is TOP else v) for fa, v in syn.items()}
    verified = set(known)
    frames = {}
    changed = True
    while changed:      # pops and verified only ever lose members, so this ends
        changed = False
        for fa in funcs:
            fr = analyze(irs[fa], pops, verified, known)
            frames[fa] = fr
            if fa in known:
                if fr.clash and pops.get(fa) is not None:
                    pops[fa] = None
                    changed = True
                if (fr.clash or not fr.balanced or fr.unproven or pops.get(fa) is None) and fa in verified:
                    verified.discard(fa)
                    changed = True
    # what each function reads or writes at and above its return address, following tail jumps
    ext = {fa: frames[fa].ext for fa in funcs}
    changed = True
    while changed:
        changed = False
        for fa in funcs:
            if ext[fa] is None:
                continue
            lo, hi = ext[fa]
            for t in irs[fa].tails:
                te = ext.get(t) if t in known else None
                if te is None:
                    lo = None
                    break
                lo, hi = min(lo, te[0]), max(hi, te[1])
            new = None if lo is None else (lo, hi)
            if new != ext[fa]:
                ext[fa], changed = new, True
    for fa in funcs:
        frames[fa].ext = ext[fa]
    return frames, ext


# ---------------------------------------------------------------------------------------------------------
# cells

def classify(fr, acc, ext, entry):
    """acc: the lifter's record of stack accesses, in translation order within each block:
    (block, insn address, offset, size, kind, reg). kind: r/w an integer operand, x anything else,
    push/pop, ret (a call's return address), call (height at the call, callee in reg).
    Returns {byte offset: (cell low offset, cell size, C name)} for the promoted cells."""
    spans = sorted({(o, o + n) for _, _, o, n, k, _ in acc if k != "call"})
    cells = []
    for lo, hi in spans:
        if cells and lo < cells[-1][1]:
            cells[-1][1] = max(cells[-1][1], hi)
        else:
            cells.append([lo, hi])
    info = {}
    for lo, hi in cells:
        info[lo] = {"lo": lo, "hi": hi, "bad": "" if hi <= 0 and hi - lo in (1, 2, 4) else "shape", "reads": 0,
                    "pushes": set(), "pops": set(), "explicit": 0, "n": 0}
    starts = [c[0] for c in cells]

    import bisect

    def cell_of(o):
        return info[starts[bisect.bisect_right(starts, o) - 1]]
    for _, _, o, n, k, reg in acc:
        if k == "call":
            continue
        c = cell_of(o)
        c["n"] += 1
        if k in ("x", "ret"):
            c["bad"] = c["bad"] or ("return address" if k == "ret" else "other access")
        elif k == "r":
            c["reads"] += 1
            c["explicit"] += 1
        elif k == "w":
            c["explicit"] += 1
        elif k == "push":
            c["pushes"].add(reg)
        elif k == "pop":
            c["pops"].add(reg)
    for c in info.values():
        if c["bad"]:
            continue
        if "ebp" in c["pushes"] and c["lo"] in fr.fps:
            # the saved frame pointer: kept in memory so the ebp chain stays walkable
            c["bad"] = "saved frame pointer"
            continue
        if c["reads"]:
            continue    # a local: the function reads it back by name
        save = (not c["explicit"] and len(c["pushes"]) == 1 and c["pushes"] == c["pops"]
                and next(iter(c["pushes"])) in CALLEE_SAVED)
        if not save:
            c["bad"] = "never read here"    # outgoing arguments and dead stores stay in memory
    # A lifted callee whose reach above its return address is known must not reach a promoted cell.
    for _, _, o, _, k, t in acc:
        if k == "call" and t is not None and ext.get(t) is not None:
            wlo, whi = o - 4 + ext[t][0], o - 4 + ext[t][1]
            for c in info.values():
                if not c["bad"] and c["lo"] < whi and wlo < c["hi"]:
                    c["bad"] = "callee reads it"
    # Every read must follow a write of the whole cell on every path, so that nothing depends on what the
    # memory held before; a call ends the life of whatever lies below esp at that point.
    cand = {c["lo"] for c in info.values() if not c["bad"]}
    byblock = {}
    for blk, _, o, n, k, reg in acc:
        byblock.setdefault(blk, []).append((o, n, k))
    preds = {}
    for a, ss in fr.succs.items():
        for s in ss:
            preds.setdefault(s, []).append(a)
    out = {a: None for a in fr.succs}   # None: not computed yet (everything defined)
    bad = set()
    changed = True
    while changed:
        changed = False
        for a in sorted(fr.succs):
            ins = [out[p] for p in preds.get(a, []) if out[p] is not None]
            if a == entry:
                cur = set()
            elif not ins:
                if preds.get(a):
                    continue
                cur = set()
            else:
                cur = set.intersection(*ins)
            cur = set(cur)
            for o, n, k in byblock.get(a, []):
                if k == "call":
                    cur = {lo for lo in cur if lo >= o}
                    continue
                c = cell_of(o)
                lo = c["lo"]
                if lo not in cand:
                    continue
                full = o == lo and n == c["hi"] - lo
                if k in ("r", "pop") or (k == "w" and not full):
                    if lo not in cur:
                        bad.add(lo)
                if k in ("w", "push") and full:
                    cur.add(lo)
            if out[a] != cur:
                out[a], changed = cur, True
    for lo in bad:
        info[lo]["bad"] = "read before written"
    prom = {}
    for c in info.values():
        if not c["bad"]:
            ent = (c["lo"], c["hi"] - c["lo"], "sl_%X" % -c["lo"])
            for o in range(c["lo"], c["hi"]):
                prom[o] = ent
    fr.stats = {"cells": len({v[0] for v in prom.values()}),
                "accesses": sum(1 for _, _, o, _, k, _ in acc if k != "call" and o in prom),
                "kept": {}}
    for c in info.values():
        if c["bad"] and c["hi"] <= 0:
            fr.stats["kept"][c["bad"]] = fr.stats["kept"].get(c["bad"], 0) + c["n"]
    return prom
