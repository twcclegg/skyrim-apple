# Calling-convention test entries for runtime/skyrim_native_g5.c (see ABI in tools/native_test.py).
# The natives inline the guest's helper calls, so they are checked against the guest's whole lifted call tree.
# Nothing extra is cleared: only the dead stack below the return address may differ; ecx, edx, the XMM registers
# and callee-visible memory are compared too (STRICT, see native_test.py).

_PTR = "(0x10004000u + (rnd() % 0x8000 & ~3u))"
_SPECIAL = ("static const uint32_t sp_[] = {0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC12345u, 0x7F800001u, 0u, "
            "0x80000000u, 0x7F7FFFFFu}; ")
ABI = {
    # [esp+4] = sphere P, [esp+8] = node (six planes, mask at +0x60). Planes and sphere are the scratch area's
    # random floats, often with a plane offset that puts the sphere inside it (the loop goes on), sometimes a
    # plane exactly at the sphere's +-w (the comparisons' equal cases), sometimes an infinity or NaN (the plane
    # test is called); the sphere sometimes overlaps the planes or the mask. Masks:
    # empty, all six planes, some, random, only bits above the six planes.
    0xB1B6C0: (
        _SPECIAL +
        "uint32_t P = %s, node = %s, mode = rnd() %% 8, m; "
        "if (mode == 0) P = node + 0x50 + 4 * (rnd() %% 9); "
        "if (mode == 1) P = node + 4 * (rnd() %% 24); "
        "for (int k = 0; k < 6; k++) if (mode >= 2 && rnd() %% 4) M32(node + 16 * k + 12) = 0xC47A0000u; "
        "if (mode == 4) for (int k = 0; k < 6; k++) if (rnd() %% 3 == 0) { uint32_t q = node + 16 * k; "
        "  M32(q) = M32(q + 4) = M32(q + 8) = 0; M32(q + 12) = M32(P + 12) ^ (rnd() %% 2 << 31); } "
        "if (mode >= 5) { for (int k = 0; k < 24; k++) if (rnd() %% 40 == 0) M32(node + 4 * k) = sp_[rnd() %% 8]; "
        "  for (int k = 0; k < 4; k++) if (rnd() %% 40 == 0) M32(P + 4 * k) = sp_[rnd() %% 8]; } "
        "switch (rnd() %% 5) { case 0: m = 0; break; case 1: m = 0x3F; break; case 2: m = rnd() & 0x3F; break; "
        "case 3: m = rnd(); break; default: m = rnd() & ~0x3Fu; } "
        "M32(node + 0x60) = m; c->r[1] = rnd(); M32(esp + 4) = P; M32(esp + 8) = node;" % (_PTR, _PTR), "", STRICT),
}

# ecx = object (planes at +0x2C, mask at +0x8C), [esp+4] = sphere P; the same kinds of values as 0xB1B6C0.
ABI[0xB07B90] = (
    _SPECIAL +
    "uint32_t P = %s, obj = %s, pl = obj + 0x2C, mode = rnd() %% 8, m; "
    "if (mode == 0) P = obj + 0x7C + 4 * (rnd() %% 9); "
    "if (mode == 1) P = pl + 4 * (rnd() %% 24); "
    "for (int k = 0; k < 6; k++) if (mode >= 2 && rnd() %% 4) M32(pl + 16 * k + 12) = 0xC47A0000u; "
    "if (mode == 4) for (int k = 0; k < 6; k++) if (rnd() %% 3 == 0) { uint32_t q = pl + 16 * k; "
    "  M32(q) = M32(q + 4) = M32(q + 8) = 0; M32(q + 12) = M32(P + 12) ^ (rnd() %% 2 << 31); } "
    "if (mode >= 5) { for (int k = 0; k < 24; k++) if (rnd() %% 40 == 0) M32(pl + 4 * k) = sp_[rnd() %% 8]; "
    "  for (int k = 0; k < 4; k++) if (rnd() %% 40 == 0) M32(P + 4 * k) = sp_[rnd() %% 8]; } "
    "switch (rnd() %% 5) { case 0: m = 0; break; case 1: m = 0x3F; break; case 2: m = rnd() & 0x3F; break; "
    "case 3: m = rnd(); break; default: m = rnd() & ~0x3Fu; } "
    "M32(obj + 0x8C) = m; c->r[1] = obj; M32(esp + 4) = P;" % (_PTR, _PTR), "", STRICT)
