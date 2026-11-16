# Calling-convention test setups for runtime/skyrim_native_g4.c (see ABI in tools/native_test.py).
ABI = {
    0xAE4CB0: (  # sort indices by key: object at 0x10004000, index array, key array with duplicates, zeros,
                 # infinities and NaNs (quiet and signalling, either sign); whole and partial ranges, empty ones
        "uint32_t obj = 0x10004000u, ia = 0x10005000u, ka = 0x10006000u; "
        "uint32_t n = 1 + rnd() % ((rnd() & 3) ? 24 : 300), nk = 1 + rnd() % 400, mode = rnd() % 8; "
        "static const uint32_t special[] = {0, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u, "
        "0x7F800001u, 0xFF812345u}; "
        "M32(obj + 0x10) = ia; M32(obj + 0x20) = ka; "
        "for (uint32_t k = 0; k < nk; k++) { float x = (float)rnd_double(); uint32_t b; memcpy(&b, &x, 4); "
        "  if (mode == 1) b = 0x3F800000u + (rnd() % 3 << 20); "
        "  if (rnd() % 40 == 0) b = special[rnd() % 8]; M32(ka + 4 * k) = b; } "
        "for (uint32_t k = 0; k < n; k++) M16(ia + 2 * k) = (uint16_t)(mode == 2 ? k % nk : mode == 3 ? (n - 1 - k) % nk "
        ": rnd() % nk); "
        "if (mode == 4) for (uint32_t k = 0; k < nk; k++) M32(ka + 4 * k) = 0x40000000u - k * 0x1000u; "
        "if (mode == 5) for (uint32_t k = 0; k < nk; k++) M32(ka + 4 * k) = 0x3E000000u + k * 0x1000u; "
        "int32_t lo = 0, hi = (int32_t)n - 1; "
        "if (rnd() % 4 == 0) { lo = (int32_t)(rnd() % n); hi = (int32_t)(rnd() % n); } "
        "c->r[1] = obj; M32(esp + 4) = (uint32_t)lo; M32(esp + 8) = (uint32_t)hi;",
        # the guest's frames and those of its recursive calls (32 bytes each), all below the stack pointer
        "memset(MEM + esp - 0x3000, 0, 0x3000);"),
}
