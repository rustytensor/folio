# shellcheck shell=bash
# Reads the output of `bin/test -V` (ctest running Folio's Qt Test suite
# verbosely) and prints what the gates compare, one key=value per line:
#
#   passed=<n>   failed=<n>   skipped=<n>   from Qt Test's "Totals:" line
#   failing=<a,b,...>         the failing test functions, sorted, comma-joined
#   english=PASS|missing      whether englishCatalogMatchesTheSources passed
#
# Returns 1, printing nothing, when the log has no "Totals:" line (the suite
# did not build or did not finish).
#
# Sourced by gates.sh and record-baseline.sh.

parse_test_log() {
    local log=$1 totals passed failed skipped failing english
    totals=$(grep -oE 'Totals: [0-9]+ passed, [0-9]+ failed, [0-9]+ skipped' "$log" | tail -n 1) || return 1
    [[ -n $totals ]] || return 1
    passed=$(sed -E 's/Totals: ([0-9]+) passed.*/\1/' <<<"$totals")
    failed=$(sed -E 's/.* ([0-9]+) failed.*/\1/' <<<"$totals")
    skipped=$(sed -E 's/.* ([0-9]+) skipped.*/\1/' <<<"$totals")
    failing=$(grep -oE 'FAIL! +: [A-Za-z_][A-Za-z0-9_]*::[A-Za-z_][A-Za-z0-9_]*' "$log" |
        sed -E 's/.*:://' | sort -u | paste -sd, -) || true
    if grep -qE 'PASS +: [A-Za-z_][A-Za-z0-9_]*::englishCatalogMatchesTheSources\(\)' "$log"; then
        english=PASS
    else
        english=missing
    fi
    printf 'passed=%s\nfailed=%s\nskipped=%s\nfailing=%s\nenglish=%s\n' \
        "$passed" "$failed" "$skipped" "$failing" "$english"
}

# The value of `key` in key=value lines read from a file or a string.
value_of() {
    local key=$1 text=$2
    sed -n -E "s/^${key}=(.*)$/\\1/p" <<<"$text" | tail -n 1
}
