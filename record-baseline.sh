#!/usr/bin/env bash
# shellcheck source-path=SCRIPTDIR
# Records the result of Folio's test suite on main, which gate 18 of
# gates.sh compares against, without touching the checkout's working tree:
# main is exported with `git archive` into a temporary folder beside this
# script, built and tested there with its own bin/test, and the folder is
# removed afterwards. Writes baseline.txt beside this script; the full log
# goes to logs/baseline-test.log.
#
# The checkout is $FOLIO_REPO (default: the current folder); the branch is
# $BASELINE_REF (default main).
set -euo pipefail

gates_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="${FOLIO_REPO:-$PWD}"
ref="${BASELINE_REF:-main}"
log_dir="$gates_dir/logs"
mkdir -p "$log_dir"
# shellcheck source=testlog.sh
source "$gates_dir/testlog.sh"

log="$log_dir/baseline-test.log"
commit=$(git -C "$repo" rev-parse "$ref")
work=$(mktemp -d "$gates_dir/baseline-tree.XXXXXX")
trap 'rm -rf "$work"' EXIT

git -C "$repo" archive "$commit" | tar -x -C "$work"
# This folder is group-writable (and has ACLs), and Folio will not read a
# plugin folder that others may change, so its plugin tests would fail
# here but not in the checkout. The export is made private to the owner.
chmod -R go-w "$work"
status=0
timeout 3600 "$work/bin/test" -V >"$log" 2>&1 || status=$?

if ! result=$(parse_test_log "$log"); then
    printf 'bin/test produced no Qt Test totals (exit %s); see %s\n' "$status" "$log" >&2
    exit 1
fi

{
    printf '# Result of bin/test on %s at %s (exit status %s), recorded %s.\n' \
        "$ref" "$commit" "$status" "$(date -u +%Y-%m-%dT%H:%MZ)"
    printf '%s\n' "$result"
} >"$gates_dir/baseline.txt"
cat "$gates_dir/baseline.txt"
