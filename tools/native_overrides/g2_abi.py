# Calling-convention test entries for runtime/skyrim_native_g2.c (see ABI in tools/native_test.py):
# address: (C argument setup, extra C clearing). These replacements inline guest helpers they call (the vector
# length and the C runtime's sqrt and fabs, the box corner builder), so they are compared with the whole lifted
# call tree. All of them leave the x87 stack as they found it and return in eax, so the x87 registers and
# condition codes are not part of the result; the box tests return a bool in al, so the rest of eax isn't either.

# (setup arithmetic is kept to one operation per expression: a fused multiply-add in one build and not the
# other would hand the two sides different arguments)
# a float, now and then a special one: zeros, infinities, NaNs, a denormal, the largest float
_FLOAT = ("\n#define g2f() __extension__({ static const uint32_t sp_[] = {0, 0x80000000u, 0x7F800000u, 0xFF800000u, "
          "0x7FC00000u, 0xFFC00001u, 0x7F800001u, 0x00000001u, 0x7F7FFFFFu, 0x3F800000u}; "
          "float f_ = (float)rnd_double(); if (rnd() % 12 == 0) memcpy(&f_, &sp_[rnd() % 10], 4); f_; })\n")
_X87 = "c->c0 = c->c1 = c->c2 = c->c3 = 0; memset(c->st, 0, sizeof c->st);"

# box (centre at +0x10, half extents at +0x20) at 0x10004000; planes (6 x nx, ny, nz, d; mask at +0x60) at
# 0x10004100. Some boxes are flat in z (0xAFBF80 then checks four corners), some planes are tiny or huge so
# both answers come up; the mask is usually one of the 64 meaningful ones.
_BOX = (_FLOAT + "uint32_t box = 0x10004000u, pl = 0x10004100u; "
        "for (int k = 0; k < 3; k++) { MF32(box + 0x10 + 4 * k) = g2f(); MF32(box + 0x20 + 4 * k) = fabsf(g2f()); } "
        "if (rnd() % 4 == 0) MF32(box + 0x28) = (rnd() & 1) ? 0.0f : -0.0f; "
        "for (int k = 0; k < 6; k++) { for (int j = 0; j < 4; j++) MF32(pl + 16 * k + 4 * j) = g2f(); "
        "  if (rnd() % 3 == 0) MF32(pl + 16 * k + 12) = -fabsf(MF32(pl + 16 * k + 12)) * 64.0f; "
        "  if (rnd() % 3 == 0) MF32(pl + 16 * k + 12) = fabsf(MF32(pl + 16 * k + 12)) * 64.0f; } "
        "uint32_t r = rnd() % 8; M32(pl + 0x60) = r == 0 ? 0 : r == 1 ? rnd() : r == 2 ? 63 : rnd() % 64; "
        "c->r[1] = box; M32(esp + 4) = pl;")

ABI = {
    0x420560: (  # normalized cross product: ordinary, parallel (zero length), out aliasing an input
        _FLOAT + "uint32_t a = 0x10004000u, b = 0x10004010u, out = 0x10004020u; "
        "for (int k = 0; k < 3; k++) { MF32(a + 4 * k) = g2f(); MF32(b + 4 * k) = g2f(); } "
        "uint32_t r = rnd() % 8; "
        "if (r == 0) for (int k = 0; k < 3; k++) MF32(b + 4 * k) = MF32(a + 4 * k) * 2.0f; "
        "if (r == 1) for (int k = 0; k < 3; k++) { static const float off_[] = {0.0f, 1e-6f, 2e-6f}; "
        "  volatile float t_ = MF32(a + 4 * k) * 1e-4f; MF32(b + 4 * k) = t_ + off_[k]; } "
        "if (r == 2) out = a; if (r == 3) out = b; "
        "if (rnd() % 16 == 0) c->fpu_cw = 0x7F; "
        "c->r[1] = a; M32(esp + 4) = out; M32(esp + 8) = b;",
        _X87),
    0xAFC100: (_BOX, "c->r[0] &= 0xFF; " + _X87),
    0xAFBF80: (_BOX, "c->r[0] &= 0xFF; " + _X87),
}
