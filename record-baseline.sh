#!/usr/bin/env bash
# Records the result of Folio's test suite on main, which gate 13 of
# gates.sh compares against. main is exported with `git archive` into the
# checkout's build/gates folder (the checkout and its own build are not
# touched), built and tested there with its bin/test -V, and removed
# afterwards; the result goes to baseline.txt beside this script and the
# full log to logs/baseline-test.log. The checkout is $FOLIO_REPO, by
# default the folder it is run from.
set -euo pipefail

gates_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="${FOLIO_REPO:-$PWD}"
log_dir="$gates_dir/logs"
export_dir="$repo/build/gates/baseline-main"
mkdir -p "$log_dir"
# shellcheck source=testlog.sh
source "$gates_dir/testlog.sh"

cleanup() {
    rm -rf "$export_dir"
}
trap cleanup EXIT

rm -rf "$export_dir"
mkdir -p "$export_dir"
git -C "$repo" archive main | tar -x -C "$export_dir"
commit=$(git -C "$repo" rev-parse main)

log="$log_dir/baseline-test.log"
status=0
timeout 3600 "$export_dir/bin/test" -V >"$log" 2>&1 || status=$?

if ! result=$(parse_test_log "$log"); then
    printf 'bin/test produced no Qt Test totals (exit %s); see %s\n' "$status" "$log" >&2
    exit 1
fi

{
    printf '# Result of bin/test on main at %s (exit status %s), recorded %s.\n' \
        "$commit" "$status" "$(date -u +%Y-%m-%dT%H:%MZ)"
    printf '%s\n' "$result"
} >"$gates_dir/baseline.txt"
cat "$gates_dir/baseline.txt"
