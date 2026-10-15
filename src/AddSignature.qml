import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Shapes

// Turns a chosen picture into a named signature: shows the picture beside the
// result with the paper removed, a threshold to tune it, a name, and an
// optional digital seal.
Rectangle {
    id: dialog

    property color accent: "#FFD60A"
    property color accentForeground: "black"
    property string error: ""
    property var certs: []             // the database's certificates: {id, name, organization}
    property string certId: ""         // "" is no seal, "new" makes a certificate on save
    property string importError: ""
    // The seal menu: off, the certificates there are, and the two ways to get
    // another.
    readonly property var certChoices: [{id: "", label: "None"}]
        .concat(certs.map(cert => ({id: cert.id, label: cert.name + (cert.organization !== "" && cert.organization !== cert.name ? " · " + cert.organization : "")})))
        .concat([{id: "new", label: "Create a certificate"}, {id: "import", label: "Import a .p12 file…"}])
    signal saved(string name)

    function open() {
        error = "";
        importError = "";
        threshold.value = 0.25;
        backend.setPreviewThreshold(0.25);
        nameField.text = backend.previewName;
        orgField.clear();
        // Replacing a signature keeps its seal; a new one starts without.
        refreshCerts(backend.signatureKey(backend.previewName));
        visible = true;
        nameField.forceActiveFocus();
        nameField.selectAll();
    }
    function close() {
        visible = false;
    }

    function refreshCerts(preselect) {
        certs = backend.signingCertificates();
        selectCert(certs.some(cert => cert.id === preselect) ? preselect : "");
    }
    function selectCert(id) {
        certId = id;
        certBox.currentIndex = certChoices.findIndex(choice => choice.id === id);
    }
    function save() {
        const name = nameField.text.trim();
        const invalid = backend.signatures.validateName(name);
        if (invalid !== "") {
            error = invalid;
            return;
        }
        const failure = backend.saveSignature(name, orgField.text.trim(), certId);
        if (failure !== "") {
            error = failure;
            return;
        }
        close();
        saved(name);
    }

    readonly property bool replacing: backend.signatures.contains(nameField.text.trim())

    visible: false
    anchors.fill: parent
    color: "#000000b0"

    MouseArea { anchors.fill: parent }
    Keys.onEscapePressed: close()

    Rectangle {
        anchors.centerIn: parent
        width: 620
        height: layout.implicitHeight + 48
        radius: theme.radius
        color: "#1c1c1e"

        ColumnLayout {
            id: layout
            anchors.fill: parent
            anchors.margins: 24
            spacing: 0

            Label { text: "Add signature"; color: "white"; font.pixelSize: 17; font.weight: Font.DemiBold }
            Label {
                Layout.topMargin: 4
                Layout.bottomMargin: 18
                text: "Use a scan or a well-lit photo of your signature on white paper."
                color: "#b8b8bc"
                font.pixelSize: 13
            }

            RowLayout {
                spacing: 14
                Layout.fillWidth: true

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 170
                    radius: Math.min(8, theme.radius)
                    color: "#e9e4da"
                    clip: true
                    Image {
                        anchors.fill: parent
                        anchors.margins: 8
                        source: dialog.visible ? "image://preview/source/" + backend.previewRevision : ""
                        fillMode: Image.PreserveAspectFit
                        cache: false
                        mipmap: true
                    }
                    MouseArea {
                        anchors.fill: parent
                        property var strokes: []
                        property var stroke: []
                        onPressed: function(mouse) {
                            stroke = [{x: mouse.x / width, y: mouse.y / height}];
                        }
                        onPositionChanged: function(mouse) {
                            if (!pressed)
                                return;
                            stroke.push({x: mouse.x / width, y: mouse.y / height});
                            backend.previewDrawing(strokes.concat([stroke]));
                        }
                        onReleased: {
                            strokes = strokes.concat([stroke]);
                            backend.previewDrawing(strokes);
                        }
                    }
                }
                Label { text: "→"; color: "#8e8e93"; font.pixelSize: 20 }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 170
                    radius: Math.min(8, theme.radius)
                    clip: true
                    color: "white"

                    // A checkerboard shows what became transparent.
                    Grid {
                        anchors.fill: parent
                        columns: Math.ceil(width / 8)
                        Repeater {
                            model: Math.ceil(parent.width / 8) * Math.ceil(parent.height / 8)
                            Rectangle {
                                required property int index
                                width: 8
                                height: 8
                                readonly property int columns: Math.ceil(parent.width / 8)
                                color: (Math.floor(index / columns) + index % columns) % 2 ? "#eeeeee" : "white"
                            }
                        }
                    }
                    Image {
                        anchors.fill: parent
                        anchors.margins: 12
                        source: dialog.visible && !backend.previewEmpty ? "image://preview/result/" + backend.previewRevision : ""
                        fillMode: Image.PreserveAspectFit
                        cache: false
                        mipmap: true
                    }
                    Label {
                        anchors.centerIn: parent
                        visible: backend.previewEmpty
                        text: "No ink left — move the slider toward Less."
                        color: "#666"
                        font.pixelSize: 12
                    }
                }
            }

            Label {
                Layout.topMargin: 18
                text: "Paper threshold"
                color: "#b8b8bc"
                font.pixelSize: 12
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label { text: "Less"; color: "#8e8e93"; font.pixelSize: 12 }
                Slider {
                    id: threshold
                    Layout.fillWidth: true
                    from: 0.05
                    to: 0.6
                    value: 0.25
                    onMoved: backend.setPreviewThreshold(value)
                }
                Label { text: "More"; color: "#8e8e93"; font.pixelSize: 12 }
            }

            TextField {
                id: nameField
                Layout.fillWidth: true
                Layout.topMargin: 10
                placeholderText: "Name"
                Material.roundedScale: theme.radius
                onTextChanged: dialog.error = ""
                onAccepted: dialog.save()
                Keys.onEscapePressed: dialog.close()
            }

            Label {
                Layout.topMargin: 4
                visible: dialog.error !== "" || dialog.replacing
                text: dialog.error !== "" ? dialog.error : "This replaces the saved signature “" + nameField.text.trim() + "”."
                color: dialog.error !== "" ? dialog.accent : "#b8b8bc"
                font.pixelSize: 12
            }

            // The digital seal is optional and starts off: most people only
            // need the picture on the page, and a self-signed certificate makes
            // some PDF viewers warn whoever opens the copy.
            RowLayout {
                Layout.topMargin: 14
                Layout.fillWidth: true
                spacing: 12

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label { Layout.fillWidth: true; text: "Digital seal"; color: "white"; font.pixelSize: 13 }
                    Label {
                        Layout.fillWidth: true
                        visible: dialog.certId !== ""
                        text: "Saved copies are sealed, so any later change shows. Self-signed: it proves the file wasn't altered, not who signed it."
                        color: "#8e8e93"
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                }
                ComboBox {
                    id: certBox
                    Layout.preferredWidth: 250
                    Material.roundedScale: theme.radius
                    textRole: "label"
                    valueRole: "id"
                    model: dialog.certChoices
                    // The same panel as the Signature menu.
                    popup.background: Rectangle {
                        color: "#1c1c1e"
                        radius: theme.radius
                        border.color: "#3a3a3c"
                    }
                    onActivated: {
                        const id = currentValue;
                        if (id === "import") {
                            backend.importCertificate();
                            dialog.selectCert(dialog.certId);  // stays put until the import lands
                        } else {
                            dialog.certId = id;
                        }
                    }
                }
            }

            TextField {
                id: orgField
                visible: dialog.certId === "new"
                Layout.fillWidth: true
                Layout.topMargin: 10
                placeholderText: "Organization on the certificate (optional)"
                Material.roundedScale: theme.radius
                onAccepted: dialog.save()
                Keys.onEscapePressed: dialog.close()
            }

            Label {
                Layout.topMargin: 4
                visible: dialog.importError !== ""
                text: dialog.importError
                color: dialog.accent
                font.pixelSize: 12
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }

            RowLayout {
                Layout.topMargin: 20
                Layout.fillWidth: true
                spacing: 10
                BarButton {
                    text: "Choose another image…"
                    accent: dialog.accent
                    onClicked: backend.chooseSignatureImage()
                }
                Item { Layout.fillWidth: true }
                BarButton {
                    text: "Cancel"
                    accent: dialog.accent
                    onClicked: dialog.close()
                }
                BarButton {
                    text: dialog.replacing ? "Replace signature" : "Save signature"
                    primary: true
                    enabled: !backend.previewEmpty
                    accent: dialog.accent
                    accentForeground: dialog.accentForeground
                    onClicked: dialog.save()
                }
            }
        }
    }

    // The password of an imported .p12 file, asked for only when the file
    // turns out to be protected. The app never stores it.
    Rectangle {
        id: passwordDialog
        visible: false
        anchors.fill: parent
        color: "#000000b0"

        MouseArea { anchors.fill: parent }
        Keys.onEscapePressed: cancel()

        function open() {
            passwordField.text = "";
            error = "";
            visible = true;
            passwordField.forceActiveFocus();
        }
        function cancel() {
            visible = false;
            backend.cancelImport();
        }
        function accept() {
            backend.importCertificatePassword(passwordField.text);
        }

        property string error: ""

        Rectangle {
            anchors.centerIn: parent
            width: 420
            height: pwLayout.implicitHeight + 48
            radius: theme.radius
            color: "#1c1c1e"

            ColumnLayout {
                id: pwLayout
                anchors.fill: parent
                anchors.margins: 24
                spacing: 0

                Label { text: "Certificate password"; color: "white"; font.pixelSize: 17; font.weight: Font.DemiBold }
                Label {
                    Layout.topMargin: 4
                    Layout.bottomMargin: 18
                    text: "The certificate file is protected. Its password is asked once for the import and never stored."
                    color: "#b8b8bc"
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }

                TextField {
                    id: passwordField
                    Layout.fillWidth: true
                    placeholderText: "Password"
                    echoMode: TextInput.Password
                    Material.roundedScale: theme.radius
                    onAccepted: passwordDialog.accept()
                    Keys.onEscapePressed: passwordDialog.cancel()
                }

                Label {
                    Layout.topMargin: 8
                    visible: passwordDialog.error !== ""
                    text: passwordDialog.error
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
                        text: "Cancel"
                        accent: dialog.accent
                        onClicked: passwordDialog.cancel()
                    }
                    BarButton {
                        text: "Import"
                        primary: true
                        accent: dialog.accent
                        accentForeground: dialog.accentForeground
                        onClicked: passwordDialog.accept()
                    }
                }
            }
        }
    }

    Connections {
        target: backend
        function onCertificateImportNeedsPassword() { passwordDialog.open(); }
        function onCertificateImportFailed(message) {
            if (passwordDialog.visible)
                passwordDialog.error = "That password didn't open the file.";
            else
                dialog.importError = message;
        }
        function onCertificateImported(nickname) { passwordDialog.visible = false; dialog.importError = ""; dialog.refreshCerts(nickname); }
    }
}
