#!/usr/bin/env bash
# Regenerate the recompiled C from hl2_xbox.xbe, in the one order that works.
#
#   xbe_parser    section layout -> game/hl2_analysis.json
#   rtti          MSVC RTTI -> classes/vtables + the seed list. Lives in the
#                 toolkit, not here: nothing about it is HL2-specific. Must run
#                 BEFORE disasm -- a vtable slot is proof of a function entry
#                 point, and seeding them finds ~8k functions the linear sweep
#                 misses (33,140 -> 41,215).
#   disasm        rewrites functions.json from scratch, so every name applied by
#                 a later step is lost and must be re-applied. ~30s for the
#                 6 MB .text. Only re-run when tools/disasm or the seeds change.
#                 Two seed files: game/rtti_seeds.json (static, regenerated
#                 above) and config/seed_functions.json (runtime-observed,
#                 committed). The second comes from tools.seed_from_log: the
#                 routine handed to PsCreateSystemThreadEx is only ever pushed
#                 as an argument, so nothing calls it and no static pass finds
#                 it. Without it the game thread never starts and the process
#                 exits cleanly after two kernel calls -- which reads like a
#                 successful run rather than zero progress.
#   func_id       library-function identification (CRT, vtable thunks).
#   abi_analysis  calling convention / params / return type. Without it every
#                 function lifts as cdecl / 0 params / int-or-void and the
#                 generated signatures carry that guess. 9,307 of HL2's
#                 functions are thiscall -- it is not optional on a C++ title.
#   datamaps      Source datamap_t field tables (class / member / byte offset).
#                 HL2-specific, so it stays in this repo.
#   recomp        emits the C.
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
    (cd "$RECOMP" && py -3 -m tools.rtti "$XBE" \
        -o "$HL2/build/rtti.json" --seeds "$HL2/game/rtti_seeds.json")

    echo "==> disasm"
    (cd "$RECOMP" && py -3 -m tools.disasm "$XBE" \
        --analysis-json "$HL2/game/hl2_analysis.json" \
        --seed-functions "$HL2/game/rtti_seeds.json" \
        --seed-functions "$HL2/config/seed_functions.json" \
        -o "$HL2/build/disasm" -v \
        | tr '\r' '\n' | grep -E "Realigned|Total functions|Reachable|Seeded")
fi

echo "==> func_id"
(cd "$RECOMP" && py -3 -m tools.func_id "$XBE" \
    --functions "$HL2/build/disasm/functions.json" \
    --strings   "$HL2/build/disasm/strings.json" \
    --xrefs     "$HL2/build/disasm/xrefs.json" \
    -o "$HL2/build/func_id" | tail -1)

echo "==> abi_analysis"
(cd "$RECOMP" && py -3 -m tools.abi_analysis "$XBE" \
    --functions  "$HL2/build/disasm/functions.json" \
    --identified "$HL2/build/func_id/identified_functions.json" \
    --output-dir "$HL2/build/abi" | tail -1)

echo "==> datamaps"
py -3 "$HL2/tools/datamaps.py" "$XBE" "$HL2/game/hl2_analysis.json" \
    -o "$HL2/build/datamaps.json"

echo "==> recomp"
(cd "$RECOMP" && py -3 -m tools.recomp "$XBE" --all --split 1000 \
    --disasm-dir  "$HL2/build/disasm" \
    --func-id-dir "$HL2/build/func_id" \
    --abi-dir     "$HL2/build/abi" \
    --gen-dir     "$HL2/src/game/recomp/gen" \
    -o "$HL2/build/recomp" | grep -aE "unresolved|functions \(|Complete")

echo "==> done. Now: cmake --build build-msvc --config Release"
