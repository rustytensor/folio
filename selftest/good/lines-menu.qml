        // Self-test stand-in for the lines button and its menu (the
        // contract's object names), inserted after the view button of
        // main's PathBar.qml. A stored style it does not know checks "off".
        IconButton {
            id: linesButton
            objectName: "linesButton"
            visible: bar.viewMode === "list"
            glyph: "list"
            text: "Hierarchy lines"
            active: linesMenu.visible
            onClicked: linesMenu.visible ? linesMenu.close() : bar.openMenu(linesMenu, linesButton)
            FolioMenu {
                id: linesMenu
                objectName: "linesMenu"
                Repeater {
                    model: ["off", "quiet", "graph", "trace"]
                    FolioMenuItem {
                        required property string modelData
                        readonly property string stored: ApplicationWindow.window ? String(ApplicationWindow.window.lineStyle) : "off"
                        objectName: "lines-" + modelData
                        text: modelData
                        checked: (["quiet", "graph", "trace"].indexOf(stored) >= 0 ? stored : "off") === modelData
                        onTriggered: ApplicationWindow.window.lineStyle = modelData
                    }
                }
            }
        }
