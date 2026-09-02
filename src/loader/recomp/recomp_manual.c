/*
 * Manual overrides and ICALL diagnostics for the recompiled loader.
 *
 * The game's copy carries nine hand-written functions; this one carries none.
 * The loader is a small C program that copies files and plays two videos, so
 * nothing here has needed replacing -- but the hooks must exist, because the
 * generated code calls them on every indirect call.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

#include "recomp_types.h"

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    (void)xbox_va;
    return (recomp_func_t)0;
}

/* An indirect call whose target is not in the dispatch table. In the game
 * these are worth chasing (they are usually a function the disassembler
 * missed, and become a seed); here they are logged once per address so a
 * failed install has something to point at. */
void recomp_icall_fail_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            return;
    if (count == SLOTS)
        return;
    seen[count++] = va;
    fprintf(stderr, "[ICALL] unresolved target 0x%08X\n", va);
    fflush(stderr);
}

/* A target that is not in a code section at all -- a null or wild pointer
 * rather than a missing translation. */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            return;
    if (count == SLOTS)
        return;
    seen[count++] = va;
    fprintf(stderr, "[ICALL] target not code: 0x%08X (from 0x%08X)\n",
            va, MEM32(g_esp));
    /* At a virtual call the object is still in ecx, so its vtable pointer
     * says whether the object was constructed or the slot is simply empty. */
    fprintf(stderr, "        this=0x%08X vtable=0x%08X slot1=0x%08X\n",
            g_ecx, MEM32(g_ecx), MEM32(MEM32(g_ecx) + 4));
    /* The constructor writes 0x0006F928 to the object's first word. Finding
     * where that value actually landed says whether the object is simply at a
     * different offset than the caller thinks. */
    {
        int k;
        for (k = -0x40; k <= 0x2400; k += 4) {
            if (MEM32(g_ecx + k) == 0x0006F928u) {
                fprintf(stderr, "        vtable value found at this%+d\n", k);
                break;
            }
        }
        if (k > 0x2400)
            fprintf(stderr, "        vtable value 0x0006F928 not present near this\n");
    }
    fflush(stderr);
}

/* ── the attract loop, replaced by the install ─────────────── */

extern void sub_00013AE0(void);   /* vtable slot 1 -- per-frame */
extern void sub_00013F70(void);   /* vtable slot 2 -- per-frame */
extern RECOMP_TLS uint32_t g_ecx, g_eax, g_esp;

static void loader_call(void (*fn)(void), uint32_t this_ptr)
{
    g_ecx = this_ptr;
    g_esp -= 4;
    MEM32(g_esp) = 0xDEADBEEFu;   /* the return address it pops */
    fn();
}

/*
 * sub_00014E60 -- the loader's attract loop, replaced.
 *
 * default.xbe's main is:
 *
 *     sub_00012070(this)   construct
 *     sub_00014DD0(this)   bring up D3D     -- succeeds here
 *     sub_00014E60(this)   attract loop     <- this
 *
 * The loop polls the gamepad (thumbstick deadzones 0x1EB8/0xE148 are visible
 * in it), then drives the install through the object's own vtable:
 *
 *     mov eax,[esi]; mov ecx,esi; call [eax+4]
 *     mov edx,[esi]; mov ecx,esi; call [edx+8]
 *     ...; jmp back
 *
 * The constructor installs the vtable at 0x0006F928, which holds
 * { 0x00014060, 0x00013AE0, 0x00013F70, 0x00014DC0 }, so the two per-frame
 * calls are sub_00013AE0 and sub_00013F70. sub_00014060 is slot 0 and is
 * called *by* them -- pumping it alone returns -1, which is what happens if
 * you mistake the state machine's step for its driver.
 *
 * Replacing the loop keeps the parts that matter: the loader has run its own
 * CRT and constructor, so its globals, heap and decompressor are live. What
 * is dropped is the input polling and the logo videos, neither of which the
 * install needs. The real loop never returns -- the loader finishes by
 * launching the game, which reaches HalReturnToFirmware -- so this pumps to a
 * cap instead.
 *
 * Bound by symbol, not through recomp_lookup_manual: the call from
 * sub_00014420 is direct, and regen_loader.sh passes --exclude-manual so no
 * body is generated for this address.
 */
void loader_attract_replacement_disabled(void)
{
    uint32_t this_ptr = g_ecx;
    int step;

    fprintf(stderr, "[LOADER] attract loop replaced; pumping the install\n");
    fflush(stderr);

    for (step = 0; step < 2000000; step++) {
        loader_call(sub_00013AE0, this_ptr);
        loader_call(sub_00013F70, this_ptr);

        if ((step % 500) == 0) {
            fprintf(stderr, "[LOADER] step %d\n", step);
            fflush(stderr);
        }
    }

    fprintf(stderr, "[LOADER] pump ended after %d steps\n", step);
    fflush(stderr);

    g_esp += 4;   /* our own return address */
}
