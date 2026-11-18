/* Models recursive walks: self and mutual recursion with stack arguments, values live across the recursive
 * call, and a depth of a few hundred frames. */
static uint32_t visits[8];
static uint32_t heap[64];

__attribute__((noinline)) static uint32_t ack_like(uint32_t m, uint32_t n, uint32_t fuel) {
    visits[m & 7]++;
    if (fuel == 0 || m == 0) return n + 1;
    if (n == 0) return ack_like(m - 1, 1, fuel - 1);
    return ack_like(m - 1, ack_like(m, n - 1, fuel - 1) & 7, fuel - 1);
}

static uint32_t odd(uint32_t n, uint32_t acc, uint32_t salt);
__attribute__((noinline)) static uint32_t even(uint32_t n, uint32_t acc, uint32_t salt) {
    uint32_t keep = acc * 3 + salt;
    if (n == 0) return keep;
    uint32_t r = odd(n - 1, keep ^ n, salt + 1);
    heap[n & 63] += r;
    return r + keep;                      /* keep survives the call */
}
__attribute__((noinline)) static uint32_t odd(uint32_t n, uint32_t acc, uint32_t salt) {
    uint32_t keep = acc + (salt << 4);
    if (n == 0) return ~keep;
    uint32_t r = even(n - 1, keep - n, salt ^ 5);
    return r ^ keep;
}

__attribute__((noinline)) static uint32_t tree(uint32_t lo, uint32_t hi, uint32_t depth, uint32_t a, uint32_t b, uint32_t c) {
    if (hi - lo < 2 || depth == 0) return heap[lo & 63] + a - b + c;
    uint32_t mid = lo + (hi - lo) / 2;
    uint32_t l = tree(lo, mid, depth - 1, a + 1, b ^ lo, c + mid);
    heap[mid & 63] ^= l;
    uint32_t r = tree(mid, hi, depth - 1, a ^ l, b + 1, c - lo);
    return l * 31 + r + mid + a + b + c;
}

__attribute__((noinline)) static uint32_t deep(uint32_t n, uint32_t x) {
    if (n == 0) return x;
    uint32_t y = x * 1664525u + 1013904223u;
    return deep(n - 1, y) + (y >> 16);    /* not a tail call: y is needed afterwards */
}

uint32_t test_main(uint32_t a, uint32_t b) {
    for (uint32_t i = 0; i < 64; i++) heap[i] = a + i * b;
    uint32_t r = ack_like(2, a & 7, 60);
    r += even(40 + (b & 7), a, b);
    r ^= tree(0, 64, 7, a, b, r);
    r += deep(300, a ^ b);
    return r + visits[r & 7] + heap[r & 63];
}
