#!/usr/bin/env bash
# shellcheck source-path=SCRIPTDIR
# Takes the screenshots of the hierarchy lines that gate 17 checks: Folio's
# real main window, 1280 x 800, offscreen, in the list view on the
# contract's fixture (docs, api, v1, schemas and guides open, the cursor on
# migrating.md), in styles quiet, graph and trace, each in a dark theme
# (Tokyo Night) and a light one (Catppuccin Latte).
#
# Writes to shots/ beside this script:
#   quiet-dark.png  quiet-light.png  graph-dark.png  graph-light.png
#   trace-dark.png  trace-light.png
# and, for gate 17's comparison, off-dark.png, off-light.png and probe.txt
# (the window pixel on the guide of api where the line colour is checked).
#
# The checkout is $FOLIO_REPO (default: the current folder). The driver is built
# first unless SKIP_DRIVER_BUILD=1 (gates.sh has just built it).
set -euo pipefail

gates_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$gates_dir/lib.sh"

shots_dir="$gates_dir/shots"
mkdir -p "$log_dir"

if [[ ${SKIP_DRIVER_BUILD:-0} != 1 ]]; then
    if ! why=$(build_driver); then
        echo "$why" >&2
        exit 1
    fi
fi

rm -rf "$shots_dir"
mkdir -p "$shots_dir"
status=0
out=$(run_driver shots "$shots_dir" 2>"$log_dir/shots.log") || status=$?
last=$(tail -n 1 <<<"$out")
if ((status != 0)) || [[ $last != PASS ]]; then
    printf 'the screenshots failed: %s (exit %s; see logs/shots.log)\n' "${last:-no result}" "$status" >&2
    exit 1
fi
for name in quiet-dark quiet-light graph-dark graph-light trace-dark trace-light; do
    if [[ ! -s $shots_dir/$name.png ]]; then
        echo "$name.png was not written" >&2
        exit 1
    fi
done
echo "six screenshots in $shots_dir"
