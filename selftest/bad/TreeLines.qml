import QtQuick

// Self-test stand-in for src/TreeLines.qml that is wrong on purpose, loaded
// by selftest/run.sh to show that gates 4, 5, 20, 21 and 22 report a wrong
// answer:
//   - in style off it computes nothing but still draws a 1 px line down the
//     list, so the list no longer looks as on main (gate 4);
//   - each guide starts at row top + 20, 2 px too high (gate 5);
//   - the curves end at the first version's fixed L + 22, L + 20 and
//     L + 37, right only at zoom 1, so at zoom 2 they stop short of the
//     arrows and pictures (gate 20);
//   - every change to the model re-reads every row with get(), even with
//     no folder open (gate 21), so a row inserted in 10,000 costs far more
//     with a style on than off (gate 25);
//   - graph's dots are one Rectangle each (gate 22).
Item {
    id: lines
    objectName: "treeLines"

    required property var view

    enabled: false
    z: 2
    width: view.width
    height: view.count * rowHeight

    readonly property string style: {
        const chosen = Window.window ? String(Window.window.lineStyle) : "off"
        return ["quiet", "graph", "trace"].indexOf(chosen) >= 0 ? chosen : "off"
    }

    // Wrong on purpose: bumped on every model change, and every bump
    // re-reads all rows.
    property int revision: 0
    Connections {
        target: lines.view.model
        function onRowsInserted() { lines.revision++ }
        function onRowsRemoved() { lines.revision++ }
        function onModelReset() { lines.revision++ }
        function onDataChanged() { lines.revision++ }
        function onLayoutChanged() { lines.revision++ }
    }

    readonly property real zoom: view.zoom
    readonly property int picture: Math.round(20 * zoom)
    readonly property int rowHeight: Math.round(32 * zoom)
    readonly property int indent: Math.round(picture / 2) + 10
    readonly property bool mirrored: LayoutMirroring.enabled

    function leftOf(depth) { return 4 + depth * indent }
    function flip(x) { return mirrored ? width - x : x }
    function centre(row) { return row * rowHeight + rowHeight / 2 }
    function endOf(row, parentDepth) {
        const l = leftOf(parentDepth)
        // Wrong on purpose: fixed numbers, right only at zoom 1.
        return flip(rows[row].isDir ? l + (style === "graph" ? 20 : 22) : l + 37)
    }

    readonly property var rows: {
        revision
        const model = view.model
        const out = []
        if (style === "off")
            return out
        for (let i = 0; i < model.count; ++i) {
            const item = model.get(i)
            out.push({ depth: item.depth, isDir: item.isDir, expanded: item.expanded })
        }
        return out
    }

    readonly property var spec: style === "off" ? null
        : { width: style === "graph" ? 1.5 : 1, opacity: 0.5, colors: [theme.foreground], route: null }

    readonly property var guideList: {
        if (style === "off" || !view.model.expandable)
            return []
        const graph = style === "graph"
        const out = []
        for (let i = 0; i < rows.length; ++i) {
            const r = rows[i]
            if (!r.expanded || i + 1 >= rows.length || rows[i + 1].depth <= r.depth)
                continue
            // Wrong on purpose: 20 below the row's top, not its centre + 6.
            const g = { row: i, depth: r.depth, x: flip(leftOf(r.depth) + (graph ? 9.25 : 9.5)), y0: i * rowHeight + 20, curves: [] }
            for (let j = i + 1; j < rows.length && rows[j].depth > r.depth; ++j)
                if (rows[j].depth === r.depth + 1)
                    g.curves.push({ row: j, y: centre(j), endX: endOf(j, r.depth) })
            g.y1 = centre(g.curves[g.curves.length - 1].row) - 8
            out.push(g)
        }
        return out
    }

    readonly property var routeRows: []
    readonly property var routeDot: null
    readonly property var dotList: {
        const out = []
        if (style === "graph")
            for (const g of guideList) {
                out.push({ x: g.x, y: g.y0 + 2.5, r: 2, color: 0 })
                for (const c of g.curves)
                    if (rows[c.row].isDir)
                        out.push({ x: c.endX, y: c.y, r: 2.5, color: 0 })
            }
        return out
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

    // Wrong on purpose (gate 22): one item per dot.
    Repeater {
        model: lines.dotList
        Rectangle {
            required property var modelData
            x: modelData.x - modelData.r
            y: modelData.y - modelData.r
            width: 2 * modelData.r
            height: width
            radius: modelData.r
            color: theme.accent
        }
    }

    // Wrong on purpose: a line drawn in style off.
    Rectangle {
        visible: lines.style === "off"
        x: 13
        width: 1
        height: lines.height
        color: theme.foreground
    }
}
