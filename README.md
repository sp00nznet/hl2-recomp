# hl2-recomp

Static recompilation of **Half-Life 2 (Xbox, 2005)** to a native Windows
executable, using [xboxrecomp](https://github.com/sp00nznet/xboxrecomp).

No emulation. The x86 in `hl2_xbox.xbe` is translated to C, compiled with MSVC,
and linked against the xboxrecomp runtime (kernel shim, D3D8 to D3D11, DirectSound,
input).

## Why this target

Every other Xbox recomp target is a stripped binary. HL2 is not — this build
ships with MSVC RTTI left on and Source's name-baking macros intact:

- **2,336** RTTI classes and **2,932** vtables, giving **12,288 unique virtual
  method addresses** with class names and an exact inheritance graph
- **2,743** `typedescription_t` entries, each with a member **name, type and
  byte offset**; 253 `datamap_t` tables recovered *with their C++ class name*
- **1,201 of 1,435** class names (84%) are declared in the **publicly released**
  [Source SDK 2013](https://github.com/ValveSoftware/source-sdk-2013), with an
  exact file to read. The misses are engine internals the SDK omits.
- Feeding RTTI back into the disassembler found **7,992 functions the linear
  sweep missed** — 33,140 to **41,223** function starts (+24%)

So a recovered function can often be traced: address to class name to the real
`.cpp` in the public SDK. Details and caveats in [docs/symbols.md](docs/symbols.md).

## Status

It boots, runs the engine's own main loop, and streams its own content out of
the disc archives. It does not yet put the game's picture on screen.

| Step | State |
|---|---|
| XBE parsed | done — entry `0x0059C612`, 10 sections, 124 kernel imports |
| RTTI recovery | done — 2,336 classes / 2,932 vtables / 12,288 methods |
| Disassembled | done — **49,172** functions, 90.7% of instructions reachable |
| Datamap recovery | done — 253 classes / 1,722 fields |
| SDK cross-reference | done — 84% of classes located in Source SDK 2013 |
| func_id / codegen | done — 49,161 of 49,172 translated, ~14.9 M lines of C |
| Static initialisation | done — all 5,305 constructors run, none faulting |
| Module factories | done — all 13 resolve; material system comes up |
| Display | done — D3D device created, `AvSetDisplayMode` accepted, clears and flips |
| Disc archives | done — decompressed and **byte-verified** (see below) |
| Content load | partial — packs mount, materials and scripts stream out of them |
| Menu / first frame | not yet — currently faults in the VGUI path |

### What actually runs

The engine reaches `CModAppSystemGroup::Main`, brings up its material system,
creates a D3D device, sets a display mode and drives its own frame loop. Give
it `-retail` and it selects the `Z:/HL2/` content root, mounts
`zip0_xbox.xzp`, reads the 19,767-entry directory and pulls materials, sound
scripts and resource files out of it. What it has not done yet is draw its own
geometry — the remaining work is in VGUI and the menu.

Command-line arguments reach the title the way the console's launcher passes
them, through the launch-data page:

```bash
RECOMP_CMDLINE="-retail" ./bin/hl2.exe
```

### The disc archives

The disc ships `.xz_`, the game reads `.xzp`, and `default.xbe` converts one to
the other. Rather than reimplement LZX, **the loader is recompiled too and its
own decompressor is called** — the console's code, doing the console's job:

```bash
./tools/install_hdd.sh
```

Verification matters more than it looks. An extraction can have the right
magic, the right footer, the exact declared length and 19,842 plausible
filenames and still be wrong — the first one had 165,448 zeroed bytes in it.
XZP stores many files twice, so the archive checks against itself with no
reference needed, and `install_hdd.sh` refuses to leave a bad extraction in
place.

### Fixes this target pushed upstream

The point of a hard target is what it finds. Each of these was a silent
miscompile in [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) affecting
any title, found here because HL2 exercised it:

- **Forward `rep movs` lowered to `memcpy`** — the hardware copies one element
  at a time, so an overlapping forward copy propagates, which is how every LZ
  decoder emits a run. Output kept its exact length and lost 165,448 bytes.
- **`bt`/`bts` on memory masked the bit offset** — with a memory bit base the
  offset indexes a bit *string*. Masking it folds MSVC's 256-bit `strpbrk` map
  onto one dword, so `'?'` and `'_'` alias and every path with an underscore
  looked like it held a wildcard.
- **Carry flag dead after arithmetic** — `jb`/`jae` only read real flags after
  a `cmp`; after arithmetic they fell back to a variable nothing assigns, so
  the branch was always false. MSVC's bit readers are built on that shape.
- **Tail calls did not end a function** — `ret` before int3 padding started a
  new function, a `jmp` did not, so a real function was swallowed and indirect
  calls into it were skipped rather than made. Found 837 more functions.
- **Frames built by `__SEH_prolog`** were classified frameless, so they never
  re-published their frame and `__finally` funclets ran against a dead one.
- Plus `FscGetCacheSize`/`FscSetCacheSize`, `RtlCompareMemory`, the arity
  of `NtWaitForMultipleObjectsEx`, `rdtsc`, and the AV pack encoding.

## Setup

You need your own copy of the game. Nothing redistributable is in this repo.

```bash
# 1. extract the disc next to this README
7z x 'Half-Life 2 [!].7z' -ogame && mv 'game/Half-Life 2'/* game/

# 2. reference source (optional but recommended, ~610 MB, gitignored)
git clone --depth 1 https://github.com/ValveSoftware/source-sdk-2013 ref/source-sdk-2013

# 3. run the pipeline
./regen.sh --disasm

# 4. sanity-check the symbol recovery
py -3 tools/rtti.py --self-check
py -3 tools/datamaps.py --self-check
py -3 tools/xcmp.py --self-check
py -3 tools/xzp_verify.py --self-check

# 5. build, install the archives, run
cmake -S . -B build-msvc && cmake --build build-msvc --config Release
./tools/install_hdd.sh
RECOMP_CMDLINE="-retail" ./bin/hl2.exe
```

Requires Python 3.10+ with `capstone`, Visual Studio 2022, CMake 3.20+, and an
`xboxrecomp` checkout at `../xboxrecomp`.

## Layout

```
config/   runtime-discovered function seeds (committed: cannot be re-derived
          without running the game)
docs/     findings
game/     your extracted disc                    (gitignored, all of it)
build/    everything derived from the XBE        (gitignored)
ref/      reference source checkouts             (gitignored)
src/      entry point + generated C              (gen/ gitignored)
tools/    HL2-specific analysis
```

**No game data is committed.** Not the XBE, not the assets, and not anything
extracted from them — the section table, the 12,288 RTTI-derived function
addresses and the 11,520 recovered class names all live in `build/` and are
regenerated by `regen.sh` in seconds. The one exception is
`config/seed_functions.json`, which holds addresses a *run* discovered and that
no static pass can find again.

## License

MIT, see [LICENSE](LICENSE). Applies to the code in this repository only.
Half-Life 2 and the Source engine are Valve's; you must provide your own copy
of the game.

See [CLAUDE.md](CLAUDE.md) for the full XBE analysis, disc layout, and known risks.
