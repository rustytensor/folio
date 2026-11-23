#!/usr/bin/env bash
# shellcheck source-path=SCRIPTDIR
# The gates for the hierarchy lines in Folio's list view, as README.md
# beside this script lists them (revision 7). Runs all 28, even after a
# failure, and prints one line per gate:
#
#   PASS <n> <name>
#   FAIL <n> <name>: <what was seen, against what was expected>
#
# Revision 4 removed the old gate 22 (a cap on the added lines); its number
# now belongs to the check that graph's dots are not items, and its other
# two checks moved to gates 2 (no QML file added but src/TreeLines.qml) and
# 3 (no pragma ComponentBehavior). Gate 24 is new. Revision 5 allows the
# outline() accessor in FolderModel and its test (gate 2), expects one more
# passing test (18), counts outline() as well as get() (21), and adds the
# time an insert costs (25). Revision 6 adds the L and Shift+L keys (26, and
# the overlay row and README in 19) and the lines at display scales 1.5 and
# 2 (27), and a folder opened just before a re-read (28). Revision 7 reads
# the drawing's own state (TreeLines' built object) instead of properties
# kept for the gates, and gate 3 checks none are left. The driver draws
# with Qt's software scene graph backend.
#
# Exits 0 only when every gate passes. Takes no arguments; the checkout is
# $FOLIO_REPO, by default the current folder. Logs go to logs/ beside this
# script. Gates 4 to 17 and 20 to 28 run the tree-gates driver (driver/), built into the
# checkout's build/gates folder; main is read with `git archive`, so the
# checkout's working tree is never changed.
set -euo pipefail

gates_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$gates_dir/lib.sh"
# shellcheck source=testlog.sh
source "$gates_dir/testlog.sh"

failures=0
# The em dash, written as its UTF-8 bytes so this file holds none.
em_dash=$'\xe2\x80\x94'
# Files the change may touch (gate 2); revision 5 adds FolderModel's
# outline() and its test.
allowed='^(src/[^/]+\.qml|src/resources\.qrc|src/i18n/en\.json|README\.md|src/foldermodel\.(h|cpp)|tests/folio_tests\.cpp)$'
# Untracked files that belong to local tools, not to the change.
tool_files='^\.[^/]+$'

pass() {
    printf 'PASS %s %s\n' "$1" "$2"
}

fail() {
    printf 'FAIL %s %s: %s\n' "$1" "$2" "$3"
    failures=$((failures + 1))
}

# The first `count` lines of a list, joined with "; ".
first_of() {
    local count=$1 text=$2
    head -n "$count" <<<"$text" | paste -sd ';' - | sed 's/;/; /g'
}

joined() {
    printf '%s; ' "$@" | sed 's/; $//'
}

# Files changed against main, and untracked files, one per line.
changed_files() {
    git -C "$repo" diff main --name-only
    git -C "$repo" ls-files --others --exclude-standard | grep -vE "$tool_files" || true
}

# The lines the change adds: added lines of the diff against main, and every
# line of untracked files. With a glob (such as 'src/*.qml'), only the files
# it matches.
added_lines() {
    local glob=${1:-*} file
    git -C "$repo" diff main -U0 -- "$glob" | grep -E '^\+' | grep -vE '^\+\+\+ ' | cut -c2- || true
    while IFS= read -r file; do
        # shellcheck disable=SC2053 # the glob is meant to match
        if [[ -n $file && $file == $glob ]]; then
            cat "$repo/$file"
        fi
    done < <(git -C "$repo" ls-files --others --exclude-standard | grep -vE "$tool_files" || true)
}

# 1: a clean bin/build exits 0 and prints no warning (compiler or QML).
gate_build() {
    local name="build without warnings" log="$log_dir/build.log" status=0 warnings
    if [[ -f $repo/build/CMakeCache.txt ]] && command -v cmake >/dev/null; then
        cmake --build "$repo/build" --target clean >/dev/null 2>&1 || true
    fi
    timeout 1800 "$repo/bin/build" >"$log" 2>&1 || status=$?
    if ((status != 0)); then
        fail 1 "$name" "bin/build exited $status, expected 0 ($(tail -n 1 "$log"))"
        return
    fi
    # Progress lines name files, which are not warnings.
    warnings=$(grep -vE '^\[ *[0-9]+%\]' "$log" | grep -iE 'warning' || true)
    if [[ -n $warnings ]]; then
        fail 1 "$name" "$(grep -c . <<<"$warnings") warning lines, expected 0 (first: $(first_of 1 "$warnings"))"
        return
    fi
    pass 1 "$name"
}

# 2: only QML files in src/, resources.qrc, en.json, README.md, FolderModel
# and the test file change, the only QML file added is src/TreeLines.qml,
# and the test file only gains one test function for outline().
gate_scope() {
    local name="scope" changed outside added extra tests problems=()
    changed=$(changed_files)
    outside=$(grep -vE "$allowed" <<<"$changed" | grep -v '^$' || true)
    [[ -z $outside ]] || problems+=("changed files outside scope: $(first_of 5 "$outside")")
    added=$(
        git -C "$repo" diff main --diff-filter=A --name-only
        git -C "$repo" ls-files --others --exclude-standard | grep -vE "$tool_files" || true
    )
    extra=$(extra_qml_files "$added")
    [[ -z $extra ]] || problems+=("QML files added besides src/TreeLines.qml: $extra")
    tests=$(git -C "$repo" diff main -U0 -- tests/folio_tests.cpp | test_file_check)
    [[ -z $tests ]] || problems+=("$(paste -sd ';' - <<<"$tests" | sed 's/;/; /g')")
    if ((${#problems[@]} > 0)); then
        fail 2 "$name" "$(joined "${problems[@]}"), expected none"
        return
    fi
    pass 2 "$name"
}

# qmllint's findings for one file, run in the file's folder, without file
# names and positions, so main's and the branch's can be compared.
lint_findings() {
    local folder=$1 file=$2 qmllint
    qmllint=$(command -v qmllint6 || command -v qmllint || echo /usr/lib/qt6/bin/qmllint)
    [[ -f $folder/$file ]] || return 0
    (cd "$folder" && "$qmllint" "$file" 2>&1 || true) |
        grep -E '^(Warning|Error|Critical)' |
        sed -E 's/^([A-Za-z]+): [^ ]*:[0-9]+:[0-9]+: /\1: /' | sort
}

# 3: qmllint finds nothing new in the changed QML files, the added text
# has no em dash and no pragma ComponentBehavior, and (revision 7) src/TreeLines.qml keeps
# no state only for the gates.
gate_lint() {
    local name="qmllint and no state only for the gates" files file new problems=() added gate_only
    if [[ ! -x $(command -v qmllint6 || command -v qmllint || echo /usr/lib/qt6/bin/qmllint) ]]; then
        fail 3 "$name" "qmllint is not installed"
        return
    fi
    files=$(changed_files | grep -E '^src/[^/]+\.qml$' || true)
    while IFS= read -r file; do
        [[ -n $file && -f $repo/$file ]] || continue
        file=${file#src/}
        new=$(comm -23 <(lint_findings "$repo/src" "$file") <(lint_findings "$main_src" "$file"))
        if [[ -n $new ]]; then
            problems+=("$file: $(grep -c . <<<"$new") new qmllint findings (first: $(first_of 1 "$new"))")
        fi
    done <<<"$files"
    added=$(added_lines)
    if grep -qF "$em_dash" <<<"$added"; then
        problems+=("$(grep -cF "$em_dash" <<<"$added") added lines with an em dash")
    fi
    if adds_pragma "$added"; then
        problems+=("pragma ComponentBehavior added")
    fi
    gate_only=$(gate_only_check "$repo/src/TreeLines.qml")
    [[ -z $gate_only ]] || problems+=("$(paste -sd ';' - <<<"$gate_only" | sed 's/;/; /g')")
    if ((${#problems[@]} > 0)); then
        fail 3 "$name" "$(joined "${problems[@]}"), expected none"
        return
    fi
    pass 3 "$name"
}

# Turns a driver run's output into a gate line.
report_driver() {
    local n=$1 name=$2 status=$3 out=$4 last
    last=$(tail -n 1 <<<"$out")
    if ((status == 0)) && [[ $last == PASS ]]; then
        pass "$n" "$name"
    elif [[ $last == FAIL:* ]]; then
        fail "$n" "$name" "${last#FAIL: }"
    elif ((status == 124)); then
        fail "$n" "$name" "the driver was killed after 200 s"
    else
        fail "$n" "$name" "the driver exited $status without a result (see logs/gate-$n.log)"
    fi
}

# 5 to 14, 16, 20, 22, 26 and 28: one run of the driver each.
gate_driver() {
    local n=$1 name=$2 why=$3 out status=0
    if [[ -n $why ]]; then
        fail "$n" "$name" "$why"
        return
    fi
    out=$(run_driver "$n" 2>"$log_dir/gate-$n.log") || status=$?
    report_driver "$n" "$name" "$status" "$out"
}

# 4: style off by default with nothing computed, and the list pixel-identical
# to main's (main's QML loaded from the export; the C++ is the same).
gate_off() {
    local name="off draws as main" why=$1 branch_png="$log_dir/off-branch.png" main_png="$log_dir/off-main.png" result
    if [[ -n $why ]]; then
        fail 4 "$name" "$why"
        return
    fi
    if [[ ! -f $main_src/Main.qml ]]; then
        fail 4 "$name" "main's src/ could not be exported"
        return
    fi
    rm -f "$branch_png" "$main_png"
    result=$(FOLIO_GATES_QML_DIR="$repo/src" driver_result "$log_dir/gate-4.log" 4 "$branch_png")
    if [[ $result != PASS ]]; then
        fail 4 "$name" "${result#FAIL: }"
        return
    fi
    result=$(FOLIO_GATES_QML_DIR="$main_src" driver_result "$log_dir/gate-4-main.log" listshot "$main_png")
    if [[ $result != PASS ]]; then
        fail 4 "$name" "main's screenshot: ${result#FAIL: }"
        return
    fi
    result=$(driver_result "$log_dir/gate-4-compare.log" compare "$branch_png" "$main_png")
    if [[ $result != PASS ]]; then
        fail 4 "$name" "${result#FAIL: }"
        return
    fi
    pass 4 "$name"
}

# 15: three bad stored values, each in a process of its own.
gate_bad_values() {
    local name="bad stored values" why=$1 value result problems=()
    if [[ -n $why ]]; then
        fail 15 "$name" "$why"
        return
    fi
    for value in banana 7 ''; do
        result=$(driver_result "$log_dir/gate-15-${value:-empty}.log" 15 "$value")
        [[ $result == PASS ]] || problems+=("${result#FAIL: }")
    done
    if ((${#problems[@]} > 0)); then
        fail 15 "$name" "$(joined "${problems[@]}")"
        return
    fi
    pass 15 "$name"
}

# 17: screenshots.sh writes the six PNGs, and they show the lines.
gate_shots() {
    local name="screenshots" why=$1 status=0 result
    if [[ -n $why ]]; then
        fail 17 "$name" "$why"
        return
    fi
    SKIP_DRIVER_BUILD=1 "$gates_dir/screenshots.sh" >"$log_dir/screenshots.log" 2>&1 || status=$?
    if ((status != 0)); then
        fail 17 "$name" "screenshots.sh exited $status ($(tail -n 1 "$log_dir/screenshots.log"))"
        return
    fi
    result=$(driver_result "$log_dir/gate-17.log" verify-shots "$gates_dir/shots")
    if [[ $result != PASS ]]; then
        fail 17 "$name" "${result#FAIL: }"
        return
    fi
    pass 17 "$name"
}

# 21: the cost of a change, four parts in processes of their own, on a copy
# of the checkout's QML whose list model is the counting wrapper.
gate_cost() {
    local name="cost of a change" why=$1 copy="$build_dir/counting-src" part result problems=()
    if [[ -n $why ]]; then
        fail 21 "$name" "$why"
        return
    fi
    if ! why=$(make_counting_copy "$repo/src" "$copy"); then
        fail 21 "$name" "$why"
        return
    fi
    for part in idle rtl big insert; do
        result=$(FOLIO_GATES_QML_DIR="$copy" driver_result "$log_dir/gate-21-$part.log" 21 "$part")
        [[ $result == PASS ]] || problems+=("${result#FAIL: }")
    done
    rm -rf "$copy"
    if ((${#problems[@]} > 0)); then
        fail 21 "$name" "$(joined "${problems[@]}")"
        return
    fi
    pass 21 "$name"
}

# 25: an insert in 10,000 rows costs at most 20 ms more with the lines on
# than off, in each style; run on the counting copy, as measure.sh is.
gate_insert_time() {
    local name="an insert in 10,000 rows" why=$1 copy="$build_dir/counting-src" result
    if [[ -n $why ]]; then
        fail 25 "$name" "$why"
        return
    fi
    if ! why=$(make_counting_copy "$repo/src" "$copy"); then
        fail 25 "$name" "$why"
        return
    fi
    result=$(FOLIO_GATES_QML_DIR="$copy" driver_result "$log_dir/gate-25.log" 25)
    rm -rf "$copy"
    if [[ $result != PASS ]]; then
        fail 25 "$name" "${result#FAIL: }"
        return
    fi
    pass 25 "$name"
}

# 27: the lines at display scales 1.5 and 2, as gate 17 (and at 2 gate
# 23) checks them at scale 1.
gate_scaled() {
    local name="lines at display scales 1.5 and 2" why=$1 scale result problems=()
    if [[ -n $why ]]; then
        fail 27 "$name" "$why"
        return
    fi
    for scale in 1.5 2; do
        result=$(scaled_result "$scale" "$gates_dir/shots-scaled/$scale" gate-27)
        [[ $result == PASS ]] || problems+=("${result#FAIL: }")
    done
    if ((${#problems[@]} > 0)); then
        fail 27 "$name" "$(joined "${problems[@]}")"
        return
    fi
    pass 27 "$name"
}

# 24: lineStyle is kept in Main.qml's one Settings block, as its last entry.
gate_settings_block() {
    local name="one Settings block" problems
    problems=$(settings_check "$repo/src/Main.qml")
    if [[ -n $problems ]]; then
        fail 24 "$name" "$(paste -sd ';' - <<<"$problems" | sed 's/;/; /g')"
        return
    fi
    pass 24 "$name"
}

# 23: over the selected row the lines blend with its tint, and joins are
# no darker than the guide; read from gate 17's screenshots.
gate_overlays() {
    local name="lines over the selection and at joins" why=$1 result
    if [[ -n $why ]]; then
        fail 23 "$name" "$why"
        return
    fi
    if [[ ! -f $gates_dir/shots/probe.txt ]]; then
        fail 23 "$name" "no screenshots (screenshots.sh failed in gate 17)"
        return
    fi
    result=$(driver_result "$log_dir/gate-23.log" verify-overlays "$gates_dir/shots")
    if [[ $result != PASS ]]; then
        fail 23 "$name" "${result#FAIL: }"
        return
    fi
    pass 23 "$name"
}

# 18: the test suite has main's result plus one passing test for
# outline(), and the English catalog matches.
gate_tests() {
    local name="test suite as baseline plus the outline test" log="$log_dir/test.log" baseline result problems
    if [[ ! -f $gates_dir/baseline.txt ]] || [[ -z $(value_of passed "$(cat "$gates_dir/baseline.txt")") ]]; then
        fail 18 "$name" "baseline.txt holds no recorded counts (run record-baseline.sh first)"
        return
    fi
    baseline=$(cat "$gates_dir/baseline.txt")
    timeout 3600 "$repo/bin/test" -V >"$log" 2>&1 || true
    if ! result=$(parse_test_log "$log"); then
        fail 18 "$name" "bin/test printed no Qt Test totals ($(tail -n 1 "$log"))"
        return
    fi
    problems=$(suite_check "$baseline" "$result")
    if [[ -n $problems ]]; then
        fail 18 "$name" "$(paste -sd ';' - <<<"$problems" | sed 's/;/; /g')"
        return
    fi
    pass 18 "$name"
}

# 19: README.md mentions the hierarchy lines and their keys, the ? overlay
# has a row for L and Shift+L, and every qsTr text the change adds is in
# en.json as itself.
gate_docs() {
    local name="documentation" problems=() missing
    if ! grep -qiE 'hierarchy lines?' "$repo/README.md"; then
        problems+=("README.md does not mention the hierarchy lines")
    fi
    if ! grep -qE '(^|[^[:alnum:]])Shift\+L([^[:alnum:]]|$)' "$repo/README.md"; then
        problems+=("README.md does not mention the keys L and Shift+L")
    fi
    if ! has_lines_keys_row "$repo/src/HelpOverlay.qml"; then
        problems+=("HelpOverlay.qml has no row with the keys L and Shift+L")
    fi
    missing=$(added_lines 'src/*.qml' | python3 -I -c '
import json, re, sys
catalog = json.load(open(sys.argv[1], encoding="utf-8"))
missing = []
for raw in re.findall(r"qsTr\(\s*\"((?:[^\"\\\\]|\\\\.)*)\"", sys.stdin.read()):
    try:
        text = json.loads("\"" + raw + "\"")
    except ValueError:
        text = raw
    entry = catalog.get(text)
    if entry is None or (isinstance(entry, str) and entry != text):
        missing.append(text)
print("; ".join(sorted(set(missing))))
' "$repo/src/i18n/en.json")
    [[ -z $missing ]] || problems+=("qsTr texts not in en.json as themselves: $missing")
    if ((${#problems[@]} > 0)); then
        fail 19 "$name" "$(joined "${problems[@]}")"
        return
    fi
    pass 19 "$name"
}

main() {
    local why=""
    if [[ ! -f $repo/src/Main.qml ]]; then
        echo "no folio checkout at $repo (set FOLIO_REPO)" >&2
        exit 2
    fi
    mkdir -p "$log_dir"
    trap remove_main_src EXIT

    gate_build
    gate_scope
    export_main_src >/dev/null 2>&1 || true
    gate_lint

    why=$(build_driver) || true
    gate_off "$why"
    gate_driver 5 "geometry" "$why"
    gate_driver 6 "width, colour and opacity" "$why"
    gate_driver 7 "dots" "$why"
    gate_driver 8 "trace route" "$why"
    gate_driver 9 "closing and opening a folder" "$why"
    gate_driver 10 "menu sets the style" "$why"
    gate_driver 11 "button only in the list" "$why"
    gate_driver 12 "zoom 1.5" "$why"
    gate_driver 13 "2000 rows, 8 deep" "$why"
    gate_driver 14 "no lines where folders do not open in place" "$why"
    gate_bad_values "$why"
    gate_driver 16 "right to left" "$why"
    gate_shots "$why"
    gate_tests
    gate_docs
    gate_driver 20 "lines meet the real arrows and pictures at every zoom" "$why"
    gate_cost "$why"
    gate_driver 22 "dots are paths, not items" "$why"
    gate_overlays "$why"
    gate_settings_block
    gate_insert_time "$why"
    gate_driver 26 "L and Shift+L step the style" "$why"
    gate_scaled "$why"
    gate_driver 28 "a folder opened just before a re-read" "$why"

    ((failures == 0))
}

main "$@"
