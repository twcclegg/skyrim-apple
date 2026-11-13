/* Models stack shapes the C compilers used for this corpus do not produce but MSVC does: a local made by
 * `push ecx`, arguments removed by `pop ecx`, callee-popped calls through pointers, an ebp frame with the
 * saved registers below the locals, a helper that moves its caller's stack pointer. Written in assembly for
 * x86 and again in C, which is what the host check runs. */
uint32_t gtab[8];
#define ASMFN __attribute__((cdecl)) uint32_t     /* the assembly below is cdecl whatever the build's default */
ASMFN p1(uint32_t a, uint32_t b); ASMFN p2(uint32_t a, uint32_t b); ASMFN p3(uint32_t a, uint32_t b);
ASMFN p4(uint32_t a, uint32_t b); ASMFN p5(uint32_t a, uint32_t b); ASMFN p6(uint32_t a, uint32_t b);
ASMFN p7(uint32_t a, uint32_t b); ASMFN p8(uint32_t a, uint32_t b); ASMFN p9(uint32_t a, uint32_t b);
#ifdef __i386__
__asm__(".intel_syntax noprefix\n"
".data\n"
"vt: .long s_add, s_mul3\n"
"vc: .long c_sub, c_neg1\n"
".text\n"
"helper1: mov eax, [esp+4]\n lea eax, [eax+eax*2+1]\n mov dword ptr [esp+4], 0xDEAD\n ret\n"
"s_add: mov eax, [esp+4]\n add eax, [esp+8]\n mov dword ptr [esp+4], 0\n mov dword ptr [esp+8], 0\n ret 8\n"
"s_mul3: mov eax, [esp+4]\n lea eax, [eax+eax*2]\n ret 4\n"
"c_sub: mov eax, [esp+4]\n sub eax, [esp+8]\n mov dword ptr [esp+8], 0x1111\n ret\n"
"c_neg1: mov eax, [esp+4]\n not eax\n ret\n"
"tailfn: add eax, [esp+4]\n sub eax, [esp+8]\n ret\n"
"alloc8: pop eax\n sub esp, 8\n push eax\n ret\n"
/* a local made by push ecx, registers saved below it, an argument removed by pop ecx */
".globl p1\np1:\n push ecx\n push esi\n push edi\n mov esi, [esp+16]\n mov edi, [esp+20]\n mov [esp+8], esi\n"
" push edi\n call helper1\n pop ecx\n add eax, [esp+8]\n mov dword ptr [gtab+4], eax\n add [esp+8], edi\n"
" mov eax, [esp+8]\n pop edi\n pop esi\n pop ecx\n ret\n"
/* ebp frame, locals above the saved registers, two callee-popped calls through pointers in a loop */
".globl p2\np2:\n push ebp\n mov ebp, esp\n sub esp, 12\n push ebx\n push esi\n push edi\n mov esi, [ebp+8]\n"
" mov edi, [ebp+12]\n mov [ebp-4], esi\n mov [ebp-8], edi\n mov dword ptr [ebp-12], 0\n mov ebx, 3\n"
"1: push dword ptr [ebp-8]\n push dword ptr [ebp-4]\n call dword ptr [vt]\n mov [ebp-4], eax\n push ebx\n"
" call dword ptr [vt+4]\n add [ebp-12], eax\n mov ecx, [ebp-12]\n mov dword ptr [gtab+8], ecx\n dec ebx\n jnz 1b\n"
" mov eax, [ebp-12]\n add eax, [ebp-4]\n pop edi\n pop esi\n pop ebx\n mov esp, ebp\n pop ebp\n ret\n"
/* esp frame, one callee-popped call through a pointer, locals read at esp offsets after it */
".globl p3\np3:\n sub esp, 8\n mov eax, [esp+12]\n mov [esp], eax\n mov ecx, [esp+16]\n mov [esp+4], ecx\n push ecx\n"
" push eax\n call dword ptr [vt]\n add eax, [esp]\n xor eax, [esp+4]\n mov dword ptr [gtab+12], eax\n add esp, 8\n ret\n"
/* a direct callee-popped call, then a tail jump that reuses the incoming arguments */
".globl p4\np4:\n push esi\n mov esi, [esp+8]\n push dword ptr [esp+12]\n push esi\n call s_add\n lea eax, [eax+esi*4]\n"
" pop esi\n jmp tailfn\n"
/* a helper that returns with esp eight bytes lower (the shape of _alloca and the SEH prologue helpers) */
".globl p5\np5:\n call alloc8\n mov eax, [esp+12]\n mov [esp], eax\n mov eax, [esp+16]\n mov [esp+4], eax\n mov eax, [esp]\n"
" imul eax, [esp+4]\n add esp, 8\n ret\n"
/* two caller-cleaned calls through pointers, with a local read between and after them */
".globl p6\np6:\n sub esp, 8\n mov eax, [esp+12]\n mov [esp], eax\n mov eax, [esp+16]\n mov [esp+4], eax\n"
" push dword ptr [esp+4]\n push dword ptr [esp+4]\n call dword ptr [vc]\n add esp, 8\n mov [esp], eax\n"
" push dword ptr [esp+4]\n call dword ptr [vc+4]\n add esp, 4\n add eax, [esp]\n add esp, 8\n ret\n"
/* an argument pushed from, and popped back into, a register that is also saved: the callee's write to
 * its argument must be what comes back */
".globl p7\np7:\n push esi\n mov esi, [esp+8]\n add esi, [esp+12]\n push esi\n call helper1\n pop esi\n add eax, esi\n"
" pop esi\n ret\n"
/* locals read back, the frame released, then arguments for a last call pushed into the cells the locals
 * had (a merged epilogue that ends in a call does this): callee-popped, then caller-cleaned */
".globl p8\np8:\n sub esp, 12\n mov eax, [esp+16]\n mov [esp+8], eax\n mov ecx, [esp+20]\n mov [esp], ecx\n"
" mov eax, [esp+8]\n add eax, [esp]\n mov dword ptr [gtab+16], eax\n mov ecx, [esp]\n add esp, 12\n push eax\n"
" push ecx\n call dword ptr [vt]\n ret\n"
".globl p9\np9:\n sub esp, 12\n mov eax, [esp+16]\n mov [esp+8], eax\n mov ecx, [esp+20]\n mov [esp], ecx\n"
" mov eax, [esp+8]\n add eax, [esp]\n mov dword ptr [gtab+20], eax\n mov ecx, [esp]\n add esp, 12\n push eax\n"
" push ecx\n call dword ptr [vc]\n add esp, 8\n ret\n"
".att_syntax\n");
#else
ASMFN p1(uint32_t a, uint32_t b) { uint32_t local = a, t = b * 3 + 1 + local; gtab[1] = t; local += b; return local; }
ASMFN p2(uint32_t a, uint32_t b) {
    uint32_t x = a, sum = 0;
    for (uint32_t i = 3; i; i--) { x = x + b; sum += i * 3; gtab[2] = sum; }
    return sum + x;
}
ASMFN p3(uint32_t a, uint32_t b) { uint32_t r = ((a + b) + a) ^ b; gtab[3] = r; return r; }
ASMFN p4(uint32_t a, uint32_t b) { return (a + b) + a * 4 + a - b; }
ASMFN p5(uint32_t a, uint32_t b) { return a * b; }
ASMFN p6(uint32_t a, uint32_t b) { return ~b + (a - b); }
ASMFN p7(uint32_t a, uint32_t b) { return (a + b) * 3 + 1 + 0xDEAD; }
ASMFN p8(uint32_t a, uint32_t b) { gtab[4] = a + b; return b + (a + b); }
ASMFN p9(uint32_t a, uint32_t b) { gtab[5] = a + b; return b - (a + b); }
#endif

__attribute__((noinline)) static uint32_t fold(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
__attribute__((noinline)) static uint32_t all(uint32_t a, uint32_t b) {
    uint32_t h = 2166136261u;
    h = fold(h, p1(a, b)); h = fold(h, p2(a, b)); h = fold(h, p3(a, b)); h = fold(h, p4(a, b));
    h = fold(h, p5(a, b)); h = fold(h, p6(a, b)); h = fold(h, p7(a, b));
    h = fold(h, p8(a, b)); h = fold(h, p9(a, b));
    return h;
}

uint32_t test_main(uint32_t a, uint32_t b) {
    uint32_t r = all(a, b);
    r = fold(r, all(b + 1, r));
    return r + gtab[1] + gtab[2] + gtab[3] + gtab[4] + gtab[5];
}
