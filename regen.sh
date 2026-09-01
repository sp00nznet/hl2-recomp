#!/usr/bin/env bash
# Regenerate the recompiled C from hl2_xbox.xbe, in the one order that works.
#
#   xbe_parser  section layout -> game/hl2_analysis.json (everything reads this)
#   rtti        MSVC RTTI -> classes/vtables, and the seed list. Must run BEFORE
#               disasm: RTTI proves 12k vtable entries are function starts, and
#               feeding them in finds ~8k functions the linear sweep misses.
#   disasm      rewrites functions.json from scratch, so every name applied by a
#               later step is lost and must be re-applied. ~30s for the 6 MB
#               .text. Only run it when tools/disasm or the seeds changed.
#   datamaps    Source datamap_t field tables (class / member / byte offset).
#   func_id     library-function identification (CRT, vtable thunks).
#   recomp      emits the C.
#
# Usage: ./regen.sh [--disasm]

set -uo pipefail

HL2="$(cd "$(dirname "$0")" && pwd)"
RECOMP="$HL2/../xboxrecomp"
XBE="$HL2/game/hl2_xbox.xbe"

if [[ ! -f "$XBE" ]]; then
    echo "missing $XBE"
    echo "extract it from your own disc image / archive, e.g.:"
    echo "  7z x 'Half-Life 2 [!].7z' -ogame && mv 'game/Half-Life 2'/* game/"
    exit 1
fi

if [[ "${1:-}" == "--disasm" ]]; then
    echo "==> xbe_parser"
    (cd "$RECOMP" && py -3 -m tools.xbe_parser "$XBE" \
        --json "$HL2/game/hl2_analysis.json" --quiet)

    echo "==> rtti (seeds)"
    py -3 "$HL2/tools/rtti.py" "$XBE" "$HL2/game/hl2_analysis.json" \
        -o "$HL2/build/rtti.json" --seeds "$HL2/game/rtti_seeds.json"

    echo "==> disasm"
    (cd "$RECOMP" && py -3 -m tools.disasm "$XBE" \
        --analysis-json "$HL2/game/hl2_analysis.json" \
        --seed-functions "$HL2/game/rtti_seeds.json" \
        -o "$HL2/build/disasm" -v \
        | tr '\r' '\n' | grep -E "Realigned|Total functions|Reachable|Seeded")
fi

echo "==> datamaps"
py -3 "$HL2/tools/datamaps.py" "$XBE" "$HL2/game/hl2_analysis.json" \
    -o "$HL2/build/datamaps.json"

echo "==> func_id"
(cd "$RECOMP" && py -3 -m tools.func_id "$XBE" \
    --functions "$HL2/build/disasm/functions.json" \
    --strings   "$HL2/build/disasm/strings.json" \
    --xrefs     "$HL2/build/disasm/xrefs.json" \
    -o "$HL2/build/func_id" >/dev/null)

echo "==> recomp"
(cd "$RECOMP" && py -3 -m tools.recomp "$XBE" --all --split 1000 \
    --disasm-dir  "$HL2/build/disasm" \
    --func-id-dir "$HL2/build/func_id" \
    --gen-dir     "$HL2/src/game/recomp/gen" \
    -o "$HL2/build/recomp" | grep -aE "unresolved|functions \(|Complete")

echo "==> done"
