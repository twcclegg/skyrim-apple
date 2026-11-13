/* Included by generated files only. */
#pragma once
#include <setjmp.h>
#include "rt_table.h"
/* indirect call through a per-site cache (see lift_terminator in tools/lift.py) */
void rt_call_ic(CPU *c, uint32_t t, const FnEntry **cache);
/* the table entry for a lifted function at t (remembered in *cache), or NULL for an import or unknown target */
const FnEntry *rt_ic_lookup(uint32_t t, const FnEntry **cache);
jmp_buf *rt_setjmp_slot(CPU *c, uint32_t guest_jb);
void rt_setjmp3(CPU *c);
/* Where the eight guest registers live while lifted code runs.
 *
 * Default: in locals of each lifted function, loaded from the CPU struct on entry (GUEST_REGS), written back
 * before every call and return (SPILL) and reread after every call (RELOAD).
 *
 * LB_PINNED (arm64; the generated code is then built with -ffixed-x21 ... -ffixed-x28): in eight host
 * registers that no compiled code in the generated files uses for anything else, so a call between lifted
 * functions moves nothing. The CPU struct is brought up to date only where control leaves lifted code for
 * code that reads it (SPILL_RT / RELOAD_RT: the runtime's dispatcher, imports, host replacements), and the
 * runtime enters lifted code through rt_enter_guest (runtime/dispatch.c), which loads the registers and
 * keeps the host's own values of x21-x28. x21-x28 are callee-saved, so runtime and library code called from
 * lifted code preserves them; whole Darwin save pairs are reserved so no prologue saves and restores one. */
#define REGS_TO_CPU() do { c->r[0] = eax; c->r[1] = ecx; c->r[2] = edx; c->r[3] = ebx; \
    c->r[4] = esp; c->r[5] = ebp; c->r[6] = esi; c->r[7] = edi; } while (0)
#define REGS_FROM_CPU() do { eax = c->r[0]; ecx = c->r[1]; edx = c->r[2]; ebx = c->r[3]; \
    esp = c->r[4]; ebp = c->r[5]; esi = c->r[6]; edi = c->r[7]; } while (0)
/* LB_TRACE_ENTRY (debugging the two register models against each other): every lifted function logs its name
 * and the registers on entry, per host thread, to $LB_TRACE_DIR/t<n>.txt, up to LB_TRACE_MAX lines a thread. */
#ifdef LB_TRACE_ENTRY
#include <stdio.h>
#include <stdlib.h>
__attribute__((weak)) int g_trace_threads;
__attribute__((weak, noinline)) void rt_trace_entry(const char *fn, uint32_t a, uint32_t cx, uint32_t d, uint32_t b,
                                                    uint32_t sp, uint32_t bp, uint32_t si, uint32_t di) {
    static __thread FILE *out; static __thread long left = -1;
    if (left < 0) {
        const char *dir = getenv("LB_TRACE_DIR"), *max = getenv("LB_TRACE_MAX");
        char path[512];
        left = dir ? (max ? atol(max) : 2000000) : 0;
        if (dir) { snprintf(path, sizeof path, "%s/t%d.txt", dir, __atomic_fetch_add(&g_trace_threads, 1, __ATOMIC_SEQ_CST));
                   out = fopen(path, "w"); if (!out) left = 0; }
    }
    if (left <= 0) return;
    if (--left == 0) { fprintf(out, "(limit)\n"); fclose(out); return; }
    fprintf(out, "%s %x %x %x %x %x %x %x %x\n", fn, a, cx, d, b, sp, bp, si, di);
    if ((left & 0xFF) == 0) fflush(out);
}
#define TRACE_ENTRY() rt_trace_entry(__func__, eax, ecx, edx, ebx, esp, ebp, esi, edi)
#else
#define TRACE_ENTRY() ((void)0)
#endif
#ifdef LB_PINNED
#ifndef __aarch64__
#error "LB_PINNED needs arm64"
#endif
register uint32_t eax __asm__("w21"); register uint32_t ecx __asm__("w22");
register uint32_t edx __asm__("w23"); register uint32_t ebx __asm__("w24");
register uint32_t esp __asm__("w25"); register uint32_t ebp __asm__("w26");
register uint32_t esi __asm__("w27"); register uint32_t edi __asm__("w28");
#define GUEST_REGS TRACE_ENTRY()
/* x21-x28 are callee-saved in the host convention, so the compiler takes a call to leave them unchanged
 * and, for one, drops a register write after a call that repeats the value written before it. A lifted
 * callee does change them. The empty asm statement after each call says so; it emits no instruction. */
#define REGS_CHANGED() __asm__ volatile("" : : : "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28")
#ifdef LB_PINNED_CHECK
/* Debugging: also keep the CPU struct current, and check at every function entry and after every call that
 * the registers and the struct agree. A mismatch means lifted code was entered or left by a path that
 * skipped the register hand-over. */
#include <stdio.h>
#include <stdlib.h>
__attribute__((weak, noinline, noreturn)) void rt_pin_fail(CPU *c, const char *fn, const char *where,
        uint32_t a, uint32_t cx, uint32_t d, uint32_t b, uint32_t sp, uint32_t bp, uint32_t si, uint32_t di) {
    fprintf(stderr, "pinned-register mismatch in %s (%s)\n  regs %x %x %x %x %x %x %x %x\n  cpu  %x %x %x %x %x %x %x %x\n",
            fn, where, a, cx, d, b, sp, bp, si, di,
            c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6], c->r[7]);
    abort();
}
#define PIN_CHECK(where) do { if (eax != c->r[0] || ecx != c->r[1] || edx != c->r[2] || ebx != c->r[3] || \
    esp != c->r[4] || ebp != c->r[5] || esi != c->r[6] || edi != c->r[7]) \
    rt_pin_fail(c, __func__, where, eax, ecx, edx, ebx, esp, ebp, esi, edi); } while (0)
#undef GUEST_REGS
#define GUEST_REGS PIN_CHECK("entry"); TRACE_ENTRY()
#define SPILL() REGS_TO_CPU()
#define RELOAD() do { REGS_CHANGED(); PIN_CHECK("after call"); } while (0)
#else
#define SPILL() ((void)0)
#define RELOAD() REGS_CHANGED()
#endif
#define SPILL_RT() REGS_TO_CPU()
#define RELOAD_RT() do { REGS_CHANGED(); REGS_FROM_CPU(); } while (0)
#define HOST_ENTRY(fn) do { REGS_TO_CPU(); fn(c); REGS_CHANGED(); REGS_FROM_CPU(); } while (0)
#else
#define GUEST_REGS uint32_t eax = c->r[0], ecx = c->r[1], edx = c->r[2], ebx = c->r[3], \
    esp = c->r[4], ebp = c->r[5], esi = c->r[6], edi = c->r[7]; TRACE_ENTRY()
#define SPILL() REGS_TO_CPU()
#define RELOAD() REGS_FROM_CPU()
#define SPILL_RT() REGS_TO_CPU()
#define RELOAD_RT() REGS_FROM_CPU()
#define HOST_ENTRY(fn) fn(c)
#endif
/* Guest atomics (xchg with memory, lock-prefixed instructions, cmpxchg; see tools/lift.py). x86 allows them
 * at any address. ARM's atomic instructions fault when the address isn't a multiple of the operand size (SIGBUS
 * on Apple CPUs, where it kills the game), so those rare cases do the operation under one process-wide lock
 * instead: correct against each other, and against aligned atomics as far as a plain read or write is. */
#include <sched.h>
__attribute__((weak)) int g_ga_lock;
static inline void ga_lock(void) { while (__atomic_exchange_n(&g_ga_lock, 1, __ATOMIC_ACQUIRE)) sched_yield(); }
static inline void ga_unlock(void) { __atomic_store_n(&g_ga_lock, 0, __ATOMIC_RELEASE); }
#define GA_ALIGNED(p) __builtin_expect(!((uintptr_t)(p) & (sizeof *(p) - 1)), 1)
#define GA_LOAD(p) __extension__({ __typeof__(p) p_ = (p); __typeof__(*p_) r_; \
    if (GA_ALIGNED(p_)) r_ = __atomic_load_n(p_, __ATOMIC_SEQ_CST); \
    else { ga_lock(); memcpy(&r_, p_, sizeof r_); ga_unlock(); } r_; })
#define GA_XCHG(p, v) __extension__({ __typeof__(p) p_ = (p); __typeof__(*p_) r_, v_ = (v); \
    if (GA_ALIGNED(p_)) r_ = __atomic_exchange_n(p_, v_, __ATOMIC_SEQ_CST); \
    else { ga_lock(); memcpy(&r_, p_, sizeof r_); memcpy(p_, &v_, sizeof v_); ga_unlock(); } r_; })
#define GA_FETCH(builtin, op, p, v) __extension__({ __typeof__(p) p_ = (p); __typeof__(*p_) r_, v_ = (v), n_; \
    if (GA_ALIGNED(p_)) r_ = builtin(p_, v_, __ATOMIC_SEQ_CST); \
    else { ga_lock(); memcpy(&r_, p_, sizeof r_); n_ = (__typeof__(n_))(r_ op v_); memcpy(p_, &n_, sizeof n_); ga_unlock(); } r_; })
#define GA_FETCH_ADD(p, v) GA_FETCH(__atomic_fetch_add, +, p, v)
#define GA_FETCH_OR(p, v) GA_FETCH(__atomic_fetch_or, |, p, v)
#define GA_FETCH_AND(p, v) GA_FETCH(__atomic_fetch_and, &, p, v)
#define GA_FETCH_XOR(p, v) GA_FETCH(__atomic_fetch_xor, ^, p, v)
/* compare-and-swap: like __atomic_compare_exchange_n, *exp receives the value found when it differs */
#define GA_CAS(p, exp, des) __extension__({ __typeof__(p) p_ = (p); __typeof__(*p_) *e_ = (exp), d_ = (des), c_; int ok_; \
    if (GA_ALIGNED(p_)) ok_ = __atomic_compare_exchange_n(p_, e_, d_, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
    else { ga_lock(); memcpy(&c_, p_, sizeof c_); ok_ = c_ == *e_; \
           if (ok_) memcpy(p_, &d_, sizeof d_); else *e_ = c_; ga_unlock(); } ok_; })

/* Stack slots kept in locals (LIFT_SLOTS=1 in tools/lift.py; docs/STACK_SLOTS.md). `v` is the local for the
 * cell `o` bytes from `fb_`, the function's esp on entry; `a` is the guest address the x86 operand names,
 * which the normal build ignores.
 *
 * SL_GUARD follows a call whose effect on esp is solved or assumed rather than proven, and every call made
 * while ebp is the frame pointer: if the callee left either somewhere else, the slot numbering after the
 * call would be wrong, so stop there instead of running on.
 *
 * -DLB_SLOT_CHECK (debugging) also keeps every promoted cell in guest memory, XORed with a key, and on
 * each read compares the two and compares `a` with the cell's address. A callee or a stray pointer that
 * writes a promoted cell, a wrong stack height, and (through the key) code that reads a promoted cell
 * from memory all show up, the first two here and the last as a wrong result. */
/* x87, SSE and MMX values in promoted cells: the local holds the bytes guest memory would hold */
static inline float sl_f32(uint32_t u) { float f; __builtin_memcpy(&f, &u, 4); return f; }
static inline double sl_f64(uint64_t u) { double d; __builtin_memcpy(&d, &u, 8); return d; }
static inline uint32_t sl_u32(float f) { uint32_t u; __builtin_memcpy(&u, &f, 4); return u; }
static inline uint64_t sl_u64(double d) { uint64_t u; __builtin_memcpy(&u, &d, 8); return u; }
#define SL_GUARD(ok, at) do { if (__builtin_expect(!(ok), 0)) { SPILL_RT(); \
    rt_unimpl(c, at, "stack-slot guard: esp or ebp is not where the lifter expects it after this call"); } } while (0)
#ifndef LB_SLOT_CHECK
#define SL_INIT(n, v, o) uint##n##_t v = 0
#define SL_R(n, v, o, a) (v)
#define SL_W(n, v, o, a, x) (v = (uint##n##_t)(x))
#else
#include <stdio.h>
#include <stdlib.h>
#define SL_KEY 0x5AC3965Au
__attribute__((weak, noinline, noreturn)) void rt_slot_fail(const char *fn, const char *what, uint32_t cell,
                                                            uint32_t addr, uint32_t local, uint32_t mem) {
    fprintf(stderr, "stack-slot check failed in %s: %s (cell at %08x, operand address %08x, local %08x, memory %08x)\n",
            fn, what, cell, addr, local, mem);
    abort();
}
#define SL_INIT(n, v, o) uint##n##_t v = 0; M##n(fb_ + (uint32_t)(o)) = (uint##n##_t)SL_KEY
#define SL_R(n, v, o, a) __extension__({ uint32_t p_ = fb_ + (uint32_t)(o), a_ = (uint32_t)(a); \
    if (a_ != p_) rt_slot_fail(__func__, "a read is not where the lifter placed it", p_, a_, v, 0); \
    if (M##n(p_) != (uint##n##_t)(v ^ SL_KEY)) \
        rt_slot_fail(__func__, "guest memory changed under a promoted cell", p_, a_, v, M##n(p_) ^ (uint##n##_t)SL_KEY); \
    v; })
#define SL_W(n, v, o, a, x) __extension__({ uint##n##_t x_ = (uint##n##_t)(x); \
    uint32_t p_ = fb_ + (uint32_t)(o), a_ = (uint32_t)(a); \
    if (a_ != p_) rt_slot_fail(__func__, "a write is not where the lifter placed it", p_, a_, x_, 0); \
    v = x_; M##n(p_) = (uint##n##_t)(x_ ^ SL_KEY); })
#endif

/* Functions that keep the x87 top index in the local `ft` (X87CACHE in tools/lift.py). */
#define SPILLF() do { SPILL(); c->top = ft; } while (0)
#define RELOADF() do { RELOAD(); ft = c->top; } while (0)
#define SPILLF_RT() do { SPILL_RT(); c->top = ft; } while (0)
#define RELOADF_RT() do { RELOAD_RT(); ft = c->top; } while (0)
static inline void rt_movsd(uint32_t n, uint32_t dst, uint32_t src) {
    /* forward rep movsd; overlapping copies must behave like the dword loop */
    if (dst <= src || dst >= src + n * 4) memmove(MEM + dst, MEM + src, (size_t)n * 4);
    else for (uint32_t k = 0; k < n; k++) M32(dst + 4 * k) = M32(src + 4 * k);
}
#pragma GCC diagnostic ignored "-Wunused-label"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"

/* Memory ordering for multi-core ARM (see mem_order in tools/lift.py). */
#if defined(__aarch64__)
#define ST_FENCE() __asm__ volatile("dmb ishst" ::: "memory")
#define FULL_FENCE() __asm__ volatile("dmb ish" ::: "memory")
#define LD_FENCE() __asm__ volatile("dmb ishld" ::: "memory")
#else
#define ST_FENCE() ((void)0)
#define FULL_FENCE() ((void)0)
#define LD_FENCE() ((void)0)
#endif

/* Guest memory other threads can see (see TSO in tools/lift.py).
 *
 * Default (0.21+): plain loads/stores, but volatile so the compiler keeps
 * every access the x86 code makes (no hoisting a flag read out of a spin
 * loop, no merging). Lock-prefixed instructions, xchg and the Interlocked*
 * imports stay atomic. 0.17-0.20 used load-acquire/store-release for full x86
 * ordering; the multi-core crashes that motivated it turned out to be a
 * dispatcher race, and ldar/stlr on every access cost the main thread a lot.
 *
 * -DLB_STRICT_ORDER brings back x86 (TSO) ordering: load-acquire /
 * store-release on aligned addresses; an unaligned access (no ldar/stlr
 * form) is a plain access with a fence. */
#if !defined(LB_STRICT_ORDER) && (defined(__aarch64__) || defined(LB_GUEST_VOLATILE))
/* LB_GUEST_VOLATILE: the same volatile accesses on other hosts, for tests run there */
#define TL8(a) (*(volatile uint8_t *)(MEM + (uint32_t)(a)))
#define TL16(a) (*(volatile u16u *)(MEM + (uint32_t)(a)))
#define TL32(a) (*(volatile u32u *)(MEM + (uint32_t)(a)))
#define TS8(a, v) (*(volatile uint8_t *)(MEM + (uint32_t)(a)) = (v))
#define TS16(a, v) (*(volatile u16u *)(MEM + (uint32_t)(a)) = (v))
#define TS32(a, v) (*(volatile u32u *)(MEM + (uint32_t)(a)) = (v))
#elif defined(__aarch64__)
static inline uint8_t TL8(uint32_t a) { return __atomic_load_n(MEM + a, __ATOMIC_ACQUIRE); }
static inline void TS8(uint32_t a, uint8_t v) { __atomic_store_n(MEM + a, v, __ATOMIC_RELEASE); }
#define TSO_PAIR(n, T, mask) \
static inline T TL##n(uint32_t a) { \
    if (__builtin_expect(!(a & mask), 1)) return __atomic_load_n((T *)(MEM + a), __ATOMIC_ACQUIRE); \
    T v; memcpy(&v, MEM + a, sizeof v); __atomic_thread_fence(__ATOMIC_ACQUIRE); return v; } \
static inline void TS##n(uint32_t a, T v) { \
    if (__builtin_expect(!(a & mask), 1)) { __atomic_store_n((T *)(MEM + a), v, __ATOMIC_RELEASE); return; } \
    __atomic_thread_fence(__ATOMIC_RELEASE); memcpy(MEM + a, &v, sizeof v); }
TSO_PAIR(16, uint16_t, 1)
TSO_PAIR(32, uint32_t, 3)
#else
#define TL8(a) M8(a)
#define TL16(a) M16(a)
#define TL32(a) M32(a)
#define TS8(a, v) (M8(a) = (v))
#define TS16(a, v) (M16(a) = (v))
#define TS32(a, v) (M32(a) = (v))
#endif

/* Guest memory in functions with no loop (LIFT_PLAIN_ACYCLIC=1 in tools/lift.py, docs/HEAP_ACCESSES.md):
 * plain accesses, except in the strict build, which keeps x86 ordering everywhere. */
#ifdef LB_STRICT_ORDER
#define PL8(a) TL8(a)
#define PL16(a) TL16(a)
#define PL32(a) TL32(a)
#define PS8(a, v) TS8(a, v)
#define PS16(a, v) TS16(a, v)
#define PS32(a, v) TS32(a, v)
#else
#define PL8(a) M8(a)
#define PL16(a) M16(a)
#define PL32(a) M32(a)
#define PS8(a, v) (M8(a) = (v))
#define PS16(a, v) (M16(a) = (v))
#define PS32(a, v) (M32(a) = (v))
#endif
