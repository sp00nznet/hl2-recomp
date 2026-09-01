/**
 * Half-Life 2 (Xbox) - Recompiled Game Entry Point
 *
 * Hosts the recompiled hl2_xbox.xbe. Boot order matters and is the same one
 * the other titles in this family use:
 *
 *   1. load the XBE                     (the recompiled code reads its own
 *                                        .rdata/.data out of the mapped image)
 *   2. map the Xbox memory layout       at the original virtual addresses
 *   3. bring up the MCPX APU            DirectSound spins on the audio DSP,
 *                                        and on Wreckless the whole engine
 *                                        init sat behind DirectSoundCreate
 *                                        succeeding -- so audio is not
 *                                        optional even when chasing pixels
 *   4. kernel replacement + thunk table
 *   5. set the guest stack pointer
 *   6. jump to the recompiled entry point
 *
 * XBE details (retail, 2005-10-14):
 *   Title:       Half-Life 2
 *   Title ID:    0x45410091
 *   Base addr:   0x00010000
 *   Entry point: 0x0059C612
 *   Code size:   6031 KB (.text) -- engine + client + server in one image
 *   Sections:    10, XDK 1.0.5849, D3D8LTCG
 *   Kernel imports: 124
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_icall_feedback.h"
#include "../kernel/kernel.h"
#include "../kernel/xbox_memory_layout.h"
#include "../d3d/d3d8_xbox.h"

/* RECOMP_TLS is required: the runtime defines these thread-local, and a plain
 * extern resolves to the image's TLS template rather than this thread's copy,
 * which starts the guest with every register at zero. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern ptrdiff_t g_xbox_mem_offset;

extern void xbe_entry_point(void);

#define HL2_XBE_DEFAULT  "game\\hl2_xbox.xbe"

/* The disc ships GameMedia/ and LoaderMedia/, but the title does not read them
 * from the disc: default.xbe (the loader) copies the .xz_ archives to
 * Z:\hl2\hl2x\*.xzp on the HDD first, per LoaderMedia/install.txt, and the
 * game then opens that path. So the file layer is pointed at game/ and the
 * Z: mapping is what has to line up -- see docs/ for where that stands. */
#define HL2_GAME_DIR     "game"
#define HL2_SAVE_DIR     "saves"

static const char *g_xbe_path;

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    long size;
    void *data;

    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return FALSE;
    }
    if (!(data = malloc((size_t)size))) {
        fclose(f);
        return FALSE;
    }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }
    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}


/* Report a host fault with the guest state that caused it.
 *
 * Recompiled code faults as ordinary native code, so without this a crash is
 * just "Segmentation fault" with nothing to act on. The guest registers are
 * globals, so they can be printed directly, and the faulting address minus
 * g_xbox_mem_offset says which guest address was touched -- which is what
 * distinguishes a null dereference from a wild pointer. */
static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;

    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    fprintf(stderr, "\n[FAULT] access violation at host %p\n",
            er->ExceptionAddress);
    if (er->NumberParameters >= 2) {
        uintptr_t addr = (uintptr_t)er->ExceptionInformation[1];
        fprintf(stderr, "  %s address host 0x%p",
                er->ExceptionInformation[0] ? "write to" : "read from",
                (void *)addr);
        if (g_xbox_mem_offset &&
            addr >= (uintptr_t)g_xbox_mem_offset &&
            addr < (uintptr_t)g_xbox_mem_offset + 0x10000000u)
            fprintf(stderr, "  = guest 0x%08X",
                    (uint32_t)(addr - (uintptr_t)g_xbox_mem_offset));
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "  guest regs: eax=%08X ecx=%08X edx=%08X ebx=%08X\n"
                    "              esp=%08X esi=%08X edi=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esp, g_esi, g_edi);
    /* The heap free-list bucket the allocator was walking.
     *
     * sub_0059FC74 indexes buckets as [esi + edi*8 + 0x180], and an empty one
     * must hold Flink = Blink = its own address. Printing it says whether
     * RtlCreateHeap ever initialised the list or whether it was corrupted
     * later, which are different bugs. */
    if (g_xbox_mem_offset && g_esi && g_edi < 0x80) {
        uint32_t bucket = g_esi + g_edi * 8 + 0x180;
        const uint32_t *p = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + bucket);
        const uint32_t *hdr = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esi);
        const uint32_t *b0 = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esi + 0x180);
        fprintf(stderr, "  heap bucket %u @0x%08X: Flink=%08X Blink=%08X"
                        " (expect both = %08X)\n",
                g_edi, bucket, p[0], p[1], bucket);
        /* 0xEEFFEEFF is the signature RtlCreateHeap writes at +0x10 just before
         * it initialises the free lists. Present means that header block ran and
         * something cleared the lists afterwards; absent means it never ran.
         * Bucket 0 says how far the initialisation loop got. */
        fprintf(stderr, "  heap hdr @0x%08X: sig=%08X (expect EEFFEEFF)  "
                        "bucket0 Flink=%08X (expect %08X)\n",
                g_esi, hdr[4], b0[0], g_esi + 0x180);
    }
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : HL2_XBE_DEFAULT;
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    g_xbe_path = path;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    AddVectoredExceptionHandler(1, veh_handler);

    printf("=== Half-Life 2 (Xbox) - Static Recompilation ===\n");

    if (!load_xbe(path, &xbe_data, &xbe_size))
        return 1;
    printf("XBE loaded: %zu bytes\n", xbe_size);

    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        fprintf(stderr, "Xbox memory layout init failed "
                        "(required VA range unavailable?)\n");
        free(xbe_data);
        return 1;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped at host offset 0x%llX\n",
           (unsigned long long)g_xbox_mem_offset);

    /* The APU reads from physical RAM, which is mapped at g_xbox_mem_offset,
     * so physical address 0 is that pointer. */
    {
        typedef struct MCPXAPUState MCPXAPUState;
        extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
        extern MCPXAPUState *g_apu_state;

        g_apu_state = mcpx_apu_init_standalone(
            (uint8_t *)(uintptr_t)xbox_GetMemoryOffset());
        printf("MCPX APU: %s\n", g_apu_state ? "initialised" : "FAILED");
    }

    xbox_WatchdogStart();

    printf("Initializing kernel replacement...\n");
    xbox_kernel_init();
    xbox_path_init(HL2_GAME_DIR, HL2_SAVE_DIR);
    xbox_kernel_bridge_init();

    g_esp = XBOX_STACK_TOP;

    /* ponytail: no GPU fence mirror registered yet.
     *
     * Wreckless needs xbox_Nv2aMirrorFence()/xbox_Nv2aFrameCounter() because
     * its D3D spins on a device field the real GPU would advance. HL2 links
     * D3D8LTCG, so the equivalent spin is inlined into game code rather than
     * sitting in a recognisable D3D function, and the device address is not
     * known yet. Expect a hang here; RECOMP_WATCHDOG_SECS will name the
     * function it is stuck in, and that is what identifies the fence. */

    printf("\nStarting game at entry point 0x0059C612...\n");
    fflush(stdout);

    RECOMP_ICALL_FEEDBACK_INIT();
    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");
    RECOMP_ICALL_FEEDBACK_DUMP();
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);
    return 0;
}
