import QtQuick
import ".."
import "../controls"

Column {
    id: root
    property var appBridge: null
    property string stageId: ""
    objectName: "vfx-controls-" + stageId
    readonly property var values: appBridge ? appBridge.vfxSettings : ({})
    readonly property bool restoration: values.vfx_vsr_mode >= 8 && values.vfx_vsr_mode <= 15
    readonly property int family: values.vfx_vsr_mode < 8 ? 0 : values.vfx_vsr_mode < 12 ? 1 : values.vfx_vsr_mode < 16 ? 2 : 3
    readonly property var bases: [0,8,12,16]
    spacing: 10
    width: parent ? parent.width : 320
    function set(key,value) { if (appBridge) appBridge.setVfxSetting(key,value) }
    function changeFamily(value) {
        var level = family === 0 ? Math.max(0, values.vfx_vsr_mode - 1) : values.vfx_vsr_mode - bases[family]
        set("vfx_vsr_mode",bases[value] + Math.min(3,level) + (value === 0 ? 1 : 0))
    }
    Loader {
        width: parent.width
        height: item ? item.implicitHeight : 0
        sourceComponent: root.stageId === "vfx_vsr" ? vfx_vsrControls
                       : root.stageId === "vfx_frame_generation" ? vfx_frame_generationControls
                       : root.stageId === "vfx_upscale" ? vfx_upscaleControls
                       : null
    }

    Component {
        id: vfx_vsrControls
        Column {
            width: parent.width; spacing: 10
            AppComboBox { width: parent.width; label: qsTranslate("App","Mode family")
                model: [{label:"Standard Upscaling Modes",value:0},{label:"Denoise Modes",value:1},{label:"Deblur Modes",value:2},{label:"High-Bitrate Modes",value:3}]
                currentValue: root.family; onActivated: v => root.changeFamily(v) }
            AppComboBox { width: parent.width; label: qsTranslate("App","Quality")
                model: root.family === 0 ? [{label:"Bicubic",value:0},{label:"Low",value:1},{label:"Medium",value:2},{label:"High",value:3},{label:"Ultra",value:4}]
                     : [{label:"Low",value:root.bases[root.family]},{label:"Medium",value:root.bases[root.family]+1},{label:"High",value:root.bases[root.family]+2},{label:"Ultra",value:root.bases[root.family]+3}]
                currentValue: root.values.vfx_vsr_mode; onActivated: v => root.set("vfx_vsr_mode",v) }
            AppSlider { width: parent.width; label: qsTranslate("App","Strength"); from:0; to:1; stepSize:0.01; defaultValue:1
                value: root.values.vfx_vsr_strength === undefined ? 1 : root.values.vfx_vsr_strength
                onValueModified: v => root.set("vfx_vsr_strength",v) }
            Column {
                width: parent.width; spacing:10; visible: !root.restoration
                AppComboBox { width: parent.width; label: qsTranslate("App","Sizing mode"); model:["Scale factor","Custom dimensions"]
                    currentValue: root.values.vfx_size_mode; onActivated: v => root.set("vfx_size_mode",v) }
                AppComboBox { width:parent.width; visible:root.values.vfx_size_mode === "Scale factor"; label:qsTranslate("App","Scale factor")
                    model:[{label:"1x",value:1},{label:"1.5x",value:1.5},{label:"2x",value:2},{label:"3x",value:3},{label:"4x",value:4}]
                    currentValue:root.values.vfx_scale_factor; onActivated:v=>root.set("vfx_scale_factor",v) }
                Row {
                    width:parent.width; spacing:8; visible:root.values.vfx_size_mode === "Custom dimensions"
                    AppTextField { width:(parent.width-8)/2; label:qsTranslate("App","Width (px)"); commitOnEveryEdit:false
                        text:String(root.values.vfx_width || 3840); onTextEdited:t=>{var v=parseInt(t);if(v>0)root.set("vfx_width",v)} }
                    AppTextField { width:(parent.width-8)/2; label:qsTranslate("App","Height (px)"); commitOnEveryEdit:false; enabled:!root.values.vfx_aspect_lock
                        text:String(root.values.vfx_height || 2160); onTextEdited:t=>{var v=parseInt(t);if(v>0)root.set("vfx_height",v)} }
                }
                AppSwitch { label:qsTranslate("App","Lock aspect ratio"); checked:!!root.values.vfx_aspect_lock
                    onToggled:c=>root.set("vfx_aspect_lock",c) }
            }
        }
    }
    Component {
        id: vfx_frame_generationControls
        Column {
            width:parent.width; spacing:10
            AppComboBox { width:parent.width; label:qsTranslate("App","Model"); model:[{label:"Low",value:0},{label:"Medium",value:1},{label:"High",value:2}]
                currentValue:root.values.vfx_frame_generation_model; onActivated:v=>root.set("vfx_frame_generation_model",v) }
            AppComboBox { width:parent.width; label:qsTranslate("App","Frame-rate mode"); model:["Multiplier","Target FPS"]
                currentValue:root.values.vfx_frame_rate_mode; onActivated:v=>root.set("vfx_frame_rate_mode",v) }
            AppComboBox { width:parent.width; visible:root.values.vfx_frame_rate_mode === "Multiplier"; label:qsTranslate("App","Multiplier")
                model:[{label:"2x",value:2},{label:"3x",value:3},{label:"4x",value:4},{label:"5x",value:5},{label:"6x",value:6},{label:"7x",value:7},{label:"8x",value:8}]
                currentValue:root.values.vfx_frame_multiplier; onActivated:v=>root.set("vfx_frame_multiplier",v) }
            AppTextField { width:parent.width; visible:root.values.vfx_frame_rate_mode === "Target FPS"; label:qsTranslate("App","Target FPS"); commitOnEveryEdit:false
                text:root.values.vfx_target_fps || "60"; onTextEdited:t=>root.set("vfx_target_fps",t) }
            AppSwitch { label:qsTranslate("App","Automatic scene-cut detection"); checked:!!root.values.vfx_automatic_shot_detection
                onToggled:c=>root.set("vfx_automatic_shot_detection",c) }
        }
    }
    Component {
        id: vfx_upscaleControls
        Column {
            width:parent.width; spacing:10
            AppComboBox { width:parent.width; label:qsTranslate("App","Scale factor")
                model:[{label:"4/3x",value:4/3},{label:"1.5x",value:1.5},{label:"2x",value:2},{label:"3x",value:3},{label:"4x",value:4}]
                currentValue:root.values.vfx_upscale_scale; onActivated:v=>root.set("vfx_upscale_scale",v) }
            AppSlider { width:parent.width; label:qsTranslate("App","Sharpening strength"); from:0; to:1; stepSize:0.01; defaultValue:0.4
                value:root.values.vfx_upscale_strength || 0; onValueModified:v=>root.set("vfx_upscale_strength",v) }
        }
    }
}
