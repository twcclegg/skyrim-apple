/* Two guest threads hand a value over through memory: one spins until the other sets a flag. Models the
 * waits on job and loading flags that run on Skyrim's worker threads. test_main(0, _) waits by calling a
 * loop-free function each time round; test_main(1, _) waits in a loop of its own on a volatile flag;
 * test_main(2, v) is the other thread, which stores v and sets both flags. */
static uint32_t flag, data, spins;
static volatile uint32_t vflag;

__attribute__((noinline)) static uint32_t ready(void) { return flag; }
__attribute__((noinline)) static uint32_t take(void) { return flag ? data : 0; }
__attribute__((noinline)) static void give(uint32_t v) { data = v; flag = 1; }

__attribute__((noinline)) static uint32_t wait_call(void) {
    uint32_t n = 0;
    while (!ready()) {
        n++;
        __asm__ volatile("" ::: "memory");  /* keeps the x86 compiler calling ready() each time round */
    }
    spins = n;
    return take();
}

__attribute__((noinline)) static uint32_t wait_inline(void) {
    while (!vflag) {}
    return data;
}

uint32_t test_main(uint32_t a, uint32_t b) {
    if (a == 0) return wait_call();
    if (a == 1) return wait_inline();
    give(b);
    vflag = 1;
    return 0;
}
