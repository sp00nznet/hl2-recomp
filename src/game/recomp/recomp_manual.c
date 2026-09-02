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
void sub_005AD700(void);
void sub_005AE3D0(void);
void sub_0059DE80(void);
void sub_005ADC0B(void);
void sub_000C6719(void);
void sub_000C85E2(void);
void sub_0031954D(void);
void sub_00319BF7(void);
void sub_000111E0(void);
void sub_00596080(void);
void sub_005960F0(void);
void recomp_watch_guest_write(uint32_t guest_va);

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    /* The natively-implemented functions below.
     *
     * regen.sh passes --exclude-manual, so tools/recomp neither generates a
     * body for these nor emits a dispatch-table entry. Direct calls still bind
     * by symbol, but an *indirect* call to one of these VAs would find nothing
     * in recomp_lookup and be dropped as unresolved. This hook is consulted
     * first, which is exactly what it is for.
     */
    if (xbox_va == 0x005AD700u) return sub_005AD700;   /* memcpy */
    if (xbox_va == 0x005AE3D0u) return sub_005AE3D0;   /* memmove */
    if (xbox_va == 0x0059DE80u) return sub_0059DE80;   /* _initterm */
    if (xbox_va == 0x005ADC0Bu) return sub_005ADC0B;   /* atexit */
    /* Two CUtlRBTree template instantiations, byte-identical apart from
     * which static invalid-node they use. Same signature, same layout
     * (LessFunc +0, elements +4, root +0x10), so one native body serves
     * both -- the native walk computes element addresses directly and
     * treats 0xFFFF as the terminator, so it never needs the sentinel. */
    if (xbox_va == 0x000C6719u) return sub_000C6719;   /* RBTree FindParent */
    if (xbox_va == 0x0031954Du) return sub_000C6719;   /* ... other instance */
    if (xbox_va == 0x000C85E2u) return sub_000C85E2;   /* RBTree LinkToParent */
    if (xbox_va == 0x00319BF7u) return sub_00319BF7;   /* ... other instance */
    if (xbox_va == 0x000111E0u) return sub_000111E0;   /* RBTree FirstInorder */

    /* The engine's own spew sink -- see the body below for why it is a
     * stub in the shipped image. */
    if (xbox_va == 0x00596080u) return sub_00596080;   /* SpewOutputFunc */
    if (xbox_va == 0x005960F0u) return sub_005960F0;   /* spew gate */

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

    /* Large copies only: the tree buffer grows are 2 KB and up, and the
     * question is whether CUtlMemory::Grow copies at all. */
    if (n >= 1024 && getenv("RECOMP_TRACE_BIGCOPY")) {
        static int big;
        if (big++ < 40)
            fprintf(stderr, "[MEMCPY] dst=0x%08X src=0x%08X n=%u\n",
                    dest, src, n);
    }

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

    if (n >= 1024 && getenv("RECOMP_TRACE_BIGCOPY")) {
        static int big;
        if (big++ < 40)
            fprintf(stderr, "[MEMMOVE] dst=0x%08X src=0x%08X n=%u\n",
                    dest, src, n);
    }

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

/* ---------------------------------------------------------------------------
 * sub_005ADC0B -- atexit, implemented natively.
 *
 * 1,587 callers, and every C++ static initialiser with a destructor goes
 * through it. It is also the single choke point that was corrupting them: the
 * -DHL2_ABI_CHECK run traced both clobbering constructors to the same chain,
 * atexit -> __onexit (sub_005ADBD3, sub_005ADB2B) -> realloc (sub_005B2C88),
 * which grows the atexit table and returns with esi, edi and ebx holding stack
 * addresses. Those are the constructor walk's cursor and limit, so the damage
 * lands on the caller rather than on the CRT.
 *
 * atexit's actual job is to append one function pointer to a list that runs at
 * shutdown. Keeping that list natively skips the whole realloc path. The
 * recorded pointers are guest VAs; nothing runs them yet, because the title
 * has never reached a clean shutdown -- when it does, walking this in reverse
 * is the whole implementation.
 *
 * ponytail: fixed array, not a growable one. The guest's own table starts at
 * 32 entries and this title registers a few thousand; 8192 is past any real
 * count and costs 32 KB. If it ever fills, that is worth knowing about, so it
 * says so rather than silently dropping.
 */
static uint32_t g_atexit_fns[8192];
static unsigned g_atexit_count;

void sub_005ADC0B(void)
{
    uint32_t fn = MEM32(g_esp + 4);

    if (fn) {
        if (g_atexit_count < (unsigned)(sizeof(g_atexit_fns) /
                                        sizeof(g_atexit_fns[0]))) {
            g_atexit_fns[g_atexit_count++] = fn;
        } else {
            static int warned;
            if (!warned++)
                fprintf(stderr, "[ATEXIT] table full at %u; further handlers dropped\n",
                        g_atexit_count);
        }
    }

    g_eax = 0;                 /* atexit returns 0 on success */
    g_esp += 4;                /* pop the return address; caller pops the arg */
}

/* ---------------------------------------------------------------------------
 * sub_000C6719 -- CUtlRBTree::FindParent, implemented natively.
 *
 * thiscall: ecx = tree, then (key, unsigned short *pParent, bool *pLeft).
 * Descends from m_Root comparing with the tree's LessFunc, recording the last
 * node visited and which way the search went -- the insertion point.
 *
 * The lifted version is faithful; the tree it walks is not. Static
 * initialisation reaches constructor #736 and stops here, with the samples
 * alternating between this function, the comparator at 0x0001D0F8 and stricmp.
 * A descent through a binary tree cannot loop, so the links form a cycle.
 *
 * This is the same trade as the __try around each constructor: detect it,
 * report it once with the cycle, and carry on with the last good parent
 * rather than spinning forever. An insert against a cyclic tree is wrong
 * either way -- but a wrong insert that returns lets the remaining 4,500
 * constructors run and shows what else is broken, which spinning does not.
 *
 * ponytail: a fixed 64-entry visited ring, not a set. A real tree of N nodes
 * is ~log2(N) deep, so anything past 64 is already pathological.
 */
#define RBTREE_INVALID 0xFFFFu

/* Which rebalance the shared LinkToParent should call. Each template
 * instantiation has its own copy at a different address. */
static uint32_t g_rbtree_rebalance = 0x000C7F18u;

static void call_guest(uint32_t fn, uint32_t guest_ret)
{
    recomp_func_t f = recomp_lookup_manual(fn);

    if (!f) f = recomp_lookup(fn);
    if (!f) f = recomp_lookup_kernel(fn);
    if (!f) {
        g_eax = 0;
        return;
    }
    g_esp -= 4;
    MEM32(g_esp) = guest_ret;
    f();
}

void sub_000C6719(void)
{
    uint32_t tree     = g_ecx;
    uint32_t key      = MEM32(g_esp + 4);
    uint32_t p_parent = MEM32(g_esp + 8);
    uint32_t p_left   = MEM32(g_esp + 12);
    uint32_t less_fn  = MEM32(tree);
    uint32_t elements = MEM32(tree + 4);
    uint32_t idx      = MEM16(tree + 0x10);
    uint16_t seen[64];
    unsigned depth = 0;

    MEM16(p_parent) = RBTREE_INVALID;
    MEM8(p_left) = 0;

    /* Catch the moment a tree's element buffer moves.
     *
     * CUtlMemory::Grow reallocates and must carry the old contents across.
     * Tree 0x0089C744 is well-formed at count=57 with one buffer and mostly
     * zeroed at count=183 with another, so the copy is the suspect. Reporting
     * the old and new pointer with a live-node count on either side pins it
     * to a single grow instead of a range of them. */
    {
        static uint32_t last_tree, last_elems;
        if (tree == last_tree && elements != last_elems) {
            uint32_t count = MEM16(tree + 0x12), n, zero = 0;
            for (n = 0; n < count; n++) {
                uint32_t e = elements + n * 16;
                if (MEM16(e) == 0 && MEM16(e + 2) == 0 && MEM16(e + 4) == 0)
                    zero++;
            }
            /* And the buffer it came from. If the old one still holds the
             * links, the copy simply never happened; if it is empty too,
             * the data was destroyed before the grow. */
            uint32_t oldzero = 0;
            for (n = 0; n < count; n++) {
                uint32_t e = last_elems + n * 16;
                if (MEM16(e) == 0 && MEM16(e + 2) == 0 && MEM16(e + 4) == 0)
                    oldzero++;
            }
            fprintf(stderr, "[RBTREE] tree 0x%08X elements moved 0x%08X -> 0x%08X, count=%u, new %u zeroed, old %u zeroed\n",
                    tree, last_elems, elements, count, zero, oldzero);
            fflush(stderr);
        }
        last_tree = tree; last_elems = elements;
    }

    if (getenv("RECOMP_TRACE_RBTREE")) {
        static int shown;
        if (shown++ < 60)
            fprintf(stderr,
                    "[RBTREE] find  tree=0x%08X root=%u elems=0x%08X "
                    "node%u=[%u,%u] count=%u\n",
                    tree, (unsigned)idx, elements, (unsigned)idx,
                    elements ? (unsigned)MEM16(elements + idx * 16) : 0u,
                    elements ? (unsigned)MEM16(elements + idx * 16 + 2) : 0u,
                    (unsigned)MEM16(tree + 0x12));
    }

    while (idx != RBTREE_INVALID) {
        uint32_t links, node;
        unsigned i;
        int less;

        for (i = 0; i < depth; i++) {
            if (seen[i] == (uint16_t)idx) {
                static int reported;
                if (reported++ < 4)
                    fprintf(stderr,
                            "[RBTREE] cycle at tree 0x%08X: node %u "
                            "revisited after %u steps; stopping\n"
                            "         less=0x%08X elems=0x%08X root=%u "
                            "count=%u free=%u last=%u node0=[%u,%u]\n",
                            tree, (unsigned)idx, depth,
                            MEM32(tree), elements,
                            (unsigned)MEM16(tree + 0x10),
                            (unsigned)MEM16(tree + 0x12),
                            (unsigned)MEM16(tree + 0x14),
                            (unsigned)MEM16(tree + 0x16),
                            elements ? (unsigned)MEM16(elements) : 0u,
                            elements ? (unsigned)MEM16(elements + 2) : 0u);
                /* The path that looped, with each node's links. A
                 * rotation bug shows up as a parent/child pair that
                 * disagree; an uninitialised node shows up as zeros. */
                {
                    unsigned k;
                    for (k = 0; k < depth && k < 8; k++) {
                        uint32_t n = elements + seen[k] * 16;
                        fprintf(stderr,
                                "           path[%u] node %-5u L=%-5u R=%-5u P=%-5u tag=%u\n",
                                k, (unsigned)seen[k],
                                (unsigned)MEM16(n),
                                (unsigned)MEM16(n + 2),
                                (unsigned)MEM16(n + 4),
                                (unsigned)MEM16(n + 6));
                    }
                }
                /* Where does the live data stop? If a grow copied a
                 * short prefix, every node past that point reads as
                 * all-zero links -- which is a cycle, since 0 is a
                 * valid index. Counting them separates a bad copy from
                 * a bad rotation. */
                {
                    uint32_t count = MEM16(tree + 0x12), n, zero = 0;
                    int first_zero = -1;
                    for (n = 0; n < count; n++) {
                        uint32_t e = elements + n * 16;
                        if (MEM16(e) == 0 && MEM16(e + 2) == 0 &&
                            MEM16(e + 4) == 0) {
                            if (first_zero < 0) first_zero = (int)n;
                            zero++;
                        }
                    }
                    fprintf(stderr, "           %u of %u nodes have all-zero links, first at %d\n",
                            zero, count, first_zero);
                }
                fflush(stderr);
                idx = RBTREE_INVALID;
                break;
            }
        }
        if (idx == RBTREE_INVALID)
            break;
        if (depth < sizeof(seen) / sizeof(seen[0]))
            seen[depth++] = (uint16_t)idx;
        else
            break;                       /* deeper than any sane tree */

        MEM16(p_parent) = (uint16_t)idx;
        links = elements + idx * 16;
        node  = links + 8;

        g_ecx = key;
        g_edx = node;
        call_guest(less_fn, 0x000C674Eu);
        less = (g_eax & 0xFF) != 0;

        MEM8(p_left) = (uint8_t)(less ? 1 : 0);
        idx = MEM16(links + (less ? 0 : 2));
    }

    g_esp += 16;                         /* ret 12: return address + 3 args */
}

/* ---------------------------------------------------------------------------
 * sub_000C85E2 -- CUtlRBTree::LinkToParent, implemented natively.
 *
 * thiscall: ecx = tree, then (i, parent, left).
 *
 * The tree at 0x0085523C ends up with one element whose links are [0,0]
 * instead of [0xFFFF,0xFFFF], which makes node 0 its own child and spins every
 * later search. This is the only function that writes those links, so either
 * it is lifted wrongly or something later overwrites them. Doing it natively
 * settles which: if the links are still [0,0] afterwards, the write is not the
 * problem.
 *
 * RECOMP_TRACE_RBTREE prints each link so the sequence can be read directly.
 */
void sub_000C85E2(void)
{
    uint32_t tree     = g_ecx;
    uint32_t i        = MEM32(g_esp + 4) & 0xFFFFu;
    uint32_t parent   = MEM32(g_esp + 8) & 0xFFFFu;
    uint32_t left     = MEM32(g_esp + 12) & 0xFFu;
    uint32_t elements = MEM32(tree + 4);
    uint32_t links    = elements + i * 16;

    MEM16(links)     = 0xFFFFu;          /* Left  */
    MEM16(links + 2) = 0xFFFFu;          /* Right */
    MEM16(links + 4) = (uint16_t)parent; /* Parent */
    MEM16(links + 6) = 0;                /* Tag (red) */

    if (parent != 0xFFFFu) {
        uint32_t plinks = elements + parent * 16;
        if (left)
            MEM16(plinks) = (uint16_t)i;
        else
            MEM16(plinks + 2) = (uint16_t)i;
    } else {
        MEM16(tree + 0x10) = (uint16_t)i;    /* m_Root */
    }

    if (getenv("RECOMP_TRACE_RBTREE")) {
        static int shown;
        if (shown++ < 20)
            fprintf(stderr,
                    "[RBTREE] link tree=0x%08X i=%u parent=%u left=%u "
                    "elems=0x%08X -> node%u=[%u,%u]\n",
                    tree, i, parent, left, elements, i,
                    (unsigned)MEM16(links), (unsigned)MEM16(links + 2));
    }

    /* Rebalance, exactly as the original tail does: ecx = this, one argument,
     * and it cleans that argument itself (ret 4). */
    g_esp -= 4;
    MEM32(g_esp) = i;
    g_ecx = tree;
    call_guest(g_rbtree_rebalance, 0x000C8637u);

    /* Arm a hardware watchpoint on the links we just wrote. They are
     * correct here and zero by the next search, and reading candidate
     * code has not found the writer -- so let the CPU name it. */
    if (getenv("RECOMP_WATCH_RBTREE"))
        recomp_watch_guest_write(links);

    if (getenv("RECOMP_TRACE_RBTREE")) {
        static int after;
        if (after++ < 20)
            fprintf(stderr,
                    "[RBTREE] after rebalance: node%u=[%u,%u] root=%u\n",
                    i, (unsigned)MEM16(links), (unsigned)MEM16(links + 2),
                    (unsigned)MEM16(tree + 0x10));
    }

    g_esp += 16;                         /* ret 12 */
}

/* The second CUtlRBTree instantiation's FindParent.
 *
 * Byte-identical to sub_000C6719 apart from which static invalid-node its
 * Links() helper returns, and the native walk never needs that -- it computes
 * element addresses directly and terminates on 0xFFFF. Direct callers bind to
 * this symbol, so it has to exist rather than just being routed in
 * recomp_lookup_manual.
 */
void sub_0031954D(void)
{
    sub_000C6719();
}

/* The second instantiation's LinkToParent.
 *
 * Bound to the same native body as the first, to test where the corruption
 * comes from: tree 0x0085523C goes through the native version and is clean,
 * while tree 0x0089C744 goes through this lifted one and ends up with 176 of
 * its 183 nodes holding all-zero links. The two lifted functions are
 * structurally identical, so running the same C for both says whether the
 * fault is in this function or further up in NewNode and the grow.
 *
 * The rebalance address differs between instantiations, so the shared body
 * takes it as state rather than hardcoding one.
 */
void sub_00319BF7(void)
{
    g_rbtree_rebalance = 0x003199E9u;
    sub_000C85E2();
    g_rbtree_rebalance = 0x000C7F18u;
}

/* sub_000111E0 -- CUtlRBTree::FirstInorder, implemented natively.
 *
 * thiscall: ecx = tree, returns the leftmost node index in AX.
 *
 * It descends i = Left(i) until Left(i) is INVALID, calling nothing. That is
 * why a cyclic Left chain shows up as a thread pegged at 100% with zero
 * indirect calls -- there is no dispatch in the loop to count. The lifted
 * version is faithful; a tree whose links form a loop is not something it can
 * defend against.
 *
 * Bounded and reported, the same trade as FindParent: a descent through a
 * binary tree cannot exceed its node count, so passing that means the links
 * are wrong, and saying which tree beats spinning silently.
 */
void sub_000111E0(void)
{
    uint32_t tree     = g_ecx;
    uint32_t elements = MEM32(tree + 4);
    uint32_t count    = MEM16(tree + 0x12);
    uint32_t idx      = MEM16(tree + 0x10);
    uint32_t steps    = 0;

    while (idx != RBTREE_INVALID) {
        uint32_t left = MEM16(elements + idx * 16);
        if (left == RBTREE_INVALID)
            break;
        if (++steps > count + 2u) {
            static int reported;
            if (reported++ < 4)
                fprintf(stderr,
                        "[RBTREE] FirstInorder ran away on tree 0x%08X: "
                        "elems=0x%08X root=%u count=%u, stopped at node %u\n",
                        tree, elements, (unsigned)MEM16(tree + 0x10),
                        count, idx);
            /* Who passed this? The guest return address is on top of
             * the stack at entry, and the caller chain below it says
             * where a float ended up in ecx. */
            {
                unsigned k, shown = 0;
                fprintf(stderr, "           called from:\n");
                for (k = 0; k < 64 && shown < 8; k++) {
                    uint32_t v = MEM32(g_esp + k * 4);
                    if (v > 0x00011000u && v < 0x005F4A6Cu) {
                        fprintf(stderr, "             [esp+%-3u] 0x%08X\n",
                                k * 4, v);
                        shown++;
                    }
                }
            }
            fflush(stderr);
            break;
        }
        idx = left;
    }

    g_eax = (g_eax & 0xFFFF0000u) | (idx & 0xFFFFu);
    g_esp += 4;                          /* ret */
}

/* ── the engine's own console ──────────────────────────────── */

/*
 * SpewRetval_t SpewOutputFunc(const char *pMsg)
 *
 * Retail ships this as a stub -- "mov eax, 0x80004005; ret 4" -- and neither
 * of the two functions that would install a real sink (sub_005960C0 and
 * sub_00596710) is ever called, so the shipped Xbox build discards every
 * engine message. The gate at 0x0081598C ships as 1, which makes
 * sub_00598650 return before it formats anything at all.
 *
 * Clearing that gate (see hl2_enable_engine_spew in main.c) and overriding
 * this function gives the engine its voice back: every Msg/Warning/DevMsg the
 * engine already knows how to format arrives here as a finished string. That
 * is worth far more than any diagnostic written from outside, because it is
 * the engine's own account of what it is doing.
 *
 * The caller formats into a stack buffer with _snprintf and then does
 * "test eax,eax; jge" on the result, so 0 (SPEW_CONTINUE) is success.
 */
void sub_00596080(void)
{
    uint32_t msg = MEM32(g_esp + 4);

    if (msg) {
        /* Guest memory, so treat the terminator as advisory: copy at most a
         * buffer's worth and stop at the first NUL. The engine's own format
         * buffer is 0x100 bytes at one call site and 0x400 at the other. */
        char line[1024];
        size_t i = 0;
        while (i < sizeof(line) - 1) {
            char c = (char)MEM8(msg + (uint32_t)i);
            if (!c)
                break;
            line[i++] = c;
        }
        line[i] = '\0';
        /* The engine embeds its own newlines; do not add one. */
        fputs(line, stderr);
        fflush(stderr);
    }

    g_eax = 0;      /* SPEW_CONTINUE */
    g_esp += 8;     /* ret 4: the return address and the argument */
}

/*
 * bool IsSpewSuppressed(void)  -- "mov al, [0x0081598C]; ret"
 *
 * sub_00598650 calls this first and returns immediately when it is true, so
 * this one byte decides whether the engine says anything at all. It ships as
 * 1, and poking it before the entry point is not enough: it lives in .data,
 * so a static initialiser can put it back while the 5,305 constructors run.
 *
 * Overriding the reader instead of chasing the writer makes that ordering
 * irrelevant. The first call reports what the byte actually held, which is
 * the evidence for whether anything was rewriting it.
 */
void sub_005960F0(void)
{
    static int reported;
    static int force = -1;
    uint8_t gate = MEM8(0x0081598Cu);

    if (force < 0) {
        const char *opt = getenv("HL2_SPEW");
        force = (opt && opt[0] != '0') ? 1 : 0;
    }

    /* Who asks matters: sub_00598650 is only one of six callers, so a query
     * alone does not prove Msg ran. The return address names the caller;
     * 0x0059865B is the site inside sub_00598650, which is main's logger. */
    if (reported++ < 16) {
        fprintf(stderr, "[SPEW] query #%d from 0x%08X (gate=%u)\n",
                reported, MEM32(g_esp), (unsigned)gate);
        fflush(stderr);
    }

    /* Faithful by default. Forcing the gate open is opt-in because on Xbox
     * the spew path does not reach a console: sub_00598650 hands the
     * formatted line to XBX_SendRemoteCommand, which talks to the debug
     * monitor. A retail image has no XBDM, so forcing it drives the engine
     * into code that cannot complete -- boot stops at
     * 'XBX_InitDebug: failed to register command processor' instead of
     * running. The engine's usable output channel is DbgPrint, which the
     * kernel bridge already surfaces as [GUEST]. */
    g_eax = (g_eax & 0xFFFFFF00u) | (force ? 0u : (uint32_t)gate);
    g_esp += 4;             /* ret */
}
