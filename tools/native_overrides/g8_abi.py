# Calling-convention test setups for runtime/skyrim_native_g8.c (see ABI in tools/native_test.py).
# The natives leave every register as the guest does, so nothing beyond the default is cleared: only the dead
# stack below the return address may differ.
ABI = {
    0xF531CC: (  # fabs: ordinary doubles of either sign, zeros, denormals, the largest and smallest normals,
                 # random bit patterns, infinities and NaNs (quiet and signalling, either sign); any control word
        "static const uint64_t sp_[] = {0, 0x8000000000000000ull, 0x7FF0000000000000ull, 0xFFF0000000000000ull, "
        "0x7FF8000000000000ull, 0xFFF8000000000001ull, 0x7FF0000000000001ull, 0xFFF4000000000000ull, 1, "
        "0x800FFFFFFFFFFFFFull, 0x7FEFFFFFFFFFFFFFull, 0xFFEFFFFFFFFFFFFFull, 0x8010000000000000ull}; "
        "static const uint16_t cw_[] = {0x27F, 0x7F, 0x37F, 0x133F, 0xA7F, 0x67F, 0xE7F}; "
        "double d_ = rnd_double(); if (rnd() & 1) d_ = -d_; uint64_t b_; memcpy(&b_, &d_, 8); "
        "switch (rnd() % 8) { case 0: b_ = sp_[rnd() % 13]; break; "
        "case 1: b_ = ((uint64_t)rnd() << 33) ^ ((uint64_t)rnd() << 2) ^ rnd(); break; "
        "case 2: b_ = (((uint64_t)rnd() << 33) ^ rnd()) & 0x800FFFFFFFFFFFFFull; break; } "
        "M64(esp + 4) = b_; c->fpu_cw = (rnd() % 4) ? cw_[rnd() % 7] : (uint16_t)rnd();",
        ""),
    0x45C640: (  # bit n of the 64-bit mask at [ecx+0x20]: n below 32, 32 to 63, 64 and up, and with high bytes set
                 # (the shift count is the low byte); masks of single bits, all ones, zero and random
        "uint32_t o_ = 0x10004000u + (rnd() % 0x8000 & ~3u), r_ = rnd() % 6; "
        "M32(o_ + 0x20) = r_ == 0 ? 0 : r_ == 1 ? 0xFFFFFFFFu : r_ == 2 ? 1u << (rnd() % 32) : rnd(); "
        "r_ = rnd() % 6; M32(o_ + 0x24) = r_ == 0 ? 0 : r_ == 1 ? 0xFFFFFFFFu : r_ == 2 ? 1u << (rnd() % 32) : rnd(); "
        "r_ = rnd() % 4; c->r[1] = o_; M32(esp + 4) = r_ == 0 ? rnd() : r_ == 1 ? rnd() % 256 : rnd() % 72;",
        ""),
}

# 0xACE100: a culler (six planes at +0x2C, plane mask at +0x8C, flag byte at +0x10D) and an object (bound at +0x88:
# centre, then a radius that is now and then 0.0 (nothing to test) or -0.0, flags at +0x98, a vtable whose +0xC4
# entry is any target: the stub stands in for it), each in its own slot of random data. Plane normals of unit-ish size and bounds whose centre distances and
# radii bring up inside, outside and straddling; now and then a NaN or infinity in a plane or the bound (the
# lifted 0x472580 then runs) and an exact tie with the radius.
_F8 = ("\n#define g8f() __extension__({ static const uint32_t sp_[] = {0x7FC00000u, 0xFFC00001u, 0x7F800000u, "
       "0xFF800000u, 0x80000000u, 0}; float f_ = (float)rnd_double(); if (rnd() % 40 == 0) memcpy(&f_, &sp_[rnd() % 6], 4); "
       "f_; })\n")
ABI[0xACE100] = (_F8 +
    "uint32_t self = 0x10004000u + (rnd() % 0x200 & ~3u), obj = 0x10006000u + (rnd() % 0x200 & ~3u), "
    "vt = 0x10008000u + (rnd() % 0x200 & ~3u); "
    "for (int k = 0; k < 6; k++) { for (int j = 0; j < 4; j++) MF32(self + 0x2C + 16 * k + 4 * j) = g8f(); "
    "  if (rnd() % 2) MF32(self + 0x2C + 16 * k + 4 * (rnd() % 3)) = (rnd() & 1) ? 1.0f : -1.0f; } "
    "uint32_t r_ = rnd() % 4; M32(self + 0x8C) = r_ == 0 ? 0x3Fu : r_ == 1 ? rnd() : (rnd() & 0x3Fu) | (rnd() & 0xFFFFFFC0u); "
    "M8(self + 0x10D) = (rnd() & 1) ? 0 : (uint8_t)rnd(); "
    "for (int j = 0; j < 3; j++) MF32(obj + 0x88 + 4 * j) = g8f(); "
    "r_ = rnd() % 10; MF32(obj + 0x94) = r_ < 6 ? fabsf(g8f()) * (float)(1 + rnd() % 100) : r_ < 8 ? g8f() : 0.0f; "
    "if (r_ == 9) M32(obj + 0x94) = 0x80000000u; M32(obj + 0x98) = rnd(); M32(obj) = vt; "
    "M32(vt + 0xC4) = 0x400000u + (rnd() % 0x100000 & ~0xFu); "
    "if (rnd() % 16 == 0) { uint32_t k_ = rnd() % 6; float d_ = MF32(self + 0x2C + 16 * k_ + 12); "
    "  MF32(obj + 0x88) = 0.0f; MF32(obj + 0x8C) = 0.0f; MF32(obj + 0x90) = 0.0f; "
    "  MF32(obj + 0x94) = (rnd() & 1) ? d_ : -d_; } "
    "c->r[1] = self; M32(esp + 4) = obj;",
    "")

# 0xF6029C: the SSE2 block copy: 1 to 64 blocks plus a remainder it ignores (below one block the guest loops 2^32
# times, never in the game: memcpy calls it from 256 bytes on), 16-byte aligned and unaligned addresses, and overlapping ranges either way round (the ranges stay clear of the stack).
ABI[0xF6029C] = (
    "uint32_t n_ = (1 + rnd() % ((rnd() & 3) ? 8 : 64)) * 128 + rnd() % 128; "
    "uint32_t al_ = (rnd() & 3) ? ~0xFu : ~0u, dst = 0x10004000u + (rnd() % 0x6000 & al_), src = 0x1000C000u + (rnd() % 0x6000 & al_); "
    "switch (rnd() % 6) { case 0: src = dst + (rnd() % 0x400 & al_); break; case 1: dst = src + (rnd() % 0x400 & al_); break; "
    "case 2: src = dst; break; } "
    "M32(esp + 4) = dst; M32(esp + 8) = src; M32(esp + 12) = n_;",
    "")
