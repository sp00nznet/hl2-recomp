# Symbol Recovery from `hl2_xbox.xbe`

What the retail Xbox binary hands us for free, and how `tools/datamaps.py` gets
at it. All numbers measured against the 2005-10-14 retail build.

## The short version

Source registers its save/restore field tables with macros that stringify the
member name. Those strings survive into retail, next to the field's **type** and
**byte offset**. That is a struct layout, in the shipping binary, for 253 named
C++ classes so far.

```
$ py -3 tools/datamaps.py --self-check
ok: 253 datamaps, 1722 fields
```

## `typedescription_t` layout in this build

The struct is **36 bytes** here — an earlier, leaner shape than the Source 2013
version (which grew `fieldSizeInBytes`, `override_field`, `override_count`,
`fieldTolerance`). Confirmed by the observed stride: 2,743 pointer-to-`m_*`
occurrences sit exactly 36 bytes apart.

| Offset | Type | Field |
|---|---|---|
| +0  | `int` | `fieldType` (`fieldtype_t`) |
| +4  | `const char *` | `fieldName` |
| +8  | `int` | `fieldOffset` |
| +12 | `unsigned short` | `fieldSize` (array element count; 1 for scalars) |
| +14 | `short` | `flags` (`FTYPEDESC_SAVE` = 2 is the common value) |
| +16 | `const char *` | `externalName` (usually null) |
| +20 | `ISaveRestoreOps *` | `pSaveRestoreOps` |
| +24 | `inputfunc_t` | `inputFunc` |
| +28 | `datamap_t *` | `td` (for `FIELD_EMBEDDED`) |
| +32 | `int` | reserved / trailing |

The initial guess of a two-element `fieldOffset[2]` array is wrong for this
build: the apparent second offset is the constant `0x00020001`, which is
`fieldSize = 1` and `flags = 2` read as one word.

### Worked example

Decoding the table at `0x007AA064` (19 entries, `m_bFadeOut` .. `m_pRagdoll`):

```
0x7aa064 type=BOOLEAN  off=1236  m_bFadeOut
0x7aa088 type=INTEGER  off=1244  m_iCurrentFriction
0x7aa0ac type=INTEGER  off=1248  m_iMinFriction
0x7aa0d0 type=INTEGER  off=1252  m_iMaxFriction
0x7aa0f4 type=FLOAT    off=1256  m_flFrictionModTime
0x7aa118 type=TIME     off=1260  m_flFrictionTime
0x7aa13c type=INTEGER  off=1264  m_iFrictionAnimState
0x7aa160 type=BOOLEAN  off=1268  m_bReleaseRagdoll
0x7aa184 type=INTEGER  off= 664  m_nBody
0x7aa1a8 type=INTEGER  off= 660  m_nSkin
0x7aa1cc type=CHARACTER off=  76  m_nRenderFX
0x7aa1f0 type=CHARACTER off= 108  m_nRenderMode
0x7aa214 type=COLOR32  off=  80  m_clrRender
0x7aa238 type=TIME     off=1240  m_flEffectTime
```

Note the offsets sort into two clusters: the low ones (76, 80, 108) are
`CBaseEntity` members, the ~660 ones are `CBaseAnimating`, the ~1240 ones belong
to the leaf class. The clustering itself is a usable signal for attributing
inherited members to the right base class.

These four are the assertions in `tools/datamaps.py --self-check`, so a change
that silently breaks the decode fails loudly.

## Finding the class name

`datamap_t` looks like this:

```c
struct datamap_t {
    typedescription_t *dataDesc;
    int                dataNumFields;
    const char        *dataClassName;
    datamap_t         *baseMap;
    bool               chained;
};
```

but in this build the `datamap_t` objects live in **BSS** and are filled in at
runtime by an initialiser, so there is nothing to scan for in the file. The
link is only visible in `.text`, as adjacent immediates in that initialiser:

```
mov  dword ptr [0x007a9e14], 0x007aa064      ; dataDesc  <- table
...
mov  dword ptr [0x007a9e1c], <class name VA> ; dataClassName
```

So `recover()` works backwards: index every 4-byte immediate in `.text`
(unaligned — these are mov operands, not aligned words), find the sites that
reference each `C*`/`C_*` class-name string, and scan +/-80 bytes around each
site for an immediate that decodes as the head of a `typedescription_t` run.
Longest run wins.

That naive window finds **253 of ~1,185** candidate class names. The remainder
are the obvious next win — widening the window, following the `baseMap` chain
once one map in a hierarchy is anchored, and using `dataNumFields` (which sits
between the two immediates) to validate rather than guess.

## Other name surfaces, not yet mined

Counted but not yet turned into a tool:

- **192 `DT_*`** network table names. `SendTable`/`RecvTable` entries pair a
  property name with a **proxy function pointer** — that is name-to-*code*
  recovery, not just name-to-data, and complements the datamaps.
- **613 entity classnames and ConVar names.** `LINK_ENTITY_TO_CLASS(npc_zombie,
  CNPC_Zombie)` puts the classname string in a factory record next to the
  factory function, whose body is the constructor, whose first store is the
  vtable — a path from a string to a whole class's virtual methods.
  Each `ConVar` is a static object constructed at init with its name string, so
  the same trick names **globals**.
- **`U:\xbox\main\src\...` paths.** Only 5 survive (in `studiorender`, `tier1`,
  and the Havok/IVP physics code), far fewer than Halo's 298, because Source's
  release build compiles `Assert()` out. Not a major surface here.

## MSVC RTTI: the biggest surface

`LIBCPMT` is linked with RTTI left on, so the retail binary carries a complete
MSVC RTTI graph. This is a much larger win than the datamaps, and it names
*code* rather than data. `tools/rtti.py` walks it:

```
$ py -3 tools/rtti.py --self-check
ok: 2336 classes, 2932 vtables, 176865 slots, 12288 unique methods
```

| Recovered | Count |
|---|---|
| `TypeDescriptor`s (class names, decorated) | 2,336 |
| `CompleteObjectLocator`s | 2,932 |
| vtables (one per COL; extra ones are multiple-inheritance subobjects) | 2,932 |
| virtual method slots | 176,865 |
| **unique function addresses reached** | **12,288** |
| ...appearing in exactly one class's vtable (uniquely attributable) | 8,992 |
| classes with a recovered inheritance chain | 2,082 |

### How it hangs together

MSVC emits, for each polymorphic class, a `CompleteObjectLocator` and places a
pointer to it at **`vtable[-1]`** — the dword immediately before the first
method. So finding vtables is: find the COLs, then find every word pointing at
one, and the vtable starts 4 bytes later. Walk forward while entries are valid
code VAs to get the method list.

```
TypeDescriptor            { void *vfptr; void *spare; char name[]; }
CompleteObjectLocator     { u32 sig; u32 offset; u32 cdOffset;
                            TypeDescriptor *pTD; ClassHierarchyDescriptor *pCD; }
ClassHierarchyDescriptor  { u32 sig; u32 attributes; u32 numBaseClasses;
                            BaseClassDescriptor **pBaseClassArray; }
BaseClassDescriptor       { TypeDescriptor *pTD; u32 numContainedBases;
                            PMD where; u32 attributes; }
```

All pointers are plain VAs — this is 32-bit MSVC, so there is none of the
image-relative-offset indirection x64 RTTI uses.

### The inheritance graph is exact

`CNPC_Alyx`, straight out of the `ClassHierarchyDescriptor`:

```
CNPC_Alyx -> CNPC_PlayerCompanion -> CAI_PlayerAlly -> CAI_BaseActor
  -> CAI_ExpresserHost<CAI_BaseHumanoid> -> CAI_BaseHumanoid
  -> CAI_BlendingHost<CAI_BehaviorHost<CAI_BaseNPC>>
  -> CAI_BehaviorHost<CAI_BaseNPC> -> CAI_BaseNPC -> CBaseCombatCharacter
  -> CBaseFlex -> CBaseAnimatingOverlay -> CBaseAnimating -> CBaseEntity
  -> IServerEntity -> IServerUnknown -> IHandleEntity
  (+ secondary bases: CAI_DefMovementSink, IAI_MovementSink,
     IBehaviorBackBridge, CAI_ExpresserSink)
```

That is textbook Source, template arguments and all, and it matches the SDK.
`CNPC_Alyx`'s primary vtable is **533 slots**.

**Per-slot "which ancestor declared this" is deliberately not inferred.** The
base-class array is a depth-first preorder, so the trailing entries are
secondary-inheritance branches, not the least-derived base — a naive
"last entry owns slot 0" rule gives `CAI_ExpresserSink` instead of
`IHandleEntity`. With multiple inheritance the question needs MSVC layout
modelling to answer honestly. What the tool reports instead is how many classes'
vtables a method appears in; **8,992** appear in exactly one and are therefore
uniquely attributable with no inference at all.

### Client and server in one binary

The Xbox build statically links client and server together, and the compiler
disambiguated the duplicate class names with a `Client_` prefix:
`Client_CAmmoDef` vs `CAmmoDef`, `Client_CActivityDataOps`, and so on. Useful
for free: it tells you which side of the engine a vtable belongs to.

## Feeding RTTI back into the disassembler

A vtable entry is *proof* of a function entry point. Comparing the 12,288
recovered method addresses against the first disassembly pass:

| | Count |
|---|---|
| already a detected function start | 4,286 |
| **fell in unclaimed bytes (missed functions)** | **7,992** |
| landed inside a detected function | 10 |

The 10 are 16-byte-spaced adjustor thunks that the sweep had merged into one
function. The other 7,992 were simply never found.

`tools/rtti.py --seeds` writes them in `tools.disasm --seed-functions` format,
and `regen.sh` runs RTTI *before* disasm for exactly this reason:

```
                    without seeds      with RTTI seeds
total functions            33,140               41,215   (+24%)
reachable instructions          --                87.4%
```

disasm also reported *"Realigned 19 seeded addresses the sweep stepped over"* —
RTTI did not just add functions, it corrected decode boundaries the linear sweep
had got wrong.

## Cross-referencing the public source

Of the 1,435 non-template class names in the binary, **1,201 (84%) are declared
in `ref/source-sdk-2013/src`**, with an exact file to read:

```
CAI_ActBusyBehavior        game\server\hl2i_behavior_actbusy.h
CAI_AllyManager            game\server\hl2i_allymanager.cpp
AR2Explosion               game\server\hl2r2_explosion.h
Beam_t                     game\clienteamdraw.h
```

The 234 misses are almost entirely **engine** classes, exactly where the public
SDK stops: `CBaseClient`, `CBaseServer`, `CAudioSourceWave`, `CAudioMixerWave`,
`CBrushBSPIterator`, `CChangeFrameList`. Game code is public; engine, sound and
BSP internals are not.

Once a function is attributed to a class, `ref/source-sdk-2013` usually has the
matching `.cpp`. Reliability by area:

| Area | Match quality |
|---|---|
| `src/tier1`, `src/mathlib`, `src/tier0` | near-identical, safe to read as-is |
| `src/public` headers, `datamap.h`, `dt_send.h` | structurally identical, field additions aside |
| `src/game/server`, `src/game/client` | same architecture, drifted bodies — 2013 has Episodes/Portal changes HL2 2005 lacks |
| `src/engine`, `src/studiorender`, physics | **not in the public SDK at all** |

So treat the SDK as a *guide to intent*, never as an oracle for exact code. The
Xbox branch is Source 2004 plus the September 2005 CPU/memory optimisations that
were later folded back into the PC build.
