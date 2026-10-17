/* AMD SSE4a (EXTRQ, INSERTQ) on CPUs without it (Intel): the PS4's Jaguar has it, and games
 * compiled for it use a few (Ratchet & Clank's occlusion jobs: insertq). Such an instruction
 * stops the game with SIGILL; the fault handler emulates it here, and an immediate form then
 * jumps to generated code doing the same with plain x86-64, so a hot loop traps only once.
 * BB_SSE4A_PATCH=0: emulate every time, without patching the game's code. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <cpuid.h>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#define POOL_SIZE (64*1024)
static uint8_t *pool;
static size_t pool_used;
static uintptr_t guest_image;
static int patching;
static char busy;
static size_t emulated, patched;

void runtime_sse4a_init(uintptr_t image) {
    unsigned a, b, c, d;
    if (__get_cpuid(0x80000001u, &a, &b, &c, &d) && (c & (1u << 6))) return; /* has SSE4a */
    guest_image = image;
    const char *env = getenv("BB_SSE4A_PATCH");
    patching = !(env && !strcmp(env, "0"));
    if (patching) {
        pool = runtime_low_map(POOL_SIZE, PROT_READ|PROT_WRITE|PROT_EXEC);
        if (!pool) patching = 0;
    }
}

static uint64_t mask_of(unsigned len) { return len ? (UINT64_C(1) << len) - 1 : ~UINT64_C(0); }
static uint64_t low64(const struct _libc_xmmreg *x) { return x->element[0] | (uint64_t)x->element[1] << 32; }
static void set_low64(struct _libc_xmmreg *x, uint64_t v) { x->element[0] = (uint32_t)v; x->element[1] = (uint32_t)(v >> 32); }
static void say(const char *line) { ssize_t n = write(2, line, strlen(line)); (void)n; }

/* movq r64,xmm / pinsrq xmm,r64,0 with rax=0, rcx=1, rdx=2. */
static uint8_t *movq_from(uint8_t *p, unsigned gpr, unsigned xmm) {
    *p++ = 0x66; *p++ = 0x48 | ((xmm >> 3) << 2) | (gpr >> 3);
    *p++ = 0x0f; *p++ = 0x7e; *p++ = 0xc0 | (xmm & 7) << 3 | (gpr & 7);
    return p;
}
static uint8_t *pinsrq_low(uint8_t *p, unsigned xmm, unsigned gpr) {
    *p++ = 0x66; *p++ = 0x48 | ((xmm >> 3) << 2) | (gpr >> 3);
    *p++ = 0x0f; *p++ = 0x3a; *p++ = 0x22; *p++ = 0xc0 | (xmm & 7) << 3 | (gpr & 7); *p++ = 0;
    return p;
}
static uint8_t *mov_rcx(uint8_t *p, uint64_t v) {
    *p++ = 0x48; *p++ = 0xb9; memcpy(p, &v, 8); return p + 8;
}

/* Code doing the instruction at `site` (length `size`), then jumping back after it. Leaves the
 * red zone below rsp alone (the game's code keeps data there) and keeps the flags. */
static uint8_t *trampoline(uintptr_t site, unsigned size, int insert, unsigned dst, unsigned src,
                           unsigned len, unsigned idx) {
    if (!pool || pool_used + 96 > POOL_SIZE) return NULL;
    uint8_t *start = pool + pool_used, *p = start;
    static const uint8_t enter[] = {0x48,0x8d,0x64,0x24,0x80, 0x9c, 0x50, 0x51, 0x52};
    static const uint8_t leave[] = {0x5a, 0x59, 0x58, 0x9d, 0x48,0x8d,0xa4,0x24,0x80,0x00,0x00,0x00};
    memcpy(p, enter, sizeof(enter)); p += sizeof(enter);
    uint64_t mask = mask_of(len);
    if (insert) {
        p = movq_from(p, 0, src);                                   /* rax = src */
        if (idx) { *p++ = 0x48; *p++ = 0xc1; *p++ = 0xe0; *p++ = (uint8_t)idx; } /* shl rax,idx */
        p = mov_rcx(p, mask << idx);
        *p++ = 0x48; *p++ = 0x21; *p++ = 0xc8;                      /* and rax,rcx */
        *p++ = 0x48; *p++ = 0xf7; *p++ = 0xd1;                      /* not rcx */
        p = movq_from(p, 2, dst);                                   /* rdx = dst */
        *p++ = 0x48; *p++ = 0x21; *p++ = 0xca;                      /* and rdx,rcx */
        *p++ = 0x48; *p++ = 0x09; *p++ = 0xc2;                      /* or rdx,rax */
        p = pinsrq_low(p, dst, 2);
    } else {
        p = movq_from(p, 0, dst);
        if (idx) { *p++ = 0x48; *p++ = 0xc1; *p++ = 0xe8; *p++ = (uint8_t)idx; } /* shr rax,idx */
        p = mov_rcx(p, mask);
        *p++ = 0x48; *p++ = 0x21; *p++ = 0xc8;
        p = pinsrq_low(p, dst, 0);
    }
    memcpy(p, leave, sizeof(leave)); p += sizeof(leave);
    int64_t back = (int64_t)(site + size) - (int64_t)((uintptr_t)p + 5);
    if (back != (int32_t)back) return NULL;
    *p++ = 0xe9; int32_t rel = (int32_t)back; memcpy(p, &rel, 4); p += 4;
    pool_used = (size_t)(p - pool + 15) & ~(size_t)15;
    __builtin___clear_cache((char *)start, (char *)p);
    return start;
}

/* The site becomes jmp trampoline (+ nops). A thread reaching it meanwhile spins on a two-byte
 * jmp-to-self until the jump is complete. */
static int patch_site(uint8_t *site, unsigned size, const uint8_t *target) {
    int64_t rel = (int64_t)(uintptr_t)target - (int64_t)((uintptr_t)site + 5);
    if (rel != (int32_t)rel || size < 5) return 0;
    long page = sysconf(_SC_PAGESIZE);
    uintptr_t from = (uintptr_t)site & ~(uintptr_t)(page - 1);
    size_t span = ((uintptr_t)site + size - from + (size_t)page - 1) & ~(size_t)(page - 1);
    if (mprotect((void *)from, span, PROT_READ|PROT_WRITE|PROT_EXEC)) return 0;
    uint8_t bytes[16];
    int32_t r = (int32_t)rel;
    bytes[0] = 0xe9; memcpy(bytes + 1, &r, 4);
    for (unsigned i = 5; i < size; ++i) bytes[i] = 0x90;
    int split = ((uintptr_t)site & 63) == 63; /* a two-byte store would cross a cache line */
    if (!split) __atomic_store_n((uint16_t *)site, (uint16_t)0xfeeb, __ATOMIC_SEQ_CST);
    for (unsigned i = split ? 0 : 2; i < size; ++i) ((volatile uint8_t *)site)[i] = bytes[i];
    if (!split) {
        uint16_t head; memcpy(&head, bytes, 2);
        __atomic_store_n((uint16_t *)site, head, __ATOMIC_SEQ_CST);
    }
    mprotect((void *)from, span, PROT_READ|PROT_EXEC);
    return 1;
}

int runtime_sse4a_fault(void *context) {
    ucontext_t *uc = context;
    uint8_t *b = (uint8_t *)uc->uc_mcontext.gregs[REG_RIP];
    /* Being patched or patched by another thread after this one trapped there (a jump never
     * raises SIGILL itself): run the jump, or spin on the jump-to-self until it is complete. */
    if (patching && (b[0] == 0xe9 || (b[0] == 0xeb && b[1] == 0xfe))) return 1;
    if (!uc->uc_mcontext.fpregs || (b[0] != 0x66 && b[0] != 0xf2)) return 0;
    unsigned i = 1, rex = 0;
    if ((b[1] & 0xf0) == 0x40) { rex = b[1]; i = 2; }
    if (b[i] != 0x0f || (b[i+1] != 0x78 && b[i+1] != 0x79) || (b[i+2] & 0xc0) != 0xc0) return 0;
    const int insert = b[0] == 0xf2, immediate = b[i+1] == 0x78;
    const unsigned modrm = b[i+2];
    const unsigned reg = ((modrm >> 3) & 7) | ((rex & 4) << 1), rm = (modrm & 7) | ((rex & 1) << 3);
    if (!insert && immediate && (modrm & 0x38)) return 0; /* extrq imm is 66 0f 78 /0 */
    const unsigned size = i + 3 + (immediate ? 2 : 0);
    struct _libc_xmmreg *x = uc->uc_mcontext.fpregs->_xmm;
    unsigned dst = insert || !immediate ? reg : rm, src = rm, len, idx;
    if (immediate) { len = b[i+3] & 63; idx = b[i+4] & 63; }
    else if (insert) { len = x[src].element[2] & 63; idx = (x[src].element[2] >> 8) & 63; }
    else { len = x[src].element[0] & 63; idx = (x[src].element[0] >> 8) & 63; }
    uint64_t mask = mask_of(len), d = low64(&x[dst]);
    if (insert) d = (d & ~(mask << idx)) | ((low64(&x[src]) << idx) & (mask << idx));
    else d = (d >> idx) & mask;
    if (immediate && patching) {
        while (__atomic_test_and_set(&busy, __ATOMIC_ACQUIRE)) {}
        /* Another thread patched it meanwhile: run the jump instead. */
        if (b[0] == 0xe9 || b[0] == 0xeb) { __atomic_clear(&busy, __ATOMIC_RELEASE); return 1; }
        uint8_t *code = trampoline((uintptr_t)b, size, insert, dst, src, len, idx);
        int ok = code && patch_site(b, size, code);
        if (ok) ++patched;
        __atomic_clear(&busy, __ATOMIC_RELEASE);
        char line[128];
        snprintf(line, sizeof(line), "Runtime: SSE4a %s at guest +0x%lx %s\n", insert ? "insertq" : "extrq",
                 (unsigned long)((uintptr_t)b - guest_image), ok ? "patched" : "emulated (not patched)");
        say(line);
        if (ok) return 1; /* resume at the site: now the jump */
    }
    set_low64(&x[dst], d);
    __atomic_add_fetch(&emulated, 1, __ATOMIC_RELAXED);
    uc->uc_mcontext.gregs[REG_RIP] += size;
    return 1;
}
void runtime_sse4a_report(void) {
    if (emulated || patched) printf("Runtime: SSE4a sites patched=%zu, emulated=%zu\n", patched, emulated);
}
#else
void runtime_sse4a_init(uintptr_t image) { (void)image; }
int runtime_sse4a_fault(void *context) { (void)context; return 0; }
void runtime_sse4a_report(void) {}
#endif
