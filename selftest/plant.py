"""Inserts a snippet file into a QML file for the self-test's stand-ins.

Usage: plant.py FILE LINE SNIPPET AFTER

The snippet goes AFTER lines below the one line of FILE that is exactly
LINE. Exits non-zero when LINE is not in FILE exactly once, so a change to
main's QML that moves the planting point shows up instead of being missed.
"""

import sys


def main() -> int:
    """Plants the snippet; returns the exit status."""
    path, line, snippet, after = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
    with open(path, encoding="utf-8") as handle:
        rows = handle.read().split("\n")
    found = [i for i, row in enumerate(rows) if row == line]
    if len(found) != 1:
        print(f"{path}: {len(found)} lines match the planting point, expected 1", file=sys.stderr)
        return 1
    with open(snippet, encoding="utf-8") as handle:
        planted = handle.read().rstrip("\n").split("\n")
    at = found[0] + after + 1
    rows[at:at] = planted
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
