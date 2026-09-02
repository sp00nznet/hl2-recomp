/*
 * Host entry point for the recompiled default.xbe -- the disc's installer.
 *
 * Half-Life 2's disc ships two images. hl2_xbox.xbe is the game and reads
 * .xzp archives; the disc carries .xz_, which is an 'xCmp' container
 * (Microsoft XCompress / LZX). default.xbe is what converts one to the other,
 * following LoaderMedia/install.txt:
 *
 *   "D:\GameMedia\zip0_xbox.xz_"    "Z:\hl2\hl2x\zip0_xbox.xzp"
 *   "D:\GameMedia\zip0_xbox_%.xz_"  "Z:\hl2\hl2x\zip0_xbox_%.xzp"
 *
 * Recompiling the loader is cheaper than reimplementing LZX and is the same
 * decompressor the console used: 1,415 functions against the game's 48,335.
 * It also plays the logo videos and then launches the game, neither of which
 * matters here -- the install is the point, so a run that reaches
 * HalReturnToFirmware has done its job.
 *
 * D: is the disc (game/) and Z: is the hard disk (saves/Cache), matching what
 * the game itself is pointed at, so the archives land where it looks for them.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kernel/kernel.h"
#include "../kernel/xbox_memory_layout.h"
#include "recomp_types.h"

/* The guest stack pointer lives in the runtime, like the other registers. */
extern RECOMP_TLS uint32_t g_esp, g_ecx, g_eax;

/* Call a recompiled thiscall function from the host.
 *
 * The generated bodies end in "esp += 4; return;", so they expect a return
 * address on the guest stack even when the host is the caller. */
static void call_guest(void (*fn)(void), uint32_t this_ptr)
{
    g_ecx = this_ptr;
    g_esp -= 4;
    MEM32(g_esp) = 0xDEADBEEFu;
    fn();
}

extern void xbe_entry_point(void);

/* The loader's install, driven directly.
 *
 * default.xbe's main (sub_00014420) is:
 *
 *     sub_00012070(this)   construct
 *     sub_00014DD0(this)   bring up D3D        -- succeeds here
 *     sub_00014E60(this)   attract loop        -- UI, videos, input
 *
 * and the install is sub_00014030 (zero the state table, parse the manifest
 * with sub_00012F60, copy with sub_00012710), reached from inside that
 * attract loop. The loop polls a per-frame callback table that nothing here
 * populates, so it spins on a null pointer -- 385 million calls in 25
 * seconds -- and the install never starts.
 *
 * Calling the install directly skips the UI, which is the whole point: the
 * loader is here because it owns the LZX decompressor, not the logo videos.
 */
extern void sub_00012070(void);   /* construct */
extern void sub_00014DD0(void);   /* device init */
extern void sub_00014030(void);   /* DoInstall */
int recomp_dispatch_init(void);

#define LOADER_XBE       "game/default.xbe"
#define LOADER_GAME_DIR  "game"
#define LOADER_SAVE_DIR  "saves"

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
    if (size <= 0) { fclose(f); return FALSE; }
    if (!(data = malloc((size_t)size))) { fclose(f); return FALSE; }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data); fclose(f); return FALSE;
    }
    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

int main(int argc, char **argv)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;
    const char *xbe_path = (argc > 1) ? argv[1] : LOADER_XBE;

    printf("HL2 Xbox loader (default.xbe) -- performing the HDD install\n");

    if (!load_xbe(xbe_path, &xbe_data, &xbe_size))
        return 1;
    printf("Loaded %s (%zu bytes)\n", xbe_path, xbe_size);

    if (!recomp_dispatch_init()) {
        fprintf(stderr, "recomp_dispatch_init failed\n");
        return 1;
    }

    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        fprintf(stderr, "xbox_MemoryLayoutInit failed\n");
        return 1;
    }

    xbox_WatchdogStart();
    xbox_kernel_init();
    xbox_path_init(LOADER_GAME_DIR, LOADER_SAVE_DIR);
    xbox_kernel_bridge_init();

    g_esp = XBOX_STACK_TOP;

    /* The same GPU fence the game waits on, at the loader's own device.
     *
     * Its D3D spins in sub_0002A170 comparing the contiguous block at
     * [device+0x30] against what it has submitted at [device+0x2C] -- the
     * identical shape to the game's sub_00612430, because it is the same
     * D3D8LTCG. 0x00034048 is the global holding the device pointer, the
     * loader's equivalent of the game's 0x0061EDE8. */
    xbox_Nv2aMirrorFence(0x00034048u, 0x2Cu, 0x30u);

    printf("\nRunning the loader...\n");
    fflush(stdout);

    /* Its own entry point, so the CRT runs: calling the install directly
     * faulted in the constructor because none of the loader's globals had
     * been initialised. The attract loop is replaced by symbol instead --
     * see recomp/recomp_manual.c. */
    xbe_entry_point();

    printf("\nLoader returned.\n");
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);
    return 0;
}
