#!/usr/bin/env bash
# Recompile default.xbe -- the disc's installer.
#
# The game reads .xzp; the disc ships .xz_, which is an 'xCmp' container
# (Microsoft XCompress / LZX). default.xbe is what turns one into the other:
# it carries the xCmp magic and both extensions, and LoaderMedia/install.txt
# is the manifest it follows. Rather than reimplement LZX, recompile the
# 466 KB loader and let it perform its own install.
#
# Same pipeline as regen.sh minus the HL2-specific steps: the loader is a
# small C program, so there is no RTTI to mine and no Source datamaps.
#
# Usage: ./regen_loader.sh [--disasm]
set -uo pipefail

HL2="$(cd "$(dirname "$0")" && pwd)"
RECOMP="$HL2/../xboxrecomp"
XBE="$HL2/game/default.xbe"
OUT="$HL2/build/loader"
GEN="$HL2/src/loader/recomp/gen"

if [[ ! -f "$XBE" ]]; then
    echo "missing $XBE -- extract it from your own disc"
    exit 1
fi
mkdir -p "$OUT" "$GEN"

if [[ "${1:-}" == "--disasm" ]]; then
    echo "==> xbe_parser"
    (cd "$RECOMP" && py -3 -m tools.xbe_parser "$XBE" \
        --json "$OUT/loader_analysis.json" --quiet)

    # RTTI is off in this image (a C title), so there are no vtable seeds to
    # mine and no seed file to pass. In the game they find ~8k functions;
    # here their absence costs nothing.
    echo "==> rtti (optional)"
    (cd "$RECOMP" && py -3 -m tools.rtti "$XBE" \
        -o "$OUT/rtti.json" --seeds "$OUT/rtti_seeds.json") || true

    echo "==> disasm"
    SEED_ARG=""
    if [[ -f "$OUT/rtti_seeds.json" ]]; then
        SEED_ARG="--seed-functions $OUT/rtti_seeds.json"
    fi
    # Runtime-observed entry points, the same feedback loop the game uses:
    # a vtable slot reached only through a pointer is invisible to every
    # static pass, and an unresolved one leaves the caller calling null.
    if [[ -f "$HL2/config/loader_seed_functions.json" ]]; then
        SEED_ARG="$SEED_ARG --seed-functions $HL2/config/loader_seed_functions.json"
    fi
    (cd "$RECOMP" && py -3 -m tools.disasm "$XBE" \
        --analysis-json "$OUT/loader_analysis.json" \
        $SEED_ARG \
        -o "$OUT/disasm" -v | tr '\r' '\n' \
        | grep -E "Realigned|Total functions|Reachable|Seeded")
fi

echo "==> func_id"
(cd "$RECOMP" && py -3 -m tools.func_id "$XBE" \
    --functions "$OUT/disasm/functions.json" \
    --strings   "$OUT/disasm/strings.json" \
    --xrefs     "$OUT/disasm/xrefs.json" \
    -o "$OUT/func_id" | tail -1)

echo "==> abi_analysis"
(cd "$RECOMP" && py -3 -m tools.abi_analysis "$XBE" \
    --functions  "$OUT/disasm/functions.json" \
    --identified "$OUT/func_id/identified_functions.json" \
    --output-dir "$OUT/abi" | tail -1)

echo "==> recomp"
(cd "$RECOMP" && py -3 -m tools.recomp "$XBE" --all --split 1000 \
    --disasm-dir  "$OUT/disasm" \
    --func-id-dir "$OUT/func_id" \
    --abi-dir     "$OUT/abi" \
    --exclude-manual "$HL2/src/loader/recomp/recomp_manual.c" \
    --gen-dir     "$GEN" \
    -o "$OUT/recomp"
) > "$OUT/recomp.log" 2>&1
recomp_status=$?
grep -aE "unresolved|functions \(|Complete" "$OUT/recomp.log" || true
if [[ $recomp_status -ne 0 ]]; then
    echo "!! recomp failed (status $recomp_status) -- gen/ is now STALE" >&2
    tail -25 "$OUT/recomp.log" >&2
    exit $recomp_status
fi

echo "==> done"
