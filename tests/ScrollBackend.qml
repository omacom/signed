import QtQml
import QtQml.Models

// Only the document geometry is needed to exercise Main.qml's scrolling.
QtObject {
    property bool hasDocument: true
    property string fileName: "scroll-test.pdf"
    property int pageCount: 12
    property real widestPage: 600
    property int generation: 0
    property bool dirty: false
    property string notice: ""
    property string lastSignature: ""
    property bool previewEmpty: true
    property string previewName: ""
    property int previewRevision: 0
    property ListModel items: ListModel {
        property bool canUndo: false
        property bool canRedo: false
    }
    property ListModel signatures: ListModel {
        function contains(name) { return false; }
    }
    signal documentChanged()
    signal signatureImageReady()
    signal certificateImportNeedsPassword()
    signal certificateImportFailed(string message)
    signal certificateImported(string nickname)
    function pageWidth(page) { return 600; }
    function pageHeight(page) { return 800; }
}
