#!/usr/bin/env python3
"""cache-overlap.py: compare two DXVK state cache files (*.dxvk-cache).

Usage:
    python3 -I cache-overlap.py A.dxvk-cache B.dxvk-cache [--normalize] [--show N] [--top N]

Reports, for each file: header version, entries read, invalid entries skipped, graphics/compute split;
then entries in both, only in A and only in B. Each "only in" entry is matched against the other file:

  new shaders   no entry in the other file uses the same shader set. Split by whether the other file
                has seen the vertex / fragment shader at all (the "near none" case of LB_PIPE_LOG=2).
  differs       the other file has entries with the same shaders; the closest one (fewest differing
                fields) is diffed field by field, and the entry is filed under the key parts that differ:
                render pass (samples, view mask, attachment formats and layouts), spec constants
                (scN, D3D9 names), and state (everything else: input layout, rasterizer, depth/stencil,
                blend, swizzle, binding mask). "dc-only" counts entries whose only differences are state
                Vulkan ignores (the rules of dxvkNormalizePipelineState), i.e. what twcclegg/dxvk-native#10
                removes. The fragment shader's output mask is not in the file, so "dc" here only uses the
                render pass's attachments, not the shader's outputs: it can undercount dc.

  --normalize   compare with that ignored state cleared first (same caveat: attachment-based only), to see
                how far two caches written before and after #10 agree.
  --show N      print up to N example entries per category (shader SHA-1 prefixes and differing fields,
                in LB_PIPE_LOG=2's "name old->new" style, A's value first).
  --top N       how many of the most common differing fields to list (default 12).

Format: twcclegg/dxvk-native's state cache (src/dxvk/dxvk_state_cache.cpp, dxvk_state_cache_types.h), versions
8-11. v11 is that fork's own (adds the render pass view mask); upstream DXVK's numbering differs. Versions
below 8 (fixed-size entries) are not supported. Apple builds use 13 spec constants; sc12 is shown as its two halves,
sc12:depthFilter (low 16 bits) and sc12:stereoLayers (bits 16+, the per-sampler two-layer bits of stereo).

The file holds shader SHA-1s and pipeline state only, but it derives from the game's shaders: read caches
where they are, don't copy or upload them. This script only reads the two files and prints counts.
"""

import argparse
import collections
import hashlib
import struct
import sys

STAGES = ["vs", "tcs", "tes", "gs", "fs", "cs"]
COMPUTE_BIT = 0x20
NUM_RT = 8
VK_FORMAT_UNDEFINED = 0
# D3D9SpecConstantId, as in describeMissFields
SC_NAMES = ["alphaOp", "samplerType", "fogEnabled", "vsFogMode", "psFogMode", "pointMode", "projection",
            "vsBools", "psBools", "fetch4", "depthMode", "vsScale", "depthFilter"]


def bits(v, lo, n):
    return (v >> lo) & ((1 << n) - 1)


class Reader:
    def __init__(self, data):
        self.d = data
        self.p = 0

    def take(self, n):
        if self.p + n > len(self.d):
            raise ValueError("entry data too short")
        b = self.d[self.p:self.p + n]
        self.p += n
        return b

    def u8(self):
        return self.take(1)[0]

    def u16(self):
        return struct.unpack("<H", self.take(2))[0]

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]


class Entry:
    """One parsed cache entry. fields: ordered (name, value, group); keys: stage -> sha1 hex."""

    __slots__ = ("keys", "compute", "rp", "fields", "dcinfo")

    def identity(self):
        return (self.compute, tuple(sorted(self.keys.items())), tuple(self.rp.items()),
                tuple((n, v) for n, v, _ in self.fields))

    def shader_set(self):
        return tuple(sorted(self.keys.items()))

    def short(self):
        return ".".join(self.keys.get(s, "-")[:8] for s in ("vs", "fs")) if not self.compute \
            else "cs." + self.keys.get("cs", "-")[:8]


def parse_entry(stage_mask, data, version):
    r = Reader(data)
    e = Entry()
    e.keys = {}
    for i, name in enumerate(STAGES):
        if stage_mask & (1 << i):
            r.u32()                      # VkShaderStageFlags m_type
            e.keys[name] = r.take(20).hex()
    e.compute = bool(stage_mask & COMPUTE_BIT)
    e.rp = collections.OrderedDict()
    f = []
    mask_len = 16 if version < 9 else 48   # DxvkBindingMaskV8 (128 bits) / DxvkBindingSet<384>
    if e.compute:
        f.append(("bindingMask", r.take(mask_len), "state"))
    else:
        e.rp["samples"] = r.u8()
        e.rp["depthFmt"] = r.u8()
        e.rp["depthLayout"] = r.u8()
        for i in range(NUM_RT):
            e.rp["rt%dFmt" % i] = r.u8()
            e.rp["rt%dLayout" % i] = r.u8()
        e.rp["viewMask"] = r.u8() if version >= 11 else 0

        f.append(("bindingMask", r.take(mask_len), "state"))
        ia = r.u16()
        f += [("topology", bits(ia, 0, 4), "state"), ("restart", bits(ia, 4, 1), "state"),
              ("patchVerts", bits(ia, 5, 6), "state")]
        n_attr, n_bind = r.u8(), r.u8()
        f += [("attrCount", n_attr, "state"), ("bindCount", n_bind, "state")]
        rs = r.u32()
        f += [("depthClip", bits(rs, 0, 1), "state"), ("depthBias", bits(rs, 1, 1), "state"),
              ("polygonMode", bits(rs, 2, 2), "state"), ("cullMode", bits(rs, 4, 2), "state"),
              ("frontFace", bits(rs, 6, 1), "state"), ("viewports", bits(rs, 7, 5), "state"),
              ("rsSamples", bits(rs, 12, 5), "state"), ("conservative", bits(rs, 17, 2), "state")]
        ms0, ms_mask = r.u16(), r.u16()
        f += [("msSamples", bits(ms0, 0, 5), "state"), ("alphaToCoverage", bits(ms0, 5, 1), "state"),
              ("sampleMask", ms_mask, "state")]
        ds = r.u16()
        f += [("depthTest", bits(ds, 0, 1), "state"), ("depthWrite", bits(ds, 1, 1), "state"),
              ("depthBounds", bits(ds, 2, 1), "state"), ("stencilTest", bits(ds, 3, 1), "state"),
              ("depthOp", bits(ds, 4, 3), "state")]
        om = r.u16()
        logic = [("logicOpEnable", bits(om, 0, 1), "state"), ("logicOp", bits(om, 1, 4), "state")]
        for face in ("front", "back"):
            s = r.u32()
            f += [(face + ".fail", bits(s, 0, 3), "state"), (face + ".pass", bits(s, 3, 3), "state"),
                  (face + ".depthFail", bits(s, 6, 3), "state"), (face + ".op", bits(s, 9, 3), "state"),
                  (face + ".readMask", bits(s, 16, 8), "state"), (face + ".writeMask", bits(s, 24, 8), "state")]
        f += logic
        swz = [r.u8() for _ in range(NUM_RT)]
        for i in range(NUM_RT):
            b = r.u32()
            f += [("rt%d.blend" % i, bits(b, 0, 1), "state"), ("rt%d.srcColor" % i, bits(b, 1, 5), "state"),
                  ("rt%d.dstColor" % i, bits(b, 6, 5), "state"), ("rt%d.colorOp" % i, bits(b, 11, 3), "state"),
                  ("rt%d.srcAlpha" % i, bits(b, 14, 5), "state"), ("rt%d.dstAlpha" % i, bits(b, 19, 5), "state"),
                  ("rt%d.alphaOp" % i, bits(b, 24, 3), "state"), ("rt%d.writeMask" % i, bits(b, 27, 4), "state"),
                  ("rt%d.swizzle" % i, swz[i], "state")]
        for i in range(n_attr):
            a = r.u32()
            f += [("attr%d.loc" % i, bits(a, 0, 5), "state"), ("attr%d.bind" % i, bits(a, 5, 5), "state"),
                  ("attr%d.fmt" % i, bits(a, 10, 7), "state"), ("attr%d.offset" % i, bits(a, 17, 11), "state")]
        for i in range(n_bind):
            b = r.u32()
            div = bits(b, 18, 14) if version >= 10 else r.u32()
            f += [("vb%d.bind" % i, bits(b, 0, 5), "state"), ("vb%d.stride" % i, bits(b, 5, 12), "state"),
                  ("vb%d.rate" % i, bits(b, 17, 1), "state"), ("vb%d.divisor" % i, div, "state")]
    # Spec constants: DXVK writes only the non-zero ones. All 32 mask slots are kept (absent = 0) so a constant
    # set in one entry and unset in another diffs as x->0. sc12 (Apple) is split: low 16 bits = depth-compare
    # PCF per sampler, high 16 bits = stereo layer bits (sampler reads layer gl_ViewIndex).
    sc_mask = r.u32()
    sc = [r.u32() if sc_mask & (1 << i) else 0 for i in range(32)]
    for i in range(32):
        if i == 12:
            f += [("sc12:depthFilter", sc[i] & 0xFFFF, "spec"), ("sc12:stereoLayers", sc[i] >> 16, "spec")]
        elif sc[i] or i < len(SC_NAMES):
            name = SC_NAMES[i] if i < len(SC_NAMES) else "sc%d" % i
            f.append(("sc%d:%s" % (i, name), sc[i], "spec"))
    if r.p != len(data):
        raise ValueError("entry has %d unread bytes" % (len(data) - r.p))
    e.fields = f
    return e


def read_cache(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) < 12 or data[:4] != b"DXVK":
        sys.exit("%s: not a DXVK state cache (bad magic)" % path)
    version, entry_size = struct.unpack_from("<II", data, 4)
    if version < 8:
        sys.exit("%s: state cache v%d has fixed-size entries; only v8-v11 are supported" % (path, version))
    if version > 11:
        print("warning: %s is v%d, newer than this reader knows (11); parsing as v11" % (path, version),
              file=sys.stderr)
    p = 12
    entries, invalid, bad_parse = [], 0, 0
    while p + 24 <= len(data):
        hdr, = struct.unpack_from("<I", data, p)
        stage_mask, size = hdr & 0xFF, hdr >> 8
        digest = data[p + 4:p + 24]
        blob = data[p + 24:p + 24 + size]
        p += 24 + size
        if len(blob) != size or size > 1024:
            invalid += 1          # truncated tail or a corrupt size: DXVK stops here too
            break
        if hashlib.sha1(blob).digest() != digest:
            invalid += 1
            continue
        try:
            entries.append(parse_entry(stage_mask, blob, version))
        except ValueError:
            bad_parse += 1
    trailing = len(data) - p if p < len(data) else 0
    return {"path": path, "version": version, "entries": entries, "invalid": invalid,
            "bad_parse": bad_parse, "trailing": trailing}


# --- state Vulkan ignores (dxvkNormalizePipelineState, attachment-based part only) ---------------------------

def dc_fields(rp, vals):
    """Names of fields whose value can't change the pipeline, given this entry's render pass and state."""
    dc = set()
    no_depth = rp.get("depthFmt", 0) == VK_FORMAT_UNDEFINED
    if no_depth:
        dc |= {"depthTest", "depthWrite", "depthBounds", "stencilTest", "depthOp"}
    if no_depth or not vals.get("depthTest"):
        dc |= {"depthWrite", "depthOp"}
    if no_depth or not vals.get("stencilTest"):
        for face in ("front", "back"):
            dc |= {face + s for s in (".fail", ".pass", ".depthFail", ".op", ".readMask", ".writeMask")}
    if not vals.get("logicOpEnable"):
        dc.add("logicOp")
    for i in range(NUM_RT):
        names = ["rt%d.%s" % (i, s) for s in ("srcColor", "dstColor", "colorOp", "srcAlpha", "dstAlpha", "alphaOp")]
        if rp.get("rt%dFmt" % i, 0) == VK_FORMAT_UNDEFINED:
            dc |= set(names) | {"rt%d.blend" % i, "rt%d.writeMask" % i, "rt%d.swizzle" % i}
        elif not vals.get("rt%d.blend" % i):
            dc |= set(names)
    return dc


def normalize(e):
    if e.compute:
        return
    vals = {n: v for n, v, _ in e.fields}
    dc = dc_fields(e.rp, vals)
    e.fields = [(n, 0 if n in dc else v, g) for n, v, g in e.fields]


def diff(a, b):
    """Fields that differ between entries a and b with the same shaders: list of (name, va, vb, group, dc)."""
    out = []
    for k in a.rp:
        if a.rp[k] != b.rp.get(k):
            out.append((k, a.rp[k], b.rp.get(k), "render pass", False))
    va = collections.OrderedDict((n, (v, g)) for n, v, g in a.fields)
    vb = collections.OrderedDict((n, (v, g)) for n, v, g in b.fields)
    dca = dc_fields(a.rp, {n: v for n, (v, _) in va.items()})
    dcb = dc_fields(b.rp, {n: v for n, (v, _) in vb.items()})
    for n in list(va) + [n for n in vb if n not in va]:
        x, gx = va.get(n, (None, None))
        y, gy = vb.get(n, (None, None))
        if x != y:
            out.append((n, x, y, gx or gy, n in dca and n in dcb))
    return out


def fmt_val(v):
    if v is None:
        return "-"
    if isinstance(v, bytes):
        return v.hex()[:8] + ".."
    return "%x" % v


def summarize(name, only, other, args):
    by_set = collections.defaultdict(list)
    vs_seen, fs_seen = set(), set()
    for e in other["entries"]:
        by_set[e.shader_set()].append(e)
        vs_seen.add(e.keys.get("vs"))
        fs_seen.add(e.keys.get("fs"))

    cats = collections.Counter()
    examples = collections.defaultdict(list)
    field_counts = collections.Counter()
    dc_only = 0
    for e in only:
        same = by_set.get(e.shader_set())
        if not same:
            if e.compute:
                cat = "new shaders: compute"
            else:
                kv, kf = e.keys.get("vs") in vs_seen, e.keys.get("fs") in fs_seen
                cat = {(False, False): "new shaders: vs and fs both unseen",
                       (False, True): "new shaders: vs unseen",
                       (True, False): "new shaders: fs unseen",
                       (True, True): "new shaders: both seen, never paired"}[(kv, kf)]
            cats[cat] += 1
            if len(examples[cat]) < args.show:
                examples[cat].append(e.short())
            continue
        best = min((diff(e, o) for o in same), key=len)
        groups = sorted({g for _, _, _, g, _ in best})
        cat = "differs: " + " + ".join(groups) if groups else "differs: (nothing parsed differs)"
        cats[cat] += 1
        if best and all(dc for *_, dc in best):
            dc_only += 1
        for n, *_ in best:
            field_counts[n] += 1
        if len(examples[cat]) < args.show:
            examples[cat].append(e.short() + "  " + ",".join(
                "%s %s->%s%s" % (n, fmt_val(x), fmt_val(y), " dc" if dc else "") for n, x, y, _, dc in best))

    print("\nOnly in %s: %d" % (name, len(only)))
    for cat, n in sorted(cats.items(), key=lambda kv: (-kv[1], kv[0])):
        print("  %-42s %5d" % (cat, n))
        for ex in examples[cat]:
            print("      " + ex)
    if field_counts:
        print("  dc-only (differ only in state Vulkan ignores): %d" % dc_only)
        print("  most common differing fields (vs the closest entry with the same shaders):")
        for n, c in field_counts.most_common(args.top):
            print("      %-22s %5d" % (n, c))


def main():
    ap = argparse.ArgumentParser(description="Compare two DXVK state cache files (see the docstring).")
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--normalize", action="store_true", help="clear state Vulkan ignores before comparing")
    ap.add_argument("--show", type=int, default=0, metavar="N", help="example entries per category")
    ap.add_argument("--top", type=int, default=12, metavar="N", help="differing fields to list")
    args = ap.parse_args()

    caches = [read_cache(args.a), read_cache(args.b)]
    for label, c in zip("AB", caches):
        if args.normalize:
            for e in c["entries"]:
                normalize(e)
        ids = collections.Counter(e.identity() for e in c["entries"])
        c["ids"] = ids
        n_cs = sum(1 for e in c["entries"] if e.compute)
        vm = collections.Counter(e.rp.get("viewMask", 0) for e in c["entries"] if not e.compute)
        print("%s: %s" % (label, c["path"]))
        print("   v%d, %d entries read (%d graphics, %d compute), %d unique; %d invalid, %d unparsed%s" % (
            c["version"], len(c["entries"]), len(c["entries"]) - n_cs, n_cs, len(ids), c["invalid"],
            c["bad_parse"], ", %d trailing bytes" % c["trailing"] if c["trailing"] else ""))
        print("   graphics by view mask: " + ", ".join("%x: %d" % kv for kv in sorted(vm.items())))

    a, b = caches
    both = set(a["ids"]) & set(b["ids"])
    only_a = [e for e in a["entries"] if e.identity() not in b["ids"]]
    only_b = [e for e in b["entries"] if e.identity() not in a["ids"]]
    # one example per identity
    only_a = list({e.identity(): e for e in only_a}.values())
    only_b = list({e.identity(): e for e in only_b}.values())
    sa = {e.shader_set() for e in a["entries"]}
    sb = {e.shader_set() for e in b["entries"]}

    print("\n%sIn both: %d   only in A: %d   only in B: %d" % (
        "(normalized) " if args.normalize else "", len(both), len(only_a), len(only_b)))
    print("Shader sets: A %d, B %d, in both %d" % (len(sa), len(sb), len(sa & sb)))
    if a["ids"]:
        print("Share of A found in B: %.1f%%   share of B found in A: %.1f%%" % (
            100.0 * len(both) / len(a["ids"]), 100.0 * len(both) / max(1, len(b["ids"]))))
    summarize("A", only_a, b, args)
    summarize("B", only_b, a, args)


if __name__ == "__main__":
    main()
