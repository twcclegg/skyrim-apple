/* Included ahead of every x86 corpus program (tests/aot/X86SPEC.md). No libc: the three functions the
 * compiler may call on its own are defined here. */
#ifndef X86PROG_H
#define X86PROG_H
typedef unsigned char uint8_t; typedef unsigned short uint16_t; typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef signed char int8_t; typedef short int16_t; typedef int int32_t; typedef long long int64_t;
typedef __SIZE_TYPE__ size_t;
__attribute__((noinline, used)) void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; return d; }
__attribute__((noinline, used)) void *memset(void *d, int v, size_t n) {
    uint8_t *a = d; while (n--) *a++ = (uint8_t)v; return d; }
__attribute__((noinline, used)) void *memmove(void *d, const void *s, size_t n) {
    uint8_t *a = d; const uint8_t *b = s;
    if (a < b) while (n--) *a++ = *b++; else { a += n; b += n; while (n--) *--a = *--b; } return d; }
uint32_t test_main(uint32_t a, uint32_t b);
/* x86 calling conventions: clang rejects regparm outright on other targets (the host check builds the same
 * source natively), and warns about stdcall and fastcall. Programs use these instead. */
#ifdef __i386__
#define X86_STDCALL __attribute__((stdcall))
#define X86_FASTCALL __attribute__((fastcall))
#define X86_REGPARM(n) __attribute__((regparm(n)))
#else
#define X86_STDCALL
#define X86_FASTCALL
#define X86_REGPARM(n)
#endif
#endif
