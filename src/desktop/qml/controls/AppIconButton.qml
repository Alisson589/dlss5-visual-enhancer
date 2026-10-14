import QtQuick
import QtQuick.Controls as QQC2
import ".."

Item {
    id: control

    property string iconName: ""
    property int iconSize: Math.min(18, Math.max(12, buttonSize - 10))
    property string tooltipText: ""
    property bool showTooltip: true
    property int buttonSize: 28
    property color activeColor: Theme.textSecondary
    property color hoverColor: Theme.textPrimary
    property color hoverBackgroundColor: Theme.bgHover
    property color hoverBorderColor: Theme.borderSubtle
    property color pressedBackgroundColor: Theme.bgPressed
    // Native title-bar hit testing bypasses MouseArea hover delivery.
    property bool nativeHovered: false
    readonly property bool hovered: enabled && (nativeHovered || mouseArea.containsMouse)

    signal clicked()

    implicitWidth: buttonSize
    implicitHeight: buttonSize
    width: implicitWidth
    height: implicitHeight
    opacity: enabled ? 1.0 : 0.4
    activeFocusOnTab: enabled
    Accessible.role: Accessible.Button
    Accessible.name: tooltipText !== "" ? tooltipText : iconName.replace(/_/g, " ")
    Keys.onPressed: (event) => {
        if (enabled && (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
            clicked()
            event.accepted = true
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusSmall
        color: mouseArea.pressed ? control.pressedBackgroundColor : (control.hovered ? control.hoverBackgroundColor : "transparent")
        border.color: control.activeFocus ? Theme.accent : (control.hovered ? control.hoverBorderColor : "transparent")
        border.width: 1
        Behavior on color { ColorAnimation { duration: Theme.animFast } }
    }

    AppIcon {
        anchors.centerIn: parent
        iconName: control.iconName
        iconSize: control.iconSize
        color: control.hovered ? control.hoverColor : control.activeColor
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: control.enabled
        cursorShape: control.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: { if (control.enabled) control.clicked() }
    }

    // Popup tooltips live in the window overlay, above clipped queue views.
    QQC2.ToolTip {
        id: tip
        visible: control.showTooltip && control.hovered && control.tooltipText !== ""
        x: (control.width - width) / 2
        y: -height - 6
        text: control.tooltipText
        leftPadding: 6; rightPadding: 6; topPadding: 3; bottomPadding: 3
        contentItem: Text {
            text: tip.text
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.textPrimary
        }
        background: Rectangle {
            radius: Theme.radiusSmall
            color: "#0F1216"
            border.color: Theme.borderActive
            border.width: 1
        }
    }
}
