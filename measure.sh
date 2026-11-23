#!/usr/bin/env bash
# shellcheck source-path=SCRIPTDIR
# Measures what one inserted row costs the hierarchy lines in a long list
# (README.md, revision 4: a measurement, not a gate). The list holds
# 10,000 rows with one folder open (50 items); a file is added inside that
# folder five times per style (off, quiet, graph, trace), and for each style
# one line gives the median CPU time of the GUI thread over the 400 ms after
# the insert and the median numbers of get() and outline() calls on the
# list's model. The difference to the line for off is what the lines cost;
# gate 25 is the same run with a limit on it.
#
# Takes no arguments; the checkout is $FOLIO_REPO (default: the current folder).
# Runs on a copy of the checkout's QML whose list model is the driver's
# counting wrapper, as gate 21 does. Exits non-zero when the run fails.
set -euo pipefail

gates_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$gates_dir/lib.sh"

copy="$build_dir/measure-src"
cleanup() {
    rm -rf "$copy"
}
trap cleanup EXIT

if [[ -z ${SKIP_DRIVER_BUILD:-} ]] && ! why=$(build_driver); then
    echo "$why" >&2
    exit 1
fi
if ! why=$(make_counting_copy "$repo/src" "$copy"); then
    echo "$why" >&2
    exit 1
fi
status=0
out=$(FOLIO_GATES_QML_DIR="$copy" run_driver measure 2>"$log_dir/measure.log") || status=$?
grep -v '^PASS$' <<<"$out" || true
if ((status != 0)); then
    echo "the measurement failed (exit $status, see logs/measure.log)" >&2
    exit 1
fi
