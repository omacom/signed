import QtQuick
import QtQuick.Controls

// A modal question over the window. ask() shows it and runs the action only
// if the user confirms.
Rectangle {
    id: confirm

    property string title: ""
    property string message: ""
    property string confirmText: qsTr("OK")
    property var action: null
    property color accent: "#FFD60A"
    property color accentForeground: "black"

    function ask(title, message, confirmText, action) {
        confirm.title = title;
        confirm.message = message;
        confirm.confirmText = confirmText;
        confirm.action = action;
        visible = true;
        confirmButton.forceActiveFocus();
    }
    function cancel() {
        visible = false;
        action = null;
    }
    function accept() {
        const run = action;
        visible = false;
        action = null;
        if (run)
            run();
    }

    visible: false
    anchors.fill: parent
    color: "#000000cc"

    MouseArea { anchors.fill: parent; onClicked: confirm.cancel() }

    Rectangle {
        anchors.centerIn: parent
        width: Math.max(column.implicitWidth, 360) + 56
        height: column.implicitHeight + 48
        radius: theme.radius
        color: "#1c1c1e"

        MouseArea { anchors.fill: parent }

        Column {
            id: column
            anchors.centerIn: parent
            width: parent.width - 56
            spacing: 8

            Label { text: confirm.title; color: "white"; font.pixelSize: 16; font.weight: Font.DemiBold }
            Label {
                width: parent.width
                text: confirm.message
                color: "#d6d6da"
                font.pixelSize: 13
                wrapMode: Text.WordWrap
                bottomPadding: 12
            }
            Row {
                anchors.right: parent.right
                spacing: 10
                BarButton {
                    text: qsTr("Cancel")
                    accent: confirm.accent
                    onClicked: confirm.cancel()
                }
                BarButton {
                    id: confirmButton
                    text: confirm.confirmText
                    primary: true
                    accent: confirm.accent
                    accentForeground: confirm.accentForeground
                    focus: true
                    Keys.onReturnPressed: confirm.accept()
                    Keys.onEnterPressed: confirm.accept()
                    Keys.onEscapePressed: confirm.cancel()
                    onClicked: confirm.accept()
                }
            }
        }
    }
}
