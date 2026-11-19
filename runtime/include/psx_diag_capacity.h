/* psx_diag_capacity.h - one capacity setting for diagnostic recording.
 *
 * Every build carries always-on diagnostic rings (function traces, SIO/CD/SPU
 * event history, parity and determinism records, crash and debug-server
 * scratch). On a desktop they total about 600 MB of .bss, which is free in
 * practice. On a memory-limited host (the Nintendo 3DS has about 124 MB for
 * the whole application) they are not, so their sizes are written as
 * PSX_DIAG_CAP(desktop size) / PSX_DIAG_BYTES(desktop bytes) and shrink by
 * 2^PSX_DIAG_CAPACITY_SHIFT.
 *
 * Only diagnostic history uses these macros. Emulation state (RAM, VRAM, SPU
 * RAM, the interpreter dispatch tables, the overlay loader's candidate
 * tables, mod apertures) keeps its full size: shrinking it would change
 * behaviour, shrinking a ring only shortens how far back it can look.
 *
 * Default shift 0: desktop builds are unchanged, entry for entry. A shifted
 * power of two stays a power of two, so `cap - 1` wrap masks stay valid. The
 * floors keep each ring usable: a ring that was smaller than its floor keeps
 * its own size.
 *
 * Set with the CMake cache variable PSX_DIAG_CAPACITY_SHIFT (runtime.cmake);
 * NINTENDO_3DS builds default to 10 (1/1024).
 */
#ifndef PSX_DIAG_CAPACITY_H
#define PSX_DIAG_CAPACITY_H

#ifndef PSX_DIAG_CAPACITY_SHIFT
#define PSX_DIAG_CAPACITY_SHIFT 0
#endif

#define PSX_DIAG_CAP_FLOOR   64u            /* entries, a power of two */
#define PSX_DIAG_BYTES_FLOOR (64u * 1024u)  /* bytes */

#define PSX_DIAG_SHRINK_(n, floor)                                    \
    ((((n) >> PSX_DIAG_CAPACITY_SHIFT) >= (floor))                    \
         ? ((n) >> PSX_DIAG_CAPACITY_SHIFT)                           \
         : ((n) < (floor) ? (n) : (floor)))

/* Entry count of a diagnostic ring or table. */
#define PSX_DIAG_CAP(n)   PSX_DIAG_SHRINK_((n), PSX_DIAG_CAP_FLOOR)
/* Byte size of a diagnostic scratch / dump buffer. */
#define PSX_DIAG_BYTES(n) PSX_DIAG_SHRINK_((n), PSX_DIAG_BYTES_FLOOR)

#endif /* PSX_DIAG_CAPACITY_H */
