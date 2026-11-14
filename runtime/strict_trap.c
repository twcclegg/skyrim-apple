/* LB_STRICT_TRAP: unaligned guest accesses under LB_STRICT_ORDER without an inline alignment test.
 *
 * Load-acquire and store-release (ldar/stlr, ldapr) take an unaligned address on CPUs with FEAT_LSE2,
 * which every Apple silicon chip has, as long as the access stays inside one 16-byte block; one that
 * crosses a block boundary raises SIGBUS. With LB_STRICT_TRAP the lifted code issues the instruction
 * without testing the address first, and this handler performs the rare crossing access the way the
 * inline slow path does (a plain access with a barrier) and resumes after the instruction. */
#if defined(__aarch64__) && defined(__APPLE__) && defined(LB_STRICT_ORDER) && defined(LB_STRICT_TRAP)
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/ucontext.h>
#include "x86.h"

static struct sigaction g_prev;
unsigned long long rt_strict_traps;     /* crossing accesses handled, for the exit report */

static int fixup(siginfo_t *si, ucontext_t *uc) {
    struct __darwin_arm_thread_state64 *ss = &uc->uc_mcontext->__ss;
    const uint32_t *pc = (const uint32_t *)__darwin_arm_thread_state64_get_pc(*ss);
    uint32_t insn = *pc, size = 1u << (insn >> 30), rn = (insn >> 5) & 31, rt = insn & 31;
    int load;
    if ((insn & 0x3FBFFC00u) == 0x089FFC00u) load = (insn >> 22) & 1;         /* stlr / ldar, any width */
    else if ((insn & 0x3FFFFC00u) == 0x38BFC000u) load = 1;                    /* ldapr */
    else return 0;
    uintptr_t a = rn == 31 ? (uintptr_t)__darwin_arm_thread_state64_get_sp(*ss) : (uintptr_t)ss->__x[rn];
    if ((uintptr_t)si->si_addr - a >= size || (a & 15) + size <= 16) return 0; /* not this access, or not a crossing */
    if (a < (uintptr_t)MEM || a - (uintptr_t)MEM > 0xFFFFFFFFull) return 0;    /* only guest memory */
    uint64_t v = 0;
    if (load) {
        memcpy(&v, (const void *)a, size);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (rt != 31) ss->__x[rt] = v;
    } else {
        v = rt == 31 ? 0 : ss->__x[rt];
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        memcpy((void *)a, &v, size);
    }
    __darwin_arm_thread_state64_set_pc_fptr(*ss, (void *)(pc + 1));
    __atomic_fetch_add(&rt_strict_traps, 1, __ATOMIC_RELAXED);
    return 1;
}

static void on_bus(int sig, siginfo_t *si, void *uc) {
    if (fixup(si, uc)) return;
    if (g_prev.sa_flags & SA_SIGINFO) { if (g_prev.sa_sigaction) { g_prev.sa_sigaction(sig, si, uc); return; } }
    else if (g_prev.sa_handler != SIG_DFL && g_prev.sa_handler != SIG_IGN) { g_prev.sa_handler(sig); return; }
    signal(sig, SIG_DFL);       /* not ours: fault again with the default action */
}

__attribute__((constructor)) static void strict_trap_install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_bus;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, &g_prev);
}
#endif
