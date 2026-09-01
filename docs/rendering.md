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

## 4. Assets have to load — and HL2's are behind an extra wrapper

Unlike the other titles, HL2's data is not readable off the disc as-is.

`LoaderMedia/install.txt` shows `default.xbe` copies the archives to the HDD
first, renaming as it goes:

```
"D:\GameMedia\zip0_xbox.xz_"      "Z:\hl2\hl2x\zip0_xbox.xzp"
"D:\GameMedia\zip0_xbox_%.xz_"    "Z:\hl2\hl2x\zip0_xbox_%.xzp"
```

So the game opens `Z:\hl2\hl2x\*.xzp`, never the disc path, and the file layer
has to present that.

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
