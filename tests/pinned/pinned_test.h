/* Shared header for corpus programs (tests/pinned/SPEC.md). Eight global register variables in x21-x28,
 * as the pinned build of lifted code uses them. Build with -ffixed-x21 ... -ffixed-x28. */
#ifndef PINNED_TEST_H
#define PINNED_TEST_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
register uint32_t eax __asm__("w21"); register uint32_t ecx __asm__("w22");
register uint32_t edx __asm__("w23"); register uint32_t ebx __asm__("w24");
register uint32_t esp __asm__("w25"); register uint32_t ebp __asm__("w26");
register uint32_t esi __asm__("w27"); register uint32_t edi __asm__("w28");
/* The compiler takes x21-x28 as unchanged by a call. Any call to a function that may write a register
 * variable must go through CALL (value) or CALLV (no value), which say that the registers changed. */
#define CLOB() __asm__ volatile("" : : : "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28")
#define CALL(x) ({ __typeof__(x) v_ = (x); CLOB(); v_; })
#define CALLV(x) do { x; CLOB(); } while (0)
#define MEM_WORDS 256
static uint32_t mem[MEM_WORDS];
#define M(i) mem[(uint32_t)(i) % MEM_WORDS]
static uint32_t pin_saved_[8];
#define PIN_BEGIN() do { pin_saved_[0] = eax; pin_saved_[1] = ecx; pin_saved_[2] = edx; pin_saved_[3] = ebx; \
    pin_saved_[4] = esp; pin_saved_[5] = ebp; pin_saved_[6] = esi; pin_saved_[7] = edi; } while (0)
#define PIN_END() do { eax = pin_saved_[0]; ecx = pin_saved_[1]; edx = pin_saved_[2]; ebx = pin_saved_[3]; \
    esp = pin_saved_[4]; ebp = pin_saved_[5]; esi = pin_saved_[6]; edi = pin_saved_[7]; CLOB(); } while (0)
#define SET_REGS(a, c, d, b, sp, bp, si, di) do { eax = (a); ecx = (c); edx = (d); ebx = (b); \
    esp = (sp); ebp = (bp); esi = (si); edi = (di); CLOB(); } while (0)
/* Print the eight registers, an extra value and a hash of mem. Call it after every scenario. */
#define REPORT(tag, v) do { uint32_t g_[8] = {eax, ecx, edx, ebx, esp, ebp, esi, edi}; uint32_t h_ = 2166136261u; \
    for (int i_ = 0; i_ < MEM_WORDS; i_++) h_ = (h_ ^ mem[i_]) * 16777619u; \
    printf("%s %08x %08x %08x %08x %08x %08x %08x %08x v=%08x m=%08x\n", tag, g_[0], g_[1], g_[2], g_[3], \
           g_[4], g_[5], g_[6], g_[7], (uint32_t)(v), h_); } while (0)
#endif
