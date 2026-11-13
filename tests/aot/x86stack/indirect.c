/* Models virtual dispatch: calls through tables of function pointers with different conventions and
 * argument counts, several in a row, with locals and saved registers live across them. */
typedef uint32_t (*op2)(uint32_t, uint32_t);
typedef uint32_t (X86_STDCALL *sop3)(uint32_t, uint32_t, uint32_t);
typedef uint32_t (X86_FASTCALL *fop4)(uint32_t, uint32_t, uint32_t, uint32_t);
static uint32_t trace[8];

__attribute__((noinline)) static uint32_t add2(uint32_t a, uint32_t b) { return a + b; }
__attribute__((noinline)) static uint32_t mix2(uint32_t a, uint32_t b) { trace[a & 7] += b; return a * 31 ^ b; }
__attribute__((noinline)) static uint32_t rot2(uint32_t a, uint32_t b) { return a << (b & 31) | a >> ((32 - (b & 31)) & 31); }
__attribute__((noinline)) X86_STDCALL static uint32_t s_sel(uint32_t a, uint32_t b, uint32_t c) { return a & 1 ? b : c; }
__attribute__((noinline)) X86_STDCALL static uint32_t s_mad(uint32_t a, uint32_t b, uint32_t c) { trace[c & 7] ^= a; return a * b + c; }
__attribute__((noinline)) X86_FASTCALL static uint32_t f_fold(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return ((a ^ b) + c) * (d | 1); }
__attribute__((noinline)) X86_FASTCALL static uint32_t f_pick(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { trace[d & 7] -= b; return a > b ? c : d; }

static const op2 ops2[4] = {add2, mix2, rot2, mix2};
static const sop3 ops3[2] = {s_sel, s_mad};
static const fop4 ops4[2] = {f_fold, f_pick};

struct obj { const op2 *vt2; const sop3 *vt3; const fop4 *vt4; uint32_t state; };

__attribute__((noinline)) static uint32_t run(struct obj *o, uint32_t x, uint32_t y) {
    uint32_t a = x ^ o->state, b = y + o->state, c = a * b, d = a - b, e = c ^ d;
    for (uint32_t i = 0; i < 7; i++) {
        a = o->vt2[i & 3](a, b);
        b = o->vt3[i & 1](b, c, d);
        c = o->vt2[(i + a) & 3](c, e);
        d = o->vt4[(a >> 3) & 1](d, e, a, b);
        e = o->vt3[(b >> 5) & 1](e, a, i);
        o->state += e;
        if ((e & 0x1f) == 3) return a + b + c;
    }
    return o->vt4[0](a, b, c, d) + o->vt2[1](e, o->state);
}

__attribute__((noinline)) static uint32_t chain(op2 f, op2 g, uint32_t x, uint32_t y, uint32_t z) {
    /* nested: g's result is an argument of f while another argument is already on the stack */
    return f(g(x, y), f(g(y, z), g(z, x)));
}

uint32_t test_main(uint32_t a, uint32_t b) {
    struct obj o = {ops2, ops3, ops4, a * 7 + b};
    uint32_t r = run(&o, a, b);
    r ^= run(&o, b, r);
    r += chain(ops2[a & 3], ops2[b & 3], a, b, r);
    r += chain(ops2[r & 3], ops2[(r >> 2) & 3], r, o.state, trace[r & 7]);
    return r + o.state;
}
