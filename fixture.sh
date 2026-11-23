#!/usr/bin/env bash
# Builds the folder trees the hierarchy line gates open, inside a folder
# given on the command line (made if missing, never emptied first).
#
#   fixture.sh tree DIR    the contract's fixture tree: DIR/project/...
#   fixture.sh deep DIR    8 folders nested in DIR/deep, 2000 rows when all open
#   fixture.sh trash DIR   a trashed folder with items, under DIR/Trash
#   fixture.sh big DIR     10,000 files in DIR/big (gate 21)
#   fixture.sh wide DIR    DIR/wide/few (2 files), DIR/wide/many (200 folders)
#   fixture.sh bigtree DIR DIR/bigtree: 10,000 rows with open/ (50 files) open
#
# Files hold a few bytes and every item gets the same fixed modification
# time, so the list's size and date columns draw the same on every run.
set -euo pipefail

# Every item's modification time (any fixed date in the past will do).
readonly stamp='2024-03-01 12:00:00'

usage() {
    echo "usage: fixture.sh tree|deep|trash|big|wide|bigtree DIR" >&2
    exit 2
}

# A file with a few bytes in it.
put() {
    mkdir -p "$(dirname "$1")"
    printf 'fixture: %s\n' "$(basename "$1")" >"$1"
}

# The contract's tree. The list opens DIR/project; docs, api, v1, schemas
# and guides are opened in place by the driver.
tree() {
    local p=$1/project
    put "$p/assets/a.txt"
    put "$p/assets/b.txt"
    put "$p/assets/c.txt"
    put "$p/docs/api/v1/schemas/order.json"
    put "$p/docs/api/v1/schemas/user.json"
    put "$p/docs/api/v1/auth.md"
    put "$p/docs/api/v1/errors.md"
    put "$p/docs/api/v2/a.md"
    put "$p/docs/api/v2/b.md"
    put "$p/docs/api/overview.md"
    put "$p/docs/guides/getting-started.md"
    put "$p/docs/guides/deployment.md"
    put "$p/docs/guides/migrating.md"
    put "$p/docs/README.md"
    put "$p/src/main.c"
    put "$p/src/util.c"
    put "$p/README.md"
    find "$p" -exec touch -h -d "$stamp" {} +
}

# DIR/deep/l1/l2/.../l8: each level folder holds the next one (none in l8)
# and 249 files. Listing DIR/deep with every level open shows the 8 level
# folders and 8 * 249 files: 2000 rows, the deepest at depth 8.
deep() {
    local folder=$1/deep level i
    for level in 1 2 3 4 5 6 7 8; do
        folder=$folder/l$level
        mkdir -p "$folder"
        for ((i = 1; i <= 249; i++)); do
            printf 'x\n' >"$(printf '%s/f%03d.txt' "$folder" "$i")"
        done
    done
    find "$1/deep" -exec touch -h -d "$stamp" {} +
}

# A freedesktop trash with one folder that holds items: DIR/Trash/files/old
# and its note in DIR/Trash/info.
trash() {
    local t=$1/Trash
    put "$t/files/old/inner/deep.txt"
    put "$t/files/old/note.txt"
    mkdir -p "$t/info"
    printf '[Trash Info]\nPath=/tmp/old\nDeletionDate=2024-03-01T12:00:00\n' >"$t/info/old.trashinfo"
    find "$t" -exec touch -h -d "$stamp" {} +
}

# DIR/big with 10,000 small files and no folder.
big() {
    local folder=$1/big i
    mkdir -p "$folder"
    for ((i = 0; i < 10000; i++)); do
        printf 'x\n' >"$(printf '%s/f%05d.txt' "$folder" "$i")"
    done
    find "$folder" -exec touch -h -d "$stamp" {} +
}

# DIR/wide for gate 22: few/ holds 2 files (1 dot in graph when open) and
# many/ 200 empty folders (201 dots when open).
wide() {
    local w=$1/wide i
    put "$w/few/a.txt"
    put "$w/few/b.txt"
    for ((i = 1; i <= 200; i++)); do
        mkdir -p "$(printf '%s/many/d%03d' "$w" "$i")"
    done
    find "$w" -exec touch -h -d "$stamp" {} +
}

# DIR/bigtree for measure.sh: the folder open/ with 50 files, and 9,949
# files beside it; with open/ open that is 10,000 rows.
bigtree() {
    local b=$1/bigtree i
    mkdir -p "$b/open"
    for ((i = 0; i < 50; i++)); do
        printf 'x\n' >"$(printf '%s/open/f%02d.txt' "$b" "$i")"
    done
    for ((i = 0; i < 9949; i++)); do
        printf 'x\n' >"$(printf '%s/t%04d.txt' "$b" "$i")"
    done
    find "$b" -exec touch -h -d "$stamp" {} +
}

(($# == 2)) || usage
mkdir -p "$2"
case $1 in
    tree) tree "$2" ;;
    deep) deep "$2" ;;
    trash) trash "$2" ;;
    big) big "$2" ;;
    wide) wide "$2" ;;
    bigtree) bigtree "$2" ;;
    *) usage ;;
esac
