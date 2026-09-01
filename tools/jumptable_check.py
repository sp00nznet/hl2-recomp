"""HL2 golden-fixture check for the toolkit's embedded-switch-table handling.

MSVC parks a switch table inside the function body, on the fall-through path
after the `jmp dword ptr [reg*4 + disp]` that reads it. A linear sweep decodes
those code pointers as instructions and comes out the far side out of phase --
usually past the epilogue, so `pop esi / pop edi` is never lifted and *every
caller* silently loses those registers.

That is what stopped HL2's C++ static initialiser at 8% of its 5,305
constructors, leaving Source with no registered interfaces. The handling lives
upstream in `tools.disasm` because nothing about it is HL2-specific; the
expected answer is a fact about this XBE, so the fixture lives here.

    py -3 tools/jumptable_check.py
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / ".." / "xboxrecomp"))

from tools.disasm.engine import DisasmEngine        # noqa: E402
from tools.disasm.functions import FunctionDetector  # noqa: E402
from tools.disasm.loader import load_image           # noqa: E402

# memcpy and memmove: same 262-byte truncation, same tail-copy table, and
# between them 324 callers -- including, transitively, the constructor walk.
CRT = {0x005AD700: 664, 0x005AE3D0: 674}


def main():
    analysis = ROOT / "build/hl2_analysis.json"
    img = load_image(str(ROOT / "game/hl2_xbox.xbe"),
                     str(analysis) if analysis.exists() else None)

    engine = DisasmEngine(img)
    for section in img.sections:
        if section.executable:
            engine.linear_sweep(section)

    resynced = engine.resync_jump_tables()
    assert resynced == 219, resynced

    # Every table must sit in an executable section and hold at least the
    # three entries the detector demands. A table whose entries leave its own
    # section is a data pointer array that got misread.
    for tbl, end in engine.jump_tables.items():
        home = img.get_section_at_va(tbl)
        assert home is not None and home.executable, hex(tbl)
        assert (end - tbl) // 4 >= 3, hex(tbl)
        for target in engine.jump_table_entries(tbl):
            assert (home.virtual_addr
                    <= target
                    < home.virtual_addr + home.virtual_size), hex(tbl)

    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = engine
    text = img.get_section_at_va(0x005AD700)
    text_end = text.virtual_addr + text.virtual_size
    for start, size in CRT.items():
        end = det._find_function_end(start, next_func=None, sec_end=text_end)
        assert end - start == size, f"sub_{start:08X}: {end - start} != {size}"

    print(f"ok: {resynced} jump tables, "
          f"memcpy {CRT[0x005AD700]} bytes, memmove {CRT[0x005AE3D0]} bytes")


if __name__ == "__main__":
    main()
