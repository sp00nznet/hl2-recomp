"""Recover Source-engine datamap_t / typedescription_t tables from hl2_xbox.xbe.

Source builds a save/restore field table per entity class via the DEFINE_FIELD
macros. Each entry carries the field's *name*, *type* and *byte offset*, and the
datamap_t header carries the C++ *class name*. Recovering them gives real struct
layouts and member names for the recompiled code -- the reason HL2 is a better
recomp target than a stripped binary.

The datamap_t headers live in BSS and are filled in at runtime, so the class
name <-> table link is only visible as a pair of immediates in the .text
initialiser. We find it by scanning near each class-name reference.

    py -3 tools/datamaps.py game/hl2_xbox.xbe build/hl2_analysis.json -o build/datamaps.json
    py -3 tools/datamaps.py --self-check
"""
import argparse, collections, json, re, struct, sys
from pathlib import Path

ENTRY = 36  # sizeof(typedescription_t) in this build

# fieldtype_t, src/public/datamap.h -- order is stable across Source branches.
FIELD_TYPES = [
    "VOID", "FLOAT", "STRING", "VECTOR", "QUATERNION", "INTEGER", "BOOLEAN",
    "SHORT", "CHARACTER", "COLOR32", "EMBEDDED", "CUSTOM", "CLASSPTR", "EHANDLE",
    "EDICT", "POSITION_VECTOR", "TIME", "TICK", "MODELNAME", "SOUNDNAME",
    "INPUT", "FUNCTION", "VMATRIX", "VMATRIX_WORLDSPACE", "MATRIX3X4_WORLDSPACE",
    "INTERVAL", "MODELINDEX", "MATERIALINDEX", "VECTOR2D",
]
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
CLASSNAME = re.compile(rb"(C_?[A-Za-z][A-Za-z0-9_]{2,60})\x00")


class Image:
    def __init__(self, xbe: Path, analysis: Path):
        self.d = xbe.read_bytes()
        secs = json.loads(analysis.read_text())["sections"]
        # (va, raw, raw_size, name) -- only raw_size is file-backed; BSS is not.
        self.secs = [(int(s["virtual_addr"], 16), int(s["raw_addr"], 16),
                      s["raw_size"], s["name"]) for s in secs]

    def raw(self, va):
        for v, r, rs, _ in self.secs:
            if v <= va < v + rs:
                return r + (va - v)
        return None

    def u32(self, va):
        r = self.raw(va)
        return struct.unpack_from("<I", self.d, r)[0] if r is not None else None

    def cstr(self, va, mx=120):
        r = self.raw(va)
        if r is None:
            return None
        end = self.d.find(b"\x00", r, r + mx)
        if end < 0:
            return None
        s = self.d[r:end]
        s = s.decode("latin1")
        return s if s and s.isprintable() else None

    def section(self, name):
        return next(s for s in self.secs if s[3] == name)


def field(img, va):
    """Decode one typedescription_t; None if it does not look like one."""
    ftype = img.u32(va)
    name = img.cstr(img.u32(va + 4) or 0, 64)
    if ftype is None or name is None or ftype >= len(FIELD_TYPES):
        return None
    if not IDENT.match(name):
        return None
    r = img.raw(va)
    offset, size, flags = struct.unpack_from("<IHh", img.d, r + 8)
    return {"name": name, "type": FIELD_TYPES[ftype], "offset": offset,
            "count": size, "flags": flags}


def table_at(img, va):
    """Walk to the start of the entry run containing va, return (start, fields)."""
    while field(img, va - ENTRY):
        va -= ENTRY
    out = []
    while (f := field(img, va + len(out) * ENTRY)):
        out.append(f)
    return va, out


def recover(img, window=80):
    text_va, text_raw, text_size, _ = img.section(".text")
    text = img.d[text_raw:text_raw + text_size]

    # Every 4-byte immediate in .text, by value. Unaligned on purpose: these are
    # operands of mov instructions, not aligned words.
    imm = collections.defaultdict(list)
    for off in range(text_size - 4):
        imm[struct.unpack_from("<I", text, off)[0]].append(text_va + off)

    names = {}
    for v, r, rs, _ in img.secs:
        for m in CLASSNAME.finditer(img.d[r:r + rs]):
            names[v + m.start()] = m.group(1).decode()

    lo = min(v for v, _, _, n in img.secs if n == ".data")
    hi = max(v + rs for v, _, rs, n in img.secs if n == ".data")

    out = {}
    for str_va, cname in names.items():
        for site in imm.get(str_va, ()):
            for k in range(-window, window + 1):
                cand = img.u32(site + k)
                if cand is None or not lo < cand < hi:
                    continue
                if not (field(img, cand) and field(img, cand + ENTRY)):
                    continue
                start, fields = table_at(img, cand)
                # Longest table wins: a short bogus run near the same site loses.
                if cname not in out or len(fields) > len(out[cname]["fields"]):
                    out[cname] = {"table": hex(start), "fields": fields}
                break
            if cname in out:
                break
    return out


def self_check():
    """Assert facts verified by hand against the retail 2005-10-14 XBE."""
    root = Path(__file__).resolve().parent.parent
    img = Image(root / "game/hl2_xbox.xbe", root / "build/hl2_analysis.json")

    # A run walked by hand: 19 CBaseAnimating-ish fields ending at m_pRagdoll.
    start, fields = table_at(img, 0x7AA064)
    assert start == 0x7AA064, hex(start)
    assert len(fields) == 19, len(fields)
    assert fields[0]["name"] == "m_bFadeOut", fields[0]
    assert fields[-1]["name"] == "m_pRagdoll", fields[-1]

    # CBaseEntity render members -- offsets cross-checked against each other's
    # spacing in the same table.
    by_name = {f["name"]: f for f in fields}
    assert by_name["m_nRenderFX"]["offset"] == 76, by_name["m_nRenderFX"]
    assert by_name["m_clrRender"]["offset"] == 80, by_name["m_clrRender"]
    assert by_name["m_clrRender"]["type"] == "COLOR32", by_name["m_clrRender"]
    assert by_name["m_nRenderMode"]["offset"] == 108, by_name["m_nRenderMode"]

    maps = recover(img)
    assert len(maps) >= 250, len(maps)
    assert "CAI_LeadGoal" in maps
    print(f"ok: {len(maps)} datamaps, "
          f"{sum(len(m['fields']) for m in maps.values())} fields")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("xbe", nargs="?", type=Path)
    ap.add_argument("analysis", nargs="?", type=Path)
    ap.add_argument("-o", "--out", type=Path, default=Path("build/datamaps.json"))
    ap.add_argument("--self-check", action="store_true")
    args = ap.parse_args()

    if args.self_check:
        return self_check()
    if not (args.xbe and args.analysis):
        ap.error("need xbe and analysis json (or --self-check)")

    maps = recover(Image(args.xbe, args.analysis))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(maps, indent=1))
    fields = sum(len(m["fields"]) for m in maps.values())
    print(f"{len(maps)} datamaps, {fields} fields -> {args.out}")


if __name__ == "__main__":
    sys.exit(main())
