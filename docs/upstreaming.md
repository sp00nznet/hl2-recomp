# What Belongs Here vs. In xboxrecomp

The toolkit is the product. This repo should be thin: a title's *facts* and its
*expected answers*, not its machinery. If a fix here would help the next title,
it is a toolkit bug being worked around locally, and it goes upstream.

The test is one question: **would another Xbox title want this?**

| Stays in `hl2/` | Goes to `xboxrecomp/` |
|---|---|
| Addresses, offsets, entry points | Anything that *finds* addresses |
| Expected values / golden fixtures | The algorithm the fixture checks |
| Valve/Source-format knowledge | Format-agnostic binary analysis |
| `main.c` boot order for this title | Runtime services that boot order calls |
| Which archive holds what | Container/compression handling |

## Ledger

| Thing | Where it lives | Why |
|---|---|---|
| `tools.rtti` (MSVC RTTI → classes, vtables, seeds) | **upstreamed** to `xboxrecomp/tools/rtti/` | Nothing about it is HL2-specific. It happens to pay off hugely here and nowhere else so far, but that is a property of the corpus, not the code. Verified to degrade cleanly on all 7 other titles on hand. |
| `tools/rtti_check.py` | `hl2/` | The *expected answer* (2,336 classes, `CNPC_Alyx` = 533 slots) is a fact about this XBE. Guards the toolkit against silent decode regressions. |
| `tools/datamaps.py` (Source `datamap_t` recovery) | `hl2/` | `typedescription_t` is a Valve structure. A Source title would want it; a generic Xbox title has no such thing. Revisit if a second Source title (the Orange Box Xbox 360 ports) ever lands. |
| `rtti → disasm` ordering | **upstream** — documented in `docs/technical/rtti-recovery.md` | The *reason* (a vtable slot is proof of an entry point) is general. |
| `abi_analysis` in the pipeline | already upstream, was **missing from this repo's `regen.sh`** | A gap in this repo, not the toolkit. Wreckless had it; HL2 did not, and would have lifted 9,307 thiscall functions as cdecl. |
| D3D8LTCG device-context map | upstream, `docs/technical/d3d8ltcg-device-context.md` | Mapped from Burnout 3, applies to HL2 unchanged — same XDK 1.0.5849. The *device address* is per-title and belongs here once found. |
| xCompress (`.xz_`) unwrapping | **should go upstream when written** | It is a Microsoft container, not a Valve one; any Xbox title could ship one. See [rendering.md](rendering.md). |
| NV2A pushbuffer execution | upstream, `src/kernel/nv2a_pb_exec.c` | The shared blocker for every title's first frame. Fix it once. |

## The rule that keeps this honest

When something here stops working and the fix is a special case, **the special
case is the bug**. Wreckless learned this the expensive way: it carried a
private copy of `tools/` and the runtime libraries, byte-identical to a March
snapshot of the toolkit — not a fork, just a frozen version — and it had to be
deleted before the project could move again.

`regen.sh` in each title repo should be a thin ordering of toolkit commands plus
that title's paths. When it starts containing logic, that logic is upstream's.

## Reproducibility

`xboxrecomp` should be able to reproduce any of these repos from the XBE alone.
For HL2 that means `./regen.sh --disasm` regenerates every derived artefact —
`build/rtti.json`, `build/disasm/`, `build/abi/`, `build/datamaps.json` and
`src/game/recomp/gen/` — none of which are committed. The only things this repo
stores are the inputs the toolkit cannot derive: documentation, `main.c`, the
fixture, and the analysis JSON.
