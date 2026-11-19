/* psx_island.h - runtime contract of the island execution profile
 * (tools/islands.py rewrites generated game C against it).
 *
 * Island functions keep MIPS registers in C locals and call other game
 * functions directly. Each function defines ISL_FLUSH() (write its modified
 * registers back to cpu->gpr) and ISL_RELOAD() (reload its locals) before
 * using the macros below. */
#ifndef PSX_ISLAND_H
#define PSX_ISLAND_H

#include "psx_cycles.h"
#include "psx_block_edge.h"
#include "psx_native_span.h"
#include "psx_direct_memory.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Set (sticky) once any verified game code word changes (memory.c). Until
 * then a direct call runs the same code the dispatcher would have validated;
 * afterwards island calls go through the dispatcher and its checks. */
extern int g_psx_text_code_changed;
static inline int psx_island_direct_ok(void) { return !g_psx_text_code_changed; }

void psx_check_interrupts_dispatch_entry(CPUState* cpu, uint32_t resume_pc);
int  psx_vsync_query_hle_try(CPUState* cpu, uint32_t addr);
extern uint32_t g_psx_vsync_query_hle_func;   /* load_accel.c */
extern int g_rfe_escape_pending;   /* interrupts.c */
extern int g_psx_call_bail;        /* dispatch: unwind to a call site */

/* After a direct call: continue inline only when the callee came back to the
 * return address with nothing the dispatcher still has to act on (an RFE that
 * escaped its handler, a call bail). Otherwise the caller returns to the
 * dispatcher, which handles it exactly as without islands. */
static inline int psx_island_returned(const CPUState *cpu, uint32_t ret) {
    return cpu->pc == ret && !g_rfe_escape_pending && !g_psx_call_bail;
}

/* A direct call does what the game dispatcher does on entry to a compiled
 * function (psx_dispatch_game_compiled): an interrupt check at the callee's
 * address (gated by the edge horizon exactly like a block edge), resume from
 * the function entry, and the vsync-query replacement for its one configured
 * function; then the function itself. */
static inline void psx_island_call(CPUState *cpu, uint32_t target, void (*fn)(CPUState *)) {
#if defined(PSX_LEAN_HOST) && PSX_LEAN_HOST && \
    defined(PSX_OBSERVATION_TIME) && PSX_OBSERVATION_TIME && !defined(PSX_OVERLAY_DLL_BUILD)
    if (__builtin_expect(psx_cycle_count >= g_psx_edge_horizon, 0))
#endif
        psx_check_interrupts_dispatch_entry(cpu, target);
    cpu->pc = 0u;
    if (__builtin_expect(target == g_psx_vsync_query_hle_func, 0) &&
        psx_vsync_query_hle_try(cpu, target))
        return;
    fn(cpu);
}

/* Back at the return address: the dispatcher would have resumed the caller
 * there through its dispatch table, which checks interrupts at that address
 * first. Same check, same gate. */
static inline void psx_island_resume(CPUState *cpu, uint32_t ret) {
#if defined(PSX_LEAN_HOST) && PSX_LEAN_HOST && \
    defined(PSX_OBSERVATION_TIME) && PSX_OBSERVATION_TIME && !defined(PSX_OVERLAY_DLL_BUILD)
    if (__builtin_expect(psx_cycle_count >= g_psx_edge_horizon, 0))
#endif
        psx_check_interrupts_dispatch_entry(cpu, ret);
    cpu->pc = 0u;
}

#ifdef __cplusplus
}
#endif

/* ---- Fast paths (lean hosts) ----------------------------------------------
 * Island functions are large, and the compiler stops inlining the general
 * memory/timing helpers into them: every guest load, store and block charge
 * became a call through a dozen checks. Islands use these instead: always
 * inlined, a few instructions on the common path, the full helper otherwise.
 *
 * g_psx_island_mem_ok folds the per-access conditions the general helpers
 * test (debug recording/replay, read watches, DMA/host store context, parity
 * and memory-card arming, substituted bus callbacks, pending overlay cycles).
 * psx_edge_horizon_refresh (interrupts.c) computes it with the horizon after
 * each full edge, and PSX_EDGE_HORIZON_DROP clears it with the horizon, so
 * it is never stale across a runtime entry. */
#if defined(PSX_LEAN_HOST) && PSX_LEAN_HOST && \
    defined(PSX_OBSERVATION_TIME) && PSX_OBSERVATION_TIME && !defined(PSX_OVERLAY_DLL_BUILD) && \
    defined(PSX_NATIVE_CPU_SPANS) && PSX_NATIVE_CPU_SPANS && \
    defined(PSX_DIRECT_GUEST_MEMORY) && PSX_DIRECT_GUEST_MEMORY
#define PSX_ISLAND_FAST 1
#define ISL_INLINE static inline __attribute__((always_inline))
#define ISL_COLD __attribute__((noinline, cold))
extern int g_psx_island_mem_ok;
extern uint8_t *g_psx_ram;
extern uint64_t g_guest_store_count;

/* Block charge: below the horizon nothing can be due (no sample, no deadline,
 * no batch, no scaling, no special mode): a plain add, exactly what
 * psx_cpu_charge does there. */
static ISL_COLD void isl_charge_slow(uint32_t c) { psx_cpu_charge(c); }
ISL_INLINE void isl_charge(uint32_t c) {
    const uint64_t n = psx_cycle_count + c;
    if (__builtin_expect(n < g_psx_edge_horizon, 1)) psx_cycle_count = n;
    else isl_charge_slow(c);
}

/* Main-RAM KSEG0 window [0x80000000, +2 MB): the common case. */
#define ISL_RAM_OFF(addr) ((uint32_t)(addr) - 0x80000000u)
#define ISL_IN_RAM(off) ((off) < 0x00200000u)

static ISL_COLD uint32_t isl_load_word_slow(CPUState *cpu, uint32_t a) { return psx_native_span_word(cpu, a, 0u, 0u); }
static ISL_COLD uint16_t isl_load_half_slow(CPUState *cpu, uint32_t a) { return psx_native_span_half(cpu, a, 0u, 0u); }
static ISL_COLD uint8_t  isl_load_byte_slow(CPUState *cpu, uint32_t a) { return psx_native_span_byte(cpu, a, 0u, 0u); }
ISL_INLINE uint32_t isl_load_word(CPUState *cpu, uint32_t addr) {
    const uint32_t off = ISL_RAM_OFF(addr);
    if (__builtin_expect(ISL_IN_RAM(off) && g_psx_island_mem_ok, 1)) {
        uint32_t v; __builtin_memcpy(&v, g_psx_ram + off, 4); return v;
    }
    return isl_load_word_slow(cpu, addr);
}
ISL_INLINE uint16_t isl_load_half(CPUState *cpu, uint32_t addr) {
    const uint32_t off = ISL_RAM_OFF(addr);
    if (__builtin_expect(ISL_IN_RAM(off) && g_psx_island_mem_ok, 1)) {
        uint16_t v; __builtin_memcpy(&v, g_psx_ram + off, 2); return v;
    }
    return isl_load_half_slow(cpu, addr);
}
ISL_INLINE uint8_t isl_load_byte(CPUState *cpu, uint32_t addr) {
    const uint32_t off = ISL_RAM_OFF(addr);
    if (__builtin_expect(ISL_IN_RAM(off) && g_psx_island_mem_ok, 1)) return g_psx_ram[off];
    return isl_load_byte_slow(cpu, addr);
}

/* Stores: what psx_direct_store_view admits, minus what g_psx_island_mem_ok
 * already covers: above the kernel area, not a code-watched page, not the
 * guarded text image, cache not isolated. */
ISL_INLINE int isl_store_direct(const CPUState *cpu, uint32_t off) {
    if (!(ISL_IN_RAM(off) && off >= 0x10000u && g_psx_island_mem_ok)) return 0;
    if (cpu->cop0[12] & 0x10000u) return 0;
    const uint32_t page = off >> 12, bit = 1u << (page & 31u);
    if ((g_psx_overlay_watch_bitmap[page >> 5] | g_dirty_ram_exec_page_bitmap[page >> 5]) & bit) return 0;
    if (g_psx_text_write_hi > g_psx_text_write_lo && off >= g_psx_text_write_lo &&
        off < g_psx_text_write_hi) return 0;
    return 1;
}
/* Stores stay out of line: the checks are several loads each, and inlining
 * them at every store site costs megabytes of a 64 MB program image. One
 * call per store is still far cheaper than the general store path. */
#define ISL_STORE_FN __attribute__((noinline))
static ISL_COLD void isl_store_word_slow(CPUState *cpu, uint32_t a, uint32_t v) { psx_native_store_word(cpu, a, v); }
static ISL_COLD void isl_store_half_slow(CPUState *cpu, uint32_t a, uint16_t v) { psx_native_store_half(cpu, a, v); }
static ISL_COLD void isl_store_byte_slow(CPUState *cpu, uint32_t a, uint8_t v) { psx_native_store_byte(cpu, a, v); }
static ISL_STORE_FN void isl_store_word(CPUState *cpu, uint32_t addr, uint32_t v) {
    const uint32_t off = ISL_RAM_OFF(addr);
    if (__builtin_expect(isl_store_direct(cpu, off) && !(off & 3u), 1)) {
        __builtin_memcpy(g_psx_ram + off, &v, 4); ++g_guest_store_count; return;
    }
    isl_store_word_slow(cpu, addr, v);
}
static ISL_STORE_FN void isl_store_half(CPUState *cpu, uint32_t addr, uint16_t v) {
    const uint32_t off = ISL_RAM_OFF(addr);
    if (__builtin_expect(isl_store_direct(cpu, off) && !(off & 1u), 1)) {
        __builtin_memcpy(g_psx_ram + off, &v, 2); ++g_guest_store_count; return;
    }
    isl_store_half_slow(cpu, addr, v);
}
static ISL_STORE_FN void isl_store_byte(CPUState *cpu, uint32_t addr, uint8_t v) {
    const uint32_t off = ISL_RAM_OFF(addr);
    if (__builtin_expect(isl_store_direct(cpu, off), 1)) {
        g_psx_ram[off] = v; ++g_guest_store_count; return;
    }
    isl_store_byte_slow(cpu, addr, v);
}

/* Generated code names (psx_native_span.h / psx_direct_memory.h aliases). */
#undef psx_cyc_load_word
#undef psx_cyc_load_half
#undef psx_cyc_load_byte
#define psx_cyc_load_word(cpu, a, rt, d) isl_load_word((cpu), (a))
#define psx_cyc_load_half(cpu, a, rt, d) isl_load_half((cpu), (a))
#define psx_cyc_load_byte(cpu, a, rt, d) isl_load_byte((cpu), (a))
#undef PSX_STORE_WORD
#undef PSX_STORE_HALF
#undef PSX_STORE_BYTE
#define PSX_STORE_WORD(cpu, a, v) isl_store_word((cpu), (a), (v))
#define PSX_STORE_HALF(cpu, a, v) isl_store_half((cpu), (a), (uint16_t)(v))
#define PSX_STORE_BYTE(cpu, a, v) isl_store_byte((cpu), (a), (uint8_t)(v))
#define psx_native_span_charge(c) isl_charge(c)
#define psx_slice_block(cpu, a, b, s) \
    (__builtin_expect(g_psx_precise_slice, 0) ? psx_slice_block_impl((cpu), (a), (b), (s)) : 0)
#endif

/* Block edge: write back and reload only when the edge actually calls out
 * (it may deliver an interrupt, which reads the registers or unwinds). */
#if defined(PSX_LEAN_HOST) && PSX_LEAN_HOST && \
    defined(PSX_OBSERVATION_TIME) && PSX_OBSERVATION_TIME && !defined(PSX_OVERLAY_DLL_BUILD)
#define ISL_EDGE(cpu, pc) do { \
    if (__builtin_expect(psx_cycle_count >= g_psx_edge_horizon, 0) || \
        PSX_OBSERVATION_WAIT_SITE(pc)) { \
        ISL_FLUSH(); psx_block_edge_call((cpu), (pc)); ISL_RELOAD(); \
    } \
} while (0)
/* Edge n of a function whose slow path is shared (tools/islands.py emits one
 * `_isl_slow:` stub per function: write back, edge call, reload, then
 * `goto *_isl_back`): an ordinary edge is one compare. GCC/Clang labels as
 * values. */
#define ISL_EDGE_AT(cpu, pc, n) do { \
    if (__builtin_expect(psx_cycle_count >= g_psx_edge_horizon, 0) || \
        PSX_OBSERVATION_WAIT_SITE(pc)) { \
        _isl_pc = (pc); _isl_back = &&_isl_ret_##n; goto _isl_slow; \
    } \
    _isl_ret_##n: ; \
} while (0)
#else
#define ISL_EDGE(cpu, pc) do { ISL_FLUSH(); PSX_BLOCK_EDGE((cpu), (pc)); ISL_RELOAD(); } while (0)
#define ISL_EDGE_AT(cpu, pc, n) do { \
    if (0) { _isl_back = &&_isl_ret_##n; goto _isl_slow; } \
    ISL_FLUSH(); PSX_BLOCK_EDGE((cpu), (pc)); ISL_RELOAD(); _isl_ret_##n: ; \
} while (0)
#endif

#endif
