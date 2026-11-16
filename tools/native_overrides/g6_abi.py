# Calling-convention test entries for runtime/skyrim_native_g6.c (see ABI in tools/native_test.py).

# 0xCC4AC0, cdecl: [esp+4] = destination, [esp+8] = records (48 bytes each), [esp+0xC] = uint16 indices,
# [esp+0x10] = count. Buffers in the scratch area's random data, a few indices beyond it. Some runs overlap the
# destination with the records, the indices or the stack (the saved ebx / esi / edi the guest pops back, its
# arguments and return address), so the order of loads and stores is checked; some misalign the destination or
# the records, wrap the records or the destination around the top of the 4 GiB guest space (that memory and
# the bottom of it are copied into the scratch area afterwards, so they are compared too), or copy nothing.
# The calling-convention test clears ecx and edx before comparing; the setup ends in a for header whose body is
# the call, so both are saved into the scratch area right after it and compared all the same.
_PTR = "(0x10004000u + (rnd() % 0x8000 & ~3u))"
ABI = {
    0xCC4AC0: (
        "uint32_t dst = %s, src = %s, idx = %s, r = rnd() %% 12, n = rnd() %% 8 ? rnd() %% 5 : rnd() %% 24; "
        "for (uint32_t k = 0; k < 0x40; k++) M32(0xFFFFFF00u + 4 * k) = rnd(); "
        "for (uint32_t k = 0; k < 0x10; k++) M32(4 * k) = rnd(); "
        "for (uint32_t k = 0; k < n; k++) M16(idx + 2 * k) = r == 4 ? rnd() %% 2 : rnd() %% 8 ? rnd() %% 48 : rnd(); "
        "switch (r) { "
        "case 0: dst = src + 4 * (rnd() %% 40) - 80; break; "
        "case 1: dst = idx + 2 * (rnd() %% 40) - 40; break; "
        "case 2: dst += 1 + rnd() %% 3; break; "
        "case 3: src += 1 + rnd() %% 3; break; "
        "case 4: src = 0xFFFFFF90u + 4 * (rnd() %% 28); break; "
        "case 5: dst = 0xFFFFFFA0u + 4 * (rnd() %% 24); n = 1 + rnd() %% 2; break; "
        "case 6: dst = esp - 8 - 4 * (rnd() %% 30); n = 1 + rnd() %% 2; break; "
        "case 7: n = 0; break; } "
        "M32(esp + 4) = dst; M32(esp + 8) = src; M32(esp + 0xC) = idx; M32(esp + 0x10) = n; "
        "for (int once_ = 1; once_; once_ = 0, M32(0x1001F100u) = c->r[1], M32(0x1001F104u) = c->r[2])"
        % (_PTR, _PTR, _PTR),
        "memcpy(MEM + 0x1001F200u, MEM + 0xFFFFFF00u, 0x100); memcpy(MEM + 0x1001F300u, MEM, 0x40);"),
}
