import QtQuick
import QtQuick.Shapes
import QtQuick.Effects
import QtQuick.Window
import QtCore as Core
import QtQuick.Dialogs
import Qt5Compat.GraphicalEffects
import VoltaApp 1.0

Rectangle {
    id: root
    property bool lightTheme: Window.window.lightTheme
    // Main window (Main): the dock's volume is read/written from here.
    // It must be taken on an Item — inside a Connections "Window.window" is always null.
    readonly property var hostWindow: Window.window
    clip: true
    color: lightTheme ? "#FFFFFF" : "#171717"

    // Absolute block of events toward the bottom: prevents the map
    // from moving or zooming if you swipe or scroll in the settings
    Item {
        anchors.fill: parent
        z: -1
        DragHandler { target: null }
        WheelHandler { target: null; onWheel: (event) => { event.accepted = true } }
        MouseArea { anchors.fill: parent; preventStealing: true }
    }

    signal themeModeSelected(string mode, string autoStart, string autoEnd)
    signal satNightSelected(string mode)
    signal androidAutoRestartRequested()
    signal mirrorSelected(bool rightHand)
    signal lowPowerSelected(bool on)
    signal ambientColorSelected(color c)
    signal ambientModeSelected(string mode)
    signal ambientBrightnessSelected(int val)
    signal carColorSelected(color c)
    signal brightnessSelected(int val)
    signal blueLightFilterSelected(bool on)
    signal screenCleanRequested()
    signal phoneDestRequested()
    signal fullScreenMapSelected(bool on)

    readonly property color txt:      lightTheme ? "#171717" : "#FFFFFF"
    readonly property color txtSub:   lightTheme ? "#8A8A8A" : "#A2A3A5"
    readonly property color txtHint:  lightTheme ? "#ABABAB" : "#6E6F71"
    readonly property color ctrlBg:   lightTheme ? "#E3E3E3" : "#2E2E2E"
    readonly property color ctrlSel:  lightTheme ? "#FFFFFF" : "#6B6B6B"
    readonly property color ctrlBrd:  lightTheme ? "#D8D8D8" : "#454545"
    readonly property color blue:     "#1667F2"
    readonly property color divider:  lightTheme ? "#DDDDDD" : "#333333"
    readonly property color sideBg:   root.color
    readonly property color popupBg:  lightTheme ? "#FFFFFF" : "#2A2A2A"

    Core.Settings {
        id: prefs
        category: "ui"
        property string themeMode:  "dark"
        property string autoStart:  "19:00"
        property string autoEnd:    "07:00"
        property bool   rightHand:  false
        property bool   lowPower:   false
        property string ambient:    "#0A84FF"
        property string ambientMode: "on"
        property int    ambientBrightness: 100
        property string ambientPalette: ""
        property string carColor:   "#C8C9CA"
        property string carPalette: ""
        property int    brightness: 100
        property bool   blueLightFilter: false
        property string hiddenApps: ""

        // Resume the last track (or radio station) at startup — see
        // Main.qml savePlaybackState()/tryResumePlayback()
        property bool   resumePlayback: true

        // Impostazioni Navigatore
        property int    navVolume: 70
        property string navOrientation: "heading" // "heading" | "north"
        property string satNight: "auto"          // "off" | "on" | "auto" (effetto notte satellite)
        property string aaTheme: "ui"             // Android Auto: "light" | "dark" | "auto" | "ui" (like the interface)
        property bool   navAvoidTolls: false
        property bool   navAvoidFerries: false

        // "Full-screen map" mode (EV style): the car
        // panel disappears and the map takes its place — see Main.qml.
        property bool   fullScreenMap: false
    }

    SystemHelper { id: sysHelper }

    // ══════ CARPLAY / ANDROID AUTO (dongle Carlinkit, src/carlink) ══════
    // The settings live in the driver (Carlink) and are saved by it. Resolution and DPI
    // apply at connection (it restarts by itself); theme and fit apply immediately.
    readonly property var aaResolutions: [
        { label: "480p",  mode: 1 },
        { label: "720p",  mode: 2 },
        { label: "1080p", mode: 3 }
    ]
    readonly property var aaDpis: [0, 120, 160, 200, 240]          // 0 = automatico
    readonly property var aaFits: ["fit", "fill"]
    function aaSetResolution(idx) {
        Carlink.resolution = root.aaResolutions[idx].mode
        root.androidAutoRestartRequested()
    }
    function aaSetDpi(v) {
        Carlink.dpi = v
        root.androidAutoRestartRequested()
    }

    // Route preferences → navigator. Settings is created at startup
    // (inside AppPanel), so they apply even without ever opening the settings.
    Binding { target: Nav; property: "avoidTolls";   value: prefs.navAvoidTolls }
    Binding { target: Nav; property: "avoidFerries"; value: prefs.navAvoidFerries }

    Timer {
        id: startupTimer
        interval: 600
        running: true
        repeat: false
        onTriggered: {
            audioRoot.fetchAudioState()
        }
    }

    signal appVisibilitySelected(string hiddenCsv)

    function isAppHidden(id) {
        return prefs.hiddenApps.length > 0 && ("," + prefs.hiddenApps + ",").indexOf("," + id + ",") >= 0
    }
    function setAppHidden(id, hidden) {
        var list = prefs.hiddenApps.length ? prefs.hiddenApps.split(",") : []
        var idx = list.indexOf(id)
        if (hidden && idx < 0) list.push(id)
        if (!hidden && idx >= 0) list.splice(idx, 1)
        prefs.hiddenApps = list.join(",")
        root.appVisibilitySelected(prefs.hiddenApps)
    }

    property int pageIdx: 0
    property var pages: [qsTr("Display"), qsTr("Lights"), "Audio", qsTr("Navigation"), qsTr("Network"), qsTr("Bluetooth"), qsTr("App"), qsTr("CarPlay / Android Auto"), "Arcade", qsTr("System"), qsTr("Updates")]
    // the Arcade (set by AppPanel): list of games, hidden ones and quick launch
    property var arcade: null
    // Material Symbols icons to the left of the menu entries (same order as "pages")
    readonly property var pageIcons: ["desktop_windows", "lightbulb", "volume_up", "near_me", "wifi",
                                      "bluetooth", "apps", "directions_car", "sports_esports", "settings", "system_update"]
    // Audio page tab to open (0 Devices, 1 Equalizer,
    // 2 Balance) — used by the mini player's EQ button
    property int audioTabRequest: -1
    function openAudioTab(i) {
        root.pageIdx = root.pages.indexOf("Audio")
        root.audioTabRequest = i
    }
    readonly property int topBarHeight: 64

    // ── SETTINGS SEARCH ──
    // Index of the entries: title, page (index of "pages"), optional Audio tab and keywords.
    readonly property var searchIndex: [
        { t: qsTr("Appearance (light / dark / automatic theme)"), p: 0, k: qsTr("theme light dark automatic night day appearance") },
        { t: qsTr("Automatic theme (time range)"), p: 0, k: qsTr("schedule time range dark from to automatic") },
        { t: qsTr("Driving position (left / right)"), p: 0, k: qsTr("driving right left mirror interface steering wheel") },
        { t: qsTr("Reduce blue light"), p: 0, k: qsTr("blue light night filter warm eyes") },
        { t: qsTr("Arcade games (show / hide)"), p: 8, k: qsTr("arcade games hide show list bottles") },
        { t: qsTr("Guided setup (service keys)"), p: 9, k: qsTr("guided setup keys api mapbox spotify tomtom weather services connect") },
        { t: qsTr("Screen brightness"), p: 0, k: qsTr("brightness screen display") },
        { t: qsTr("Screen cleaning"), p: 0, k: qsTr("clean cleaning screen lock touch glass") },
        { t: qsTr("Ambient lights"), p: 1, k: qsTr("ambient lights color led interior brightness") },
        { t: qsTr("Resume on startup"), p: 2, k: qsTr("resume startup playback music track radio last") },
        { t: qsTr("Audio output"), p: 2, a: 0, k: qsTr("audio output speakers device") },
        { t: qsTr("Audio input (microphone)"), p: 2, a: 0, k: qsTr("input microphone device audio") },
        { t: qsTr("Equalizer"), p: 2, a: 1, k: qsTr("equalizer eq bass treble preset sound") },
        { t: qsTr("Audio balance"), p: 2, a: 2, k: qsTr("balance fader left right front rear") },
        { t: qsTr("Fullscreen map"), p: 3, k: qsTr("fullscreen map model s x navigation") },
        { t: qsTr("Avoid tolls"), p: 3, k: qsTr("tolls toll highway route avoid") },
        { t: qsTr("Avoid ferries"), p: 3, k: qsTr("ferries ferry route avoid") },
        { t: qsTr("Map view"), p: 3, k: qsTr("map view heading north up satellite style") },
        { t: qsTr("Night effect (satellite)"), p: 3, k: qsTr("night satellite dark map effect") },
        { t: qsTr("Voice guidance volume"), p: 3, k: qsTr("volume voice directions spoken navigation guidance") },
        { t: qsTr("Wi-Fi"), p: 4, k: qsTr("wifi wi-fi network internet connection wireless password") },
        { t: qsTr("Bluetooth"), p: 5, k: qsTr("bluetooth devices phone pair") },
        { t: qsTr("Hide apps"), p: 6, k: qsTr("apps hide drawer applications show") },
        { t: qsTr("On-screen keyboard"), p: 9, k: qsTr("keyboard virtual screen text") },
        { t: qsTr("Keyboard languages"), p: 9, k: qsTr("keyboard languages language layout") },
        { t: qsTr("Debug"), p: 9, k: qsTr("debug system information tools") },
        { t: qsTr("Check for updates"), p: 10, k: qsTr("update updates upgrade version new release download install") },
        { t: qsTr("Android Auto: theme"), p: 7, k: qsTr("android auto carplay theme") },
        { t: qsTr("Android Auto: screen fit"), p: 7, k: qsTr("android auto carplay fit fill adapt fullscreen") },
        { t: qsTr("Android Auto: resolution"), p: 7, k: qsTr("android auto resolution 480p 720p 1080p") },
        { t: qsTr("Android Auto: density (DPI)"), p: 7, k: qsTr("android auto density dpi size") }
    ]
    property string searchQuery: ""
    function _norm(t) {
        return (t || "").toLowerCase().normalize("NFD").replace(/[\u0300-\u036f]/g, "")
    }
    readonly property var searchResults: {
        var q = root._norm(root.searchQuery).trim()
        if (q === "") return []
        var words = q.split(/\s+/)
        var out = []
        for (var i = 0; i < root.searchIndex.length; i++) {
            var it = root.searchIndex[i]
            var hay = root._norm(it.t + " " + it.k + " " + root.pages[it.p])
            var ok = true
            for (var w = 0; w < words.length; w++)
                if (hay.indexOf(words[w]) < 0) { ok = false; break }
            if (ok) out.push(it)
        }
        return out.slice(0, 7)
    }
    function openSearchResult(it) {
        if (!it) return
        if (it.a !== undefined) root.openAudioTab(it.a)
        else root.pageIdx = it.p
        settingsSearchInput.text = ""
        settingsSearchInput.focus = false
    }

    Rectangle {
        id: searchBar
        anchors.left: parent.left; anchors.right: parent.right
        x: 0; y: 0; height: root.topBarHeight
        color: root.color

        Item {
            id: searchIconBox
            anchors.left: parent.left; anchors.leftMargin: 24
            anchors.verticalCenter: parent.verticalCenter
            width: 24; height: 24
            opacity: 0.8
            // Same icon as the map's search
            Image {
                id: settingsSearchImg
                anchors.fill: parent
                source: "qrc:/VoltaApp/icons/Icons_search_black.svg"
                sourceSize: Qt.size(48, 48)
                visible: false
            }
            ColorOverlay {
                anchors.fill: settingsSearchImg
                source: settingsSearchImg
                color: root.txt
            }
        }

        TextInput {
            id: settingsSearchInput
            anchors.left: searchIconBox.right; anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            width: 260
            color: root.txt; font.pixelSize: 14
            clip: true; selectByMouse: true
            onTextChanged: root.searchQuery = text
            Keys.onReturnPressed: root.openSearchResult(root.searchResults[0])
            Keys.onEnterPressed: root.openSearchResult(root.searchResults[0])
            Keys.onEscapePressed: { text = ""; focus = false }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: settingsSearchInput.text === ""
                text: qsTr("Search settings")
                color: root.txtHint; font.pixelSize: 14
            }
        }
    }

    // Search results (above everything else on the page)
    Rectangle {
        id: searchPopup
        x: 24; y: root.topBarHeight + 4
        width: 440
        height: root.searchQuery.trim() === "" ? 0 : (Math.max(1, root.searchResults.length) * 46 + 12)
        visible: root.searchQuery.trim() !== ""
        z: 100
        radius: 10
        color: root.popupBg
        border.width: 1; border.color: root.ctrlBrd
        Column {
            x: 6; y: 6; width: parent.width - 12
            Text {
                visible: root.searchResults.length === 0
                width: parent.width; height: 46
                verticalAlignment: Text.AlignVCenter; leftPadding: 12
                text: qsTr("No results"); color: root.txtHint; font.pixelSize: 14
            }
            Repeater {
                model: root.searchResults
                Rectangle {
                    width: parent.width; height: 46; radius: 8
                    color: _srMA2.pressed ? (root.lightTheme ? "#E3E3E3" : "#3A3A3A") : "transparent"
                    Text {
                        anchors.left: parent.left; anchors.leftMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.t; color: root.txt; font.pixelSize: 15
                        elide: Text.ElideRight; width: parent.width - 150
                    }
                    Text {
                        anchors.right: parent.right; anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.pages[modelData.p]; color: root.txtHint; font.pixelSize: 12
                    }
                    MouseArea { id: _srMA2; anchors.fill: parent; onClicked: root.openSearchResult(modelData) }
                }
            }
        }
    }

    Rectangle {
        anchors.left: parent.left; anchors.right: parent.right
        y: root.topBarHeight; height: 1
        z: 10
        color: root.divider
    }

    Rectangle {
        id: sidebar
        x: 0; y: root.topBarHeight; width: 250; height: parent.height - root.topBarHeight
        color: root.sideBg

        // Highlight that slides vertically to the chosen entry
        Rectangle {
            x: 16; width: parent.width - 32; height: 46; radius: 8
            y: 20 + root.pageIdx * (46 + 6)
            color: root.lightTheme ? "#D5D5D5" : "#3A3A3A"
            Behavior on y {
                NumberAnimation { duration: root.selAnimMs; easing.type: root.selAnimCurve; easing.overshoot: root.selAnimOvershoot }
            }
        }

        Column {
            x: 16; y: 20; width: parent.width - 32; spacing: 6

            Repeater {
                model: root.pages
                Rectangle {
                    width: parent.width; height: 46; radius: 8
                    // No gray "pressed" rectangle: the only
                    // visual feedback of the selection is the highlight
                    // that slides to the chosen entry (above, in the sidebar).
                    color: "transparent"
                    Item {
                        x: 16; anchors.verticalCenter: parent.verticalCenter
                        width: 22; height: 22
                        Image {
                            id: pgIconImg
                            anchors.fill: parent
                            source: "qrc:/VoltaApp/icons/ms_" + root.pageIcons[index] + ".svg"
                            sourceSize: Qt.size(44, 44)
                            fillMode: Image.PreserveAspectFit
                            visible: false
                        }
                        ColorOverlay {
                            anchors.fill: pgIconImg
                            source: pgIconImg
                            color: index === root.pageIdx ? root.txt : root.txtSub
                            Behavior on color { ColorAnimation { duration: root.selAnimMs } }
                        }
                    }
                    Text {
                        x: 52; anchors.verticalCenter: parent.verticalCenter
                        text: modelData
                        color: index === root.pageIdx ? root.txt : root.txtSub
                        Behavior on color { ColorAnimation { duration: root.selAnimMs } }
                        font.pixelSize: 15
                        font.weight: index === root.pageIdx ? Font.DemiBold : Font.Normal
                    }
                    MouseArea { id: _pgMA; anchors.fill: parent; onClicked: root.pageIdx = index }
                }
            }
        }
    }

    component SectionTitle: Row {
        property string title
        spacing: 6
        Text { text: title; color: root.txt; font.pixelSize: 15; font.weight: Font.Normal }
        Text { text: "ⓘ"; color: root.txtHint; font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter }
    }

    // Common duration/curve for the indicators that slide between options:
    // fast but soft (iOS/EV style), with a very slight final bounce.
    readonly property int  selAnimMs: 280
    readonly property var  selAnimCurve: Easing.OutBack
    readonly property real selAnimOvershoot: 0.9

    component TSegmented: Rectangle {
        id: seg
        property var options: []
        property int current: 0
        signal picked(int idx)
        width: Math.max(280, segRow.implicitWidth); height: 46; radius: 8
        color: root.ctrlBg
        border.width: 1; border.color: root.ctrlBrd

        readonly property real cellW: seg.options.length > 0 ? seg.width / seg.options.length : seg.width

        // ── Indicator that slides under the text ──
        Rectangle {
            id: segThumb
            visible: seg.current >= 0 && seg.current < seg.options.length
            width: seg.cellW; height: seg.height
            x: seg.current * seg.cellW
            radius: 8
            color: root.ctrlSel
            border.width: root.lightTheme ? 1 : 0
            border.color: "#E0E0E0"
            Behavior on x {
                NumberAnimation { duration: root.selAnimMs; easing.type: root.selAnimCurve; easing.overshoot: root.selAnimOvershoot }
            }
        }

        Row {
            id: segRow
            anchors.fill: parent; anchors.margins: 0
            Repeater {
                model: seg.options
                Item {
                    width: seg.cellW; height: parent.height
                    Text {
                        anchors.centerIn: parent; text: modelData
                        color: index === seg.current ? root.txt : root.txtSub
                        Behavior on color { ColorAnimation { duration: root.selAnimMs } }
                        font.pixelSize: 14
                        font.weight: index === seg.current ? Font.DemiBold : Font.Normal
                    }
                    MouseArea { anchors.fill: parent; onClicked: { seg.current = index; seg.picked(index) } }
                }
            }
        }
    }

    component TToggle: Rectangle {
        id: tgl
        property bool checked: false
        signal toggled(bool on)
        width: 52; height: 30; radius: 15
        color: checked ? root.blue : (root.lightTheme ? "#D0D0D0" : "#5A5A5A")
        Behavior on color { ColorAnimation { duration: 150 } }
        Rectangle {
            width: 24; height: 24; radius: 12; color: "#FFFFFF"
            anchors.verticalCenter: parent.verticalCenter
            x: tgl.checked ? tgl.width - width - 3 : 3
            Behavior on x { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
        }
        MouseArea { anchors.fill: parent; onClicked: { tgl.checked = !tgl.checked; tgl.toggled(tgl.checked) } }
    }

    component TPrimary: Rectangle {
        property string label
        signal clicked()
        id: pbtn
        width: Math.max(150, pTxt.implicitWidth + 48); height: 44; radius: 8
        color: _pbMA.pressed ? Qt.darker(root.blue, 1.2) : root.blue
        Text { id: pTxt; anchors.centerIn: parent; text: pbtn.label; color: "#FFFFFF"; font.pixelSize: 14; font.weight: Font.DemiBold }
        MouseArea { id: _pbMA; anchors.fill: parent; onClicked: pbtn.clicked() }
    }

    component TSecondary: Rectangle {
        property string label
        signal clicked()
        id: sbtn
        width: Math.max(150, sTxt.implicitWidth + 48); height: 44; radius: 8
        color: _sbMA.pressed ? (root.lightTheme ? "#C8C8C8" : "#5A5A5A")
                             : (root.lightTheme ? "#D5D5D5" : "#6B6B6B")
        Text { id: sTxt; anchors.centerIn: parent; text: sbtn.label; color: root.txt; font.pixelSize: 14; font.weight: Font.DemiBold }
        MouseArea { id: _sbMA; anchors.fill: parent; onClicked: sbtn.clicked() }
    }

    component TDivider: Rectangle { width: parent.width; height: 1; color: root.divider }

    component TimeField: Row {
        id: tfld
        property int hh: 19
        property int mm: 0
        spacing: 0
        function txt() { return ("0"+hh).slice(-2) + ":" + ("0"+mm).slice(-2) }
        Rectangle {
            width: 88; height: 44; radius: 8; color: root.ctrlBg
            border.width: 1; border.color: root.ctrlBrd
            Text { anchors.centerIn: parent; text: tfld.txt(); color: root.txt; font.pixelSize: 16; font.weight: Font.DemiBold }
        }
        Column {
            spacing: 2; leftPadding: 6
            Rectangle {
                width: 34; height: 21; radius: 8; color: root.ctrlBg
                Text { anchors.centerIn: parent; text: "▲"; color: root.txtSub; font.pixelSize: 9 }
                MouseArea { anchors.fill: parent; onClicked: { tfld.mm += 30; if (tfld.mm >= 60) { tfld.mm = 0; tfld.hh = (tfld.hh+1)%24 } } }
            }
            Rectangle {
                width: 34; height: 21; radius: 8; color: root.ctrlBg
                Text { anchors.centerIn: parent; text: "▼"; color: root.txtSub; font.pixelSize: 9 }
                MouseArea { anchors.fill: parent; onClicked: { tfld.mm -= 30; if (tfld.mm < 0) { tfld.mm = 30; tfld.hh = (tfld.hh+23)%24 } } }
            }
        }
    }

    Flickable {
        x: sidebar.width; y: root.topBarHeight
        width: parent.width - sidebar.width; height: parent.height - root.topBarHeight
        contentHeight: contentCol.height + 70
        clip: true; boundsBehavior: Flickable.StopAtBounds

        Column {
            id: contentCol
            x: 44; y: 34; width: parent.width - 88; spacing: 24

            // ══════ PAGINA: DISPLAY ══════
            Column {
                visible: root.pageIdx === 0
                width: parent.width; spacing: 24

                SectionTitle { title: qsTr("Appearance") }
                TSegmented {
                    id: themeSegDisplay
                    options: [qsTr("Light"), qsTr("Dark"), qsTr("Auto")]
                    current: prefs.themeMode === "light" ? 0 : (prefs.themeMode === "dark" ? 1 : 2)
                    onPicked: idx => {
                        if (idx === 2) { autoPopup.open() }
                        else {
                            prefs.themeMode = idx === 0 ? "light" : "dark"
                            root.themeModeSelected(prefs.themeMode, prefs.autoStart, prefs.autoEnd)
                        }
                    }
                }

                TDivider {}

                SectionTitle { title: qsTr("Driving position") }
                Column {
                    spacing: 10
                    TSegmented {
                        options: [qsTr("Left"), qsTr("Right")]
                        current: prefs.rightHand ? 1 : 0
                        onPicked: idx => {
                            prefs.rightHand = idx === 1
                            root.mirrorSelected(prefs.rightHand)
                        }
                    }
                    Text {
                        text: qsTr("Mirror the interface: car panel and apps move to the chosen side")
                        color: root.txtHint; font.pixelSize: 12
                    }
                }

                Row {
                    width: parent.width
                    Column {
                        width: parent.width - 60; spacing: 4
                        Text { text: qsTr("Reduce blue light"); color: root.txt; font.pixelSize: 15 }
                        Text {
                            width: parent.width
                            text: qsTr("Warmer tones in the evening (system Night Light)")
                            color: root.txtHint; font.pixelSize: 12; wrapMode: Text.WordWrap
                        }
                    }
                    TToggle {
                        anchors.verticalCenter: parent.verticalCenter
                        checked: prefs.blueLightFilter
                        onToggled: on => {
                            prefs.blueLightFilter = on;
                            root.blueLightFilterSelected(on)
                            if (on) {
                                sysHelper.runDetached("gsettings", ["set", "org.gnome.settings-daemon.plugins.color", "night-light-enabled", "true"]);
                            } else {
                                sysHelper.runDetached("gsettings", ["set", "org.gnome.settings-daemon.plugins.color", "night-light-enabled", "false"]);
                            }
                        }
                    }
                }

                TDivider {}

                SectionTitle { title: qsTr("Brightness") }
                Rectangle {
                    width: 320; height: 56; radius: 10
                    color: root.ctrlBg
                    border.width: 1; border.color: root.ctrlBrd
                    clip: true

                    Row {
                        anchors.fill: parent
                        spacing: 0

                        Item {
                            width: 44; height: 56
                            Text { text: "☼"; color: root.txtSub; font.pixelSize: 16; anchors.centerIn: parent }
                        }

                        Item {
                            id: briBar
                            width: 320 - 88; height: 56

                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width; height: 4; radius: 2
                                color: root.divider
                            }

                            Rectangle {
                                id: briThumb
                                width: 74; height: 56; radius: 10
                                color: root.lightTheme ? "#FFFFFF" : "#A2A3A5"
                                border.width: 1; border.color: root.ctrlBrd
                                anchors.verticalCenter: parent.verticalCenter
                                x: (prefs.brightness / 100) * (parent.width - width)

                                Text {
                                    anchors.centerIn: parent
                                    text: prefs.brightness + "%"
                                    color: root.lightTheme ? "#171717" : "#FFFFFF"
                                    font.pixelSize: 15; font.weight: Font.DemiBold
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                preventStealing: true
                                function upd(mx) {
                                    var safeX = Math.max(0, Math.min(mx - briThumb.width/2, briBar.width - briThumb.width))
                                    var pct = Math.round((safeX / (briBar.width - briThumb.width)) * 100)
                                    if (isNaN(pct)) pct = 0
                                    prefs.brightness = pct
                                    root.brightnessSelected(pct)
                                }
                                onPressed: (m) => upd(m.x)
                                onPositionChanged: (m) => { if (pressed) upd(m.x) }
                            }
                        }

                        Item {
                            width: 44; height: 56
                            Text { text: "☀"; color: root.txtSub; font.pixelSize: 22; anchors.centerIn: parent }
                        }
                    }
                }

                TDivider {}

                TSecondary {
                    label: qsTr("Screen cleaning mode")
                    onClicked: root.screenCleanRequested()
                }
                Text {
                    text: qsTr("Locks the touch for a few seconds while you clean the glass")
                    color: root.txtHint; font.pixelSize: 12
                }

            }

            // ══════ PAGINA: LUCI AMBIENTALI ══════
            Column {
                visible: root.pageIdx === 1
                width: parent.width; spacing: 24

                SectionTitle { title: qsTr("Ambient lights") }

                Row {
                    width: parent.width; spacing: 14

                    TSegmented {
                        width: 320
                        options: [qsTr("Off"), "On", qsTr("Night only")]
                        current: prefs.ambientMode === "off" ? 0 : (prefs.ambientMode === "on" ? 1 : 2)
                        onPicked: (idx) => {
                            var m = idx === 0 ? "off" : (idx === 1 ? "on" : "night")
                            prefs.ambientMode = m
                            root.ambientModeSelected(m)
                        }
                    }

                    Item {
                        width: 46; height: 46
                        anchors.verticalCenter: parent.verticalCenter

                        Canvas {
                            id: smallWheelCanvas
                            anchors.fill: parent
                            anchors.margins: 4
                            onPaint: {
                                var ctx = getContext("2d")
                                var cx = width / 2; var cy = height / 2; var r = Math.min(cx, cy)
                                ctx.clearRect(0, 0, width, height)

                                for (var a = 0; a < 360; a += 2) {
                                    ctx.beginPath()
                                    ctx.moveTo(cx, cy)
                                    ctx.arc(cx, cy, r, (a - 1) * Math.PI / 180, (a + 2) * Math.PI / 180)
                                    ctx.closePath()
                                    ctx.fillStyle = Qt.hsva(a / 360.0, 1.0, 1.0, 1.0)
                                    ctx.fill()
                                }

                                var grad = ctx.createRadialGradient(cx, cy, 0, cx, cy, r)
                                grad.addColorStop(0, qsTr("rgba(255, 255, 255, 1.0)"))
                                grad.addColorStop(1, qsTr("rgba(255, 255, 255, 0.0)"))
                                ctx.fillStyle = grad
                                ctx.beginPath()
                                ctx.arc(cx, cy, r, 0, 2 * Math.PI)
                                ctx.fill()
                            }
                            onWidthChanged: requestPaint()
                            Component.onCompleted: requestPaint()
                        }

                        Rectangle {
                            anchors.fill: parent; radius: 10
                            color: "transparent"
                            border.width: 1; border.color: root.lightTheme ? "#D0D0D0" : "#454545"
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: ambientColorPopup.open()
                        }
                    }
                }

                Text {
                    visible: prefs.ambientMode === "night"
                    text: qsTr("Active from ") + prefs.autoStart + " alle " + prefs.autoEnd + qsTr(" (same range as the automatic theme, in Display)")
                    color: root.txtHint; font.pixelSize: 12
                }

                Rectangle {
                    width: 320; height: 56; radius: 10
                    color: root.ctrlBg
                    border.width: 1; border.color: root.ctrlBrd
                    clip: true

                    Row {
                        anchors.fill: parent
                        spacing: 0

                        Item {
                            width: 44; height: 56
                            Text { text: "☼"; color: root.txtSub; font.pixelSize: 16; anchors.centerIn: parent }
                        }

                        Item {
                            id: ambBrightBar
                            width: 320 - 88; height: 56

                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width; height: 4; radius: 2
                                color: root.divider
                            }

                            Rectangle {
                                id: ambThumb
                                width: 74; height: 56; radius: 10
                                color: root.lightTheme ? "#FFFFFF" : "#A2A3A5"
                                border.width: 1; border.color: root.ctrlBrd
                                anchors.verticalCenter: parent.verticalCenter
                                x: (prefs.ambientBrightness / 100) * (parent.width - width)

                                Text {
                                    anchors.centerIn: parent
                                    text: prefs.ambientBrightness + "%"
                                    color: root.lightTheme ? "#171717" : "#FFFFFF"
                                    font.pixelSize: 15; font.weight: Font.DemiBold
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                preventStealing: true
                                function upd(mx) {
                                    var safeX = Math.max(0, Math.min(mx - ambThumb.width/2, ambBrightBar.width - ambThumb.width))
                                    var pct = Math.round((safeX / (ambBrightBar.width - ambThumb.width)) * 100)
                                    if (isNaN(pct)) pct = 0
                                    prefs.ambientBrightness = pct
                                    root.ambientBrightnessSelected(pct)
                                }
                                onPressed: (m) => upd(m.x)
                                onPositionChanged: (m) => { if (pressed) upd(m.x) }
                            }
                        }

                        Item {
                            width: 44; height: 56
                            Text { text: "☀"; color: root.txtSub; font.pixelSize: 22; anchors.centerIn: parent }
                        }
                    }
                }
            }

            // ══════ WI-FI (Rete, indice 4) ══════
            Column {
                visible: root.pageIdx === 4
                width: parent.width; spacing: 14
                SectionTitle { title: qsTr("Network") }
                WifiSettings {
                    width: parent.width
                    txt: root.txt; txtSub: root.txtSub; txtHint: root.txtHint
                    ctrlBg: root.ctrlBg; ctrlBrd: root.ctrlBrd; blue: root.blue
                    divider: root.divider; popupBg: root.popupBg; lightTheme: root.lightTheme
                }
            }

            // ══════ BLUETOOTH (indice 5) ══════
            Column {
                visible: root.pageIdx === 5
                width: parent.width; spacing: 14
                SectionTitle { title: qsTr("Bluetooth") }
                BluetoothSettings {
                    width: parent.width
                    txt: root.txt; txtSub: root.txtSub; txtHint: root.txtHint
                    ctrlBg: root.ctrlBg; ctrlBrd: root.ctrlBrd; blue: root.blue
                    divider: root.divider; lightTheme: root.lightTheme
                }
            }

            // ══════ PAGINA: AUDIO ══════
            Column {
                visible: root.pageIdx === 2
                width: parent.width; spacing: 20

                SectionTitle { title: qsTr("Playback") }
                Row {
                    width: parent.width
                    Column {
                        width: parent.width - 110; spacing: 4
                        Text { text: qsTr("Resume on startup"); color: root.txt; font.pixelSize: 15 }
                        Text {
                            width: parent.width
                            text: qsTr("Resumes the last track or radio station")
                            color: root.txtHint; font.pixelSize: 12; wrapMode: Text.WordWrap
                        }
                    }
                    TToggle {
                        anchors.verticalCenter: parent.verticalCenter
                        checked: prefs.resumePlayback
                        onToggled: on => prefs.resumePlayback = on
                    }
                }

                TDivider {}

                Rectangle {
                    width: 480; height: 46; radius: 10
                    color: root.ctrlBg; border.width: 1; border.color: root.ctrlBrd
                    clip: true
                    // Indicator that slides between tabs
                    Rectangle {
                        width: 480 / 3; height: parent.height; radius: 10
                        x: audioRoot.eqTab * (480 / 3)
                        color: root.ctrlSel
                        Behavior on x {
                            NumberAnimation { duration: root.selAnimMs; easing.type: root.selAnimCurve; easing.overshoot: root.selAnimOvershoot }
                        }
                    }
                    Row {
                        anchors.fill: parent
                        Repeater {
                            model: [qsTr("Devices"), qsTr("Equalizer"), qsTr("Balance")]
                            Item {
                                width: 480 / 3; height: parent.height
                                Text {
                                    anchors.centerIn: parent; text: modelData
                                    color: index === audioRoot.eqTab ? root.txt : root.txtSub
                                    Behavior on color { ColorAnimation { duration: root.selAnimMs } }
                                    font.pixelSize: 13; font.weight: index === audioRoot.eqTab ? Font.DemiBold : Font.Normal
                                }
                                MouseArea { anchors.fill: parent; onClicked: audioRoot.eqTab = index }
                            }
                        }
                    }
                }

                Item {
                    id: audioRoot
                    property int eqTab: 0
                    function _takeTabRequest() {
                        if (root.audioTabRequest >= 0) { audioRoot.eqTab = root.audioTabRequest; root.audioTabRequest = -1 }
                    }
                    Component.onCompleted: _takeTabRequest()
                    Connections {
                        target: root
                        function onAudioTabRequestChanged() { audioRoot._takeTabRequest() }
                    }
                    property var bandLabels: ["SUB", "BASS", "", "MID", "", "TREBLE"]
                    width: 1; height: 1

                    property bool eqAvailable: false
                    property bool eqDirty: false
                    property var bandGains: [0, 0, 0, 0, 0, 0]
                    property var bandFreqs: [60, 150, 400, 1000, 2500, 8000]
                    property string eqPresetName: ""
                    property var eqPresets: []

                    function fetchEqState() {
                        audioRoot.eqAvailable = sysHelper.eqAvailable()
                        if (!audioRoot.eqAvailable) return
                        sysHelper.setEqEnabled(true)
                        audioRoot.eqPresetName = sysHelper.eqCurrentPreset() || ""
                        audioRoot.eqPresets = sysHelper.eqPresetList()
                        var bands = sysHelper.eqBands()
                        var gains = [], freqs = []
                        for (var i = 0; i < bands.length; i++) { gains.push(bands[i].gain); freqs.push(bands[i].frequency) }
                        audioRoot.bandGains = gains
                        audioRoot.bandFreqs = freqs
                        audioRoot.eqDirty = false
                    }
                    function loadEqPreset(name) {
                        if (sysHelper.eqLoadPreset(name)) audioRoot.fetchEqState()
                    }
                    function saveEqPreset(name) {
                        if (sysHelper.eqSaveAsPreset(name)) {
                            audioRoot.fetchEqState()
                            audioRoot.eqPresetName = name
                            audioRoot.eqDirty = false
                        }
                    }
                    function deleteEqPreset(name) {
                        // Before, the C++ function did not exist: the "typeof" check
                        // failed silently and the preset file stayed on disk,
                        // so it reappeared in the dropdown menu.
                        if (!sysHelper.eqDeletePreset(name)) return
                        // fetchEqState re-reads the list of files and the preset now in use
                        audioRoot.fetchEqState()
                    }

                    property int eqPendingBand: -1
                    function setBandGainLive(band, db) {
                        var arr = audioRoot.bandGains.slice()
                        arr[band] = db
                        audioRoot.bandGains = arr
                        audioRoot.eqPendingBand = band
                        audioRoot.eqDirty = true
                        eqCommitTimer.restart()
                    }
                    Timer {
                        id: eqCommitTimer
                        interval: 150
                        onTriggered: {
                            if (audioRoot.eqPendingBand >= 0)
                                sysHelper.setEqBandGain(audioRoot.eqPendingBand, audioRoot.bandGains[audioRoot.eqPendingBand])
                        }
                    }
                    function resetEqBands() {
                        for (var i = 0; i < audioRoot.bandGains.length; i++) sysHelper.setEqBandGain(i, 0)
                        audioRoot.bandGains = [0, 0, 0, 0, 0, 0]
                    }

                    property bool audioAvailable: false
                    property var audioState: ({ sinks: [], sources: [], defaultSink: "", defaultSource: "", volume: 100, balance: 0.0 })

                    function fetchAudioState() {
                        var sinks = sysHelper.listAudioSinks()
                        audioRoot.audioAvailable = sinks.length > 0
                        var defSink = sysHelper.defaultAudioSink()
                        if (defSink.indexOf("easyeffects") >= 0 && sinks.length > 0) defSink = sinks[0].name
                        audioRoot.audioState = {
                            sinks: sinks,
                            sources: sysHelper.listAudioSources(),
                            defaultSink: defSink,
                            defaultSource: sysHelper.defaultAudioSource(),
                            volume: sysHelper.getVolume(),
                            balance: sysHelper.getAudioBalance()
                        }
                        // Same value as the dock / banner volume
                        if (root.hostWindow && root.hostWindow.volume !== undefined)
                            root.hostWindow.volume = audioRoot.audioState.volume
                    }
                    // Volume changed from the dock (‹ ›, mute) or from the banner: updates the bar here
                    Connections {
                        target: root.hostWindow
                        ignoreUnknownSignals: true
                        function onVolumeChanged() {
                            var v = root.hostWindow.volume
                            if (audioRoot.audioState.volume !== v) {
                                audioRoot.audioState.volume = v
                                audioRoot.audioStateChanged()
                            }
                        }
                    }

                    function sinkLabel(name) {
                        for (var i = 0; i < audioState.sinks.length; i++) if (audioState.sinks[i].name === name) return audioState.sinks[i].label
                        return name || "—"
                    }
                    function sourceLabel(name) {
                        for (var i = 0; i < audioState.sources.length; i++) if (audioState.sources[i].name === name) return audioState.sources[i].label
                        return name || "—"
                    }
                    function selectSink(name) {
                        sysHelper.setDefaultAudioSink(name)
                        audioState.defaultSink = name; audioStateChanged()
                    }
                    function selectSource(name) {
                        sysHelper.setDefaultAudioSource(name)
                        audioState.defaultSource = name; audioStateChanged()
                    }
                    function setVolumeLive(pct) {
                        audioState.volume = pct; audioStateChanged()
                        sysHelper.setVolume(pct)
                        // Aligns the interface volume (dock, banner, mute)
                        if (root.hostWindow && root.hostWindow.volume !== undefined) root.hostWindow.volume = pct
                    }
                    function setBalanceLive(v) {
                        audioState.balance = v; audioStateChanged()
                        sysHelper.setAudioBalance(v)
                    }
                    function testSink() { sysHelper.testAudioOutput() }
                }

                Rectangle {
                    visible: audioRoot.eqTab === 0
                    width: 480; radius: 8
                    color: "transparent"
                    height: devCol.height
                    Component.onCompleted: audioRoot.fetchAudioState()

                    Column {
                        id: devCol
                        x: 0; y: 0; width: parent.width; spacing: 22

                        Text {
                            visible: !audioRoot.audioAvailable
                            width: parent.width
                            text: qsTr("No audio devices detected: pactl isn't installed or found no outputs.")
                            color: root.txtHint; font.pixelSize: 12; wrapMode: Text.WordWrap
                        }

                        Column {
                            visible: audioRoot.audioAvailable
                            width: parent.width; spacing: 14

                            SectionTitle { title: qsTr("Output") }

                            Rectangle {
                                width: parent.width; height: 56; radius: 10
                                color: root.ctrlBg; border.width: 1; border.color: root.ctrlBrd
                                Text {
                                    anchors.left: parent.left; anchors.leftMargin: 18
                                    anchors.right: parent.right; anchors.rightMargin: 48
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: audioRoot.sinkLabel(audioRoot.audioState.defaultSink)
                                    color: root.txt; font.pixelSize: 15; elide: Text.ElideRight
                                }
                                Text { anchors.right: parent.right; anchors.rightMargin: 18; anchors.verticalCenter: parent.verticalCenter; text: "\u2304"; color: root.txtSub; font.pixelSize: 20 }
                                MouseArea { anchors.fill: parent; onClicked: devicePopup.open("sink") }
                            }

                            Rectangle {
                                id: volBar
                                width: parent.width; height: 56; radius: 10
                                color: root.ctrlBg; border.width: 1; border.color: root.ctrlBrd
                                clip: true
                                // same bar as the brightness: small icon on the left, large on the right, knob with the percentage
                                Shape {
                                    x: 12; y: 18; width: 20; height: 20
                                    ShapePath {
                                        fillColor: root.txtSub; strokeColor: "transparent"; fillRule: ShapePath.WindingFill
                                        PathSvg { path: "M7,9V15H11L16,20V4L11,9H7Z" }
                                    }
                                    transform: Scale { xScale: 20 / 24; yScale: 20 / 24 }
                                }
                                Shape {
                                    x: parent.width - 36; y: 16; width: 24; height: 24
                                    ShapePath {
                                        fillColor: root.txtSub; strokeColor: "transparent"; fillRule: ShapePath.WindingFill
                                        PathSvg { path: "M14,3.23V5.29C16.89,6.15 19,8.83 19,12C19,15.17 16.89,17.84 14,18.7V20.77C18,19.86 21,16.28 21,12C21,7.72 18,4.14 14,3.23M16.5,12C16.5,10.23 15.5,8.71 14,7.97V16C15.5,15.29 16.5,13.76 16.5,12M3,9V15H7L12,20V4L7,9H3Z" }
                                    }
                                }
                                Item {
                                    id: volTrack
                                    x: 44; width: parent.width - 88; height: 56
                                    Rectangle {
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: parent.width; height: 4; radius: 2
                                        color: root.divider
                                    }
                                    Rectangle {
                                        id: volThumb
                                        width: 74; height: 56; radius: 10
                                        color: root.lightTheme ? "#FFFFFF" : "#A2A3A5"
                                        border.width: 1; border.color: root.ctrlBrd
                                        anchors.verticalCenter: parent.verticalCenter
                                        x: (audioRoot.audioState.volume / 100) * (parent.width - width)
                                        Text {
                                            anchors.centerIn: parent
                                            text: Math.round(audioRoot.audioState.volume) + "%"
                                            color: root.lightTheme ? "#171717" : "#FFFFFF"
                                            font.pixelSize: 15; font.weight: Font.DemiBold
                                        }
                                    }
                                    MouseArea {
                                        id: volMA
                                        anchors.fill: parent; preventStealing: true
                                        function upd(mx) {
                                            var sx = Math.max(0, Math.min(mx - volThumb.width / 2, volTrack.width - volThumb.width))
                                            var pct = Math.round((sx / (volTrack.width - volThumb.width)) * 100)
                                            if (isNaN(pct)) pct = 0
                                            audioRoot.setVolumeLive(pct)
                                        }
                                        onPressed: (m) => upd(m.x)
                                        onPositionChanged: (m) => { if (pressed) upd(m.x) }
                                    }
                                }
                            }
                        }

                        TDivider { visible: audioRoot.audioAvailable }

                        Column {
                            visible: audioRoot.audioAvailable
                            width: parent.width; spacing: 10
                            SectionTitle { title: qsTr("Input") }
                            Rectangle {
                                width: parent.width; height: 56; radius: 10
                                color: root.ctrlBg; border.width: 1; border.color: root.ctrlBrd
                                Text {
                                    anchors.left: parent.left; anchors.leftMargin: 18
                                    anchors.right: parent.right; anchors.rightMargin: 48
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: audioRoot.audioState.sources.length === 0
                                          ? qsTr("No input device")
                                          : audioRoot.sourceLabel(audioRoot.audioState.defaultSource)
                                    color: audioRoot.audioState.sources.length === 0 ? root.txtHint : root.txt
                                    font.pixelSize: 15; elide: Text.ElideRight
                                }
                                Text { visible: audioRoot.audioState.sources.length > 0; anchors.right: parent.right; anchors.rightMargin: 18; anchors.verticalCenter: parent.verticalCenter; text: "\u2304"; color: root.txtSub; font.pixelSize: 20 }
                                MouseArea {
                                    anchors.fill: parent
                                    enabled: audioRoot.audioState.sources.length > 0
                                    onClicked: devicePopup.open("source")
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    visible: audioRoot.eqTab === 1
                    width: 480; radius: 8
                    color: root.lightTheme ? "#DCDCDC" : "#242424"
                    height: eqCol.height + 40 // Margins adapted to the real content
                    Component.onCompleted: audioRoot.fetchEqState()

                    Column {
                        id: eqCol
                        x: 20; y: 20; width: parent.width - 40; spacing: 24

                        Text {
                            visible: !audioRoot.eqAvailable
                            width: parent.width
                            text: qsTr("Equalizer unavailable: install EasyEffects and create a preset.")
                            color: root.txtHint; font.pixelSize: 12; wrapMode: Text.WordWrap
                        }

                        Column {
                            visible: audioRoot.eqAvailable
                            width: parent.width; spacing: 24

                            // Top Bar: Selezione Preset e Azioni (Salva / Elimina)
                            Item {
                                width: parent.width; height: 36

                                Rectangle {
                                    width: 160; height: 36; radius: 8
                                    color: root.lightTheme ? "#D5D5D5" : "#3A3A3A"
                                    border.width: 1; border.color: root.ctrlBrd
                                    Row {
                                        anchors.fill: parent; anchors.margins: 12; spacing: 8
                                        Text {
                                            text: audioRoot.eqPresetName !== "" ? audioRoot.eqPresetName : qsTr("Custom")
                                            color: root.txt; font.pixelSize: 14; font.weight: Font.DemiBold
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: parent.width - 24; elide: Text.ElideRight
                                        }
                                        Text { text: "⌄"; color: root.txtSub; font.pixelSize: 16; anchors.verticalCenter: parent.verticalCenter }
                                    }
                                    MouseArea { anchors.fill: parent; onClicked: presetDropdownPopup.open() }
                                }

                                Row {
                                    anchors.right: parent.right; spacing: 10
                                    TPrimary {
                                        visible: audioRoot.eqDirty && audioRoot.eqPresetName !== ""
                                        width: 80; height: 36
                                        label: qsTr("Save")
                                        onClicked: {
                                            audioRoot.saveEqPreset(audioRoot.eqPresetName)
                                            audioRoot.eqDirty = false
                                        }
                                    }
                                    TSecondary {
                                        // Hidden if it is the only preset: without presets the EQ stops working
                                        visible: audioRoot.eqPresetName !== "" && audioRoot.eqPresets.length > 1
                                        width: 80; height: 36
                                        label: qsTr("Delete")
                                        onClicked: audioRoot.deleteEqPreset(audioRoot.eqPresetName)
                                    }
                                }
                            }

                            // Valori in alto (es. +6.0 ... dB)
                            Item {
                                width: parent.width - 34 // Reserves the final space on the right
                                height: 20
                                Row {
                                    anchors.fill: parent
                                    spacing: (parent.width - (6 * 34)) / 5
                                    Repeater {
                                        model: audioRoot.bandGains.length
                                        Item {
                                            width: 34; height: 20
                                            Text {
                                                anchors.centerIn: parent
                                                text: (audioRoot.bandGains[index] > 0 ? "+" : "") + audioRoot.bandGains[index].toFixed(1)
                                                color: root.txt; font.pixelSize: 13; font.weight: Font.DemiBold
                                            }
                                        }
                                    }
                                }
                                Text {
                                    anchors.right: parent.right; anchors.rightMargin: -28
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "dB"; color: root.txtSub; font.pixelSize: 12; font.weight: Font.DemiBold
                                }
                            }

                            // Slider with a central zero line
                            Item {
                                width: parent.width; height: 160

                                // Continuous horizontal line at 0 dB that cuts across the knobs
                                Rectangle {
                                    width: parent.width - 34; height: 2
                                    color: root.lightTheme ? "#A0A0A0" : "#555555"
                                    y: 80
                                }

                                // Reset EQ button positioned on the axis of the 0 dB line on the right
                                Item {
                                    width: 34; height: 34
                                    anchors.right: parent.right
                                    y: 80 - 17 // centered on row Y:80
                                    Text {
                                        anchors.centerIn: parent
                                        text: "⟲"
                                        color: root.txtSub
                                        font.pixelSize: 22
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        onClicked: { audioRoot.resetEqBands(); audioRoot.eqDirty = true }
                                    }
                                }

                                Row {
                                    width: parent.width - 34
                                    height: parent.height
                                    spacing: (width - (6 * 34)) / 5

                                    Repeater {
                                        model: audioRoot.bandGains.length
                                        Item {
                                            width: 34; height: 160

                                            Rectangle {
                                                anchors.horizontalCenter: parent.horizontalCenter
                                                width: 2; height: parent.height
                                                color: root.lightTheme ? "#A0A0A0" : "#555555"
                                            }

                                            Rectangle {
                                                id: bandHandle
                                                width: 16; height: 16; radius: 8
                                                color: "#FFFFFF"
                                                anchors.horizontalCenter: parent.horizontalCenter
                                                y: (1 - (audioRoot.bandGains[index] + 12) / 24) * (160 - height)
                                            }

                                            MouseArea {
                                                anchors.fill: parent
                                                anchors.margins: -10
                                                preventStealing: true
                                                function updateFromY(py) {
                                                    var t = 1 - Math.max(0, Math.min(1, py / 160))
                                                    var db = Math.round((t * 24 - 12) * 2) / 2
                                                    audioRoot.setBandGainLive(index, db)
                                                }
                                                onPressed: (m) => updateFromY(m.y)
                                                onPositionChanged: (m) => { if (pressed) updateFromY(m.y) }
                                            }
                                        }
                                    }
                                }
                            }

                            // Etichette basse (SUB, BASS, MID, TREBLE)
                            Item {
                                width: parent.width - 34
                                height: 20
                                Row {
                                    anchors.fill: parent
                                    spacing: (parent.width - (6 * 34)) / 5
                                    Repeater {
                                        model: audioRoot.bandLabels
                                        Item {
                                            width: 34; height: 20
                                            Text {
                                                anchors.centerIn: parent
                                                text: modelData
                                                color: root.txtSub; font.pixelSize: 11; font.weight: Font.DemiBold
                                                font.letterSpacing: 0.5
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    visible: audioRoot.eqTab === 2
                    width: 480
                    height: cabinDiagram.height // No extra margin! Only the photo.
                    radius: 8
                    color: root.lightTheme ? "#DCDCDC" : "#242424"
                    clip: true
                    Component.onCompleted: audioRoot.fetchAudioState()

                    Item {
                        id: cabinDiagram
                        width: parent.width
                        height: width * (cabinPhoto.sourceSize.height > 0 ? cabinPhoto.sourceSize.height / cabinPhoto.sourceSize.width : 0.55)
                        anchors.centerIn: parent

                        readonly property real trackLFrac: 0.16
                        readonly property real trackRFrac: 0.84
                        readonly property real trackYFrac: 0.5

                        Image {
                            id: cabinPhoto
                            anchors.fill: parent
                            fillMode: Image.PreserveAspectCrop
                            smooth: true
                            asynchronous: true
                            source: "qrc:/VoltaApp/images/cabin_balance.png"
                        }

                        Rectangle {
                            width: 3; radius: 1.5
                            height: cabinDiagram.height
                            color: "#FFFFFF"; opacity: 0.55
                            x: balTrack.x + balDot.x + balDot.width / 2 - width / 2
                        }

                        Item {
                            id: balTrack
                            x: cabinDiagram.width * cabinDiagram.trackLFrac
                            y: cabinDiagram.height * cabinDiagram.trackYFrac
                            width: cabinDiagram.width * (cabinDiagram.trackRFrac - cabinDiagram.trackLFrac)
                            height: 1

                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width; height: 3; radius: 1.5
                                color: "#FFFFFF"; opacity: 0.55
                            }

                            Rectangle {
                                anchors.centerIn: parent
                                width: 30; height: 30; radius: 15
                                color: "transparent"
                                border.width: 3; border.color: "#FFFFFF"
                            }

                            Rectangle {
                                id: balDot
                                width: 28; height: 28; radius: 14
                                color: "#FFFFFF"; opacity: 0.55
                                border.width: 2; border.color: "#FFFFFF"
                                anchors.verticalCenter: parent.verticalCenter
                                x: ((audioRoot.audioState.balance + 1) / 2) * (parent.width - width)
                                Behavior on x { enabled: !balMouse.pressed; NumberAnimation { duration: 120; easing.type: Easing.OutQuad } }
                            }
                        }

                        MouseArea {
                            id: balMouse
                            anchors.fill: cabinDiagram
                            preventStealing: true
                            readonly property real snapPx: 14
                            function upd(mx) {
                                var localX = mx - balTrack.x
                                var half = balTrack.width / 2
                                if (Math.abs(localX - half) <= snapPx) {
                                    audioRoot.setBalanceLive(0)
                                    return
                                }
                                audioRoot.setBalanceLive(Math.max(-1, Math.min(1, (localX / balTrack.width) * 2 - 1)))
                            }
                            onPressed: (m) => upd(m.x)
                            onPositionChanged: (m) => { if (pressed) upd(m.x) }
                        }
                    }
                }
            }

            // ══════ NAVIGAZIONE ══════
            Column {
                visible: root.pageIdx === 3
                width: parent.width; spacing: 24

                SectionTitle { title: qsTr("Navigation") }

                Column {
                    width: parent.width; spacing: 14

                    Row {
                        width: parent.width
                        Column {
                            width: parent.width - 60; spacing: 4
                            Text { text: qsTr("Fullscreen mode (Model S/X style)"); color: root.txt; font.pixelSize: 15 }
                            Text {
                                width: parent.width
                                text: qsTr("The car panel disappears and the map takes its place: fullscreen when no app is open, blurred in the background when you open one")
                                color: root.txtHint; font.pixelSize: 12; wrapMode: Text.WordWrap
                            }
                        }
                        TToggle {
                            anchors.verticalCenter: parent.verticalCenter
                            checked: prefs.fullScreenMap
                            onToggled: on => {
                                prefs.fullScreenMap = on
                                root.fullScreenMapSelected(on)
                            }
                        }
                    }
                    TDivider {}

                    Row {
                        width: parent.width
                        Column {
                            width: parent.width - 60; spacing: 4
                            Text { text: qsTr("Avoid tolls"); color: root.txt; font.pixelSize: 15 }
                            Text { text: qsTr("Don't include toll roads in the route"); color: root.txtHint; font.pixelSize: 12; wrapMode: Text.WordWrap }
                        }
                        TToggle {
                            anchors.verticalCenter: parent.verticalCenter
                            checked: prefs.navAvoidTolls
                            onToggled: on => { prefs.navAvoidTolls = on }
                        }
                    }
                    TDivider {}
                    Row {
                        width: parent.width
                        Column {
                            width: parent.width - 60; spacing: 4
                            Text { text: qsTr("Avoid ferries"); color: root.txt; font.pixelSize: 15 }
                            Text { text: qsTr("Don't use ferry connections"); color: root.txtHint; font.pixelSize: 12; wrapMode: Text.WordWrap }
                        }
                        TToggle {
                            anchors.verticalCenter: parent.verticalCenter
                            checked: prefs.navAvoidFerries
                            onToggled: on => { prefs.navAvoidFerries = on }
                        }
                    }
                }

                TDivider {}

                SectionTitle { title: qsTr("Map view") }
                Column {
                    width: parent.width; spacing: 14

                    TSegmented {
                        width: 320
                        options: [qsTr("Heading"), qsTr("North")]
                        current: prefs.navOrientation === "heading" ? 0 : 1
                        onPicked: idx => {
                            prefs.navOrientation = (idx === 0) ? "heading" : "north"
                        }
                    }
                }

                TDivider {}

                SectionTitle { title: qsTr("Night effect (satellite)") }
                Column {
                    width: parent.width; spacing: 10
                    TSegmented {
                        width: 320
                        options: [qsTr("Off"), "On", qsTr("Auto")]
                        current: prefs.satNight === "off" ? 0 : (prefs.satNight === "on" ? 1 : 2)
                        onPicked: idx => {
                            prefs.satNight = idx === 0 ? "off" : (idx === 1 ? "on" : "auto")
                            root.satNightSelected(prefs.satNight)
                        }
                    }
                    Text {
                        width: parent.width
                        text: qsTr("Darkens the satellite map. Auto: same hours as the automatic theme (") + prefs.autoStart + " – " + prefs.autoEnd + ")"
                        color: root.txtHint; font.pixelSize: 12; wrapMode: Text.WordWrap
                    }
                }

                TDivider {}

                SectionTitle { title: qsTr("GPS receiver") }
                Text {
                    width: parent.width
                    text: Gps.connected
                          ? qsTr("Connected: ") + Gps.source
                          : qsTr("No receiver connected. Plug in a USB GPS or pair one over Bluetooth: it's recognized automatically.")
                    color: Gps.connected ? root.txt : root.txtHint; font.pixelSize: 14; wrapMode: Text.WordWrap
                }
                Row {
                    spacing: 12
                    TSecondary { label: qsTr("Automatic"); opacity: Gps.btSelected === "" ? 1 : 0.6; onClicked: Gps.selectAuto() }
                    TSecondary { label: qsTr("Find Bluetooth GPS"); onClicked: Gps.refreshBtDevices() }
                }
                Text {
                    visible: Gps.btDevices.length > 0
                    width: parent.width
                    text: qsTr("Paired Bluetooth devices: tap one to use it as GPS")
                    color: root.txtHint; font.pixelSize: 12; wrapMode: Text.WordWrap
                }
                Column {
                    width: parent.width; spacing: 8
                    Repeater {
                        model: Gps.btDevices
                        Rectangle {
                            width: parent.width; height: 48; radius: 8
                            color: Gps.btSelected === modelData.mac ? root.blue : root.ctrlBg
                            border.width: 1; border.color: root.ctrlBrd
                            Text {
                                x: 16; anchors.verticalCenter: parent.verticalCenter
                                text: modelData.name + (modelData.gps ? "   (GPS)" : "")
                                color: Gps.btSelected === modelData.mac ? "#FFFFFF" : root.txt; font.pixelSize: 15
                            }
                            MouseArea { anchors.fill: parent; onClicked: Gps.selectBluetooth(modelData.mac) }
                        }
                    }
                }

                TDivider {}

                SectionTitle { title: qsTr("Voice guidance volume") }
                Rectangle {
                    width: 320; height: 56; radius: 10
                    color: root.ctrlBg
                    border.width: 1; border.color: root.ctrlBrd
                    clip: true
                    Row {
                        anchors.fill: parent; spacing: 0
                        Item {
                            width: 44; height: 56
                            Text { text: "🔈"; color: root.txtSub; font.pixelSize: 16; anchors.centerIn: parent }
                        }
                        Item {
                            id: navVolBar
                            width: 320 - 88; height: 56
                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width; height: 4; radius: 2; color: root.divider
                            }
                            Rectangle {
                                id: navVolThumb
                                width: 74; height: 56; radius: 10
                                color: root.lightTheme ? "#FFFFFF" : "#A2A3A5"
                                border.width: 1; border.color: root.ctrlBrd
                                anchors.verticalCenter: parent.verticalCenter
                                x: (prefs.navVolume / 100) * (parent.width - width)
                                Text {
                                    anchors.centerIn: parent
                                    text: prefs.navVolume + "%"
                                    color: root.lightTheme ? "#171717" : "#FFFFFF"
                                    font.pixelSize: 15; font.weight: Font.DemiBold
                                }
                            }
                            MouseArea {
                                anchors.fill: parent; preventStealing: true
                                function upd(mx) {
                                    var safeX = Math.max(0, Math.min(mx - navVolThumb.width/2, navVolBar.width - navVolThumb.width))
                                    var pct = Math.round((safeX / (navVolBar.width - navVolThumb.width)) * 100)
                                    if (isNaN(pct)) pct = 0
                                    prefs.navVolume = pct
                                }
                                onPressed: (m) => upd(m.x)
                                onPositionChanged: (m) => { if (pressed) upd(m.x) }
                            }
                        }
                        Item {
                            width: 44; height: 56
                            Text { text: "🔊"; color: root.txtSub; font.pixelSize: 22; anchors.centerIn: parent }
                        }
                    }
                }
            }

            // ══════ PAGINA: APP ══════
            Column {
                visible: root.pageIdx === 6
                width: parent.width; spacing: 14

                SectionTitle { title: qsTr("App") }
                Text {
                    width: parent.width
                    text: qsTr("Hide the apps you don't use: they disappear from the drawer but stay installed, and you can always bring them back from here.")
                    color: root.txtHint; font.pixelSize: 12; wrapMode: Text.WordWrap
                }

                Column {
                    width: parent.width; spacing: 0
                    Repeater {
                        model: [
                            { id: "theater",     name: "Theater", icon: "qrc:/VoltaApp/icons/video.svg" },
                            { id: "androidauto", name: "CarPlay / Android Auto", icon: "qrc:/VoltaApp/icons/androidauto.svg" },
                            { id: "arcade",       name: "Arcade", icon: "qrc:/VoltaApp/icons/arcade.svg" },
                            { id: "radio",        name: "Radio", icon: "qrc:/VoltaApp/icons/radio.svg" },
                            { id: "browser",      name: "Browser", icon: "qrc:/VoltaApp/icons/chrome.svg" },
                            { id: "messages",     name: qsTr("Messages"), icon: "qrc:/VoltaApp/icons/messages.svg" },
                            { id: "spotify",      name: "YT Music", icon: "qrc:/VoltaApp/icons/ytmusic.svg" },
                            { id: "spotifyapp",   name: "Spotify", icon: "qrc:/VoltaApp/icons/spotify.svg" },
                            { id: "localmusic",   name: qsTr("Music"), icon: "qrc:/VoltaApp/icons/localmusic.svg" },
                            { id: "calendar",     name: qsTr("Calendar"), icon: "data:image/svg+xml;utf8," + encodeURIComponent('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="1.886 1.886 60.227 60.227">' + '<defs><linearGradient id="a" x1="32" y1="6" x2="32" y2="58" gradientUnits="userSpaceOnUse"><stop offset="0" stop-color="#FFFFFF"/><stop offset="1" stop-color="#D0D0D0"/></linearGradient>' + '<linearGradient id="b" x1="32" y1="6" x2="32" y2="16" gradientUnits="userSpaceOnUse"><stop offset="0" stop-color="#E53935"/><stop offset="1" stop-color="#C62828"/></linearGradient>' + '<clipPath id="c"><rect x="6" y="6" width="52" height="52" rx="5"/></clipPath></defs>' + '<rect x="6" y="6" width="52" height="52" rx="5" fill="url(#a)" stroke="#A0A0A0" stroke-width="1"/>' + '<rect x="6" y="6" width="52" height="10" fill="url(#b)" clip-path="url(#c)"/>' + '<text x="32" y="44" font-family="Arial, Helvetica, sans-serif" font-size="28" font-weight="900" fill="#1A1A1A" text-anchor="middle">' + new Date().getDate() + '</text></svg>') },
                            { id: "phone",        name: qsTr("Phone"), icon: "qrc:/VoltaApp/icons/phone.svg" },
                            { id: "toybox",       name: "Toybox", icon: "qrc:/VoltaApp/icons/toybox.svg" },
                            { id: "camera",       name: "Camera", icon: "qrc:/VoltaApp/icons/camera.svg" },
                            { id: "bluetooth",    name: "Bluetooth", icon: "qrc:/VoltaApp/icons/bluetooth.svg" }
                        ]
                        Rectangle {
                            width: parent.width; height: 56
                            color: "transparent"
                            Image {
                                id: appIconSrc
                                anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                                width: 36; height: 36
                                source: modelData.icon || ""
                                sourceSize: Qt.size(108, 108); fillMode: Image.PreserveAspectFit; smooth: true; mipmap: true
                            }
                            Text {
                                anchors.left: parent.left; anchors.leftMargin: 52; anchors.verticalCenter: parent.verticalCenter
                                text: modelData.name; color: root.txt; font.pixelSize: 15
                            }
                            TToggle {
                                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                                checked: !root.isAppHidden(modelData.id)
                                onToggled: on => root.setAppHidden(modelData.id, !on)
                            }
                            TDivider { anchors.bottom: parent.bottom }
                        }
                    }
                }
            }

            // ══════ PAGINA: CARPLAY / ANDROID AUTO ══════
            Column {
                visible: root.pageIdx === 7
                width: parent.width; spacing: 24

                SectionTitle { title: qsTr("Connection") }
                Column {
                    width: parent.width; spacing: 10
                    TSegmented {
                        width: 560
                        options: [qsTr("Carlinkit dongle"), qsTr("Android Auto: USB cable"), qsTr("Android Auto: Wi-Fi")]
                        current: Carlink.source === "usb" ? 1 : Carlink.source === "wireless" ? 2 : 0
                        onPicked: idx => Carlink.source = ["dongle", "usb", "wireless"][idx]
                    }
                    Text {
                        width: parent.width
                        text: Carlink.source === "usb"
                              ? qsTr("Android Auto without a dongle: plug the phone into a USB port of this computer. Android only; CarPlay needs the dongle.")
                              : Carlink.source === "wireless"
                                ? qsTr("Android Auto without a dongle, over Wi-Fi. Needs Bluetooth and a Wi-Fi adapter that can create a network. Android only; CarPlay needs the dongle.")
                                : qsTr("CarPlay and Android Auto through a Carlinkit dongle plugged into a USB port.")
                        color: root.txtHint; font.pixelSize: 12; wrapMode: Text.WordWrap
                    }
                }

                TDivider {}

                SectionTitle { title: qsTr("Theme") }
                Column {
                    width: parent.width; spacing: 10
                    TSegmented {
                        width: 460
                        options: [qsTr("Light"), qsTr("Dark"), qsTr("Automatic")]
                        current: Carlink.theme === "light" ? 0 : Carlink.theme === "dark" ? 1 : 2
                        onPicked: idx => Carlink.theme = ["light", "dark", "ui"][idx]
                    }
                    Text {
                        width: parent.width
                        text: qsTr("Applies immediately. \"Automatic\" follows this interface's light/dark theme.")
                        color: root.txtHint; font.pixelSize: 12; wrapMode: Text.WordWrap
                    }
                }

                TDivider {}

                SectionTitle { title: qsTr("Fit in the small panel") }
                Column {
                    width: parent.width; spacing: 10
                    TSegmented {
                        width: 300
                        options: [qsTr("Automatic"), qsTr("Fill")]
                        current: Math.max(0, root.aaFits.indexOf(Carlink.fitSmall))
                        onPicked: idx => Carlink.fitSmall = root.aaFits[idx]
                    }
                    Text {
                        width: parent.width
                        text: qsTr("The video is always 16:9. \"Automatic\" shows it whole with two bars; \"Fill\" stretches it to the edges (it distorts a little).")
                        color: root.txtHint; font.pixelSize: 12; wrapMode: Text.WordWrap
                    }
                }

                SectionTitle { title: qsTr("Fullscreen fit") }
                TSegmented {
                    width: 300
                    options: [qsTr("Automatic"), qsTr("Fill")]
                    current: Math.max(0, root.aaFits.indexOf(Carlink.fitFull))
                    onPicked: idx => Carlink.fitFull = root.aaFits[idx]
                }

                TDivider {}

                SectionTitle { title: qsTr("Resolution") }
                Column {
                    width: parent.width; spacing: 10
                    TSegmented {
                        width: 330
                        options: root.aaResolutions.map(function(r) { return r.label })
                        current: Carlink.resolution - 1
                        onPicked: idx => root.aaSetResolution(idx)
                    }
                    Text {
                        width: parent.width
                        text: qsTr("1080p is the sharpest; 720p and 480p are lighter on the computer. Android Auto only accepts these three sizes.")
                        color: root.txtHint; font.pixelSize: 12; wrapMode: Text.WordWrap
                    }
                }

                TDivider {}

                SectionTitle { title: qsTr("Density (DPI)") }
                Column {
                    width: parent.width; spacing: 10
                    TSegmented {
                        width: 480
                        options: root.aaDpis.map(function(d) { return d === 0 ? qsTr("Auto") : String(d) })
                        current: Math.max(0, root.aaDpis.indexOf(Carlink.dpi))
                        onPicked: idx => root.aaSetDpi(root.aaDpis[idx])
                    }
                    Text {
                        width: parent.width
                        text: qsTr("Auto chooses based on the resolution (240 at 1080p). Lower = more content on screen, small icons and text; higher = everything bigger.")
                        color: root.txtHint; font.pixelSize: 12; wrapMode: Text.WordWrap
                    }
                }

                TDivider {}

                Row {
                    spacing: 16
                    TSecondary {
                        label: qsTr("Reconnect the phone")
                        onClicked: root.androidAutoRestartRequested()
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Resolution and DPI apply on reconnection (it starts by itself when you change them)")
                        color: root.txtHint; font.pixelSize: 12
                    }
                }
            }

            FileDialog {
                id: artPicker
                property var game: null
                property string kind: "cover"
                title: kind === "cover" ? qsTr("Choose the cover") : qsTr("Choose the hero image")
                nameFilters: [qsTr("Images (*.png *.jpg *.jpeg *.webp)")]
                onAccepted: if (root.arcade && game) root.arcade.setCustomArt(game, kind, selectedFile.toString().replace("file://", ""))
            }

            // ══════ PAGINA: ARCADE ══════
            Column {
                visible: root.pageIdx === root.pages.indexOf("Arcade")
                width: parent.width; spacing: 24

                SectionTitle { title: "Arcade" }

                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: qsTr("Choose which ones to show in the Arcade. Games you add in Bottles appear here by themselves.")
                    color: root.txtHint; font.pixelSize: 12
                }
                Column {
                    width: parent.width; spacing: 8
                    Repeater {
                        model: root.arcade ? root.arcade.allGames : []
                        Rectangle {
                            id: gRow
                            width: parent.width; height: 72; radius: 8
                            color: root.ctrlBg
                            readonly property bool shown: root.arcade ? !root.arcade.isHidden(modelData.key) : true
                            // small cover
                            Rectangle {
                                id: gThumbBox
                                x: 12; anchors.verticalCenter: parent.verticalCenter
                                width: 40; height: 54; radius: 5; clip: true
                                color: root.lightTheme ? "#D0D0D0" : "#1F1F1F"
                                Text {        // without a cover: the game's initial
                                    anchors.centerIn: parent
                                    visible: gThumb.status !== Image.Ready
                                    text: (modelData.name || "?").charAt(0).toUpperCase()
                                    color: root.txtSub; font.pixelSize: 20; font.weight: Font.DemiBold
                                }
                                Image {
                                    id: gThumb
                                    anchors.fill: parent
                                    source: root.arcade ? root.arcade.coverUrl(modelData) : ""
                                    sourceSize: Qt.size(80, 108)
                                    fillMode: Image.PreserveAspectCrop; asynchronous: true; smooth: true
                                }
                            }
                            // a single button: opens the popup to choose cover and hero
                            Rectangle {
                                id: gArtBtns
                                anchors.right: gToggle.left; anchors.rightMargin: 16
                                anchors.verticalCenter: parent.verticalCenter
                                width: 44; height: 44; radius: 22
                                color: _gaMA.pressed ? (root.lightTheme ? "#C8C8C8" : "#5A5A5A") : (root.lightTheme ? "#D5D5D5" : "#6B6B6B")
                                Image {
                                    id: gImgIco
                                    anchors.centerIn: parent; width: 22; height: 22
                                    source: "qrc:/VoltaApp/icons/image.svg"; sourceSize: Qt.size(66, 66)
                                    visible: false
                                }
                                MultiEffect { anchors.fill: gImgIco; source: gImgIco; colorization: 1.0; colorizationColor: root.txt }
                                MouseArea { id: _gaMA; anchors.fill: parent
                                    onClicked: { artDlg.game = gRow.modelRef; artDlg.visible = true } }
                            }
                            property var modelRef: modelData
                            Column {
                                anchors.left: gThumbBox.right; anchors.leftMargin: 14
                                anchors.right: gArtBtns.left; anchors.rightMargin: 16
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 3
                                Text { width: parent.width; elide: Text.ElideRight; text: modelData.name
                                       color: parent.parent.shown ? root.txt : root.txtSub; font.pixelSize: 15 }
                                Text { width: parent.width; elide: Text.ElideRight
                                       text: qsTr("Bottles · ") + (modelData.bottle || root.arcade.bottlesBottle)
                                       color: root.txtHint; font.pixelSize: 12 }
                            }
                            TToggle {
                                id: gToggle
                                anchors.right: parent.right; anchors.rightMargin: 16
                                anchors.verticalCenter: parent.verticalCenter
                                checked: parent.shown
                                onToggled: on => { if (root.arcade) root.arcade.setHidden(modelData.key, !on) }
                            }
                        }
                    }
                }
            }

            // ══════ PAGINA: SISTEMA ══════
            Column {
                visible: root.pageIdx === 9
                width: parent.width; spacing: 24

                SectionTitle { title: qsTr("System") }
                Text {
                    text: qsTr("System information and tools — section being expanded.")
                    color: root.txtHint; font.pixelSize: 12
                }

                TDivider {}

                SectionTitle { title: qsTr("3D car") }
                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: qsTr("Load another model in place of the current one, or redo the setup of doors, hood, trunk, wheels and color.")
                    color: root.txtHint; font.pixelSize: 12
                }
                Flow {
                    width: parent.width; spacing: 12
                    TSecondary { label: qsTr("Load new model"); onClicked: CarModels.requestReload() }
                    TSecondary { label: qsTr("Repeat setup"); onClicked: CarModels.requestSetup() }
                }

                TDivider {}

                SectionTitle { title: qsTr("Performance") }
                Row {
                    width: parent.width
                    Column {
                        width: parent.width - 60; spacing: 4
                        Text { text: qsTr("Low performance mode"); color: root.txt; font.pixelSize: 15 }
                        Text {
                            width: parent.width
                            text: qsTr("Removes the car reflection, panel blurs and other heavy effects: the UI gets lighter on less powerful computers")
                            color: root.txtHint; font.pixelSize: 12; wrapMode: Text.WordWrap
                        }
                    }
                    TToggle {
                        anchors.verticalCenter: parent.verticalCenter
                        checked: prefs.lowPower
                        onToggled: on => { prefs.lowPower = on; root.lowPowerSelected(on) }
                    }
                }

                TDivider {}

                SectionTitle { title: qsTr("Language") }
                Item {
                    width: parent.width; height: langPickS.open ? 60 + 6 + 5 * 56 + 8 : 60
                    LanguagePicker {
                        id: langPickS
                        ctrlBg: root.ctrlBg; selBg: root.popupBg; txt: root.txt; txtSub: root.txtSub; divider: root.divider
                    }
                }

                TDivider {}

                SectionTitle { title: qsTr("Connected services") }
                Row {
                    width: parent.width
                    Column {
                        width: parent.width - setupBtn.width - 20; spacing: 4
                        Text { text: qsTr("Guided setup"); color: root.txt; font.pixelSize: 15 }
                        Text {
                            width: parent.width
                            text: qsTr("Connect or change maps, search, weather and Spotify, step by step")
                            color: root.txtHint; font.pixelSize: 12; wrapMode: Text.WordWrap
                        }
                    }
                    TPrimary {
                        id: setupBtn
                        anchors.verticalCenter: parent.verticalCenter
                        label: qsTr("Open")
                        onClicked: Setup.open()
                    }
                }

                TDivider {}

                SectionTitle { title: qsTr("On-screen keyboard") }
                Row {
                    width: parent.width
                    Column {
                        width: parent.width - 60; spacing: 4
                        Text { text: qsTr("On-screen keyboard"); color: root.txt; font.pixelSize: 15 }
                        Text {
                            width: parent.width
                            text: qsTr("Show the on-screen keyboard when you tap a text field")
                            color: root.txtHint; font.pixelSize: 12; wrapMode: Text.WordWrap
                        }
                    }
                    TToggle {
                        anchors.verticalCenter: parent.verticalCenter
                        checked: KeyboardSettings.keyboardEnabled
                        onToggled: on => KeyboardSettings.keyboardEnabled = on
                    }
                }

                Item {
                    width: parent.width; height: 52
                    Text {
                        anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Select keyboard languages"); color: root.txt; font.pixelSize: 15
                    }
                    Text {
                        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                        text: KeyboardSettings.enabledLanguageCodes.length + qsTr(" selected  ↗")
                        color: root.txtSub; font.pixelSize: 13
                    }
                    MouseArea { anchors.fill: parent; onClicked: kbLanguagesPopup.open() }
                }
                Text {
                    width: parent.width
                    text: qsTr("The languages enabled here appear in the menu that opens when you tap the globe icon 🌐 on the keyboard")
                    color: root.txtHint; font.pixelSize: 12; wrapMode: Text.WordWrap
                }

                TDivider {}

                SectionTitle { title: qsTr("Debug") }
                TSecondary {
                    label: qsTr("Open terminal")
                    onClicked: sysHelper.runDetached("/bin/sh", ["-c",
                        "for t in x-terminal-emulator foot alacritty kitty gnome-terminal konsole xfce4-terminal xterm; do " +
                        "command -v \"$t\" >/dev/null 2>&1 && { setsid \"$t\" >/tmp/terminal.log 2>&1 & disown; exit 0; }; " +
                        "done; echo 'no terminal found in PATH' > /tmp/terminal.log"
                    ])
                }
            }

            // ══════ PAGE: UPDATES ══════
            Column {
                id: updatesPage
                visible: root.pageIdx === 10
                width: parent.width; spacing: 18
                // looks for a new version the first time the page is opened
                onVisibleChanged: if (visible && Updates.state === "idle") Updates.check()

                readonly property string errorText: {
                    switch (Updates.errorCode) {
                    case "network":   return qsTr("Could not reach GitHub. Check the internet connection and try again.")
                    case "norelease": return qsTr("No release has been published yet.")
                    case "noasset":   return qsTr("The new release has no AppImage to download.")
                    case "nospace":   return qsTr("Not enough free space to download the update.")
                    case "write":     return qsTr("Could not save the update on the disk.")
                    case "badfile":   return qsTr("The downloaded file is damaged: nothing was changed. Try again.")
                    }
                    return qsTr("Something went wrong with the update.")
                }
                readonly property bool busy: Updates.state === "checking" || Updates.state === "downloading"

                SectionTitle { title: qsTr("Updates") }

                Row {
                    spacing: 8
                    Text { text: qsTr("Installed version"); color: root.txtSub; font.pixelSize: 14 }
                    Text { text: Updates.currentVersion; color: root.txt; font.pixelSize: 14; font.weight: Font.DemiBold }
                }

                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    color: Updates.state === "error" ? "#E5484D" : root.txt
                    font.pixelSize: 16; font.weight: Font.DemiBold
                    text: {
                        switch (Updates.state) {
                        case "checking":    return qsTr("Looking for updates…")
                        case "uptodate":    return qsTr("Volta OS is up to date.")
                        case "available":   return qsTr("Version %1 is available.").arg(Updates.latestVersion)
                        case "downloading": return qsTr("Downloading version %1…").arg(Updates.latestVersion)
                        case "ready":       return qsTr("Version %1 is installed. Restart Volta OS to start using it.").arg(Updates.latestVersion)
                        case "error":       return updatesPage.errorText
                        }
                        return ""
                    }
                }

                Text {
                    visible: Updates.state === "available" && Updates.downloadSize > 0
                    text: qsTr("Download size: %1 MB").arg(Math.round(Updates.downloadSize / 1048576))
                    color: root.txtHint; font.pixelSize: 12
                }

                // download progress
                Column {
                    visible: Updates.state === "downloading"
                    width: parent.width; spacing: 6
                    Rectangle {
                        width: parent.width; height: 8; radius: 4; color: root.ctrlBg
                        Rectangle {
                            width: parent.width * Updates.progress; height: parent.height; radius: 4; color: root.blue
                        }
                    }
                    Text { text: Math.round(Updates.progress * 100) + " %"; color: root.txtSub; font.pixelSize: 12 }
                }

                // release notes
                Column {
                    visible: (Updates.state === "available" || Updates.state === "ready") && Updates.notes !== ""
                    width: parent.width; spacing: 8
                    Text { text: qsTr("What's new"); color: root.txt; font.pixelSize: 15 }
                    Rectangle {
                        width: parent.width; height: Math.min(220, notesText.implicitHeight + 24); radius: 8
                        color: root.ctrlBg
                        Flickable {
                            anchors.fill: parent; anchors.margins: 12
                            contentHeight: notesText.implicitHeight
                            clip: true; boundsBehavior: Flickable.StopAtBounds
                            Text {
                                id: notesText
                                width: parent.width
                                text: Updates.notes; textFormat: Text.MarkdownText
                                wrapMode: Text.WordWrap; color: root.txtSub; font.pixelSize: 13
                            }
                        }
                    }
                }

                // a source build or a copy that is not an AppImage cannot replace itself
                Text {
                    visible: Updates.state === "available" && !Updates.canInstall
                    width: parent.width; wrapMode: Text.WordWrap
                    text: qsTr("This copy was not started from the AppImage, so it cannot update itself. Download the new version from the releases page.")
                    color: root.txtSub; font.pixelSize: 13
                }

                Flow {
                    width: parent.width; spacing: 12
                    TPrimary {
                        visible: Updates.state === "available" && Updates.canInstall
                        label: qsTr("Install update")
                        onClicked: Updates.install()
                    }
                    TPrimary {
                        visible: Updates.state === "available" && !Updates.canInstall
                        label: qsTr("Open releases page")
                        onClicked: Qt.openUrlExternally(Updates.releasePage)
                    }
                    TPrimary {
                        visible: Updates.state === "ready"
                        label: qsTr("Restart now")
                        onClicked: Updates.restart()
                    }
                    TSecondary {
                        visible: Updates.state === "downloading"
                        label: qsTr("Cancel")
                        onClicked: Updates.cancel()
                    }
                    TSecondary {
                        visible: !updatesPage.busy && Updates.state !== "ready"
                        label: qsTr("Check again")
                        onClicked: Updates.check()
                    }
                }

                Text {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: qsTr("Your settings, keys and sign-ins are kept. The new version is checked before it replaces the old one: if the download is interrupted, nothing changes.")
                    color: root.txtHint; font.pixelSize: 12
                }
            }
        }
    }

    // ═══════════════ POPUP: MENU TENDINA PRESET EQ ═══════════════
    Item {
        id: presetDropdownPopup
        anchors.fill: parent
        visible: false
        z: 500
        function open() { visible = true }
        function close() { visible = false }

        Rectangle {
            anchors.fill: parent; color: "#99000000"
            // Dimmed background only. Clicking around closes it
            MouseArea { anchors.fill: parent; onClicked: presetDropdownPopup.close() }
        }

        Rectangle {
            anchors.centerIn: parent
            width: 360; radius: 8; color: root.popupBg
            height: presetListCol.height + 32
            MouseArea { anchors.fill: parent } // Prevents closing when clicking inside the menu

            Column {
                id: presetListCol
                x: 16; y: 16; width: parent.width - 32; spacing: 4

                Row {
                    width: parent.width
                    Text { text: qsTr("Select preset"); color: root.txt; font.pixelSize: 17; font.weight: Font.Normal; bottomPadding: 10 }
                    Item { width: parent.width - 220; height: 1 }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "✕"; color: root.txtSub; font.pixelSize: 20
                        MouseArea { anchors.fill: parent; anchors.margins: -10; onClicked: presetDropdownPopup.close() }
                    }
                }

                Repeater {
                    model: audioRoot.eqPresets
                    Rectangle {
                        width: parent.width; height: 46; radius: 8
                        color: _prMA.pressed ? (root.lightTheme ? "#DDDDDD" : "#2E2E2E") : (modelData === audioRoot.eqPresetName ? root.ctrlBg : "transparent")
                        Text {
                            x: 12; anchors.verticalCenter: parent.verticalCenter
                            text: modelData; color: root.txt; font.pixelSize: 14
                            font.weight: modelData === audioRoot.eqPresetName ? Font.DemiBold : Font.Normal
                        }
                        MouseArea {
                            id: _prMA
                            anchors.fill: parent
                            onClicked: {
                                audioRoot.loadEqPreset(modelData)
                                audioRoot.eqDirty = false
                                presetDropdownPopup.close()
                            }
                        }
                    }
                }

                Rectangle { width: parent.width; height: 1; color: root.divider; anchors.margins: 4 }

                Rectangle {
                    width: parent.width; height: 46; radius: 8
                    color: _newPrMA.pressed ? (root.lightTheme ? "#DDDDDD" : "#2E2E2E") : "transparent"
                    Text {
                        x: 12; anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("+ Create new preset...")
                        color: root.blue; font.pixelSize: 14; font.weight: Font.DemiBold
                    }
                    MouseArea {
                        id: _newPrMA
                        anchors.fill: parent
                        onClicked: {
                            presetDropdownPopup.close()
                            newPresetPopup.open()
                        }
                    }
                }
            }
        }
    }

    // ═══════════════ POPUP: SELEZIONA LINGUE TASTIERA ═══════════════
    Item {
        id: kbLanguagesPopup
        anchors.fill: parent
        visible: false
        z: 500
        function open() { visible = true }
        function close() { visible = false }

        Rectangle {
            anchors.fill: parent; color: "#99000000"
            MouseArea { anchors.fill: parent; onClicked: kbLanguagesPopup.close() }
        }

        Rectangle {
            anchors.centerIn: parent
            width: 380; radius: 8; color: root.popupBg
            height: kbLangCol.height + 32
            MouseArea { anchors.fill: parent } // Does not close when clicking inside the menu

            Column {
                id: kbLangCol
                x: 16; y: 16; width: parent.width - 32; spacing: 4

                Row {
                    width: parent.width
                    Text { text: qsTr("Keyboard languages"); color: root.txt; font.pixelSize: 17; font.weight: Font.Normal; bottomPadding: 10 }
                    Item { width: parent.width - 220; height: 1 }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "✕"; color: root.txtSub; font.pixelSize: 20
                        MouseArea { anchors.fill: parent; anchors.margins: -10; onClicked: kbLanguagesPopup.close() }
                    }
                }

                Text {
                    width: parent.width
                    text: qsTr("At least one language always stays active")
                    color: root.txtHint; font.pixelSize: 12; wrapMode: Text.WordWrap
                    bottomPadding: 6
                }

                Repeater {
                    model: KeyboardSettings.languages
                    Rectangle {
                        width: parent.width; height: 50
                        color: "transparent"
                        Text {
                            anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                            text: modelData.name; color: root.txt; font.pixelSize: 14
                        }
                        TToggle {
                            anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                            checked: KeyboardSettings.isLanguageEnabled(modelData.code)
                            onToggled: on => KeyboardSettings.setLanguageEnabled(modelData.code, on)
                        }
                        TDivider { anchors.bottom: parent.bottom }
                    }
                }
            }
        }
    }

    // ═══════════════ POPUP: FASCIA ORARIA TEMA AUTO ═══════════════
    Item {
        id: autoPopup
        anchors.fill: parent
        visible: false
        z: 500
        function open() { visible = true }
        function close() { visible = false }

        Rectangle {
            anchors.fill: parent; color: "#99000000"
            // We do not close when clicking on the background
            MouseArea { anchors.fill: parent }
        }

        Rectangle {
            anchors.centerIn: parent
            width: 420; height: 280; radius: 8
            color: root.popupBg
            MouseArea { anchors.fill: parent }

            Column {
                anchors.fill: parent; anchors.margins: 28; spacing: 20

                Row {
                    width: parent.width
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Automatic theme"); color: root.txt; font.pixelSize: 19; font.weight: Font.Normal
                    }
                    Item { width: parent.width - 180; height: 1 }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "✕"; color: root.txtSub; font.pixelSize: 20
                        MouseArea { anchors.fill: parent; anchors.margins: -10; onClicked: autoPopup.close() }
                    }
                }

                Text {
                    text: qsTr("The dark theme turns on during the chosen time range")
                    color: root.txtSub; font.pixelSize: 13
                }

                Row {
                    spacing: 20
                    Column {
                        spacing: 8
                        Text { text: qsTr("DARK FROM"); color: root.txtHint; font.pixelSize: 11; font.weight: Font.ExtraBold; font.letterSpacing: 1 }
                        TimeField {
                            id: startField
                            Component.onCompleted: {
                                var p = prefs.autoStart.split(":"); hh = parseInt(p[0]); mm = parseInt(p[1])
                            }
                        }
                    }
                    Column {
                        spacing: 8
                        Text { text: qsTr("TO"); color: root.txtHint; font.pixelSize: 11; font.weight: Font.ExtraBold; font.letterSpacing: 1 }
                        TimeField {
                            id: endField
                            Component.onCompleted: {
                                var p = prefs.autoEnd.split(":"); hh = parseInt(p[0]); mm = parseInt(p[1])
                            }
                        }
                    }
                }

                Row {
                    spacing: 12
                    anchors.right: parent.right
                    TSecondary { label: qsTr("Cancel"); onClicked: autoPopup.close() }
                    TPrimary {
                        label: qsTr("Done")
                        onClicked: {
                            prefs.autoStart = startField.txt()
                            prefs.autoEnd   = endField.txt()
                            prefs.themeMode = "auto"
                            root.themeModeSelected("auto", prefs.autoStart, prefs.autoEnd)
                            autoPopup.close()
                        }
                    }
                }
            }
        }
    }

    // (The car's color is now chosen from Toybox › Colorizer)

    // ═══════════════ POPUP: AMBIENT LIGHTS WITH WHEEL ═══════════════
    Item {
        id: ambientColorPopup
        anchors.fill: parent
        visible: false
        z: 500
        property real hue: 0.6
        property real sat: 1.0
        readonly property color wheelColor: Qt.hsva(hue, sat, 1.0, 1.0)

        readonly property var defaultPalette: ["#FFFFFF", "#0A84FF", "#BF5AF2", "#FF2D75", "#FF3B30", "#FF9500", "#34C759", "#0088FF"]
        readonly property var customPalette: {
            var arr = prefs.ambientPalette.length ? prefs.ambientPalette.split(",") : []
            return arr.slice(Math.max(0, arr.length - 7))
        }

        function open() {
            visible = true
            bigWheelCanvas.requestPaint()
        }
        function close() { visible = false }

        function addToPalette(hex) {
            if (defaultPalette.indexOf(hex) >= 0) return
            var list = prefs.ambientPalette.length ? prefs.ambientPalette.split(",") : []
            var idx = list.indexOf(hex)
            if (idx >= 0) {
                list.splice(idx, 1)
                list.push(hex)
            } else {
                list.push(hex)
                if (list.length > 7) list.shift()
            }
            prefs.ambientPalette = list.join(",")
        }

        function pick(hex) {
            prefs.ambient = hex
            root.ambientColorSelected(hex)
        }

        Rectangle {
            anchors.fill: parent; color: "#99000000"
            MouseArea { anchors.fill: parent }
        }

        Rectangle {
            anchors.centerIn: parent
            width: 640; height: 380; radius: 8
            color: root.popupBg
            MouseArea { anchors.fill: parent }

            Item {
                width: parent.width; height: 50
                Text {
                    x: 20; anchors.verticalCenter: parent.verticalCenter
                    text: "✕"; color: root.txtSub; font.pixelSize: 20
                    MouseArea { anchors.fill: parent; anchors.margins: -10; onClicked: ambientColorPopup.close() }
                }
                Text {
                    anchors.centerIn: parent
                    text: qsTr("Ambient light color"); color: root.txt; font.pixelSize: 18; font.weight: Font.DemiBold
                }
            }

            Rectangle { y: 50; width: parent.width; height: 1; color: root.divider }

            Item {
                y: 70; x: 24; width: parent.width - 48; height: 260

                Column {
                    id: gridContainer
                    width: 280; anchors.verticalCenter: parent.verticalCenter
                    spacing: 16

                    Grid {
                        columns: 4; spacing: 14
                        Repeater {
                            model: ambientColorPopup.defaultPalette
                            Rectangle {
                                width: 48; height: 48; radius: 8
                                color: modelData
                                border.width: Qt.colorEqual(prefs.ambient, modelData) ? 3 : (modelData.toString().toUpperCase() === "#FFFFFF" ? 1 : 0)
                                border.color: Qt.colorEqual(prefs.ambient, modelData) ? root.blue : root.txtHint
                                MouseArea { anchors.fill: parent; onClicked: ambientColorPopup.pick(modelData) }
                            }
                        }
                    }

                    Rectangle { width: parent.width; height: 1; color: root.divider }

                    Grid {
                        columns: 4; spacing: 14
                        Repeater {
                            model: ambientColorPopup.customPalette
                            Rectangle {
                                width: 48; height: 48; radius: 8
                                color: modelData
                                border.width: Qt.colorEqual(prefs.ambient, modelData) ? 3 : (modelData.toString().toUpperCase() === "#FFFFFF" ? 1 : 0)
                                border.color: Qt.colorEqual(prefs.ambient, modelData) ? root.blue : root.txtHint
                                MouseArea { anchors.fill: parent; onClicked: ambientColorPopup.pick(modelData) }
                            }
                        }
                        Rectangle {
                            width: 48; height: 48; radius: 8
                            color: "transparent"
                            border.width: 1.5; border.color: root.txtSub
                            Text { anchors.centerIn: parent; text: "+"; color: root.txtSub; font.pixelSize: 24 }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: {
                                    var hex = Qt.rgba(ambientColorPopup.wheelColor.r, ambientColorPopup.wheelColor.g, ambientColorPopup.wheelColor.b, 1).toString().substring(0, 7).toUpperCase()
                                    ambientColorPopup.addToPalette(hex)
                                    ambientColorPopup.pick(hex)
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    width: 1; height: 240; color: root.divider
                    anchors.left: gridContainer.right; anchors.leftMargin: 20
                    anchors.verticalCenter: parent.verticalCenter
                }

                Item {
                    id: wheelBox
                    width: 240; height: 240
                    anchors.right: parent.right; anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter

                    Canvas {
                        id: bigWheelCanvas
                        anchors.fill: parent
                        onPaint: {
                            var ctx = getContext("2d"); var cx = width / 2; var cy = height / 2; var r = Math.min(cx, cy); ctx.clearRect(0, 0, width, height)
                            for (var a = 0; a < 360; a += 1) { ctx.beginPath(); ctx.moveTo(cx, cy); ctx.arc(cx, cy, r, (a - 0.5) * Math.PI / 180, (a + 1.5) * Math.PI / 180); ctx.closePath(); ctx.fillStyle = Qt.hsva(a / 360.0, 1.0, 1.0, 1.0); ctx.fill() }
                            var grad = ctx.createRadialGradient(cx, cy, 0, cx, cy, r); grad.addColorStop(0, qsTr("rgba(255, 255, 255, 1.0)")); grad.addColorStop(1, qsTr("rgba(255, 255, 255, 0.0)")); ctx.fillStyle = grad; ctx.beginPath(); ctx.arc(cx, cy, r, 0, 2 * Math.PI); ctx.fill()
                        }
                    }

                    Rectangle {
                        width: 24; height: 24; radius: 12; color: "transparent"; border.width: 3; border.color: "#FFFFFF"
                        x: wheelBox.width / 2 + Math.cos(ambientColorPopup.hue * 2 * Math.PI) * ambientColorPopup.sat * (wheelBox.width / 2) - width / 2
                        y: wheelBox.height / 2 + Math.sin(ambientColorPopup.hue * 2 * Math.PI) * ambientColorPopup.sat * (wheelBox.height / 2) - height / 2
                    }

                    MouseArea {
                        anchors.fill: parent
                        function upd(mx, my) {
                            var cx = wheelBox.width / 2, cy = wheelBox.height / 2, r = wheelBox.width / 2, dx = mx - cx, dy = my - cy
                            var dist = Math.min(r, Math.sqrt(dx * dx + dy * dy))
                            var angle = Math.atan2(dy, dx)
                            if (angle < 0) angle += 2 * Math.PI

                            ambientColorPopup.hue = angle / (2 * Math.PI)
                            ambientColorPopup.sat = dist / r
                            ambientColorPopup.pick(Qt.hsva(ambientColorPopup.hue, ambientColorPopup.sat, 1.0, 1.0).toString().substring(0, 7).toUpperCase())
                        }
                        onPressed: (m) => upd(m.x, m.y)
                        onPositionChanged: (m) => { if (pressed) upd(m.x, m.y) }
                    }
                }
            }
        }
    }

    // ═══════════════ POPUP: NUOVA PREIMPOSTAZIONE EQ ═══════════════
    Item {
        id: newPresetPopup
        anchors.fill: parent
        visible: false
        z: 500
        function open() { newPresetInput.text = ""; visible = true; newPresetInput.forceActiveFocus() }
        function close() { visible = false }

        Rectangle {
            anchors.fill: parent; color: "#99000000"
            MouseArea { anchors.fill: parent }
        }

        Rectangle {
            anchors.centerIn: parent
            width: 360; radius: 8; color: root.popupBg
            height: newPresetCol.height + 32
            MouseArea { anchors.fill: parent }

            Column {
                id: newPresetCol
                x: 16; y: 16; width: parent.width - 32; spacing: 14

                Row {
                    width: parent.width
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("New preset"); color: root.txt; font.pixelSize: 17
                    }
                    Item { width: parent.width - 200; height: 1 }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "✕"; color: root.txtSub; font.pixelSize: 20
                        MouseArea { anchors.fill: parent; anchors.margins: -10; onClicked: newPresetPopup.close() }
                    }
                }

                Text {
                    width: parent.width
                    text: qsTr("Save the current equalization with a name, to recall it whenever you like.")
                    color: root.txtHint; font.pixelSize: 12; wrapMode: Text.WordWrap
                }

                Rectangle {
                    width: parent.width; height: 44; radius: 8
                    color: root.ctrlBg; border.width: 1; border.color: root.ctrlBrd
                    TextInput {
                        id: newPresetInput
                        anchors.fill: parent; anchors.margins: 12
                        verticalAlignment: TextInput.AlignVCenter
                        color: root.txt; font.pixelSize: 14
                        clip: true; selectByMouse: true
                        Keys.onReturnPressed: newPresetSaveBtn.clicked()
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: newPresetInput.text === ""
                            text: qsTr("E.g. Rock, Deep bass…")
                            color: root.txtHint; font.pixelSize: 14
                        }
                    }
                }

                Row {
                    width: parent.width; spacing: 10
                    TSecondary { label: qsTr("Cancel"); onClicked: newPresetPopup.close() }
                    TPrimary {
                        id: newPresetSaveBtn
                        label: qsTr("Save")
                        onClicked: {
                            var name = newPresetInput.text.trim()
                            if (name.length > 0) {
                                audioRoot.saveEqPreset(name)
                                newPresetPopup.close()
                            }
                        }
                    }
                }
            }
        }
    }

    // ═══════════════ POPUP: SCELTA DISPOSITIVO AUDIO ═══════════════
    Item {
        id: devicePopup
        anchors.fill: parent
        visible: false
        z: 500
        property string target: "sink"
        function open(t) { target = t; visible = true }
        function close() { visible = false }
        readonly property var options: target === "sink" ? audioRoot.audioState.sinks : audioRoot.audioState.sources

        Rectangle {
            anchors.fill: parent; color: "#99000000"
            MouseArea { anchors.fill: parent }
        }

        Rectangle {
            anchors.centerIn: parent
            width: 420; radius: 8; color: root.popupBg
            height: devPopCol.height + 32
            MouseArea { anchors.fill: parent }

            Column {
                id: devPopCol
                x: 16; y: 16; width: parent.width - 32; spacing: 4

                Row {
                    width: parent.width
                    Text {
                        text: devicePopup.target === "sink" ? qsTr("Output device") : qsTr("Input device")
                        color: root.txt; font.pixelSize: 17; font.weight: Font.Normal; bottomPadding: 10
                    }
                    Item { width: parent.width - 200; height: 1 }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "✕"; color: root.txtSub; font.pixelSize: 20
                        MouseArea { anchors.fill: parent; anchors.margins: -10; onClicked: devicePopup.close() }
                    }
                }

                Repeater {
                    model: devicePopup.options
                    Rectangle {
                        width: parent.width; height: 46; radius: 8
                        color: _devMA.pressed ? (root.lightTheme ? "#DDDDDD" : "#2E2E2E") : "transparent"
                        Text {
                            x: 12; anchors.verticalCenter: parent.verticalCenter
                            text: modelData.label; color: root.txt; font.pixelSize: 14
                        }
                        MouseArea {
                            id: _devMA
                            anchors.fill: parent
                            onClicked: {
                                if (devicePopup.target === "sink") audioRoot.selectSink(modelData.name)
                                else audioRoot.selectSource(modelData.name)
                                devicePopup.close()
                            }
                        }
                    }
                }
                Text {
                    visible: devicePopup.options.length === 0
                    text: qsTr("No device found")
                    color: root.txtHint; font.pixelSize: 13
                    topPadding: 6; bottomPadding: 6
                }
            }
        }
    }

    // Popup: game cover and hero, chosen by the user
    Item {
        id: artDlg
        property var game: null
        visible: false
        anchors.fill: parent
        z: 900
        Rectangle { anchors.fill: parent; color: "#000000"; opacity: 0.55
                    MouseArea { anchors.fill: parent; onClicked: artDlg.visible = false } }
        Rectangle {
            anchors.centerIn: parent
            width: 420; height: _artCol.height + 48; radius: 16
            color: root.lightTheme ? "#F2F2F2" : "#262626"
            MouseArea { anchors.fill: parent }
            Column {
                id: _artCol
                x: 24; y: 24; width: parent.width - 48; spacing: 14
                Text { width: parent.width; elide: Text.ElideRight
                       text: artDlg.game ? artDlg.game.name : ""
                       color: root.txt; font.pixelSize: 20; font.weight: Font.DemiBold }
                Text { width: parent.width; wrapMode: Text.WordWrap
                       text: qsTr("Cover: vertical image (e.g. 600×900). Hero: wide image for the big picture.")
                       color: root.txtHint; font.pixelSize: 13 }
                Repeater {
                    model: [ { k: "cover", t: qsTr("Cover") }, { k: "hero", t: qsTr("Hero") } ]
                    Rectangle {
                        width: _artCol.width; height: 48; radius: 10
                        color: _pbMA.pressed ? "#1E63D0" : "#2C7BF2"
                        Text { anchors.centerIn: parent; text: modelData.t; color: "white"; font.pixelSize: 16; font.weight: Font.DemiBold }
                        MouseArea { id: _pbMA; anchors.fill: parent
                            onClicked: { artPicker.game = artDlg.game; artPicker.kind = modelData.k; artPicker.open() } }
                    }
                }
                Row {
                    spacing: 10
                    Rectangle {
                        width: (_artCol.width - 10) / 2; height: 44; radius: 10
                        color: _rsMA.pressed ? (root.lightTheme ? "#C8C8C8" : "#5A5A5A") : (root.lightTheme ? "#D5D5D5" : "#3A3A3A")
                        Text { anchors.centerIn: parent; text: qsTr("Reset"); color: root.txt; font.pixelSize: 15 }
                        MouseArea { id: _rsMA; anchors.fill: parent
                            onClicked: { if (root.arcade && artDlg.game) { root.arcade.setCustomArt(artDlg.game, "cover", ""); root.arcade.setCustomArt(artDlg.game, "hero", "") } } }
                    }
                    Rectangle {
                        width: (_artCol.width - 10) / 2; height: 44; radius: 10
                        color: _clMA.pressed ? (root.lightTheme ? "#C8C8C8" : "#5A5A5A") : (root.lightTheme ? "#D5D5D5" : "#3A3A3A")
                        Text { anchors.centerIn: parent; text: qsTr("Close"); color: root.txt; font.pixelSize: 15 }
                        MouseArea { id: _clMA; anchors.fill: parent; onClicked: artDlg.visible = false }
                    }
                }
            }
        }
    }
}
