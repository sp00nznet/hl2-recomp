/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "recomp_types.h"

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns these: xbox_kernel defines them in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- defining them here too is a duplicate symbol and
 * the link fails with LNK2005 on all three.
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern ptrdiff_t g_xbox_mem_offset;

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* ---------------------------------------------------------------------------
 * sub_005AD700 -- the CRT's memcpy, implemented natively.
 *
 * MSVC's memcpy embeds a jump table of unrolled tail-copy cases in the middle
 * of the function. tools.disasm decodes that table as instructions, loses sync,
 * and the lifted body ends in nonsense (MEM8(ebx + -1981510076), esp++). More
 * damaging than the garbage itself: the real epilogue is never reached, so the
 * "push edi; push esi" at entry is never undone and **every caller of memcpy
 * gets esi and edi silently clobbered**.
 *
 * On Half-Life 2 that lands squarely on RtlCreateHeap:
 *
 *     0x0059F85C  xor esi, esi
 *     ...         call memcpy          <- esi comes back 0x00F7FF48
 *     0x0059F8AB  cmp ebx, esi
 *     0x0059F8AD  jl  fail             <- taken, returns 0
 *
 * so heap creation fails, and a clobbered edi then defeats the caller's own
 * "cmp eax, edi / je" check of that failure, letting CRT init continue with a
 * null heap.
 *
 * regen.sh passes --exclude-manual, so tools.recomp reads this file and skips
 * generating a body for anything defined here; this definition links instead.
 *
 * cdecl: on entry g_esp points at the return address, arguments above it, and
 * the caller cleans the stack. memmove rather than memcpy because the original
 * tests for overlap (cmp edi, esi / jbe) and handles it.
 *
 * ponytail: a per-title workaround for a toolkit bug. The general fix is for
 * disasm to recognise the jump table, or for func_id + recomp to emit native
 * bodies for identified CRT routines -- func_id's memcpy signature does not
 * even match this one, because HL2's orders the loads esi/ecx/edi where the
 * signature expects esi/edi/ecx.
 */
void sub_005AD700(void)
{
    uint32_t dest = MEM32(g_esp + 4);
    uint32_t src  = MEM32(g_esp + 8);
    uint32_t n    = MEM32(g_esp + 12);

    if (n)
        memmove((void *)XBOX_PTR(dest), (const void *)XBOX_PTR(src), n);

    if (getenv("RECOMP_TRACE_MEMCPY")) {
        static int shown;
        if (shown++ < 6) {
            uint32_t i;
            fprintf(stderr, "[MEMCPY] dst=0x%08X src=0x%08X n=%u  src bytes:",
                    dest, src, n);
            for (i = 0; i < n && i < 48; i += 4)
                fprintf(stderr, " %08X", MEM32(src + i));
            fprintf(stderr, "\n");
        }
    }

    g_eax = dest;          /* memcpy returns its destination */
    g_esp += 4;            /* pop the return address; caller pops the args */
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* sub_005AE3D0 -- memmove, the same lost-sync decode as memcpy above.
 *
 * Structurally identical to sub_005AD700: same prologue saving edi and esi,
 * same (dest, src, count) cdecl layout, same 262 bytes, same embedded jump
 * table that tools.disasm decodes as instructions. So it has the same effect:
 * the epilogue is never reached and every one of its 61 callers gets esi and
 * edi clobbered.
 *
 * That is not a cosmetic loss here. The C++ static-initialiser driver
 * (sub_0059DE80) walks 5,314 constructor pointers with esi as the cursor and
 * edi as the limit, calling each one:
 *
 *     esi = 0x7A3530; edi = 0x7A8818;
 *     loop: eax = MEM32(esi); if (eax) ICALL(eax);
 *           esi += 4; if (esi < edi) goto loop;
 *
 * A callee that does not restore esi ends that loop wherever the clobbered
 * value lands. Source registers its interfaces from those constructors, so a
 * truncated loop leaves s_pInterfaceRegs (0x009A9DEC) empty, and the engine
 * hangs on the first CreateInterface("VFileSystem017") it attempts.
 */
void sub_005AE3D0(void)
{
    uint32_t dest = MEM32(g_esp + 4);
    uint32_t src  = MEM32(g_esp + 8);
    uint32_t n    = MEM32(g_esp + 12);

    if (n)
        memmove((void *)XBOX_PTR(dest), (const void *)XBOX_PTR(src), n);

    g_eax = dest;
    g_esp += 4;
}

/* ---------------------------------------------------------------------------
 * sub_0059DE80 -- MSVC's _initterm, implemented natively.
 *
 * The lifted version is correct; what breaks it is its callees. esi and edi
 * live in globals here rather than on the host stack, so a constructor that
 * returns without restoring them rewrites this loop's cursor and limit. A
 * probe at the epilogue caught exactly that: esi = 0x00F7FECC, edi =
 * 0x00F7FEC4, both stack addresses, with esi < edi already false. 461 indirect
 * calls for the entire run against 5,305 constructors -- the walk ran 8% of
 * the list, so Source registered almost no interfaces and the first
 * CreateInterface("VFileSystem017") found no factory.
 *
 * This does the walk in C and re-establishes esi/edi/ebx/esp around every
 * constructor, which both survives a misbehaving callee and names it. The
 * layout is read from the binary, not hardcoded: the two arrays are the ones
 * the original loads.
 *
 * ponytail: forcing esp back after each call papers over a callee that leaves
 * the stack unbalanced, which is a real bug in that callee. It is reported
 * rather than fixed here; -DHL2_ABI_CHECK finds the same class of problem
 * across every indirect call rather than just this loop.
 */
#define INITTERM_PRE_LO   0x007A881Cu   /* pre-C++ init table */
#define INITTERM_PRE_HI   0x007A8830u
#define INITTERM_CTOR_LO  0x007A3530u   /* the 5,305 C++ constructors */
#define INITTERM_CTOR_HI  0x007A8818u

static void initterm_call(uint32_t fn, uint32_t *ran,
                          uint32_t *clobbered, uint32_t *faulted)
{
    recomp_func_t f = recomp_lookup_manual(fn);
    uint32_t esi0, edi0, ebx0, esp0;

    if (!f) f = recomp_lookup(fn);
    if (!f) f = recomp_lookup_kernel(fn);
    if (!f) {
        fprintf(stderr, "[INITTERM] no body for constructor 0x%08X\n", fn);
        return;
    }

    esi0 = g_esi; edi0 = g_edi; ebx0 = g_ebx; esp0 = g_esp;
    g_esp -= 4;
    MEM32(g_esp) = 0x0059DECEu;          /* guest return address */
    /* Survive a constructor that faults instead of stopping the walk.
     *
     * One bad constructor otherwise ends static initialisation, and every
     * later one -- including whichever registers the filesystem interface --
     * simply never runs. Continuing turns "it dies at #296" into a count of
     * how many of the 5,305 are actually broken, which is the difference
     * between one bug and a class of them. The guest state is re-established
     * below either way, so a fault costs that constructor's side effects and
     * nothing else.
     *
     * ponytail: __try is the whole recovery. Anything more (retry, quarantine,
     * per-constructor rollback) is guesswork until the count says how bad it
     * is.
     */
    __try {
        f();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if ((*faulted)++ < 12)
            fprintf(stderr, "[INITTERM] ctor 0x%08X faulted; continuing\n", fn);
    }
    (*ran)++;

    if (g_esi != esi0 || g_edi != edi0 || g_ebx != ebx0 || g_esp != esp0) {
        /* Report the first few only: the useful fact is which constructors
         * misbehave, not how many times the loop noticed. */
        if ((*clobbered)++ < 12)
            fprintf(stderr, "[INITTERM] ctor 0x%08X did not restore:"
                            " esi %08X->%08X edi %08X->%08X"
                            " ebx %08X->%08X esp %08X->%08X\n",
                    fn, esi0, g_esi, edi0, g_edi,
                    ebx0, g_ebx, esp0, g_esp);
        g_esi = esi0; g_edi = edi0; g_ebx = ebx0; g_esp = esp0;
    }
}

static void initterm_walk(uint32_t lo, uint32_t hi, uint32_t *ran,
                          uint32_t *clobbered, uint32_t *faulted)
{
    /* RECOMP_TRACE_INITTERM prints each constructor before it runs, so a fault
     * inside one is attributable to a slot index rather than a bare host
     * address. 5,305 lines is a lot, but a crash here is otherwise anonymous:
     * the fault handler sees guest registers and no guest PC. */
    const int trace = getenv("RECOMP_TRACE_INITTERM") != NULL;
    uint32_t p;

    for (p = lo; p < hi; p += 4) {
        uint32_t fn = MEM32(p);
        if (!fn || fn == 0xFFFFFFFFu)
            continue;
        if (trace) {
            fprintf(stderr, "[INITTERM] #%u slot 0x%08X -> 0x%08X\n",
                    *ran, p, fn);
            fflush(stderr);
        }
        initterm_call(fn, ran, clobbered, faulted);
    }
}

void sub_0059DE80(void)
{
    uint32_t esi0 = g_esi, edi0 = g_edi;
    uint32_t ran = 0, clobbered = 0, faulted = 0;
    uint32_t hook = MEM32(0x00817648u);

    if (hook)
        initterm_call(hook, &ran, &clobbered, &faulted);

    initterm_walk(INITTERM_PRE_LO, INITTERM_PRE_HI,
                  &ran, &clobbered, &faulted);
    ran = 0;
    initterm_walk(INITTERM_CTOR_LO, INITTERM_CTOR_HI,
                  &ran, &clobbered, &faulted);

    fprintf(stderr, "[INITTERM] ran %u C++ constructors, "
                    "%u clobbered callee-saved state, %u faulted\n",
            ran, clobbered, faulted);
    fflush(stderr);

    g_esi = esi0;
    g_edi = edi0;
    g_esp += 4;                          /* pop the return address */
}
