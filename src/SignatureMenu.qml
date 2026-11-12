import QtQuick
import QtQuick.Controls
import QtQuick.Shapes

// The saved signatures, by name. Click one (or arrow to it and press Enter) to
// place it; hover for × to delete it.
Popup {
    id: menu

    property color accent: "#FFD60A"
    property int highlighted: 0
    signal picked(string name)
    signal deleteRequested(string name)
    signal addRequested()

    width: 340
    padding: 8
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

    onOpened: {
        highlighted = Math.max(0, list.count - 1 >= 0 ? Math.min(highlighted, list.count - 1) : 0);
        content.forceActiveFocus();
    }

    background: Rectangle {
        color: "#1c1c1e"
        radius: theme.radius
        border.color: "#3a3a3c"
    }

    contentItem: FocusScope {
        id: content
        implicitHeight: column.implicitHeight
        focus: true

        Keys.onUpPressed: menu.highlighted = Math.max(0, menu.highlighted - 1)
        Keys.onDownPressed: menu.highlighted = Math.min(list.count, menu.highlighted + 1)
        Keys.onReturnPressed: activate()
        Keys.onEnterPressed: activate()
        function activate() {
            if (menu.highlighted < list.count)
                menu.picked(backend.signatures.data(backend.signatures.index(menu.highlighted, 0), Qt.UserRole + 1));
            else
                menu.addRequested();
        }

        Column {
            id: column
            width: parent.width
            spacing: 2

            Label {
                visible: list.count === 0
                width: parent.width
                padding: 10
                text: qsTr("Add an image of your signature — a scan or a photo of it on white paper.")
                wrapMode: Text.WordWrap
                color: "#b8b8bc"
                font.pixelSize: 13
            }

            Repeater {
                id: list
                model: backend.signatures
                delegate: Rectangle {
                    id: row
                    required property int index
                    required property string name
                    required property url source

                    width: column.width
                    height: 62
                    radius: Math.min(8, theme.radius)
                    color: menu.highlighted === index ? "#2c2c2f" : "transparent"

                    HoverHandler {
                        id: rowHover
                        cursorShape: Qt.PointingHandCursor
                        onHoveredChanged: if (hovered) menu.highlighted = row.index
                    }
                    TapHandler { onTapped: menu.picked(row.name) }

                    Rectangle {
                        id: preview
                        x: 8
                        anchors.verticalCenter: parent.verticalCenter
                        width: 150
                        height: 46
                        radius: Math.min(5, theme.radius)
                        color: "white"
                        Image {
                            anchors.fill: parent
                            anchors.margins: 5
                            source: row.source
                            fillMode: Image.PreserveAspectFit
                            mipmap: true
                            smooth: true
                            asynchronous: true
                        }
                    }
                    Label {
                        anchors.left: preview.right
                        anchors.leftMargin: 14
                        anchors.right: remove.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.name
                        elide: Text.ElideRight
                        color: "white"
                        font.pixelSize: 13
                    }
                    Label {
                        id: remove
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        visible: rowHover.hovered
                        text: "×"
                        color: removeHover.hovered ? "white" : "#8e8e93"
                        font.pixelSize: 18
                        HoverHandler { id: removeHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: menu.deleteRequested(row.name) }
                    }
                }
            }

            Rectangle {
                visible: list.count > 0
                width: parent.width - 8
                x: 4
                height: 1
                color: "#3a3a3c"
            }

            Rectangle {
                width: parent.width
                height: 40
                radius: Math.min(8, theme.radius)
                color: menu.highlighted === list.count ? "#2c2c2f" : "transparent"
                HoverHandler {
                    cursorShape: Qt.PointingHandCursor
                    onHoveredChanged: if (hovered) menu.highlighted = list.count
                }
                TapHandler { onTapped: menu.addRequested() }
                Label {
                    x: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("+ Add signature…")
                    color: menu.accent
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                }
            }
        }
    }
}
