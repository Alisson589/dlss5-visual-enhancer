import QtQuick
import QtQuick.Window
import QtQuick.Dialogs
import QtQuick.Controls as QQC2
import QtCore as Core
import ".."
import "../controls"

Item {
    id: root
    property var appBridge: null
    signal finished()

    function openImport() {
        if (appBridge) importDialog.open()
    }

    function openExport() {
        if (!appBridge) return
        var folder = saveDialog.currentFolder.toString()
        if (folder.length === 0)
            folder = Core.StandardPaths.writableLocation(Core.StandardPaths.DocumentsLocation).toString()
        var filename = "preset_" + Qt.formatDateTime(new Date(), "yyyy-MM-dd_hh-mm-ss") + ".json"
        saveDialog.selectedFile = folder.replace(/\/?$/, "/") + filename
        saveDialog.open()
    }

    function showResult(success) {
        feedback.success = success
        feedback.message = appBridge ? appBridge.presetStatus : ""
        feedback.open()
        if (success) feedbackTimer.restart()
        else feedbackTimer.stop()
    }

    FileDialog {
        id: importDialog
        objectName: "presetImportDialog"
        parentWindow: root.Window.window
        title: qsTranslate("App", "Import Preset JSON")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTranslate("App", "JSON Presets (*.json)"), qsTranslate("App", "All Files (*.*)")]
        onAccepted: {
            if (root.appBridge && selectedFile.toString().length > 0)
                root.showResult(root.appBridge.importPreset(selectedFile.toString()))
            root.finished()
        }
        onRejected: root.finished()
    }

    FileDialog {
        id: saveDialog
        objectName: "presetSaveDialog"
        parentWindow: root.Window.window
        title: qsTranslate("App", "Export Preset JSON")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: [qsTranslate("App", "JSON Presets (*.json)")]
        onAccepted: {
            if (root.appBridge && selectedFile.toString().length > 0)
                root.showResult(root.appBridge.exportPresetTo("", selectedFile.toString()).length > 0)
            root.finished()
        }
        onRejected: root.finished()
    }

    QQC2.Popup {
        id: feedback
        objectName: "presetFeedback"
        property string message: ""
        property bool success: true
        parent: QQC2.Overlay.overlay
        x: Math.max(8, (parent.width - width) / 2)
        y: 58
        width: Math.min(560, parent.width - 16)
        padding: 12
        z: 20001
        closePolicy: QQC2.Popup.CloseOnEscape | QQC2.Popup.CloseOnPressOutside
        background: Rectangle {
            color: Theme.bgSurface
            border.color: feedback.success ? Theme.success : Theme.danger
            radius: Theme.radiusMedium
        }
        contentItem: Row {
            spacing: 8
            Text {
                width: parent.width - dismissButton.width - parent.spacing
                anchors.verticalCenter: parent.verticalCenter
                text: feedback.message
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeLabel
                color: Theme.textPrimary
            }
            AppIconButton {
                id: dismissButton
                iconName: "close"
                tooltipText: qsTranslate("App", "Close")
                onClicked: feedback.close()
            }
        }
        onClosed: feedbackTimer.stop()
    }
    Timer { id: feedbackTimer; interval: 6000; onTriggered: feedback.close() }
}
