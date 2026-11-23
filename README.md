# Gates for Folio's draggable pane lines

These are the checks for [omacom/folio#114](https://github.com/omacom/folio/pull/114), kept out of the pull request so it stays small. There are 15 gates. Each has an expected result that was fixed before the code was written, so each one passes or fails and nothing is left to judgement.

## Running them

You need `cmake`, `qmake6` and what Folio itself needs to build. In a checkout of the pull request's branch that also has a local `main`:

```sh
git clone -b pane-lines-gates https://github.com/rustytensor/folio ../folio-gates
../folio-gates/gates.sh
```

It prints one line per gate, `PASS <n> <name>` or `FAIL <n> <name>: <what was seen, against what was expected>`, and exits 0 only when all 15 pass. A run takes a few minutes, because gate 1 rebuilds Folio from clean and gate 13 runs the whole test suite. Logs go to `logs/` beside the script.

Gates 3 to 12 and 15 run `pane-gates`, a small program in `driver/`. It is compiled from Folio's own sources and QML into `build/gates` in the checkout. It opens the real `Main.qml` window offscreen, with a private home folder, and drags the lines with synthetic mouse events. `PANE_GATES_TRACE=1 build/gates/pane-gates <n>` runs one gate and logs each drag.

Gate 13 compares `bin/test` with `baseline.txt`, which I recorded on `main` at `eba4276` on Arch. If the suite gives other numbers on your machine, run `record-baseline.sh` in a checkout of `main` first.

## What is being checked

Three kinds of line between panes can be dragged with the left mouse button:

1. the sidebar's line, which sizes the sidebar,
2. the line between a split's two panes, which sizes the left pane,
3. the line after every column in the columns view, which sizes all the columns together.

A double-click on a line puts its pane back. The widths are kept in the window's `Settings` in `Main.qml`. With nothing dragged, the window is laid out exactly as on `main`.

### Names

Properties on the window in `src/Main.qml`, each kept in its `Settings` under the same key:

| Property | Type | Meaning | Not dragged |
| --- | --- | --- | --- |
| `sidebarWidth` | int | the sidebar's width in px | `0` |
| `columnWidth` | int | the width of every column in px | `0` |
| `splitShare` | real | the left pane's share of the room the two panes have | `0` |

Anything at or below 0, and anything that isn't a number, means "not dragged".

`src/PaneDivider.qml` is the grip on a line: a `MouseArea` with no look of its own and the resize cursor. It has `paneWidth` (the width on screen of the pane before the line), `share` (the part of the pointer's travel the pane grows by), and the signals `dragged(int width)` and `reset()`.

The grips are named `sidebarDivider`, `splitDivider` and `columnDivider`. The driver finds them by those names and reads each pane's width from `paneWidth`.

### Numbers

| Pane | Normal | Least | Most |
| --- | --- | --- | --- |
| Sidebar | 190 px | 140 px | 400 px |
| Column | 260 px | 180 px | 480 px |
| Split, left pane | half of the room | 240 px | the room less 240 px |

- The room of a split is the row's width, less the sidebar and the two 1 px lines. The left pane is `round(share * room)` wide and the right pane has the rest.
- Each pane of a split keeps at least 240 px. When the room is under 480 px there is no such width, and the split is even whatever `splitShare` holds.
- The limits are applied where a stored value is read, so a setting edited by hand shows at the nearest limit.
- A grip is 7 px wide: the line and the first 6 px of the pane after it. It doesn't reach over the pane before the line, where that pane's scroll bar is.
- The split's grip starts below the 60 px top bar.
- A press is a drag once the pointer has moved across by `Qt.styleHints.startDragDistance`. Before that nothing changes.
- Right to left, the pane before a line is on the line's right, so dragging the line left grows the pane.

### Columns while a line is held

- All the columns share one width. For the line to stay under the pointer, the pointer's travel is shared among the columns between the view's starting edge and the grabbed line.
- While a column's line is pressed, a change of the content's width doesn't call `revealEnd()`. The view keeps the number of columns scrolled past its starting edge, as far as it can be scrolled.

## The gates

| # | Group | Gate |
| --- | --- | --- |
| 1 | Standards | `bin/build` exits 0 with no compiler or QML warnings in its output |
| 2 | Standards | The change touches no file under `tests/`, no `.cpp`, `.h` or `CMakeLists.txt`; the sources have no `W` or `Shift+W` shortcut and no `PaneWidths` |
| 3 | Normal | Nothing dragged: sidebar 190, every column 260, split panes equal within 1 px |
| 4 | Normal | Sidebar line dragged +60 gives 250; the first column's line dragged +40 gives 300 for every column; split line dragged +100 grows the left pane by 100 (within 1 px), and the panes and lines still fill the room exactly |
| 5 | Normal | A double-click on each line gives 190, 260 and an even split again, and the stored value is 0 |
| 6 | Normal | A second window opened after a drag has the dragged widths |
| 7 | Normal | In a folder at least 5 columns deep, scrolled back so an earlier column is first in view: dragging a column's line by +30 leaves the scroll offset at (columns scrolled past on press) × (new width + 1) within 1 px, doesn't jump to the end, and the line ends within 2 px of the pointer |
| 8 | Normal | Right to left: dragging each line away from its pane grows it by the travel (sidebar and split within 1 px, first column exactly) |
| 9 | Limits | Dragging far out stops at 140 and 400 (sidebar), 180 and 480 (columns); the split stops with the left pane at 240 px and at the room less 240 px, exactly |
| 10 | Limits | A press that moves less than the drag distance and lets go changes no width and leaves all three values 0 |
| 11 | Bad input | A settings file with `sidebarWidth=99999`, `columnWidth=-5`, `splitShare=abc` opens with sidebar 400, columns 260 and an even split |
| 12 | Bad input | In a column long enough to scroll, pressing on that column's own scroll bar and dragging down scrolls the list and changes no width |
| 13 | Project | `bin/test` has the same passed, failed and skipped counts and the same failing tests as the baseline, and `englishCatalogMatchesTheSources` passes |
| 14 | Project | `README.md` and `HelpOverlay.qml` mention dragging and double-clicking a line; nothing mentions `Shift+W` or a details divider |
| 15 | Limits | Window 720 px wide with the sidebar dragged to 400 (room 318 px): after the split line is dragged +100 and after it is dragged -100, the two panes are equal within 1 px; widened to 1280 px, the left pane is within its 240 px limits again |

No gate checks how wide a grip is. The gates press on the line's own pixel, so how easy a line is to grab is something to try by hand.
