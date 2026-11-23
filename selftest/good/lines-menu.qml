        // Self-test stand-in for the lines button and its menu (the
        // contract's object names), inserted after the view button of
        // main's PathBar.qml. The menu is made on the button's first click,
        // as the sort menu is (contract revision 8); a stored style it does
        // not know checks "off".
        IconButton {
            id: linesButton
            objectName: "linesButton"
            visible: bar.viewMode === "list"
            glyph: "list"
            text: "Hierarchy lines"
            active: !!linesMenuLoader.item && linesMenuLoader.item.visible
            onClicked: {
                if (linesMenuLoader.item && linesMenuLoader.item.visible) {
                    linesMenuLoader.item.close()
                } else {
                    linesMenuLoader.active = true
                    bar.openMenu(linesMenuLoader.item, linesButton)
                }
            }
            Loader {
                id: linesMenuLoader
                active: false
                visible: false
                sourceComponent: FolioMenu {
                    objectName: "linesMenu"
                    // A press on the button is left to its click, which closes it.
                    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
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
        }
