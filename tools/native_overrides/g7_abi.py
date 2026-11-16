# Calling-convention test setups for runtime/skyrim_native_g7.c (see ABI in tools/native_test.py).
# Only the guest's dead stack below the return address may differ (cleared in both). ecx and edx, which the
# harness clears, are set as the guest leaves them; that was checked once with the clearing taken out (see the commit).

_PTR = "(0x10004000u + (rnd() % 0x8000 & ~3u))"
ABI = {
    0x4488B0: (  # transpose: ecx = in, [esp+4] = out; sometimes the same matrix, overlapping, or unaligned
        "uint32_t in = %s, out = %s; switch (rnd() %% 8) { "
        "case 0: out = in; break; case 1: out = in + 4 * (rnd() %% 17) - 32; break; "
        "case 2: out += 1 + rnd() %% 3; break; case 3: in += 1 + rnd() %% 3; break; "
        "case 4: in += 1 + rnd() %% 3; out = in + rnd() %% 33 - 16; break; } "
        "c->r[1] = in; M32(esp + 4) = out; M32(esp + 8) = rnd();" % (_PTR, _PTR), ""),
}
