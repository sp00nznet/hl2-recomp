# Boot path

Notes on the engine's own startup, recovered while driving the recompiled
binary far enough to render. Addresses are `hl2_xbox.xbe` VAs.

## The static-link factory table

The Xbox build has no DLLs -- every Source "module" is linked into the one
image -- so the port replaces `LoadLibrary`/`GetProcAddress` with two static
registries.

### `sub_00595A30` -- `InterfaceReg::InterfaceReg(name, value)`

384 callers, all in the `0x005C7Axx` static-initialiser band. Pushes a
`{ name, value, next }` node onto the list headed at `0x009A9DEC`:

```
mov  edx, [esp+8]          ; value
mov  eax, ecx              ; this (the node, a static)
mov  ecx, [esp+4]          ; name
mov  [eax],   ecx
mov  [eax+4], edx
mov  ecx, [0x9A9DEC]
mov  [eax+8], ecx
mov  [0x9A9DEC], eax
ret  8
```

The nodes are statics, so the list only exists once the C++ constructors have
run -- which makes this registry a direct read-out of whether static init
worked.

### `sub_00595B50` -- `Sys_GetFactory(module_index, symbol)`

The lookup side. Only `"CreateInterface"` (the string at `0x0065DE90`) is a
legal symbol; anything else returns 0 immediately. `module_index` must be in
`1..13`, and indexes an 8-byte-stride table at `0x0081591C` whose first field
is the module's name. It then walks `0x009A9DEC` comparing that name and
returns the matching node's value.

`sub_005A1960` is the one-argument wrapper: it substitutes
`"CreateInterface"` for the symbol and tail-jumps here, so its `ret` is
`sub_00595B50`'s -- a plain `ret`, no argument cleanup.

### The module table at `0x0081591C`

| idx | name | idx | name |
|---|---|---|---|
| 1 | `FileSystem_Stdio` | 8 | `VPhysics` |
| 2 | `MatSys` | 9 | `gameui` |
| 3 | `VguiMatSurface` | 10 | `client` |
| 4 | `VguiDLL` | 11 | `Game` |
| 5 | `ShaderDX8` | 12 | `SoundEmitterSystem` |
| 6 | `StudioRender` | 13 | `datacache` |
| 7 | `Engine` | | |

Index 0 is not a module: the range check is `1 <= idx < 14`.

That list is the shape of the boot -- filesystem first, then materials and
the shader backend, then the engine, then the game DLLs. A module whose
factory does not resolve is a module that never registered, which means its
constructors did not run.

## `sub_005A1700` -- the dispatch that consumes a factory

Reached from `sub_005C0AE0` via `sub_005A17A0`, off the `WinMain` region at
`sub_005C0F0A`. Given a record index it reads a 12-byte record from
`[this+4]`:

```
rec[0] != 0  ->  ecx = rec[0]; call sub_005A1960   ; resolve by module index
rec[0] == 0  ->  eax = rec[1]                      ; use the stored pointer
                 call eax                          ; unconditionally
```

The `call eax` is not guarded, so a factory that fails to resolve is a call
through a null pointer. On hardware that faults; under the recompiler it took
the indirect-call failure path instead, which is how it surfaced.

Note the calling convention: the target takes `ecx`/`edx` only, with nothing
pushed, so it must end in a plain `ret`. See `docs/upstreaming.md` for the
recompiler bug this exposed.

## The content pipeline

The game reads `.xzp`; the disc ships `.xz_`. Those are not the same file
under two names.

`.xz_` is an **`xCmp` container** -- Microsoft XCompress, i.e. LZX:

```
magic  'xCmp'          uncompressed  434,653,523 (414.5 MB)
version 1              window        0x80000 (512 KB)
                       block         0x4000  (16 KB)
```

`zip0_xbox.xz_` is 253 MB on disc, a ratio of 1.72. `default.xbe` carries the
`xCmp` magic and both extensions; `hl2_xbox.xbe` knows only `.xzp`. So the
loader decompresses during its copy, and renaming produces a file the game
cannot parse -- which fails late and confusingly, as the engine hunting
`gameinfo.txt`, `valve.rc` and the whole `cfg` tree and eventually parsing a
string as a pointer. `tools/install_hdd.sh` refuses rather than pretending.

### Recompiling the loader

`regen_loader.sh` and `src/loader/` build `default.xbe` as a second target,
so the decompressor the console used does the work instead of a
reimplementation of LZX. It is small enough to make that cheap: **1,430
functions and 161 K lines of C, generated in under three seconds**, against
the game's 48,335 and 14.9 M.

What works: it loads, runs its CRT, brings up D3D, reads
`LoaderMedia/install.txt`, resolves `Z:` to `saves/Cache`, enumerates the
destination `.xzp`/`.mrk` paths, and reads all six source archives' 24-byte
`xCmp` headers. Every step of the install *scan* is correct.

Note the marker: the manifest's first line is labelled "Must Be First, Change
target to force a recopy" and maps `install.txt` to `Z:\version_235.txt`. The
loader compares them and skips the whole install when they match, so writing
that file by hand makes a later run decide there is nothing to do. It is the
loader's to write, once it has actually copied.

### Where it stops

The attract loop drives the install through the object's own vtable:

```
mov eax,[esi]; mov ecx,esi; call [eax+4]     ; sub_00013AE0
mov edx,[esi]; mov ecx,esi; call [edx+8]     ; sub_00013F70
```

with the vtable the constructor installs at `0x0006F928`:
`{ 0x00014060, 0x00013AE0, 0x00013F70, 0x00014DC0 }`. `sub_00014060` is slot
0 -- the install *step*, called by the other two, not instead of them.

At runtime `this` is `0x00F7DC90` and its vtable reads **0**. The memory
around that pointer is return addresses (`0x0001443A`, `0x0001444B` -- the
call sites in `sub_00014420`), so the pointer is inside the frame rather than
at the 0x22AC object the frame is supposed to hold. The prime suspect is the
large-allocation path of `_chkstk` (`sub_0001D520`, taken for sizes >= 0x1000,
which 0x22AC is): it must move `esp` down by the requested amount, and the
object clearly is not where the code expects it.

Driving the install without the loop does not avoid this -- the same wrong
object is passed -- and calling the constructor directly is worse, because it
skips the loader's CRT and faults immediately.

That frame-arithmetic bug turned out to be a disassembler one, now fixed
upstream: the linear sweep drifts through XPP's zero padding and decodes
001C950600558D at 0x00069533, swallowing the `push ebp` at 0x00069538 that
sub_0006A204 tail-jumps to. The seed for it was rejected as
"mid-instruction", the target stayed stubbed, and a stub returns without the
callee's `ret 8` -- so esp walked off by 4 per call and the object pointer
slid 8 bytes out from under its own vtable. Accepting a mid-instruction seed
when it decodes as a prologue fixes it, and Half-Life 2 is unchanged by the
rule (48,335 functions either way).

With that and the loader's own GPU fence registered (its device global is
0x00034048, the equivalent of the game's 0x0061EDE8), the loader runs its
real attract loop and reaches video playback -- which never completes. That
is where the loader route stands: everything up to and including the install
scan works, and XMV is the remaining blocker. Removing the videos does not
help; the stall persists with the files absent, so it is the subsystem rather
than the file, and Title_Load.xmv is itself a copy target.

## The xCmp container

Reversed from the file, then confirmed against the loader's own walker. The
spec below is exact: it accounts for every byte of zip0_xbox.xz_ (253 MB on
disc) and produces the 434,653,523 bytes the header claims, in 26,530 blocks.
`tools/xcmp.py` walks and checks it.

| | |
|---|---|
| header | 24 bytes: `'xCmp'`, version 1, uncompressed size, window 0x80000, field4 0x00390080, block 0x4000 |
| record | `{ uint16 length; payload[length] }`, next at `+2+length` |
| chunk | one 0x80000 window, zero-padded at the end |

24 bytes is not a guess: it is what default.xbe's own header check reads
(`sub_00011F10` reads 0x18 and compares the magic and version).

The length field is a discriminator as much as a size, which is the part that
resisted a read of the file alone:

| length | meaning |
|---|---|
| `0x0000` | padding. The rest of this chunk is zero; the next record is at the next window boundary. |
| bit `0x8000` set | a **stored** block. The low 15 bits are its size, so the record is `2 + size` and the payload is already the output. |
| anything else | a **compressed** block of `length` bytes, opening with a six-byte sub-header `{ uint16 0x434A, uint32 output size }`, then LZX. |

Reading the file bottom-up nearly got there and then stalled, because the
stored records all carry `0xC000` and taking that as a length walks 4 bytes
off per block -- after which the framing never recovers. `sub_00011000`, which
is the walker, settles it in nine lines:

```
len = *(uint16 *)src;
if (len == 0)      break;                                 end of chunk
if (len & 0x8000)  memcpy(dst, src + 2, len & 0x7fff);     stored
else               dst += sub_0001CE74(src + 2, dst);      compressed
```

so `0xC000` is `0x8000 | 0x4000`: a stored block of one 16 KB block. In
zip0_xbox.xz_, 25,894 blocks are compressed and 636 stored.

Chunks are independently decodable -- LZX back-references never cross a
window -- so the whole 414 MB never has to be resident. The largest chunk
produces 3,735,552 bytes.

### Decompressing with the console's own decoder

No LZX implementation was written. `sub_0001CE74` is the block decoder, it is
a pure function of `(src, dst)` touching no globals, and it is already
recompiled along with the rest of default.xbe -- so the install runs it:

```
./tools/install_hdd.sh              # or:
./bin/hl2_loader.exe --extract game/GameMedia/zip0_xbox.xz_                                 saves/Cache/hl2/hl2x/zip0_xbox.xzp
```

`--extract` maps the XBE (the decoder reads static tables out of its own
`.rdata`), then feeds `sub_00011000` one chunk at a time with a 512 KB source
buffer and an 8 MB destination. It does not need the loader to boot, which is
what makes this route work while the attract loop is still stalled on XMV.

The output begins with `piZx` and ends with the `xZfT` footer, at exactly the
length the header states.

**That is not sufficient verification, and believing it was cost a day.** The
first extraction matched magic, footer and length exactly, listed 19,842
plausible filenames, and was still wrong: 165,448 bytes in it were zeros where
real bytes belonged, spread over 352 of the 26,530 blocks. Length is preserved
by the bug, so every cheap check passed.

What catches it is that the archive stores many files **twice** -- once in the
preload block and once as a file -- so the two copies can be compared against
each other with no external reference:

```
compared 390 duplicated files: 0 mismatching, 0 differing bytes
```

Before the fix that read 383 mismatching of 415. `tools/xcmp.py --self-check`
covers the framing; this pair check is what covers the payload, and it is the
one worth running after any change to the decoder.

The cause was not in the container or the decoder at all. Forward `rep movs`
was being lowered to `memcpy`, and the LZ run -- a match of distance 1 and
length N, repeating one byte N times -- is precisely an overlapping forward
copy whose destination reads what it has already written. `memcpy` is
undefined there. Fixed upstream in `0283b5a`; see [upstreaming.md](upstreaming.md).

**This took a toolkit fix to work at all.** `sub_0001CE74` is a bit reader:
`add edx, edx` shifts the top bit into the carry and `jae` tests it. Carry
conditions were only lowered when the flags came from a `cmp` -- after
arithmetic they fell back to a `_flags` variable nothing ever assigns, so the
branch was permanently false and the decoder read a garbage pointer on its
first block. Three related gaps, all fixed upstream:

- `jb`/`jae` after `add`/`sub`/`adc`/`sbb`/shifts now read `_cf`, which the
  lifter already computed beside the write.
- That rule runs *before* the per-mnemonic reconstructions, which compute CF
  from the operands after the write and are therefore wrong whenever the
  destination is also the source -- `add edx, edx` became `edx < edx`.
- At a block boundary the flag tracking resets, since the predecessor is not
  known. `_cf` survives that: it is a real variable, so the fallback reads it
  rather than `_flags`.

`_cf` is still only declared where something consumes it; the translator now
recognises a carry-consuming branch as a consumer, not just `adc`/`sbb`.

## What the engine renders

It reaches its own main loop. `CModAppSystemGroup::Main` is `sub_0040ED00`:

```
while (engine->GetQuitting() == 0)   ; [vtable+0x34], sub_0040F490
    engine->Frame();                  ; [vtable+0x14], sub_0040F4E0
```

on the static `CEngine` at 0x008095E0 (RTTI-confirmed, base `IEngine`). Most
iterations return early because the frame limiter says "not yet"; roughly
1,650 in 100 seconds do real work, which is a sane rate rather than a spin.

Each of those frames clears and flips. The command stream is real -- 1,457
segments, 25,608 words, 295 distinct methods, none unrecognised by the
scanner -- and includes `CLEAR_SURFACE`, `SET_COLOR_CLEAR_VALUE`,
`SET_TRANSFORM_PROGRAM`, and depth/blend/cull state.

**It issues no draw calls.** No `NV097_SET_BEGIN_END` (0x17FC) appears
anywhere in the stream, no vertex data methods, and the executor counts
`draws 0, 0 indices`. The engine is clearing to opaque black (0xFF000000) and
presenting empty frames, because it has no materials to draw with.

That the clear reaches the screen is not an assumption: `RECOMP_RASTER_TEST`
draws one known triangle after each clear, and it lands in the window at
exactly its own area -- 75,264 of 307,200 pixels for
(320,72) (544,408) (96,408). Executor, surface addressing, framebuffer window:
all working.

### Addressing, which is what made this hard to see

`NV097_SET_SURFACE_COLOR_OFFSET` and `AvSetDisplayMode` both report *physical*
addresses. Guest VA and physical are the same number in this runtime, so
reading them as VAs works until it does not: Half-Life 2's framebuffer is at
physical 0x84000, which as a VA is inside the loaded image. The executor
refused to write there (correctly), and the framebuffer window was displaying
the game's own code as pixels. Both now resolve through XBOX_CONTIG_BASE,
where MmAllocateContiguousMemory actually put the buffer.

### On fabricated config files

Empty stand-ins for `cfg/*.cfg` are worse than absent ones: the engine opens
one, takes a garbage size from it, and issues a 16 MB read that ends in
STATUS_END_OF_FILE. They also do not help -- the engine's behaviour is
identical without them. Removed.

### The content path, end to end

With the archives installed the engine's own search machinery came into view,
and it turned out the last blocker was not content at all.

**How the game names its content.** Three roots are registered, and the choice
between the last two is a runtime flag:

```
mov  ecx, 0x76fac0   ; "R:/HL2/"     always
mov  al,  [0x9aa324]
test al, al
mov  ecx, 0x76fab8   ; "T:/HL2/"     flag == 0
je   .done
mov  ecx, 0x76fab0   ; "Z:/HL2/"     flag != 0
```

`R:` is not a drive. Nothing in either XBE links `\\??\\R:`, and none is needed:
`R:/HL2/` is a prefix HL2 registers with its own file layer, and
`sub_00596B70` classifies a path by matching it against the registered
prefixes before rewriting it onto the real root. So `r:\\hl2\\hl2x\\zip0_xbox.xzp`
becomes `Z:\\HL2\\hl2x\\zip0_xbox.xzp`, which is exactly where
`install.txt` puts it.

The flag is set by a 64 MB memory check -- a retail console rather than a
devkit -- or by `-retail` on the command line, alongside `-dev` and
`-novxconsole`. `RECOMP_CMDLINE="-retail"` pins it, which is worth doing rather
than depending on what this runtime reports for memory size.

**Why it still found nothing.** The pack scan ran, built the right names, and
rewrote them to the right root, yet no `.xzp` ever reached the file layer.
`sub_0041E650` probes each candidate with the CRT's `stat`, and `stat` rejects
a path containing a wildcard before it opens anything:

```
push 0x7714ac     ; "?*"
push esi          ; the path
call strpbrk
test eax, eax
jne  .enoent
```

`Z:\\HL2\\hl2x\\zip0_xbox.xzp` has no wildcard, and it was rejected anyway.
MSVC's `strpbrk` is a 256-bit character map on the stack:

```
push 0 x8                  ; eight zero dwords
bts  dword ptr [esp], eax  ; per character of the set
bt   dword ptr [esp], eax  ; per character of the string
jae  next
```

A memory bit base is a **bit string**: the operand addresses the byte holding
bit 0 and the offset runs over the whole string, so the hardware takes the
dword at `base + (offset/32)*4` and bit `offset%32`. The lifter masked the
offset to 31 -- correct for a register bit base, where the offset really is
modulo the operand size -- which folded all eight dwords onto the first. The
map then aliased mod 32, and `'?'` (0x3F) set the very bit `'_'` (0x5F) tests.

So every path with an underscore was "contains a wildcard". The archives are
`zip0_xbox.xzp` and `zip0_xbox_english.xzp`. Paths without one -- the engine's
`materials\\debug\\debugmrmwireframe.vmt` and friends -- opened normally, which
is why this read as a content problem for so long rather than a string one.

Fixed upstream in `5633c13`, both the standalone lift and the fused `bt`+`jcc`,
since the testing loop is `bt [esp], eax` fused with the `jae` after it. An
immediate offset really is limited to 0..31 of the addressed dword and keeps
the simple form.

Two smaller gaps came out of the same investigation, in `195113e`:
`FscGetCacheSize`/`FscSetCacheSize` had no bridge, so a title that saves the
cache size and restores it was restoring zero; and the missing-bridge warning
could not tell a genuine zero-argument function from an ordinal nobody had
written down, so it accused `FscGetCacheSize` of corrupting the stack when it
was fine. That false alarm cost an hour of this investigation, which is reason
enough to fix it.

### What is left

The archives are installed and the pack probe now resolves to a real file:

```
saves/Cache/hl2/hl2x/zip0_xbox.xzp          434,653,523
saves/Cache/hl2/hl2x/zip0_xbox_english.xzp  186,195,539
```

Maps are not in them: 90 `VBSP` files sit uncompressed on the disc and are read
from `D:` directly, as the console does. There are no `background0N.bsp` -- the
Xbox port's menu is 2D VGUI rather than a map behind a menu as on PC -- so the
first picture does not wait on a level load.
