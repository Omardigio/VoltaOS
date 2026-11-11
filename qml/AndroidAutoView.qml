import QtQuick
import QtMultimedia
import VoltaApp 1.0

// CarPlay / Android Auto from the Carlinkit dongle (src/carlink/CarlinkDongle):
// the phone's video and touch, with waiting screens in the UI's style.
Rectangle {
    id: aav
    color: "#000000"
    property bool shown: false          // app visible (starts the search for the dongle)
    property bool lightTheme: false
    property bool fullscreen: false     // full-screen app (different fit from the small panel)

    onShownChanged: if (shown) Carlink.enabled = true
    onWidthChanged:  Carlink.setViewSize(width, height)
    onHeightChanged: Carlink.setViewSize(width, height)
    Component.onCompleted: {
        Carlink.videoSink = video.videoSink
        Carlink.setViewSize(width, height)
    }
    Binding { target: Carlink; property: "uiLight"; value: aav.lightTheme }

    readonly property bool live: Carlink.state === "running" && Carlink.videoActive

    // The video is always 16:9 (1920x1080). Fit chosen in the Settings (one for the
    // small panel, one for full screen): "fill" stretches to fill, "fit" leaves
    // the bars without distortion.
    readonly property string fitMode: fullscreen ? Carlink.fitFull : Carlink.fitSmall
    readonly property real vw: Math.max(1, Carlink.videoSize.width)
    readonly property real vh: Math.max(1, Carlink.videoSize.height)
    // rectangle where the video is really shown
    readonly property real _sc: fitMode === "fill" ? 0 : Math.min(width / vw, height / vh)
    readonly property real vidW: fitMode === "fill" ? width  : vw * _sc
    readonly property real vidH: fitMode === "fill" ? height : vh * _sc
    readonly property real vidX: (width  - vidW) / 2
    readonly property real vidY: (height - vidH) / 2

    Item {
        anchors.fill: parent
        clip: true
        visible: aav.live
        VideoOutput {
            id: video
            x: aav.vidX; y: aav.vidY
            width: aav.vidW; height: aav.vidH
            fillMode: VideoOutput.Stretch
        }
    }

    // Touch is mapped onto the video rectangle: it matches what you see
    MouseArea {
        anchors.fill: parent
        enabled: aav.live
        function send(action, mouse) {
            Carlink.touch(action, Math.max(0, Math.min(1, (mouse.x - aav.vidX) / aav.vidW)),
                                  Math.max(0, Math.min(1, (mouse.y - aav.vidY) / aav.vidH)))
        }
        onPressed:         function(mouse) { send(0, mouse) }
        onPositionChanged: function(mouse) { if (pressed) send(2, mouse) }
        onReleased:        function(mouse) { send(1, mouse) }
    }

    // ── waiting screens ──
    Column {
        anchors.centerIn: parent
        width: Math.min(parent.width - 64, 440)
        spacing: 22
        visible: !aav.live

        Image {
            anchors.horizontalCenter: parent.horizontalCenter
            source: "qrc:/VoltaApp/icons/androidauto.svg"
            width: 96; height: 96
            sourceSize: Qt.size(192, 192)
            smooth: true; mipmap: true
        }

        Spinner {
            anchors.horizontalCenter: parent.horizontalCenter
            size: 26
            visible: aav.busy
            running: visible && aav.visible
        }

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: aav.title
            color: "#FFFFFF"
            font.pixelSize: 20; font.weight: Font.DemiBold
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            visible: text !== ""
            text: aav.subtitle
            color: "#A2A3A5"
            font.pixelSize: 15
            lineHeight: 1.2
        }
    }

    readonly property bool busy: Carlink.state === "starting" || Carlink.state === "connecting"
                                 || (Carlink.state === "running" && !Carlink.videoActive)
    readonly property bool noDongle: Carlink.source !== "dongle"
    readonly property string title: {
        if (noDongle) {
            switch (Carlink.state) {
            case "starting":   return qsTr("Preparing the phone…")
            case "connecting": return qsTr("Connecting to Android Auto…")
            case "running":    return qsTr("Starting…")
            case "error":      return Carlink.detail === "permission" ? qsTr("Cannot reach the phone") : qsTr("Android Auto problem")
            default:           return Carlink.source === "wireless" ? qsTr("Waiting for your phone") : qsTr("Connect your phone")
            }
        }
        switch (Carlink.state) {
        case "starting":   return qsTr("Starting the dongle…")
        case "waiting":    return qsTr("Connect your phone")
        case "connecting": return Carlink.phoneKind !== "" ? qsTr("Connecting to ") + Carlink.phoneKind + "…" : qsTr("Connecting…")
        case "running":    return qsTr("Starting…")
        case "error":      return qsTr("Dongle unavailable")
        default:           return qsTr("Connect the USB dongle")
        }
    }
    readonly property string subtitle: {
        if (noDongle) {
            switch (Carlink.state) {
            case "error":
                return Carlink.detail === "permission"
                       ? qsTr("Volta OS is not allowed to use the phone over USB. Install the udev rule from the documentation (51-volta-android-auto.rules), then unplug the phone and plug it in again.")
                       : Carlink.detail
            case "searching":
            case "off":
                return Carlink.source === "wireless"
                       ? qsTr("Waiting for your phone to connect over Wi-Fi.")
                       : qsTr("Plug your Android phone into a USB port with a data cable. The Android Auto app must be installed.")
            default: return ""
            }
        }
        switch (Carlink.state) {
        case "waiting":    return qsTr("Turn on Bluetooth on your phone and connect it to the dongle: CarPlay or Android Auto start by themselves.")
        case "error":      return Carlink.detail
        case "searching":
        case "off":        return qsTr("Plug the Carlinkit dongle into a USB port.")
        default:           return ""
        }
    }
}
