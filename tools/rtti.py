"""Recover C++ classes, vtables and virtual methods from hl2_xbox.xbe via MSVC RTTI.

The Xbox build links LIBCPMT with RTTI left on, so the retail binary carries a
full MSVC RTTI graph. That gives us, with no guessing:

  * every class name, as a decorated ".?AV...@@" TypeDescriptor
  * every vtable, because MSVC puts the CompleteObjectLocator at vtable[-1]
  * every virtual method address, by walking the vtable
  * the complete inheritance chain, from the ClassHierarchyDescriptor

Per-slot "which ancestor declared this" is deliberately NOT inferred: MSVC's
base-class array is a depth-first preorder, so trailing entries are secondary
inheritance branches rather than the least-derived base, and with multiple
inheritance the question is ambiguous without modelling MSVC's layout. What is
unambiguous, and what this reports, is how many classes' vtables a given method
appears in -- a method in exactly one vtable is uniquely attributable.

    py -3 tools/rtti.py game/hl2_xbox.xbe game/hl2_analysis.json -o build/rtti.json
    py -3 tools/rtti.py --self-check

Structures (32-bit MSVC, all VAs, no image-relative offsets on this target):

    TypeDescriptor            { void *vfptr; void *spare; char name[]; }
    CompleteObjectLocator     { u32 sig; u32 offset; u32 cdOffset;
                                TypeDescriptor *pTD; ClassHierarchyDescriptor *pCD; }
    ClassHierarchyDescriptor  { u32 sig; u32 attributes; u32 numBaseClasses;
                                BaseClassDescriptor **pBaseClassArray; }
    BaseClassDescriptor       { TypeDescriptor *pTD; u32 numContainedBases;
                                PMD where; u32 attributes; }
"""
import argparse, json, re, struct, sys
from collections import Counter
from pathlib import Path

TD_NAME = re.compile(rb"\.\?A[VU][A-Za-z0-9_@?$]{2,250}@@\x00")
CODE_SECTIONS = (".text", "D3D", "D3DX", "XGRPH", "DSOUND", "XPP")


def demangle(name):
    """.?AVCNPC_Alyx@@ -> CNPC_Alyx. Template args are left decorated."""
    if name.startswith((".?AV", ".?AU")) and name.endswith("@@"):
        name = name[4:-2]
    return name.removeprefix("?$")


class Image:
    def __init__(self, xbe: Path, analysis: Path):
        self.d = xbe.read_bytes()
        self.secs = [(int(s["virtual_addr"], 16), int(s["raw_addr"], 16),
                      s["raw_size"], s["name"])
                     for s in json.loads(analysis.read_text())["sections"]]
        self.code = [(v, v + rs) for v, _, rs, n in self.secs if n in CODE_SECTIONS]

    def raw(self, va):
        for v, r, rs, _ in self.secs:
            if v <= va < v + rs:
                return r + (va - v)
        return None

    def u32(self, va):
        r = self.raw(va)
        return struct.unpack_from("<I", self.d, r)[0] if r is not None else None

    def is_code(self, va):
        return any(lo <= va < hi for lo, hi in self.code)


def type_descriptors(img):
    """{typedescriptor_va: decorated_name}. The name field is 8 bytes in."""
    out = {}
    for v, r, rs, _ in img.secs:
        for m in TD_NAME.finditer(img.d[r:r + rs]):
            out[v + m.start() - 8] = m.group()[:-1].decode("latin1")
    return out


def locators(img, td):
    """{col_va: (name, subobject_offset, class_hierarchy_desc_va)}."""
    out = {}
    for v, r, rs, _ in img.secs:
        for off in range(0, rs - 20, 4):
            sig, sub, _cd, ptd, pcd = struct.unpack_from("<5I", img.d, r + off)
            if sig != 0 or ptd not in td or img.raw(pcd) is None:
                continue
            out[v + off] = (td[ptd], sub, pcd)
    return out


def hierarchy(img, td, cols):
    """{decorated_name: [base names, most-derived first]}."""
    out = {}
    for name, _sub, pcd in cols.values():
        if name in out:
            continue
        n, pba = img.u32(pcd + 8), img.u32(pcd + 12)
        if not n or not (0 < n < 200) or pba is None or img.raw(pba) is None:
            continue
        bases = []
        for i in range(n):
            b = img.u32(pba + i * 4)
            if b is None or img.raw(b) is None:
                break
            ptd = img.u32(b)
            if ptd not in td:
                break
            bases.append(td[ptd])
        else:
            out[name] = bases
    return out


def vtables(img, cols):
    """[(vtable_va, name, subobject_offset, [method VAs])] -- one per COL reference."""
    out = []
    for v, r, rs, n in img.secs:
        if n not in (".rdata", ".data"):
            continue
        for off in range(0, rs - 4, 4):
            w = struct.unpack_from("<I", img.d, r + off)[0]
            if w not in cols:
                continue
            start = v + off + 4
            methods = []
            while img.is_code(e := img.u32(start + len(methods) * 4) or 0):
                methods.append(e)
            if methods:
                name, sub, _ = cols[w]
                out.append((start, name, sub, methods))
    return out


def recover(img):
    td = type_descriptors(img)
    cols = locators(img, td)
    hier = hierarchy(img, td, cols)
    vts = vtables(img, cols)
    # The primary vtable is the subobject-offset-0 one; longest wins on ties.
    primary_len, primary_va = {}, {}
    for va, name, sub, methods in vts:
        if sub == 0 and len(methods) > primary_len.get(name, 0):
            primary_len[name], primary_va[name] = len(methods), va
    return {
        "type_descriptors": td, "locators": cols, "hierarchy": hier,
        "vtables": vts, "primary_len": primary_len, "primary_va": primary_va,
    }


def self_check():
    """Assert facts verified by hand against the retail 2005-10-14 XBE."""
    root = Path(__file__).resolve().parent.parent
    img = Image(root / "game/hl2_xbox.xbe", root / "game/hl2_analysis.json")
    r = recover(img)

    assert len(r["type_descriptors"]) >= 2300, len(r["type_descriptors"])
    assert len(r["vtables"]) >= 2900, len(r["vtables"])

    # CNPC_Alyx's chain is textbook Source and cross-checks against the SDK.
    alyx = [demangle(b) for b in r["hierarchy"][".?AVCNPC_Alyx@@"]]
    for expect in ("CNPC_Alyx", "CNPC_PlayerCompanion", "CAI_PlayerAlly",
                   "CAI_BaseNPC", "CBaseCombatCharacter", "CBaseAnimating",
                   "CBaseEntity", "IHandleEntity"):
        assert expect in alyx, expect
    assert alyx.index("CBaseAnimating") < alyx.index("CBaseEntity")

    # CBaseEntity derives only from the three server interfaces.
    assert [demangle(b) for b in r["hierarchy"][".?AVCBaseEntity@@"]] == [
        "CBaseEntity", "IServerEntity", "IServerUnknown", "IHandleEntity"]

    # An NPC vtable is large, and a derived class is never shorter than its base.
    assert r["primary_len"][".?AVCNPC_Alyx@@"] >= 500
    assert (r["primary_len"][".?AVCNPC_Alyx@@"]
            >= r["primary_len"][".?AVCBaseEntity@@"])

    funcs = {m for _, _, _, ms in r["vtables"] for m in ms}
    print(f"ok: {len(r['type_descriptors'])} classes, {len(r['vtables'])} vtables, "
          f"{sum(len(m) for *_, m in r['vtables'])} slots, {len(funcs)} unique methods")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("xbe", nargs="?", type=Path)
    ap.add_argument("analysis", nargs="?", type=Path)
    ap.add_argument("-o", "--out", type=Path, default=Path("build/rtti.json"))
    ap.add_argument("--seeds", type=Path,
                    help="also write virtual-method addresses as a tools.disasm "
                         "--seed-functions file (RTTI proves these are entry points)")
    ap.add_argument("--self-check", action="store_true")
    args = ap.parse_args()

    if args.self_check:
        return self_check()
    if not (args.xbe and args.analysis):
        ap.error("need xbe and analysis json (or --self-check)")

    r = recover(Image(args.xbe, args.analysis))
    classes = {}
    for name, n in r["primary_len"].items():
        classes[demangle(name)] = {
            "vtable": hex(r["primary_va"][name]),
            "vtable_slots": n,
            "bases": [demangle(b) for b in r["hierarchy"].get(name, [])[1:]],
        }
    methods = {}
    for va, name, sub, ms in r["vtables"]:
        for m in ms:
            methods.setdefault(hex(m), set()).add(demangle(name))

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps({
        "classes": classes,
        "methods": {k: sorted(v) for k, v in methods.items()},
    }, indent=1))
    print(f"{len(classes)} classes, {len(methods)} unique virtual methods -> {args.out}")
    named = Counter(len(v) for v in methods.values())
    print(f"  appearing in exactly one class vtable: {named[1]}")

    if args.seeds:
        args.seeds.write_text(json.dumps(sorted(int(k, 16) for k in methods)))
        print(f"  {len(methods)} seed addresses -> {args.seeds}")


if __name__ == "__main__":
    sys.exit(main())
