import QtQuick
import QtQuick.Window
import QtCore as Core
import QtWebEngine
import Qt5Compat.GraphicalEffects
import VoltaApp 1.0

// SMART THEATER — grid launcher + WebEngineView to play the
// selected service. The tile images can be customized by putting
// i PNG in volta_os/assets/theater/ (youtube, netflix, prime, disney, paramount)
// (or changing the `banner` property of each service) and the labels will disappear.
Rectangle {
    id: theater
    // Same background as Toybox and Arcade
    color: lightTheme ? "#FFFFFF" : "#171717"
    clip: true

    property bool lightTheme: false
    property bool active: false               // true when it is the foreground app

    // URL currently open ("" = shows the grid launcher)
    property string currentUrl: ""
    // Full-screen container (Main.qml): apps open there, the Theater panel stays still
    property Item overlayHost: null
    property string currentName: ""

    // "Back" navigation — driven by the panel's handle (AppPanel)
    property bool canGoBack: currentUrl !== ""
    // closing the app returns to the Theater launcher, which is NOT full screen
    function goBack() { theater.currentUrl = "" }

    // Exposed to AppPanel for the global pause logic
    property alias webView: theaterWeb

    signal requestFullscreen(bool on)
    signal mediaActivated()
    // Menu Theater/Arcade in alto: preme "Arcade" -> chi instanzia Theater
    // (AppPanel) cambia app.
    signal switchToArcade()
    signal switchToToybox()

    // When the app is no longer active, return to the launcher and unload the webview
    onActiveChanged: if (!active) { currentUrl = ""; editMode = false; addOpen = false }

    // ── Servizi ────────────────────────────────────────────────────────────
    // The banners are images ON DISK in the volta_os/assets/theater/ folder:
    // youtube.png, netflix.png, prime.png, disney.png, paramount.png
    // Replace those files with YOUR images (same name) and they will appear here.
    readonly property string bannerBase: appUrl + "/assets/theater/"
    // Saved list (default services + the ones the user adds). Each: {key,name,url,accent,custom}
    readonly property var defaultServices: [
        { key: "youtube",   name: "YouTube",     url: "https://m.youtube.com/",        accent: "#FF0000" },
        { key: "netflix",   name: "Netflix",     url: "https://www.netflix.com",       accent: "#E50914" },
        { key: "prime",     name: "Prime Video", url: "https://www.primevideo.com",    accent: "#1FA2FF" },
        { key: "disney",    name: "Disney+",     url: "https://www.disneyplus.com",    accent: "#2A6DF4" },
        { key: "paramount", name: "Paramount+",  url: "https://www.paramountplus.com", accent: "#0064FF" }
    ]
    Core.Settings {
        id: svcPrefs
        category: "theater"
        property string json: ""
    }
    property var services: {
        try { var l = JSON.parse(svcPrefs.json); if (Array.isArray(l)) return l } catch (e) {}
        return theater.defaultServices
    }
    readonly property int maxApps: 9
    property bool editMode: false
    property bool addOpen: false
    function saveServices(l) { svcPrefs.json = JSON.stringify(l) }
    function removeService(key) {
        saveServices(services.filter(function(x) { return x.key !== key }))
        if (services.length <= 1) editMode = false
    }
    function addService(addr) {
        var t = String(addr || "").trim()
        if (t === "" || services.length >= maxApps) return false
        if (t.indexOf("://") < 0) t = "https://" + t
        var host = (t.split("://")[1] || "").split("/")[0].replace(/^www\./i, "")
        if (host.indexOf(".") < 0 || host.indexOf(" ") >= 0) return false
        var parts = host.split(".")
        var nm = parts.length >= 2 ? parts[parts.length - 2] : parts[0]
        nm = nm.charAt(0).toUpperCase() + nm.slice(1)
        var l = services.slice()
        l.push({ key: "c" + Date.now().toString(36), name: nm, url: t, accent: "#8A8A8A", custom: true })
        saveServices(l)
        return true
    }

    // Emitted instead of opening the webview: AppPanel catches it (leftover
    // from the Waydroid era — today the handler in AppPanel.qml is empty, but the
    // signal must still exist for that QML assignment to be valid).
    signal openAndroidApp(string key)

    // User agent: YouTube in tablet mode (grid layout like on tablets), the other sites as desktop.
    readonly property string desktopUserAgent: "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
    readonly property string youtubeUserAgent: "Mozilla/5.0 (Linux; Android 13; SM-X906C) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"

    function openService(url, name, key) {
        // the user agent must be set BEFORE loading the page
        theaterProfile.httpUserAgent = (key === "youtube") ? theater.youtubeUserAgent : theater.desktopUserAgent
        theater.currentUrl = url
        theater.currentName = name
        // The Theater apps open directly in fullscreen (see AppPanel):
        // here we immediately request the fullscreen state, as if the user had
        // pressed the dedicated button.
    }

    // ══════════════════════════════════════════════════════════════════════
    //  HEADER BAR — fixed at the top (does not scroll with the content),
    //  app icon on the left, Theater/Arcade menu centered over the full
    //  width (not just next to the icon). Same text style as the
    //  YT Music tabs (no background/pill).
    // ══════════════════════════════════════════════════════════════════════
    Item {
        id: headerBar
        visible: theater.currentUrl === ""
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: 20
        height: 26
        z: 50

        Image {
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            source: "qrc:/VoltaApp/icons/theater.svg"
            sourceSize: Qt.size(26, 26)
        }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            spacing: 18

            Text {
                text: qsTr("Theater")
                color: lightTheme ? "#444444" : "#FFFFFF"
                font.pixelSize: 20; font.weight: Font.DemiBold
                // No MouseArea: we are already on Theater, the solid color
                // (vs. Arcade's muted one) shows the active state.
            }
            Text {
                text: "Arcade"
                color: lightTheme ? "#555555" : "#A2A3A5"
                font.pixelSize: 20; font.weight: Font.DemiBold
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -8
                    onClicked: theater.switchToArcade()
                }
            }
            Text {
                text: qsTr("Toybox")
                color: lightTheme ? "#555555" : "#A2A3A5"
                font.pixelSize: 20; font.weight: Font.DemiBold
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -8
                    onClicked: theater.switchToToybox()
                }
            }
        }
    }

    // ══════════════════════════════════════════════════════════════════════
    //  LAUNCHER (griglia)
    // ══════════════════════════════════════════════════════════════════════
    Flickable {
        id: launcher
        anchors.fill: parent
        visible: theater.currentUrl === ""
        contentWidth: width
        contentHeight: Math.max(height, contentCol.height + 60)
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        MouseArea {
            width: launcher.width; height: launcher.contentHeight
            onClicked: theater.editMode = false
        }

        Column {
            id: contentCol
            width: Math.min(launcher.width - 60, 1100)
            anchors.horizontalCenter: parent.horizontalCenter
            // Vertically centered to fill the page well
            y: Math.max(30, (launcher.height - height) / 2)
            spacing: 40

            // ── Header ───────────────────────────────────────────────────
            Column {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 6
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "S M A R T"
                    color: "#8A8A8A"
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                    font.letterSpacing: 6
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "THEATER"
                    color: lightTheme ? "#171717" : "#FFFFFF"
                    font.pixelSize: 46
                    font.weight: Font.DemiBold
                    font.letterSpacing: 9
                }
            }

            // ── Griglia servizi (max 9, trascinabili) ────────────────────
            Item {
                id: grid
                width: parent.width
                anchors.horizontalCenter: parent.horizontalCenter
                readonly property int columns: 3
                readonly property real gap: 22
                readonly property real cellW: (width - gap * (columns - 1)) / columns
                readonly property real cellH: cellW * 0.55
                readonly property int slots: theater.services.length + ((theater.editMode && theater.services.length < theater.maxApps) ? 1 : 0)
                height: Math.ceil(slots / columns) * cellH + (Math.ceil(slots / columns) - 1) * gap

                // order while dragging (list of keys); empty = order of services
                property var dragOrder: []
                function slotOf(key, idx) {
                    if (dragOrder.length === 0) return idx
                    var s = dragOrder.indexOf(key)
                    return s < 0 ? idx : s
                }
                function slotX(s) { return (s % columns) * (cellW + gap) }
                function slotY(s) { return Math.floor(s / columns) * (cellH + gap) }
                function commitOrder() {
                    if (dragOrder.length === 0) return
                    var byKey = {}
                    theater.services.forEach(function(x) { byKey[x.key] = x })
                    var l = dragOrder.map(function(k) { return byKey[k] }).filter(function(x) { return !!x })
                    dragOrder = []
                    theater.saveServices(l)
                }

                Repeater {
                    model: theater.services

                    Rectangle {
                        id: tile
                        readonly property int slot: grid.slotOf(modelData.key, index)
                        property bool dragging: false
                        property real dragX: 0
                        property real dragY: 0
                        width: grid.cellW
                        height: grid.cellH
                        x: dragging ? dragX : grid.slotX(slot)
                        y: dragging ? dragY : grid.slotY(slot)
                        z: dragging ? 10 : 1
                        scale: dragging ? 1.04 : (theater.editMode ? 0.955 : 1)
                        radius: 10
                        color: tileArea.pressed ? (lightTheme ? "#EDEDED" : "#1C1C1C") : (lightTheme ? "#FFFFFF" : "#111111")
                        border.width: theater.editMode ? 2 : 1
                        border.color: theater.editMode ? (lightTheme ? "#8E8E93" : "#5A5A5E") : (lightTheme ? "#DDDDDD" : "#242424")

                        Behavior on border.color { ColorAnimation { duration: 250 } }
                        Behavior on x { enabled: !tile.dragging; NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
                        Behavior on y { enabled: !tile.dragging; NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
                        Behavior on scale { NumberAnimation { duration: 280; easing.type: Easing.OutCubic } }
                        Behavior on color { ColorAnimation { duration: 200 } }

                        // edit mode (Tesla style): the tile settles back slightly and its border lights up
        transformOrigin: Item.Center
        // Banner: YOUR image from volta_os/assets/theater/<key>.png
                        Image {
                            id: banner
                            anchors.fill: parent
                            anchors.margins: 1
                            source: modelData.custom ? "" : theater.bannerBase + modelData.key + ".png"
                            fillMode: Image.PreserveAspectCrop
                            visible: status === Image.Ready
                            smooth: true
                            layer.enabled: true
                            layer.effect: OpacityMask {
                                maskSource: Rectangle { width: banner.width; height: banner.height; radius: 8 }
                            }
                        }

                        // Custom app: ready-made card (logo on its own colour), built by the service
        Image {
            id: siteIcon
            visible: !!modelData.custom && status === Image.Ready
            anchors.fill: parent
            anchors.margins: 1
            source: modelData.custom ? "http://127.0.0.1:9098/webapp-card?url=" + encodeURIComponent(modelData.url) : ""
            fillMode: Image.PreserveAspectCrop
            smooth: true; asynchronous: true
            layer.enabled: true
            layer.effect: OpacityMask {
                maskSource: Rectangle { width: siteIcon.width; height: siteIcon.height; radius: 8 }
            }
        }

        // Placeholder (service name) if the image is missing
                        Column {
                            anchors.centerIn: parent
                            spacing: 10
                            visible: !banner.visible && !(modelData.custom && siteIcon.status === Image.Ready)

                            Rectangle {
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: 12; height: 12; radius: 6
                                color: modelData.accent
                            }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: modelData.name
                                color: lightTheme ? "#171717" : "#F0F0F0"
                                font.pixelSize: 24
                                font.weight: Font.Normal
                                font.letterSpacing: 0.5
                            }
                        }

                        MouseArea {
                            id: tileArea
                            anchors.fill: parent
                            property bool held: false
                            property bool moved: false
                            property real grabX: 0
                            property real grabY: 0
                            onPressed: function(m) { held = false; moved = false; grabX = m.x; grabY = m.y }
                            onPressAndHold: { held = true; theater.editMode = true }
                            onPositionChanged: function(m) {
                                if (!pressed || !theater.editMode) return
                                var p = tileArea.mapToItem(grid, m.x, m.y)
                                if (!tile.dragging) {
                                    if (Math.abs(m.x - grabX) + Math.abs(m.y - grabY) < 10) return
                                    var order = []
                                    theater.services.forEach(function(x) { order.push(x.key) })
                                    grid.dragOrder = order
                                    tile.dragging = true
                                    moved = true
                                }
                                tile.dragX = p.x - grabX
                                tile.dragY = p.y - grabY
                                var cx = tile.dragX + tile.width / 2, cy = tile.dragY + tile.height / 2
                                var col = Math.max(0, Math.min(grid.columns - 1, Math.floor(cx / (grid.cellW + grid.gap))))
                                var row = Math.max(0, Math.floor(cy / (grid.cellH + grid.gap)))
                                var target = Math.min(row * grid.columns + col, theater.services.length - 1)
                                var cur = grid.dragOrder.indexOf(modelData.key)
                                if (target !== cur) {
                                    var o = grid.dragOrder.slice()
                                    o.splice(cur, 1)
                                    o.splice(target, 0, modelData.key)
                                    grid.dragOrder = o
                                }
                            }
                            onReleased: {
                                if (tile.dragging) { tile.dragging = false; grid.commitOrder() }
                            }
                            onCanceled: if (tile.dragging) { tile.dragging = false; grid.commitOrder() }
                            onClicked: {
                                if (held || moved) return
                                if (theater.editMode) theater.editMode = false
                                else theater.openService(modelData.url, modelData.name, modelData.key)
                            }
                        }

                        // edit mode: same circle with "x" as the drawer's web apps
                        Rectangle {
                            x: -width / 3; y: -height / 3
                            width: 26; height: width; radius: width / 2
                            color: _tDelMA.pressed ? "#8E8E93" : "#AEAEB2"
                            visible: scale > 0.01
                            scale: theater.editMode ? 1 : 0
                            Behavior on scale { NumberAnimation { duration: 220; easing.type: Easing.OutBack } }
                            Repeater {
                                model: [45, -45]
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: parent.width * 0.46; height: 2; radius: 1
                                    color: "#FFFFFF"; rotation: modelData; antialiasing: true
                                }
                            }
                            MouseArea { id: _tDelMA; anchors.fill: parent; anchors.margins: -8; onClicked: theater.removeService(modelData.key) }
                        }
                    }
                }

                // "+" silhouette next to the last icon: edit mode only, and only below the maximum
                Rectangle {
                    id: plusTile
                    readonly property bool shown: theater.editMode && theater.services.length < theater.maxApps
                    x: grid.slotX(theater.services.length)
                    y: grid.slotY(theater.services.length)
                    width: grid.cellW
                    height: grid.cellH
                    radius: 10
                    color: "transparent"
                    border.width: 2
                    border.color: lightTheme ? "#BBBBBB" : "#3A3A3A"
                    visible: opacity > 0.01
                    opacity: shown ? 1 : 0
                    scale: shown ? 1 : 0.8
                    Behavior on opacity { NumberAnimation { duration: 200 } }
                    Behavior on scale { NumberAnimation { duration: 220; easing.type: Easing.OutBack } }
                    Text {
                        anchors.centerIn: parent
                        text: "+"
                        color: lightTheme ? "#888888" : "#6A6A6A"
                        font.pixelSize: 56; font.weight: Font.Light
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: plusTile.shown
                        onClicked: { addField.text = ""; addErr.visible = false; theater.addOpen = true; addField.forceActiveFocus() }
                    }
                }
            }
        }
    }

    // ── Popup: address of the custom Theater app ──
    Rectangle {
        anchors.fill: parent
        visible: theater.addOpen
        color: "#99000000"
        z: 100
        MouseArea { anchors.fill: parent; onClicked: theater.addOpen = false }
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 60, 520)
            height: popCol.height + 48
            radius: 16
            color: lightTheme ? "#FFFFFF" : "#262626"
            border.width: 1; border.color: lightTheme ? "#DDDDDD" : "#3A3A3A"
            MouseArea { anchors.fill: parent }
            Column {
                id: popCol
                anchors.centerIn: parent
                width: parent.width - 48
                spacing: 14
                Text {
                    text: qsTr("Add a Theater app")
                    color: lightTheme ? "#171717" : "#FFFFFF"
                    font.pixelSize: 22; font.weight: Font.DemiBold
                }
                Text {
                    width: parent.width
                    text: qsTr("Type the web address of the site (e.g. raiplay.it). The icon is found automatically.")
                    color: lightTheme ? "#555555" : "#A2A3A5"
                    font.pixelSize: 14; wrapMode: Text.WordWrap
                }
                Rectangle {
                    width: parent.width; height: 44; radius: 10
                    color: lightTheme ? "#F0F0F0" : "#171717"
                    border.width: 1; border.color: lightTheme ? "#DDDDDD" : "#3A3A3A"
                    TextInput {
                        id: addField
                        anchors.fill: parent; anchors.margins: 12
                        verticalAlignment: TextInput.AlignVCenter
                        color: lightTheme ? "#171717" : "#FFFFFF"
                        font.pixelSize: 16; clip: true; selectByMouse: true
                        inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoPredictiveText
                        onAccepted: addBtn.go()
                    }
                }
                Text {
                    id: addErr
                    visible: false
                    text: qsTr("Invalid address")
                    color: "#E5484D"; font.pixelSize: 13
                }
                Row {
                    anchors.right: parent.right
                    spacing: 12
                    Rectangle {
                        width: 110; height: 40; radius: 10
                        color: lightTheme ? "#E6E6E6" : "#3A3A3A"
                        Text { anchors.centerIn: parent; text: qsTr("Cancel"); color: lightTheme ? "#171717" : "#FFFFFF"; font.pixelSize: 15 }
                        MouseArea { anchors.fill: parent; onClicked: theater.addOpen = false }
                    }
                    Rectangle {
                        id: addBtn
                        width: 110; height: 40; radius: 10
                        color: "#3E6AE1"
                        function go() {
                            if (theater.addService(addField.text)) theater.addOpen = false
                            else addErr.visible = true
                        }
                        Text { anchors.centerIn: parent; text: qsTr("Add"); color: "#FFFFFF"; font.pixelSize: 15; font.weight: Font.DemiBold }
                        MouseArea { anchors.fill: parent; onClicked: addBtn.go() }
                    }
                }
            }
        }
    }

    // ══════════════════════════════════════════════════════════════════════
    //  WEBVIEW (servizio aperto)
    // ══════════════════════════════════════════════════════════════════════
    // Web profile with data saved to disk (VoltaTheaterData). No "offTheRecord: false":
    // set before storageName it produced the warning "Storage name is empty...".
    // (Qt 6.9's WebEngineProfilePrototype does not work here: instance() is null while
    //  the WebEngineView is created, and setProfile(null) crashes.)
    SystemHelper { id: cookieSys; Component.onCompleted: cookieSys.persistCookies(theaterProfile, "theater") }
    WebEngineProfile {
        id: theaterProfile
        storageName: "VoltaTheaterData"
        persistentCookiesPolicy: WebEngineProfile.ForcePersistentCookies
        httpUserAgent: "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
        // The service addresses are the generic ones (.com): each site sends the user to its own
        // country from the IP address, and picks the language from this header.
        httpAcceptLanguage: Lang.code + ", en;q=0.8"
    }

    Item {
        id: webWrap
        parent: (theater.overlayHost && theater.currentUrl !== "") ? theater.overlayHost : theater
        anchors.fill: parent
        visible: theater.currentUrl !== ""
        clip: true

        Rectangle { anchors.fill: parent; color: "#000000" }
        FullscreenBar { id: wbar; width: parent.width; onBackRequested: theater.goBack() }

        // Opening a service: the screen slides from the bottom to
        // the top (above the dock), like all the apps that start full
        // screen. The handle at the top stays visible here (unlike the
        // Toybox's Romance fireplace).
        transform: Translate { id: webSlideT; y: 0 }
        NumberAnimation {
            id: webSlideAnim
            target: webSlideT; property: "y"
            to: 0
            duration: 360; easing.type: Easing.OutCubic
        }
        onVisibleChanged: if (visible) {
            webSlideT.y = webWrap.height
            webSlideAnim.restart()
        }

        WebEngineView {
            settings.showScrollBars: false
            id: theaterWeb
            anchors.top: wbar.bottom; anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            profile: theaterProfile
            url: theater.currentUrl
            settings.pluginsEnabled: true
            settings.javascriptEnabled: true
            settings.playbackRequiresUserGesture: false
            settings.allowRunningInsecureContent: false
            settings.javascriptCanAccessClipboard: true
            settings.localContentCanAccessRemoteUrls: true

            lifecycleState: (theater.active && theater.currentUrl !== "")
                            ? WebEngineView.LifecycleState.Active
                            : WebEngineView.LifecycleState.Discarded

            onRecentlyAudibleChanged: if (recentlyAudible) theater.mediaActivated()

            onFullScreenRequested: function(request) {
                request.accept()
            }
        }
    }
}
