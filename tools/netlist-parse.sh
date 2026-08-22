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

IVL="${IVL:-iverilog}"
command -v "$IVL" >/dev/null 2>&1 || { echo "no $IVL on PATH" >&2; exit 2; }

LIB="lib/ttl7400.lib"
[ -f "$LIB" ] || { echo "missing $LIB" >&2; exit 2; }

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

awk '
/cell *\(/ {
    if (match($0, /"[^"]+"/)) cell = substr($0, RSTART + 1, RLENGTH - 2)
}
/pin *\(/ {
    if (match($0, /"[^"]+"/)) { pin = substr($0, RSTART + 1, RLENGTH - 2) }
}
/direction *:/ {
    dir = ($0 ~ /output/) ? "output" : "input"
    if (cell != "" && pin != "" && !seen[cell "." pin]++) {
        order[cell] = order[cell] (order[cell] ? "," : "") pin
        d[cell "." pin] = dir
    }
}
END {
    for (c in order) {
        printf "module %s (%s);\n", c, order[c]
        n = split(order[c], p, ",")
        for (i = 1; i <= n; i++) printf "  %s %s;\n", d[c "." p[i]], p[i]
        printf "endmodule\n\n"
    }
}
' "$LIB" > "$OUT/stubs.v"

if [ ! -s "$OUT/stubs.v" ]; then
    echo "no cell stubs built from $LIB" >&2
    exit 2
fi

fails=0
printf '%-24s %-10s %s\n' design cells result

for src in designs/*.sv designs/*.vhd tests/smoke.sv tests/bigger.sv \
           tests/hier.sv; do
    [ -f "$src" ] || continue
    name=$(basename "$src")
    vhdl=""
    case "$src" in *.vhd) vhdl="--vhdl" ;; esac

    if ! $TAKAHE $vhdl --lib "$LIB" --opt --map "$OUT/n.v" "$src" \
         >"$OUT/log" 2>&1; then
        printf '%-24s %-10s %s\n' "$name" "-" "SYNTH FAILED"
        fails=$((fails+1))
        continue
    fi

    n=$(grep -cE '^[A-Za-z_][A-Za-z0-9_]* +[A-Za-z_][A-Za-z0-9_]* *\( *\.' "$OUT/n.v")

    if "$IVL" -tnull -o /dev/null "$OUT/stubs.v" "$OUT/n.v" \
        >"$OUT/ivl" 2>&1; then
        printf '%-24s %-10s %s\n' "$name" "$n" "parsed"
    else
        printf '%-24s %-10s %s\n' "$name" "$n" "REJECTED"
        sed 's/^/    /' "$OUT/ivl" | head -5
        fails=$((fails+1))
    fi
done

echo
if [ "$fails" -gt 0 ]; then
    echo "$fails netlist(s) another tool would not read" >&2
    exit 1
fi
echo "every emitted netlist parses with $IVL"
