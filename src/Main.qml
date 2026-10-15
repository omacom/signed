import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Shapes

ApplicationWindow {
    id: win
    width: 1200
    height: 860
    minimumWidth: 720
    minimumHeight: 480
    visible: true
    title: backend.hasDocument ? backend.fileName + " — Signed" : "Signed"
    color: "#0e0e10"

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    readonly property color accent: theme.accent
    readonly property color accentForeground: theme.accentForeground

    property real zoom: 1
    // Fit the widest page to the window, but no bigger than a comfortable
    // reading size on a wide screen.
    readonly property real fitScale: Math.max(0.1, Math.min(1.6, (pageList.width - 64) / backend.widestPage))
    readonly property real viewScale: fitScale * zoom
    property int currentPage: 0
    property int selected: -1
    property int editingRow: -1
    property bool helpVisible: false
    property bool quitting: false

    // Single-key shortcuts must never fire while typing into a field.
    readonly property bool typing: activeFocusItem !== null && activeFocusItem.cursorPosition !== undefined
    readonly property bool blocked: helpVisible || confirm.visible || addSignature.visible || signatureMenu.opened
    readonly property bool canEdit: backend.hasDocument && !typing && !blocked
    readonly property var selectedItem: selected >= 0 && backend.items.count > selected ? backend.items.get(selected) : null

    readonly property var icons: ({
        signature: "M3 17 C6 11 8 8 9 9 C10 10 7 16 9 16 C11 16 12 11 14 11 C16 11 13 15 15 15 C17 15 18 13 21 12 M3 21 L21 21",
        text: "M5 5 L19 5 M12 5 L12 19 M9 19 L15 19",
        date: "M6 5 L18 5 Q20 5 20 7 L20 18 Q20 20 18 20 L6 20 Q4 20 4 18 L4 7 Q4 5 6 5 Z M4 10 L20 10 M9 3 L9 7 M15 3 L15 7"
    })

    function select(row) {
        if (editingRow >= 0 && editingRow !== row)
            finishEditing();
        selected = row;
    }
    function deselect() {
        if (editingRow >= 0)
            finishEditing();
        selected = -1;
    }

    // The spot on a page under a point in the body, or null when the point
    // isn't over a page.
    function pagePointAt(position) {
        const view = pageList.mapFromItem(body, position.x, position.y);
        const page = pageList.indexAt(pageList.contentX + view.x, pageList.contentY + view.y);
        const row = page >= 0 ? pageList.itemAtIndex(page) : null;
        if (!row)
            return null;
        const local = row.sheet.mapFromItem(body, position.x, position.y);
        if (local.x < 0 || local.y < 0 || local.x > row.sheet.width || local.y > row.sheet.height)
            return null;
        return { page: page, x: local.x / viewScale, y: local.y / viewScale, pointer: true };
    }

    // Where a new item goes: under the mouse when it's over a page, otherwise
    // the middle of the part of the current page that's on screen.
    function insertPoint() {
        if (pageHover.hovered && !blocked) {
            const p = pagePointAt(pageHover.point.position);
            if (p)
                return p;
        }
        const page = currentPage;
        const row = pageList.itemAtIndex(page);
        if (!row)
            return { page: page, x: backend.pageWidth(page) / 2, y: backend.pageHeight(page) / 2 };
        const sheetX = row.x + row.sheet.x;
        const top = Math.max(pageList.contentY, row.y);
        const bottom = Math.min(pageList.contentY + pageList.height, row.y + row.height);
        const left = Math.max(pageList.contentX, sheetX);
        const right = Math.min(pageList.contentX + pageList.width, sheetX + row.sheet.width);
        return { page: page, x: ((left + right) / 2 - sheetX) / viewScale, y: ((top + bottom) / 2 - row.y) / viewScale, pointer: false };
    }

    // A signature picked from the menu (or added through it) goes where the
    // mouse was when the menu opened, not where the menu is.
    property var pendingPoint: null

    function placeSignature(name, at) {
        signatureMenu.close();
        if (!backend.hasDocument)
            return;
        deselect();
        const p = at || insertPoint();
        const row = backend.placeSignature(name, p.page, p.x, p.y);
        if (row >= 0)
            selected = row;
    }
    function placeLastSignature() {
        if (backend.lastSignature !== "")
            placeSignature(backend.lastSignature);
        else
            openSignatureMenu();
    }
    function placeText() {
        deselect();
        const p = insertPoint();
        // A new text item and its first edit are one undo step; if nothing is
        // typed, it disappears without a trace.
        backend.items.beginChange();
        const row = backend.placeText(p.page, p.x, p.y, "", p.pointer);
        if (row < 0) {
            backend.items.endChange();
            return;
        }
        selected = row;
        editingRow = row;
    }
    function placeDate() {
        deselect();
        const p = insertPoint();
        const row = backend.placeText(p.page, p.x, p.y, backend.today(), p.pointer);
        if (row >= 0)
            selected = row;
    }
    function startEditing(row) {
        if (row < 0 || backend.items.get(row).kind !== "text")
            return;
        if (editingRow >= 0)
            finishEditing();
        backend.items.beginChange();
        selected = row;
        editingRow = row;
    }
    function finishEditing() {
        const row = editingRow;
        if (row < 0)
            return;
        editingRow = -1;
        const item = backend.items.get(row);
        if (item.text !== undefined && item.text.trim() === "") {
            backend.items.remove(row);
            selected = -1;
        }
        backend.items.endChange();
        if (row < backend.items.count)
            backend.rememberSize(row);
        pageList.forceActiveFocus();
    }
    function removeSelected() {
        if (selected < 0)
            return;
        backend.items.remove(selected);
        selected = -1;
    }
    function openSignatureMenu() {
        if (!backend.hasDocument && backend.signatures.count === 0) {
            backend.chooseSignatureImage();
            return;
        }
        pendingPoint = backend.hasDocument ? insertPoint() : null;
        signatureMenu.open();
    }
    function goToPage(page) {
        if (page < 0 || page >= backend.pageCount)
            return;
        pageList.positionViewAtIndex(page, ListView.Beginning);
        currentPage = page;
    }
    function updateCurrentPage() {
        let index = pageList.indexAt(pageList.contentX + pageList.width / 2, pageList.contentY + pageList.height / 2);
        if (index < 0)
            index = pageList.indexAt(pageList.contentX + pageList.width / 2, pageList.contentY + pageList.height / 2 + 24);
        if (index >= 0)
            currentPage = index;
    }
    function setZoom(value) {
        zoom = Math.max(0.25, Math.min(4, value));
    }
    // Discarding placed items always asks first.
    function guard(title, action) {
        if (backend.dirty && backend.items.count > 0)
            confirm.ask(title, "Your signatures and text haven't been saved to a signed copy yet.", "Discard", action);
        else
            action();
    }
    function openPdf(url) {
        guard("Open another PDF?", () => {
            selected = -1;
            editingRow = -1;
            if (url)
                backend.openUrl(url);
            else
                backend.open();
        });
    }
    function urlToPath(url) {
        return decodeURIComponent(url.toString().replace(/^file:\/\//, ""));
    }

    onClosing: (close) => {
        if (quitting || !backend.dirty || backend.items.count === 0)
            return;
        close.accepted = false;
        confirm.ask("Quit without saving?", "Your signatures and text haven't been saved to a signed copy yet.", "Quit", () => {
            quitting = true;
            Qt.quit();
        });
    }

    Connections {
        target: backend
        function onDocumentChanged() {
            win.zoom = 1;
            win.currentPage = 0;
            win.selected = -1;
            win.editingRow = -1;
            pageList.positionViewAtBeginning();
        }
        function onSignatureImageReady() { addSignature.open(); }
        function onNoticeChanged() { if (backend.notice !== "") noticeTimer.restart(); }
    }
    Connections {
        target: backend.items
        function onCountChanged() {
            if (win.selected >= backend.items.count)
                win.selected = -1;
            if (win.editingRow >= backend.items.count)
                win.editingRow = -1;
        }
        function onModelReset() {
            win.selected = -1;
        }
    }
    Timer { id: noticeTimer; interval: 6000; onTriggered: backend.clearNotice() }

    // --- shortcuts ---
    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; enabled: !win.typing && !win.blocked; onActivated: win.close() }
    Shortcut { sequence: "Ctrl+Q"; context: Qt.ApplicationShortcut; onActivated: win.close() }
    Shortcut { sequences: [StandardKey.Open]; context: Qt.ApplicationShortcut; enabled: !win.blocked; onActivated: win.openPdf() }
    Shortcut { sequences: [StandardKey.Save]; context: Qt.ApplicationShortcut; enabled: backend.hasDocument && !win.blocked; onActivated: { win.finishEditing(); backend.save(); } }
    Shortcut { sequence: "S"; context: Qt.ApplicationShortcut; enabled: win.canEdit; onActivated: win.placeLastSignature() }
    Shortcut { sequence: "Shift+S"; context: Qt.ApplicationShortcut; enabled: !win.typing && !win.blocked; onActivated: win.openSignatureMenu() }
    Shortcut { sequence: "T"; context: Qt.ApplicationShortcut; enabled: win.canEdit; onActivated: win.placeText() }
    Shortcut { sequence: "D"; context: Qt.ApplicationShortcut; enabled: win.canEdit; onActivated: win.placeDate() }
    Shortcut { sequences: ["Return", "Enter"]; context: Qt.ApplicationShortcut; enabled: win.canEdit && win.selected >= 0; onActivated: win.startEditing(win.selected) }
    Shortcut { sequences: ["Delete", "Backspace", "X"]; context: Qt.ApplicationShortcut; enabled: win.canEdit && win.selected >= 0; onActivated: win.removeSelected() }
    Shortcut { sequence: "Ctrl+D"; context: Qt.ApplicationShortcut; enabled: win.canEdit && win.selected >= 0; onActivated: win.selected = backend.items.duplicate(win.selected) }
    Shortcut { sequence: "Escape"; context: Qt.ApplicationShortcut; enabled: !win.blocked && win.selected >= 0; onActivated: win.deselect() }
    Shortcut { sequence: "Escape"; context: Qt.ApplicationShortcut; enabled: win.helpVisible; onActivated: win.helpVisible = false }
    Shortcut { sequence: "Left"; context: Qt.ApplicationShortcut; enabled: win.canEdit && win.selected >= 0; onActivated: backend.items.nudge(win.selected, -1, 0) }
    Shortcut { sequence: "Right"; context: Qt.ApplicationShortcut; enabled: win.canEdit && win.selected >= 0; onActivated: backend.items.nudge(win.selected, 1, 0) }
    Shortcut { sequence: "Up"; context: Qt.ApplicationShortcut; enabled: win.canEdit && win.selected >= 0; onActivated: backend.items.nudge(win.selected, 0, -1) }
    Shortcut { sequence: "Down"; context: Qt.ApplicationShortcut; enabled: win.canEdit && win.selected >= 0; onActivated: backend.items.nudge(win.selected, 0, 1) }
    Shortcut { sequence: "Shift+Left"; context: Qt.ApplicationShortcut; enabled: win.canEdit && win.selected >= 0; onActivated: backend.items.nudge(win.selected, -10, 0) }
    Shortcut { sequence: "Shift+Right"; context: Qt.ApplicationShortcut; enabled: win.canEdit && win.selected >= 0; onActivated: backend.items.nudge(win.selected, 10, 0) }
    Shortcut { sequence: "Shift+Up"; context: Qt.ApplicationShortcut; enabled: win.canEdit && win.selected >= 0; onActivated: backend.items.nudge(win.selected, 0, -10) }
    Shortcut { sequence: "Shift+Down"; context: Qt.ApplicationShortcut; enabled: win.canEdit && win.selected >= 0; onActivated: backend.items.nudge(win.selected, 0, 10) }
    Shortcut { sequence: "Alt+PgDown"; context: Qt.ApplicationShortcut; enabled: win.canEdit && win.selected >= 0; onActivated: backend.items.moveToPage(win.selected, backend.items.get(win.selected).page + 1) }
    Shortcut { sequence: "Alt+PgUp"; context: Qt.ApplicationShortcut; enabled: win.canEdit && win.selected >= 0; onActivated: backend.items.moveToPage(win.selected, backend.items.get(win.selected).page - 1) }
    Shortcut { sequence: "["; context: Qt.ApplicationShortcut; enabled: win.canEdit && win.selected >= 0; onActivated: { backend.items.scaleBy(win.selected, 1 / 1.1); backend.rememberSize(win.selected); } }
    Shortcut { sequence: "]"; context: Qt.ApplicationShortcut; enabled: win.canEdit && win.selected >= 0; onActivated: { backend.items.scaleBy(win.selected, 1.1); backend.rememberSize(win.selected); } }
    Shortcut { sequences: ["PgUp"]; context: Qt.ApplicationShortcut; enabled: win.canEdit; onActivated: win.goToPage(win.currentPage - 1) }
    Shortcut { sequences: ["PgDown"]; context: Qt.ApplicationShortcut; enabled: win.canEdit; onActivated: win.goToPage(win.currentPage + 1) }
    Shortcut { sequences: ["Ctrl+=", "Ctrl++", StandardKey.ZoomIn]; context: Qt.ApplicationShortcut; enabled: backend.hasDocument; onActivated: win.setZoom(win.zoom * 1.25) }
    Shortcut { sequences: ["Ctrl+-", StandardKey.ZoomOut]; context: Qt.ApplicationShortcut; enabled: backend.hasDocument; onActivated: win.setZoom(win.zoom / 1.25) }
    Shortcut { sequence: "Ctrl+0"; context: Qt.ApplicationShortcut; enabled: backend.hasDocument; onActivated: win.setZoom(1) }
    Shortcut { sequences: [StandardKey.Undo]; context: Qt.ApplicationShortcut; enabled: win.canEdit; onActivated: { win.selected = -1; backend.items.undo(); } }
    Shortcut { sequences: [StandardKey.Redo, "Ctrl+Shift+Z"]; context: Qt.ApplicationShortcut; enabled: win.canEdit; onActivated: { win.selected = -1; backend.items.redo(); } }

    // --- top bar ---
    Rectangle {
        id: topBar
        anchors.left: parent.left
        anchors.right: parent.right
        height: 52
        color: "#1c1c1e"

        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "black" }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 12
            spacing: 8

            Label {
                text: backend.hasDocument ? backend.fileName : "Signed"
                color: "white"
                font.pixelSize: 14
                font.weight: Font.DemiBold
                elide: Text.ElideMiddle
                Layout.maximumWidth: topBar.width - tools.width - 80
            }
            Rectangle {
                visible: backend.dirty && backend.items.count > 0
                width: 7
                height: 7
                radius: 3.5
                color: win.accent
            }
            Item { Layout.fillWidth: true }

            Row {
                id: tools
                spacing: 8
                visible: backend.hasDocument

                BarButton {
                    id: signatureButton
                    text: "Signature"
                    icon: win.icons.signature
                    key: "S"
                    menu: true
                    active: signatureMenu.opened
                    accent: win.accent
                    onClicked: signatureMenu.opened ? signatureMenu.close() : win.openSignatureMenu()
                }
                BarButton {
                    text: "Text"
                    icon: win.icons.text
                    key: "T"
                    accent: win.accent
                    onClicked: win.placeText()
                }
                BarButton {
                    text: "Date"
                    icon: win.icons.date
                    key: "D"
                    accent: win.accent
                    onClicked: win.placeDate()
                }
                Rectangle { width: 1; height: 22; color: "#3a3a3c"; anchors.verticalCenter: parent.verticalCenter }
                BarButton {
                    text: "Save signed copy…"
                    primary: true
                    accent: win.accent
                    accentForeground: win.accentForeground
                    onClicked: { win.finishEditing(); backend.save(); }
                }
            }
        }
    }

    // --- pages ---
    Item {
        id: body
        anchors.top: topBar.bottom
        anchors.bottom: statusBar.top
        anchors.left: parent.left
        anchors.right: parent.right
        visible: backend.hasDocument

        ListView {
            id: thumbs
            width: backend.pageCount > 1 ? 132 : 0
            visible: backend.pageCount > 1
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            topMargin: 16
            bottomMargin: 16
            spacing: 10
            clip: true
            model: backend.hasDocument ? backend.pageCount : 0
            boundsBehavior: Flickable.StopAtBounds

            Rectangle { anchors.fill: parent; color: "#161618"; z: -1 }

            delegate: Item {
                id: thumb
                required property int index
                width: thumbs.width
                height: page.height + 26
                readonly property bool current: index === win.currentPage

                Rectangle {
                    id: page
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 76
                    height: Math.round(76 * backend.pageHeight(thumb.index) / backend.pageWidth(thumb.index))
                    color: "white"
                    opacity: thumb.current ? 1 : 0.85

                    Image {
                        anchors.fill: parent
                        asynchronous: true
                        source: "image://pages/" + backend.generation + "/" + thumb.index + "/" + Math.round(152 * Screen.devicePixelRatio)
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -5
                        color: "transparent"
                        radius: Math.min(3, theme.radius)
                        border.width: 2.5
                        border.color: win.accent
                        visible: thumb.current
                    }
                }
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: page.bottom
                    anchors.topMargin: 6
                    text: thumb.index + 1
                    color: "#8e8e93"
                    font.pixelSize: 11
                }
                TapHandler { onTapped: win.goToPage(thumb.index) }
            }
        }
        HoverHandler { id: pageHover }

        Rectangle { visible: thumbs.visible; x: thumbs.width; width: 1; height: parent.height; color: "black" }

        ListView {
            id: pageList
            anchors.left: thumbs.right
            anchors.leftMargin: thumbs.visible ? 1 : 0
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            clip: true
            focus: true
            spacing: 24
            topMargin: 28
            bottomMargin: 28
            cacheBuffer: Math.max(0, height)
            model: backend.hasDocument ? backend.pageCount : 0
            contentWidth: Math.max(width, backend.widestPage * win.viewScale + 64)
            flickableDirection: Flickable.AutoFlickIfNeeded
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            ScrollBar.horizontal: ScrollBar {}

            onContentYChanged: win.updateCurrentPage()
            onHeightChanged: win.updateCurrentPage()
            onCountChanged: win.updateCurrentPage()

            // ListView pins delegates to x = 0, so each page centers itself
            // inside a full-width row.
            delegate: Item {
                id: row
                required property int index
                readonly property alias sheet: sheet
                width: pageList.contentWidth
                height: sheet.height

                PageView {
                    id: sheet
                    index: row.index
                    viewScale: win.viewScale
                    accent: win.accent
                    x: Math.max(32, (row.width - width) / 2)
                    onBackgroundPressed: {
                        win.deselect();
                        pageList.forceActiveFocus();
                    }
                }
            }

            // Ctrl+scroll zooms, like everywhere else.
            WheelHandler {
                acceptedModifiers: Qt.ControlModifier
                onWheel: (event) => win.setZoom(win.zoom * (event.angleDelta.y > 0 ? 1.1 : 1 / 1.1))
            }
        }
    }

    // --- empty state ---
    ColumnLayout {
        anchors.centerIn: parent
        visible: !backend.hasDocument
        spacing: 14

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.bottomMargin: 8
            width: 120
            height: 150
            radius: theme.radius
            color: "transparent"
            border.color: "#3a3a3c"
            border.width: 2
            Shape {
                anchors.centerIn: parent
                width: 48
                height: 48
                preferredRendererType: Shape.CurveRenderer
                ShapePath {
                    scale: Qt.size(2, 2)
                    strokeColor: "#5a5a60"
                    strokeWidth: 1.5 / 2 * 2
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap
                    joinStyle: ShapePath.RoundJoin
                    PathSvg { path: "M6 3 L14 3 L19 8 L19 21 L6 21 Z M14 3 L14 8 L19 8 M9 16 C11 13 12 11 13 12 C14 13 12 16 14 16 C15 16 16 14 17 14" }
                }
            }
        }
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: "Open a PDF to sign"
            color: "white"
            font.pixelSize: 20
            font.weight: Font.DemiBold
        }
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: "or drop one on the window"
            color: "#b8b8bc"
            font.pixelSize: 14
        }
        BarButton {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 8
            text: "Open PDF…"
            key: "Ctrl+O"
            primary: true
            accent: win.accent
            accentForeground: win.accentForeground
            onClicked: win.openPdf()
        }
    }

    // --- status line ---
    Rectangle {
        id: statusBar
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: 30
        color: "#1c1c1e"

        Rectangle { width: parent.width; height: 1; color: "black" }

        Label {
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            visible: backend.hasDocument
            textFormat: Text.StyledText
            text: {
                const items = backend.items.count;
                let s = "Page " + (win.currentPage + 1) + " of " + backend.pageCount;
                if (items > 0)
                    s += " · " + items + (items === 1 ? " item" : " items");
                if (backend.dirty && items > 0)
                    s += " · <font color=\"" + win.accent + "\">unsaved</font>";
                if (win.zoom !== 1)
                    s += " · " + Math.round(win.zoom * 100) + "%";
                return s;
            }
            color: "#b8b8bc"
            font.pixelSize: 12
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            spacing: 14

            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: {
                    if (backend.notice !== "")
                        return backend.notice;
                    if (win.editingRow >= 0)
                        return "Editing text · Shift+Enter for a new line · Enter or Esc when done";
                    const item = win.selectedItem;
                    if (!item)
                        return "";
                    if (item.kind === "text")
                        return Math.round(item.fontSize * 10) / 10 + " pt · double-click to edit · [ ] to resize";
                    return "Drag to move · drag the corner or [ ] to resize";
                }
                color: backend.notice !== "" ? win.accent : "#8e8e93"
                font.pixelSize: 12
            }
            Rectangle {
                width: 22
                height: 22
                radius: Math.min(5, theme.radius)
                color: helpHover.hovered ? "#2c2c2f" : "transparent"
                border.color: "#3a3a3c"
                Label { anchors.centerIn: parent; text: "?"; color: helpHover.hovered ? "white" : "#7a7a80"; font.pixelSize: 12 }
                HoverHandler { id: helpHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: win.helpVisible = !win.helpVisible }
            }
        }
    }

    // --- drop a PDF to open it, or an image to make it a signature ---
    DropArea {
        anchors.fill: parent
        onEntered: (drag) => drag.accepted = drag.hasUrls
        onDropped: (drop) => {
            if (!drop.hasUrls)
                return;
            const url = drop.urls[0];
            const name = url.toString().toLowerCase();
            if (name.endsWith(".pdf"))
                win.openPdf(url);
            else if (/\.(png|jpe?g|webp)$/.test(name)) {
                // A dropped signature lands where it was dropped.
                win.pendingPoint = backend.hasDocument ? win.pagePointAt(body.mapFromItem(null, drop.x, drop.y)) : null;
                backend.loadSignatureImage(win.urlToPath(url));
            }
        }
    }

    SignatureMenu {
        id: signatureMenu
        parent: signatureButton
        x: signatureButton.width - width
        y: signatureButton.height + 8
        accent: win.accent
        onPicked: (name) => win.placeSignature(name, win.pendingPoint)
        onAddRequested: {
            close();
            backend.chooseSignatureImage();
        }
        onDeleteRequested: (name) => {
            close();
            confirm.ask("Delete “" + name + "”?",
                        "It's removed from your saved signatures. Anything already placed stays.",
                        "Delete", () => backend.removeSignature(name));
        }
    }

    AddSignature {
        id: addSignature
        accent: win.accent
        accentForeground: win.accentForeground
        onSaved: (name) => win.placeSignature(name, win.pendingPoint)
    }

    // --- hotkeys ---
    Rectangle {
        visible: win.helpVisible
        anchors.fill: parent
        color: "#000000cc"

        MouseArea { anchors.fill: parent; onClicked: win.helpVisible = false }

        Rectangle {
            anchors.centerIn: parent
            width: helpLayout.implicitWidth + 64
            height: helpLayout.implicitHeight + 56
            radius: theme.radius
            color: "#1c1c1e"

            ColumnLayout {
                id: helpLayout
                anchors.centerIn: parent
                spacing: 16

                Label { text: "Hotkeys"; color: "white"; font.pixelSize: 17; font.weight: Font.DemiBold }

                GridLayout {
                    columns: 4
                    columnSpacing: 16
                    rowSpacing: 9

                    Repeater {
                        model: [
                            ["S", "Place last signature", "Ctrl+O", "Open a PDF"],
                            ["Shift+S", "Signature menu", "Ctrl+S", "Save signed copy"],
                            ["T", "Place text", "PgUp / PgDn", "Previous / next page"],
                            ["D", "Place today's date", "Ctrl+= / − / 0", "Zoom in / out / fit"],
                            ["Enter", "Edit selected text", "Ctrl+Z", "Undo"],
                            ["Arrows", "Nudge 1 pt (Shift: 10 pt)", "Ctrl+Shift+Z", "Redo"],
                            ["[ / ]", "Smaller / larger", "Esc", "Deselect / stop editing"],
                            ["Ctrl+D", "Duplicate", "Q", "Quit"],
                            ["Delete", "Remove item", "?", "This list"]
                        ]
                        delegate: Repeater {
                            required property var modelData
                            model: 4
                            delegate: Label {
                                required property int index
                                Layout.alignment: index % 2 === 0 ? Qt.AlignRight : Qt.AlignLeft
                                Layout.leftMargin: index === 2 ? 24 : 0
                                text: modelData[index]
                                color: index % 2 === 0 ? win.accent : "#d6d6da"
                                font.pixelSize: 13
                                font.family: index % 2 === 0 ? "monospace" : Qt.application.font.family
                            }
                        }
                    }
                }
            }
        }
    }

    Confirm {
        id: confirm
        accent: win.accent
        accentForeground: win.accentForeground
    }
}
