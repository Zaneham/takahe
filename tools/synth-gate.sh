#!/bin/bash
# Synthesise every shipped design and check the cell count against a floor.
#
#   tools/synth-gate.sh              # report
#   tools/synth-gate.sh --update     # rewrite the floors from this build
#
# The unit tests prove a construct lowers. They cannot see a whole design
# quietly getting smaller, which is how 8182188 dropped picorv32 from 3,305
# cells to 257 and still exited zero. A floor per design catches that: any
# change that stops logic reaching the netlist trips it.
#
# Floors sit a little under the current count so ordinary optimiser work does
# not trip them. Anything that legitimately shrinks a design updates the floor
# in the same commit, which is the point at which somebody looks at why.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2

TAKAHE="$ROOT/takahe"
[ -x "$TAKAHE" ] || TAKAHE="$ROOT/takahe.exe"
if [ ! -x "$TAKAHE" ]; then
    echo "no takahe binary, run make first" >&2
    exit 2
fi

LIB="lib/sky130_fd_sc_hd__tt_025C_1v80.lib"
FLOORS="tools/synth-floors.txt"
UPDATE=""
[ "${1:-}" = "--update" ] && UPDATE=1

[ -f "$FLOORS" ] || { echo "missing $FLOORS" >&2; exit 2; }

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

fails=0
printf '%-24s %8s %8s   %s\n' design cells floor result

while read -r name floor; do
    case "$name" in ''|'#'*) continue ;; esac

    src=""
    for cand in "designs/$name.sv" "designs/$name.vhd" "tests/$name.sv"; do
        [ -f "$cand" ] && { src="$cand"; break; }
    done
    if [ -z "$src" ]; then
        printf '%-24s %8s %8s   %s\n' "$name" - "$floor" "MISSING SOURCE"
        fails=$((fails+1))
        continue
    fi

    vhdl=""
    case "$src" in *.vhd) vhdl="--vhdl" ;; esac

    # Cell count comes from the emitted netlist, not the log line, so a
    # backend that reports more than it writes cannot pass this.
    if ! $TAKAHE $vhdl --lib "$LIB" --opt --map "$OUT/$name.v" "$src" \
         >"$OUT/$name.log" 2>&1; then
        printf '%-24s %8s %8s   %s\n' "$name" - "$floor" "SYNTH FAILED"
        fails=$((fails+1))
        continue
    fi

    n=$(grep -cE '^[a-z0-9_]+__[a-z0-9_]+ ' "$OUT/$name.v" 2>/dev/null)
    n=${n:-0}

    if [ -n "$UPDATE" ]; then
        printf '%-24s %8s %8s   %s\n' "$name" "$n" "$floor" "-> $(( n * 9 / 10 ))"
        continue
    fi

    if [ "$n" -lt "$floor" ]; then
        printf '%-24s %8s %8s   %s\n' "$name" "$n" "$floor" "BELOW FLOOR"
        fails=$((fails+1))
    else
        printf '%-24s %8s %8s   %s\n' "$name" "$n" "$floor" "ok"
    fi
done < "$FLOORS"

if [ -n "$UPDATE" ]; then
    echo
    echo "floors above are 90% of the measured count; edit $FLOORS by hand"
    exit 0
fi

echo
if [ "$fails" -gt 0 ]; then
    echo "$fails design(s) below floor or failed to synthesise" >&2
    exit 1
fi
echo "all designs at or above their floor"
