#!/usr/bin/env bash
# Records the result of Folio's test suite on the untouched branch, which
# gate 13 of gates.sh compares against: runs `bin/test -V` in the checkout
# ($FOLIO_REPO, default the folder it is run from) and writes baseline.txt beside this
# script. The full log goes to logs/baseline-test.log.
set -euo pipefail

gates_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="${FOLIO_REPO:-$PWD}"
log_dir="$gates_dir/logs"
mkdir -p "$log_dir"
# shellcheck source=testlog.sh
source "$gates_dir/testlog.sh"

log="$log_dir/baseline-test.log"
commit=$(git -C "$repo" rev-parse HEAD)
branch=$(git -C "$repo" rev-parse --abbrev-ref HEAD)
status=0
timeout 3600 "$repo/bin/test" -V >"$log" 2>&1 || status=$?

if ! result=$(parse_test_log "$log"); then
    printf 'bin/test produced no Qt Test totals (exit %s); see %s\n' "$status" "$log" >&2
    exit 1
fi

{
    printf '# Result of bin/test on %s at %s (exit status %s), recorded %s.\n' \
        "$branch" "$commit" "$status" "$(date -u +%Y-%m-%dT%H:%MZ)"
    printf '%s\n' "$result"
} >"$gates_dir/baseline.txt"
cat "$gates_dir/baseline.txt"
