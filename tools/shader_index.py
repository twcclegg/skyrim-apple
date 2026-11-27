#!/usr/bin/env python3
"""shader_index.py: index the D3D9 shaders in a Skyrim (2011, 1.9.32) install by DXVK shader key.

    python3 -I shader_index.py index    GAME_DIR [--json OUT.json]
    python3 -I shader_index.py verify   GAME_DIR|INDEX.json CACHE.dxvk-cache... [--min-entries PCT] [--show]
    python3 -I shader_index.py siblings GAME_DIR|INDEX.json EARLIER.dxvk-cache LATER.dxvk-cache
    python3 -I shader_index.py dxso     GAME_DIR CACHE.dxvk-cache... --out FILE.dxso-cache [--all]

DXVK names a D3D9 shader by the SHA-1 of its bytecode, version token through END token. The game never
compiles shaders: it hands DXVK bytecode it reads from three places, and this tool reads the same three.

  Data/Skyrim - Shaders.bsa, Data/Update.bsa
      shadersfx\\*.fxp packages. A package is a run of records: tag BSVS or BSPS, u32 id, u32 byte length,
      the bytecode, then a fixed trailer (36 bytes after a vertex shader, 78 after a pixel shader). Lighting
      packs (shadersfx\\lighting\\059\\p<flags>.fxp) start with BSFX, u32, u32 count; the others have 8-byte
      group headers between runs of records. In a lighting pack, id XOR flags is the variant: bits 4-6 the
      number of point lights, bits 7-9 the number of shadow-casting lights, bits 14-15 a class.
  TESV.exe
      Shader model 3 bytecode embedded in the executable's data (the interface renderer's shaders). Found by
      scanning for a version token and walking the instructions to END, so a few entries may be data that
      only looks like a shader; such a key is never asked for.

index     prints counts; --json writes the table: key, stage, length and where the bytes are (source file and
          absolute offset), plus pack and variants for lighting shaders. Hashes, offsets and names only.
verify    reads state caches (twcclegg/dxvk-native format, v8-v11) and reports how many of their shader keys
          the table has, and how many entries have every shader in the table, i.e. could be compiled ahead
          with no bytecode cache. Keys it can't find are listed. DXVK's fixed-function shaders are named by a
          hash of a state key, not of bytecode, so those are never in the game files.
siblings  the light-count test: for every entry the later cache has and the earlier lacks whose pixel shader
          the earlier never saw, is there an earlier entry with the same vertex shader, a pixel shader of the
          same lighting pack, and the same state apart from the binding mask (and, counted separately, the
          spec constants)?
dxso      writes the shaders that the given state caches name (or --all) as a bytecode cache in the format of
          twcclegg/dxvk-native#8 (<exe>.dxso-cache), which that build's startup path creates shaders from.
          THE OUTPUT CONTAINS THE GAME'S SHADER CODE: it is refused inside a git work tree. Keep it private.

Exit status: 0 success; 1 a file can't be read or parsed, or coverage is under --min-entries; 2 usage.
"""

import argparse
import collections
import hashlib
import json
import os
import struct
import sys
import zlib

STAGE_BITS = {"vs": 0x01, "ps": 0x10}           # VK_SHADER_STAGE_VERTEX_BIT / FRAGMENT_BIT, also the cache's stage mask
VERSION_HIGH = {"vs": 0xFFFE, "ps": 0xFFFF}
TRAILER = {"vs": 36, "ps": 78}
ARCHIVES = ["Data/Skyrim - Shaders.bsa", "Data/Update.bsa"]   # load order: a later file of the same name wins
EXECUTABLE = "TESV.exe"
MAX_SHADER_BYTES = 1 << 20                      # MaxCachedBytecodeLength in dxvk-native#8


class FormatError(Exception):
    pass


def dxso_length(buf, start, limit=None):
    """Bytes from the version token at `start` through the END token, as DxsoModule::runAnalyzer counts them.
    None if there is no END before `limit`, an opcode no shader has, or a shader model below 2 (whose
    instructions don't carry their length)."""
    limit = len(buf) if limit is None else min(limit, len(buf))
    if start + 4 > limit:
        return None
    version, = struct.unpack_from("<I", buf, start)
    if version >> 16 not in (0xFFFE, 0xFFFF) or not 2 <= (version >> 8) & 0xFF <= 3:
        return None
    p = start + 4
    while p + 4 <= limit:
        token, = struct.unpack_from("<I", buf, p)
        op = token & 0xFFFF
        p += 4
        if op == 0xFFFF:
            return p - start
        if op == 0xFFFE:                         # comment: length in bits 16-30
            p += 4 * ((token >> 16) & 0x7FFF)
        elif op <= 0x60 or op == 0xFFFD:         # instruction: length in bits 24-27
            p += 4 * ((token >> 24) & 0xF)
        else:
            return None
    return None


# --- Bethesda archive, version 104 ---------------------------------------------------------------------------

def read_bsa(path):
    """[(name, offset, size, compressed, name_prefixed)] for every file; name is folder\\file, lower case."""
    with open(path, "rb") as fh:
        head = fh.read(36)
        if len(head) < 36 or head[:4] != b"BSA\0":
            raise FormatError("%s: not a BSA archive" % path)
        version, _, flags, folders, files, _, names_len, _ = struct.unpack_from("<8I", head, 4)
        if version != 104:
            raise FormatError("%s: BSA version %d, this reads 104" % (path, version))
        if not flags & 1 or not flags & 2:
            raise FormatError("%s: archive has no folder or file names" % path)
        folder_records = [struct.unpack("<QII", fh.read(16)) for _ in range(folders)]
        out = []
        for _, count, _ in folder_records:
            length = fh.read(1)[0]
            folder = fh.read(length).rstrip(b"\0").decode("cp1252").lower()
            for _ in range(count):
                _, size, offset = struct.unpack("<QII", fh.read(16))
                out.append([folder, offset, size & 0x3FFFFFFF, bool(flags & 4) != bool(size & 0x40000000)])
        names = fh.read(names_len).split(b"\0")
        if len(out) != files or len(names) < files:
            raise FormatError("%s: file table doesn't match the header" % path)
        return [(folder + "\\" + names[i].decode("cp1252").lower(), offset, size, packed, bool(flags & 0x100))
                for i, (folder, offset, size, packed) in enumerate(out)]


def bsa_file(fh, record):
    """(bytes, absolute offset of those bytes or None when they are compressed)."""
    _, offset, size, packed, prefixed = record
    fh.seek(offset)
    if prefixed:
        skip = fh.read(1)[0] + 1
        fh.seek(skip - 1, os.SEEK_CUR)
        offset, size = offset + skip, size - skip
    if packed:
        fh.seek(4, os.SEEK_CUR)                  # uncompressed size
        return zlib.decompress(fh.read(size - 4)), None
    data = fh.read(size)
    if len(data) != size:
        raise FormatError("archive is truncated")
    return data, offset


# --- shader packages -----------------------------------------------------------------------------------------

def walk_fxp(data):
    """[(stage, id, offset, length)] for every record of one package; raises FormatError on anything else."""
    tags = {b"BSVS": "vs", b"BSPS": "ps"}
    p = 12 if data[:4] == b"BSFX" else 0
    out = []
    while p < len(data):
        stage = tags.get(data[p:p + 4])
        if stage is None:
            if data[p + 8:p + 12] in tags or p + 8 == len(data):
                p += 8                           # group header
                continue
            raise FormatError("no shader record at offset %d" % p)
        if p + 12 > len(data):
            raise FormatError("record header cut short at offset %d" % p)
        ident, length = struct.unpack_from("<II", data, p + 4)
        start = p + 12
        if start + length > len(data) or not 8 <= length <= MAX_SHADER_BYTES:
            raise FormatError("record at offset %d has a bad length" % p)
        if struct.unpack_from("<I", data, start)[0] >> 16 != VERSION_HIGH[stage]:
            raise FormatError("record at offset %d: bytecode is not a %s" % (p, stage))
        if dxso_length(data, start, start + length) != length:
            raise FormatError("record at offset %d: bytecode doesn't end where the record says" % p)
        out.append((stage, ident, start, length))
        p = start + length + TRAILER[stage]
    if p != len(data):
        raise FormatError("last record's trailer runs past the end")
    return out


def scan_executable(data):
    """[(stage, offset, length)]: shader model 3 bytecode at 4-byte alignment that starts with a comment (the
    compiler's constant table) and walks cleanly to END."""
    out = []
    for stage, token in (("vs", b"\x00\x03\xfe\xff"), ("ps", b"\x00\x03\xff\xff")):
        p = data.find(token)
        while p >= 0:
            if p % 4 == 0:
                length = dxso_length(data, p, p + MAX_SHADER_BYTES)
                if length and length >= 16 and data[p + 4:p + 6] == b"\xfe\xff":
                    out.append((stage, p, length))
            p = data.find(token, p + 1)
    return sorted(out, key=lambda s: s[1])


# --- the table -----------------------------------------------------------------------------------------------

def lighting_pack(name):
    """(pack name, flag word) for shadersfx\\lighting\\NNN\\p<hex>.fxp, else None."""
    parts = name.split("\\")
    if len(parts) == 4 and parts[1] == "lighting" and parts[3].startswith("p") and parts[3].endswith(".fxp"):
        try:
            return parts[3][:-4], int(parts[3][1:-4], 16)
        except ValueError:
            return None
    return None


def build_index(game_dir, archives=ARCHIVES, executable=EXECUTABLE):
    """The table, as the dict --json writes. Missing sources are skipped and named in "missing"."""
    sources, records, missing = [], [], []
    for relative in archives:
        path = os.path.join(game_dir, relative)
        if not os.path.isfile(path):
            missing.append(relative)
            continue
        index = len(sources)
        sources.append({"path": relative, "kind": "bsa", "size": os.path.getsize(path)})
        with open(path, "rb") as fh:
            for record in read_bsa(path):
                name = record[0]
                if not (name.startswith("shadersfx\\") and name.endswith(".fxp")):
                    continue
                data, base = bsa_file(fh, record)
                try:
                    found = walk_fxp(data)
                except FormatError as error:
                    raise FormatError("%s: %s: %s" % (relative, name, error))
                for stage, ident, start, length in found:
                    records.append((stage, hashlib.sha1(data[start:start + length]).hexdigest(), length, index,
                                    None if base is None else base + start, name, ident, start))
    if executable:
        path = os.path.join(game_dir, executable)
        if os.path.isfile(path):
            index = len(sources)
            sources.append({"path": executable, "kind": "exe", "size": os.path.getsize(path)})
            with open(path, "rb") as fh:
                data = fh.read()
            for stage, start, length in scan_executable(data):
                records.append((stage, hashlib.sha1(data[start:start + length]).hexdigest(), length, index,
                                start, None, None, start))
        else:
            missing.append(executable)

    # A file in a later archive replaces the same file in an earlier one
    last = {}
    for record in records:
        if record[5] is not None:
            last[record[5]] = max(last.get(record[5], 0), record[3])
    shaders = collections.OrderedDict()
    for stage, key, length, source, offset, name, ident, inner in records:
        live = name is None or last[name] == source
        entry = shaders.get((stage, key))
        if entry is None:
            entry = shaders[(stage, key)] = {"stage": stage, "key": key, "length": length, "records": 0,
                                             "live": False}
        entry["records"] += 1
        if live and not entry["live"] or "source" not in entry:
            entry.update(source=source, offset=offset, file=name, inner=inner, live=live)
        pack = lighting_pack(name) if name and live else None
        if pack:
            entry.setdefault("packs", {}).setdefault(pack[0], []).append(ident ^ pack[1])
    counts = collections.OrderedDict()
    for source in sources + [{"path": "all"}, {"path": "live"}]:
        row = counts[source["path"]] = collections.OrderedDict()
        index = sources.index(source) if source in sources else None
        for stage in ("vs", "ps"):
            if index is not None:
                mine = [r for r in records if r[0] == stage and r[3] == index]
                row[stage] = {"records": len(mine), "unique": len({r[1] for r in mine})}
            else:
                mine = [e for e in shaders.values() if e["stage"] == stage and (e["live"] or source["path"] == "all")]
                row[stage] = {"records": sum(e["records"] for e in mine), "unique": len(mine)}
    return {"format": 1, "sources": sources, "missing": missing, "counts": counts,
            "shaders": list(shaders.values())}


def load_index(path):
    """A table from a game folder or from a file written by index --json."""
    if os.path.isdir(path):
        return build_index(path)
    try:
        with open(path, "r", encoding="utf-8") as fh:
            table = json.load(fh)
    except ValueError:
        table = None
    if not isinstance(table, dict) or table.get("format") != 1 or "shaders" not in table:
        raise FormatError("%s: not a game folder or a shader index" % path)
    return table


# --- DXVK state cache ----------------------------------------------------------------------------------------

def read_state_cache(path):
    """(version, [(shaders {stage: key}, state bytes after the shader keys)], entries dropped as invalid)."""
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) < 12 or data[:4] != b"DXVK":
        raise FormatError("%s: not a DXVK state cache" % path)
    version, = struct.unpack_from("<I", data, 4)
    if not 8 <= version <= 11:
        raise FormatError("%s: state cache v%d; this reads v8-v11" % (path, version))
    entries, invalid, p = [], 0, 12
    while p + 24 <= len(data):
        header, = struct.unpack_from("<I", data, p)
        mask, size = header & 0xFF, header >> 8
        blob = data[p + 24:p + 24 + size]
        if len(blob) != size:
            invalid += 1
            break
        if hashlib.sha1(blob).digest() != data[p + 4:p + 24]:
            invalid += 1
        else:
            shaders, q = {}, 0
            for bit, stage in ((0x01, "vs"), (0x02, "tcs"), (0x04, "tes"), (0x08, "gs"), (0x10, "ps"), (0x20, "cs")):
                if mask & bit:
                    shaders[stage] = blob[q + 4:q + 24].hex()
                    q += 24
            entries.append((shaders, blob[q:]))
        p += 24 + size
    return version, entries, invalid


def split_state(state, version):
    """(state with the binding mask zeroed, the same with the spec constants cut off too), graphics entries."""
    mask_at = 19 + (1 if version >= 11 else 0)   # samples, depth format + layout, 8 x (format, layout), view mask
    mask_len = 48 if version >= 9 else 16
    blanked = state[:mask_at] + bytes(mask_len) + state[mask_at + mask_len:]
    counts_at = mask_at + mask_len + 2           # after the input assembly word
    attributes, bindings = state[counts_at], state[counts_at + 1]
    spec_at = counts_at + 2 + 60 + 4 * attributes + (4 if version >= 10 else 8) * bindings
    return blanked, blanked[:spec_at]


# --- commands ------------------------------------------------------------------------------------------------

def cmd_index(args):
    table = build_index(args.game)
    for name in table["missing"]:
        print("not found: %s" % name)
    print("%-28s %10s %10s %10s %10s" % ("source", "vs records", "vs unique", "ps records", "ps unique"))
    for name, row in table["counts"].items():
        print("%-28s %10d %10d %10d %10d" % (name, row["vs"]["records"], row["vs"]["unique"],
                                             row["ps"]["records"], row["ps"]["unique"]))
    print('"live" leaves out files that a later archive replaces')
    lighting = [e for e in table["shaders"] if e.get("packs")]
    print("lighting packs: %d, %d unique pixel shaders" % (len({p for e in lighting for p in e["packs"]}),
                                                           len(lighting)))
    lengths = sorted(e["length"] for e in table["shaders"] if e["stage"] == "ps")
    if lengths:
        print("pixel shader bytes: median %d, max %d; all shaders: %d bytes" % (
            lengths[len(lengths) // 2], lengths[-1], sum(e["length"] for e in table["shaders"])))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(table, fh, indent=0)
        print("wrote %s (%d shaders)" % (args.json, len(table["shaders"])))
    return 1 if not table["shaders"] else 0


def coverage(table, caches):
    """Per cache and for all together: keys found per stage, entries whose shaders are all found, misses."""
    known = {(e["stage"], e["key"]): e for e in table["shaders"]}
    kinds = [s["kind"] for s in table["sources"]]
    rows, union_keys, union_entries = [], set(), {}
    for path in caches:
        version, entries, invalid = read_state_cache(path)
        keys = {(stage, key) for shaders, _ in entries for stage, key in shaders.items()}
        union_keys |= keys
        for shaders, state in entries:
            union_entries[(tuple(sorted(shaders.items())), state)] = shaders
        rows.append((path, version, invalid, keys, [shaders for shaders, _ in entries]))
    if len(caches) > 1:
        rows.append(("all %d caches" % len(caches), None, 0, union_keys, list(union_entries.values())))
    out = []
    for path, version, invalid, keys, entries in rows:
        row = {"path": path, "version": version, "invalid": invalid, "entries": len(entries),
               "covered": sum(all((s, k) in known for s, k in e.items()) for e in entries), "stages": {}}
        for stage in sorted({s for s, _ in keys}):
            mine = sorted(k for s, k in keys if s == stage)
            found = [k for k in mine if (stage, k) in known]
            row["stages"][stage] = {
                "keys": len(mine), "found": len(found),
                "by_source": dict(collections.Counter(kinds[known[(stage, k)]["source"]] for k in found)),
                "missing": {k: sum(e.get(stage) == k for e in entries) for k in mine if (stage, k) not in known}}
        out.append(row)
    return out


def cmd_verify(args):
    table = load_index(args.index)
    worst = 100.0
    for row in coverage(table, args.caches):
        print(row["path"] + ("" if row["version"] is None else "  (v%d, %d invalid entries skipped)"
                             % (row["version"], row["invalid"])))
        for stage, s in row["stages"].items():
            where = ", ".join("%d from %s" % (n, kind) for kind, n in sorted(s["by_source"].items()))
            print("  %-3s keys %4d, in the table %4d (%s)" % (stage, s["keys"], s["found"], where or "none"))
            for key, uses in s["missing"].items():
                print("      not in the game files: %s, used by %d entries" % (key if args.show else key[:8], uses))
        share = 100.0 * row["covered"] / row["entries"] if row["entries"] else 0.0
        worst = min(worst, share)
        print("  entries %d, every shader in the table for %d (%.1f%%)" % (row["entries"], row["covered"], share))
    if args.min_entries is not None and worst < args.min_entries:
        print("coverage %.1f%% is under --min-entries %.1f" % (worst, args.min_entries))
        return 1
    return 0


SIBLING_RANKS = ["sibling, same state apart from the binding mask",
                 "sibling, same state apart from the binding mask and spec constants",
                 "sibling, other state differs",
                 "no earlier entry with this vertex shader and pack"]


def cmd_siblings(args):
    table = load_index(args.index)
    packs = {e["key"]: e["packs"] for e in table["shaders"] if e.get("packs")}
    version_a, earlier, _ = read_state_cache(args.earlier)
    version_b, later, _ = read_state_cache(args.later)
    if version_a != version_b:
        raise FormatError("the two caches differ in version (v%d, v%d)" % (version_a, version_b))
    have = {(tuple(sorted(s.items())), state) for s, state in earlier}
    seen_ps = {s.get("ps") for s, _ in earlier}
    by_group = collections.defaultdict(list)       # (vertex shader, pack) -> [(variants, masked, masked + no spec)]
    for shaders, state in earlier:
        for pack, variants in packs.get(shaders.get("ps"), {}).items():
            by_group[(shaders.get("vs"), pack)].append((variants,) + split_state(state, version_a))
    new = [(s, state) for s, state in later if (tuple(sorted(s.items())), state) not in have]
    new_ps = [(s, state) for s, state in new if "ps" in s and s["ps"] not in seen_ps]
    lighting = [(s, state) for s, state in new_ps if s["ps"] in packs]
    result, steps = collections.Counter(), collections.Counter()
    for shaders, state in lighting:
        masked, loose = split_state(state, version_b)
        best, step = len(SIBLING_RANKS) - 1, None
        for pack, variants in packs[shaders["ps"]].items():
            for other, other_masked, other_loose in by_group.get((shaders["vs"], pack), ()):
                rank = 0 if other_masked == masked else 1 if other_loose == loose else 2
                distance = min((abs((v >> 4 & 7) - (o >> 4 & 7)), abs((v >> 7 & 7) - (o >> 7 & 7)))
                               for v in variants for o in other)
                if (rank, distance) < (best, step or (8, 8)):
                    best, step = rank, distance
        result[SIBLING_RANKS[best]] += 1
        if best < 2:
            steps[step] += 1
    print("earlier: %d entries, later: %d; in later only: %d" % (len(earlier), len(later), len(new)))
    print("  with a pixel shader the earlier cache never saw: %d (%d lighting, %d other)"
          % (len(new_ps), len(lighting), len(new_ps) - len(lighting)))
    for name, count in sorted(result.items()):
        print("    %4d  %s" % (count, name))
    for (lights, shadows), count in sorted(steps.items()):
        print("          nearest sibling %d lights and %d shadow lights away: %d" % (lights, shadows, count))
    passed = sum(n for name, n in result.items() if "same state" in name)
    if lighting:
        print("light-count siblings: %d of %d new lighting entries (%.0f%%), %d of all %d new-shader entries (%.0f%%)"
              % (passed, len(lighting), 100.0 * passed / len(lighting), passed, len(new_ps),
                 100.0 * passed / len(new_ps)))
    return 0


def inside_git_tree(path):
    folder = os.path.dirname(os.path.realpath(path))
    while True:
        if os.path.exists(os.path.join(folder, ".git")):
            return True
        parent = os.path.dirname(folder)
        if parent == folder:
            return False
        folder = parent


def cmd_dxso(args):
    if inside_git_tree(args.out):
        print("refusing to write the game's shader code inside a git work tree: %s" % args.out)
        return 2
    table = build_index(args.game)
    wanted = None
    if not args.all:
        if not args.caches:
            print("name the state caches whose shaders to write, or pass --all")
            return 2
        wanted = {(stage, key) for path in args.caches
                  for shaders, _ in read_state_cache(path)[1] for stage, key in shaders.items()}
    handles, archives, written = {}, {}, 0
    with open(os.open(args.out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "wb") as out:
        out.write(b"DXSO" + struct.pack("<I", 1))
        for entry in table["shaders"]:
            if wanted is not None and (entry["stage"], entry["key"]) not in wanted:
                continue
            source = entry["source"]
            path = os.path.join(args.game, table["sources"][source]["path"])
            if source not in handles:
                handles[source] = open(path, "rb")
            fh = handles[source]
            if entry["offset"] is not None:
                fh.seek(entry["offset"])
                code = fh.read(entry["length"])
            else:                                # compressed archive member
                if source not in archives:
                    archives[source] = {r[0]: r for r in read_bsa(path)}
                code = bsa_file(fh, archives[source][entry["file"]])[0][entry["inner"]:entry["inner"] + entry["length"]]
            if hashlib.sha1(code).hexdigest() != entry["key"]:
                raise FormatError("%s changed while reading" % path)
            out.write(struct.pack("<II", STAGE_BITS[entry["stage"]], len(code)) + bytes.fromhex(entry["key"]) + code)
            written += 1
    for fh in handles.values():
        fh.close()
    missing = 0 if wanted is None else len(wanted) - written
    print("wrote %d shaders to %s%s" % (written, args.out, "" if not missing else
                                        "; %d keys of the caches are not in the game files" % missing))
    print("this file holds the game's shader code: keep it out of repositories, logs and uploads")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    index = commands.add_parser("index")
    index.add_argument("game")
    index.add_argument("--json")
    verify = commands.add_parser("verify")
    verify.add_argument("index")
    verify.add_argument("caches", nargs="+")
    verify.add_argument("--min-entries", type=float)
    verify.add_argument("--show", action="store_true", help="print missing keys in full")
    siblings = commands.add_parser("siblings")
    siblings.add_argument("index")
    siblings.add_argument("earlier")
    siblings.add_argument("later")
    dxso = commands.add_parser("dxso")
    dxso.add_argument("game")
    dxso.add_argument("caches", nargs="*")
    dxso.add_argument("--out", required=True)
    dxso.add_argument("--all", action="store_true")
    args = parser.parse_args(argv)
    try:
        return {"index": cmd_index, "verify": cmd_verify, "siblings": cmd_siblings, "dxso": cmd_dxso}[args.command](args)
    except (FormatError, OSError, struct.error, zlib.error, IndexError) as error:
        print("error: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
