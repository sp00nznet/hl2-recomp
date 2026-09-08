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
#include <dbghelp.h>
#include <stdbool.h>
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

/* Why the loader draws nothing, measured rather than read.
 *
 * Its frame function skips every string when the font object is null:
 *
 *     mov eax, [ebx + 0x22a8]     ; the font
 *     test eax, eax
 *     je   0x1222a                ; null -> draw nothing, submit nothing
 *
 * The font is built in sub_00014060 from a 5,413-byte blob in .rdata, but the
 * call is guarded on a status word at [obj + 0x310] -- error class 0x9E skips
 * it. Which of those is happening cannot be told apart by reading, only by
 * looking, and the loader has none of the instrumentation the game has.
 *
 * The object is findable without knowing where it was allocated: its
 * constructor writes the vtable 0x0006F928 into its first word, and nothing
 * else in the image holds that value. Scan for it, then read the two fields.
 */
#define LOADER_VTABLE   0x0006F928u
#define LOADER_FONT_OFF 0x22A8u
#define LOADER_STAT_OFF 0x0310u

static DWORD WINAPI loader_probe(LPVOID unused)
{
    uint32_t found = 0;
    int reported = 0;

    (void)unused;
    for (;;) {
        uintptr_t base = (uintptr_t)xbox_GetMemoryOffset();
        uint32_t va;

        Sleep(3000);
        if (!base)
            continue;

        /* Re-scan until it is found; the object does not exist at startup. */
        if (!found) {
            for (va = 0x00010000u; va < 0x04000000u; va += 4) {
                if (*(const uint32_t *)(base + va) == LOADER_VTABLE) {
                    found = va;
                    fprintf(stderr, "  [PROBE] loader object at 0x%08X\n", va);
                    break;
                }
            }
            if (!found)
                continue;
        }

        {
            uint32_t font = *(const uint32_t *)(base + found + LOADER_FONT_OFF);
            uint32_t stat = *(const uint32_t *)(base + found + LOADER_STAT_OFF);
            /* The state machine and the two flags that route to the fatal
             * error screen at sub_00012170:
             *
             *   [+0x2290] && [+0x218]  -> error screen
             *   [+0x2270]              -> state, 0xA is the last one
             *
             * Reading them beats reading the disassembly, which has been
             * wrong about this loader three times now. */
            /* The buffer the video was read into. The loader allocates
             * 1,736,752 bytes at 0x01081000 and reads the whole file into
             * it; a valid XMV has the magic "Xbox" at offset 12. If that
             * is not there, the read went somewhere the parser is not
             * looking and the parser is right to reject it. */
            {
                const unsigned char *v =
                    (const unsigned char *)(base + 0x01081000u);
                const unsigned char *w =
                    (const unsigned char *)(base + 0x81081000u);
                fprintf(stderr, "  [PROBE] window 0x81081000: %02X %02X %02X %02X"
                                " magic=%c%c%c%c\n",
                        w[0], w[1], w[2], w[3],
                        w[12], w[13], w[14], w[15]);
                fprintf(stderr, "  [PROBE] xmv buf: %02X %02X %02X %02X"
                                " magic=%c%c%c%c\n",
                        v[0], v[1], v[2], v[3],
                        v[12], v[13], v[14], v[15]);
            }
            fprintf(stderr, "  [PROBE] state=%u f2290=%u f218=0x%08X\n",
                    *(const uint32_t *)(base + found + 0x2270u),
                    *(const unsigned char *)(base + found + 0x2290u),
                    *(const uint32_t *)(base + found + 0x0218u));
            fprintf(stderr, "  [PROBE] font=0x%08X status=0x%08X%s\n",
                    font, stat,
                    font ? "" : "   <- null, every string is skipped");
            /* The D3D device, field by field.
             *
             * The title submits its frame, asks for a flip and then waits on
             * a word it expects hardware to change. Which word is faster to
             * watch than to read out of the disassembly: print the first
             * sixteen dwords every few seconds, and the one that never moves
             * while its neighbours do is the answer. 0x00034048 holds the
             * device pointer -- the same global the fence mirror uses. */
            {
                uint32_t dev = *(const uint32_t *)(base + 0x00034048u);
                if (dev && dev < 0x04000000u) {
                    int k;
                    fprintf(stderr, "  [PROBE] device 0x%08X:", dev);
                    for (k = 0; k < 16; k++)
                        fprintf(stderr, " %08X",
                                *(const uint32_t *)(base + dev + k * 4));
                    fprintf(stderr, "\n");
                }
            }
            fflush(stderr);
            if (++reported > 12)
                break;
        }
    }
    return 0;
}

/* Which function is it spinning in.
 *
 * Every theory about where the loader stops has been formed by reading its
 * disassembly, and every one has been wrong: the font was not null, and the
 * function it looked parked in turned out to be Release. The guest has no
 * instruction pointer of its own -- it is recompiled C -- but the host
 * thread running it does, and a suspended sample of that names the
 * generated function, which names the guest one.
 *
 * Ported from the game main.c, which has had this for a while. The loader
 * never got it, which is why five build cycles went into guessing. */
static HANDLE g_probe_thread;

static void probe_symbol(void *addr)
{
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen   = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)addr,
                    &disp, sym))
        fprintf(stderr, "  [SPIN] %s + 0x%llX\n", sym->Name,
                (unsigned long long)disp);
    else
        fprintf(stderr, "  [SPIN] %p (no symbol)\n", addr);
}

static DWORD WINAPI loader_spin_probe(LPVOID unused)
{
    int i;

    (void)unused;
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    for (i = 0; i < 6; i++) {
        CONTEXT ctx;

        Sleep(6000);
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (SuspendThread(g_probe_thread) == (DWORD)-1)
            break;
        if (GetThreadContext(g_probe_thread, &ctx)) {
            fprintf(stderr, "\n[SPIN] sample %d:\n", i + 1);
            probe_symbol((void *)(uintptr_t)ctx.Rip);
        }
        ResumeThread(g_probe_thread);
        fflush(stderr);
    }
    return 0;
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

extern ptrdiff_t g_xbox_mem_offset;

/* Report where a guest fault landed, in guest addresses. */
/* The emulated APU answers its own registers; see loader_veh. */
bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                          uint32_t fault_xbox_va, int is_write);

static LONG CALLBACK loader_veh(PEXCEPTION_POINTERS ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    uintptr_t base = (uintptr_t)g_xbox_mem_offset;

    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    /* Trapped device registers, before this is treated as a crash.
     *
     * RECOMP_AC97_READY makes the APU aperture PAGE_NOACCESS so its
     * registers can be answered by the emulated APU, and until now
     * nothing answered them: apu_hook_handle_mmio existed in the tree
     * with no caller at all. The result was a title that got further and
     * then took an access violation on the first APU register it read.
     *
     * The loader needs this because its logo videos play their audio
     * through DirectSound, which is what the APU is for. */
    if (base && er->NumberParameters >= 2) {
        uintptr_t fault = (uintptr_t)er->ExceptionInformation[1];

        if (fault >= base) {
            uint32_t va = (uint32_t)(fault - base);

            if (va >= 0xFE800000u && va < 0xFE880000u
             && apu_hook_handle_mmio(ep->ContextRecord, fault, va,
                                     (int)er->ExceptionInformation[0]))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    fprintf(stderr, "\n[FAULT] host %p", er->ExceptionAddress);
    if (er->NumberParameters >= 2) {
        uintptr_t a = (uintptr_t)er->ExceptionInformation[1];
        fprintf(stderr, "  %s host 0x%p",
                er->ExceptionInformation[0] ? "write" : "read", (void *)a);
        if (base && a >= base && a < base + 0x100000000ull)
            fprintf(stderr, " = guest 0x%08X", (uint32_t)(a - base));
    }
    fprintf(stderr, "\n  guest esp 0x%08X\n", g_esp);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---- xCmp extraction -------------------------------------------------- *
 *
 * The disc's .xz_ files are Microsoft XCompress containers. Rather than
 * reimplement LZX, run the decompressor default.xbe already carries.
 *
 *   sub_00011000(src_len, src, dst_cap, dst)  walks one chunk's records
 *   sub_0001CE74(src, dst)                    decodes one LZX block
 *
 * sub_00011000 is the record walker, and reading it is what pinned the
 * framing down:
 *
 *     len = *(uint16 *)src;
 *     if (len == 0)      break;                      end of chunk
 *     if (len & 0x8000)  memcpy(dst, src + 2, len & 0x7fff);
 *     else               dst += sub_0001CE74(src + 2, dst);
 *
 * so the top bit marks a stored block and the low 15 bits are its size --
 * the 0xC000 records are 0x8000 | 0x4000, a stored block of one 16 KB block.
 *
 * A chunk is one 0x80000 window, zero-padded, and LZX back-references never
 * cross one, so chunks decode independently and the buffers stay small: the
 * largest chunk in zip0_xbox.xz_ produces 3,735,552 bytes.
 */
#define XCMP_MAGIC     0x78436D70u
#define XCMP_HDR_SIZE  24
#define XCMP_SRC_CAP   0x00080000u      /* one window                     */
#define XCMP_DST_CAP   0x00800000u      /* 8 MB, vs the 3.6 MB worst case */

extern void sub_00011000(void);         /* the record walker */

/* Call a recompiled cdecl function: arguments right to left, caller cleans. */
static uint32_t call_guest_cdecl(void (*fn)(void), const uint32_t *args, int n)
{
    int i;

    for (i = n - 1; i >= 0; i--) {
        g_esp -= 4;
        MEM32(g_esp) = args[i];
    }
    g_esp -= 4;
    MEM32(g_esp) = 0xDEADBEEFu;         /* the return address it pops */
    fn();
    g_esp += (uint32_t)n * 4;
    return g_eax;
}

static int extract_xcmp(const char *in_path, const char *out_path)
{
    unsigned char hdr[XCMP_HDR_SIZE];
    uint32_t magic, version, usize, window, block;
    uint32_t src_va, dst_va;
    unsigned char *src_host, *dst_host;
    unsigned long long produced = 0;
    unsigned chunk = 0;
    FILE *fin, *fout;
    long long file_size;

    if (!(fin = fopen(in_path, "rb"))) {
        fprintf(stderr, "cannot open %s\n", in_path);
        return 1;
    }
    if (fread(hdr, 1, sizeof(hdr), fin) != sizeof(hdr)) {
        fprintf(stderr, "%s: short header\n", in_path);
        fclose(fin);
        return 1;
    }
    memcpy(&magic, hdr + 0, 4);
    memcpy(&version, hdr + 4, 4);
    memcpy(&usize, hdr + 8, 4);
    memcpy(&window, hdr + 12, 4);
    memcpy(&block, hdr + 20, 4);

    if (magic != XCMP_MAGIC || version != 1) {
        fprintf(stderr, "%s: not an xCmp v1 container\n", in_path);
        fclose(fin);
        return 1;
    }
    if (window > XCMP_SRC_CAP) {
        fprintf(stderr, "%s: %u byte window exceeds the buffer\n",
                in_path, window);
        fclose(fin);
        return 1;
    }
    fseek(fin, 0, SEEK_END);
    file_size = ftell(fin);

    printf("%s\nxCmp v1: %u bytes out, %u KB window, %u byte blocks\n",
           in_path, usize, window / 1024, block);

    if (!(fout = fopen(out_path, "wb"))) {
        fprintf(stderr, "cannot write %s\n", out_path);
        fclose(fin);
        return 1;
    }

    src_va = xbox_HeapAlloc(XCMP_SRC_CAP, 4096);
    dst_va = xbox_HeapAlloc(XCMP_DST_CAP, 4096);
    if (!src_va || !dst_va) {
        fprintf(stderr, "cannot allocate the guest buffers\n");
        fclose(fin);
        fclose(fout);
        return 1;
    }
    src_host = (unsigned char *)XBOX_PTR(src_va);
    dst_host = (unsigned char *)XBOX_PTR(dst_va);

    /* Chunks are window-aligned from the end of the header, so each read is
     * just the next window -- the walker stops on its own at the padding. */
    for (;;) {
        /* Chunk boundaries are window multiples of the file offset, so the
         * first one is short by the header rather than starting a window
         * after it. */
        long long at  = chunk ? (long long)chunk * window : XCMP_HDR_SIZE;
        long long end = (long long)(chunk + 1) * window;
        size_t want, got;
        uint32_t args[4], out;

        if (end > file_size)
            end = file_size;
        if (at >= end)
            break;
        want = (size_t)(end - at);
        fseek(fin, (long)at, SEEK_SET);
        got = fread(src_host, 1, want, fin);
        if (got == 0)
            break;
        memset(src_host + got, 0, XCMP_SRC_CAP - got);

        args[0] = (uint32_t)got;
        args[1] = src_va;
        args[2] = XCMP_DST_CAP;
        args[3] = dst_va;
        out = call_guest_cdecl(sub_00011000, args, 4);

        if (out == 0xFFFFFFFFu) {
            fprintf(stderr, "chunk %u overflowed the output buffer\n", chunk);
            fclose(fin);
            fclose(fout);
            return 1;
        }
        if (out && fwrite(dst_host, 1, out, fout) != out) {
            fprintf(stderr, "write failed at chunk %u\n", chunk);
            fclose(fin);
            fclose(fout);
            return 1;
        }
        produced += out;
        chunk++;
        if ((chunk % 64) == 0) {
            printf("  %u chunks, %llu / %u bytes\n", chunk, produced, usize);
            fflush(stdout);
        }
    }

    fclose(fin);
    fclose(fout);
    printf("  %u chunks, %llu / %u bytes\n", chunk, produced, usize);

    if (produced != usize) {
        fprintf(stderr, "  size mismatch -- the header wants %u\n", usize);
        return 1;
    }
    printf("  -> %s\n", out_path);
    return 0;
}

int main(int argc, char **argv)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;
    int extract = (argc > 3 && strcmp(argv[1], "--extract") == 0);
    const char *xbe_path = (!extract && argc > 1) ? argv[1] : LOADER_XBE;

    setvbuf(stdout, NULL, _IONBF, 0);
    AddVectoredExceptionHandler(1, loader_veh);
    printf("HL2 Xbox loader (default.xbe) -- %s\n",
           extract ? "extracting an xCmp archive" : "performing the HDD install");

    if (!load_xbe(xbe_path, &xbe_data, &xbe_size))
        return 1;
    printf("Loaded %s (%zu bytes)\n", xbe_path, xbe_size);

    if (!recomp_dispatch_init()) {
        fprintf(stderr, "recomp_dispatch_init failed\n");
        return 1;
    }

    /* The default 64 MB map is left alone deliberately.
     *
     * XMV playback writes through the tiled alias at 0xF0000000 and runs
     * off the end of it. Enlarging the map does not fix that -- it just
     * moves the fault: at 64 MB it lands on 0xF4000000, and at 256 MB it
     * lands on 0xFE000000, having walked the whole aperture and the NV2A
     * register block behind it. Both are exactly the end of whatever is
     * mapped, which makes it an unbounded write rather than a small one,
     * and 64 MB is what the console actually has. */

    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        fprintf(stderr, "xbox_MemoryLayoutInit failed\n");
        return 1;
    }

    /* The MCPX APU, the same bring-up the game does.
     *
     * The loader plays its logos through XMV, and XMV playback opens
     * DirectSound for the audio track. Without an APU DirectSoundCreate
     * returns DSERR_NODRIVER, the playback call fails, and the loader
     * concludes the disc is unreadable and puts up its fatal error screen --
     * which is exactly what it was doing: sub_00014880 returning 0x88780078,
     * traced right through to the error screen.
     *
     * The codec still has to report ready for DirectSound to get that far,
     * which is RECOMP_AC97_READY. */
    {
        typedef struct MCPXAPUState MCPXAPUState;
        extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
        extern MCPXAPUState *g_apu_state;

        g_apu_state = mcpx_apu_init_standalone(
            (uint8_t *)(uintptr_t)xbox_GetMemoryOffset());
        printf("MCPX APU: %s\n", g_apu_state ? "initialised" : "FAILED");
    }

    xbox_WatchdogStart();
    if (getenv("RECOMP_LOADER_SPIN")
     && DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                        GetCurrentProcess(), &g_probe_thread, 0,
                        FALSE, DUPLICATE_SAME_ACCESS))
        CloseHandle(CreateThread(NULL, 0, loader_spin_probe, NULL, 0, NULL));
    if (getenv("RECOMP_LOADER_PROBE"))
        CloseHandle(CreateThread(NULL, 0, loader_probe, NULL, 0, NULL));
    xbox_kernel_init();
    xbox_path_init(LOADER_GAME_DIR, LOADER_SAVE_DIR);
    xbox_kernel_bridge_init();

    g_esp = XBOX_STACK_TOP;

    /* Decompressing does not need the loader's own boot: sub_00011000 and
     * the LZX decoder under it read only their arguments and the static
     * tables already present in the mapped image. */
    if (extract) {
        int rc = extract_xcmp(argv[2], argv[3]);
        xbox_kernel_shutdown();
        xbox_MemoryLayoutShutdown();
        free(xbe_data);
        return rc;
    }

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
