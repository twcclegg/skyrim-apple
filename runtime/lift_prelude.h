/* Included by generated files only. */
#pragma once
#include <setjmp.h>
#include "rt_table.h"
#include "import_inline.h"
/* indirect call through a per-site cache (see lift_terminator in tools/lift.py) */
void rt_call_ic(CPU *c, uint32_t t, const FnEntry **cache);
jmp_buf *rt_setjmp_slot(CPU *c, uint32_t guest_jb);
void rt_setjmp3(CPU *c);
#define SPILL() do { c->r[0] = eax; c->r[1] = ecx; c->r[2] = edx; c->r[3] = ebx; \
    c->r[4] = esp; c->r[5] = ebp; c->r[6] = esi; c->r[7] = edi; } while (0)
#define RELOAD() do { eax = c->r[0]; ecx = c->r[1]; edx = c->r[2]; ebx = c->r[3]; \
    esp = c->r[4]; ebp = c->r[5]; esi = c->r[6]; edi = c->r[7]; } while (0)
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

/* Functions that keep the x87 top index in the local `ft` (X87CACHE in tools/lift.py). */
#define SPILLF() do { SPILL(); c->top = ft; } while (0)
#define RELOADF() do { RELOAD(); ft = c->top; } while (0)
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
#if defined(__aarch64__) && !defined(LB_STRICT_ORDER)
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
