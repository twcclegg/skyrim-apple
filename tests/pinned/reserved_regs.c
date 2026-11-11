/* Compiler check for LB_PINNED_REGS: global register variables in reserved registers (arm64).
 *
 * The pinned build keeps guest registers in x21-x28 with -ffixed-xN. Three LLVM code-generation passes move
 * or speculate writes to reserved registers (see docs/perf/LOG.md, 2026-11-11):
 *   post-RA machine LICM   hoists an invariant write out of a loop, above reads of the old value, and
 *                          hoists a conditional write so it happens when its condition never holds
 *   early if-conversion    turns a branch into a select and performs the guarded write on both paths
 *   AArch64 cond. compare  folds two compares into cmp+ccmp and performs the write between them always
 * Each function below is the smallest shape that showed one of them. tests/pinned/run.sh builds this file
 * with the flags the pinned build uses and expects every check to pass, and builds it without them to
 * report which of the bugs the installed compiler still has.
 * Build with -ffixed-x21 -ffixed-x22. */
#include <stdio.h>
register unsigned eax __asm__("w21");
register unsigned ecx __asm__("w22");
unsigned mem[16];
#define NI __attribute__((noinline))
/* 1. loop: read the old value, then overwrite with an invariant */
NI void loop_read_then_write(unsigned n) { for (unsigned i = 0; i < n; i++) { mem[i] = eax; eax = 7; } }
/* 2. loop: conditional invariant write that never executes */
NI void loop_cond_write(unsigned n, unsigned *p) { while (n--) { if (p[n]) eax = 7; p[n] += ecx; } }
/* 3. triangle with a value merge: write only on one side */
NI unsigned tri_write(unsigned a, unsigned b) { unsigned r = 3; if (a) { eax = b; r = b + 1; } return r; }
/* 4. diamond: each side writes a different register */
NI unsigned dia_write(unsigned a, unsigned b) { unsigned r; if (a) { eax = b; r = b + 1; } else { ecx = b; r = b - 1; } return r; }
/* 5. compare chain: write between the two compares */
NI void cmp_chain(unsigned a, unsigned b) { if (a == 5) { eax = b + 1; if (ecx == b) mem[2] = 1; } }
/* 6. compare chain reading a register written in the first block */
NI unsigned cmp_chain2(unsigned a, unsigned b) { if (a == 5 && (eax = b, ecx == b)) return 1; return 0; }
/* 7. short-circuit or: the write is on the path where the first test fails */
NI unsigned or_write(unsigned a, unsigned b, unsigned c) { if (a == 5 || (eax = c, b == 9)) return 1; return 0; }
void ext(void) { mem[3]++; }
/* 8. compare chain ending in a call */
NI void cmp_call(unsigned a, unsigned b) { if (a == 5) { eax = b + 1; if (ecx == b) ext(); } }
static int fails;
#define CHECK(name, cond) do { if (!(cond)) { fails++; printf("FAIL %s (eax=%u ecx=%u)\n", name, eax, ecx); } } while (0)
#define CLOB() __asm__ volatile("" : : : "x21", "x22")
int main(void) {
    unsigned s21 = eax, s22 = ecx; unsigned p[9] = {0}; unsigned r;
    eax = 1; ecx = 2; CLOB(); loop_read_then_write(3); CLOB(); CHECK("loop_read_then_write", mem[0] == 1 && mem[1] == 7 && eax == 7);
    eax = 1; ecx = 2; CLOB(); loop_cond_write(3, p); CLOB(); CHECK("loop_cond_write small", eax == 1);
    for (int i = 0; i < 9; i++) p[i] = 0;
    eax = 1; ecx = 0; CLOB(); loop_cond_write(9, p); CLOB(); CHECK("loop_cond_write large", eax == 1);
    eax = 1; ecx = 2; CLOB(); r = tri_write(0, 9); CLOB(); CHECK("tri_write", r == 3 && eax == 1);
    eax = 1; ecx = 2; CLOB(); r = dia_write(0, 9); CLOB(); CHECK("dia_write else", r == 8 && eax == 1 && ecx == 9);
    eax = 1; ecx = 2; CLOB(); r = dia_write(1, 9); CLOB(); CHECK("dia_write then", r == 10 && eax == 9 && ecx == 2);
    mem[2] = 0; eax = 1; ecx = 2; CLOB(); cmp_chain(4, 2); CLOB(); CHECK("cmp_chain", eax == 1 && mem[2] == 0);
    eax = 1; ecx = 2; CLOB(); r = cmp_chain2(4, 2); CLOB(); CHECK("cmp_chain2", r == 0 && eax == 1);
    eax = 1; ecx = 2; CLOB(); r = or_write(5, 0, 9); CLOB(); CHECK("or_write", r == 1 && eax == 1);
    eax = 1; ecx = 2; CLOB(); cmp_call(4, 2); CLOB(); CHECK("cmp_call", eax == 1 && mem[3] == 0);
    int f = fails; eax = s21; ecx = s22; CLOB();
    printf(f ? "%d failed\n" : "all passed\n", f); return f != 0;
}
