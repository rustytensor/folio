#!/usr/bin/env bash
# shellcheck source-path=SCRIPTDIR/..
# Shows that the gates tell a right answer from a wrong one, without the
# code under test: main's QML is exported, given the window's lineStyle
# setting and an instance of a stand-in TreeLines.qml in the list's
# contentItem, and loaded into the driver through FOLIO_GATES_QML_DIR.
#
#   good/TreeLines.qml  the contract's numbers (revision 2), rows from one
#                       outline() per rebuild, no item per dot, and lines
#                       drawn without a layer; with good/lines-menu.qml and
#                       good/lines-keys.qml planted into PathBar.qml and
#                       FolderPane.qml, gates 4 to 9, 12 to 14, 16, 20 to 22
#                       and 25 to 28 must pass
#   bad/TreeLines.qml   wrong on purpose; gates 4, 5, 20, 21, 22 and 25 to
#                       28 must fail
#   main's QML as is    no lines at all; gates 4 to 17, 20 to 22 and 25 to 28
#                       must fail cleanly
#   gates 2, 3, 18, 24  their checks on made-up inputs: a second QML file,
#                       a pragma ComponentBehavior, a test file change that
#                       is not one outline() test, a suite result that is
#                       not main's plus one outline test, a second Settings
#                       block or lineStyle not last in it fail; the right
#                       ones pass
#
# main's C++ has no FolderModel::outline(), so the counting wrapper stands
# in for it here (FOLIO_GATES_OUTLINE_FALLBACK); gates.sh never sets that.
#
# Gate 15 is not tried on the stand-ins: it reads the lines menu, which
# only the real PathBar.qml has; main's QML must fail it cleanly.
#
# With gate numbers as arguments (run.sh 15 22 24), only the cases of those
# gates run. Prints one line per check (OK or WRONG, with the result) and
# exits 0 only when every result is the one expected. The temporary QML
# folders live in the checkout's build/gates and are removed afterwards.
set -euo pipefail

gates_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib.sh
source "$gates_dir/lib.sh"
# shellcheck source=testlog.sh
source "$gates_dir/testlog.sh"

export FOLIO_GATES_OUTLINE_FALLBACK=1
selftest_dir="$gates_dir/selftest"
work="$build_dir/selftest-qml"
wrong=0
only=("$@")

# Whether the cases of a gate run: all do when no gate was named.
wanted() {
    local gate
    ((${#only[@]} == 0)) && return 0
    for gate in "${only[@]}"; do
        [[ $gate == "$1" ]] && return 0
    done
    return 1
}

cleanup() {
    rm -rf "$work"
    remove_main_src
}
trap cleanup EXIT

if ! why=$(build_driver); then
    echo "$why" >&2
    exit 1
fi
export_main_src

# Inserts a snippet file into a QML file, `after` lines below the one line
# that is exactly `line`; fails when that line is not there once.
plant() {
    python3 -I "$selftest_dir/plant.py" "$@"
}

# main's QML with the stand-in wired in as the contract places it, and a
# counting copy of it for gate 21.
make_variant() {
    local variant=$1 dir="$work/$1"
    rm -rf "$dir"
    mkdir -p "$dir"
    cp -r "$main_src/." "$dir/"
    cp "$selftest_dir/$variant/TreeLines.qml" "$dir/TreeLines.qml"
    sed -i 's/^    property real zoom: 1$/&\n    property string lineStyle: "off"/' "$dir/Main.qml"
    sed -i 's/^        property alias zoom: win.zoom$/&\n        property alias lineStyle: win.lineStyle/' "$dir/Main.qml"
    sed -i 's/^    FolioText {$/    TreeLines { view: list; parent: list.contentItem }\n\n&/' "$dir/FolderList.qml"
    grep -q 'property string lineStyle' "$dir/Main.qml" && grep -q 'TreeLines {' "$dir/FolderList.qml"
    if [[ $variant == good ]]; then
        plant "$dir/PathBar.qml" '            onClicked: bar.cycleView()' "$selftest_dir/good/lines-menu.qml" 1
        plant "$dir/FolderPane.qml" '    Shortcut { sequence: "Shift+S"; enabled: pane.keys; onActivated: pathBar.stepSort(-1) }' \
            "$selftest_dir/good/lines-keys.qml" 0
    fi
    make_counting_copy "$dir" "$work/$variant-counting"
}

report() {
    local want=$1 variant=$2 label=$3 result=$4
    if [[ $result == "$want"* ]]; then
        printf 'OK    %s gate %s: %s\n' "$variant" "$label" "$result"
    else
        printf 'WRONG %s gate %s: %s (expected %s)\n' "$variant" "$label" "$result" "$want"
        wrong=$((wrong + 1))
    fi
}

# expect <PASS|FAIL> <qml folder name> <label> <driver arguments...>; the
# label starts with the gate number.
expect() {
    local want=$1 variant=$2 label=$3 result
    shift 3
    wanted "${label%%-*}" || return 0
    result=$(FOLIO_GATES_QML_DIR="$work/$variant" driver_result "$log_dir/selftest-$variant-$label.log" "$@")
    report "$want" "$variant" "$label" "$result"
}

# Gate 4 as gates.sh runs it: the variant's list against main's.
expect_off() {
    local want=$1 variant=$2 result
    wanted 4 || return 0
    local png="$log_dir/selftest-$variant-off.png" main_png="$log_dir/selftest-main-off.png"
    result=$(FOLIO_GATES_QML_DIR="$work/$variant" driver_result "$log_dir/selftest-$variant-4.log" 4 "$png")
    if [[ $result == PASS ]]; then
        result=$(FOLIO_GATES_QML_DIR="$main_src" driver_result "$log_dir/selftest-main-listshot.log" listshot "$main_png")
        [[ $result != PASS ]] || result=$(driver_result "$log_dir/selftest-$variant-compare.log" compare "$png" "$main_png")
    fi
    report "$want" "$variant" 4 "$result"
}

# Gate 21 as gates.sh runs it: four parts on the counting copy; the first
# failure is the result.
expect_cost() {
    local want=$1 variant=$2 part result=PASS
    wanted 21 || return 0
    for part in idle rtl big insert; do
        result=$(FOLIO_GATES_QML_DIR="$work/$variant-counting" driver_result "$log_dir/selftest-$variant-21-$part.log" 21 "$part")
        [[ $result == PASS ]] || break
    done
    report "$want" "$variant" 21 "$result"
}

# Gate 27 as gates.sh runs it: shots at scales 1.5 and 2, checked; the
# first failure is the result.
expect_scaled() {
    local want=$1 variant=$2 scale result=PASS
    wanted 27 || return 0
    for scale in 1.5 2; do
        result=$(FOLIO_GATES_QML_DIR="$work/$variant" scaled_result "$scale" "$work/$variant-shots-$scale" "selftest-$variant-27")
        [[ $result == PASS ]] || break
    done
    report "$want" "$variant" 27 "$result"
}

# Gate 3's no-gate-only-state check on a made-up TreeLines.qml: the
# drawing's state in `built`, plus `extra` lines in the root item.
gate_only_file() {
    local file="$work/TreeLines-$1.qml"
    printf '%s\n' 'Item {' '    id: lines' "$2" '    QtObject {' '        id: built' "$3" \
        '        property var guides: []' '        property var route: []' '        property var dotPaths: []' \
        '        property string routeDotPath: ""' '    }' \
        '    // readonly property var dots: built.dots (a comment, not code)' \
        '    component Layer: Item { property var dots: [] }' '}' >"$file"
    gate_only_check "$file"
}

# Gate 19's overlay check on a made-up overlay holding the S row and `row`.
overlay_row() {
    local file="$work/HelpOverlay-$1.qml"
    printf '            [qsTr("Next or previous sort order"), [["S"], ["Shift", "S"]]],\n%s\n' "$2" >"$file"
    has_lines_keys_row "$file" || echo "no row with the keys L and Shift+L"
}

# A static check on made-up inputs: report the problems it prints.
# expect_check <PASS|FAIL> <gate> <label> <problems>
expect_check() {
    local want=$1 gate=$2 label=$3 problems=$4 result
    wanted "$gate" || return 0
    if [[ -z $problems ]]; then
        result=PASS
    else
        result="FAIL: $(paste -sd ';' - <<<"$problems" | sed 's/;/; /g')"
    fi
    report "$want" "$label" "$gate" "$result"
}

# Gate 24 on a Main.qml made from main's: lineStyle added as the last entry
# of its block (right), in a second block, or before zoom (both wrong).
settings_variant() {
    local file="$work/Main-$1.qml"
    cp "$main_src/Main.qml" "$file"
    case $1 in
        right) sed -i 's/^        property alias zoom: win.zoom$/&\n        property alias lineStyle: win.lineStyle/' "$file" ;;
        second) sed -i 's/^    \/\/ Ctrl+Alt+N: a second pane/    Settings { property alias lineStyle: win.lineStyle }\n\n&/' "$file" ;;
        early) sed -i 's/^        property alias viewMode: win.viewMode$/&\n        property alias lineStyle: win.lineStyle/' "$file" ;;
    esac
    grep -q 'alias lineStyle' "$file"
    settings_check "$file"
}

# Gate 2's test file check on a made-up diff.
check_tests() {
    test_file_check <<<"$1"
}
readonly outline_test=$'@@ -9300,0 +9301,7 @@\n+    // outline() gives every row\'s depth and whether it is a folder.\n+    void folderModelGivesItsOutline()\n+    {\n+        FolderModel model;\n+        QCOMPARE(model.outline().value("depths").toList().size(), 0);\n+    }\n+'

# Gate 18's check on made-up results against a made-up baseline.
readonly suite_base=$'passed=278\nfailed=0\nskipped=1\nfailing=\nenglish=PASS'
suite_result() {
    printf 'passed=%s\nfailed=0\nskipped=1\nfailing=\nenglish=PASS\noutline=%s\n' "$1" "$2"
}

make_variant good
make_variant bad

expect_off PASS good
for gate in 5 6 7 8 9 12 13 14 16 20 22; do
    expect PASS good "$gate" "$gate"
done
expect_cost PASS good

expect_off FAIL bad
expect FAIL bad 5 5
expect FAIL bad 20 20
expect_cost FAIL bad
expect FAIL bad 22 22
expect FAIL bad-counting 25 25
expect PASS good-counting 25 25
expect PASS good 26 26
expect FAIL bad 26 26
expect_scaled PASS good
expect_scaled FAIL bad
expect PASS good 28 28
expect FAIL bad 28 28

expect_check PASS 2 tests-none "$(check_tests '')"
expect_check PASS 2 tests-one "$(check_tests "$outline_test")"
expect_check PASS 2 tests-declared "$(check_tests "$outline_test"$'\n@@ -580,0 +581 @@\n+    void folderModelGivesItsOutline();')"
expect_check FAIL 2 tests-removed "$(check_tests "$outline_test"$'\n@@ -40 +40,0 @@\n-    QVERIFY(true);')"
expect_check FAIL 2 tests-two "$(check_tests "$outline_test"$'\n@@ -9400,0 +9408,4 @@\n+    void outlineOfAnEmptyFolder()\n+    {\n+        QCOMPARE(FolderModel().outline().size(), 2);\n+    }')"
expect_check FAIL 2 tests-helper "$(check_tests "$outline_test"$'\n@@ -120,0 +121,3 @@\n+static int helper()\n+{ return 1; }\n+')"
expect_check FAIL 2 tests-unnamed "$(check_tests "${outline_test//folderModelGivesItsOutline/folderModelRows}")"
expect_check PASS 3 state-clean "$(gate_only_file clean '' '')"
expect_check FAIL 3 state-guides "$(gate_only_file guides '    readonly property var guides: built.guides' '')"
expect_check FAIL 3 state-route "$(gate_only_file route '    readonly property var route: built.route' '')"
expect_check FAIL 3 state-dots "$(gate_only_file dots '    readonly property var dots: built.dots.concat(built.routeDots)' '')"
expect_check FAIL 3 state-built-dots "$(gate_only_file built-dots '' '        property var dots: []')"
expect_check FAIL 3 state-route-dots "$(gate_only_file route-dots '' '        property var routeDots: []')"
expect_check PASS 19 overlay-row "$(overlay_row right '            [qsTr("Next or previous line style"), [["L"], ["Shift", "L"]]],')"
expect_check FAIL 19 overlay-none "$(overlay_row none '')"
expect_check FAIL 19 overlay-l-only "$(overlay_row l-only '            [qsTr("Next line style"), [["L"]]],')"
expect_check PASS 18 suite-plus-one "$(suite_check "$suite_base" "$(suite_result 279 folderModelGivesItsOutline)")"
expect_check FAIL 18 suite-as-main "$(suite_check "$suite_base" "$(suite_result 278 '')")"
expect_check FAIL 18 suite-no-outline "$(suite_check "$suite_base" "$(suite_result 279 '')")"
expect_check FAIL 18 suite-plus-two "$(suite_check "$suite_base" "$(suite_result 280 folderModelGivesItsOutline,outlineIsEmpty)")"

expect_check PASS 2 one-qml "$(extra_qml_files 'src/TreeLines.qml')"
expect_check FAIL 2 two-qml "$(extra_qml_files $'src/TreeLines.qml\nsrc/LinesMenu.qml')"
expect_check PASS 3 no-pragma "$(adds_pragma 'import QtQuick' && echo 'pragma ComponentBehavior added')"
expect_check FAIL 3 pragma "$(adds_pragma $'pragma ComponentBehavior: Bound\nimport QtQuick' && echo 'pragma ComponentBehavior added')"
expect_check PASS 24 settings-right "$(settings_variant right)"
expect_check FAIL 24 settings-second "$(settings_variant second)"
expect_check FAIL 24 settings-early "$(settings_variant early)"

# main's QML as it is: the code is missing, and every driver gate must
# fail with a message, never crash or hang.
ln -sfn "$main_src" "$work/main"
make_counting_copy "$main_src" "$work/main-counting"
expect_off FAIL main
for gate in 5 6 7 8 9 10 11 12 13 14 16 20 22; do
    expect FAIL main "$gate" "$gate"
done
expect FAIL main 15-banana 15 banana
expect_cost FAIL main
expect FAIL main-counting 25 25
expect FAIL main 26 26
expect_scaled FAIL main
expect FAIL main 28 28
expect FAIL main 17-shots shots "$work/main-shots"

((wrong == 0))
