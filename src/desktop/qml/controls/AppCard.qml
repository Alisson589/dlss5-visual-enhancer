import QtQuick
import ".."

Item {
    id: card

    property string title: ""
    property bool collapsible: true
    property bool collapsed: false
    default property alias content: contentContainer.data

    implicitWidth: 320
    clip: true
    implicitHeight: header.height + (collapsed ? 0 : (contentContainer.childrenRect.height + 24))

    Rectangle {
        id: bg
        anchors.fill: parent
        radius: Theme.radiusLarge
        color: Theme.bgCard
        border.color: Theme.borderSubtle
        border.width: 1

        // Header
        Rectangle {
            id: header
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 38
            color: "transparent"

            Row {
                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.right: chevron.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8

                Text {
                    text: card.title
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSubtitle
                    font.weight: Font.DemiBold
                    color: Theme.textPrimary
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            AppIcon {
                id: chevron
                visible: card.collapsible
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                iconName: card.collapsed ? "chevron_down" : "chevron_up"
                iconSize: 12
                color: headerMouse.containsMouse ? Theme.textPrimary : Theme.textMuted
            }

            MouseArea {
                id: headerMouse
                objectName: "cardHeaderMouse"
                anchors.fill: parent
                hoverEnabled: card.collapsible
                cursorShape: card.collapsible ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: {
                    if (card.collapsible) {
                        card.collapsed = !card.collapsed
                    }
                }
            }

            // Hairline separator when expanded
            Rectangle {
                visible: !card.collapsed
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.right: parent.right
                anchors.rightMargin: 12
                height: 1
                color: Theme.borderSubtle
            }
        }

        // Content Area
        Item {
            id: contentContainer
            // Keep positioners measured while collapsed. Hiding their ancestor
            // invalidates every nested Column and rebuilds the export layout.
            // The card clips the body below its collapsed header instead.
            enabled: !card.collapsed
            anchors.top: header.bottom
            anchors.topMargin: 12
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.right: parent.right
            anchors.rightMargin: 12
            height: childrenRect.height
            clip: false
        }
    }
}
