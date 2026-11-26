import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// One certificate-store password entry. confirm adds the second field used
// when a new store is being created.
Rectangle {
    id: dialog

    property color accent: "#FFD60A"
    property color accentForeground: "black"
    property bool confirm: false
    property string error: ""
    signal accepted(string password)
    signal cancelled()

    function open() {
        error = "";
        passwordField.text = "";
        confirmationField.text = "";
        visible = true;
        passwordField.forceActiveFocus();
    }
    function showError(message) {
        error = message;
        visible = true;
        passwordField.forceActiveFocus();
        passwordField.selectAll();
    }
    function cancel() {
        visible = false;
        cancelled();
    }
    function submit() {
        const password = passwordField.text;
        if (password.length === 0 || (confirm && password.length < 8)) {
            error = qsTr("Use at least 8 characters.");
            return;
        }
        if (confirm && password !== confirmationField.text) {
            error = qsTr("The passwords don't match.");
            return;
        }
        visible = false;
        passwordField.text = "";
        confirmationField.text = "";
        accepted(password);
    }

    visible: false
    anchors.fill: parent
    color: "#000000b0"

    MouseArea { anchors.fill: parent }
    Keys.onEscapePressed: cancel()

    Rectangle {
        anchors.centerIn: parent
        width: 420
        height: layout.implicitHeight + 48
        radius: theme.radius
        color: "#1c1c1e"

        ColumnLayout {
            id: layout
            anchors.fill: parent
            anchors.margins: 24
            spacing: 0

            Label { text: qsTr("Certificate store password"); color: "white"; font.pixelSize: 17; font.weight: Font.DemiBold }
            Label {
                Layout.fillWidth: true
                Layout.topMargin: 6
                text: qsTr("This password protects Signed's certificate store. It is asked each time you seal a copy.")
                color: "#b8b8bc"
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }

            TextField {
                id: passwordField
                Layout.fillWidth: true
                Layout.topMargin: 14
                placeholderText: qsTr("Password")
                echoMode: TextInput.Password
                Material.roundedScale: theme.radius
                onAccepted: dialog.submit()
                Keys.onEscapePressed: dialog.cancel()
            }
            TextField {
                id: confirmationField
                visible: dialog.confirm
                Layout.fillWidth: true
                Layout.topMargin: 8
                placeholderText: qsTr("Confirm password")
                echoMode: TextInput.Password
                Material.roundedScale: theme.radius
                onAccepted: dialog.submit()
                Keys.onEscapePressed: dialog.cancel()
            }

            Label {
                Layout.topMargin: 8
                visible: dialog.error !== ""
                text: dialog.error
                color: dialog.accent
                font.pixelSize: 12
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }

            RowLayout {
                Layout.topMargin: 20
                Layout.fillWidth: true
                spacing: 10
                Item { Layout.fillWidth: true }
                BarButton {
                    text: qsTr("Cancel")
                    accent: dialog.accent
                    onClicked: dialog.cancel()
                }
                BarButton {
                    text: qsTr("OK")
                    primary: true
                    accent: dialog.accent
                    accentForeground: dialog.accentForeground
                    onClicked: dialog.submit()
                }
            }
        }
    }
}
