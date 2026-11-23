    // Self-test stand-in: L and Shift+L step the window's line style in the
    // menu's order while the list has the keys; a style it does not know
    // steps as off. Inserted after Shift+S in main's FolderPane.qml.
    function stepLines(by) {
        const order = ["off", "quiet", "graph", "trace"]
        const at = Math.max(0, order.indexOf(String(pane.host.lineStyle)))
        pane.host.lineStyle = order[(at + by + order.length) % order.length]
    }
    Shortcut { sequence: "L"; enabled: pane.keys && pane.viewMode === "list"; onActivated: pane.stepLines(1) }
    Shortcut { sequence: "Shift+L"; enabled: pane.keys && pane.viewMode === "list"; onActivated: pane.stepLines(-1) }
