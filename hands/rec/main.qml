// Frametop Hand Recorder (Kirigami). Backend: ft_handrec.py ("backend"); frame sets come from
// its image provider (image://frames/...).
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: root
    title: "Frametop Hand Recorder"
    width: Kirigami.Units.gridUnit * 50
    height: Kirigami.Units.gridUnit * 36

    // The session chosen on the Export page, for the Upload page.
    property string chosenSession: ""

    globalDrawer: Kirigami.GlobalDrawer {
        isMenu: false
        modal: false
        collapsible: true
        collapsed: root.width < Kirigami.Units.gridUnit * 34
        actions: [
            Kirigami.Action { text: "Welcome"; icon.name: "help-about"; onTriggered: root.show(welcomePage) },
            Kirigami.Action {
                text: "Before you start"; icon.name: "view-task"; enabled: !backend.needsConsent
                onTriggered: root.show(checklistPage)
            },
            Kirigami.Action {
                text: "Session"; icon.name: "media-record"; enabled: !backend.needsConsent
                onTriggered: root.show(sessionPage)
            },
            Kirigami.Action { text: "Review"; icon.name: "view-preview"; onTriggered: root.show(reviewPage) },
            Kirigami.Action {
                text: "Export"; icon.name: "document-export"; enabled: !backend.needsConsent
                onTriggered: root.show(exportPage)
            },
            Kirigami.Action {
                text: "Upload"; icon.name: "cloud-upload"; enabled: !backend.needsConsent
                onTriggered: root.show(uploadPage)
            }
        ]
    }

    // The texts wait for a legal review; until then nobody should contribute.
    component DraftBanner: Kirigami.InlineMessage {
        visible: backend.textsDraft
        position: Kirigami.InlineMessage.Position.Header
        type: Kirigami.MessageType.Warning
        text: "Contributions aren't open yet. These texts are drafts waiting for a legal review: you can "
              + "record and review, but please don't upload anything until this message is gone."
    }

    // Read-only Markdown (CONSENT.md, UPLOAD.md), selectable for copying.
    component MarkdownText: Controls.TextArea {
        id: markdownText
        property string markdown
        // Links get the application palette's colour when the text is parsed, and Kirigami's theme
        // doesn't set that palette: set it, then parse again (the theme resolves after creation).
        readonly property color linkColor: Kirigami.Theme.linkColor
        function parse() {
            backend.setLinkColor(linkColor)
            text = markdown
        }
        onLinkColorChanged: parse()
        onMarkdownChanged: parse()
        Component.onCompleted: parse()
        textFormat: TextEdit.MarkdownText
        readOnly: true
        selectByMouse: true
        wrapMode: Text.Wrap
        background: null
        padding: 0
        onLinkActivated: link => Qt.openUrlExternally(link)
    }

    function plural(n, word) {
        return n + " " + word + (n === 1 ? "" : "s")
    }

    function show(page) {
        pageStack.clear()
        pageStack.push(page)
    }

    // --page welcome|checklist|session|review|export|upload opens the app on that page; the
    // Welcome page comes first until the consent is agreed to.
    pageStack.initialPage: backend.needsConsent && startPage !== "review" ? welcomePage
        : (({ welcome: welcomePage, checklist: checklistPage, session: sessionPage, review: reviewPage,
              export: exportPage, upload: uploadPage })[startPage] || checklistPage)

    Connections {
        target: backend
        function onMessage(text, isError) {
            root.showPassiveNotification(text, isError ? "long" : "short")
        }
    }

    // Asks before something is deleted for good: ask(title, text, function).
    Kirigami.PromptDialog {
        id: confirm
        property var action: null
        standardButtons: Controls.Dialog.Ok | Controls.Dialog.Cancel
        onAccepted: if (action) action()
        function ask(title, text, fn) {
            confirm.title = title
            confirm.subtitle = text
            confirm.action = fn
            confirm.open()
        }
    }

    // Restart SteamVR (the camera check's fix). What it does to Frametop, read from the code
    // (hands/README.md, "Camera check"): ft-screens quits when SteamVR does, which ends the
    // desktop; its unit doesn't restart, and stopping it ends what was started in it.
    function askRestartSteamVR() {
        confirm.ask("Restart SteamVR?",
                    "SteamVR starts its cameras again, and with them the colour camera module. This closes every "
                    + "VR app. It also closes the Frametop desktop with all its windows, this one too: Frametop's "
                    + "screens don't come back by themselves. Save your work first; programs started from the "
                    + "desktop, such as terminals and what runs in them, stop too.\n\nOnce SteamVR is back, start "
                    + "the desktop again (Desktop in the library) and open the Hand recorder: it checks the cameras "
                    + "again. If they're still off, restart the headset.",
                    () => backend.restartSteamVR())
    }

    // The camera check's problem, with its fix (a header message on the checklist and session pages).
    component CameraMessage: Kirigami.InlineMessage {
        position: Kirigami.InlineMessage.Position.Header
        type: backend.restartingSteamVR ? Kirigami.MessageType.Information : Kirigami.MessageType.Error
        text: backend.restartingSteamVR ? "Restarting SteamVR, then checking the cameras again…" : backend.cameraText
        actions: [
            Kirigami.Action {
                text: "Restart SteamVR…"
                icon.name: "system-reboot"
                enabled: !backend.restartingSteamVR && !backend.sessionActive
                onTriggered: root.askRestartSteamVR()
            },
            Kirigami.Action {
                text: "Check again"
                icon.name: "view-refresh"
                enabled: !backend.cameraBusy
                onTriggered: backend.checkCameras()
            }
        ]
    }

    // Closing during a session asks first. Quitting closes the window again (Qt 6), so the
    // answer is remembered; the backend stops the session as the app quits.
    property bool quitting: false
    onClosing: close => {
        if (backend.sessionActive && !quitting) {
            close.accepted = false
            confirm.ask("Stop the session?", "A session is recording. Closing stops it; what's recorded so far is kept.",
                        () => { root.quitting = true; Qt.quit() })
        } else if (backend.uploading && !quitting) {
            close.accepted = false
            confirm.ask("Cancel the upload?", "An upload is running. Closing cancels it; you can start it again later.",
                        () => { root.quitting = true; Qt.quit() })
        }
    }

    // ---------------------------------------------------------------- Welcome
    Component {
        id: welcomePage
        Kirigami.ScrollablePage {
            title: "Welcome"
            header: DraftBanner { }

            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing

                MarkdownText {
                    Layout.fillWidth: true
                    markdown: backend.consentText
                }

                Kirigami.Separator { Layout.fillWidth: true }

                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: !backend.needsConsent
                    type: Kirigami.MessageType.Positive
                    text: "You agreed to this text (" + backend.consentAccepted + "). Your contributor id is "
                          + backend.contributor + ": keep it if you might want to withdraw later."
                }
                Controls.Button {
                    visible: !backend.needsConsent
                    text: "Copy contributor id"
                    icon.name: "edit-copy"
                    onClicked: backend.copy(backend.contributor)
                }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: backend.needsConsent && backend.contributor !== ""
                    type: Kirigami.MessageType.Information
                    text: "This text changed since you last agreed to it. Please read it again; your contributor id stays the same."
                }

                Controls.CheckBox {
                    id: adult
                    visible: backend.needsConsent
                    text: "I'm 18 or older"
                }
                Controls.CheckBox {
                    id: region
                    visible: backend.needsConsent
                    text: "I don't live in Illinois, Texas or Washington (USA)"
                }
                Controls.CheckBox {
                    id: agree
                    visible: backend.needsConsent
                    text: "I agree to the text above"
                }
                RowLayout {
                    visible: backend.needsConsent
                    Controls.Label { text: "Handedness (optional):" }
                    Controls.ComboBox {
                        id: handed
                        model: backend.handednessChoices
                        textRole: "text"
                        valueRole: "value"
                        Component.onCompleted: currentIndex = Math.max(0, indexOfValue(backend.handedness))
                    }
                }
                Controls.Button {
                    visible: backend.needsConsent
                    enabled: adult.checked && region.checked && agree.checked
                    text: "Agree and continue"
                    icon.name: "go-next"
                    onClicked: {
                        backend.acceptConsent(adult.checked, region.checked, agree.checked, handed.currentValue)
                        if (!backend.needsConsent)
                            root.show(checklistPage)
                    }
                }
                Controls.Button {
                    visible: !backend.needsConsent
                    text: "Continue"
                    icon.name: "go-next"
                    onClicked: root.show(checklistPage)
                }
            }
        }
    }

    // ---------------------------------------------------------------- Before you start
    Component {
        id: checklistPage
        Kirigami.ScrollablePage {
            id: checklist
            title: "Before you start"
            readonly property bool privacyOk: privacy1.checked && privacy2.checked && privacy3.checked
            readonly property bool ready: privacyOk && backend.diskOk
                                          && backend.runnerError === "" && !backend.sessionActive

            function answers() {
                const objects = []
                for (let i = 0; i < objectBoxes.count; ++i) {
                    const box = objectBoxes.itemAt(i)
                    if (box.checked)
                        objects.push(box.value)
                }
                return {
                    objects: objects,
                    own_objects: ownObjects.text.split(",").map(s => s.trim()).filter(s => s !== ""),
                    controllers: straps.checked ? "straps" : "none",
                    sleeves: sleeves.currentValue,
                    rings: rings.checked,
                    watch: watch.checked,
                    notes: notes.text,
                    privacy: privacyOk
                }
            }

            header: ColumnLayout {
                spacing: 0
                DraftBanner { Layout.fillWidth: true }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: backend.runnerError !== ""
                    position: Kirigami.InlineMessage.Position.Header
                    type: Kirigami.MessageType.Error
                    text: backend.runnerError
                }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: backend.sessionActive
                    position: Kirigami.InlineMessage.Position.Header
                    type: Kirigami.MessageType.Information
                    text: "A session is running."
                    actions: [ Kirigami.Action { text: "Go to it"; onTriggered: root.show(sessionPage) } ]
                }
                CameraMessage {
                    Layout.fillWidth: true
                    visible: !backend.sessionActive && (backend.cameraText !== "" || backend.restartingSteamVR)
                }
            }
            Component.onCompleted: backend.checkCameras()

            Kirigami.FormLayout {
                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Within reach" }
                Controls.Label {
                    Kirigami.FormData.label: ""
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    wrapMode: Text.Wrap
                    text: "Put these on the desk or table in front of you. Untick what you don't have; "
                          + "the session skips those."
                }
                Repeater {
                    id: objectBoxes
                    model: backend.objects
                    Controls.CheckBox {
                        required property var modelData
                        required property int index
                        readonly property string value: modelData.value
                        Kirigami.FormData.label: index === 0 ? "Objects:" : ""
                        text: modelData.text
                        checked: true
                    }
                }
                Controls.TextField {
                    id: ownObjects
                    Kirigami.FormData.label: "Your own:"
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 20
                    placeholderText: "other things you use, separated by commas"
                }

                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Controllers" }
                ColumnLayout {
                    Kirigami.FormData.label: "Ergonomic Kit straps:"
                    Controls.RadioButton { id: straps; text: "Yes, I have the controllers with the straps" }
                    Controls.RadioButton { text: "No"; checked: true }
                }
                Controls.Label {
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: "With the straps, the controllers stay on your hands while your fingers move freely. "
                          + "The controllers' tracking then says exactly where your hands are, which teaches the "
                          + "model how far away a hand is. Without them, the two controller sections are skipped."
                }

                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Light" }
                Controls.Label {
                    Kirigami.FormData.label: "The cameras see:"
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    wrapMode: Text.Wrap
                    text: backend.lightingMeasured || "Not measured yet"
                }
                Controls.ComboBox {
                    id: lighting
                    Kirigami.FormData.label: "Lighting this round:"
                    model: backend.lightingChoices
                    textRole: "text"
                    valueRole: "value"
                    currentIndex: 0
                    onActivated: backend.checkLighting(currentValue)
                    Component.onCompleted: backend.checkLighting(currentValue)
                }
                Controls.Label {
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: "Each round in a different light helps the most: dim, a normal room, daylight. The cameras "
                          + "tell daylight from indoor light themselves; to say dim or a normal room, pick it here."
                }
                Kirigami.InlineMessage {
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    Layout.fillWidth: true
                    visible: backend.lightingNote !== ""
                    Layout.preferredHeight: visible ? implicitHeight : 0
                    type: Kirigami.MessageType.Warning
                    text: backend.lightingNote
                }
                Controls.Button {
                    visible: backend.lightingNote !== "" || backend.lightingMeasured === ""
                    text: "Measure the light again"
                    icon.name: "view-refresh"
                    onClicked: backend.checkLighting(lighting.currentValue)
                }

                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "You" }
                Controls.ComboBox {
                    id: sleeves
                    Kirigami.FormData.label: "Sleeves:"
                    model: backend.sleeveChoices
                    textRole: "text"
                    valueRole: "value"
                }
                Controls.CheckBox { id: rings; Kirigami.FormData.label: "Wearing:"; text: "Rings" }
                Controls.CheckBox { id: watch; text: "A watch or bracelet" }
                Controls.Label {
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: "Wear what you normally do: the dataset needs hands with and without them. This only notes it."
                }
                Controls.TextField {
                    id: notes
                    Kirigami.FormData.label: "Notes (optional):"
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 20
                    placeholderText: "anything that helps, e.g. a bandage on a finger"
                }

                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Privacy" }
                Controls.CheckBox {
                    id: privacy1
                    Kirigami.FormData.label: "I've checked:"
                    text: "I'm facing away from other people, mirrors and windows"
                }
                Controls.CheckBox {
                    id: privacy2
                    text: "No papers, photos, screens or other private things are in view"
                }
                Controls.CheckBox {
                    id: privacy3
                    text: "Nobody else's face or hands will be in view"
                }

                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Space" }
                Controls.Label {
                    Kirigami.FormData.label: "Free space:"
                    text: backend.freeText + " (a round takes about 10 GB)"
                }
                Kirigami.InlineMessage {
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    Layout.fillWidth: true
                    visible: !backend.diskOk
                    type: Kirigami.MessageType.Error
                    text: "Not enough free space for a round. Export and delete earlier sessions (Review), "
                          + "or free up space, first."
                }

                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Cameras" }
                RowLayout {
                    Kirigami.FormData.label: "Tracking cameras:"
                    Controls.Label {
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 20
                        wrapMode: Text.Wrap
                        text: backend.camerasIgnored ? "Not checked (--ignore-cameras)"
                              : backend.cameraState === "ok" ? "All four are running."
                              : backend.cameraState === "degraded" ? "Not all running: see the message at the top."
                              : backend.cameraState === "unknown" ? "Couldn't tell (" + backend.cameraSummary.replace(/^unknown: /, "")
                                                                    + "). The session checks again when it starts."
                              : "Checking…"
                    }
                    Controls.ToolButton {
                        visible: backend.cameraEvidence !== ""
                        icon.name: "documentinfo"
                        text: "Details"
                        checkable: true
                        id: cameraDetails
                        display: Controls.AbstractButton.IconOnly
                        Controls.ToolTip.text: "What the check looked at"
                        Controls.ToolTip.visible: hovered
                    }
                    Controls.ToolButton {
                        icon.name: "view-refresh"
                        text: "Check again"
                        display: Controls.AbstractButton.IconOnly
                        enabled: !backend.cameraBusy
                        onClicked: backend.checkCameras()
                        Controls.ToolTip.text: text
                        Controls.ToolTip.visible: hovered
                    }
                }
                Controls.Label {
                    visible: cameraDetails.checked && backend.cameraEvidence !== ""
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 34
                    wrapMode: Text.WrapAnywhere
                    font: Kirigami.Theme.smallFont
                    opacity: 0.7
                    textFormat: Text.PlainText
                    text: backend.cameraSummary + "\n" + backend.cameraEvidence
                }

                Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "What will happen" }
                Controls.Label {
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    wrapMode: Text.Wrap
                    text: "Press Start, then put the headset on. A panel in the headset shows each step: a picture "
                          + "of the hand pose, where to hold your hands, and what to do. The sections: hand poses, "
                          + "gestures, typing and the mouse, your objects, touching a dot, and moves with the "
                          + "controllers on and off. Each section is recorded as one take. In the pose steps your "
                          + "hands keep moving while a row of pictures lights up one shape after another: follow it.\n\n"
                          + "Each step waits until you're ready: press the button on the right side of the headset, "
                          + "or Space or Next in this window. A 3-2-1 "
                          + "countdown follows, then hold the pose until the bar runs out. Nothing is recorded while "
                          + "a step waits. The headset button also pauses and resumes a recording; press it twice "
                          + "to record a step again, or hold it to stop. In this window P "
                          + "pauses, R records the last step again, S skips a section and Esc stops.\n\n"
                          + "Nothing leaves the headset. Afterwards you watch the takes in Review, delete anything "
                          + "you don't want to share, and only then export."
                }
                ColumnLayout {
                    Kirigami.FormData.label: "Round:"
                    Controls.RadioButton {
                        id: fullRound
                        text: "Full session"
                        checked: !backend.hasFullSession
                    }
                    Controls.RadioButton {
                        id: quickRound
                        text: "Quick round (about 3 min): hand size, poses, the dot, no hands"
                        checked: backend.hasFullSession
                    }
                }
                Kirigami.InlineMessage {
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    Layout.fillWidth: true
                    visible: backend.hasFullSession
                    Layout.preferredHeight: visible ? implicitHeight : 0
                    type: Kirigami.MessageType.Information
                    text: "You've recorded a full session already. A quick round in a different light (dim, "
                          + "daylight) adds the most now."
                }
                Controls.CheckBox {
                    id: autoAdvance
                    Kirigami.FormData.label: "Pace:"
                    text: "Advance by itself (no Next between steps)"
                }
                Controls.Label {
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: (autoAdvance.checked
                           ? "Each step shows for a few seconds and the next follows by itself. Quicker if you "
                             + "know the steps already. "
                           : "") + "Length: " + backend.planText(checklist.answers(), autoAdvance.checked, quickRound.checked) + "."
                }
                Controls.Button {
                    text: "Start"
                    icon.name: "media-record"
                    enabled: checklist.ready && !backend.camerasBlockStart && !backend.restartingSteamVR
                    onClicked: {
                        if (backend.startSession(checklist.answers(), lighting.currentValue, autoAdvance.checked,
                                                 quickRound.checked))
                            root.show(sessionPage)
                    }
                }
                Controls.Label {
                    visible: (!checklist.ready || backend.camerasBlockStart) && !backend.sessionActive
                    opacity: 0.7
                    text: !checklist.privacyOk ? "Tick the three privacy checks to start."
                          : backend.camerasBlockStart ? "The headset's cameras aren't all running: see the message at the top." : ""
                }
            }
        }
    }

    // ---------------------------------------------------------------- Session
    Component {
        id: sessionPage
        Kirigami.Page {
            id: sessionView
            title: "Session"
            readonly property var st: backend.status
            readonly property string state: st.state || ""
            readonly property bool waiting: !!st.waiting
            readonly property bool stepMode: st.mode !== "auto"
            readonly property var stateText: ({
                starting: "Starting…", intro: "Get ready", ready: "Get ready", countdown: "Starting",
                running: "Recording", paused: "Paused", between: "Between sections", done: "Done",
                stopped: "Stopped", error: "Error", nohands: "No hands seen"
            })

            // The keys while the window has focus (the panel in the headset lists them too).
            Shortcut {
                sequence: "Space"
                enabled: backend.sessionActive
                onActivated: backend.nextStep()
            }
            Shortcut {
                sequence: "P"
                enabled: backend.sessionActive
                onActivated: backend.togglePause()
            }
            Shortcut {
                sequence: "R"
                enabled: backend.sessionActive
                onActivated: backend.redo()
            }
            Shortcut {
                sequence: "S"
                enabled: backend.sessionActive
                onActivated: backend.skipSection()
            }
            Shortcut {
                sequence: "Esc"
                enabled: backend.sessionActive
                onActivated: backend.stopSession()
            }

            // A hand chip like the panel's: seen (green), lost (orange) or hidden.
            component HandChip: Controls.Label {
                property var seen
                visible: seen === true || seen === false || seen === "seen" || seen === "lost"
                readonly property bool ok: seen === true || seen === "seen"
                padding: Kirigami.Units.smallSpacing
                leftPadding: Kirigami.Units.largeSpacing
                rightPadding: Kirigami.Units.largeSpacing
                color: "white"
                background: Rectangle {
                    radius: height / 2
                    color: parent.ok ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.neutralTextColor
                }
            }

            // The pose picture as the panel shows it: a right hand as drawn, a left hand flipped,
            // both hands as a flipped copy beside it (DESIGN.md "The pose pictures").
            component PosePicture: Row {
                property string path
                property string mode
                property real side: Kirigami.Units.gridUnit * 9
                spacing: Kirigami.Units.smallSpacing
                visible: path !== ""
                Image {
                    visible: parent.mode === "both"
                    width: visible ? parent.side * 0.75 : 0
                    height: width
                    source: parent.path ? "file://" + parent.path : ""
                    fillMode: Image.PreserveAspectFit
                    mirror: true
                    smooth: true
                    mipmap: true
                }
                Image {
                    width: parent.mode === "both" ? parent.side * 0.75 : parent.side
                    height: width
                    source: parent.path ? "file://" + parent.path : ""
                    fillMode: Image.PreserveAspectFit
                    mirror: parent.mode === "mirror"
                    smooth: true
                    mipmap: true
                }
            }

            // Where to hold the hands: a front view (left, centre, right, up, down; the push
            // sections' chest, desk and eye) and how far out (near, mid, far), as on the panel.
            component WhereDiagram: RowLayout {
                id: where
                property string position
                property string distance
                readonly property var cell: ({ centre: [1, 1], center: [1, 1], chest: [1, 1], left: [0, 1],
                                               right: [2, 1], up: [1, 0], eye: [1, 0], down: [1, 2],
                                               desk: [1, 2] })[position] || null
                readonly property int step: ["near", "mid", "far"].indexOf(distance)
                readonly property real unit: Kirigami.Units.gridUnit * 1.1
                readonly property color accent: "#4cd9ff"
                spacing: Kirigami.Units.gridUnit
                visible: cell !== null || step >= 0
                ColumnLayout {
                    visible: where.cell !== null
                    Grid {
                        Layout.alignment: Qt.AlignHCenter
                        columns: 3
                        spacing: 2
                        Repeater {
                            model: 9
                            Rectangle {
                                required property int index
                                width: where.unit * 1.33
                                height: where.unit
                                radius: 3
                                readonly property bool on: where.cell !== null && index === where.cell[1] * 3 + where.cell[0]
                                color: on ? where.accent
                                          : Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g,
                                                    Kirigami.Theme.textColor.b, 0.12)
                            }
                        }
                    }
                    Controls.Label {
                        Layout.alignment: Qt.AlignHCenter
                        font: Kirigami.Theme.smallFont
                        text: ({ left: "To your left", right: "To your right", up: "Up high", down: "Down low",
                                 chest: "Chest height", desk: "Desk height", eye: "Eye level" })[where.position]
                              || "In front"
                    }
                }
                ColumnLayout {
                    visible: where.step >= 0
                    Row {
                        Layout.alignment: Qt.AlignHCenter
                        spacing: where.unit * 0.6
                        height: where.unit * 1.2
                        // the head, seen from the side, then the arm's reach
                        Rectangle {
                            width: where.unit
                            height: width
                            radius: width / 2
                            anchors.verticalCenter: parent.verticalCenter
                            color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g,
                                           Kirigami.Theme.textColor.b, 0.6)
                        }
                        Repeater {
                            model: 3
                            Rectangle {
                                required property int index
                                readonly property bool on: index === where.step
                                width: on ? where.unit * 0.8 : where.unit * 0.35
                                height: width
                                radius: width / 2
                                anchors.verticalCenter: parent.verticalCenter
                                color: on ? where.accent : Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g,
                                                                   Kirigami.Theme.textColor.b, 0.35)
                            }
                        }
                    }
                    Controls.Label {
                        Layout.alignment: Qt.AlignHCenter
                        font: Kirigami.Theme.smallFont
                        text: ["Close: a hand's length", "Halfway out", "Arm stretched out"][Math.max(0, where.step)]
                    }
                }
            }

            header: ColumnLayout {
                spacing: 0
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: backend.runnerError !== "" || (sessionView.state === "error" && sessionView.st.error !== backend.cameraText)
                    position: Kirigami.InlineMessage.Position.Header
                    type: Kirigami.MessageType.Error
                    text: backend.runnerError || ("The session stopped with an error: " + (sessionView.st.error || "unknown"))
                }
                // The camera check stopped the session (at its start, or after a step with no hands).
                CameraMessage {
                    Layout.fillWidth: true
                    visible: (!backend.sessionActive && sessionView.state !== "" && backend.cameraText !== "")
                             || backend.restartingSteamVR
                }
            }

            Kirigami.PlaceholderMessage {
                anchors.centerIn: parent
                width: parent.width - Kirigami.Units.gridUnit * 4
                visible: sessionView.state === ""
                icon.name: "media-record"
                text: "No session yet"
                explanation: "Go through Before you start, then press Start there."
                helpfulAction: Kirigami.Action {
                    text: "Before you start"; icon.name: "view-task"
                    onTriggered: root.show(checklistPage)
                }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Kirigami.Units.gridUnit
                visible: sessionView.state !== ""
                spacing: Kirigami.Units.largeSpacing

                RowLayout {
                    Layout.fillWidth: true
                    Kirigami.Heading {
                        level: 1
                        text: sessionView.stateText[sessionView.state] || sessionView.state
                    }
                    Item { Layout.fillWidth: true }
                    Controls.Label {
                        visible: !!sessionView.st.take && sessionView.state !== "paused"
                                 && (sessionView.state === "countdown" || sessionView.state === "running"
                                     || !sessionView.stepMode)
                        text: "● Recording"
                        color: Kirigami.Theme.negativeTextColor
                    }
                }
                Controls.Label {
                    visible: (sessionView.st.section_index || 0) > 0
                    text: "Section " + sessionView.st.section_index + " of " + sessionView.st.section_count
                          + (sessionView.st.title ? ": " + sessionView.st.title : "")
                          + ((sessionView.st.step_index || 0) > 0 && sessionView.stepMode
                             ? " · step " + sessionView.st.step_index + " of " + sessionView.st.step_count : "")
                    font.bold: true
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.gridUnit * 1.5

                    ColumnLayout {
                        visible: !!sessionView.st.image || !!sessionView.st.position || !!sessionView.st.distance
                        spacing: Kirigami.Units.largeSpacing
                        PosePicture {
                            Layout.alignment: Qt.AlignHCenter
                            path: sessionView.st.image || ""
                            mode: sessionView.st.image_mode || ""
                        }
                        Controls.Label {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.maximumWidth: Kirigami.Units.gridUnit * 14
                            visible: !!sessionView.st.image && !!sessionView.st.caption
                            wrapMode: Text.Wrap
                            horizontalAlignment: Text.AlignHCenter
                            opacity: 0.7
                            font: Kirigami.Theme.smallFont
                            text: sessionView.st.caption || ""
                        }
                        WhereDiagram {
                            id: where
                            Layout.alignment: Qt.AlignHCenter
                            position: sessionView.st.position || ""
                            distance: sessionView.st.distance || ""
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.largeSpacing
                        Controls.Label {
                            Layout.fillWidth: true
                            visible: !!sessionView.st.prompt
                            wrapMode: Text.Wrap
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.4
                            text: sessionView.st.prompt || ""
                        }
                        // A sweep's row of pictures, the lit one framed, as on the panel
                        Row {
                            visible: (sessionView.st.strip || []).length > 0
                            spacing: Kirigami.Units.largeSpacing
                            Repeater {
                                model: sessionView.st.strip || []
                                Column {
                                    required property var modelData
                                    required property int index
                                    readonly property bool lit: index === sessionView.st.cue
                                    spacing: Kirigami.Units.smallSpacing
                                    Rectangle {
                                        width: Kirigami.Units.gridUnit * 5.5
                                        height: width
                                        radius: Kirigami.Units.smallSpacing * 2
                                        color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g,
                                                       Kirigami.Theme.textColor.b, 0.06)
                                        border.width: parent.lit ? 3 : 0
                                        border.color: "#4cd9ff"
                                        Image {
                                            anchors.fill: parent
                                            anchors.margins: 4
                                            source: modelData.image ? "file://" + modelData.image : ""
                                            fillMode: Image.PreserveAspectFit
                                            mirror: modelData.mode === "mirror"
                                            opacity: parent.parent.lit ? 1 : 0.4
                                            smooth: true
                                            mipmap: true
                                        }
                                    }
                                    Controls.Label {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: modelData.label || ""
                                        color: parent.lit ? "#4cd9ff" : Kirigami.Theme.textColor
                                        opacity: parent.lit ? 1 : 0.6
                                    }
                                }
                            }
                        }
                        // The countdown's 3, 2, 1, then the hold's word, big
                        Controls.Label {
                            visible: !!sessionView.st.big && sessionView.state !== "paused"
                            text: sessionView.st.big || ""
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 4
                            font.bold: true
                            color: Kirigami.Theme.highlightColor
                        }
                        Controls.Label {
                            visible: !sessionView.waiting && sessionView.state !== "countdown"
                                     && sessionView.state !== "ready" && sessionView.st.seconds_left !== undefined
                                     && sessionView.st.seconds_left !== null && sessionView.st.seconds_left > 0
                            text: Math.ceil(sessionView.st.seconds_left || 0) + " s left"
                            opacity: 0.7
                        }
                        Controls.Button {
                            visible: sessionView.waiting && sessionView.state !== "paused"
                            focusPolicy: Qt.NoFocus
                            text: sessionView.state === "nohands" ? "Try again" : "Next"
                            icon.name: sessionView.state === "nohands" ? "edit-undo" : "go-next"
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.5
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 10
                            Layout.preferredHeight: Kirigami.Units.gridUnit * 3
                            onClicked: backend.nextStep()
                        }
                        Controls.Label {
                            visible: sessionView.waiting && sessionView.state !== "paused"
                            opacity: 0.7
                            text: sessionView.state === "nohands"
                                  ? "Try again starts the same step (Space or the headset button do too). Stop ends the session, "
                                    + "keeping what's recorded."
                                  : (sessionView.st.ready_text || "Ready? Press Space or click Next")
                                    + ". A 3-2-1 countdown starts the recording."
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }
                    }
                }

                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: !!sessionView.st.note
                    // (A hidden InlineMessage keeps its height in a layout.)
                    Layout.preferredHeight: visible ? implicitHeight : 0
                    type: Kirigami.MessageType.Warning
                    text: sessionView.st.note || ""
                }
                RowLayout {
                    spacing: Kirigami.Units.largeSpacing
                    HandChip { text: "Left hand"; seen: sessionView.st.hands ? sessionView.st.hands.left : undefined }
                    HandChip { text: "Right hand"; seen: sessionView.st.hands ? sessionView.st.hands.right : undefined }
                }
                Controls.Label {
                    visible: !!sessionView.st.take
                    text: "Take " + (sessionView.st.take || "")
                    opacity: 0.7
                    font: Kirigami.Theme.smallFont
                }

                Item { Layout.fillHeight: true }

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    text: "The instructions appear in the headset. "
                          + (sessionView.st.button ? "The button on the right side of the headset: "
                             + (sessionView.stepMode ? "next, " : "") + "pause or resume; twice: record "
                             + "the last step again; hold: stop. " : "")
                          + "While this window has focus: "
                          + (sessionView.stepMode ? "Space: next · " : "")
                          + "P: pause or resume · R: record the last step again · S: skip section · Esc: stop. "
                          + "Stopping keeps what's recorded so far."
                }
                RowLayout {
                    // No keyboard focus on the buttons, so Space always reaches the shortcut.
                    Controls.Button {
                        visible: !backend.sessionActive
                        focusPolicy: Qt.NoFocus
                        text: "New session…"
                        icon.name: "media-record"
                        onClicked: root.show(checklistPage)
                    }
                    Controls.Button {
                        visible: backend.sessionActive
                        focusPolicy: Qt.NoFocus
                        text: sessionView.state === "paused" ? "Resume" : "Pause"
                        icon.name: sessionView.state === "paused" ? "media-playback-start" : "media-playback-pause"
                        onClicked: backend.togglePause()
                    }
                    Controls.Button {
                        visible: backend.sessionActive
                        enabled: !!sessionView.st.can_redo
                        focusPolicy: Qt.NoFocus
                        text: "Redo step"
                        icon.name: "edit-undo"
                        onClicked: backend.redo()
                    }
                    Controls.Button {
                        visible: backend.sessionActive
                        focusPolicy: Qt.NoFocus
                        text: "Skip section"
                        icon.name: "media-skip-forward"
                        onClicked: backend.skipSection()
                    }
                    Controls.Button {
                        visible: backend.sessionActive
                        focusPolicy: Qt.NoFocus
                        text: "Stop"
                        icon.name: "media-playback-stop"
                        onClicked: backend.stopSession()
                    }
                    Controls.Button {
                        visible: !backend.sessionActive && backend.sessionId !== ""
                        focusPolicy: Qt.NoFocus
                        text: "Review this session"
                        icon.name: "view-preview"
                        onClicked: {
                            root.show(reviewPage)
                            pageStack.push(takesPage, { session: backend.sessionId })
                        }
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------- Review
    Component {
        id: reviewPage
        Kirigami.ScrollablePage {
            title: "Review"
            actions: [
                Kirigami.Action { text: "Refresh"; icon.name: "view-refresh"; onTriggered: backend.refreshSessions() }
            ]

            ListView {
                model: backend.sessions
                spacing: Kirigami.Units.smallSpacing

                Kirigami.PlaceholderMessage {
                    anchors.centerIn: parent
                    width: parent.width - Kirigami.Units.gridUnit * 4
                    visible: parent.count === 0
                    text: "No sessions yet"
                    explanation: "Recorded sessions appear here, to watch and trim before you export them."
                }

                delegate: Controls.ItemDelegate {
                    id: sessionRow
                    required property var modelData
                    width: ListView.view.width
                    onClicked: pageStack.push(takesPage, { session: modelData.id })

                    contentItem: RowLayout {
                        spacing: Kirigami.Units.largeSpacing
                        Kirigami.Icon {
                            source: sessionRow.modelData.active ? "media-record" : "folder-videos"
                            implicitWidth: Kirigami.Units.iconSizes.medium
                            implicitHeight: implicitWidth
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Controls.Label {
                                text: sessionRow.modelData.label + (sessionRow.modelData.active ? " (recording)" : "")
                                      + (sessionRow.modelData.dry_run ? " (dry run)" : "")
                                font.bold: true
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                opacity: 0.7
                                font: Kirigami.Theme.smallFont
                                text: root.plural(sessionRow.modelData.takes, "take") + " · " + sessionRow.modelData.sizeText
                                      + (sessionRow.modelData.statusText ? " · " + sessionRow.modelData.statusText : "")
                                      + (sessionRow.modelData.lightingText ? " · " + sessionRow.modelData.lightingText : "")
                                      + (sessionRow.modelData.export_stale ? " · changed since its export"
                                         : sessionRow.modelData.exported ? " · exported (" + sessionRow.modelData.exportText + ")" : "")
                            }
                        }
                        Controls.ToolButton {
                            icon.name: "edit-delete"
                            text: "Delete session"
                            display: Controls.AbstractButton.IconOnly
                            enabled: !sessionRow.modelData.active
                            Controls.ToolTip.text: text
                            Controls.ToolTip.visible: hovered
                            onClicked: {
                                const sid = sessionRow.modelData.id
                                confirm.ask("Delete this session?",
                                            "The session of " + sessionRow.modelData.label + " (" + root.plural(sessionRow.modelData.takes, "take")
                                            + ", " + sessionRow.modelData.sizeText + ") and its export are deleted from the "
                                            + "headset for good.",
                                            () => { pageStack.pop(pageStack.items[0]); backend.deleteSession(sid) })
                            }
                        }
                    }
                }
            }
        }
    }

    Component {
        id: takesPage
        Kirigami.ScrollablePage {
            id: takesView
            property string session: ""
            property var rows: backend.takeList(session)
            title: "Takes"

            Connections {
                target: backend
                function onSessionsChanged() { takesView.rows = backend.takeList(takesView.session) }
            }

            ListView {
                model: takesView.rows
                spacing: Kirigami.Units.smallSpacing

                Kirigami.PlaceholderMessage {
                    anchors.centerIn: parent
                    width: parent.width - Kirigami.Units.gridUnit * 4
                    visible: parent.count === 0
                    text: "No takes in this session"
                }

                delegate: Controls.ItemDelegate {
                    id: takeRow
                    required property var modelData
                    width: ListView.view.width
                    onClicked: pageStack.push(viewerPage, { session: takesView.session, take: modelData.id })

                    contentItem: RowLayout {
                        spacing: Kirigami.Units.largeSpacing
                        Image {
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 5
                            Layout.preferredHeight: Kirigami.Units.gridUnit * 4.5
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            source: takeRow.modelData.sets > 0
                                    ? "image://frames/thumb/" + takesView.session + "/" + takeRow.modelData.id : ""
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Controls.Label { text: takeRow.modelData.title; font.bold: true }
                            Controls.Label {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                opacity: 0.7
                                font: Kirigami.Theme.smallFont
                                text: takeRow.modelData.status + " · " + takeRow.modelData.durationText + " · "
                                      + takeRow.modelData.sets + " sets"
                                      + (takeRow.modelData.deleted_sets ? " (" + takeRow.modelData.deleted_sets + " deleted)" : "")
                                      + " · " + takeRow.modelData.sizeText
                            }
                        }
                        Controls.ToolButton {
                            icon.name: "edit-delete"
                            text: "Delete take"
                            display: Controls.AbstractButton.IconOnly
                            Controls.ToolTip.text: text
                            Controls.ToolTip.visible: hovered
                            onClicked: {
                                const take = takeRow.modelData.id
                                confirm.ask("Delete this take?",
                                            "\"" + takeRow.modelData.title + "\" (" + takeRow.modelData.durationText
                                            + ") is deleted from the headset for good.",
                                            () => { pageStack.pop(takesView); backend.deleteTake(takesView.session, take) })
                            }
                        }
                    }
                }
            }
        }
    }

    Component {
        id: viewerPage
        Kirigami.Page {
            id: viewer
            property string session: ""
            property string take: ""
            property var info: backend.takeInfo(session, take)
            property int index: 0
            property int markStart: -1
            property int markEnd: -1
            title: info.title
            readonly property bool deletedHere: isDeleted(index)

            function step(n) {
                index = Math.max(0, Math.min(info.count - 1, index + n))
            }
            function isDeleted(i) {
                for (const r of info.ranges)
                    if (i >= r[0] && i <= r[1] && r[0] >= 0)
                        return true
                return false
            }
            function deleteMarked() {
                backend.deleteRange(session, take, markStart, markEnd)
                markStart = markEnd = -1
                info = backend.takeInfo(session, take)
            }

            Connections {
                target: backend
                function onSessionsChanged() { viewer.info = backend.takeInfo(viewer.session, viewer.take) }
            }

            // Keyboard stepping: arrows one set, Page Up/Down ten, Home/End, [ and ] mark a range,
            // Delete deletes it.
            Item {
                id: keys
                focus: true
                Component.onCompleted: forceActiveFocus()
                Keys.onPressed: event => {
                    const k = event.key
                    if (k === Qt.Key_Left) viewer.step(-1)
                    else if (k === Qt.Key_Right) viewer.step(1)
                    else if (k === Qt.Key_PageUp) viewer.step(-10)
                    else if (k === Qt.Key_PageDown) viewer.step(10)
                    else if (k === Qt.Key_Home) viewer.index = 0
                    else if (k === Qt.Key_End) viewer.index = Math.max(0, viewer.info.count - 1)
                    else if (k === Qt.Key_BracketLeft) viewer.markStart = viewer.index
                    else if (k === Qt.Key_BracketRight) viewer.markEnd = viewer.index
                    else if (k === Qt.Key_Delete && viewer.markStart >= 0 && viewer.markEnd >= 0) viewer.deleteMarked()
                    else return
                    event.accepted = true
                }
            }

            Kirigami.PlaceholderMessage {
                anchors.centerIn: parent
                visible: viewer.info.count === 0
                text: "This take has no frame sets"
            }

            ColumnLayout {
                anchors.fill: parent
                visible: viewer.info.count > 0
                spacing: Kirigami.Units.smallSpacing

                Image {
                    id: frame
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    cache: false
                    retainWhileLoading: true
                    source: viewer.info.count > 0
                            ? "image://frames/set/" + viewer.session + "/" + viewer.take + "/" + viewer.index : ""
                    MouseArea { anchors.fill: parent; onClicked: keys.forceActiveFocus() }

                    Rectangle {
                        visible: viewer.deletedHere
                        x: (frame.width - frame.paintedWidth) / 2
                        y: (frame.height - frame.paintedHeight) / 2
                        width: frame.paintedWidth
                        height: frame.paintedHeight
                        color: Qt.rgba(0.8, 0, 0, 0.35)
                        Controls.Label {
                            anchors.centerIn: parent
                            text: "Deleted: not exported"
                            color: "white"
                            font.bold: true
                        }
                    }
                }

                // The slider, with the deleted ranges (red) and the marked range (blue) under it.
                Item {
                    Layout.fillWidth: true
                    implicitHeight: slider.implicitHeight
                    Repeater {
                        model: viewer.info.ranges
                        Rectangle {
                            required property var modelData
                            visible: modelData[0] >= 0
                            readonly property real unit: (slider.availableWidth) / Math.max(1, viewer.info.count - 1)
                            x: slider.leftPadding + modelData[0] * unit - 2
                            width: Math.max(4, (modelData[1] - modelData[0]) * unit + 4)
                            y: slider.topPadding + slider.availableHeight / 2 + 4
                            height: 4
                            color: Kirigami.Theme.negativeTextColor
                        }
                    }
                    Rectangle {
                        visible: viewer.markStart >= 0
                        readonly property int last: viewer.markEnd >= 0 ? viewer.markEnd : viewer.index
                        readonly property real unit: (slider.availableWidth) / Math.max(1, viewer.info.count - 1)
                        x: slider.leftPadding + Math.min(viewer.markStart, last) * unit - 2
                        width: Math.abs(last - viewer.markStart) * unit + 4
                        y: slider.topPadding + slider.availableHeight / 2 - 8
                        height: 4
                        color: Kirigami.Theme.highlightColor
                    }
                    Controls.Slider {
                        id: slider
                        anchors.left: parent.left
                        anchors.right: parent.right
                        focusPolicy: Qt.NoFocus
                        from: 0
                        to: Math.max(0, viewer.info.count - 1)
                        stepSize: 1
                        snapMode: Controls.Slider.SnapAlways
                        value: viewer.index
                        onMoved: viewer.index = Math.round(value)
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Controls.Label {
                        text: "Set " + (viewer.index + 1) + " of " + viewer.info.count + " · "
                              + backend.setTime(viewer.session, viewer.take, viewer.index)
                    }
                    Item { Layout.fillWidth: true }
                    Controls.Button {
                        focusPolicy: Qt.NoFocus
                        text: viewer.markStart >= 0 ? "Start: " + (viewer.markStart + 1) : "Mark start"
                        icon.name: "go-first"
                        onClicked: viewer.markStart = viewer.index
                    }
                    Controls.Button {
                        focusPolicy: Qt.NoFocus
                        text: viewer.markEnd >= 0 ? "End: " + (viewer.markEnd + 1) : "Mark end"
                        icon.name: "go-last"
                        onClicked: viewer.markEnd = viewer.index
                    }
                    Controls.Button {
                        focusPolicy: Qt.NoFocus
                        enabled: viewer.markStart >= 0 && viewer.markEnd >= 0
                        text: "Delete range"
                        icon.name: "edit-cut"
                        onClicked: viewer.deleteMarked()
                    }
                    Controls.Button {
                        focusPolicy: Qt.NoFocus
                        visible: viewer.markStart >= 0 || viewer.markEnd >= 0
                        text: "Clear marks"
                        icon.name: "edit-clear"
                        onClicked: viewer.markStart = viewer.markEnd = -1
                    }
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing
                    visible: viewer.info.ranges.length > 0
                    Controls.Label { text: "Deleted:"; topPadding: Kirigami.Units.smallSpacing }
                    Repeater {
                        model: viewer.info.ranges
                        Controls.Button {
                            required property var modelData
                            required property int index
                            focusPolicy: Qt.NoFocus
                            flat: true
                            text: (modelData[0] < 0 ? "a range with no sets"
                                   : "sets " + (modelData[0] + 1) + " to " + (modelData[1] + 1)) + ": restore"
                            icon.name: "edit-undo"
                            onClicked: backend.restoreRange(viewer.session, viewer.take, index)
                        }
                    }
                }

                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    font: Kirigami.Theme.smallFont
                    text: "← → step one set, Page Up/Down ten, Home/End. [ and ] mark a range's start and end, "
                          + "Delete deletes it. Deleted sets stay on the headset until export, which leaves them out."
                }
            }
        }
    }

    // ---------------------------------------------------------------- Export
    Component {
        id: exportPage
        Kirigami.ScrollablePage {
            id: exportView
            title: "Export"
            property bool worn: backend.headsetWorn()
            readonly property var chosen: {
                const list = backend.sessions
                for (const s of list)
                    if (s.id === sessionBox.currentValue)
                        return s
                return null
            }

            Timer {
                interval: 5000
                running: true
                repeat: true
                onTriggered: exportView.worn = backend.headsetWorn()
            }

            header: ColumnLayout {
                spacing: 0
                DraftBanner { Layout.fillWidth: true }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: exportView.worn && backend.exporting
                    position: Kirigami.InlineMessage.Position.Header
                    type: Kirigami.MessageType.Information
                    text: "Exporting keeps the processor busy for a few minutes, so VR may stutter a little until "
                          + "it's done. You can keep using the headset meanwhile."
                }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: !backend.zstdFound
                    position: Kirigami.InlineMessage.Position.Header
                    type: Kirigami.MessageType.Error
                    text: "zstd isn't installed in the dev container, so nothing can be exported. Run setup/dev-container.sh."
                }
            }

            Kirigami.FormLayout {
                Controls.ComboBox {
                    id: sessionBox
                    Kirigami.FormData.label: "Session:"
                    model: backend.sessions
                    textRole: "label"
                    valueRole: "id"
                    enabled: !backend.exporting
                    Component.onCompleted: {
                        const i = indexOfValue(root.chosenSession)
                        currentIndex = i >= 0 ? i : (count > 0 ? 0 : -1)
                    }
                    onActivated: root.chosenSession = currentValue
                }
                Controls.Label {
                    Kirigami.FormData.label: "Recorded:"
                    visible: exportView.chosen !== null
                    text: exportView.chosen ? root.plural(exportView.chosen.takes, "take") + ", " + exportView.chosen.sizeText : ""
                }
                Controls.Label {
                    Kirigami.FormData.label: "Export:"
                    visible: exportView.chosen !== null
                    text: !exportView.chosen ? ""
                          : exportView.chosen.export_stale ? "changed since it was exported: export it again"
                          : exportView.chosen.exported ? "ready, " + exportView.chosen.exportText : "not exported yet"
                }
                RowLayout {
                    Controls.Button {
                        text: backend.exporting ? "Exporting…"
                              : exportView.chosen && exportView.chosen.exported ? "Export again" : "Export"
                        icon.name: "document-export"
                        enabled: exportView.chosen !== null && !backend.exporting && backend.zstdFound
                                 && !exportView.chosen.active
                        onClicked: backend.exportSession(exportView.chosen.id, false)
                    }
                    // Shows the moment Export is pressed: the first progress can take a few seconds.
                    Controls.BusyIndicator {
                        visible: backend.exporting
                        running: visible
                        implicitWidth: Kirigami.Units.gridUnit * 1.5
                        implicitHeight: implicitWidth
                    }
                    Controls.Button {
                        visible: backend.exporting
                        text: "Cancel"
                        icon.name: "dialog-cancel"
                        onClicked: backend.cancelExport()
                    }
                }
                // Drawn here: the desktop style's ProgressBar paints nothing without a QApplication.
                Rectangle {
                    Kirigami.FormData.label: "Progress:"
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 20
                    implicitHeight: Kirigami.Units.smallSpacing * 2
                    visible: backend.exporting
                    radius: height / 2
                    color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.15)
                    Rectangle {
                        width: parent.width * Math.max(0, Math.min(1, backend.exportFraction))
                        height: parent.height
                        radius: parent.radius
                        color: Kirigami.Theme.highlightColor
                    }
                }
                Controls.Label {
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 26
                    visible: backend.exportText !== "" && (backend.exporting || backend.exportSessionId === sessionBox.currentValue)
                    wrapMode: Text.WrapAnywhere
                    text: backend.exportText
                }
                RowLayout {
                    visible: exportView.chosen !== null && exportView.chosen.exported && !backend.exporting
                    Controls.Button {
                        text: "Upload"
                        icon.name: "go-next"
                        onClicked: root.show(uploadPage)
                    }
                    Controls.Button {
                        text: "Delete export"
                        icon.name: "edit-delete"
                        onClicked: {
                            const sid = exportView.chosen.id
                            confirm.ask("Delete this export?", "The export is deleted; the session's recordings stay.",
                                        () => backend.deleteExport(sid))
                        }
                    }
                }
            }

            footer: Controls.Label {
                padding: Kirigami.Units.largeSpacing
                wrapMode: Text.Wrap
                opacity: 0.7
                text: "Export leaves out the ranges you deleted, compresses the rest and adds a manifest and "
                      + "checksums, in " + backend.exportsDir + ". It runs at the lowest priority "
                      + "and takes a few minutes per round; the headset stays awake until it's done. Nothing is "
                      + "uploaded until you press Upload on the Upload page."
            }
        }
    }

    // ---------------------------------------------------------------- Upload
    Component {
        id: uploadPage
        Kirigami.ScrollablePage {
            id: uploadView
            title: "Upload"
            readonly property var exported: backend.sessions.filter(s => s.exported)
            readonly property string session: exportBox.currentIndex >= 0 ? exportBox.currentValue || "" : ""
            readonly property var chosen: exportBox.currentIndex >= 0 && exportBox.currentIndex < exported.length
                                          ? exported[exportBox.currentIndex] : null
            readonly property var up: backend.upload
            readonly property var upError: up.error || ({})
            readonly property var upResult: up.result || ({})
            // The upload shown is this export's (the window keeps the last one's result).
            readonly property bool mine: up.session === session
            property var info: ({ previous: {}, uploads: 0 })
            readonly property bool loggedIn: backend.login.state === "ok"
            // Why Upload is off, or "" when it can go.
            readonly property string blocked: {
                if (session === "") return "Choose an export"
                if (chosen && chosen.export_stale) return "Export the session again first"
                if (backend.exporting) return "Wait for the export to finish"
                if (backend.uploading) return ""
                if (backend.hubDryRun) return ""
                if (!backend.uploadAllowed) return "Contributions aren't open yet"
                if (!loggedIn) return "Log in first (step 2)"
                return ""
            }

            function refreshInfo() { info = backend.uploadInfo(session) }
            onSessionChanged: refreshInfo()
            Connections {
                target: backend
                function onSessionsChanged() { uploadView.refreshInfo() }
            }
            Component.onCompleted: {
                refreshInfo()
                if (backend.login.state === "unknown") backend.checkLogin()
            }

            function startUpload() {
                if (info.previous && info.previous.export_sha)
                    confirm.ask("Upload this export again?",
                                "It was uploaded on " + info.previous.uploaded + " (" + (info.previous.pr_url || "no link")
                                + "). Uploading it again opens a second pull request.",
                                () => backend.startUpload(uploadView.session, true))
                else
                    backend.startUpload(session, false)
            }

            header: ColumnLayout {
                spacing: 0
                DraftBanner { Layout.fillWidth: true }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: backend.hubDryRun
                    position: Kirigami.InlineMessage.Position.Header
                    type: Kirigami.MessageType.Information
                    text: "Dry run (--hub-dry-run): Upload checks the export and lists what it would send. "
                          + "Nothing goes over the network."
                }
                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: backend.textsDraft && backend.allowUploadSet && !backend.hubDryRun
                    position: Kirigami.InlineMessage.Position.Header
                    type: Kirigami.MessageType.Warning
                    text: "Rehearsal: FT_HANDREC_ALLOW_UPLOAD=1 allows uploads to " + backend.dataset
                          + " while the texts are drafts."
                }
            }

            ColumnLayout {
                spacing: Kirigami.Units.largeSpacing

                Kirigami.PlaceholderMessage {
                    Layout.fillWidth: true
                    visible: uploadView.exported.length === 0
                    icon.name: "document-export"
                    text: "Nothing exported yet"
                    explanation: "Review a session, then export it. It can be uploaded from here."
                    helpfulAction: Kirigami.Action {
                        text: "Export"; icon.name: "document-export"
                        onTriggered: root.show(exportPage)
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: uploadView.exported.length > 0
                    spacing: Kirigami.Units.largeSpacing

                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        textFormat: Text.StyledText
                        text: "Your export goes to <a href=\"" + backend.datasetUrl + "\">" + backend.dataset + "</a> as a "
                              + "pull request from your own Hugging Face account. Nothing is published until the "
                              + "maintainer has checked it."
                        onLinkActivated: link => Qt.openUrlExternally(link)
                    }

                    // 1. The export
                    Kirigami.Heading { level: 3; text: "1. Choose the export" }
                    RowLayout {
                        Layout.fillWidth: true
                        Controls.ComboBox {
                            id: exportBox
                            Layout.fillWidth: true
                            Layout.maximumWidth: Kirigami.Units.gridUnit * 22
                            model: uploadView.exported
                            textRole: "label"
                            valueRole: "id"
                            enabled: !backend.uploading
                            Component.onCompleted: currentIndex = Math.max(0, indexOfValue(root.chosenSession))
                            onActivated: root.chosenSession = currentValue
                        }
                        Controls.Label {
                            opacity: 0.7
                            text: uploadView.chosen ? uploadView.chosen.exportText : ""
                        }
                    }
                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        visible: uploadView.chosen !== null && uploadView.chosen.export_stale
                        type: Kirigami.MessageType.Warning
                        text: "This session changed since it was exported. Export it again so the upload has your latest deletions."
                    }
                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        visible: uploadView.session !== "" && uploadView.info.previous.export_sha !== undefined
                        type: Kirigami.MessageType.Information
                        text: "This export was uploaded on " + (uploadView.info.previous.uploaded || "") + ": "
                              + (uploadView.info.previous.pr_url || "") + ". There's no need to upload it again."
                    }

                    // 2. The login: Log in opens Hugging Face in the browser, where the person approves
                    // it. The token goes from there to hub.py, never into this window.
                    Kirigami.Heading { level: 3; text: "2. Log in to Hugging Face" }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: loginState !== "waiting"
                        readonly property string loginState: backend.login.state
                        Kirigami.Icon {
                            visible: parent.loginState === "ok"
                            source: "checkmark"
                            implicitWidth: Kirigami.Units.iconSizes.small
                            implicitHeight: implicitWidth
                        }
                        Controls.BusyIndicator {
                            visible: ["checking", "starting", "unknown"].indexOf(parent.loginState) >= 0
                            running: visible
                            implicitWidth: Kirigami.Units.gridUnit * 1.5
                            implicitHeight: implicitWidth
                        }
                        Controls.Label {
                            Layout.fillWidth: true
                            visible: parent.loginState !== "waiting"
                            wrapMode: Text.Wrap
                            text: parent.loginState === "dry" ? "Not needed for a dry run."
                                  : parent.loginState === "unknown" ? "Checking your login"
                                  : parent.loginState === "none"
                                    ? "Press Log in. Hugging Face opens in your browser: sign in, or make a free "
                                      + "account, then enter the code this page shows."
                                  : backend.login.text || ""
                        }
                        Controls.Button {
                            visible: ["none", "read", "error", "ok"].indexOf(parent.loginState) >= 0
                            text: parent.loginState === "ok" ? "Use another account" : "Log in"
                            icon.name: parent.loginState === "ok" ? "system-switch-user" : "im-user-online"
                            flat: parent.loginState === "ok"
                            enabled: !backend.uploading
                            onClicked: backend.logIn()
                        }
                        Controls.Button {
                            visible: parent.loginState === "error"
                            text: "Check again"
                            icon.name: "view-refresh"
                            onClicked: backend.checkLogin()
                        }
                    }
                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        visible: backend.login.state === "waiting"
                        type: Kirigami.MessageType.Information
                        text: "Hugging Face is open in your browser (" + (backend.login.url || "") + "). Sign in, or "
                              + "make a free account, enter the code below and approve the login. If it asks about "
                              + "organizations, leave them unticked. The code works for "
                              + Math.round((backend.login.expires_in || 300) / 60) + " minutes; this page carries on "
                              + "by itself once you've approved."
                        actions: [openLogin, copyLogin, cancelLogin]
                        Kirigami.Action {
                            id: openLogin
                            text: "Open again"
                            icon.name: "internet-services"
                            onTriggered: Qt.openUrlExternally(backend.login.url)
                        }
                        Kirigami.Action {
                            id: copyLogin
                            text: "Copy code"
                            icon.name: "edit-copy"
                            onTriggered: backend.copy(backend.login.code)
                        }
                        Kirigami.Action {
                            id: cancelLogin
                            text: "Cancel"
                            icon.name: "dialog-cancel"
                            onTriggered: backend.cancelLogin()
                        }
                    }
                    Controls.Label {
                        visible: backend.login.state === "waiting"
                        Layout.leftMargin: Kirigami.Units.gridUnit
                        text: backend.login.code || ""
                        font.family: "monospace"
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 2.5
                        font.letterSpacing: 4
                    }

                    // 3. The upload
                    Kirigami.Heading { level: 3; text: "3. Upload" }
                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        textFormat: Text.StyledText
                        text: "The first time, accept the dataset's terms on <a href=\"" + backend.datasetUrl
                              + "\">its page</a>. Upload checks the export, opens your pull request, then sends the files."
                        onLinkActivated: link => Qt.openUrlExternally(link)
                    }
                    RowLayout {
                        Controls.Button {
                            text: backend.uploading ? "Uploading…" : backend.hubDryRun ? "Upload (dry run)" : "Upload"
                            icon.name: "cloud-upload"
                            enabled: uploadView.blocked === "" && !backend.uploading
                            onClicked: uploadView.startUpload()
                        }
                        Controls.BusyIndicator {
                            visible: backend.uploading
                            running: visible
                            implicitWidth: Kirigami.Units.gridUnit * 1.5
                            implicitHeight: implicitWidth
                        }
                        Controls.Button {
                            visible: backend.uploading
                            text: "Cancel"
                            icon.name: "dialog-cancel"
                            onClicked: backend.cancelUpload()
                        }
                        Controls.Label {
                            visible: !backend.uploading && uploadView.blocked !== ""
                            opacity: 0.7
                            text: uploadView.blocked
                        }
                    }

                    // Progress: a share while the export is checked, a sweeping bar while it's sent
                    // (huggingface_hub doesn't report progress). Drawn here, as on the Export page.
                    Rectangle {
                        id: track
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 20
                        implicitHeight: Kirigami.Units.smallSpacing * 2
                        visible: backend.uploading
                        radius: height / 2
                        clip: true
                        color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.15)
                        readonly property bool unknown: uploadView.up.fraction === undefined || uploadView.up.fraction < 0
                        Rectangle {
                            id: fill
                            height: parent.height
                            radius: parent.radius
                            color: Kirigami.Theme.highlightColor
                            width: track.unknown ? parent.width / 4 : parent.width * Math.max(0, Math.min(1, uploadView.up.fraction))
                            x: 0
                            SequentialAnimation on x {
                                running: track.visible && track.unknown
                                loops: Animation.Infinite
                                onRunningChanged: if (!running) fill.x = 0
                                NumberAnimation { from: -fill.width; to: track.width; duration: 1600 }
                            }
                        }
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        visible: uploadView.mine && (uploadView.up.text || "") !== ""
                                 && (backend.uploading || uploadView.up.phase === "failed" && uploadView.upError.kind === "cancelled")
                        wrapMode: Text.Wrap
                        text: uploadView.up.text || ""
                    }

                    // The pull request is open and the files are on their way: time to plug in.
                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        visible: backend.uploading && uploadView.mine && (uploadView.up.pr_url || "") !== ""
                        type: Kirigami.MessageType.Positive
                        text: "Your pull request is open: " + (uploadView.up.pr_url || "") + ". The files are uploading "
                              + "to it now, which can take a while. Plug in the headset and leave it plugged in until "
                              + "this page says Uploaded. You can take the headset off: it stays awake until the upload "
                              + "is done. Keep the Hand Recorder open."
                        actions: [openOpenedPr, copyOpenedPr]
                        Kirigami.Action {
                            id: openOpenedPr
                            text: "Open"
                            icon.name: "internet-services"
                            onTriggered: Qt.openUrlExternally(uploadView.up.pr_url)
                        }
                        Kirigami.Action {
                            id: copyOpenedPr
                            text: "Copy link"
                            icon.name: "edit-copy"
                            onTriggered: backend.copy(uploadView.up.pr_url)
                        }
                    }
                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        visible: !backend.uploading && uploadView.mine && uploadView.up.phase === "done"
                        type: Kirigami.MessageType.Positive
                        text: uploadView.upResult.dry_run
                              ? "Dry run: the export passed its checks. It would go to " + uploadView.upResult.repo + "/"
                                + uploadView.upResult.path_in_repo + " (" + uploadView.upResult.files + " files). Nothing was sent."
                              : "Uploaded. Your pull request: " + (uploadView.upResult.pr_url || "")
                                + ". The maintainer reviews it before it joins the dataset."
                        actions: uploadView.upResult.pr_url ? [openPr, copyPr] : []
                        Kirigami.Action {
                            id: openPr
                            text: "Open"
                            icon.name: "internet-services"
                            onTriggered: Qt.openUrlExternally(uploadView.upResult.pr_url)
                        }
                        Kirigami.Action {
                            id: copyPr
                            text: "Copy link"
                            icon.name: "edit-copy"
                            onTriggered: backend.copy(uploadView.upResult.pr_url)
                        }
                    }
                    Kirigami.InlineMessage {
                        Layout.fillWidth: true
                        visible: !backend.uploading && uploadView.mine && uploadView.up.phase === "failed"
                                 && uploadView.upError.kind !== "cancelled"
                        type: Kirigami.MessageType.Error
                        text: (uploadView.upError.text || "")
                              + ((uploadView.upError.errors || []).length ? "\n\n• " + uploadView.upError.errors.join("\n• ") : "")
                        actions: uploadView.upError.link ? [openErrorLink] : []
                        Kirigami.Action {
                            id: openErrorLink
                            text: "Open"
                            icon.name: "internet-services"
                            onTriggered: Qt.openUrlExternally(uploadView.upError.link)
                        }
                    }
                    Controls.TextArea {
                        Layout.fillWidth: true
                        visible: uploadView.mine && (uploadView.up.log || "") !== ""
                        readOnly: true
                        selectByMouse: true
                        wrapMode: Text.WrapAnywhere
                        font.family: "monospace"
                        text: uploadView.up.log || ""
                    }

                    Kirigami.Separator { Layout.fillWidth: true; visible: uploadView.session !== "" }
                    MarkdownText {
                        Layout.fillWidth: true
                        visible: uploadView.session !== ""
                        markdown: backend.uploadText(uploadView.session)
                    }
                }
            }
        }
    }
}
