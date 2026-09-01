# Getting Pixels Out of HL2

The goal is a visible frame. This is what stands between here and there, what
the sibling projects already learned, and what is specific to HL2.

## The shape of the problem

The XBE contains its own D3D. It is not a call into an API we shim — it is code
we recompile, and that code talks to the GPU by **building an NV2A command
stream in guest RAM and advancing `DMA_PUT`**. On real hardware the GPU consumes
that stream. Here, nothing does, so the framebuffer stays whatever it was.

Wreckless is the furthest-along title and it stops in exactly this place:

> Submits valid NV2A pushbuffers; nothing executes them. `RECOMP_PB_SCAN=1`
> decodes the stream cleanly — 617 words, 245 distinct methods, 0 unrecognised —
> and nothing executes it, so the framebuffer stays black.

So "recompiled correctly" and "renders" are separate milestones, and HL2 will
hit the same wall for the same reason. What follows is the ordered list of
things that have to be true.

## 1. It has to boot far enough to submit anything

The first pixel is downstream of a lot of engine init. Wreckless taught the
lesson worth carrying: **the blocker under rendering was audio.** Its whole
engine init sat behind `DirectSoundCreate` succeeding, so a failed AC'97 probe
left a null table pointer, which produced a NaN transform, which produced a
garbage index, which crashed — and several rounds of chasing a *rendering* crash
ended at *audio*.

`main.c` therefore brings the MCPX APU up before the kernel, not as an
afterthought. Expect HL2 to have its own version of this: a single early failure
that gates far more than it looks like it should.

## 2. The GPU fence has to advance

D3D8LTCG spins waiting for the GPU to catch up. In the device context (see
`../../xboxrecomp/docs/technical/d3d8ltcg-device-context.md`, mapped from
Burnout 3 — **same XDK 1.0.5849 D3D8LTCG as HL2**):

| Offset | Field |
|---|---|
| +0x00 | `pb_put` — PB write cursor |
| **+0x30** | **`pb_gpu_read_ptr` — pointer to GPU read position** |
| **+0x34** | **`fence_write_idx`** |
| +0x48 | `fence_array_ptr` |

Wreckless resolves this with one call, because the GPU never runs and the D3D11
layer draws instead:

```c
xbox_Nv2aMirrorFence(device_ptr, 0x30, 0x34);
xbox_Nv2aFrameCounter(device_ptr, 0x245C);
```

**The HL2-specific unknown is `device_ptr`.** For Burnout 3 the device is a
static at `0x0035D6A0` with a global pointer at `0x35FB48`. HL2's D3D section is
only 85 KB (`0x0060B100`–`0x00620004`) and the ~16 KB context will live in
`.data`, so the address has to be found, not assumed. Two ways:

- statically — find the large `.data` static that D3D-section code initialises
  and returns from device creation;
- dynamically — run it, let `RECOMP_WATCHDOG_SECS` name the function it is
  spinning in, and read the address off the spin.

The dynamic route is cheaper and is what the other titles used. `main.c`
deliberately registers no fence yet and says so in a comment, because a
*plausible* wrong address means the ack thread writes through a pointer into
live game state, which is worse than hanging.

D3D8LTCG makes this harder than it was for Wreckless: link-time codegen inlines
D3D into the caller, so the spin is not sitting in a tidily-named D3D function.

## 3. Something has to consume the pushbuffer

`xboxrecomp` already ships a partial executor, `src/kernel/nv2a_pb_exec.c`
(`RECOMP_PB_EXEC=1`). Its own header is honest about the split:

> This walks the same command stream `nv2a_pb_scan.c` surveys and carries out the
> subset that decides what is on screen: which surface is being drawn into, and
> clearing it. **Geometry is deliberately not here.** Draws need vertex-array
> gathering, `ARRAY_ELEMENT16` batching, texture upload with Xbox swizzling and
> translated vertex programs — a renderer, not a command decoder.

It handles `SET_SURFACE_*`, `SET_COLOR_CLEAR_VALUE`, `CLEAR_SURFACE`. Anything
it does not handle is counted and ranked by `nv2a_pb_exec_report()`, so the gap
is a list rather than a guess.

**That is enough for a first visible pixel.** A cleared surface at the right
address, shown with `RECOMP_FB_WINDOW=1`, is a real frame — the clear colour is
the title's own. It is not the game rendering itself, and it should not be
reported as such, but it is the first thing on screen and it proves the surface
address, pitch and format are right.

### The executor now rasterises too

`nv2a_pb_exec.c` fills triangles, strips, fans and quads flat into the guest
framebuffer — but only batches whose attribute 0 is **already in screen space**,
and that is measured (every vertex must land inside the surface) rather than
assumed. Titles draw UI, HUD and 2D overlays pre-transformed, so that is the
first geometry to appear. A batch needing a vertex program executed is counted
as skipped rather than drawn somewhere wrong.

Two switches exist because "nothing on screen" has three indistinguishable
causes — wrong surface/pitch, broken rasteriser, empty vertex buffers:

| Variable | What it does |
|---|---|
| `RECOMP_RASTER_TEST=1` | draw one known triangle after every clear |
| `RECOMP_FB_DUMP=<prefix>` | write the surface to `<prefix>000.bmp` |

**Proven on Wreckless.** The self-test triangle rasterises correctly at 640x480
into a 2 bpp surface and dumps to a BMP you can open. Its own batches decode
correctly too — prim 5, 6 indices, attribute 0 float, size 2, stride 16, at
`0x01956000` — but that buffer stays all zeros, and `RECOMP_FIND_QUAD=1` reports
the vertices are "not in RAM in that form" anywhere. So the rasteriser and the
surface are right, and Wreckless's remaining blocker is that its engine never
computes the geometry: the chain its README documents, from `DirectSoundCreate`
failing through a NaN transform matrix.

That is the value of the split — the same three causes will present identically
on HL2, and now they are distinguishable in one run.

## The boot chain, as far as it is understood

Mapped by disassembly and confirmed against a run. Entry point is `0x0059C612`.

```
0x0059C612  entry
  sub_0059C9E8              CRT _beginthreadex
    PsCreateSystemThreadEx(routine=0x0059C950, ctx1=0x0059C59E)
      sub_0059C950          TLS copy from template, SEH prologue
        sub_0059C6DB        callback-list walk  <- Enter/Leave pair #1
        call [ebp+8]        -> ctx1
          sub_0059C59E      CRT thread init
            sub_0059DF65    reads globals, no kernel calls
            sub_0059D73E    calls HalRegisterShutdownNotification
            sub_0059DED8, sub_0059DE80
            sub_005969A0    startup timing + 7 calls to the Msg-alike
              sub_005C0EA0  THE ENGINE: thiscall methods, string
                            construction, vtable dispatch
        sub_0059C6DB        callback-list walk  <- Enter/Leave pair #2
      PsTerminateSystemThread
```

Two of these needed seeding before anything ran at all, and neither is
findable statically: `0x0059C950` is only ever *pushed* as an argument, and
`0x0059C59E` only reached through `[ebp+8]`. Both come from
`tools.seed_from_log` and live in `config/seed_functions.json`.

**Where it stops.** The thread runs and terminates after four kernel calls --
two `RtlEnterCriticalSection`/`RtlLeaveCriticalSection` pairs, which are exactly
the two `sub_0059C6DB` walks. Those walks *bracket* the indirect call, so
execution provably passed through it, and there are zero
`[ICALL] Failed to resolve` lines. So the game main is entered and returns
early, having touched no bridged kernel call.

`sub_0059C6DB` itself explains why the walks are silent: it iterates a circular
list at `0x815B14` calling `[eax+8]` per entry, and an empty list means the body
never runs. Enter, nothing, Leave.

Note `sub_005969A0` is *not* the engine main despite being what the CRT calls --
it is a timing report that tail-calls `sub_005C0EA0`, which is.

Static reading cannot separate "returned early" from "never entered" past this
point, so the next step is `tools.recomp --trace-functions` over the chain
(`config/trace_boot.json`), which emits entry/exit for each. The answer decides
between three unrelated fixes: a lifting bug in a CRT function, an engine-level
early-out on state we have not set up, or a call-lowering problem.

## Where it stops today: the filesystem interface lookup

The engine runs. `sub_005C0EA0` is the launcher's `WinMain` -- it names itself,
via the profiling string at `0x772BBC` -- and it hangs looking up its first
interface. Backtrace, from the watchdog through `tools/stackwalk.py`:

```
sub_005C0EA0+0x6A     engine WinMain
  sub_005A1610+0xA6
    sub_005A10C0+0x4C   CreateInterface("VFileSystem017")
      sub_005A1700       module-table dispatch
        sub_00011070+0x9
          sub_000111E0   CUtlLinkedList walk
```

`sub_005A1700` indexes a 12-byte-stride module table and calls the factory in
it:

```
mov edx, [eax+ecx]      ; entry.module   -> 0
je  ...
mov eax, [eax+ecx+4]    ; entry.factory  -> 0
call eax                ; calls 0
```

**Both fields are null**, so it calls address 0. RECOMP_ICALL rejects that as
not-code and returns `eax = 0`, and the caller then spins on the null result.

Three measurements pin this down, and each was needed:

| Question | Tool | Answer |
|---|---|---|
| Hung, or just slow? | watchdog `icalls so far` | 461 at 15s **and** 45s -- a tight spin |
| Does the null repeat? | `recomp_icall_not_code_log` frequency | exactly once |
| Is the list corrupt? | watchdog `RECOMP_PEEK` | no: head is `0xFFFF`, init flag set |

461 is also the call number the null was logged at, so the null call is the
*last* thing that happens. Skipping it is what causes the hang, not repeating
it -- a distinction that took both the frequency count and the running total to
establish, and that a first reading of the evidence got backwards.

### Why no factory is registered: the static-init walk stops at 8%

`sub_0059DE80` is MSVC's `_initterm`. Its second loop walks the C++
constructor array with `esi` as the cursor and `edi` as the limit:

```
esi = 0x7A3530; edi = 0x7A8818;      /* 5,306 slots, 5,305 non-null */
loop: eax = MEM32(esi);
      if (eax && eax != -1) ICALL(eax);
      esi += 4; if (esi < edi) goto loop;
```

A probe at its epilogue caught it leaving with `esi = 0x00F7FECC` and
`edi = 0x00F7FEC4` -- both *stack* addresses, and `esi < edi` false, so the
loop had already exited. The recompiler keeps `esi`/`edi` in globals rather
than on the host stack, so a callee that fails to restore them corrupts the
caller directly. Total indirect calls for the whole run was 461 against 5,305
constructors: the walk ran 8% of the list.

The cause was in the disassembler. MSVC parks a switch table *inside* the
function body, and `tools.disasm` decoded those code pointers as instructions,
came out of phase, and never reached the epilogue -- so `pop esi / pop edi`
was never lifted. 219 such tables in this XBE. Fixed upstream (see
[upstreaming.md](upstreaming.md)); `memcpy` alone went from 262 lifted bytes
to its real 664, and 38 functions regained 8,168 bytes of body.

`-DRECOMP_ABI_CHECK` now reports any function that returns with `ebx`, `esi`
or `edi` changed, so the next one of these is a build flag rather than six
wrong theories and a hand-placed probe.

### What actually unblocked it: surviving a bad constructor

The register clobber was real but it was not the binding constraint. The loop
stopped because one constructor *faulted*, and a fault ended static
initialisation outright -- so every constructor after it, including whichever
registers the filesystem interface, simply never ran.

Wrapping each constructor call in `__try` changed the picture completely:

| | indirect calls | outcome |
|---|---|---|
| lifted `_initterm` | 461 | stops at ctor #56 |
| native walk, restoring esi/edi/ebx | 461 -> ctor #296 | faults in ctor #296 |
| native walk + `__try` | **104,025** | runs on |

Of the hundreds of constructors that now run, **three** clobber callee-saved
state and **one** faults. That is a handful of badly lifted functions, not a
systematic failure -- which is the single most useful thing the survey
established, because it says this is worth fixing one at a time.

Worth recording what did *not* work, since both cost a full build:

- `-DRECOMP_ABI_CHECK` reported zero violations. It hooks the `ICALL` macros,
  so it sees indirect calls only, and CRT and static-init paths are almost
  entirely direct C calls to symbols with no macro to hook. Right tool for
  vtable-heavy code, wrong one for early boot.
- The bogus heap handle reaching `RtlAllocateHeap` was a symptom, not a cause:
  `free()` was running on a pointer from an already-broken constructor.
  Continuing past that constructor made it moot.

The run now stops on two indirect-call targets with no generated body,
`0x0001D0F8` and `0x000618C1`. Both sit in gaps between detected functions and
both decode as complete routines -- a 21-byte comparator and a thiscall
constructor returning `this` -- so they are entry points the static pass
missed. They are in `config/seed_functions.json`, which exists for exactly
this.

So the remaining problem is not lifting and not the list: **no factory is
registered for the filesystem interface**. In Source, `EXPOSE_INTERFACE` builds
that registration from a static constructor, and static initialisation now runs.
Finding which registration is missing, or which module load silently returned an
empty slot, is the next step.

Two strings worth knowing sit next to this code:

- `0x772BC4` = `r:\hl2\hl2x` -- a dev-tree game path left in the retail build,
  next to the `T:/hl2/hl2x/%s%s` runtime paths
- `0x772BD0` = `g_pEngineAPI->SetStartupInfo` -- an assert string, so the
  sequence is filesystem, then engine API, then `SetStartupInfo`

Note `0x772BA4` is *not* a string despite sitting among them: it is a function
pointer (`0x005C0AE0`) in a callback table, which is worth saying because
reading it as text is an easy mistake to make twice.

## 4. Assets have to load — and HL2's are behind an extra wrapper

Unlike the other titles, HL2's data is not readable off the disc as-is.

`LoaderMedia/install.txt` shows `default.xbe` copies the archives to the HDD
first, renaming as it goes:

```
"D:\GameMedia\zip0_xbox.xz_"      "Z:\hl2\hl2x\zip0_xbox.xzp"
"D:\GameMedia\zip0_xbox_%.xz_"    "Z:\hl2\hl2x\zip0_xbox_%.xzp"
```

But `install.txt` is what the *loader* does, not what the game asks for. The
format strings in `hl2_xbox.xbe` are the authority, and they say more:

```
D:/GameMedia/maps/%s.bsp      <- maps come straight off the disc
zip%i_xbox%s.xzp              <- the archive name pattern
T:/hl2/hl2x/%s%s              T:/cfg/%s      T:/HL2/      Z:/HL2/
```

Two consequences. `D:` maps to `<game_dir>`, so **map loading needs no unwrapping
at all** — the ~120 BSPs are uncompressed on the disc and already in place. And
the game reads its packages from `T:` as well as `Z:`, which in the runtime's
path layer (`kernel_path.c`) are `<save_dir>/TitleData/` and `<save_dir>/Cache/`
respectively. So the XZP has to land at `saves/Cache/hl2/hl2x/` or
`saves/TitleData/hl2/hl2x/`, not next to the disc files.

There are no absolute drive paths in the binary — Source composes them at
runtime from a root — so these format strings are the only statement of intent.

The `.xz_` files are **not** XZP archives with a different extension. The header
is:

```
70 6d 43 78  01 00 00 00  53 49 E8 19  00 00 08 00
"pmCx"       version 1
```

`pmCx` is `xCmp` byte-reversed — Valve's **xCompress** container (LZX). The
252 MB `zip0_xbox.xz_` has to be decompressed to an XZP before anything can read
it. Once it is an XZP, GCFScape 1.4.1, HLLib and
[`desukuran/lynx-project`](https://github.com/desukuran/lynx-project) all read
the format.

**The game cannot do that unwrapping itself.** `pmCx` and the constant
`0x78436D70` appear exactly once in `default.xbe` and *zero* times in
`hl2_xbox.xbe`. So xCompress lives entirely in the loader: it decompresses
during the HDD copy, and the game only ever opens a plain XZP. That settles a
question the file names leave open -- `.xz_` to `.xzp` is a real format change,
not a rename -- and it means the unwrapping is an offline, one-time step rather
than a runtime path to shim.

Two ways to get it, and the second is more interesting than it sounds:

1. Write the LZX decoder. It is a Microsoft container, so it belongs upstream
   in the toolkit rather than here.
2. **Recompile `default.xbe`.** It is 467 KB against the game's 8.4 MB, it has
   no renderer to speak of, and its whole job is this copy. Running the port
   house's own installer to produce its own asset files is a smaller problem
   than reimplementing its decompressor, and it validates the toolkit on a
   second target from the same disc.


Two options, and the second is probably right:

1. decompress offline, ship the `.xzp` in a `Z:` directory the path layer maps;
2. decompress on open in the file layer, so the disc layout stays untouched.

Neither is written yet.

## Order of work

1. **Compile.** 41,215 functions of C++-derived C is itself unproven at this
   size; MSVC time and correctness are both unknowns.
2. **Boot until it stops**, with `RECOMP_TRAP_NULL=1` and
   `RECOMP_WATCHDOG_SECS` on — a null read that returns zero surfaces hundreds
   of steps later as a NaN, and the trap turns it into one fault with a name.
3. **Find the D3D device context** from wherever it spins; register the fence.
4. **Unwrap the xCompress archives** so asset loads succeed.
5. **`RECOMP_PB_SCAN=1`** to confirm the command stream decodes, then
   `RECOMP_PB_EXEC=1` + `RECOMP_FB_WINDOW=1` for a cleared surface.
6. Geometry. That is a renderer, and it is a different project.

## Feeding runtime back into codegen

Static analysis cannot see where a vtable call goes; running the title can.
Wreckless's `seed_from_log.sh` feeds unresolved indirect-call targets from a run
back into the seed file, and that loop found its two worst bugs.

HL2 starts from a much better place — RTTI resolved 176,865 vtable slots
statically, which is most of that surface — but the loop still applies for what
RTTI cannot see. **Both gates matter**: a seed must be in an executable section
*and* decode as a function body. Seeding an address that is not a function start
truncates the function containing it, and the damage shows up nowhere near the
seed. On Wreckless, seeding two kernel-thunk-table entries once took a boot that
reached 34 assets down to 1.
