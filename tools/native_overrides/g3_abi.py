# Calling-convention test entries for group 3 (see ABI in tools/native_test.py): address: (setup, extra clearing).
ABI = {
    0x40A4B0: (  # vector length at ecx: ordinary, huge (sum overflows), tiny (denormal sums), infinities, NaNs;
                 # any x87 control word (the CRT sqrt changes and restores it) and either value of the CRT flag it reads
        "static const uint32_t sp[] = {0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00001u, 0x7F800001u, "
        "0x80000000u, 0x00000001u, 0x7F7FFFFFu}; static const uint16_t cw[] = {0x27F, 0x7F, 0x37F, 0x25F, 0x1272}; "
        "uint32_t v = 0x10004000u + (rnd() % 0x1000 & ~3u); c->r[1] = v; "
        "for (int k = 0; k < 3; k++) { float x = (float)(rnd_double() * ((rnd() & 1) ? -1 : 1)); "
        "if (rnd() % 4 == 0) x *= (rnd() & 1) ? 1e19f : 1e-21f; uint32_t b; memcpy(&b, &x, 4); "
        "if (rnd() % 10 == 0) b = sp[rnd() % 8]; M32(v + 4 * k) = b; } "
        "c->fpu_cw = cw[rnd() % 5]; M32(0x1BBEB4Cu) = rnd() & 1;",
        "for (int k = 0; k < 8; k++) if (k != c->top) c->st[k] = 0;"),
    0xABED70: (  # plane from p0, p1, p2: general, collinear and coincident points, edge lengths around the 1e-6
                 # cut-off, overflowing and special coordinates, the plane overlapping p0 or p1
        "static const uint32_t sp[] = {0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x80000000u, 0x00000001u, "
        "0x7F7FFFFFu}; static const float sc[] = {1.0f, 1e-3f, 3e-4f, 1e-4f, 1e20f, 1e-30f, 100.0f}; "
        "uint32_t base = 0x10004000u + (rnd() % 0x800 & ~3u), p0 = base + 0x100, p1 = base + 0x140, "
        "p2 = base + 0x180, out = base; float scale = sc[rnd() % 7]; int mode = rnd() % 4; "
        "for (int k = 0; k < 3; k++) { float a = (float)(rnd_double() * 64.0), "
        "d1 = (float)rnd_double() * scale, d2 = (float)rnd_double() * scale; "
        "MF32(p0 + 4 * k) = a; MF32(p1 + 4 * k) = a + d1; "
        "MF32(p2 + 4 * k) = mode == 0 ? a + d1 + d1 : mode == 1 ? a + d1 : a + d1 + d2; } "
        "if (rnd() % 12 == 0) M32(base + 0x100 + 0x40 * (rnd() % 3) + 4 * (rnd() % 3)) = sp[rnd() % 6]; "
        "switch (rnd() % 6) { case 0: out = p0; break; case 1: out = p1; break; default: break; } "
        "c->r[1] = out; M32(esp + 4) = p0; M32(esp + 8) = p1; M32(esp + 12) = p2;",
        "for (int k = 0; k < 8; k++) c->st[k] = 0;"),
}
