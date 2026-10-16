import QtQuick
import QtQuick.Controls
import "components"

PopupBase {
    id: root
    property int scal: 0
    property int quality: 3
    property int denoiseQuality: -1
    property int frucFps: -1
    property int frucQuality: 1
    signal scalPicked(int v)
    signal qualityPicked(int v)
    signal denoiseQualityPicked(int v)
    signal frucFpsPicked(int v)
    signal frucQualityPicked(int v)

    width: 360

    readonly property var scaleModel: [
        {label: qsTr("Off"), value: -1},
        {label: qsTr("Auto"), value: 0},
        {label: qsTr("2×"), value: 2},
        {label: qsTr("3×"), value: 3},
        {label: qsTr("4×"), value: 4}
    ]
    readonly property var levelModel: [
        {label: qsTr("Low"), value: 1},
        {label: qsTr("Medium"), value: 2},
        {label: qsTr("High"), value: 3},
        {label: qsTr("Ultra"), value: 4}
    ]
    readonly property var denoiseModel: [
        {label: qsTr("Off"), value: -1},
        {label: qsTr("Low"), value: 8},
        {label: qsTr("Medium"), value: 9},
        {label: qsTr("High"), value: 10},
        {label: qsTr("Ultra"), value: 11}
    ]
    // 插帧档位模型：数据来源改为 C++（PlayerViewModel::frucModel，随编译
    // 后端分支产出标签——VFG = Off/×2/×3/×4，RIFE = Off/40 fps/48 fps/60 fps），
    // 由 main.qml 绑定注入；消费方（modelData.label/value）不变。
    property var frucModel: []
    // 插帧质量档（VFG VideoFrameGeneration 的 mode）：模型同样来自 C++
    //（PlayerViewModel::frucQualityModel = Low/Medium/High）；RIFE 分支
    // frucQualityAvailable=false → 该排隐藏（后端无此参数）。
    property var frucQualityModel: []
    property bool frucQualityAvailable: true

    Column {
        spacing: 12; anchors { left: parent.left; right: parent.right }

        Text { text: qsTr("Scale"); color: "#b0b0b0"; font.pixelSize: 13 }
        Row { spacing: 6; anchors { left: parent.left; right: parent.right }
            Repeater {
                model: root.scaleModel
                delegate: Rectangle {
                    width: 56; height: 32; radius: 4
                    color: sHover.containsMouse ? "#33ffffff"
                         : (root.scal === modelData.value ? "#33ffcc00" : "transparent")
                    Text { anchors.centerIn: parent
                        text: modelData.label
                        color: root.scal === modelData.value ? "#ffcc00" : "#e0e0e0"
                        font.pixelSize: 13 }
                    MouseArea { id: sHover; anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { root.scalPicked(modelData.value); } }
                }
            }
        }

        Rectangle { width: parent.width; height: 1; color: "#0fffffff" }

        Text { text: qsTr("Upscale quality"); color: "#b0b0b0"; font.pixelSize: 13 }
        Row { spacing: 6; anchors { left: parent.left; right: parent.right }
            Repeater {
                model: root.levelModel
                delegate: Rectangle {
                    width: 70; height: 32; radius: 4
                    color: qHover.containsMouse ? "#33ffffff"
                         : (root.quality === modelData.value ? "#33ffcc00" : "transparent")
                    Text { anchors.centerIn: parent
                        text: modelData.label
                        color: root.quality === modelData.value ? "#ffcc00" : "#e0e0e0"
                        font.pixelSize: 13 }
                    MouseArea { id: qHover; anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { root.qualityPicked(modelData.value); } }
                }
            }
        }

        Rectangle { width: parent.width; height: 1; color: "#0fffffff" }

        Text { text: qsTr("Force denoise (active when scale=1)"); color: "#b0b0b0"; font.pixelSize: 13 }
        Row { spacing: 6; anchors { left: parent.left; right: parent.right }
            Repeater {
                model: root.denoiseModel
                delegate: Rectangle {
                    width: 56; height: 32; radius: 4
                    color: dHover.containsMouse ? "#33ffffff"
                         : (root.denoiseQuality === modelData.value ? "#33ffcc00" : "transparent")
                    Text { anchors.centerIn: parent
                        text: modelData.label
                        color: root.denoiseQuality === modelData.value ? "#ffcc00" : "#e0e0e0"
                        font.pixelSize: 13 }
                    MouseArea { id: dHover; anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { root.denoiseQualityPicked(modelData.value); } }
                }
            }
        }

        Rectangle { width: parent.width; height: 1; color: "#0fffffff" }

        Text { text: qsTr("Frame interpolation"); color: "#b0b0b0"; font.pixelSize: 13 }
        Row { spacing: 6; anchors { left: parent.left; right: parent.right }
            Repeater {
                model: root.frucModel
                delegate: Rectangle {
                    width: 64; height: 32; radius: 4
                    color: fHover.containsMouse ? "#33ffffff"
                         : (root.frucFps === modelData.value ? "#33ffcc00" : "transparent")
                    Text { anchors.centerIn: parent
                        text: modelData.label
                        color: root.frucFps === modelData.value ? "#ffcc00" : "#e0e0e0"
                        font.pixelSize: 13 }
                    MouseArea { id: fHover; anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { root.frucFpsPicked(modelData.value); } }
                }
            }
        }

        // ── 插帧质量档（仅 VFG 分支可见）：markup/样式逐字复用上面的
        // "Upscale quality" 排（Text/Row/delegate 的尺寸、配色、字体）；
        // 仅排标题、绑定属性与 MouseArea id 不同。RIFE 无 mode → 整排隐藏
        //（可见性绑定 frucQualityAvailable，隐藏项不参与 Column 布局）。
        Rectangle { width: parent.width; height: 1; color: "#0fffffff"
            visible: root.frucQualityAvailable }

        Text { text: qsTr("Interpolation quality"); color: "#b0b0b0"; font.pixelSize: 13
            visible: root.frucQualityAvailable }
        Row { spacing: 6; anchors { left: parent.left; right: parent.right }
            visible: root.frucQualityAvailable
            Repeater {
                model: root.frucQualityModel
                delegate: Rectangle {
                    width: 70; height: 32; radius: 4
                    color: fqHover.containsMouse ? "#33ffffff"
                         : (root.frucQuality === modelData.value ? "#33ffcc00" : "transparent")
                    Text { anchors.centerIn: parent
                        text: modelData.label
                        color: root.frucQuality === modelData.value ? "#ffcc00" : "#e0e0e0"
                        font.pixelSize: 13 }
                    MouseArea { id: fqHover; anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { root.frucQualityPicked(modelData.value); } }
                }
            }
        }
    }
}
