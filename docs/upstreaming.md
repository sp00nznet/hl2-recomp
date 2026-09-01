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
| `func_id` XDK section ranges | **fixed upstream** | Was a table of Burnout 3's VAs with no way to pass the real ones. See below. |
| Generated-C banner title | **fixed upstream** | Every title's generated C said "Burnout 3: Takedown". Now read from the XBE certificate. |
| MSVC embedded switch tables | **fixed upstream** in `tools/disasm/` | `jmp dword ptr [reg*4 + disp]` reads a table MSVC parks inside the function. The sweep decoded it as instructions and came out misaligned, usually losing the epilogue -- so `pop esi / pop edi` was never lifted and *every caller* silently lost those registers. 219 tables in this XBE; 38 functions were truncated, `memcpy` at 262 bytes of its real 664. Nothing about it is HL2-specific: wreckless has 87 tables. |
| `-DRECOMP_ABI_CHECK` | **upstreamed** to `templates/runtime/recomp_types.h` | Three compares per indirect call that name any function returning with `ebx`/`esi`/`edi` changed. Built to find the above by measurement instead of by the six wrong theories it actually took. |
| `recomp_types.h` | **not copied here** | Every other title repo has its own copy; Bloodwake's and Burnout 3's have since diverged from the template by 641 and 983 lines. This repo includes `${XBOXRECOMP_DIR}/templates/runtime` instead, so it cannot drift. |

## Worked example: the Burnout 3 section table

The toolkit grew out of Burnout 3, so some of it was written as if there were
only ever going to be one title. `tools/func_id/config.py` carried Burnout 3's
XDK section addresses, and `func_id` had **no argument for supplying the real
ones**:

```python
XDK_SECTIONS = {
    "D3D":     (0x0034C2E0, 0x0034C2E0 + 83828,  "game_render"),
    "DSOUND":  (0x002F3F40, 0x002F3F40 + 52668,  "game_audio"),
    ...
}
```

All eight of those ranges land inside **HL2's `.text`**. So HL2's first
`func_id` run mislabelled 4,498 functions — 2,697 of them `game_audio`, 884
`game_video`, in a title with no video section at all. And because that pass
runs before vtable scanning, which only classifies functions *not already
labelled*, the wrong labels were also stealing functions from a pass that would
have got them right:

| | before | after |
|---|---|---|
| `xdk_caller` | 4,498 | **730** |
| `vtable_scan` | 13,792 | **15,397** |
| `vtable_ctor` | 1,584 | **2,185** |

The fix was not to add an HL2 section table here. `identify.py` already parsed
the real one for "game-agnostic vtable scanning" — it just did it *after*
propagation. Parsing once up front and passing it through fixed both, and
`config` now holds only a section-name-to-category map, which is the part that
actually generalises.

Checked for regression the cheap way: every previously-hardcoded Burnout 3 range
is reproduced *exactly* by the derived table, so Burnout 3's classification is
unchanged by construction. No re-run needed to know that.

**This is the shape to look for.** Not a crash — a plausible-looking number that
is quietly wrong for every title but one.

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
`build/hl2_analysis.json`, `build/rtti_seeds.json`, `build/rtti_names.json`,
`build/rtti.json`, `build/disasm/`, `build/abi/`, `build/datamaps.json` and
`src/game/recomp/gen/` — none of which are committed.

That is also the rule that keeps game data out of the repo, and the two
requirements turn out to be the same one: **anything derived from the retail
XBE is regenerable, so committing it is both redundant and wrong.** Addresses
and Valve's class names are facts about a binary you must own a copy of; they
belong in `build/`.

The only things stored here are what the toolkit cannot derive: documentation,
`main.c`, the golden fixture, and `config/seed_functions.json` — addresses a
*run* discovered, which no static pass can find again.
