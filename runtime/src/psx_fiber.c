/*
 * psx_fiber.c — Win32-Fiber / Nintendo 3DS (ARM register swap) / POSIX-ucontext
 * backends for psx_fiber.h.
 */
#include "psx_fiber.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

psx_fiber_t psx_fiber_convert_thread(void)
{
    /* Attempt the conversion unconditionally and check the error code on
     * failure. The 0x1E00000000000000 TEB sentinel for "not yet a fiber"
     * is MSVC-internal and not guaranteed by MinGW's GetCurrentFiber()
     * implementation — reading it before ConvertThreadToFiber is called
     * can return an unspecified TEB value that isn't the sentinel, causing
     * the sentinel check to silently skip ConvertThreadToFiber, leaving
     * the thread as a non-fiber, and crashing the first SwitchToFiber. */
    void* fib = ConvertThreadToFiber(NULL);
    if (!fib && GetLastError() == ERROR_ALREADY_FIBER)
        fib = GetCurrentFiber();
    return (psx_fiber_t)fib;
}

psx_fiber_t psx_fiber_current(void)      { return (psx_fiber_t)GetCurrentFiber(); }

psx_fiber_t psx_fiber_create(size_t stack_size, psx_fiber_entry entry, void* arg)
{
    return (psx_fiber_t)CreateFiber((SIZE_T)stack_size,
                                    (LPFIBER_START_ROUTINE)entry, arg);
}

void psx_fiber_switch(psx_fiber_t target) { SwitchToFiber((LPVOID)target); }
void psx_fiber_destroy(psx_fiber_t fiber) { if (fiber) DeleteFiber((LPVOID)fiber); }

#elif defined(__3DS__)

/* Nintendo 3DS (ARMv6K, AAPCS hard-float). newlib has no ucontext, so the
 * switch is a hand-written callee-saved register swap: r4-r11, sp, lr and the
 * VFP registers d8-d15 that the hard-float ABI requires a callee to preserve.
 * Everything else is caller-saved, so the C caller of psx_fiber_swap has
 * already spilled it. Cooperative and single-threaded, like the other
 * backends. */

#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>

#define PSX_FIBER_MIN_STACK (64u * 1024u)

typedef struct psx_fiber_impl {
    uint32_t        core[10];    /* r4-r11, sp, lr      (byte offsets 0..39) */
    uint64_t        vfp[8];      /* d8-d15              (byte offset 40)     */
    void*           stack;       /* NULL for the thread-fiber                */
    psx_fiber_entry entry;
    void*           arg;
} psx_fiber_impl;

_Static_assert(__builtin_offsetof(psx_fiber_impl, vfp) == 40,
               "psx_fiber_swap hard-codes the d8-d15 offset");

static psx_fiber_impl* s_current = NULL;

/* Save the running fiber's callee-saved state into *from, load *to's, and
 * return into *to (bx lr). */
__attribute__((naked, noinline))
static void psx_fiber_swap(psx_fiber_impl* from, psx_fiber_impl* to)
{
    (void)from; (void)to;
    __asm__ volatile(
        "add    r2, r0, #40      \n\t"
        "vstmia r2, {d8-d15}     \n\t"
        "stmia  r0, {r4-r11}     \n\t"
        "mov    r2, sp           \n\t"
        "str    r2, [r0, #32]    \n\t"
        "str    lr, [r0, #36]    \n\t"
        "add    r2, r1, #40      \n\t"
        "vldmia r2, {d8-d15}     \n\t"
        "ldmia  r1, {r4-r11}     \n\t"
        "ldr    r2, [r1, #32]    \n\t"
        "mov    sp, r2           \n\t"
        "ldr    lr, [r1, #36]    \n\t"
        "bx     lr               \n\t");
}

static void psx_fiber_run(psx_fiber_impl* f) __attribute__((noreturn, used));
static void psx_fiber_run(psx_fiber_impl* f)
{
    f->entry(f->arg);
    /* The BIOS thread entry never returns normally (it switches back to its
     * scheduler target, or trap_crashes). Reaching here is a bug. */
    abort();
}

/* First instruction a new fiber executes: psx_fiber_create parks the fiber
 * pointer in r4 and this address in lr. */
__attribute__((naked, noinline, used))
static void psx_fiber_bootstrap(void)
{
    __asm__ volatile(
        "mov r0, r4              \n\t"
        "bl  psx_fiber_run       \n\t");
}

psx_fiber_t psx_fiber_convert_thread(void)
{
    if (!s_current) {
        psx_fiber_impl* f = (psx_fiber_impl*)calloc(1, sizeof(*f));
        if (!f) return NULL;
        s_current = f;   /* runs on the real thread stack; state saved on first switch */
    }
    return (psx_fiber_t)s_current;
}

psx_fiber_t psx_fiber_current(void) { return (psx_fiber_t)s_current; }

psx_fiber_t psx_fiber_create(size_t stack_size, psx_fiber_entry entry, void* arg)
{
    if (stack_size < PSX_FIBER_MIN_STACK) stack_size = PSX_FIBER_MIN_STACK;
    stack_size = (stack_size + 7u) & ~(size_t)7u;
    psx_fiber_impl* f = (psx_fiber_impl*)calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->stack = memalign(8, stack_size);
    if (!f->stack) { free(f); return NULL; }
    f->entry = entry;
    f->arg   = arg;
    f->core[0] = (uint32_t)(uintptr_t)f;                                   /* r4 */
    f->core[8] = (uint32_t)(uintptr_t)((uint8_t*)f->stack + stack_size);  /* sp, 8-aligned */
    f->core[9] = (uint32_t)(uintptr_t)psx_fiber_bootstrap;                 /* lr */
    return (psx_fiber_t)f;
}

void psx_fiber_switch(psx_fiber_t target)
{
    psx_fiber_impl* to   = (psx_fiber_impl*)target;
    psx_fiber_impl* from = s_current;
    if (!to || to == from) return;
    s_current = to;
    psx_fiber_swap(from, to);
    /* Resumed: s_current was set back to `from` by whoever switched here. */
}

void psx_fiber_destroy(psx_fiber_t fiber)
{
    psx_fiber_impl* f = (psx_fiber_impl*)fiber;
    if (!f) return;
    free(f->stack);
    free(f);
}

#else /* POSIX: ucontext */

#ifndef _XOPEN_SOURCE
#  define _XOPEN_SOURCE 700
#endif
#ifdef __APPLE__
#  define _DARWIN_C_SOURCE 1
#endif

#include <ucontext.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <signal.h>    /* SIGSTKSZ — minimum fiber stack floor */

/* glibc >= 2.34 no longer exposes SIGSTKSZ as a compile-time constant under
 * _XOPEN_SOURCE; provide a portable fallback (used only as a stack floor). */
#ifndef SIGSTKSZ
#  define SIGSTKSZ 16384
#endif

#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

typedef struct psx_fiber_impl {
    ucontext_t      ctx;
    void*           stack;    /* NULL for the thread-fiber */
    psx_fiber_entry entry;
    void*           arg;
} psx_fiber_impl;

/* Cooperative + single-threaded, so a plain static tracks who's running. */
static psx_fiber_impl* s_current = NULL;

/* makecontext only passes ints; split the fiber pointer across two. */
static void psx_fiber_trampoline(unsigned int hi, unsigned int lo)
{
    uintptr_t p = ((uintptr_t)hi << 32) | (uintptr_t)lo;
    psx_fiber_impl* f = (psx_fiber_impl*)p;
    f->entry(f->arg);
    /* The BIOS thread entry never returns normally (it switches back to its
     * scheduler target, or trap_crashes). Reaching here is a bug. */
    abort();
}

psx_fiber_t psx_fiber_convert_thread(void)
{
    if (!s_current) {
        psx_fiber_impl* f = (psx_fiber_impl*)calloc(1, sizeof(*f));
        f->stack = NULL;       /* runs on the real thread stack */
        s_current = f;
    }
    return (psx_fiber_t)s_current;
}

psx_fiber_t psx_fiber_current(void) { return (psx_fiber_t)s_current; }

psx_fiber_t psx_fiber_create(size_t stack_size, psx_fiber_entry entry, void* arg)
{
    if (stack_size < SIGSTKSZ) stack_size = SIGSTKSZ;
    psx_fiber_impl* f = (psx_fiber_impl*)calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->stack = malloc(stack_size);
    if (!f->stack) { free(f); return NULL; }
    f->entry = entry;
    f->arg   = arg;
    if (getcontext(&f->ctx) != 0) { free(f->stack); free(f); return NULL; }
    f->ctx.uc_stack.ss_sp   = f->stack;
    f->ctx.uc_stack.ss_size = stack_size;
    f->ctx.uc_link          = NULL;   /* entry never returns; see trampoline */
    uintptr_t p = (uintptr_t)f;
    makecontext(&f->ctx, (void (*)(void))psx_fiber_trampoline, 2,
                (unsigned int)(p >> 32), (unsigned int)(p & 0xFFFFFFFFu));
    return (psx_fiber_t)f;
}

void psx_fiber_switch(psx_fiber_t target)
{
    psx_fiber_impl* to   = (psx_fiber_impl*)target;
    psx_fiber_impl* from = s_current;
    if (!to || to == from) return;
    s_current = to;
    swapcontext(&from->ctx, &to->ctx);
    /* Resumed: s_current was set back to `from` by whoever switched here. */
}

void psx_fiber_destroy(psx_fiber_t fiber)
{
    psx_fiber_impl* f = (psx_fiber_impl*)fiber;
    if (!f) return;
    free(f->stack);
    free(f);
}

#endif /* _WIN32 */
