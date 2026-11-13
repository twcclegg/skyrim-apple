/* Windows imports that tools/lift.py emits inline at their call sites (INLINE_IMPORTS there), shared with the
 * shims that serve every other caller (kernel32.c, skyrim_shims.c), so the two can't drift apart. They are the
 * busiest imports by far: in a heavy fight the game makes millions of these calls a second, and through the import
 * dispatcher each one also paid a return-address push, a register spill and reload, and watchdog bookkeeping.
 * Each takes the call's guest arguments and returns what goes in eax. */
#pragma once
#include "x86.h"

#define IMP_TLS_SLOTS (64 + 512)
/* TEB offsets: TlsSlots[64] at 0xE10, TlsExpansionSlots at 0x1800 (kept inline in our TEB page) */
static inline uint32_t imp_tls_off(uint32_t i) { return i < 64 ? 0xE10 + 4 * i : 0x1800 + 4 * (i - 64); }

static inline uint32_t imp_InterlockedIncrement(uint32_t p) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32_t r = __atomic_add_fetch((uint32_t *)(MEM + p), 1, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return r;
}
static inline uint32_t imp_InterlockedDecrement(uint32_t p) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32_t r = __atomic_sub_fetch((uint32_t *)(MEM + p), 1, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return r;
}
static inline uint32_t imp_InterlockedExchange(uint32_t p, uint32_t v) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32_t r = __atomic_exchange_n((uint32_t *)(MEM + p), v, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return r;
}
static inline uint32_t imp_InterlockedExchangeAdd(uint32_t p, uint32_t v) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32_t r = __atomic_fetch_add((uint32_t *)(MEM + p), v, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return r;
}
static inline uint32_t imp_InterlockedCompareExchange(uint32_t p, uint32_t exchange, uint32_t comparand) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_compare_exchange_n((uint32_t *)(MEM + p), &comparand, exchange, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return comparand;
}
static inline uint32_t imp_GetCurrentThreadId(CPU *c) { return M32(c->fs_base + 0x24); }
static inline uint32_t imp_GetLastError(CPU *c) { return M32(c->fs_base + 0x34); }
static inline void imp_SetLastError(CPU *c, uint32_t e) { M32(c->fs_base + 0x34) = e; }
static inline uint32_t imp_TlsGetValue(CPU *c, uint32_t i) {
    imp_SetLastError(c, 0);
    return i < IMP_TLS_SLOTS ? M32(c->fs_base + imp_tls_off(i)) : 0;
}
