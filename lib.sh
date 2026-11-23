# shellcheck shell=bash
# Shared by gates.sh, screenshots.sh and selftest/run.sh: where things are,
# how the tree-gates driver is built and run, and main's QML exported for
# comparison. The caller sets `gates_dir` before sourcing this file.
#
# The checkout is $FOLIO_REPO (default: the current folder); the driver is built
# into its build/gates folder, release only, without debug information.

gates_dir="${gates_dir:?set gates_dir before sourcing lib.sh}"
repo="${FOLIO_REPO:-$PWD}"
build_dir="$repo/build/gates"
driver="$build_dir/tree-gates"
log_dir="$gates_dir/logs"
# Main's src/, exported with git archive (the checkout is never touched).
# The driver loads it, like any QML folder, in place of qrc:/FolioUI/.
main_src="$build_dir/main-src"
# The driver finds fixture.sh through this.
export FOLIO_GATES_DIR="$gates_dir"

# Builds the driver. Prints why it could not, and returns 1, when it fails.
build_driver() {
    local log="$log_dir/driver-build.log"
    mkdir -p "$log_dir"
    if ! command -v qmake6 >/dev/null; then
        echo "qmake6 is not installed"
        return 1
    fi
    mkdir -p "$build_dir"
    # Built for another checkout or another driver before: start over, as
    # make goes by file times and would keep objects from the other tree.
    if [[ $(cat "$build_dir/.checkout" 2>/dev/null || true) != "$repo tree-gates" ]]; then
        find "$build_dir" -mindepth 1 -delete
        printf '%s\n' "$repo tree-gates" >"$build_dir/.checkout"
    fi
    # The QML is compiled into the driver; it is packed afresh every run, so
    # a QML file with an older time than the last pack is never missed.
    rm -rf "$build_dir/rcc" "$build_dir"/obj/qrc_*.o
    if ! (cd "$build_dir" &&
        env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS \
            qmake6 FOLIO_REPO="$repo" "$gates_dir/driver/tree-gates.pro" &&
        make -j"$(nproc)") >"$log" 2>&1; then
        echo "the driver did not build (see logs/driver-build.log)"
        return 1
    fi
    if grep -qE '^(CFLAGS|CXXFLAGS|LFLAGS)[[:space:]]*=.*[[:space:]]-g([0-9a-z]*)?([[:space:]]|$)' "$build_dir/Makefile"; then
        echo "the driver's Makefile compiles with -g"
        return 1
    fi
}

# Exports main's src/ into $main_src. Returns 1 when git cannot.
export_main_src() {
    rm -rf "$main_src"
    mkdir -p "$main_src"
    git -C "$repo" archive main src | tar -x -C "$main_src" --strip-components=1
}

remove_main_src() {
    rm -rf "$main_src"
}

# Copies a QML folder to another and gives the list the counting wrapper
# for gate 21 and measure.sh: FolderList.qml's `model: backend.folder` becomes
# `model: gatesCounting.wrap(backend.folder)`. Prints why it could not,
# and returns 1, when the line is not there.
make_counting_copy() {
    local from=$1 to=$2
    rm -rf "$to"
    mkdir -p "$to"
    cp -r "$from/." "$to/"
    if ! grep -qE '^[[:space:]]*model: backend\.folder[[:space:]]*$' "$to/FolderList.qml"; then
        echo "FolderList.qml has no 'model: backend.folder' line to wrap"
        return 1
    fi
    sed -i -E 's/^([[:space:]]*)model: backend\.folder[[:space:]]*$/\1model: gatesCounting.wrap(backend.folder)/' "$to/FolderList.qml"
}

# Gate 2's check on the files the change adds (one per line): prints the
# QML files among them other than src/TreeLines.qml and src/LinesMenu.qml
# (revision 8: the lines menu may live in a file of its own), comma-separated.
extra_qml_files() {
    grep -E '\.qml$' <<<"$1" | grep -vxE 'src/(TreeLines|LinesMenu)\.qml' | sort -u | paste -sd, - || true
}

# Gate 2's check on `git diff main -U0 -- tests/folio_tests.cpp` (revision
# 5): the test file only gains lines, and they form one new test function
# whose name holds "outline" (any case) and whose body calls outline(),
# plus at most one declaration line `void <that name>();` elsewhere.
# Prints one problem per line; nothing when the diff passes (an empty diff
# passes too).
test_file_check() {
    python3 -I -c '
import re
import sys

diff = sys.stdin.read()
problems = []
removed = [l for l in diff.splitlines() if l.startswith("-") and not l.startswith("---")]
if removed:
    problems.append(f"{len(removed)} lines of tests/folio_tests.cpp removed or changed, expected only added lines")
hunks = []
for line in diff.splitlines():
    if line.startswith("@@"):
        hunks.append([])
    elif line.startswith("+") and not line.startswith("+++") and hunks:
        hunks[-1].append(line[1:])
hunks = [h for h in hunks if any(l.strip() for l in h)]
definition = re.compile(r"^\s*void\s+(\w+)\s*\(\s*\)\s*(?:const\s*)?\{?\s*$")
declaration = re.compile(r"^\s*void\s+(\w+)\s*\(\s*\)\s*;\s*$")
bodies, declared, other = [], [], 0
for hunk in hunks:
    code = [l for l in hunk if l.strip() and not l.strip().startswith("//")]
    if len(code) == 1 and declaration.match(code[0]):
        declared.append(declaration.match(code[0]).group(1))
    elif code and definition.match(code[0]):
        bodies.append((definition.match(code[0]).group(1), hunk))
    else:
        other += 1
if other:
    problems.append(f"{other} added blocks in tests/folio_tests.cpp that are not a test function or its declaration")
if len(bodies) > 1:
    problems.append(f"{len(bodies)} test functions added to tests/folio_tests.cpp, expected 1")
for name, hunk in bodies:
    text = "\n".join(hunk)
    names = [m.group(1) for l in hunk for m in [definition.match(l)] if m]
    if len(names) > 1:
        problems.append(f"the added block {name} holds {len(names)} functions, expected 1")
    if "outline" not in name.lower():
        problems.append(f"the added test function {name} has no \"outline\" in its name")
    if not re.search(r"\boutline\s*\(", text):
        problems.append(f"the added test function {name} does not call outline()")
    if text.count("{") != text.count("}"):
        problems.append(f"the added block {name} is not one whole function (braces do not balance)")
for name in declared:
    if not bodies or name != bodies[0][0]:
        problems.append(f"a declaration of {name} added without its test function")
if len(declared) > 1:
    problems.append(f"{len(declared)} declarations added, expected at most 1")
print("\n".join(problems))
' | grep -v '^$' || true
}

# Gate 18's check (revision 5): `result` (parse_test_log's output) against
# `baseline` (main's): one test more passed, the same failed and skipped
# counts and failing names, one passing test with "outline" in its name,
# and the English catalog test passing. Prints one problem per line.
suite_check() {
    local baseline=$1 result=$2 key want got
    want=$(value_of passed "$baseline")
    got=$(value_of passed "$result")
    if [[ ! $want =~ ^[0-9]+$ ]] || [[ $got != "$((want + 1))" ]]; then
        echo "passed ${got:-none}, expected baseline ${want:-none} + 1"
    fi
    for key in failed skipped failing; do
        want=$(value_of "$key" "$baseline")
        got=$(value_of "$key" "$result")
        [[ $got == "$want" ]] || echo "$key ${got:-none}, baseline ${want:-none}"
    done
    got=$(value_of outline "$result")
    if [[ -z $got ]]; then
        echo "no passing test with outline in its name"
    elif [[ $got == *,* ]]; then
        echo "passing tests with outline in their names: $got, expected 1"
    fi
    [[ $(value_of english "$result") == PASS ]] || echo "englishCatalogMatchesTheSources did not pass"
}

# Gate 27 at one display scale (revision 6): the screenshots taken with
# QT_SCALE_FACTOR=<scale> into <dir>, checked as gate 17 checks them and,
# at a whole scale, as gate 23 does. The QML is the caller's
# FOLIO_GATES_QML_DIR, if any. Prints PASS, or the first FAIL line with the
# scale in front.
scaled_result() {
    local scale=$1 dir=$2 tag=$3 result
    rm -rf "$dir"
    mkdir -p "$dir"
    result=$(QT_SCALE_FACTOR=$scale driver_result "$log_dir/$tag-shots-$scale.log" shots "$dir")
    if [[ $result == PASS ]]; then
        result=$(driver_result "$log_dir/$tag-verify-$scale.log" verify-shots "$dir")
    fi
    if [[ $result == PASS && $scale != *.* ]]; then
        result=$(driver_result "$log_dir/$tag-overlays-$scale.log" verify-overlays "$dir")
    fi
    if [[ $result == PASS ]]; then
        echo PASS
    else
        echo "FAIL: scale $scale: ${result#FAIL: }"
    fi
}

# Gate 19's check on the ? overlay (revision 6): HelpOverlay.qml has a row
# whose keys are L and Shift+L, written as the S row is. Succeeds when it
# has.
has_lines_keys_row() {
    grep -qE '\[\[[[:space:]]*"L"[[:space:]]*\],[[:space:]]*\[[[:space:]]*"Shift",[[:space:]]*"L"[[:space:]]*\]\]' "$1"
}

# Gate 3's check on src/TreeLines.qml (revision 7): no state kept only for
# the gates. Outside comments, the root item declares no property named
# guides, dots or route; the object with `id: built` declares no dots or
# routeDots; and nothing refers to built.dots or built.routeDots. Revision 9
# adds that built declares no open and nothing refers to built.open (no
# "a folder may be open" shortcut). Prints one problem per line; nothing
# when the file passes (or is missing).
gate_only_check() {
    python3 -I - "$1" <<'PY'
import re
import sys

try:
    with open(sys.argv[1], encoding="utf-8") as handle:
        text = handle.read()
except OSError:
    sys.exit(0)
text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
text = re.sub(r"//[^\n]*", "", text)
problems = []
# The innermost open brace block at the start of each line; block 0 is the
# root item's.
stack, blocks, next_id = [], [], 0
for line in text.split("\n"):
    blocks.append(stack[-1] if stack else None)
    for char in line:
        if char == "{":
            stack.append(next_id)
            next_id += 1
        elif char == "}" and stack:
            stack.pop()
lines = text.split("\n")
declaration = re.compile(r"^\s*(?:readonly\s+|required\s+|default\s+)*property\s+[\w<>.]+\s+(\w+)\b")
built = {blocks[i] for i, line in enumerate(lines) if re.match(r"^\s*id\s*:\s*built\s*$", line)}
for i, line in enumerate(lines):
    match = declaration.match(line)
    if not match:
        continue
    name = match.group(1)
    if blocks[i] == 0 and name in ("guides", "dots", "route"):
        problems.append(f"TreeLines declares a {name} property on its root item")
    if blocks[i] in built and name in ("dots", "routeDots", "open"):
        problems.append(f"built declares {name}")
for name in sorted(set(re.findall(r"\bbuilt\.(dots|routeDots|open)\b", text))):
    problems.append(f"TreeLines uses built.{name}")
print("\n".join(problems))
PY
}

# Gate 3's check on the lines the change adds: succeeds when they hold a
# pragma ComponentBehavior.
adds_pragma() {
    grep -qE 'pragma[[:space:]]+ComponentBehavior' <<<"$1"
}

# Gate 24's check on one Main.qml, so it can be tried on made-up files:
# outside comments the file has exactly one `Settings {` block, and the last
# entry of that block is the alias of lineStyle. Prints one problem per
# line; nothing when the file passes.
settings_check() {
    python3 -I - "$1" <<'PY'
import re
import sys

try:
    with open(sys.argv[1], encoding="utf-8") as handle:
        text = handle.read()
except OSError as error:
    print(f"cannot read Main.qml: {error.strerror}")
    sys.exit(0)
# Comments out, so a comment naming a Settings block does not count.
code = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
code = re.sub(r"//[^\n]*", "", code)
starts = [m.end() for m in re.finditer(r"\bSettings\s*\{", code)]
if len(starts) != 1:
    print(f"{len(starts)} Settings blocks in src/Main.qml, expected 1")
    sys.exit(0)
depth, at = 1, starts[0]
while at < len(code) and depth:
    depth += {"{": 1, "}": -1}.get(code[at], 0)
    at += 1
body = code[starts[0]:at - 1]
entries = re.findall(r"\bproperty\s+(?:alias|\w+)\s+(\w+)", body)
if not entries:
    print("the Settings block in src/Main.qml has no entries")
elif entries[-1] != "lineStyle":
    print(f"the last entry of the Settings block is {entries[-1]}, expected lineStyle "
          f"(entries: {', '.join(entries)})")
PY
}

# Runs the driver with a time limit; its output is the caller's.
run_driver() {
    timeout 200 "$driver" "$@"
}

# One driver run, its messages to the log given first. Prints PASS, or the
# FAIL line it printed, or a FAIL line saying why there is none.
driver_result() {
    local log=$1 out status=0 last
    shift
    out=$(run_driver "$@" 2>"$log") || status=$?
    last=$(tail -n 1 <<<"$out")
    if ((status == 0)) && [[ $last == PASS ]]; then
        echo PASS
    elif [[ $last == FAIL:* ]]; then
        echo "$last"
    elif ((status == 124)); then
        echo "FAIL: the driver was killed after 200 s"
    else
        echo "FAIL: the driver exited $status without a result (see logs/$(basename "$log"))"
    fi
}
