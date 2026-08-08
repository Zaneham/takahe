#!/bin/bash
# Score Takahe's SystemVerilog frontend against the sv-tests conformance suite.
#
#   git clone --depth 1 https://github.com/chipsalliance/sv-tests.git
#   tools/sv-tests.sh path/to/sv-tests            # report
#   tools/sv-tests.sh path/to/sv-tests --min 66   # and fail below 66%
#
# A test carrying :should_fail_because: must be rejected; every other test must
# be accepted. The signal is takahe's exit status, which only started meaning
# anything once lexer and parser diagnostics were wired into it. Before that a
# sweep like this scored every malformed file as a pass and reported 92%.
#
# Paths in the output are relative to the suite root on purpose. Absolute ones
# carry a home directory, and this output is meant to be publishable.

set -u

SUITE="${1:-}"
MIN=""
[ "${2:-}" = "--min" ] && MIN="${3:-}"

if [ -z "$SUITE" ] || [ ! -d "$SUITE/tests" ]; then
    echo "usage: $0 <path-to-sv-tests> [--min PERCENT]" >&2
    exit 2
fi

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TAKAHE="$ROOT/takahe"
[ -x "$TAKAHE" ] || TAKAHE="$ROOT/takahe.exe"
if [ ! -x "$TAKAHE" ]; then
    echo "no takahe binary, run make first" >&2
    exit 2
fi

SUITE=$(cd "$SUITE" && pwd)
cd "$ROOT" || exit 2          # defs/ are resolved relative to cwd

pass=0; fail=0; wrong_reject=0; wrong_accept=0; timeouts=0
declare -A ch_ok ch_tot

while IFS= read -r abs; do
    rel=${abs#"$SUITE"/}
    if grep -q ':should_fail_because:' "$abs"; then want=fail; else want=pass; fi

    timeout 10 "$TAKAHE" --parse "$abs" >/dev/null 2>&1
    rc=$?
    if   [ $rc -eq 124 ]; then got=timeout; timeouts=$((timeouts+1))
    elif [ $rc -eq 0 ];   then got=pass
    else                       got=fail
    fi

    # Chapter is the directory under tests/, which is how the suite is grouped.
    ch=$(echo "$rel" | cut -d/ -f2)

    if [ "$want" = pass ]; then
        ch_tot[$ch]=$(( ${ch_tot[$ch]:-0} + 1 ))
        if [ "$got" = pass ]; then
            pass=$((pass+1)); ch_ok[$ch]=$(( ${ch_ok[$ch]:-0} + 1 ))
        else
            wrong_reject=$((wrong_reject+1))
            echo "$rel" >> /tmp/takahe_sv_rejected.$$
        fi
    else
        if [ "$got" = fail ]; then fail=$((fail+1))
        else wrong_accept=$((wrong_accept+1)); fi
    fi
done < <(find "$SUITE/tests" -name '*.sv' | sort)

valid=$((pass + wrong_reject))
invalid=$((fail + wrong_accept))
total=$((valid + invalid))
rate=$(( valid ? 100 * pass / valid : 0 ))

echo
printf '%-28s %s\n' "suite"                  "$total tests"
printf '%-28s %d / %d  (%d%%)\n' "valid SystemVerilog accepted" "$pass" "$valid" "$rate"
printf '%-28s %d\n' "valid wrongly rejected" "$wrong_reject"
printf '%-28s %d / %d\n' "invalid rejected"  "$fail" "$invalid"
printf '%-28s %d\n' "invalid wrongly accepted" "$wrong_accept"
[ "$timeouts" -gt 0 ] && printf '%-28s %d\n' "timeouts" "$timeouts"

echo
printf '%-16s %6s %6s %6s\n' chapter accepted total rate
for ch in $(printf '%s\n' "${!ch_tot[@]}" | sort); do
    t=${ch_tot[$ch]}; o=${ch_ok[$ch]:-0}
    printf '%-16s %6d %6d %5d%%\n' "$ch" "$o" "$t" $(( 100 * o / t ))
done | sort -k4 -n

rm -f /tmp/takahe_sv_rejected.$$

if [ -n "$MIN" ] && [ "$rate" -lt "$MIN" ]; then
    echo
    echo "conformance $rate% is below the $MIN% floor" >&2
    exit 1
fi
exit 0
