import QtQuick
import QtQuick.Shapes

// Self-test stand-in for src/TreeLines.qml (contract revision 9): it
// computes the numbers the contract asks for, straight from its formulas,
// and draws the guides and curves (no dots, no trace route) in one Shape
// with one ShapePath per colour and the style's opacity on the Shape, with
// no layer, so they draw at any display scale. Loaded by selftest/run.sh to
// show that the gates pass on a right answer.
//
// The rows come from one call of the model's outline() per rebuild while a
// style is on and folders open in place, whether or not one is open
// (contract revision 9; the counting wrapper supplies outline() in the
// self-test, as main's FolderModel has none); a folder is open when the next
// row is deeper. In style off, or where folders do not open in place,
// nothing is read. Where the model has no outline() at all (the gates that
// run without the counting copy) the rows are read with get() instead.
Item {
    id: lines
    objectName: "treeLines"

    required property var view

    enabled: false
    z: 2
    width: view.width
    height: view.count * rowHeight

    // The window's lineStyle, where any value but the three styles is off.
    readonly property string style: {
        const chosen = Window.window ? String(Window.window.lineStyle) : "off"
        return ["quiet", "graph", "trace"].indexOf(chosen) >= 0 ? chosen : "off"
    }

    readonly property real zoom: view.zoom
    readonly property int picture: Math.round(20 * zoom)
    readonly property int rowHeight: Math.round(32 * zoom)
    readonly property int indent: Math.round(picture / 2) + 10
    readonly property bool mirrored: LayoutMirroring.enabled
    readonly property var model: view.model

    function leftOf(depth) { return 4 + depth * indent }
    function flip(x) { return mirrored ? width - x : x }
    function centre(row) { return row * rowHeight + rowHeight / 2 }
    // The open arrow's centre: FileRow's arrow item is 32 px high, so in a
    // row lower than that (zoom 0.75) it sits at the row's top, below the
    // row's centre. At zoom 1 and above this is centre(row).
    function arrowCentre(row) { return row * rowHeight + Math.max(rowHeight, 32) / 2 }
    function endOf(row, parentDepth) {
        return flip(rows[row].isDir ? leftOf(parentDepth) + indent + (style === "graph" ? 0 : 2)
                                    : leftOf(parentDepth) + indent + 17)
    }

    // Each row's depth and whether it is a folder.
    property var rows: []

    function rebuild() {
        const count = model ? model.count : 0
        const out = []
        if (style === "off" || !model.expandable) {
            for (let i = 0; i < count; ++i)
                out.push({ depth: 0, isDir: false })
        } else if (typeof model.outline === "function") {
            const outline = model.outline()
            const depths = outline.depths || [], folders = outline.folders || []
            for (let i = 0; i < depths.length; ++i)
                out.push({ depth: depths[i], isDir: !!folders[i] })
        } else {
            for (let i = 0; i < count; ++i) {
                const item = model.get(i)
                out.push({ depth: item.depth, isDir: item.isDir })
            }
        }
        rows = out
    }
    // Changes in one turn of the event loop give one read.
    function schedule() { Qt.callLater(rebuild) }

    onStyleChanged: schedule()
    Component.onCompleted: schedule()
    Connections {
        target: lines.model
        function onModelReset() { lines.schedule() }
        function onLayoutChanged() { lines.schedule() }
        function onRowsInserted() { lines.schedule() }
        function onRowsRemoved() { lines.schedule() }
        function onExpandableChanged() { lines.schedule() }
    }

    readonly property var spec: {
        if (style === "off")
            return null
        if (style === "graph")
            return { width: 1.5, opacity: 0.78, colors: [theme.accent, theme.magenta, theme.cyan, theme.orange], route: null }
        if (style === "quiet")
            return { width: 1, opacity: theme.light ? 0.30 : 0.18, colors: [theme.foreground], route: null }
        return { width: 1, opacity: theme.light ? 0.24 : 0.14, colors: [theme.foreground],
                 route: { width: 1.5, opacity: 0.9, color: theme.accent } }
    }

    readonly property var guideList: {
        if (style === "off" || !model.expandable)
            return []
        const graph = style === "graph"
        const out = []
        for (let i = 0; i < rows.length; ++i) {
            const r = rows[i]
            if (i + 1 >= rows.length || rows[i + 1].depth <= r.depth)
                continue
            const g = { row: i, depth: r.depth, x: flip(leftOf(r.depth) + (graph ? 9.25 : 9.5)), y0: arrowCentre(i) + 6, curves: [] }
            for (let j = i + 1; j < rows.length && rows[j].depth > r.depth; ++j)
                if (rows[j].depth === r.depth + 1)
                    g.curves.push({ row: j, y: centre(j), endX: endOf(j, r.depth) })
            g.y1 = centre(g.curves[g.curves.length - 1].row) - 8
            out.push(g)
        }
        return out
    }

    readonly property var routeRows: {
        const cursor = view.currentIndex
        if (style !== "trace" || cursor < 0 || cursor >= rows.length || rows[cursor].depth === 0)
            return []
        const out = [cursor]
        let row = cursor
        while (rows[row].depth > 0) {
            row = model.parentRow(row)
            out.unshift(row)
        }
        return out
    }

    // Graph's dots, each with the index of its colour.
    readonly property var dotList: {
        const out = []
        if (style === "graph") {
            for (const g of guideList) {
                const color = g.depth % 4
                out.push({ x: g.x, y: g.y0 + 2.5, r: 2, color: color })
                for (const c of g.curves)
                    if (rows[c.row].isDir)
                        out.push({ x: c.endX, y: c.y, r: 2.5, color: color })
            }
        }
        return out
    }
    // Trace's dot at the end of the route.
    readonly property var routeDot: {
        if (style !== "trace" || routeRows.length < 2)
            return null
        const child = routeRows[routeRows.length - 1]
        const parentRow = routeRows[routeRows.length - 2]
        return { x: endOf(child, rows[parentRow].depth), y: centre(child), r: 2.5 }
    }

    // What the drawing is made from, where the gates read it (contract
    // revision 7): the guides, the trace route, and the dots as paths, one
    // per colour, each dot written by circle().
    function circle(x, y, r) {
        return "M" + (x - r) + " " + y + "a" + r + " " + r + " 0 1 0 " + 2 * r + " 0"
            + "a" + r + " " + r + " 0 1 0 " + -2 * r + " 0Z"
    }
    QtObject {
        id: built
        readonly property var guides: lines.guideList
        readonly property var route: lines.routeRows
        readonly property var dotPaths: {
            const out = ["", "", "", ""]
            for (const d of lines.dotList)
                out[d.color] += lines.circle(d.x, d.y, d.r)
            return out
        }
        readonly property string routeDotPath: lines.routeDot ? lines.circle(lines.routeDot.x, lines.routeDot.y, lines.routeDot.r) : ""
    }

    // Each colour's path: a guide and, from it, a quarter circle of radius 8
    // and a level line into each child.
    readonly property var strokes: {
        const out = ["", "", "", ""]
        if (!spec)
            return out
        for (const g of guideList) {
            let path = "M" + g.x + " " + g.y0 + "V" + g.y1
            for (const c of g.curves)
                path += "M" + g.x + " " + (c.y - 8) + "A8 8 0 0 " + (mirrored ? 1 : 0) + " "
                    + (g.x + (mirrored ? -8 : 8)) + " " + c.y + "H" + c.endX
            out[style === "graph" ? g.depth % 4 : 0] += path
        }
        return out
    }
    function strokeColor(k) { return spec ? spec.colors[k % spec.colors.length] : "transparent" }

    Shape {
        visible: lines.spec !== null
        opacity: lines.spec ? lines.spec.opacity : 0
        ShapePath {
            fillColor: "transparent"; strokeColor: lines.strokeColor(0); strokeWidth: lines.spec ? lines.spec.width : 1
            capStyle: ShapePath.FlatCap
            PathSvg { path: lines.strokes[0] }
        }
        ShapePath {
            fillColor: "transparent"; strokeColor: lines.strokeColor(1); strokeWidth: lines.spec ? lines.spec.width : 1
            capStyle: ShapePath.FlatCap
            PathSvg { path: lines.strokes[1] }
        }
        ShapePath {
            fillColor: "transparent"; strokeColor: lines.strokeColor(2); strokeWidth: lines.spec ? lines.spec.width : 1
            capStyle: ShapePath.FlatCap
            PathSvg { path: lines.strokes[2] }
        }
        ShapePath {
            fillColor: "transparent"; strokeColor: lines.strokeColor(3); strokeWidth: lines.spec ? lines.spec.width : 1
            capStyle: ShapePath.FlatCap
            PathSvg { path: lines.strokes[3] }
        }
    }
}
