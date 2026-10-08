// Frametop Display Settings (Kirigami). Backend: ft_display_settings.py ("backend").
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: root
    title: "Frametop Display Settings"
    width: Kirigami.Units.gridUnit * 46
    height: Kirigami.Units.gridUnit * 36

    // Pages as tabs across the top (a side drawer was easy to miss).
    readonly property var pages: backend.backend === "screens"
        ? [{ name: "screens", text: "Screens", icon: "video-display", page: screensPage },
           { name: "layout", text: "Layout", icon: "view-grid", page: layoutPage },
           { name: "visibility", text: "Visibility & pins", icon: "view-visible", page: visibilityPage },
           { name: "power", text: "Power", icon: "preferences-system-power-management", page: powerPage }]
        : [{ name: "screens", text: "Screens", icon: "video-display", page: screensPage },
           { name: "layout", text: "Layout", icon: "view-grid", page: layoutPage },
           { name: "power", text: "Power", icon: "preferences-system-power-management", page: powerPage }]

    header: Controls.TabBar {
        id: tabs
        Repeater {
            model: root.pages
            Controls.TabButton {
                required property var modelData
                text: modelData.text
                icon.name: modelData.icon
                onClicked: root.show(modelData.page)
            }
        }
        Component.onCompleted: currentIndex = Math.max(0, root.pages.findIndex(p => p.name === startPage))
    }

    function show(page) {
        pageStack.clear()
        pageStack.push(page)
    }

    // FT_DISPLAY_PAGE=layout|visibility|power opens the app on that page.
    pageStack.initialPage: ({ layout: layoutPage, visibility: visibilityPage, power: powerPage })[startPage] || screensPage

    // "1 hour", "15 minutes", "30 seconds".
    function duration(seconds) {
        const unit = (n, word) => n + " " + word + (n === 1 ? "" : "s")
        if (seconds >= 3600 && seconds % 3600 === 0) return unit(seconds / 3600, "hour")
        if (seconds >= 60 && seconds % 60 === 0) return unit(seconds / 60, "minute")
        return unit(seconds, "second")
    }

    Connections {
        target: backend
        function onMessage(text, isError) {
            root.showPassiveNotification(text, isError ? "long" : "short")
        }
    }

    Kirigami.PromptDialog {
        id: restartDialog
        title: "Restart the desktop?"
        subtitle: "Every window on the desktop closes, this app too. It starts again with the current settings."
        standardButtons: Kirigami.Dialog.NoButton
        customFooterActions: [
            Kirigami.Action {
                text: "Restart"
                icon.name: "view-refresh"
                onTriggered: { restartDialog.close(); backend.restartDesktop() }
            },
            Kirigami.Action {
                text: "Cancel"
                icon.name: "dialog-cancel"
                onTriggered: restartDialog.close()
            }
        ]
    }

    // Save the arrangement under a name, or rename a saved layout.
    Kirigami.PromptDialog {
        id: nameDialog
        property string mode: "save"  // save | rename
        property string oldName: ""
        readonly property var names: backend.layoutNames
        readonly property string name: nameField.text.trim().split(/\s+/).join(" ")
        readonly property bool taken: name !== oldName && names.indexOf(name) >= 0
        readonly property bool ok: name !== "" && !(mode === "rename" && taken)
        title: mode === "save" ? "Save the arrangement" : "Rename " + oldName
        standardButtons: Kirigami.Dialog.NoButton

        function openFor(m, text) {
            mode = m
            oldName = m === "rename" ? text : ""
            nameField.text = text
            open()
            nameField.forceActiveFocus()
            nameField.selectAll()
        }
        function accept() {
            if (!ok) return
            close()
            if (mode === "save") backend.saveLayout(name)
            else if (name !== oldName) backend.renameLayout(oldName, name)
        }

        ColumnLayout {
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: nameDialog.mode === "save"
                      ? "Where the screens are now, with their sizes, curves, and pins, under this name:"
                      : "New name:"
            }
            Controls.TextField {
                id: nameField
                Layout.fillWidth: true
                maximumLength: 40
                onAccepted: nameDialog.accept()
            }
            Controls.Label {
                visible: nameDialog.taken
                opacity: 0.7
                text: nameDialog.mode === "save" ? "Replaces the saved layout with that name."
                                                 : "There's already a layout with that name."
            }
        }
        customFooterActions: [
            Kirigami.Action {
                text: nameDialog.mode === "save" ? "Save" : "Rename"
                icon.name: nameDialog.mode === "save" ? "document-save" : "edit-rename"
                enabled: nameDialog.ok
                onTriggered: nameDialog.accept()
            },
            Kirigami.Action {
                text: "Cancel"
                icon.name: "dialog-cancel"
                onTriggered: nameDialog.close()
            }
        ]
    }

    Kirigami.PromptDialog {
        id: deleteDialog
        property string name: ""
        title: "Delete " + name + "?"
        subtitle: "The screens stay where they are; only the saved layout goes."
        standardButtons: Kirigami.Dialog.NoButton
        customFooterActions: [
            Kirigami.Action {
                text: "Delete"
                icon.name: "edit-delete"
                onTriggered: { deleteDialog.close(); backend.deleteLayout(deleteDialog.name) }
            },
            Kirigami.Action {
                text: "Cancel"
                icon.name: "dialog-cancel"
                onTriggered: deleteDialog.close()
            }
        ]
    }

    // ---------------------------------------------------------------- Screens
    Component {
        id: screensPage
        Kirigami.ScrollablePage {
            id: spage
            title: "Screens"
            property bool md: backend.backend === "screens"

            actions: [
                Kirigami.Action {
                    visible: spage.md
                    text: "Add screen"
                    icon.name: "list-add"
                    onTriggered: backend.addScreen()
                },
                Kirigami.Action {
                    visible: spage.md && backend.desktopRunning
                    text: "Hide/show screens"
                    icon.name: "view-visible"
                    onTriggered: backend.toggleScreens()
                },
                Kirigami.Action {
                    visible: spage.md
                    text: "Remote displays"
                    icon.name: "network-workgroup"
                    tooltip: "Other computers' monitors as screens: Frametop Remote Displays"
                    onTriggered: backend.openRemoteDisplays()
                },
                Kirigami.Action {
                    visible: backend.desktopRunning
                    text: "Restart desktop"
                    icon.name: "view-refresh"
                    tooltip: "Closes the desktop's windows (and this app) and starts it again"
                    onTriggered: restartDialog.open()
                }
            ]

            header: Kirigami.InlineMessage {
                position: Kirigami.InlineMessage.Position.Header
                visible: backend.restartNeeded || !backend.desktopRunning
                type: Kirigami.MessageType.Information
                text: backend.restartNeeded
                      ? (spage.md ? "Screens were added or removed. That applies when the desktop starts again; restarting closes its windows (and this app)."
                                  : "The number of screens, resolution, or panel width changed. They apply when the desktop starts again; restarting closes its windows (and this app).")
                      : "The desktop isn't running. These settings apply the next time it starts."
                actions: [
                    Kirigami.Action {
                        visible: backend.restartNeeded
                        text: "Restart desktop"
                        icon.name: "system-reboot"
                        onTriggered: backend.restartDesktop()
                    }
                ]
            }

            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing

                // ft-screens: every screen its own resolution and size.
                Repeater {
                    model: spage.md ? backend.screenList : []
                    delegate: Kirigami.AbstractCard {
                        id: card
                        required property var modelData
                        Layout.fillWidth: true
                        property int customIndex: backend.screenResolutions.length
                        contentItem: ColumnLayout {
                            RowLayout {
                                Kirigami.Heading { level: 3; text: "Screen " + (card.modelData.index + 1) }
                                Controls.Label {
                                    text: card.modelData.width + " × " + card.modelData.height
                                          + (card.modelData.scale !== 1 ? ", works like " + card.modelData.effective : "")
                                    opacity: 0.7
                                }
                                Item { Layout.fillWidth: true }
                                Controls.RadioButton {
                                    text: "Taskbar here"
                                    checked: card.modelData.primary
                                    onToggled: if (checked) backend.setPrimary(card.modelData.index)
                                }
                                Controls.ToolButton {
                                    icon.name: "edit-delete-remove"
                                    enabled: backend.screenList.length > 1
                                    display: Controls.AbstractButton.IconOnly
                                    text: "Remove this screen"
                                    Controls.ToolTip.text: text
                                    Controls.ToolTip.visible: hovered
                                    onClicked: backend.removeScreen(card.modelData.index)
                                }
                            }
                            Kirigami.FormLayout {
                                Layout.fillWidth: true
                                RowLayout {
                                    Kirigami.FormData.label: "Resolution:"
                                    Controls.ComboBox {
                                        id: res
                                        model: backend.screenResolutions.concat([{ text: "Custom…", width: 0, height: 0 }])
                                        textRole: "text"
                                        Component.onCompleted: {
                                            const i = backend.screenResolutions.findIndex(r => r.width === card.modelData.width && r.height === card.modelData.height)
                                            currentIndex = i >= 0 ? i : card.customIndex
                                        }
                                        onActivated: {
                                            const r = model[currentIndex]
                                            if (r.width > 0) backend.setScreenSize(card.modelData.index, r.width, r.height)
                                        }
                                    }
                                    Controls.SpinBox {
                                        id: cw
                                        visible: res.currentIndex === card.customIndex
                                        from: 320; to: 16384; stepSize: 8; editable: true
                                        value: card.modelData.width
                                    }
                                    Controls.Label { visible: cw.visible; text: "×" }
                                    Controls.SpinBox {
                                        id: ch
                                        visible: cw.visible
                                        from: 200; to: 16384; stepSize: 8; editable: true
                                        value: card.modelData.height
                                    }
                                    Controls.Button {
                                        visible: cw.visible
                                        text: "Set"
                                        onClicked: backend.setScreenSize(card.modelData.index, cw.value, ch.value)
                                    }
                                }
                                RowLayout {
                                    Kirigami.FormData.label: "Width in VR:"
                                    Controls.Slider {
                                        id: metres
                                        from: 0.5; to: 6.0; stepSize: 0.05
                                        value: card.modelData.metres
                                        Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                                        onMoved: backend.setScreenMetres(card.modelData.index, value)
                                    }
                                    Controls.Label {
                                        text: metres.value.toFixed(2) + " m wide, "
                                              + (metres.value * card.modelData.height / card.modelData.width).toFixed(2) + " m tall"
                                    }
                                }
                                Controls.ComboBox {
                                    Kirigami.FormData.label: "Scale:"
                                    model: backend.scales
                                    textRole: "text"
                                    valueRole: "value"
                                    Component.onCompleted: currentIndex = Math.max(0, indexOfValue(card.modelData.scale))
                                    onActivated: backend.setScale(card.modelData.index, currentValue)
                                }
                                Controls.Switch {
                                    Kirigami.FormData.label: "Curved:"
                                    text: "Bend around you"
                                    checked: card.modelData.curved
                                    onToggled: backend.setCurved(card.modelData.index, checked)
                                }
                            }
                        }
                    }
                }

                // gamescope: one shared resolution.
                Kirigami.FormLayout {
                    visible: !spage.md
                    Layout.fillWidth: true

                    Controls.SpinBox {
                        Kirigami.FormData.label: "Screens:"
                        from: 1
                        to: 6
                        value: backend.screens
                        onValueModified: backend.setScreens(value)
                    }

                    RowLayout {
                        Kirigami.FormData.label: "Resolution:"
                        Controls.ComboBox {
                            id: resBox
                            property bool custom: currentIndex === count - 1
                            model: backend.resolutions.concat([{ text: "Custom…", width: 0, height: 0 }])
                            textRole: "text"
                            Component.onCompleted: {
                                const i = backend.resolutions.findIndex(r => r.width === backend.width && r.height === backend.height)
                                currentIndex = i >= 0 ? i : count - 1
                            }
                            onActivated: {
                                const r = model[currentIndex]
                                if (r.width > 0) backend.setResolution(r.width, r.height)
                            }
                        }
                        Controls.SpinBox {
                            id: customW
                            visible: resBox.custom
                            from: 640; to: 7680; stepSize: 8
                            editable: true
                            value: backend.width
                        }
                        Controls.Label { visible: resBox.custom; text: "×" }
                        Controls.SpinBox {
                            id: customH
                            visible: resBox.custom
                            from: 360; to: 4320; stepSize: 8
                            editable: true
                            value: backend.height
                        }
                        Controls.Button {
                            visible: resBox.custom
                            text: "Set"
                            onClicked: backend.setResolution(customW.value, customH.value)
                        }
                    }

                    RowLayout {
                        Kirigami.FormData.label: "Panel width:"
                        Controls.Slider {
                            id: physSlider
                            from: 0.8; to: 3.0; stepSize: 0.05
                            value: backend.physWidth
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                            onMoved: backend.setPhysWidth(value)
                        }
                        Controls.Label { text: physSlider.value.toFixed(2) + " m" }
                    }

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Each screen" }

                    Repeater {
                        model: spage.md ? [] : backend.screenList
                        delegate: RowLayout {
                            required property var modelData
                            Kirigami.FormData.label: "Screen " + (modelData.index + 1) + ":"
                            Controls.ComboBox {
                                model: backend.rotations
                                textRole: "text"
                                valueRole: "value"
                                Component.onCompleted: currentIndex = Math.max(0, indexOfValue(modelData.rotation))
                                onActivated: backend.setRotation(modelData.index, currentValue)
                            }
                            Controls.ComboBox {
                                model: backend.scales
                                textRole: "text"
                                valueRole: "value"
                                Component.onCompleted: currentIndex = Math.max(0, indexOfValue(modelData.scale))
                                onActivated: backend.setScale(modelData.index, currentValue)
                            }
                            Controls.Label {
                                text: "looks like " + modelData.effective
                                opacity: 0.7
                            }
                        }
                    }
                }
            }

            footer: Controls.Label {
                padding: Kirigami.Units.largeSpacing
                wrapMode: Text.Wrap
                opacity: 0.7
                text: spage.md
                      ? "Each screen is a real monitor of its own: any resolution, portrait by choosing a tall one. Resolution, "
                        + "width, and curve apply at once. In VR: move a screen by the bar underneath, curve it with the round "
                        + "button next to the bar, resize it by the tab on its bottom right corner; Save as profile… on the "
                        + "Layout & profiles page keeps all of it."
                      : "gamescope draws every screen at the same resolution, at most 1920 × 1080 worth of pixels. Portrait turns "
                        + "a screen on its side. Rotation and scale apply at once; the rest when the desktop starts."
            }
        }
    }

    // ---------------------------------------------------------------- Layout
    Component {
        id: layoutPage
        Kirigami.ScrollablePage {
            id: lpage
            title: "Layout & profiles"
            property var layout: backend.layout
            property var preset: layout.preset || {}
            property bool hasCustom: (layout.screens || []).some(s => s.pos !== undefined)
            // Named layouts: the arrangement is one of them (named) when it came from it, and
            // hasn't been placed by hand and saved without a name since.
            property var names: backend.layoutNames
            property bool fromNamed: names.indexOf(layout.active) >= 0
            property bool named: layout.mode === "custom" && fromNamed
            property bool unnamed: names.length === 0 || ((hasCustom || layout.mode === "custom") && !fromNamed)
            property var choices: [{ text: "Curved around you", value: "arc" }, { text: "Flat wall", value: "flat" }]
                .concat(names.map(n => ({ text: n, value: "layout:" + n })))
                .concat(unnamed ? [{ text: names.length ? "Unnamed arrangement" : "Saved arrangement", value: "custom" }] : [])

            actions: [
                Kirigami.Action {
                    text: lpage.named ? "Open profile" : "Arrange now"
                    icon.name: "view-restore"
                    tooltip: lpage.named ? "Put the screens in this profile's places, around where you're facing, and open its apps (windows already open move; nothing closes)"
                                         : "Float the screens out of the dashboard and put them in this layout, around where you're facing"
                    enabled: backend.desktopRunning && backend.busy === ""
                    onTriggered: backend.arrange()
                },
                Kirigami.Action {
                    text: "Save as profile…"
                    icon.name: "document-save"
                    tooltip: "Save where the screens are now, which ones are hidden, and the open apps and where their windows are, under a name"
                    enabled: backend.desktopRunning && backend.busy === ""
                    onTriggered: nameDialog.openFor("save", lpage.named ? lpage.layout.active
                                                                        : "Layout " + (lpage.names.length + 1))
                }
            ]

            header: Kirigami.InlineMessage {
                position: Kirigami.InlineMessage.Position.Header
                visible: backend.busy !== ""
                type: Kirigami.MessageType.Information
                text: backend.busy + "… (the pointer is borrowed for a few seconds)"
            }

            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing

                Kirigami.FormLayout {
                    Layout.fillWidth: true

                    RowLayout {
                        Kirigami.FormData.label: "Arrangement:"
                        Controls.ComboBox {
                            model: lpage.choices
                            textRole: "text"
                            valueRole: "value"
                            currentIndex: lpage.layout.mode !== "custom" ? (lpage.preset.kind === "flat" ? 1 : 0)
                                        : lpage.named ? 2 + lpage.names.indexOf(lpage.layout.active)
                                        : lpage.choices.length - 1
                            onActivated: {
                                if (currentValue === "custom") backend.setMode("custom")
                                else if (currentValue.startsWith("layout:")) backend.useLayout(currentValue.slice(7))
                                else backend.setPreset("kind", currentValue)
                            }
                        }
                        Controls.ToolButton {
                            visible: lpage.named
                            icon.name: "edit-rename"
                            text: "Rename…"
                            display: Controls.AbstractButton.IconOnly
                            Controls.ToolTip.text: text
                            Controls.ToolTip.visible: hovered
                            onClicked: nameDialog.openFor("rename", lpage.layout.active)
                        }
                        Controls.ToolButton {
                            visible: lpage.named
                            icon.name: "edit-delete"
                            text: "Delete…"
                            display: Controls.AbstractButton.IconOnly
                            Controls.ToolTip.text: text
                            Controls.ToolTip.visible: hovered
                            onClicked: { deleteDialog.name = lpage.layout.active; deleteDialog.open() }
                        }
                    }

                    Controls.Label {
                        visible: lpage.layout.mode === "custom"
                        Kirigami.FormData.label: ""
                        text: lpage.named ? "Where the screens were when you saved it, and the apps that were open. Open "
                                            + "profile puts the screens there and opens the apps. Save as profile updates "
                                            + "it or saves a new one."
                              : lpage.hasCustom ? "Where the screens were when you saved. Save as profile "
                                                  + "names it. Pick a preset to edit."
                              : "Nothing saved yet: place the screens by hand, open your apps, then Save as profile."
                        opacity: 0.7
                        wrapMode: Text.Wrap
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 20
                    }

                    // The profile's apps (docs/profiles.md): each window and where it goes.
                    ColumnLayout {
                        id: profileApps
                        visible: lpage.named
                        Kirigami.FormData.label: "Apps:"
                        property var windows: lpage.named ? backend.profileWindows(lpage.layout.active) : []
                        property var hidden: lpage.named ? backend.profileHidden(lpage.layout.active) : []
                        Connections {
                            target: backend
                            function onChanged() {
                                profileApps.windows = lpage.named ? backend.profileWindows(lpage.layout.active) : []
                                profileApps.hidden = lpage.named ? backend.profileHidden(lpage.layout.active) : []
                            }
                        }
                        Controls.Label {
                            visible: profileApps.windows.length === 0
                            text: "None saved. Open the apps you want, place their windows, then Save as profile."
                            opacity: 0.7
                            wrapMode: Text.Wrap
                            Layout.maximumWidth: Kirigami.Units.gridUnit * 20
                        }
                        Repeater {
                            model: profileApps.windows
                            delegate: RowLayout {
                                required property var modelData
                                required property int index
                                Controls.Label { text: modelData.app + " (" + modelData.where + ")" }
                                Controls.ToolButton {
                                    icon.name: "list-remove"
                                    text: "Leave out"
                                    display: Controls.AbstractButton.IconOnly
                                    Controls.ToolTip.text: "Leave this window out of the profile"
                                    Controls.ToolTip.visible: hovered
                                    onClicked: backend.removeProfileWindow(lpage.layout.active, index)
                                }
                            }
                        }
                        Controls.Label {
                            visible: profileApps.hidden.length > 0
                            text: "Hides screen" + (profileApps.hidden.length > 1 ? "s " : " ") + profileApps.hidden.join(", ")
                            opacity: 0.7
                        }
                    }

                    Controls.SpinBox {
                        Kirigami.FormData.label: "Rows:"
                        visible: lpage.layout.mode !== "custom"
                        from: 1
                        to: Math.max(1, backend.screens)
                        value: lpage.preset.rows || 1
                        onValueModified: backend.setPreset("rows", value)
                    }

                    Repeater {
                        model: [
                            { key: "distance", label: "Distance", from: 0.6, to: 3.0, step: 0.05, unit: "m", def: 1.2 },
                            { key: "gap", label: "Gap", from: 0.0, to: 0.3, step: 0.01, unit: "m", def: 0.04 },
                            { key: "height", label: "Height", from: -0.8, to: 0.8, step: 0.05, unit: "m", def: 0.0 }
                        ]
                        delegate: RowLayout {
                            required property var modelData
                            visible: lpage.layout.mode !== "custom"
                            Kirigami.FormData.label: modelData.label + ":"
                            Controls.Slider {
                                id: s
                                from: modelData.from; to: modelData.to; stepSize: modelData.step
                                value: lpage.preset[modelData.key] !== undefined ? lpage.preset[modelData.key] : modelData.def
                                Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                                onMoved: backend.setPreset(modelData.key, value)
                            }
                            Controls.Label {
                                text: (modelData.key === "height" && s.value > 0 ? "+" : "") + s.value.toFixed(2) + " " + modelData.unit
                                      + (modelData.key === "height" ? " (from eye level)" : "")
                            }
                        }
                    }

                    Controls.Switch {
                        Kirigami.FormData.label: "When the desktop starts:"
                        text: "Float the screens and arrange them"
                        checked: lpage.layout.auto !== false
                        enabled: backend.defaultProfile === ""
                        onToggled: backend.setAuto(checked)
                    }
                    Controls.ComboBox {
                        Kirigami.FormData.label: "Start in profile:"
                        model: [{ text: "None", value: "" }].concat(lpage.names.map(n => ({ text: n, value: n })))
                        textRole: "text"
                        valueRole: "value"
                        currentIndex: Math.max(0, indexOfValue(backend.defaultProfile))
                        onActivated: backend.setDefaultProfile(currentValue)
                        Controls.ToolTip.text: "The desktop starts in this profile: its screens, and its apps open. Each profile also has its own entry in SteamVR's Launch a program list"
                        Controls.ToolTip.visible: hovered
                    }
                }

                // Preview: from above (you at the bottom) and from the front.
                Kirigami.Heading { level: 3; text: "Preview" }
                Canvas {
                    id: preview
                    Layout.fillWidth: true
                    Layout.preferredHeight: Kirigami.Units.gridUnit * 13
                    property var plan: backend.plan
                    onPlanChanged: requestPaint()
                    onWidthChanged: requestPaint()

                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.reset()
                        const text = Kirigami.Theme.textColor
                        const accent = Kirigami.Theme.highlightColor
                        const half = width / 2
                        ctx.font = Kirigami.Theme.smallFont.pixelSize + "px sans-serif"
                        ctx.fillStyle = text
                        ctx.fillText("From above", 4, 12)
                        ctx.fillText("From the front", half + 8, 12)
                        if (!plan || plan.length === 0) return

                        // From above: x right, forward (-z) up; you are the dot at the bottom.
                        let ends = [[0, 0]]
                        for (const p of plan) {
                            const f = p.faceYaw * Math.PI / 180
                            const rx = Math.cos(f) * p.width / 2, rz = -Math.sin(f) * p.width / 2
                            ends.push([p.x - rx, p.z - rz], [p.x + rx, p.z + rz])
                        }
                        const xs = ends.map(e => e[0]), zs = ends.map(e => e[1])
                        const span = Math.max(Math.max(...xs) - Math.min(...xs), Math.max(...zs) - Math.min(...zs), 0.5)
                        const k = Math.min(half - 20, height - 44) / span
                        const cx = half / 2 - (Math.max(...xs) + Math.min(...xs)) / 2 * k
                        const cz = height - 12 - Math.max(...zs) * k  // your dot 12 px above the bottom
                        const P = (x, z) => [cx + x * k, cz + z * k]
                        ctx.fillStyle = text
                        ctx.beginPath(); const me = P(0, 0); ctx.arc(me[0], me[1], 4, 0, 2 * Math.PI); ctx.fill()
                        ctx.lineWidth = 4
                        ctx.lineCap = "round"
                        // Screens stacked in rows share a spot from above, so they share a label ("1·3"):
                        // the same direction on a curve, the same x on a flat wall.
                        const flat = plan.every(p => Math.abs(p.faceYaw - plan[0].faceYaw) < 0.1
                                                   && Math.abs(p.facePitch - plan[0].facePitch) < 0.1)
                        const labels = {}
                        for (const p of plan) {
                            const f = p.faceYaw * Math.PI / 180
                            const rx = Math.cos(f) * p.width / 2, rz = -Math.sin(f) * p.width / 2
                            const a = P(p.x - rx, p.z - rz), b = P(p.x + rx, p.z + rz)
                            ctx.strokeStyle = accent
                            ctx.beginPath(); ctx.moveTo(a[0], a[1]); ctx.lineTo(b[0], b[1]); ctx.stroke()
                            const c = P(p.x, p.z), spot = flat ? Math.round(p.x * 50) : Math.round(p.faceYaw * 2)
                            labels[spot] = labels[spot] || { at: c, names: [] }
                            labels[spot].names.push(p.index + 1)
                        }
                        ctx.fillStyle = text
                        for (const spot in labels) {
                            const l = labels[spot], t = l.names.join("·")
                            ctx.fillText(t, l.at[0] - ctx.measureText(t).width / 2, l.at[1] - 7)
                        }

                        // From the front: x right, y up. A flat wall as it is; a curved layout
                        // unrolled (arc length by yaw and pitch), so the gaps show true.
                        const front = plan.map(p => {
                            if (flat) return { x: p.x, y: p.y, w: p.width, h: p.height }
                            const r = Math.sqrt(p.x * p.x + p.y * p.y + p.z * p.z)
                            const arc = m => 2 * r * Math.atan(m / 2 / r)  // what the screen spans on the curve
                            return { x: -p.faceYaw * Math.PI / 180 * r, y: p.facePitch * Math.PI / 180 * r,
                                     w: arc(p.width), h: arc(p.height) }
                        })
                        const fx = [], fy = []
                        front.forEach(f => { fx.push(f.x - f.w / 2, f.x + f.w / 2); fy.push(f.y - f.h / 2, f.y + f.h / 2) })
                        fy.push(0)
                        const fspan = Math.max(Math.max(...fx) - Math.min(...fx), Math.max(...fy) - Math.min(...fy), 0.5)
                        const fk = Math.min(half - 20, height - 44) / fspan
                        const ox = half + half / 2 - (Math.max(...fx) + Math.min(...fx)) / 2 * fk
                        const oy = 30 + (height - 42) / 2 + (Math.max(...fy) + Math.min(...fy)) / 2 * fk
                        ctx.strokeStyle = Kirigami.Theme.disabledTextColor
                        ctx.lineWidth = 1
                        ctx.setLineDash([4, 4])
                        ctx.beginPath(); ctx.moveTo(half + 8, oy); ctx.lineTo(width - 4, oy); ctx.stroke()  // eye level
                        ctx.setLineDash([])
                        plan.forEach((p, i) => {
                            const f = front[i], x = ox + (f.x - f.w / 2) * fk, y = oy - (f.y + f.h / 2) * fk
                            ctx.fillStyle = Qt.rgba(accent.r, accent.g, accent.b, 0.35)
                            ctx.fillRect(x, y, f.w * fk, f.h * fk)
                            ctx.strokeStyle = accent
                            ctx.lineWidth = 2
                            ctx.strokeRect(x, y, f.w * fk, f.h * fk)
                            ctx.fillStyle = text
                            ctx.fillText(String(p.index + 1), x + f.w * fk / 2 - 3, y + f.h * fk / 2 + 4)
                        })
                    }
                }

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: "The layout goes around where you're facing when it's applied. Move screens by hand any time "
                          + "(grab bar under each screen); to put them back: Meta+Shift+R in the desktop, the Reset "
                          + "Screen Layout menu entry, Arrange now here, or a mouse button mapped to \"Reset desktop "
                          + "screen layout\" in Frametop Input Settings → Buttons."
                }
            }
        }
    }

    // ---------------------------------------------------------------- Visibility
    Component {
        id: visibilityPage
        Kirigami.ScrollablePage {
            id: vpage
            title: "Visibility"
            property var v: backend.visibility

            actions: [
                Kirigami.Action {
                    text: "Hide/show now"
                    icon.name: "view-visible"
                    enabled: backend.desktopRunning
                    onTriggered: backend.toggleScreens()
                }
            ]

            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing

                Kirigami.FormLayout {
                    Layout.fillWidth: true

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "When the screens show" }

                    Repeater {
                        model: [
                            { value: "always", text: "Always", help: "Meta+Shift+H (or a mapped button) hides them. During VR games, see below." },
                            { value: "dashboard", text: "Only with the SteamVR dashboard open", help: "They come and go with the dashboard. Meta+Shift+H shows them anyway." },
                            { value: "gesture", text: "While I look at my wrist", help: "They show while you look toward the controller below. Meta+Shift+H shows them anyway." },
                            { value: "toggle", text: "Only when I show them", help: "Hidden until Meta+Shift+H (or a mapped button) shows them." }
                        ]
                        delegate: ColumnLayout {
                            required property var modelData
                            spacing: 0
                            Controls.RadioButton {
                                text: modelData.text
                                checked: vpage.v.mode === modelData.value
                                onToggled: if (checked) backend.setVisibility("mode", modelData.value)
                            }
                            Controls.Label {
                                text: modelData.help
                                opacity: 0.7
                                font: Kirigami.Theme.smallFont
                                leftPadding: Kirigami.Units.gridUnit * 1.6
                                wrapMode: Text.Wrap
                                Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                            }
                        }
                    }

                    RowLayout {
                        Kirigami.FormData.label: "Wrist:"
                        visible: vpage.v.mode === "gesture"
                        Controls.ComboBox {
                            model: [{ text: "Left controller", value: "left" }, { text: "Right controller", value: "right" }]
                            textRole: "text"
                            valueRole: "value"
                            Component.onCompleted: currentIndex = indexOfValue(vpage.v.gesture_hand)
                            onActivated: backend.setVisibility("gesture_hand", currentValue)
                        }
                    }
                    RowLayout {
                        Kirigami.FormData.label: "Look within:"
                        visible: vpage.v.mode === "gesture"
                        Controls.Slider {
                            id: gesture
                            from: 5; to: 60; stepSize: 1
                            value: vpage.v.gesture_angle
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                            onMoved: backend.setVisibility("gesture_angle", value)
                        }
                        Controls.Label { text: Math.round(gesture.value) + "° of it" }
                    }

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "During VR games" }

                    Repeater {
                        model: [
                            { value: "hide", text: "Hide them unless the SteamVR dashboard is open", help: "The game has the view to itself; open the dashboard (or press Meta+Shift+H) to see the screens." },
                            { value: "visible", text: "Keep them visible over the game", help: "They float over the game as they are outside it. Turn off Pause while a VR game runs in Frametop Input Settings (Game optimization), or Frametop pauses and hides them anyway." }
                        ]
                        delegate: ColumnLayout {
                            required property var modelData
                            spacing: 0
                            Controls.RadioButton {
                                text: modelData.text
                                enabled: vpage.v.mode === "always"
                                checked: (vpage.v.in_games || "hide") === modelData.value
                                onToggled: if (checked) backend.setVisibility("in_games", modelData.value)
                            }
                            Controls.Label {
                                text: modelData.help
                                opacity: 0.7
                                font: Kirigami.Theme.smallFont
                                leftPadding: Kirigami.Units.gridUnit * 1.6
                                wrapMode: Text.Wrap
                                Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                            }
                        }
                    }
                    Controls.Label {
                        visible: vpage.v.mode !== "always"
                        text: "Applies when the screens show \"Always\"; the other choices above already keep them out of the way."
                        opacity: 0.7
                        font: Kirigami.Theme.smallFont
                        wrapMode: Text.Wrap
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    }

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Controllers on the screens" }

                    Repeater {
                        model: [
                            { value: "outside_games", text: "Except during VR games", help: "Over a VR game the screens stay up, but the controllers stay in the game. Use the 3D mouse, or open the SteamVR dashboard, to work the screens." },
                            { value: "always", text: "Always", help: "Controllers' lasers work the screens whenever they're visible, even over a VR game (which then can't use the controllers)." },
                            { value: "dashboard", text: "Only with the SteamVR dashboard open", help: "Otherwise only the 3D mouse works the screens. Also for flatscreen games, which don't count as VR games." }
                        ]
                        delegate: ColumnLayout {
                            required property var modelData
                            spacing: 0
                            Controls.RadioButton {
                                text: modelData.text
                                checked: (vpage.v.controllers || "outside_games") === modelData.value
                                onToggled: if (checked) backend.setVisibility("controllers", modelData.value)
                            }
                            Controls.Label {
                                text: modelData.help
                                opacity: 0.7
                                font: Kirigami.Theme.smallFont
                                leftPadding: Kirigami.Units.gridUnit * 1.6
                                wrapMode: Text.Wrap
                                Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                            }
                        }
                    }

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Screens shown" }

                    Repeater {
                        model: backend.screensShown
                        delegate: Controls.Switch {
                            required property var modelData
                            required property int index
                            Kirigami.FormData.label: "Screen " + (index + 1) + ":"
                            text: modelData ? "Shown" : "Hidden"
                            checked: modelData
                            onToggled: backend.setScreenShown(index, checked)
                        }
                    }
                    Controls.Label {
                        text: "A hidden screen stays hidden whatever the choices above say, and Meta+Shift+H doesn't bring it back. Windows on it stay there; new ones that would open on it float instead."
                        opacity: 0.7
                        font: Kirigami.Theme.smallFont
                        wrapMode: Text.Wrap
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    }

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Pinned screens" }

                    Repeater {
                        model: backend.pins
                        delegate: Controls.ComboBox {
                            required property var modelData
                            required property int index
                            Kirigami.FormData.label: "Screen " + (index + 1) + ":"
                            model: [
                                { text: "In the room", value: "none" },
                                { text: "On the left wrist", value: "left" },
                                { text: "On the right wrist", value: "right" },
                                { text: "On your head", value: "head" }
                            ]
                            textRole: "text"
                            valueRole: "value"
                            currentIndex: Math.max(0, ["none", "left", "right", "head"].indexOf(modelData))
                            onActivated: backend.pin(String(index + 1), currentValue)
                        }
                    }
                    RowLayout {
                        Kirigami.FormData.label: "Wrist screens show within:"
                        Controls.Slider {
                            id: wrist
                            from: 20; to: 120; stepSize: 1
                            value: vpage.v.wrist_angle
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                            onMoved: backend.setVisibility("wrist_angle", value)
                        }
                        Controls.Label { text: Math.round(wrist.value) + "°" }
                    }
                    RowLayout {
                        Kirigami.FormData.label: "All screens:"
                        Controls.Button {
                            text: "Pin to left wrist"
                            enabled: backend.desktopRunning
                            onClicked: backend.pin("all", "left")
                        }
                        Controls.Button {
                            text: "Pin to right wrist"
                            enabled: backend.desktopRunning
                            onClicked: backend.pin("all", "right")
                        }
                        Controls.Button {
                            text: "Pin to head"
                            enabled: backend.desktopRunning
                            onClicked: backend.pin("all", "head")
                        }
                        Controls.Button {
                            text: "Unpin"
                            enabled: backend.desktopRunning
                            onClicked: backend.pin("all", "none")
                        }
                    }
                }

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: "Pin one screen: carry it by its bar and sweep its laser across your other controller. A "
                          + "ring shows the target and a dot shows where the laser is; crossing the ring arms the pin (ring "
                          + "and bar turn blue), crossing it again disarms it. Turn and place the screen the way you want, "
                          + "then let go: it rides on that wrist at that size and distance, however far away. To adjust a "
                          + "pinned screen, grab its bar, move it, and let go (it stays pinned); sweep across the ring to "
                          + "take it off. It shows while you see its front within the angle above, and fades out beyond "
                          + "it.\n\nPin a screen to your head: choose On your head above. It rides on the headset where it "
                          + "is now, like a HUD, and shows whenever the screens do. Grab its bar to move it; it stays on "
                          + "your head where you let go. Choosing a pin above keeps the screen where it is now, so place "
                          + "it first. Save as profile… (Layout) keeps pins."
                }
            }
        }
    }

    // ---------------------------------------------------------------- Power
    Component {
        id: powerPage
        Kirigami.ScrollablePage {
            id: ppage
            title: "Power"
            property var p: backend.power
            // The timeout choices, plus a value set by hand in frametop.conf.
            property var offChoices: {
                const list = [{ text: "Never", value: 0 }].concat([1, 2, 5, 10, 15, 30, 60].map(
                    m => ({ text: root.duration(m * 60), value: m })))
                if (!list.some(c => c.value === p.offMinutes))
                    list.push({ text: root.duration(Math.round(p.offMinutes * 60)), value: p.offMinutes })
                return list
            }

            Component.onCompleted: backend.refreshPower()

            actions: [
                Kirigami.Action {
                    text: "Turn displays off now"
                    icon.name: "system-suspend"
                    tooltip: "To try it: they come back on when the headset moves or any input is used"
                    enabled: ppage.p.service && ppage.p.state === "on"
                    onTriggered: backend.displaysOffNow()
                }
            ]

            header: Kirigami.InlineMessage {
                position: Kirigami.InlineMessage.Position.Header
                visible: !ppage.p.service
                type: Kirigami.MessageType.Warning
                text: "The power service (frametop-power) isn't running, so the displays won't turn off on their own. "
                      + "It starts with SteamVR once it's installed: power/run.sh install, or run ./install.sh again."
            }

            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing

                Kirigami.FormLayout {
                    Layout.fillWidth: true

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Displays" }

                    Controls.ComboBox {
                        Kirigami.FormData.label: "Turn off when unused for:"
                        model: ppage.offChoices
                        textRole: "text"
                        valueRole: "value"
                        Component.onCompleted: currentIndex = Math.max(0, indexOfValue(ppage.p.offMinutes))
                        onActivated: backend.setDisplayOffMinutes(currentValue)
                    }
                    Controls.Label {
                        text: "Unused means the headset and controllers haven't moved and no mouse, keyboard, or button "
                              + "was used. This works even when the headset seems to be worn, like on a display mount "
                              + "that covers its proximity sensor. Moving the headset or using any input turns the "
                              + "displays back on. Taking the headset off still turns them off within seconds."
                        opacity: 0.7
                        font: Kirigami.Theme.smallFont
                        wrapMode: Text.Wrap
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    }
                    Controls.Label {
                        Kirigami.FormData.label: "Now:"
                        visible: ppage.p.service
                        text: ppage.p.state === "off" ? "Off. Move the headset or use any input to turn them on."
                              : ppage.p.state === "away" ? "Off. SteamVR turned them off because the headset isn't being worn."
                              : ppage.p.offMinutes > 0
                                ? "On, unused for " + (ppage.p.unused < 60 ? Math.floor(ppage.p.unused) + " s"
                                    : Math.floor(ppage.p.unused / 60) + " min " + Math.floor(ppage.p.unused % 60) + " s")
                                : "On"
                    }

                    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Sleep" }

                    Controls.Switch {
                        id: awake
                        Kirigami.FormData.label: "While plugged in:"
                        text: "Stay awake"
                        checked: ppage.p.acSleep === 0
                        enabled: ppage.p.steam && !ppage.p.steamBusy
                        onToggled: {
                            backend.setStayAwake(checked)
                            checked = Qt.binding(() => ppage.p.acSleep === 0)  // follow what Steam has
                        }
                    }
                    Controls.Label {
                        text: !ppage.p.steam
                              ? (ppage.p.steamBusy ? "Checking Steam's setting…" : "Couldn't reach Steam: " + ppage.p.steamError)
                              : "Keeps the Frame awake and connected while it charges, for remote access, downloads, and "
                                + "anything else running. This is Steam's own setting (Settings → Power → When Plugged In "
                                + "and Idle), so the power button still puts the Frame to sleep. "
                                + (ppage.p.acSleep > 0 ? "Now Steam puts it to sleep after " + root.duration(ppage.p.acSleep)
                                                         + " without input, even while it charges. " : "")
                                + "On battery, Steam's battery setting still applies ("
                                + (ppage.p.batterySleep > 0 ? "sleep after " + root.duration(ppage.p.batterySleep) : "never sleep")
                                + ")."
                        opacity: 0.7
                        font: Kirigami.Theme.smallFont
                        wrapMode: Text.Wrap
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    }
                }

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: "With the displays off, the headset keeps tracking and drawing, so it can wake the moment "
                          + "it moves. It still uses most of its power, so leave it on a charger that keeps up with it "
                          + "in use."
                }
            }
        }
    }
}
