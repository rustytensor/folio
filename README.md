# Gates for the hierarchy lines in Folio's list view

These are the checks for the pull request that draws hierarchy lines in the list's tree, kept out of the pull request so it stays small. There are 28 gates. Each has an expected result that was fixed before the code was written, so each one passes or fails and nothing is left to judgement.

The comments in the scripts call this file "the contract". Where they say "revision", they mean one of the rounds the contract went through while the code was written.

## Running them

You need `cmake`, `qmake6`, `python3` and what Folio itself needs to build. In a checkout of the pull request's branch that also has a local `main`:

```sh
git clone -b tree-lines-gates https://github.com/rustytensor/folio ../folio-tree-gates
../folio-tree-gates/gates.sh
```

It prints one line per gate, `PASS <n> <name>` or `FAIL <n> <name>: <what was seen, against what was expected>`, and exits 0 only when all 28 pass. A run takes a few minutes, because gate 1 rebuilds Folio from clean in the checkout's `build/` folder and gate 18 runs the whole test suite. Logs go to `logs/` beside the script. `FOLIO_REPO` names another checkout than the current folder.

Most gates run `tree-gates`, a small program in `driver/`. It is compiled from Folio's own sources and QML into `build/gates` in the checkout. It opens the real `Main.qml` window offscreen, with a private home folder, and reads where the lines are. Offscreen there is no GPU, so the window is drawn with Qt's software renderer.

Gate 18 compares `bin/test` with `baseline.txt`, which I recorded on `main` at `eba4276` on Arch. If the suite gives other numbers on your machine, run `record-baseline.sh` in a checkout of `main` first.

Gate 25 measures time, so on a busy machine it can fail by a few milliseconds. Run it again before believing it.

`measure.sh` prints what a change costs in a folder of 10,000 items. `screenshots.sh` takes the six screenshots in `shots/`. `selftest/` runs the gates against stand-ins that must pass and stand-ins that must fail, to show the gates can fail.

## What is being checked

The list view opens folders in place as a tree. With a line style chosen, a guide runs down from each open folder and turns into each of its items with a quarter circle.

- The setting is `lineStyle` on the window in `src/Main.qml`, kept in its one `Settings` block. Its values are `off`, `quiet`, `graph` and `trace`, and the default is `off`. Any other stored value draws no lines and is left as it is.
- A button in the top bar, shown only in the list view, opens a menu with the four choices. The one in effect is checked.
- In the list view, `L` steps to the next style and `Shift+L` to the one before, the way `S` and `Shift+S` step the sort order.
- Both panes of a split follow the one setting.
- With `off`, the list is drawn exactly as on `main`.
- Lines are drawn only where folders open in place: not in a search, Recents or the trash.

### What the gates read from Folio

- Four object names: `treeLines` (the item that draws the lines), `linesButton`, `linesMenu`, and `lines-off`, `lines-quiet`, `lines-graph`, `lines-trace` for the menu's entries.
- The window's `lineStyle`, the menu entries' `checked`, and the `style` and `spec` of `treeLines`, which the drawing itself binds to.
- The object `TreeLines.qml` builds its drawing from: the guides, trace's route, and the dots as the SVG paths that are drawn.
- The calls the lines make on the list's model, counted by a wrapper put in the model's place.

Nothing else in `src/` exists for the gates, and gate 3 checks that.

### Sizes

`picture = round(20 * zoom)`, `rowHeight = round(32 * zoom)`, `indent = round(picture / 2) + 10`, and a row at depth `d` starts at `L(d) = 4 + d * indent`. This is the arithmetic of `FileRow.qml`.

For an open folder at row `i` and depth `d`:

- The guide is at `x = L(d) + 9.5`, under the centre of the folder's arrow. It starts 6 px under the arrow's centre and ends 8 px above the centre line of the folder's last item.
- Each item's curve leaves the guide 8 px above the item's centre line, turns with a quarter circle of radius 8, and runs level to its end.
- The line ends at `L(d) + indent + 2` for a folder (`L(d) + indent` in `graph`, where the dot sits) and at `L(d) + indent + 17` for a file.
- Right to left, every x is mirrored and the curves turn the other way.

### Styles

| | quiet | graph | trace |
| --- | --- | --- | --- |
| Line width | 1 | 1.5 | 1 (route 1.5) |
| Color | the text color | by depth: accent, magenta, cyan, orange, then again | the text color (route: accent) |
| Opacity | 0.18 dark, 0.30 light | 0.78 | 0.14 dark, 0.24 light (route 0.9) |
| Dots | none | radius 2 where a guide starts, radius 2.5 at each folder inside an open folder | radius 2.5 at the end of the route |

Trace's route runs from the top-level folder down to the item the cursor is on. With no cursor, or the cursor on a top-level item, there is no route.

### The tree the gates use

```
assets/                (closed; holds 3 files)
docs/
  api/
    v1/
      schemas/
        order.json
        user.json
      auth.md
      errors.md
    v2/                (closed; holds 2 files)
    overview.md
  guides/
    getting-started.md
    deployment.md
    migrating.md       (the cursor is here)
  README.md
src/                   (closed; holds 2 files)
README.md
```

`docs`, `api`, `v1`, `schemas` and `guides` are open. That is 18 rows, 5 guides and 14 curves.

## The gates

| # | Group | Gate |
| --- | --- | --- |
| 1 | Standards | `bin/build` exits 0 with no compiler or QML warnings in its output |
| 2 | Standards | The change touches only `src/*.qml`, `src/resources.qrc`, `src/i18n/en.json`, `README.md`, `src/foldermodel.h`, `src/foldermodel.cpp` and `tests/folio_tests.cpp`. The only QML file added is `src/TreeLines.qml`. The test file only gains one whole test function with "outline" in its name |
| 3 | Standards | `qmllint` reports nothing on the changed QML files that it doesn't report on `main`. The added text has no `pragma ComponentBehavior`. `src/TreeLines.qml` keeps no state only for the gates |
| 4 | Normal | With `off` (the default) there are no guides and no dots, and a screenshot of the list is pixel-identical to one taken with `main`'s QML |
| 5 | Normal | In each style the tree above gives 5 guides and 14 curves, and every position is within 0.5 px of the sizes above |
| 6 | Normal | Each style's width, colors and opacity are the table's, in a dark and a light theme |
| 7 | Normal | Dots: quiet 0, graph 10, trace 1 at the end of the route |
| 8 | Normal | Trace: with the cursor on `migrating.md` the route is docs, guides, migrating.md. On `order.json` it is docs, api, v1, schemas, order.json. On a top-level item, or with no cursor, it is empty |
| 9 | Normal | Closing `v1` removes its guide and curves, and the guides below move up by exactly its rows' height. Opening it brings them back |
| 10 | Normal | Choosing a style in the menu sets `lineStyle` and checks that entry. A second window opened after has the same style, and the settings file holds it |
| 11 | Normal | The button is visible in the list view and hidden in grid and columns |
| 12 | Limits | At zoom 1.5 the positions follow the sizes above |
| 13 | Limits | A tree 8 levels deep with 2,000 rows has its guides within 1 s, with no error |
| 14 | Limits | In a search, in Recents and in the trash there are no guides |
| 15 | Bad input | A settings file with `lineStyle=banana`, then `lineStyle=7`, then an empty value: no lines are drawn, only "No lines" is checked, and the file still holds the stored text afterwards |
| 16 | Right to left | The guides' and curves' x are mirrored and each curve turns the other way |
| 17 | Screenshots | `screenshots.sh` writes six PNGs of the window, 1280 x 800, none blank. In each, the pixel on a chosen guide has the line's color at the style's opacity, and the same pixel with `off` does not |
| 18 | Project | `bin/test` passes one test more than the baseline, with the same failed and skipped counts and the same failing tests. One passing test has "outline" in its name, and `englishCatalogMatchesTheSources` passes |
| 19 | Docs | `README.md` mentions the hierarchy lines and `Shift+L`. `HelpOverlay.qml` has a row with the keys `L` and `Shift+L`. Every new `qsTr` text is in `en.json` |
| 20 | Limits | At zoom 0.75, 1, 1.25, 1.5 and 2, measured against the real arrow and picture items of the rows: a guide starts 4 to 8 px under the open folder's arrow, a folder's line ends 2 to 6 px short of its arrow, a file's line ends 3 to 8 px short of its picture, and in graph a folder's dot is at least 1 px clear of the arrow |
| 21 | Limits | With no folder open, a reset, a row change and a width change right to left call neither `get()` nor `outline()` on the model. With 10,000 rows and nothing open a rebuild takes under 50 ms. With 10,000 rows and one folder of 50 items open, one inserted row calls `get()` 0 times and `outline()` at most once, and the guides are right afterwards |
| 22 | Limits | Dots are paths, not items: opening a folder with 200 folders in it gives at least 200 dots in graph, and the number of items under `treeLines` is the same as with a folder of 2 files open |
| 23 | Screenshots | Over the selected row, a line's pixel is that row's tint blended with the line's color at the style's opacity, within 10 per channel. The pixel where a curve joins its guide equals the plain guide's within 4 per channel |
| 24 | Standards | `src/Main.qml` has exactly one `Settings` block, and `lineStyle` is its last entry |
| 25 | Limits | In 10,000 rows with one folder open, the median CPU time of an insert with a style on is at most 20 ms above the median with `off`, for each style |
| 26 | Normal | In the list, `L` from off gives quiet, graph, trace, off, and `Shift+L` goes the other way, with the matching menu entry checked. From an unknown stored style, `L` gives quiet and `Shift+L` gives trace. In grid and columns the keys change nothing. `Ctrl+L` still opens the path field, and in the path field and the search field the keys type letters |
| 27 | Screenshots | With the window drawn at display scales 1.5 and 2, gate 17's check holds in every style, and at scale 2 gate 23's two checks hold as well |
| 28 | Limits | With nothing open, `docs` is opened and the folder re-read at once, before the opening's own listing arrives. Once the rows have settled there is exactly one guide, with the right positions, in each style |

Gate 14 is weak, and I know it: Folio's model never holds an open folder where folders don't open in place, so the gate can't set up a state in which code that ignored this would draw.
