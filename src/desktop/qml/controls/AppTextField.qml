import QtQuick
import ".."

Item {
    id: control

    property string label: ""
    property string placeholderText: ""
    property string text: ""
    property int fieldHeight: Theme.controlHeight
    property bool readOnly: false
    property bool commitOnEveryEdit: true

    signal accepted()
    signal textEdited(string newText)
    signal textCommitted(string newText)

    implicitWidth: 240
    implicitHeight: label !== "" ? (fieldHeight + 20) : fieldHeight
    opacity: enabled ? 1.0 : 0.45
    activeFocusOnTab: enabled && !readOnly

    function syncFromCanonical() {
        if (!input.activeFocus || input.text !== control.text) {
            input.text = control.text
        }
    }
    onTextChanged: syncFromCanonical()
    Component.onCompleted: syncFromCanonical()

    Text {
        visible: control.label !== ""
        anchors.top: parent.top
        anchors.left: parent.left
        text: control.label
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeLabel
        color: Theme.textSecondary
    }

    Rectangle {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: control.fieldHeight
        radius: Theme.radiusMedium
        color: Theme.bgInput
        border.color: input.activeFocus ? Theme.accent : Theme.borderDefault
        border.width: 1

        TextInput {
            id: input
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.right: clearBtn.visible ? clearBtn.left : parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            readOnly: control.readOnly
            enabled: control.enabled
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeLabel
            color: Theme.textPrimary
            selectByMouse: true
            selectionColor: Theme.accent
            selectedTextColor: "#FFFFFF"
            clip: true

            onTextEdited: {
                if (control.commitOnEveryEdit) control.textEdited(text)
            }
            onEditingFinished: {
                control.textCommitted(text)
                if (!control.commitOnEveryEdit) control.textEdited(text)
                control.syncFromCanonical()
            }
            onAccepted: {
                control.textCommitted(text)
                if (!control.commitOnEveryEdit) control.textEdited(text)
                control.accepted()
            }

            Text {
                anchors.fill: parent
                visible: input.text === "" && !input.activeFocus
                text: control.placeholderText
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeLabel
                color: Theme.textMuted
            }
        }

        AppIcon {
            id: clearBtn
            visible: input.text !== "" && !control.readOnly && control.enabled
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            iconName: "close"
            iconSize: 14
            color: clearMouse.containsMouse ? Theme.textPrimary : Theme.textMuted
            MouseArea {
                id: clearMouse
                anchors.fill: parent
                anchors.margins: -5
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    input.text = ""
                    control.textEdited("")
                    control.textCommitted("")
                }
            }
        }
    }
}
