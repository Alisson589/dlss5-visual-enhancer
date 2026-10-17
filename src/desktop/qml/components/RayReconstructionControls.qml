import QtQuick
import ".."
import "../controls"

Column {
    id: root
    property var appBridge: null
    readonly property bool isImage: appBridge ? appBridge.nrMode === "Image" : true
    readonly property var values: appBridge ? appBridge.rrSettings : ({})
    width: parent ? parent.width : 320
    spacing: 12
    AppComboBox {
        objectName: "rrOpticalFlowQuality"
        width: parent.width
        visible: !root.isImage
        label: qsTranslate("App", "Optical Flow Quality")
        model: appBridge ? appBridge.opticalFlowQualityChoices : ["High", "Medium", "Low"]
        currentValue: root.values.rr_optical_flow_quality || "High"
        onActivated: v => { if (appBridge) appBridge.setRrSetting("rr_optical_flow_quality", v) }
    }
}
