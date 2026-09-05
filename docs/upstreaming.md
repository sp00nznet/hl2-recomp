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
| xCompress (`.xz_`) unwrapping | `hl2/` -- **and nothing to upstream** | Solved without writing a decoder: the container framing is in `tools/xcmp.py`, and the LZX itself is `sub_0001CE74` out of the disc's own `default.xbe`, recompiled and called per block. That is the general lesson, not general code -- when the decompressor ships on the disc, recompiling it beats reimplementing it. See [boot.md](boot.md). |
| `bt`/`bts` on memory as a bit string | **fixed upstream**, `5633c13` | The offset was masked to 31, which is right for a register bit base and folds a 256-bit map onto its first dword for a memory one. MSVC's `strpbrk`/`strspn`/`strcspn` are built on exactly that map, so `'?'` and `'_'` aliased and every path with an underscore looked like it held a wildcard. Nothing HL2-specific: it is the CRT. |
| Forward `rep movs` as `memcpy` | **fixed upstream**, `0283b5a` | The hardware copies one element at a time, so an overlapping forward copy propagates -- which is how every LZ decompressor emits a run. `memcpy` is undefined there and a vectorised one reads ahead. Output kept its exact length and lost 165,448 bytes of content. Nothing target-specific: it is the instruction. |
| Carry flag on branches after arithmetic | **fixed upstream**, `2bfb9e0` | `jb`/`jae` only read real flags after a `cmp`; after arithmetic they fell back to a `_flags` nobody assigns, so the branch was always false. MSVC bit readers (`add reg,reg` then `jae`) are built on it -- including the XCompress decoder above. |
| `FscGetCacheSize` / `FscSetCacheSize` | **fixed upstream**, `195113e` | Unbridged, so a title that saves the filesystem cache size and restores it restored zero. Same commit stops the missing-bridge warning calling a genuine zero-argument function a stack corruption. |
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

## Case study: the indirect-call `esp` recovery

`RECOMP_ICALL_SAFE` restores `g_esp` to a saved value when a target fails to
resolve, so a failed stdcall does not leak its arguments. That saved value is
chosen by `_fixup_icall_esp_save` in `tools/recomp/translator.py`, which walked
backwards from the call absorbing every `PUSH32` it met and stepping over
interleaved computation.

At `0x005A1731` HL2 does this:

```
push ebx                  ; callee-saved register save
mov  ebx, [esp+0x14]
push esi                  ; callee-saved register save
lea  edx, [esp+0x14]
mov  ecx, ebx
call eax                  ; arguments in ecx/edx -- nothing on the stack
```

Both pushes are `sub_005A1700`'s own register saves, and the call takes no
stack arguments at all. The scan absorbed both, so the failure path rewound
`g_esp` over them, and the epilogue's `pop esi; pop ebx; pop edi` then read 8
bytes too high -- `edi` landed on the return address and a float landed in the
caller's `esi`. Three frames up, a `CUtlRBTree` method received a `this` of
`0x41976D79`.

The first rule I tried -- "a push of a callee-saved register the function also
pops is a save" -- fixed this and immediately broke its mirror image at
`0x00135265`:

```
push edi                  ; prologue save
...
push edi                  ; argument to the virtual call
mov  ecx, esi
call [eax+0x68]
pop  edi                  ; epilogue restore
```

Here the pushed `edi` really is an argument, and the function pops `edi` too,
so the rule stopped the run early, left the argument on the stack and shifted
the epilogue the other way -- `esi` and `edi` came back swapped.

What separates the two is the **push and pop counts**. A register popped at
least as often as it is pushed is restored on every path out, so a push of it
is a save and the run ends there. A register pushed more often than popped has
a push nobody restores -- an argument -- and the run absorbs it;
`sub_00135265` pushes `edi` twice and pops it once.

The comparison is `pops >= pushes`, not equality, and the difference is not
cosmetic: `sub_005A1700` has two epilogues, so it pops `ebx` twice against a
single push. Written as equality the rule silently reverted that function to
the original broken placement, and the regression test only caught it after it
was rewritten with a second return path -- a reminder that a test for a
frame-shape rule has to model more than one way out of the function.

Where even that is ambiguous, stopping early is the safer error, an asymmetry
worth stating because it decides the default whenever the cases cannot be told
apart:

- Stopping too early **under-rewinds**: the failed call leaks stack, `esp`
  comes back low, and the existing `esp >= entry + 4` invariant reports it.
- Stopping too late **over-rewinds**: the caller's saved registers are
  silently wrong, and the damage shows up somewhere else entirely.

So when in doubt, absorb less. This is general -- it is a fact about x86
frames, not about HL2 -- so it belongs in the toolkit, and three regression
tests in `tools/recomp/test_call_retaddr.py` pin all three shapes: the save,
the argument, and the register that is both.

It is still a heuristic. A function that pushes a register twice and pops it
twice, one pair being an argument, will be read as two saves and under-rewind
-- deliberately the detectable direction.

### What made it findable

The ABI check's `esp` invariant is deliberately one-sided, since a callee may
legitimately pop its own arguments; it therefore cannot see a frame that comes
back *too high*. Printing the signed `esp` delta alongside the last few
indirect-call targets is what turned this from a wrong register into a
one-line diagnosis: the newest target was `00000000`, which named the failure
path directly.

## Known issue: alias entries inherit the enclosing function's end

A mid-function entry point (a vtable slot or a jumped-to label inside another
function) is recorded as an *alias*: same end address as the function that
contains it, so the generated body covers everything reachable from the alias
start. That is right in principle -- flow from a mid-function entry really does
continue into the rest of the function -- but the end is far too coarse
wherever the enclosing "function" is itself a mis-detected blob.

Measured on `hl2_xbox.xbe`:

| | |
|---|---|
| alias functions | 5,741 of 48,334 (12%) |
| bytes covered by aliases | 52.2 MB of 58.1 MB claimed (90%) |
| median alias size | 5,206 bytes |
| p90 alias size | 27,289 bytes |
| largest alias | 43,195 bytes |
| aliases sharing end `0x005DDA70` | 1,770 |

`.text` is 6 MB, so 58 MB of "claimed" bytes is itself the overlap talking.
The static-initialiser region is the worst case: thousands of tiny functions,
each `push; push; mov ecx; call; ret`, get detected as one giant span with
thousands of entry points, and every one of them then carries a body reaching
to the far end of the blob.

Two costs, one of them subtle:

- **Codegen.** 14.9 M lines of C, most of it the same instructions emitted
  once per alias, and a full build that takes tens of minutes.
- **Attribution.** `called_by` counts every call inside the shared span for
  every alias covering it. `sub_00595A30` (`InterfaceReg::InterfaceReg`)
  reports 384 callers; it has **15**. That sent me looking for 369 static
  initialisers that had never failed to run, on a boot path where the real
  answer was elsewhere. A wrong xref is worse than a missing one.

The fix is to end an alias at the first terminator actually reachable from its
own start -- `engine.probes_as_returning_body()` already does that walk for the
gap-target passes -- instead of inheriting the enclosing end. `sub_005C7A8F`
would then be 58 bytes (it ends in `ret` at `0x005C7AC8`) rather than 13,798.

Not done yet: it is a change to how every alias body is bounded, and worth
making when it can be measured on its own rather than in the middle of a boot
investigation.

## Known issue: flags that die at a block boundary

Fixed for the carry flag, still open for the rest. The lifter tracks which
instruction last set the flags so a `jcc` can be lowered from real values, but
that tracking resets at a label -- correctly, because which predecessor
arrives is not known there. The fallback is a `_flags` variable that nothing
ever assigns, so the branch is unconditionally false, and it compiles clean.

The carry case was a boot blocker and is fixed (xboxrecomp `2bfb9e0`): `_cf`
is a real variable, so it survives the boundary and `jb`/`jae` now read it.
Measured over the 48,324 translated functions in `hl2_xbox.xbe` afterwards:

| condition | count | why it is still stuck |
|---|---|---|
| `jo` | 64 | no overflow flag is modelled at all |
| `je` / `jne` | 92 | needs ZF, which has no persistent variable |
| `jbe` / `ja` | 43 | needs CF *and* ZF |
| `jb` / `jae` | 10 | producer is `comiss` or `fcompi`, not integer arithmetic |
| signed / parity | 20 | needs SF, OF, PF |

229 branches, against 48,324 functions -- rare, but each one is silently
always-false rather than merely imprecise.

The shape of the fix is already visible in the carry case. The other
producers also write persistent state: a `cmp` records `_fa`/`_fb`, and
`comiss`/`fcompi` write `g_fp_cmp`. Both outlive the boundary exactly as `_cf`
does. What is missing is knowing *which* producer the incoming edge used, so
the fallback picks the right one. That is a predecessor walk: if every
predecessor of a block ends in the same kind of flag setter -- which is the
common case, a compare and its branch split only by a label -- the fallback is
determined and can be lowered as if fused.

Not attempted yet because it wants the real CFG rather than the linear pass
the lifter makes today, and because none of the remaining 229 is known to be
on a path that matters. The carry ones were: they were the whole XCompress
decoder.
