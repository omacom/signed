import QtQuick

// One page: its render, and the items placed on it.
Item {
    id: page

    property int index: 0
    property real viewScale: 1
    property color accent: "#FFD60A"
    property int renderWidth: 0
    signal backgroundPressed()

    // pageWidth() is a call QML can't watch, so the generation is read to
    // measure again when a document with as many pages replaces this one.
    readonly property size pageSize: {
        backend.generation;
        return Qt.size(backend.pageWidth(index), backend.pageHeight(index));
    }
    width: Math.round(pageSize.width * viewScale)
    height: Math.round(pageSize.height * viewScale)

    // Re-render only once a zoom settles, in steps, so dragging the zoom
    // doesn't queue a render per frame.
    function updateRenderWidth() {
        renderWidth = Math.ceil(width * Screen.devicePixelRatio / 64) * 64;
    }
    onWidthChanged: renderTimer.restart()
    Component.onCompleted: updateRenderWidth()
    Timer { id: renderTimer; interval: 150; onTriggered: page.updateRenderWidth() }

    Rectangle {
        anchors.fill: parent
        anchors.margins: -1
        anchors.topMargin: 2
        anchors.bottomMargin: -6
        color: "#60000000"
        radius: Math.min(2, theme.radius)
    }
    Rectangle { anchors.fill: parent; color: "white" }

    // The last finished render stays up while a sharper one loads behind it,
    // but never one of the document this replaced.
    Image {
        id: shown
        anchors.fill: parent
        cache: true
        smooth: true
    }
    Connections {
        target: backend
        function onDocumentChanged() { shown.source = ""; }
    }
    Image {
        id: loader
        visible: false
        asynchronous: true
        cache: true
        source: page.renderWidth > 0 ? "image://pages/" + backend.generation + "/" + page.index + "/" + page.renderWidth : ""
        onStatusChanged: if (status === Image.Ready) shown.source = source
    }

    MouseArea {
        anchors.fill: parent
        onPressed: page.backgroundPressed()
    }

    Repeater {
        model: backend.items
        delegate: PlacedItem {
            pageIndex: page.index
            viewScale: page.viewScale
            accent: page.accent
        }
    }
}
