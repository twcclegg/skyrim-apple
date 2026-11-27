#!/usr/bin/env python3
"""Tests for shader_index.py on made-up archives, packages, an executable and state caches. No game data.

    python3 -I tools/test_shader_index.py
"""
import contextlib
import hashlib
import io
import os
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shader_index as si  # noqa: E402


def shader(stage, seed, words=3):
    """Made-up shader model 3 bytecode: version, a comment, `words` two-operand instructions, END."""
    out = struct.pack("<I", (si.VERSION_HIGH[stage] << 16) | 0x0300)
    out += struct.pack("<I", 0xFFFE | (2 << 16)) + b"CTAB" + struct.pack("<I", seed)
    for i in range(words):
        out += struct.pack("<III", 0x01 | (2 << 24), seed + i, 0x0FFF0000)   # an operand may look like END's low half
    return out + struct.pack("<I", 0x0000FFFF)


def key(code):
    return hashlib.sha1(code).hexdigest()


def record(stage, ident, code):
    return (b"BSVS" if stage == "vs" else b"BSPS") + struct.pack("<II", ident, len(code)) + code \
        + bytes(si.TRAILER[stage])


def bsa(files, compress=False):
    """A version 104 archive holding {folder\\name: bytes}."""
    folders = {}
    for path, data in files.items():
        folder, name = path.rsplit("\\", 1)
        folders.setdefault(folder, []).append((name, data))
    names = b"".join(name.encode() + b"\0" for entries in folders.values() for name, _ in entries)
    count = sum(len(entries) for entries in folders.values())
    offset = 36 + 16 * len(folders) + sum(len(f) + 2 + 16 * len(e) for f, e in folders.items()) + len(names)
    head = b"BSA\0" + struct.pack("<8I", 104, 36, 3 | (4 if compress else 0), len(folders), count,
                                  sum(len(f) + 1 for f in folders), len(names), 0)
    head += b"".join(struct.pack("<QII", 0, len(entries), 0) for entries in folders.values())
    body = b""
    for folder, entries in folders.items():
        head += bytes([len(folder) + 1]) + folder.encode() + b"\0"
        for _, data in entries:
            stored = struct.pack("<I", len(data)) + zlib.compress(data) if compress else data
            head += struct.pack("<QII", 0, len(stored), offset + len(body))
            body += stored
    return head + names + body


def entry(version, vs, ps, mask=b"\1", fill=7, spec=(), bad_hash=False):
    """One graphics entry of a state cache: shader keys, render pass, binding mask, state, spec constants."""
    blob = b""
    for bits, digest in ((1, vs), (16, ps)):
        blob += struct.pack("<I", bits) + bytes.fromhex(digest)
    blob += bytes([1, 0, 0]) + bytes(16) + (b"\3" if version >= 11 else b"")
    blob += mask.ljust(48, b"\0") + struct.pack("<HBB", 3, 1, 1) + bytes([fill]) * 60
    blob += struct.pack("<II", 0x1234, 0x5678)
    blob += struct.pack("<I", sum(1 << i for i, _ in spec)) + b"".join(struct.pack("<I", v) for _, v in spec)
    digest = hashlib.sha1(blob).digest()
    if bad_hash:
        digest = bytes(20)
    return struct.pack("<I", 0x11 | (len(blob) << 8)) + digest + blob


def cache(version, entries):
    return b"DXVK" + struct.pack("<II", version, 0) + b"".join(entries)


def run(*argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
        status = si.main(list(argv))
    return status, out.getvalue()


class ShaderIndexTest(unittest.TestCase):
    PACK = 0x2600001

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="shader-index-test-")
        self.root = self.temporary.name
        self.game = os.path.join(self.root, "game")
        os.makedirs(os.path.join(self.game, "Data"))
        self.vs = [shader("vs", 100 + i) for i in range(3)]
        self.main_ps = [shader("ps", 200 + i, words=i + 1) for i in range(3)]
        # variant -> pixel shader of the pack; 0x8080 shares the bytes of 0x0000, as in the real packs
        self.variants = {0x00: shader("ps", 300), 0x10: shader("ps", 301), 0x20: shader("ps", 302),
                         0x8090: shader("ps", 303), 0x8080: shader("ps", 300)}
        self.old_variant = shader("ps", 399)
        main = struct.pack("<II", 2, 2) + record("vs", 0, self.vs[0]) + record("vs", 1, self.vs[0]) \
            + record("vs", 0, self.vs[1]) + struct.pack("<II", 5, 9) + record("ps", 0, self.main_ps[0]) \
            + record("ps", 1, self.main_ps[1]) + struct.pack("<II", 1, 1)
        pack = b"BSFX" + struct.pack("<II", 0, len(self.variants)) \
            + b"".join(record("ps", self.PACK ^ v, code) for v, code in self.variants.items())
        old_pack = b"BSFX" + struct.pack("<II", 0, 1) + record("ps", self.PACK, self.old_variant)
        other = b"BSFX" + struct.pack("<II", 0, 1) + record("ps", 0x40, self.main_ps[2])
        self.write("Data/Skyrim - Shaders.bsa", bsa({
            "shadersfx\\shaders001.fxp": main,
            "shadersfx\\lighting\\059\\p2600001.fxp": old_pack,
            "shadersfx\\lighting\\059\\p40.fxp": other,
            "textures\\not a shader.dds": b"BSVS but not in shadersfx",
        }))
        self.write("Data/Update.bsa", bsa({"shadersfx\\lighting\\059\\p2600001.fxp": pack,
                                           "meshes\\x.nif": b"\0" * 9}, compress=True))
        self.exe_vs, self.exe_ps = self.vs[2], shader("ps", 400)
        no_table = struct.pack("<II", 0xFFFF0300, 0x0000FFFF) * 2
        self.write("TESV.exe", b"MZ" + bytes(98) + self.exe_vs + bytes(8) + self.exe_ps + bytes(3)
                   + shader("ps", 401) + bytes(1) + no_table + struct.pack("<I", 0xFFFE0300) + b"\xfe\xff" + bytes(2))
        self.fixed = (hashlib.sha1(b"fixed function vs").hexdigest(), hashlib.sha1(b"fixed function ps").hexdigest())

    def tearDown(self):
        self.temporary.cleanup()

    def write(self, relative, data):
        path = os.path.join(self.root if relative.endswith("-cache") else self.game, relative)
        with open(path, "wb") as fh:
            fh.write(data)
        return path

    def test_bytecode_length(self):
        code = self.vs[0]
        self.assertEqual(si.dxso_length(code + b"junk" * 4, 0), len(code))
        self.assertEqual(si.dxso_length(b"\0" * 8 + code, 8), len(code))
        self.assertIsNone(si.dxso_length(code[:-4], 0))                                  # no END
        self.assertIsNone(si.dxso_length(struct.pack("<II", 0xFFFE0101, 0xFFFF), 0))     # shader model 1
        self.assertIsNone(si.dxso_length(struct.pack("<III", 0xFFFF0300, 0x1234, 0xFFFF), 0))  # no such opcode

    def test_index_counts_keys_and_locations(self):
        table = si.build_index(self.game)
        self.assertEqual(table["missing"], [])
        counts = table["counts"]
        self.assertEqual(counts["Data/Skyrim - Shaders.bsa"]["vs"], {"records": 3, "unique": 2})
        self.assertEqual(counts["Data/Skyrim - Shaders.bsa"]["ps"], {"records": 4, "unique": 4})
        self.assertEqual(counts["Data/Update.bsa"]["ps"], {"records": 5, "unique": 4})
        self.assertEqual(counts["TESV.exe"]["vs"], {"records": 1, "unique": 1})
        self.assertEqual(counts["TESV.exe"]["ps"], {"records": 1, "unique": 1})   # misaligned and table-less ones are out
        self.assertEqual(counts["all"]["ps"]["unique"], 9)
        self.assertEqual(counts["live"]["ps"]["unique"], 8)                        # the replaced pack's shader is out
        by_key = {e["key"]: e for e in table["shaders"]}
        for code in self.vs + self.main_ps + list(self.variants.values()) + [self.exe_ps, self.old_variant]:
            self.assertIn(key(code), by_key)
        self.assertFalse(by_key[key(self.old_variant)]["live"])
        self.assertEqual(by_key[key(self.vs[0])]["records"], 2)
        self.assertEqual(by_key[key(self.variants[0])]["packs"], {"p2600001": [0x00, 0x8080]})
        self.assertEqual(by_key[key(self.main_ps[2])]["packs"], {"p40": [0]})
        self.assertNotIn("packs", by_key[key(self.main_ps[0])])
        for item in table["shaders"]:                                              # offsets point at the bytes
            if item["offset"] is None:
                self.assertEqual(table["sources"][item["source"]]["path"], "Data/Update.bsa")   # compressed
                continue
            with open(os.path.join(self.game, table["sources"][item["source"]]["path"]), "rb") as fh:
                fh.seek(item["offset"])
                self.assertEqual(key(fh.read(item["length"])), item["key"])

    def test_damaged_package_is_an_error(self):
        code = self.vs[0]
        for package in (record("vs", 0, code)[:-1],                                   # trailer cut short
                        b"BSPS" + record("vs", 0, code)[4:],                         # stage and bytecode disagree
                        b"BSVS" + struct.pack("<II", 0, len(code) + 4) + code + bytes(40),   # length past END
                        b"junk" * 5 + record("vs", 0, code)):
            with self.assertRaises(si.FormatError):
                si.walk_fxp(package)
        self.write("Data/Update.bsa", bsa({"shadersfx\\shaders002.fxp": b"junk" * 5}))
        status, text = run("index", self.game)
        self.assertEqual(status, 1)
        self.assertIn("shaders002.fxp", text)

    def test_missing_sources(self):
        os.remove(os.path.join(self.game, "Data/Update.bsa"))
        os.remove(os.path.join(self.game, "TESV.exe"))
        table = si.build_index(self.game)
        self.assertEqual(table["missing"], ["Data/Update.bsa", "TESV.exe"])
        self.assertTrue(next(e for e in table["shaders"] if e["key"] == key(self.old_variant))["live"])
        self.assertEqual(run("index", os.path.join(self.root, "nowhere"))[0], 1)

    def caches(self):
        vs, ps = key(self.vs[0]), key(self.variants[0x10])
        first = cache(10, [entry(10, vs, ps), entry(10, key(self.exe_vs), key(self.exe_ps)),
                           entry(10, *self.fixed), entry(10, vs, ps, fill=9, bad_hash=True)])
        second = cache(11, [entry(11, vs, ps), entry(11, vs, key(self.main_ps[0]), fill=8)])
        return self.write("a.dxvk-cache", first), self.write("b.dxvk-cache", second)

    def test_verify(self):
        first, second = self.caches()
        rows = si.coverage(si.build_index(self.game), [first, second])
        self.assertEqual([(r["entries"], r["covered"], r["invalid"]) for r in rows], [(3, 2, 1), (2, 2, 0), (5, 4, 0)])
        self.assertEqual(rows[0]["stages"]["vs"]["missing"], {self.fixed[0]: 1})
        self.assertEqual(rows[0]["stages"]["ps"]["by_source"], {"bsa": 1, "exe": 1})
        self.assertEqual(rows[1]["stages"]["ps"]["missing"], {})
        index = os.path.join(self.root, "index.json")
        self.assertEqual(run("index", self.game, "--json", index)[0], 0)
        status, text = run("verify", index, first, second, "--min-entries", "60")
        self.assertEqual(status, 0, text)
        self.assertIn(self.fixed[0][:8], text)
        self.assertNotIn(self.fixed[0], text)
        self.assertEqual(run("verify", index, first, "--min-entries", "70")[0], 1)
        self.assertEqual(run("verify", self.game, second, "--min-entries", "100")[0], 0)
        self.assertEqual(run("verify", index, self.write("c.dxvk-cache", b"DXVK" + struct.pack("<II", 7, 4)))[0], 1)
        self.assertEqual(run("verify", first, second)[0], 1)                       # a cache is not an index

    def test_siblings(self):
        vs, other_vs = key(self.vs[0]), key(self.vs[1])
        ps = {v: key(code) for v, code in self.variants.items()}
        for version in (10, 11):
            earlier = cache(version, [entry(version, vs, ps[0x10], spec=[(0, 7)]),
                                      entry(version, vs, key(self.main_ps[0]))])
            later = cache(version, [
                entry(version, vs, ps[0x10], spec=[(0, 7)]),                       # in both
                entry(version, vs, ps[0x10], fill=8, spec=[(0, 7)]),               # new state, known shader: not counted
                entry(version, vs, ps[0x20], mask=b"\3", spec=[(0, 7)]),           # one light more, mask differs
                entry(version, vs, ps[0x8090], mask=b"\3", spec=[(0, 7), (12, 1)]),  # also a spec constant
                entry(version, vs, ps[0x00], fill=8, spec=[(0, 7)]),               # sibling, other state
                entry(version, other_vs, ps[0x8090]),                              # pack with another vertex shader
                entry(version, vs, key(self.main_ps[2])),                          # a pack the earlier never used
                entry(version, vs, key(self.main_ps[1]))])                         # not a lighting shader
            status, text = run("siblings", self.game, self.write("a.dxvk-cache", earlier),
                               self.write("b.dxvk-cache", later))
            self.assertEqual(status, 0, text)
            self.assertIn("in later only: 7", text)
            self.assertIn("never saw: 6 (5 lighting, 1 other)", text)
            lines = [line.strip() for line in text.splitlines()]
            self.assertIn("1  sibling, same state apart from the binding mask", lines)
            self.assertIn("1  sibling, same state apart from the binding mask and spec constants", lines)
            self.assertIn("1  sibling, other state differs", lines)
            self.assertIn("2  no earlier entry with this vertex shader and pack", lines)
            self.assertIn("nearest sibling 1 lights and 0 shadow lights away: 1", lines)
            self.assertIn("nearest sibling 0 lights and 1 shadow lights away: 1", lines)
            self.assertIn("light-count siblings: 2 of 5 new lighting entries (40%), 2 of all 6", text)
        self.assertEqual(run("siblings", self.game, self.write("a.dxvk-cache", cache(10, [])),
                             self.write("b.dxvk-cache", cache(11, [])))[0], 1)

    def test_bytecode_cache(self):
        first, second = self.caches()
        out = os.path.join(self.root, "game.dxso-cache")
        status, text = run("dxso", self.game, first, second, "--out", out)
        self.assertEqual(status, 0, text)
        self.assertIn("wrote 5 shaders", text)
        self.assertIn("2 keys of the caches are not in the game files", text)
        with open(out, "rb") as fh:
            data = fh.read()
        self.assertEqual(data[:8], b"DXSO" + struct.pack("<I", 1))
        found, p = {}, 8
        while p < len(data):                      # as StartBytecodeCache of dxvk-native#8 reads it
            stage, length = struct.unpack_from("<II", data, p)
            code = data[p + 28:p + 28 + length]
            self.assertEqual(hashlib.sha1(code).digest(), data[p + 8:p + 28])
            found[code] = stage
            p += 28 + length
        self.assertEqual(found, {self.vs[0]: 1, self.exe_vs: 1, self.variants[0x10]: 16, self.exe_ps: 16,
                                 self.main_ps[0]: 16})
        self.assertEqual(os.stat(out).st_mode & 0o077, 0)
        self.assertEqual(run("dxso", self.game, "--all", "--out", out)[0], 0)
        self.assertEqual(run("dxso", self.game, "--out", out)[0], 2)
        work_tree = os.path.join(self.root, "repo")
        os.makedirs(os.path.join(work_tree, ".git"))
        os.makedirs(os.path.join(work_tree, "deep"))
        refused = os.path.join(work_tree, "deep", "game.dxso-cache")
        self.assertEqual(run("dxso", self.game, first, "--out", refused)[0], 2)
        self.assertFalse(os.path.exists(refused))


if __name__ == "__main__":
    unittest.main()
