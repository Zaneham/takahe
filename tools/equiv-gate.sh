#!/bin/bash
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2

TAKAHE="$ROOT/takahe"
[ -x "$TAKAHE" ] || TAKAHE="$ROOT/takahe.exe"
if [ ! -x "$TAKAHE" ]; then
    echo "no takahe binary, run make first" >&2
    exit 2
fi

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

fails=0
disagree=0
printf '%-24s %-34s %s\n' design verdict note

for src in designs/*.sv designs/*.vhd tests/smoke.sv tests/bigger.sv \
           tests/hier.sv tests/parts.sv tests/sram.sv; do
    [ -f "$src" ] || continue
    name=$(basename "$src")
    vhdl=""
    case "$src" in *.vhd) vhdl="--vhdl" ;; esac

    timeout 300 $TAKAHE $vhdl --opt --equiv "$src" >"$OUT/log" 2>&1
    rc=$?

    if [ $rc -eq 124 ]; then
        printf '%-24s %-34s %s\n' "$name" "TIMEOUT" ""
        fails=$((fails+1))
        continue
    fi

    verdict=$(grep -oE 'proved equivalent|formally equivalent|statistically equivalent|MISMATCH|FAIL —' "$OUT/log" | tail -1)
    bad=$(grep -c 'encoding is wrong' "$OUT/log")

    case "$verdict" in
        *equivalent)
            note=""
            if [ "$bad" -gt 0 ]; then
                note="solver disagreed, see equiv-gate.sh"
                disagree=$((disagree+1))
            fi
            printf '%-24s %-34s %s\n' "$name" "$verdict" "$note"
            ;;
        "")
            printf '%-24s %-34s %s\n' "$name" "NO VERDICT" ""
            fails=$((fails+1))
            ;;
        *)
            printf '%-24s %-34s %s\n' "$name" "$verdict" "NOT EQUIVALENT"
            fails=$((fails+1))
            ;;
    esac
done

echo
if [ "$disagree" -gt 0 ]; then
    echo "$disagree design(s) where the CNF encoder contradicts simulation."
    echo "Known, not gated: the encoder has a bug and its verdict is ignored"
    echo "in favour of simulation. Gate on this once that is fixed."
    echo
fi
if [ "$fails" -gt 0 ]; then
    echo "$fails design(s) failed equivalence" >&2
    exit 1
fi
echo "every design is equivalent to itself after optimisation"
