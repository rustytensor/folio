#!/usr/bin/env bash
# The gates for Folio's draggable pane lines, as README.md beside this
# script lists them. Runs all 14, even after a failure, and prints one line
# per gate:
#
#   PASS <n> <name>
#   FAIL <n> <name>: <what was seen, against what was expected>
#
# Exits 0 only when every gate passes. Takes no arguments; the checkout is
# $FOLIO_REPO, by default the folder it is run from. Logs go to logs/ beside this
# script. Gates 3 to 12 run the pane-gates driver (driver/), built into the
# checkout's build/gates folder.
set -euo pipefail

gates_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="${FOLIO_REPO:-$PWD}"
build_dir="$repo/build/gates"
driver="$build_dir/pane-gates"
log_dir="$gates_dir/logs"
# shellcheck source=testlog.sh
source "$gates_dir/testlog.sh"

failures=0

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

# 1: a clean bin/build exits 0 and prints no warning. The CMake build is
# cleaned first, so every file is compiled and every warning is seen.
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

# 2: the change stays inside its scope: nothing under tests/, no C++ or
# CMake file, no W or Shift+W shortcut and no PaneWidths.
gate_scope() {
    local name="scope" changed forbidden shortcuts panewidths problems=()
    changed=$(
        git -C "$repo" diff main --name-only
        git -C "$repo" ls-files --others --exclude-standard
    )
    forbidden=$(grep -E '^tests/|\.(cpp|h)$|(^|/)CMakeLists\.txt$' <<<"$changed" || true)
    [[ -z $forbidden ]] || problems+=("changed files outside scope: $(first_of 5 "$forbidden")")
    shortcuts=$(grep -rnE 'sequences?[[:space:]]*:.*"(Shift\+)?W"' "$repo/src" --include='*.qml' || true)
    [[ -z $shortcuts ]] || problems+=("W shortcut: $(first_of 2 "${shortcuts#"$repo/"}")")
    panewidths=$(grep -rlI --exclude-dir=build --exclude-dir=.git 'PaneWidths' "$repo" || true)
    [[ -z $panewidths ]] || problems+=("PaneWidths in: $(first_of 3 "$panewidths")")
    if ((${#problems[@]} > 0)); then
        fail 2 "$name" "$(printf '%s; ' "${problems[@]}" | sed 's/; $//'), expected none"
        return
    fi
    pass 2 "$name"
}

# Builds the driver for gates 3 to 12 (release, no debug information).
# Prints why it could not, and returns 1, when it fails.
build_driver() {
    local log="$log_dir/driver-build.log"
    if ! command -v qmake6 >/dev/null; then
        echo "qmake6 is not installed"
        return 1
    fi
    mkdir -p "$build_dir"
    # Built for another checkout before: start over, as make goes by file
    # times and would keep objects compiled from the other tree.
    if [[ $(cat "$build_dir/.checkout" 2>/dev/null || true) != "$repo" ]]; then
        find "$build_dir" -mindepth 1 -delete
        printf '%s\n' "$repo" >"$build_dir/.checkout"
    fi
    # The QML is compiled into the driver; it is packed afresh every run, so
    # a QML file with an older time than the last pack is never missed.
    rm -rf "$build_dir/rcc" "$build_dir"/obj/qrc_*.o
    if ! (cd "$build_dir" &&
        env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS \
            qmake6 FOLIO_REPO="$repo" "$gates_dir/driver/pane-gates.pro" &&
        make -j"$(nproc)") >"$log" 2>&1; then
        echo "the driver did not build (see logs/driver-build.log)"
        return 1
    fi
    if grep -qE '^(CFLAGS|CXXFLAGS|LFLAGS)[[:space:]]*=.*[[:space:]]-g([0-9a-z]*)?([[:space:]]|$)' "$build_dir/Makefile"; then
        echo "the driver's Makefile compiles with -g"
        return 1
    fi
}

# 3 to 12: one run of the driver each, in a process of its own.
gate_driver() {
    local n=$1 name=$2 why=$3 out status=0 last
    if [[ -n $why ]]; then
        fail "$n" "$name" "$why"
        return
    fi
    out=$(timeout 150 "$driver" "$n" 2>"$log_dir/gate-$n.log") || status=$?
    last=$(tail -n 1 <<<"$out")
    if ((status == 0)) && [[ $last == PASS ]]; then
        pass "$n" "$name"
    elif [[ $last == FAIL:* ]]; then
        fail "$n" "$name" "${last#FAIL: }"
    elif ((status == 124)); then
        fail "$n" "$name" "the driver was killed after 150 s"
    else
        fail "$n" "$name" "the driver exited $status without a result (see logs/gate-$n.log)"
    fi
}

# 13: the test suite has the baseline's result, and the English catalog
# matches the sources.
gate_tests() {
    local name="test suite as baseline" log="$log_dir/test.log" baseline result key want got problems=()
    if [[ ! -f $gates_dir/baseline.txt ]] || [[ -z $(value_of passed "$(cat "$gates_dir/baseline.txt")") ]]; then
        fail 13 "$name" "baseline.txt holds no recorded counts (run record-baseline.sh on main first)"
        return
    fi
    baseline=$(cat "$gates_dir/baseline.txt")
    timeout 3600 "$repo/bin/test" -V >"$log" 2>&1 || true
    if ! result=$(parse_test_log "$log"); then
        fail 13 "$name" "bin/test printed no Qt Test totals ($(tail -n 1 "$log"))"
        return
    fi
    for key in passed failed skipped failing; do
        want=$(value_of "$key" "$baseline")
        got=$(value_of "$key" "$result")
        [[ $got == "$want" ]] || problems+=("$key ${got:-none}, baseline ${want:-none}")
    done
    [[ $(value_of english "$result") == PASS ]] ||
        problems+=("englishCatalogMatchesTheSources did not pass")
    if ((${#problems[@]} > 0)); then
        fail 13 "$name" "$(printf '%s; ' "${problems[@]}" | sed 's/; $//')"
        return
    fi
    pass 13 "$name"
}

# 14: the README's keys table and the help overlay each say, on one line,
# that a line is dragged and double-clicked; nothing mentions Shift+W or a
# details divider.
gate_docs() {
    local name="documentation" file problems=() stale
    for file in README.md src/HelpOverlay.qml; do
        if [[ ! -f $repo/$file ]]; then
            problems+=("$file is missing")
        elif ! grep -iE 'drag' "$repo/$file" | grep -qiE 'double[- ]?click'; then
            problems+=("$file has no line mentioning dragging and double-clicking")
        fi
    done
    stale=$(grep -rlIiE 'Shift\+W|details divider' "$repo/src" "$repo/docs" "$repo/README.md" 2>/dev/null || true)
    [[ -z $stale ]] || problems+=("Shift+W or a details divider mentioned in: $(first_of 3 "$stale")")
    if ((${#problems[@]} > 0)); then
        fail 14 "$name" "$(printf '%s; ' "${problems[@]}" | sed 's/; $//')"
        return
    fi
    pass 14 "$name"
}

main() {
    local why=""
    if [[ ! -f $repo/src/Main.qml ]]; then
        echo "no folio checkout at $repo (set FOLIO_REPO)" >&2
        exit 2
    fi
    mkdir -p "$log_dir"

    gate_build
    gate_scope

    why=$(build_driver) || true
    gate_driver 3 "defaults" "$why"
    gate_driver 4 "drag sizes panes" "$why"
    gate_driver 5 "double-click resets" "$why"
    gate_driver 6 "kept for the next window" "$why"
    gate_driver 7 "columns keep their scroll" "$why"
    gate_driver 8 "right to left" "$why"
    gate_driver 9 "limits" "$why"
    gate_driver 10 "press below drag distance" "$why"
    gate_driver 11 "bad settings" "$why"
    gate_driver 12 "scroll bar stays a scroll bar" "$why"

    gate_tests
    gate_docs

    ((failures == 0))
}

main "$@"
