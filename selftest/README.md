# Self-test of the gates (contract revision 7)

`run.sh` shows that the gates tell a right answer from a wrong one without
depending on the code under test. It exports main's `src/` with `git archive`
(into the checkout's `build/gates`, removed afterwards), and the driver loads
that QML instead of its compiled-in resources (`FOLIO_GATES_QML_DIR`). The C++
is the same as on the branch, since the contract allows no C++ change. For
gate 21 each QML set is copied once more with FolderList.qml's
`model: backend.folder` replaced by the driver's counting wrapper (an identity
proxy over Folio's model that counts the calls of `get()`), as gates.sh does
with the checkout's QML.

| QML | What it is | Expected | Seen |
| --- | --- | --- | --- |
| `good/` | main, plus the window's `lineStyle` (and its setting) and a stand-in `TreeLines.qml` that computes revision 2's numbers and `spec`, keeps a row cache updated from the model's signals (it reads a row only when the row is new or opens or closes, and nothing while no folder is open), and draws nothing | gates 4 to 9, 12 to 14, 16, 20 and 21 pass | all pass |
| `bad/` | revision 1's fixed numbers (`L + 22`, `L + 20`, `L + 37`), each guide starting 2 px high, a full re-read of every row on every model change, and a line drawn in style off | gates 4, 5, 20 and 21 fail | 4: `576 pixels differ from main's list`; 5: `guide of docs (row 1) y0 52, expected 54`; 20: `zoom 1.25, quiet: the line to api ends 7 px short of its arrow, expected 2 to 6` (96 problems); 21: `a model reset with no folder open called get() 4 times, expected 0` |
| main as is | no lines at all (the code missing) | gates 4 to 17, 20 and 21 fail with a message, no crash, no hang | all fail, each with one readable line |
| `bad/` (revision 4) | the same, plus one `Rectangle` per dot (and graph's guide dots, so the dot counts are right) | gate 22 fails | `treeLines has 203 items under it with 201 dots and 3 with 1 dot` |
| gates 2, 3 and 24 on made-up inputs | a second added QML file, a `pragma ComponentBehavior`, main's `Main.qml` with `lineStyle` in a second `Settings` block or before `zoom`, against the right ones | the wrong ones fail, the right ones pass | `src/LinesMenu.qml`; `pragma ComponentBehavior added`; `2 Settings blocks in src/Main.qml, expected 1`; `the last entry of the Settings block is zoom, expected lineStyle` |

Revision 5 cases (`selftest/run.sh 2 18 21 25`, last run in
`../logs/selftest-rev5.txt`). main's C++ has no `FolderModel::outline()`, so the
counting wrapper stands in for it from two roles when `FOLIO_GATES_OUTLINE_FALLBACK`
is set, which only this script does:

| Case | Expected | Seen |
| --- | --- | --- |
| `good/` now reads rows with one `outline()` per rebuild | gates 21 and 25 pass | both pass |
| `bad/` re-reads every row with `get()` on every change while a style is on | gates 21 and 25 fail | 21: `a model reset with no folder open called get() 4 times`; 25: `quiet 466.9 ms, graph 473.9 ms, trace 459.2 ms (off 50.8 ms)` |
| main's QML | gates 21 and 25 fail cleanly | `no item named treeLines`; `the window has no lineStyle property` |
| gate 2's test file check on made-up diffs | an empty diff, one `outline` test, and the test with its declaration pass; a removed line, two tests, a helper outside a test, and a test without "outline" in its name fail | as expected |
| gate 18's check on made-up results against passed 278 | 279 with one `outline` test passes; 278, 279 without an `outline` test, and 280 with two fail | as expected |

Revision 6 cases (`selftest/run.sh 19 26 27` in `../logs/selftest-rev6.txt`, then
`selftest/run.sh 21 26 28` in `../logs/selftest-rev6b.txt` after the changes below):

| Case | Expected | Seen |
| --- | --- | --- |
| `good/` with `good/lines-menu.qml` planted after the view button of main's `PathBar.qml` and `good/lines-keys.qml` after Shift+S in main's `FolderPane.qml` (by `plant.py`, which fails if the planting line moved) | gate 26 passes | passes |
| `good/TreeLines.qml` now draws its guides and curves in one `Shape`, one `ShapePath` per colour, opacity on the `Shape`, no layer | gate 27 passes at 1.5 and 2 | passes |
| `good/TreeLines.qml` keeps reading while a folder marked open has no items listed yet (`pendingRow`), so items that come in a reset are read | gate 28 passes, gate 21 still passes | both pass |
| `bad/` (no keys, no lines drawn) | gates 26, 27 and 28 fail | 26: `L from off in the list gave lineStyle off`; 27: `scale 1.5: quiet-dark.png at (366, 474) is #1a1b26, expected #343646`; 28: `guide of docs (row 1) y0 52, expected 54` |
| main's QML | gates 26, 27 and 28 fail cleanly | `lineStyle is  on a first launch, expected off`; `the window has no lineStyle property` (27 and 28) |
| gate 19's overlay check on made-up rows | an `[["L"], ["Shift", "L"]]` row passes; none, or L alone, fails | as expected |

The first run of 26 on the good stand-in failed on the driver, not the stand-in: Ctrl+L
selects the path field's text, so typed letters replace it; the check now asks that the
text ends with the two letters.

Revision 7 cases (`selftest/run.sh 3 5 7 8 13 22`, in `../logs/selftest-rev7.txt`):
both stand-ins keep their guides, route and dot paths in a `QtObject` child (`id:
built`) and no longer declare `guides`, `dots` or `route` on the item; the driver reads
them from there and parses the dots from the paths. Good passes 5, 7, 8, 13 and 22; bad
fails 5 and 22 as before; main's QML fails cleanly. Gate 3's no-gate-only-state check on
made-up files passes a clean one (with a `dots` property inside a `component`, and a
commented-out declaration, both allowed) and fails each of a root `guides`, `route` or
`dots`, and a `dots` or `routeDots` in `built`.

What this proves:

- Gate 20 measures the lines against FileRow's real arrow glyph (its centre,
  less 3 for its visible edge) and its item named `picture`, mapped into the
  list, at zoom 0.75, 1, 1.25, 1.5 and 2. It passes a stand-in that ties the
  lines to the indent and fails one with revision 1's fixed numbers.
- Gate 21 counts real calls of `get()` through the list's model: it passes a
  stand-in that reads only what changed and fails one that walks every row.
- Gates 5, 6, 12 and 16 accept revision 2's numbers and `spec`.
- Gate 22 (revision 4) counts the items under `treeLines` with 1 and with
  201 dots: it passes a stand-in that makes no item per dot and fails one
  that does. Gates 2, 3 and 24's static checks each trip on their own.

Gate 15 is not tried on the stand-ins: it checks which entry of the lines
menu is checked (revision 4: only "No lines"), and only the real `PathBar.qml`
has that menu; main's QML fails it cleanly, and against the code under test
it fails on the checked entry (see `../logs/gates-run.txt`). Gates 1,
2, 3, 18, 19 and 23 are not run against a planted error (23 reads screenshots
of the real window).

Found while doing this: at zoom 0.75 the row is 24 px but FileRow's arrow item
is a fixed 32 px, so the arrow's centre is 4 px below the row's centre. The
contract's `y0 = cy(i) + 6` is then only 2 px under the arrow, where gate 20
asks for 4 to 8. The good stand-in starts 6 px under the real arrow centre
(the same as `cy(i) + 6` from zoom 1 up).

The last full run is in `../logs/selftest.txt`; the revision 4 cases alone
(`selftest/run.sh 2 3 15 22 24`) are in `../logs/selftest-rev4.txt`. Run it
again with `selftest/run.sh [gate ...]` (it builds the driver first and exits
0 only when every result is the expected one).
