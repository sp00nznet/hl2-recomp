#!/usr/bin/env bash
# Regenerate the recompiled C from hl2_xbox.xbe, in the one order that works.
#
#   disasm    rewrites functions.json from scratch, so every name applied by a
#             later step is lost and must be re-applied. Only run it when
#             tools/disasm changed. ~30s for the 6 MB .text.
#   datamaps  recovers Source datamap_t field tables (class/member/offset).
#   func_id   library-function identification (CRT, vtable thunks).
#   recomp    emits the C.
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
    (cd "$RECOMP" && py -3 -m tools.xbe_parser "$XBE" --json "$HL2/game/hl2_analysis.json" --quiet)

    echo "==> disasm"
    (cd "$RECOMP" && py -3 -m tools.disasm "$XBE" \
        --analysis-json "$HL2/game/hl2_analysis.json" \
        -o "$HL2/build/disasm" -v | grep -E "Realigned|Total functions|Reachable")
fi

echo "==> datamaps"
py -3 "$HL2/tools/datamaps.py" "$XBE" "$HL2/game/hl2_analysis.json" -o "$HL2/build/datamaps.json"

echo "==> func_id"
(cd "$RECOMP" && py -3 -m tools.func_id "$XBE"     --functions "$HL2/build/disasm/functions.json"     --strings   "$HL2/build/disasm/strings.json"     --xrefs     "$HL2/build/disasm/xrefs.json"     -o "$HL2/build/func_id" >/dev/null)

echo "==> recomp"
(cd "$RECOMP" && py -3 -m tools.recomp "$XBE" --all --split 1000     --disasm-dir  "$HL2/build/disasm"     --func-id-dir "$HL2/build/func_id"     --gen-dir     "$HL2/src/game/recomp/gen"     -o "$HL2/build/recomp" | grep -aE "unresolved|functions \(|Complete")

echo "==> done"
