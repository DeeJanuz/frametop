// Frametop Remote Displays (Kirigami). Backend: ft_remote_displays.py ("backend").
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: root
    title: "Frametop Remote Displays"
    width: Kirigami.Units.gridUnit * 42
    height: Kirigami.Units.gridUnit * 34

    pageStack.initialPage: hostsPage

    Connections {
        target: backend
        function onMessage(text, isError) {
            root.showPassiveNotification(text, isError ? "long" : "short")
        }
    }

    // What a stream is doing, in words (ft-screens' state, or ours for a disconnected one).
    function stateText(state) {
        const words = { live: "streaming", connecting: "connecting", starting: "starting", queued: "waiting its turn",
                        lost: "lost, trying again", disconnected: "disconnected",
                        "desktop off": "connects when the Frametop desktop starts", "not running": "not running" }
        if (state in words) return words[state]
        if (state.startsWith("live via ")) return "streaming over the " + state.slice(9)
        if (state.startsWith("lost ")) return "lost (" + state.slice(5) + "), trying again"
        return state
    }
    function stateColor(state) {
        if (state.startsWith("live")) return Kirigami.Theme.positiveTextColor
        if (state.startsWith("lost")) return Kirigami.Theme.negativeTextColor
        if (state === "connecting" || state === "starting" || state === "queued") return Kirigami.Theme.neutralTextColor
        return Kirigami.Theme.disabledTextColor
    }

    footer: Controls.ToolBar {
        visible: backend.busy !== ""
        RowLayout {
            anchors.fill: parent
            Controls.BusyIndicator { running: backend.busy !== ""; Layout.preferredHeight: Kirigami.Units.iconSizes.medium }
            Controls.Label { text: backend.busy + "…"; Layout.fillWidth: true }
        }
    }

    // ---------------------------------------------------------------- dialogs
    // A host: picked from the computers found on the network (or typed in), then a sign-in
    // with its Vibepollo Web UI login, for Frametop's own API token. Also signs in again to
    // one already added (openFor).
    Kirigami.PromptDialog {
        id: hostDialog
        objectName: "hostDialog"
        title: existing ? "Sign in to " + fixedName : "Add a computer"
        standardButtons: Kirigami.Dialog.NoButton
        property bool existing: false
        property string fixedName: ""
        property string fixedAddress: ""
        property string fixedDongle: ""
        property var found: []
        property bool searching: false
        property bool working: false
        property string error: ""
        property int picked: -1  // index in found; found.length: typed in
        readonly property bool typed: picked === found.length
        readonly property string pickedName: existing ? fixedName
            : typed ? (otherName.text.trim() || otherAddress.text.trim()) : picked >= 0 ? found[picked].name : ""
        readonly property string pickedAddress: existing ? fixedAddress
            : typed ? otherAddress.text.trim() : picked >= 0 ? found[picked].address : ""
        readonly property string pickedDongle: existing ? fixedDongle : !typed && picked >= 0 ? found[picked].dongle : ""
        readonly property bool ok: pickedName !== "" && pickedAddress !== "" && user.text !== "" && password.text !== "" && !working
        function look() {
            searching = true
            backend.discoverHosts()
        }
        function openNew() {
            existing = false; found = []; picked = -1; error = ""; working = false
            otherName.text = ""; otherAddress.text = ""; password.text = ""
            open()
            look()
        }
        function openFor(h) {
            existing = true; fixedName = h.name; fixedAddress = h.address; fixedDongle = h.direct.split(",")[0].trim()
            error = ""; working = false; password.text = ""
            open()
            user.forceActiveFocus()
        }
        function accept() {
            if (!ok) return
            working = true; error = ""
            backend.signIn(pickedName, pickedAddress, pickedDongle, user.text, password.text)
        }
        Connections {
            target: backend
            function onHostsFound(list) {
                hostDialog.searching = false
                hostDialog.found = list
                if (hostDialog.picked < 0 || hostDialog.picked > list.length) {
                    const free = list.findIndex(h => !h.added)
                    hostDialog.picked = free >= 0 ? free : list.length
                }
            }
            function onSignedIn(name, ok, why) {
                if (!hostDialog.working) return
                hostDialog.working = false
                if (!ok) {
                    hostDialog.error = why
                    return
                }
                password.text = ""
                hostDialog.close()
                root.showPassiveNotification("Signed in to " + name)
                const h = backend.hosts.find(x => x.name === name)
                if (h && !hostDialog.existing) displayDialog.openFor(h)
            }
        }
        ColumnLayout {
            RowLayout {
                visible: !hostDialog.existing
                Controls.Label { text: "Computers running Vibepollo on your network:"; Layout.fillWidth: true }
                Controls.ToolButton {
                    icon.name: "view-refresh"
                    text: "Look again"
                    display: Controls.AbstractButton.IconOnly
                    enabled: !hostDialog.searching
                    onClicked: hostDialog.look()
                    Controls.ToolTip.text: text
                    Controls.ToolTip.visible: hovered
                }
            }
            Controls.BusyIndicator { visible: hostDialog.searching; running: visible; Layout.alignment: Qt.AlignHCenter }
            Repeater {
                model: hostDialog.existing ? [] : hostDialog.found
                Controls.RadioButton {
                    required property var modelData
                    required property int index
                    enabled: !modelData.added
                    text: modelData.name + " (" + modelData.address + (modelData.dongle ? ", and on the dongle" : "") + ")"
                          + (modelData.added ? ": added" : "")
                    checked: hostDialog.picked === index
                    onToggled: if (checked) hostDialog.picked = index
                }
            }
            Controls.RadioButton {
                visible: !hostDialog.existing && !hostDialog.searching
                text: hostDialog.found.length ? "Another one, by its address" : "None found: type its address"
                checked: hostDialog.typed
                onToggled: if (checked) hostDialog.picked = hostDialog.found.length
            }
            Kirigami.FormLayout {
                Layout.fillWidth: true
                visible: hostDialog.typed && !hostDialog.existing
                Controls.TextField { id: otherAddress; Kirigami.FormData.label: "Address:"; placeholderText: "192.168.1.20" }
                Controls.TextField { id: otherName; Kirigami.FormData.label: "Name:"; placeholderText: "My PC"; maximumLength: 40 }
            }
            Controls.Label {
                Layout.fillWidth: true
                Layout.topMargin: Kirigami.Units.largeSpacing
                wrapMode: Text.Wrap
                text: "Sign in with its Vibepollo Web UI user name and password. Frametop keeps a token that can only pair its displays, set their permissions and list the monitors; the password isn't kept."
            }
            Kirigami.FormLayout {
                Layout.fillWidth: true
                Controls.TextField { id: user; Kirigami.FormData.label: "User name:" }
                Controls.TextField {
                    id: password
                    Kirigami.FormData.label: "Password:"
                    echoMode: TextInput.Password
                    onAccepted: hostDialog.accept()
                }
            }
            Controls.BusyIndicator { visible: hostDialog.working; running: visible; Layout.alignment: Qt.AlignHCenter }
            Controls.Label {
                visible: hostDialog.error !== ""
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Kirigami.Theme.negativeTextColor
                text: hostDialog.error
            }
        }
        customFooterActions: [
            Kirigami.Action { text: "Sign in"; icon.name: "go-next"; enabled: hostDialog.ok; onTriggered: hostDialog.accept() },
            Kirigami.Action { text: "Cancel"; icon.name: "dialog-cancel"; onTriggered: { password.text = ""; hostDialog.close() } }
        ]
    }

    // A host's displays to add: its monitors as they are (all of them ticked, but the ones
    // already added), and a virtual one at any size.
    Kirigami.PromptDialog {
        id: displayDialog
        objectName: "displayDialog"
        title: "Add displays from " + host
        standardButtons: Kirigami.Dialog.NoButton
        property string host: ""
        property string address: ""
        property var added: []     // what its displays stream (app), so a monitor isn't added twice
        property var monitors: []
        property var chosen: ({})  // app -> ticked
        property bool virtualChosen: false
        property string error: ""
        property bool loading: false
        readonly property int count: monitors.filter(m => chosen[m.app]).length + (virtualChosen ? 1 : 0)
        function openFor(h) {
            host = h.name; address = h.address; added = h.displays.map(d => d.app)
            monitors = []; chosen = ({}); virtualChosen = false; error = ""; loading = true
            open()
            backend.listMonitors(address)
        }
        function tick(app, on) {
            const c = Object.assign({}, chosen)
            c[app] = on
            chosen = c
        }
        function accept() {
            if (!count) return
            close()
            const fps = displayRate.currentValue, kbps = Math.round(displayBitrate.value * 1000)
            for (const m of monitors)
                if (chosen[m.app]) backend.addDisplay(host, m.app, m.name, m.width, m.height, fps, kbps)
            if (virtualChosen) {
                const r = backend.resolutions[displayRes.currentIndex]
                backend.addDisplay(host, "monitor", "Virtual", r.width, r.height, fps, kbps)
            }
        }
        Connections {
            target: backend
            function onMonitorsReady(address, monitors, error) {
                if (address !== displayDialog.address) return
                displayDialog.loading = false
                displayDialog.monitors = monitors.filter(m => !m.virtual && m.active)
                const c = {}
                for (const m of displayDialog.monitors) c[m.app] = !displayDialog.added.includes(m.app)
                displayDialog.chosen = c
                displayDialog.error = error
            }
        }
        ColumnLayout {
            Controls.BusyIndicator { visible: displayDialog.loading; running: visible; Layout.alignment: Qt.AlignHCenter }
            Controls.Label {
                visible: displayDialog.error !== ""
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "Couldn't list its monitors: " + displayDialog.error
            }
            Repeater {
                model: displayDialog.monitors
                Controls.CheckBox {
                    required property var modelData
                    readonly property bool already: displayDialog.added.includes(modelData.app)
                    enabled: !already
                    text: modelData.name + " (" + modelData.width + " × " + modelData.height + (modelData.primary ? ", main" : "") + ")"
                          + (already ? ": added" : "")
                    checked: displayDialog.chosen[modelData.app] === true
                    onToggled: displayDialog.tick(modelData.app, checked)
                }
            }
            Controls.CheckBox {
                visible: !displayDialog.loading
                text: "A virtual display (the host makes a new monitor at the size you pick)"
                checked: displayDialog.virtualChosen
                onToggled: displayDialog.virtualChosen = checked
            }
            Kirigami.FormLayout {
                Layout.fillWidth: true
                visible: displayDialog.count > 0
                Controls.ComboBox {
                    id: displayRes
                    Kirigami.FormData.label: "Virtual display:"
                    visible: displayDialog.virtualChosen
                    model: backend.resolutions
                    textRole: "text"
                    Component.onCompleted: currentIndex = Math.max(0, backend.resolutions.findIndex(r => r.width === 2560 && r.height === 1440))
                }
                Controls.ComboBox {
                    id: displayRate
                    Kirigami.FormData.label: "Frame rate:"
                    model: backend.streamRates.map(r => ({ text: r + " fps", value: r }))
                    textRole: "text"
                    valueRole: "value"
                    Component.onCompleted: currentIndex = Math.max(0, indexOfValue(60))
                }
                RowLayout {
                    Kirigami.FormData.label: "Bitrate:"
                    Controls.SpinBox { id: displayBitrate; from: 0; to: 150; stepSize: 5; value: 0; editable: true }
                    Controls.Label { text: displayBitrate.value === 0 ? "Mbit/s (0: by size and rate)" : "Mbit/s" }
                }
            }
        }
        customFooterActions: [
            Kirigami.Action {
                text: displayDialog.count > 1 ? "Add " + displayDialog.count : "Add"
                icon.name: "list-add"
                enabled: displayDialog.count > 0
                onTriggered: displayDialog.accept()
            },
            Kirigami.Action { text: "Later"; icon.name: "dialog-cancel"; onTriggered: displayDialog.close() }
        ]
    }

    // ---------------------------------------------------------------- hosts and their displays
    Component {
        id: hostsPage
        Kirigami.ScrollablePage {
            title: "Remote displays"
            actions: [
                Kirigami.Action {
                    text: "Add computer"
                    icon.name: "list-add"
                    onTriggered: hostDialog.openNew()
                }
            ]
            header: Kirigami.InlineMessage {
                position: Kirigami.InlineMessage.Position.Header
                visible: true
                type: backend.desktopRunning ? Kirigami.MessageType.Information : Kirigami.MessageType.Warning
                text: !backend.desktopRunning
                      ? "The Frametop desktop isn't running. The connected displays start with it."
                      : backend.hosts.length === 0
                        ? "Another computer's monitors as Frametop screens, with the screens' controls, in your layouts and profiles. The computer runs Vibepollo: add it, sign in with its Web UI login, then pick its displays."
                        : "Each display is a panel like a screen: move, size, curve and pin it in VR, and Display Settings' Save as profile keeps where it is. A disconnected display keeps its place and settings."
            }
            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing
                Repeater {
                    model: backend.hosts
                    delegate: Kirigami.AbstractCard {
                        id: hostCard
                        required property var modelData
                        readonly property bool anyConnected: modelData.displays.some(d => d.connected)
                        Layout.fillWidth: true
                        contentItem: ColumnLayout {
                            RowLayout {
                                Kirigami.Icon {
                                    source: "computer"
                                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                                }
                                ColumnLayout {
                                    spacing: 0
                                    Kirigami.Heading { level: 3; text: hostCard.modelData.name }
                                    Controls.Label {
                                        text: hostCard.modelData.address + "  ·  "
                                              + (hostCard.modelData.online === true ? "online"
                                                 : hostCard.modelData.online === false ? "not answering" : "checking…")
                                              + (hostCard.modelData.hasToken ? "" : "  ·  not signed in")
                                        color: hostCard.modelData.online === false ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.textColor
                                        opacity: hostCard.modelData.online === false ? 1 : 0.7
                                    }
                                }
                                Item { Layout.fillWidth: true }
                                Controls.Switch {
                                    text: "Connected"
                                    visible: hostCard.modelData.displays.length > 0
                                    checked: hostCard.anyConnected
                                    onToggled: backend.setHostConnected(hostCard.modelData.name, checked)
                                    Controls.ToolTip.text: "Connect or disconnect all of its displays"
                                    Controls.ToolTip.visible: hovered
                                }
                                Controls.Button {
                                    visible: hostCard.modelData.hasToken
                                    text: "Add displays"
                                    icon.name: "list-add"
                                    onClicked: displayDialog.openFor(hostCard.modelData)
                                }
                                Controls.Button {
                                    visible: !hostCard.modelData.hasToken
                                    text: "Sign in"
                                    icon.name: "unlock"
                                    onClicked: hostDialog.openFor(hostCard.modelData)
                                }
                                Controls.ToolButton {
                                    visible: hostCard.modelData.hasToken
                                    icon.name: "unlock"
                                    display: Controls.AbstractButton.IconOnly
                                    text: "Sign in again"
                                    Controls.ToolTip.text: "Sign in again: a new token replaces the one kept here (revoke the old one in its Web UI, API Tokens)"
                                    Controls.ToolTip.visible: hovered
                                    onClicked: hostDialog.openFor(hostCard.modelData)
                                }
                                Controls.ToolButton {
                                    icon.name: "edit-delete-remove"
                                    display: Controls.AbstractButton.IconOnly
                                    enabled: hostCard.modelData.displays.length === 0
                                    text: "Remove this host"
                                    Controls.ToolTip.text: enabled ? text : "Remove its displays first"
                                    Controls.ToolTip.visible: hovered
                                    onClicked: backend.removeHost(hostCard.modelData.name)
                                }
                            }
                            // How its streams reach it: its Steam Link dongle on the Frame's hotspot
                            // (no router in the way) or the home network. Without a dongle, the
                            // network, and Find to look for one.
                            RowLayout {
                                id: route
                                property string finding: ""
                                readonly property bool hasDongle: hostCard.modelData.direct !== ""
                                Controls.Label { text: route.hasDongle ? "Connection:" : "Connection: the network" }
                                Controls.ComboBox {
                                    visible: route.hasDongle
                                    model: [{ text: "Auto (dongle when it's up)", value: "auto" },
                                            { text: "Network only", value: "network" },
                                            { text: "Dongle only", value: "dongle" }]
                                    textRole: "text"
                                    valueRole: "value"
                                    currentIndex: Math.max(0, indexOfValue(hostCard.modelData.route))
                                    onActivated: if (currentValue !== hostCard.modelData.route) backend.setRoute(hostCard.modelData.name, currentValue)
                                    Controls.ToolTip.text: "Changing it starts the host's streams over (a virtual display is made again, so its windows move)"
                                    Controls.ToolTip.visible: hovered
                                }
                                Controls.Label { visible: route.hasDongle; text: "Dongle:" }
                                Controls.TextField {
                                    id: dongle
                                    visible: route.hasDongle
                                    text: hostCard.modelData.direct
                                    placeholderText: "not found yet"
                                    Layout.preferredWidth: Kirigami.Units.gridUnit * 7
                                    onEditingFinished: if (text !== hostCard.modelData.direct) backend.setDirect(hostCard.modelData.name, text)
                                }
                                Controls.Button {
                                    text: route.finding === "…" ? "Looking…" : route.hasDongle ? "Find" : "Find a dongle"
                                    icon.name: "edit-find"
                                    flat: !route.hasDongle
                                    enabled: route.finding !== "…"
                                    onClicked: { route.finding = "…"; backend.findDongle(hostCard.modelData.name) }
                                    Controls.ToolTip.text: "Look for this computer on the Frame's hotspot (Steam Link's dongle)"
                                    Controls.ToolTip.visible: hovered
                                }
                                Controls.Label {
                                    visible: route.finding !== "" && route.finding !== "…"
                                    text: route.finding
                                    opacity: 0.7
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }
                                Connections {
                                    target: backend
                                    function onDongleFound(name, address, why) {
                                        if (name !== hostCard.modelData.name) return
                                        route.finding = address ? "found " + address : "not found: " + why
                                        if (address && address !== hostCard.modelData.direct) backend.setDirect(name, address)
                                    }
                                }
                            }
                            Controls.Label {
                                visible: hostCard.modelData.displays.length === 0
                                text: "No displays yet."
                                opacity: 0.7
                            }
                            Repeater {
                                model: hostCard.modelData.displays
                                delegate: ColumnLayout {
                                    id: disp
                                    required property var modelData
                                    property bool expanded: false
                                    Layout.fillWidth: true
                                    Kirigami.Separator { Layout.fillWidth: true }
                                    RowLayout {
                                        Kirigami.Icon {
                                            source: disp.modelData.virtual ? "video-display-symbolic" : "video-display"
                                            Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                                            Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                                        }
                                        ColumnLayout {
                                            spacing: 0
                                            Kirigami.Heading { level: 4; text: disp.modelData.label }
                                            Controls.Label {
                                                text: (disp.modelData.virtual ? "virtual display" : "monitor") + ", "
                                                      + disp.modelData.width + " × " + disp.modelData.height + " at " + disp.modelData.fps
                                                      + " fps, screen " + disp.modelData.number
                                                opacity: 0.7
                                            }
                                            Controls.Label {
                                                text: root.stateText(disp.modelData.state)
                                                color: root.stateColor(disp.modelData.state)
                                            }
                                        }
                                        Item { Layout.fillWidth: true }
                                        Controls.Switch {
                                            text: "Connected"
                                            checked: disp.modelData.connected
                                            onToggled: backend.setConnected(disp.modelData.id, checked)
                                        }
                                        Controls.Switch {
                                            text: "Shown"
                                            enabled: disp.modelData.connected
                                            checked: disp.modelData.shown
                                            onToggled: backend.setShown(disp.modelData.id, checked)
                                            Controls.ToolTip.text: "Hide its panel in VR; the stream keeps going, slowly"
                                            Controls.ToolTip.visible: hovered
                                        }
                                        Controls.ToolButton {
                                            icon.name: disp.expanded ? "go-up" : "configure"
                                            display: Controls.AbstractButton.IconOnly
                                            text: disp.expanded ? "Hide its settings" : "Its stream and size"
                                            Controls.ToolTip.text: text
                                            Controls.ToolTip.visible: hovered
                                            onClicked: disp.expanded = !disp.expanded
                                        }
                                        Controls.ToolButton {
                                            icon.name: "edit-delete-remove"
                                            display: Controls.AbstractButton.IconOnly
                                            text: "Remove this display"
                                            Controls.ToolTip.text: text
                                            Controls.ToolTip.visible: hovered
                                            onClicked: backend.removeDisplay(disp.modelData.id)
                                        }
                                    }
                                    Kirigami.FormLayout {
                                        Layout.fillWidth: true
                                        visible: disp.expanded
                                        RowLayout {
                                            Kirigami.FormData.label: "Stream:"
                                            Controls.SpinBox {
                                                id: sw
                                                from: 640; to: 7680; stepSize: 8; editable: true
                                                value: disp.modelData.width
                                            }
                                            Controls.Label { text: "×" }
                                            Controls.SpinBox {
                                                id: sh
                                                from: 360; to: 4320; stepSize: 8; editable: true
                                                value: disp.modelData.height
                                            }
                                            Controls.ComboBox {
                                                id: sr
                                                model: backend.streamRates.map(r => ({ text: r + " fps", value: r }))
                                                textRole: "text"
                                                valueRole: "value"
                                                Component.onCompleted: currentIndex = Math.max(0, indexOfValue(disp.modelData.fps))
                                            }
                                            Controls.SpinBox {
                                                id: sb
                                                from: 0; to: 150; stepSize: 5; editable: true
                                                value: Math.round(disp.modelData.bitrate / 1000)
                                            }
                                            Controls.Label { text: sb.value === 0 ? "Mbit/s (auto)" : "Mbit/s" }
                                            Controls.Button {
                                                text: "Apply"
                                                enabled: sw.value !== disp.modelData.width || sh.value !== disp.modelData.height
                                                         || sr.currentValue !== disp.modelData.fps || sb.value * 1000 !== disp.modelData.bitrate
                                                onClicked: backend.setStream(disp.modelData.id, sw.value, sh.value, sr.currentValue, sb.value * 1000)
                                            }
                                        }
                                        Controls.Label {
                                            visible: !disp.modelData.virtual
                                            Layout.fillWidth: true
                                            wrapMode: Text.Wrap
                                            opacity: 0.7
                                            text: "A monitor is scaled to the stream's size; its own resolution stays as it is."
                                        }
                                        RowLayout {
                                            Kirigami.FormData.label: "Width in VR:"
                                            Controls.Slider {
                                                id: rm
                                                from: 0.3; to: 6.0; stepSize: 0.05
                                                value: disp.modelData.metres
                                                Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                                                onMoved: backend.setMetres(disp.modelData.id, value)
                                            }
                                            Controls.Label {
                                                text: rm.value.toFixed(2) + " m wide, "
                                                      + (rm.value * disp.modelData.height / disp.modelData.width).toFixed(2) + " m tall"
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
