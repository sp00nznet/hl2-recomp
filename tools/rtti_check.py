"""HL2 golden-fixture check for the toolkit's RTTI recovery.

The recovery itself lives upstream in xboxrecomp (`tools.rtti`) because nothing
about it is HL2-specific. What *is* HL2-specific is the expected answer, so the
fixture lives here: if a toolkit change silently breaks the decode, this fails.

    py -3 tools/rtti_check.py
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / ".." / "xboxrecomp"))

from tools.rtti import demangle, methods_by_class, recover, seeds  # noqa: E402


def main():
    r = recover(str(ROOT / "game/hl2_xbox.xbe"))

    assert len(r["type_descriptors"]) == 2336, len(r["type_descriptors"])
    assert len(r["vtables"]) == 2932, len(r["vtables"])
    assert sum(len(m) for *_, m in r["vtables"]) == 176865

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

    assert r["primary_len"][".?AVCNPC_Alyx@@"] == 533
    assert (r["primary_len"][".?AVCNPC_Alyx@@"]
            >= r["primary_len"][".?AVCBaseEntity@@"])

    methods = methods_by_class(r)
    assert len(methods) == 12288, len(methods)
    assert sum(1 for v in methods.values() if len(v) == 1) == 8992
    assert len(seeds(r)) == 12288

    print(f"ok: {len(r['type_descriptors'])} classes, {len(r['vtables'])} "
          f"vtables, {len(methods)} methods, {len(seeds(r))} seeds")


if __name__ == "__main__":
    main()
