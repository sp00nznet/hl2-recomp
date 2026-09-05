# Half-Life 2 (Xbox) — Static Recompilation

## Project Overview
Static recompilation of the 2005 original-Xbox port of **Half-Life 2** into a
native Windows executable using the [xboxrecomp](https://github.com/sp00nznet/xboxrecomp)
toolkit (`../xboxrecomp`).

Like Halo, this doubles as a driver for the toolkit itself: HL2 is the largest
and most complex Xbox target attempted so far, and gaps it exposes get fixed
**upstream** in `../xboxrecomp`, not patched locally.

## Why HL2 Is The Best Target On The Platform

Every other Xbox recomp target is a stripped binary where every function is
`sub_XXXXXXXX`. Halo was a step up because Bungie left 298 assert `__FILE__`
strings in the beta build. HL2 is a different category entirely, for two
compounding reasons.

### 1. The binary is unusually self-describing

Source's macro-heavy design bakes names into the shipping binary. Measured in
`hl2_xbox.xbe` (retail, 2005-10-14):

| Surface | Count | What it gives us |
|---|---|---|
| **MSVC RTTI classes** (`TypeDescriptor`) | **2,336** | every polymorphic **class name** |
| **vtables** (COL at `vtable[-1]`) | **2,932** | **12,288 unique virtual method addresses** |
| RTTI inheritance chains | 2,082 | exact base-class graph, templates included |
| `typedescription_t` field entries | 2,743 | field **name + type + byte offset** |
| `datamap_t` tables recovered w/ class name | 253 | C++ **class name** to its field table |
| `DT_*` network table names | 192 | SendTable/RecvTable to networked members |
| entity classnames + ConVars | 613 | `LINK_ENTITY_TO_CLASS` / `ConVar` to globals |

Two tools mine this today, both with self-checks:

- **`tools/rtti.py`** — RTTI is left on in this build, so 2,932 vtables and
  12,288 virtual methods come out with class names and a complete inheritance
  graph. 8,992 of those methods appear in exactly one class's vtable and are
  uniquely attributable.
- **`tools/datamaps.py`** — real struct layouts: class name, member name,
  `fieldtype_t`, byte offset.

RTTI also **feeds back into disassembly**: a vtable entry proves a function
entry point, and 7,992 of the 12,288 landed in bytes the linear sweep never
claimed. Seeding them took the function count **33,140 to 41,223 (+24%)**.

See [docs/symbols.md](docs/symbols.md) for the structures and the numbers.

### 2. The source code is public

Valve ships [`ValveSoftware/source-sdk-2013`](https://github.com/ValveSoftware/source-sdk-2013)
publicly, covering `src/game/{client,server}`, `src/public`, `src/tier1`,
`src/mathlib`, `src/vgui2`. **1,201 of the 1,435 non-template class names in the
XBE (84%) are declared in that SDK**, with an exact file to read. The 234 misses
are almost all engine internals the SDK omits (`CBaseClient`, `CBaseServer`,
`CAudioSourceWave`, `CBrushBSPIterator`).

The Xbox port is a branch of **Source 2004**, so 2013 is close-but-not-equal —
near-identical for `tier1`/`mathlib`, drifted for game code. The XBE's own build
paths confirm the tree layout matches:

```
U:\xbox\main\src\launcher\Retail_XBox\launcher.exe     <- XBE debug path
U:\xbox\main\src\studiorender\cstudiorender.cpp
U:\xbox\main\src\tier1\mempool.cpp
U:\xbox\main\src\ivp\havana\havok\hk_math\matrix3.cpp
```

So for a given recovered function we can often go: address -> datamap class name
-> the actual `.cpp` in the SDK -> read what the code is supposed to do. No other
Xbox target has that.

**Clone the reference yourself** (gitignored, ~610 MB):
```bash
git clone --depth 1 https://github.com/ValveSoftware/source-sdk-2013 ref/source-sdk-2013
```

## Game Info
- **Title:** Half-Life 2 (Xbox)
- **Developer:** Valve, Xbox port with Barking Lizards
- **Publisher:** Electronic Arts / VU Games
- **Release:** November 15 2005 (NA)
- **Engine:** Source, Xbox branch of Source 2004
- **Disc version marker:** `version_235.txt` (from `LoaderMedia/install.txt`)

## The Two XBEs

The disc ships a small loader and the actual game. **`hl2_xbox.xbe` is the
recompilation target.**

| File | Size | Build date | Debug path | Role |
|---|---|---|---|---|
| `default.xbe` | 466,944 | 2005-10-14 02:23 | `e:\hl2_xbox_dev\src\utils\xbox\xbox_loader\Retail_XBox\xbox_loader.exe` | Installer/attract shell: copies the `.xz_` archives to HDD, plays the XMV logos |
| **`hl2_xbox.xbe`** | **8,450,048** | **2005-10-14 19:52** | `U:\xbox\main\src\launcher\Retail_XBox\launcher.exe` | **The game. Engine + client + server + physics, all statically linked into one image.** |

Note the loader was built from a *different* tree (`e:\hl2_xbox_dev`) than the
game (`U:\xbox\main`) — the loader is the port house's utility, the game is
Valve's main branch.

## XBE Analysis (hl2_xbox.xbe)

- **Title ID:** 0x45410091, Region NA/JP/RoW, Retail
- **Base Address:** 0x00010000, **Entry Point:** 0x0059C612
- **Image Size:** 0x009A68C0 (9.65 MB)
- **Kernel Thunk Table:** 0x00626160
- **Heap:** reserve 0x8000000 (128 MB) — note the Xbox only has 64 MB
- **XDK:** 1.0.5849 across all libs

### Memory Map
```
0x00011000 - 0x005F4A6C  .text     (6031 KB)  Engine + game + CRT, one blob
0x005F4A80 - 0x005F5EB0  D3DX      (5 KB)
0x005F5EC0 - 0x00600D98  XGRPH     (44 KB)    Xbox graphics helper
0x00600DA0 - 0x0060B0E4  DSOUND    (41 KB)
0x0060B100 - 0x00620004  D3D       (85 KB)    D3D8LTCG
0x00620020 - 0x00626154  XPP       (24 KB)
0x00626160 - 0x007A3504  .rdata    (1524 KB)  Constants + the name strings
0x007A3520 - 0x009AF0DC  .data     (2095 KB)  Static state; datamaps live here
0x009AF0E0 - 0x009B6260  DOLBY     (28 KB)
0x009B6260 - 0x009B68C0  .XTLID    (1 KB)
```

### Linked Libraries
`XAPILIB`, `D3DX8`, `XGRAPHC`, `DSOUND`, `XBOXKRNL`, `LIBCMT`, `LIBCPMT`,
`D3D8LTCG` — all 1.0.5849.

**`D3D8LTCG`** is the link-time-codegen D3D8, same as Burnout 3 — D3D calls are
inlined into game code with non-standard calling conventions rather than clean
`IDirect3DDevice8::` calls. See `../burnout3/docs/d3d8ltcg_device_context.md`.

**`LIBCPMT`** (C++ multithreaded CRT) means real C++: exceptions, RTTI, `std::`
containers, vtables everywhere. The other targets are mostly C.

### Disassembly (`tools.disasm`, seeded with RTTI)
```
total_instructions   2,044,960
total_functions         41,223      (.text 40,383)
  seed_vtable_thunk     12,293      <- from tools/rtti.py
  by call target        16,959
  by cc boundary         6,125
  by prologue            5,596
reachable instructions   87.4%
total_xrefs            455,004
kernel imports             124
```
Without the RTTI seeds this pass finds 33,140. 41k functions is roughly 5x Halo;
expect the recomp step to be the long pole.

### Kernel Imports (124)
Nothing exotic — Av/Ex/Hal/Io/Ke/Mm/Nt/Ps/Rtl/Xc/Xe, the normal set. There is no
XNET section, so **no Xbox Live surface to shim** (unlike Halo). That is a real
simplification.

## Disc Layout

160 files, 2.0 GB extracted. Deliberately simple — the engine streams from a
handful of archives:

```
default.xbe                       loader
hl2_xbox.xbe                      game
GameMedia/zip0_xbox.xz_           main asset archive (XZP, compressed)
GameMedia/zip0_xbox_{english,french,german,italian,spanish}.xz_
GameMedia/maps/*.bsp              ~120 maps, uncompressed on disc
GameMedia/sound/music/*.wav
LoaderMedia/*.xmv                 attract/logo video
LoaderMedia/loader.xpr            loader UI textures
LoaderMedia/install.txt           HDD copy manifest
```

`install.txt` shows the loader copies the `.xz_` files to `Z:\hl2\hl2x\*.xzp` on
the HDD before launching the game — so the runtime's file layer must present
that path, not the disc path.

**XZP** is Valve's Xbox package format. GCFScape 1.4.1 and HLLib can already
read it; so can [`desukuran/lynx-project`](https://github.com/desukuran/lynx-project),
which also injects.

## Architecture
Based on [xboxrecomp](https://github.com/sp00nznet/xboxrecomp).

### Key Directories
- `game/` — extracted disc contents (git-ignored) and `hl2_analysis.json`
- `tools/` — HL2-specific analysis (`rtti.py`, `datamaps.py`, `xcmp.py`,
  `xzp_verify.py`, `install_hdd.sh`)
- `build/` — pipeline output and CMake build tree (git-ignored)
- `ref/` — reference source checkouts, clone yourself (git-ignored)
- `src/game/recomp/gen/` — auto-generated C (git-ignored, rebuild via `regen.sh`)
- `docs/` — findings specific to HL2

### Regenerating
```bash
./regen.sh --disasm     # full pipeline: rtti -> seeded disasm -> recomp
./regen.sh              # skip disasm, re-run datamaps/func_id/recomp
py -3 tools/rtti.py --self-check
py -3 tools/datamaps.py --self-check
py -3 tools/xcmp.py --self-check         # .xz_ container framing
py -3 tools/xzp_verify.py --self-check   # decompressed .xzp integrity
```

### Installing the disc archives
```bash
./tools/install_hdd.sh          # decompress .xz_ -> .xzp, then verify
```
Runs the console's own LZX out of the recompiled `default.xbe` rather than a
reimplementation, then checks the result against itself: XZP stores many files
twice, and the two copies must agree. Magic, footer and length all match even
when the decompression is wrong, so that pair check is the one that counts.

## Prior Art
- [chipsnapper/HL2-Xbox-research](https://github.com/chipsnapper/HL2-Xbox-research) — archived Apr 2023, map/material research
- [VDC: Half-Life 2 (Xbox)](https://developer.valvesoftware.com/wiki/Half-Life_2_(Xbox)) and its Modding Guide — behind an Anubis bot wall, fetch in a browser
- [VDC: AndrewNeo/The HL2 Xbox Project](https://developer.valvesoftware.com/wiki/User:AndrewNeo/The_HL2_Xbox_Project)

## Known Risks
- **Size.** 6 MB `.text` / 33k functions / 2M instructions is 3-4x anything the
  toolkit has digested. Codegen time and MSVC compile time are both unknowns.
- **C++.** `LIBCPMT` means vtables, RTTI and EH. Indirect-call resolution
  matters far more here than on the C targets — though RTTI already resolves
  176,865 vtable slots statically, which is most of the indirect-call surface.
  Plan on the `HL2_ICALL_FEEDBACK` loop for the rest.
- **D3D8LTCG.** Inlined D3D means the D3D translation layer cannot just hook
  clean interface methods. Burnout 3 hit this first; reuse that work.
- **128 MB heap reserve on a 64 MB console.** Worth understanding before wiring
  `xbox_MemoryLayoutInit` — the port may rely on reserve-vs-commit behaviour.
