# hl2-recomp

Static recompilation of **Half-Life 2 (Xbox, 2005)** to a native Windows
executable, using [xboxrecomp](https://github.com/sp00nznet/xboxrecomp).

No emulation. The x86 in `hl2_xbox.xbe` is translated to C, compiled with MSVC,
and linked against the xboxrecomp runtime (kernel shim, D3D8 to D3D11, DirectSound,
input).

## Why this target

Every other Xbox recomp target is a stripped binary. HL2 is not:

- **2,743** `typedescription_t` entries in the retail XBE, each with a member
  **name, type and byte offset**
- **253** `datamap_t` tables recovered *with their C++ class name*, so far
- **1,185** class-name strings, **192** `DT_*` network tables, **613** entity
  classnames and ConVars
- Valve's build tree paths (`U:\xbox\main\src\tier1\mempool.cpp`) confirm the
  layout matches the **publicly released** [Source SDK 2013](https://github.com/ValveSoftware/source-sdk-2013)

So a recovered function can often be traced: address to class name to the real
`.cpp` in the public SDK. Details and caveats in [docs/symbols.md](docs/symbols.md).

## Status

Analysis phase. Nothing runs yet.

| Step | State |
|---|---|
| XBE parsed | done — entry `0x0059C612`, 10 sections, 124 kernel imports |
| Disassembled | done — 33,140 functions, 2,047,949 instructions |
| Datamap recovery | done — `tools/datamaps.py`, 253 classes / 1,722 fields |
| func_id / codegen | not started |
| Runtime bring-up | not started |

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
py -3 tools/datamaps.py --self-check
```

Requires Python 3.10+ with `capstone`, Visual Studio 2022, CMake 3.20+, and an
`xboxrecomp` checkout at `../xboxrecomp`.

## Layout

```
game/     extracted disc + hl2_analysis.json   (binaries gitignored)
tools/    HL2-specific analysis
build/    pipeline output                       (gitignored)
ref/      reference source checkouts            (gitignored)
src/      entry point + generated C             (gen/ gitignored)
docs/     findings
```

See [CLAUDE.md](CLAUDE.md) for the full XBE analysis, disc layout, and known risks.
