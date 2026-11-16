# Calling-convention test entries for runtime/skyrim_native_g1.c (see ABI in tools/native_test.py).
# The natives inline the guest's helper calls, so they are checked against the guest's whole lifted call tree.
# Nothing extra is cleared: only the dead stack below the return address may differ.

# Three transforms in the scratch area's random floats; sometimes the same one twice, or overlapping, so the
# order of reads and writes is checked too.
_PTR = "(0x10004000u + (rnd() % 0x8000 & ~3u))"
ABI = {
    0x4EC9C0: (  # NiTransform out = A * B: ecx = A, [esp+4] = out, [esp+8] = B
        "uint32_t A = %s, B = %s, out = %s; switch (rnd() %% 8) { "
        "case 0: out = A; break; case 1: out = B; break; case 2: B = A; break; case 3: out = B = A; break; "
        "case 4: out = A + 4 * (rnd() %% 25) - 48; break; case 5: out = B + 4 * (rnd() %% 25) - 48; break; } "
        "c->r[1] = A; M32(esp + 4) = out; M32(esp + 8) = B;" % (_PTR, _PTR, _PTR), ""),
    0xCDA170: (  # ecx = object (0x700 bytes used), [esp+4] = transform T, sometimes inside the object
        "uint32_t th = %s, T = %s; switch (rnd() %% 6) { "
        "case 0: T = th + 0x690 + 4 * (rnd() %% 40) - 80; break; case 1: T = th + 0x3D8 + 4 * (rnd() %% 30) - 60; break; } "
        "c->r[1] = th; M32(esp + 4) = T; M32(esp + 8) = rnd();" % (_PTR, _PTR), ""),
}

# 0xCDA260: a renderer whose frame clock is [[ecx+0x658]+0x3C], a skin instance P with its skin data, root node,
# bone transform pointers (some null), per-bone data and a palette buffer large enough for the size asked (a
# buffer that must grow goes to the guest code natively too); every object in its own 4 KiB slot of random data.
# The frame and size sometimes match P's, which returns at once. Its call tree goes deeper than 0x100 bytes below
# the return address, and a function returning nothing leaves the x87 registers (not the top) to its caller.
ABI[0xCDA260] = (
    "uint32_t o_[10]; for (int k = 0; k < 10; k++) o_[k] = 0x10004000u + k * 0x1000u + (rnd() % 0x200 & ~3u); "
    "uint32_t th = o_[0], F = o_[1], P = o_[2], sk = o_[3], N = o_[4], bones = o_[5], tr = o_[6], data = o_[7], "
    "buf = o_[8], X = o_[9], count = rnd() % 7, r = rnd() % 8, mode = r < 3 ? 4 : r < 6 ? 3 : r - 5; "
    "M32(th + 0x658) = F; M32(F + 0x3C) = rnd() % 3; "
    "M32(P + 8) = sk; M32(P + 0x10) = N; M32(P + 0x18) = bones; M32(P + 0x1C) = rnd() % 3; "
    "M32(P + 0x24) = (rnd() & 1) ? mode : rnd() % 5; M32(P + 0x28) = count * mode * 16 + rnd() % 64; M32(P + 0x2C) = buf; "
    "M32(sk + 0x40) = data; M32(sk + 0x44) = count; "
    "for (uint32_t k = 0; k < count; k++) M32(bones + 4 * k) = rnd() % 5 ? tr + 0x34 * k : 0; "
    "c->r[1] = th; M32(esp + 4) = P; M32(esp + 8) = X; M32(esp + 0xC) = (rnd() & 1) ? 0 : rnd(); "
    "M32(esp + 0x10) = mode; M32(esp + 0x14) = (rnd() & 1) ? 0 : rnd();",
    "memset(MEM + esp - 0x300, 0, 0x300); for (int k = 0; k < 8; k++) c->st[k] = 0;")
