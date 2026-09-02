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

Reversed from the file, with `tools/xcmp.py` to walk and check it. Verified
against zip0_xbox.xz_ (253 MB on disc, 434,653,523 bytes out):

| | |
|---|---|
| header | 24 bytes: `'xCmp'`, version 1, uncompressed size, window 0x80000, field4 0x00390080, block 0x4000 |
| record | `{ uint16 length; payload[length] }`, next at `+2+length` |
| chunk | records pack until the next would cross a 512 KB boundary, then zero padding up to it; a length of 0 means padding |

24 bytes is not a guess: it is what default.xbe's own header check reads
(`sub_00011F10` reads 0x18 and compares the magic and version). The chunking
explains the 512 KB window -- each chunk is independently decodable -- and
every resume point in the file is exactly 0x80000-aligned. 483 chunks.

One thing is not yet pinned down. Most payloads open with `'JC'` (0x434A) and
a uint32 output size of 0x4000, which reads as a per-block sub-header; 12,241
do, 4,354 do not, and among the latter are 47 records of exactly 0xC000
bytes. Summing only the `'JC'` records lands 14,289 blocks short of the
header's total, so how the rest declare their output size is still open.

After that comes the payload itself, which is LZX: Huffman main/length/
aligned trees, delta-coded lengths through a pretree, position slots for a
512 KB window, and E8 translation. The header states the total output and the
result has to begin with the XZP magic, so any implementation is checkable
against two independent facts.
