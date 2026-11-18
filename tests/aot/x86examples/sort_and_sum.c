/* Example x86 corpus program: fills a table from the inputs, sorts it, folds it into a checksum. */
static uint32_t table[32];
static uint8_t seen[16];
struct pair { uint32_t key; int32_t weight; };
static struct pair pairs[8];

static uint32_t next(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

__attribute__((noinline)) static void sort(uint32_t *v, int n) {
    for (int i = 1; i < n; i++) {
        uint32_t x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
        v[j + 1] = x;
    }
}

__attribute__((noinline)) static int32_t weigh(const struct pair *p, int n, uint32_t key) {
    int32_t total = 0;
    for (int i = 0; i < n; i++) {
        if (p[i].key == key) continue;
        total += (p[i].key < key) ? p[i].weight : -p[i].weight;
    }
    return total;
}

uint32_t test_main(uint32_t a, uint32_t b) {
    uint32_t state = a ^ (b << 7) ^ 0x9e3779b9u;
    int n = (int)(a & 31) + 1;
    for (int i = 0; i < n; i++) table[i] = next(&state) & 0xffff;
    sort(table, n);
    uint32_t sum = 0;
    for (int i = 0; i < n; i++) {
        sum = (sum << 1 | sum >> 31) ^ table[i];
        seen[table[i] & 15]++;
    }
    for (int i = 0; i < 8; i++) { pairs[i].key = table[i % n]; pairs[i].weight = (int32_t)(b >> (i & 7)) - 100; }
    return sum + (uint32_t)weigh(pairs, 8, table[n / 2]) + seen[b & 15];
}
