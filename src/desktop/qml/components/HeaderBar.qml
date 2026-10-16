import QtQuick
import QtQuick.Window
import QtQuick.Controls as QQC2
import ".."
import "../controls"

Rectangle {
    id: root
    // Native window buttons retain their Windows positions in RTL locales.
    LayoutMirroring.enabled: false
    LayoutMirroring.childrenInherit: true
    property var appBridge: null
    property var windowRef: null
    readonly property bool compact: width < 1320
    readonly property bool narrow: width < 1140
    signal importPresetRequested()
    signal exportPresetRequested()
    property Item presetOpener: null

    function restorePresetFocus() {
        if (presetOpener) presetOpener.forceActiveFocus()
    }

    function togglePresets(button) {
        presetOpener = button
        if (presetsMenu.opened) presetsMenu.close()
        else {
            // Keep the popup below the native title-bar drag region.
            const top = button.mapToItem(root, 0, 0).y
            presetsMenu.popup(button, (button.width - presetsMenu.width) / 2, root.height - top + 4)
        }
    }

    height: 50
    color: Theme.bgSurface
    border.color: Theme.borderSubtle
    border.width: 1

    // QML drag/double-click fallbacks. On Windows the native filter reports
    // HTCAPTION for empty header space, so the OS owns drag, double-click
    // maximize and edge snap there (double-handling would fight it). The
    // fallbacks stay live until hit-test rects are published, so the header
    // is never dead while the filter is still blind.
    readonly property bool nativeChrome: windowRef && windowRef.nativeChromeActive === true && windowRef.chromeRectsReady === true
    MouseArea {
        anchors.fill: parent; z: 0
        onPressed: { if (!root.nativeChrome && windowRef && windowRef.visibility !== Window.Maximized && windowRef.visibility !== Window.FullScreen) windowRef.startSystemMove() }
        onDoubleClicked: {
            if (!windowRef || root.nativeChrome) return
            root.toggleMaximize()
        }
    }

    // Native maximize/restore (the OS filter supplies a real frame behind the
    // frameless visuals, so geometry, taskbar and animations are correct).
    function toggleMaximize() {
        if (!windowRef) return
        if (windowRef.visibility === Window.Maximized || windowRef.visibility === Window.FullScreen) windowRef.showNormal()
        else windowRef.showMaximized()
    }

    // Window-relative chrome rects (DIPs) for the native hit-test filter, so
    // snap/min/max/close land exactly on the drawn buttons and tab clicks
    // still reach QML. Refreshed whenever the header layout changes.
    function updateChromeRects() {
        if (!windowRef || !windowRef.contentItem) return
        var p
        p = minBtn.mapToItem(windowRef.contentItem, 0, 0)
        windowRef.minBtnRect = Qt.rect(p.x, p.y, Math.max(minBtn.width, minBtn.implicitWidth), Math.max(minBtn.height, minBtn.implicitHeight))
        p = maxBtn.mapToItem(windowRef.contentItem, 0, 0)
        windowRef.maxBtnRect = Qt.rect(p.x, p.y, Math.max(maxBtn.width, maxBtn.implicitWidth), Math.max(maxBtn.height, maxBtn.implicitHeight))
        p = closeBtn.mapToItem(windowRef.contentItem, 0, 0)
        windowRef.closeBtnRect = Qt.rect(p.x, p.y, Math.max(closeBtn.width, closeBtn.implicitWidth), Math.max(closeBtn.height, closeBtn.implicitHeight))
        p = navTabs.mapToItem(windowRef.contentItem, 0, 0)
        windowRef.navTabsRect = Qt.rect(p.x, p.y, navTabs.width, navTabs.height)
        windowRef.chromeRectsReady = windowRef.minBtnRect.width > 0 && windowRef.maxBtnRect.width > 0
            && windowRef.closeBtnRect.width > 0 && windowRef.navTabsRect.width > 0
            && windowRef.minBtnRect.height > 0 && windowRef.maxBtnRect.height > 0
            && windowRef.closeBtnRect.height > 0 && windowRef.navTabsRect.height > 0
    }
    onWidthChanged: Qt.callLater(updateChromeRects)
    Component.onCompleted: Qt.callLater(updateChromeRects)
    Connections {
        target: rightArea
        function onWidthChanged() { Qt.callLater(root.updateChromeRects) }
    }
    Connections {
        target: navTabs
        function onWidthChanged() { Qt.callLater(root.updateChromeRects) }
    }
    Connections {
        target: windowRef
        function onHeightChanged() { Qt.callLater(root.updateChromeRects) }
        function onWidthChanged() { Qt.callLater(root.updateChromeRects) }
        // Maximize/restore settles geometry asynchronously; refresh once the
        // state lands so the native filter never aims at pre-toggle rects.
        function onVisibilityChanged() { Qt.callLater(root.updateChromeRects) }
    }

    Row {
        id: brand; z: 2
        anchors.left: parent.left; anchors.leftMargin: 12; anchors.verticalCenter: parent.verticalCenter; spacing: 8
        Image {
            source: Qt.resolvedUrl("../../../branding/icon.png")
            sourceSize: Qt.size(112, 112)
            width: 28; height: 28
            fillMode: Image.PreserveAspectFit
            smooth: true
            anchors.verticalCenter: parent.verticalCenter
        }
        Text { visible: root.width >= 700; text: "Visual Enhancer"; font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeTitle; font.weight: Font.Normal; color: Theme.textPrimary; anchors.verticalCenter: parent.verticalCenter }
    }

    Row {
        id: rightArea; z: 2
        anchors.right: parent.right; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter; spacing: root.compact ? 6 : 10
        Row {
            spacing: 2
            // Window-chrome buttons: vector icons, no hover tooltips (they sit
            // at y=0 where an above-anchored popup would clip outside the window).
            // On Windows the native filter hit-tests these rects (snap flyout,
            // system press animations); the QML handlers are the fallback path
            // for the pre-rects blind window only. The nativeChrome guard keeps
            // single ownership: without it a transition-frame double delivery
            // would toggle maximize twice (apparent no-op) or fight the OS.
            // Minimize intentionally has no nativeChrome guard: minimize is
            // idempotent, and if the OS hit-test claimed the click QML never
            // receives it — so a click arriving here always means the OS did
            // NOT claim it (mapping miss) and minimizing is the right action.
            // maxBtn/closeBtn keep the guard (toggle/close double-delivery).
            AppIconButton { id: minBtn; objectName: "windowMinimizeButton"; iconName: "minimize"; showTooltip: false; onClicked: { if (windowRef) windowRef.showMinimized() } }
            AppIconButton {
                id: maxBtn
                objectName: "windowMaximizeButton"
                iconName: windowRef && (windowRef.visibility === Window.Maximized || windowRef.visibility === Window.FullScreen) ? "restore_window" : "maximize"
                showTooltip: false
                nativeHovered: root.nativeChrome && windowRef.nativeHoveredButton === "maximize"
                onClicked: { if (root.nativeChrome) return; root.toggleMaximize() }
            }
            AppIconButton {
                id: closeBtn
                objectName: "windowCloseButton"
                iconName: "close"
                showTooltip: false
                hoverBackgroundColor: Theme.danger
                hoverBorderColor: Theme.danger
                pressedBackgroundColor: Theme.dangerPressed
                nativeHovered: root.nativeChrome && windowRef.nativeHoveredButton === "close"
                onClicked: { if (root.nativeChrome) return; if (windowRef) windowRef.close() }
            }
        }
    }

    // Menu-style navigation: plain compact text tabs grouped on the left,
    // active tab shown by accent text color only (no button chrome).
    Row {
        id: navTabs; z: 3
        objectName: "headerNavigation"
        anchors.left: brand.right; anchors.leftMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        spacing: 4
        readonly property real availableWidth: Math.max(0, rightArea.x - x - 8)
        readonly property bool showIcons: availableWidth >= navigationMetrics.width + 4 * 42 + 12
        readonly property real buttonPadding: showIcons ? 42 : 16
        readonly property real labelScale: Math.min(1, Math.max(0, availableWidth - 12 - 4 * buttonPadding) / Math.max(1, navigationMetrics.width + 4))
        readonly property var tabs: [
            { id:"neural-rendering", full:qsTranslate("App", "Neural Rendering"), short:qsTranslate("App", "Neural"), icon:"neural_rendering" },
            { id:"presets", full:qsTranslate("App", "Presets"), short:qsTranslate("App", "Presets"), icon:"save_preset" },
            { id:"settings", full:qsTranslate("App", "Settings"), short:qsTranslate("App", "Settings"), icon:"settings" },
            { id:"help", full:qsTranslate("App", "Help"), short:qsTranslate("App", "Help"), icon:"help" }
        ]
        TextMetrics {
            id: navigationMetrics
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeLabel
            text: navTabs.tabs.map(function(tab) { return root.compact ? tab.short : tab.full }).join("")
        }
        Repeater {
            id: navigationButtons
            model: navTabs.tabs
            Item {
                id: tabBtn
                objectName: "header-" + modelData.id
                activeFocusOnTab: true
                height: 28
                width: navTabs.buttonPadding + Math.ceil(tabLabel.implicitWidth * navTabs.labelScale)
                readonly property bool isActive: modelData.id !== "presets" && appBridge ? appBridge.activeTab === modelData.id : false
                function activate() {
                    forceActiveFocus()
                    if (modelData.id === "presets") root.togglePresets(tabBtn)
                    else {
                        presetsMenu.close()
                        if (appBridge) appBridge.activeTab = modelData.id
                    }
                }
                Keys.onPressed: (event) => {
                    if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space ||
                        (modelData.id === "presets" && event.key === Qt.Key_Down)) {
                        activate(); event.accepted = true
                    } else if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
                        const next = (index + (event.key === Qt.Key_Left ? -1 : 1) + navigationButtons.count) % navigationButtons.count
                        navigationButtons.itemAt(next).forceActiveFocus()
                        event.accepted = true
                    }
                }
                Row {
                    anchors.centerIn: parent
                    spacing: navTabs.showIcons ? 6 : 0
                    AppIcon {
                        visible: navTabs.showIcons
                        iconName: modelData.icon
                        iconSize: 20
                        color: tabBtn.isActive ? Theme.accent : (tabMouse.containsMouse ? Theme.textPrimary : Theme.textSecondary)
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        id: tabLabel
                        width: Math.min(implicitWidth, Math.max(0, tabBtn.width - navTabs.buttonPadding))
                        elide: Text.ElideRight
                        text: root.compact ? modelData.short : modelData.full
                        font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeLabel
                        font.weight: Font.Normal
                        color: tabBtn.isActive ? Theme.accent : (tabMouse.containsMouse ? Theme.textPrimary : Theme.textSecondary)
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
                Accessible.role: modelData.id === "presets" ? Accessible.Button : Accessible.PageTab
                Accessible.name: modelData.full
                Accessible.onPressAction: activate()
                MouseArea {
                    id: tabMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: tabBtn.activate()
                }
            }
        }
    }

    component PresetAction: QQC2.MenuItem {
        id: action
        property string menuIcon: ""
        implicitHeight: 34
        leftPadding: 10; rightPadding: 10
        contentItem: Row {
            spacing: 10
            opacity: action.enabled ? 1 : 0.4
            AppIcon { iconName: action.menuIcon; iconSize: 16; anchors.verticalCenter: parent.verticalCenter }
            Text {
                text: action.text
                font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeLabel
                color: Theme.textPrimary
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        background: Rectangle { color: action.highlighted ? Theme.bgHover : "transparent"; radius: Theme.radiusSmall }
    }

    QQC2.Menu {
        id: presetsMenu
        objectName: "presetsMenu"
        popupType: QQC2.Popup.Item
        focus: true
        margins: 8
        padding: 4
        width: Math.ceil(Math.max(importAction.implicitWidth, exportAction.implicitWidth)) + leftPadding + rightPadding
        closePolicy: QQC2.Popup.CloseOnEscape | QQC2.Popup.CloseOnPressOutside
        background: Rectangle { color: Theme.bgSurface; border.color: Theme.borderDefault; radius: Theme.radiusMedium }
        PresetAction {
            id: importAction
            objectName: "importPresetAction"
            text: qsTranslate("App", "Import Preset")
            menuIcon: "import"
            enabled: root.appBridge && root.appBridge.operationState === "Idle"
            onTriggered: {
                presetsMenu.close()
                Qt.callLater(function() { root.importPresetRequested() })
            }
        }
        PresetAction {
            id: exportAction
            objectName: "exportPresetAction"
            text: qsTranslate("App", "Export Preset")
            menuIcon: "export"
            enabled: root.appBridge !== null
            onTriggered: {
                presetsMenu.close()
                Qt.callLater(function() { root.exportPresetRequested() })
            }
        }
        onOpened: {
            currentIndex = importAction.enabled ? 0 : 1
            if (importAction.enabled) importAction.forceActiveFocus()
            else exportAction.forceActiveFocus()
        }
    }
}
