#!/usr/bin/env bash
# Measure what ABC adds on top of Takahe's native optimiser.
# Two-question experiment:
#   1. Does ABC accept Takahe's BLIF cleanly?
#   2. By how much does it shrink the netlist?
#
# Run from the takahe directory: ./tests/abc_bench.sh
#
# Install ABC if missing:
#   Linux:  apt install berkeley-abc      (or build from github.com/berkeley-abc/abc)
#   macOS:  brew install abc
#   Win:    build from source under MSYS2 (mingw works), or use WSL

set -u

TAKAHE="./takahe.exe"
[ -x "./takahe" ] && TAKAHE="./takahe"

if [ ! -x "$TAKAHE" ]; then
    echo "ERROR: takahe binary not found. Run 'make' first."
    exit 1
fi

if ! command -v abc >/dev/null 2>&1; then
    cat <<EOF
ERROR: 'abc' not found on PATH.

ABC is the Berkeley logic synthesis tool. Get it from:
  https://github.com/berkeley-abc/abc

Quick install on Linux: sudo apt install berkeley-abc
Quick install on macOS: brew install abc

Once installed, rerun this script.
EOF
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Designs to bench. Add more here as they prove out.
DESIGNS=(
    "tests/smoke.sv"
    "tests/bigger.sv"
    "tests/picorv32.v"
)

count_blif() {
    # $1 = blif file. Prints: names latches subckts
    local f="$1"
    local n l s
    n=$(grep -c '^\.names' "$f" 2>/dev/null || echo 0)
    l=$(grep -c '^\.latch' "$f" 2>/dev/null || echo 0)
    s=$(grep -c '^\.subckt' "$f" 2>/dev/null || echo 0)
    printf "%5d %5d %5d" "$n" "$l" "$s"
}

run_abc() {
    # $1 = input blif, $2 = output blif. Returns ABC exit code.
    abc -q "read_blif $1; strash; balance; rewrite; refactor; balance; rewrite -z; balance; rewrite -z; balance; write_blif $2" >"$WORK/abc.log" 2>&1
}

printf "%-22s | %-17s | %-17s | %s\n" "DESIGN" "TAKAHE (n l s)" "+ ABC   (n l s)" "DELTA NAMES"
printf "%-22s-+-%-17s-+-%-17s-+-%s\n" "----------------------" "-----------------" "-----------------" "-----------"

for design in "${DESIGNS[@]}"; do
    name=$(basename "$design")
    before="$WORK/${name%.??}_before.blif"
    after="$WORK/${name%.??}_after.blif"

    if ! "$TAKAHE" --opt --blif "$before" "$design" >"$WORK/takahe.log" 2>&1; then
        printf "%-22s | takahe FAILED. See %s\n" "$name" "$WORK/takahe.log"
        cp "$WORK/takahe.log" "./abc_bench_${name}_takahe.log"
        continue
    fi

    pre=$(count_blif "$before")
    pre_n=$(echo "$pre" | awk '{print $1}')

    if ! run_abc "$before" "$after"; then
        printf "%-22s | %-17s | ABC FAILED. Log saved.\n" "$name" "$pre"
        cp "$WORK/abc.log" "./abc_bench_${name}_abc.log"
        continue
    fi

    post=$(count_blif "$after")
    post_n=$(echo "$post" | awk '{print $1}')

    if [ "$pre_n" -gt 0 ]; then
        delta=$(awk -v a="$pre_n" -v b="$post_n" 'BEGIN{printf "%+.1f%%", (b-a)*100.0/a}')
    else
        delta="n/a"
    fi

    printf "%-22s | %-17s | %-17s | %s\n" "$name" "$pre" "$post" "$delta"
done

echo ""
echo "n=.names  l=.latch  s=.subckt"
echo "Negative delta = ABC shrank the netlist (good)."
echo "ABC failures usually mean it choked on a .subckt cell takahe emits"
echo "that isn't in ABC's native vocabulary. That's a real integration"
echo "finding — note which design failed and inspect the .blif by hand."
