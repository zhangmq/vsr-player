import QtQuick
import QtQuick.Controls

Drawer {
    id: root
    edge: Qt.RightEdge
    width: 320; height: parent ? parent.height : 600; z: 10
    dragMargin: 0
    topPadding: 0; bottomPadding: 0; leftPadding: 0; rightPadding: 0
    background: Rectangle { color: "#d9000000" }

    signal fileSelected(string path)

    Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 48; color: "#22ffffff"
        Text { anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
            text: "播放列表"; color: "#e0e0e0"; font.pixelSize: 15; font.bold: true }
        Text { anchors { right: closeBtn.left; rightMargin: 8; verticalCenter: parent.verticalCenter }
            text: playlist ? (playlist.currentIndex + 1) + "/" + playlist.count : ""
            color: "#b0b0b0"; font.pixelSize: 12 }

        Item { id: closeBtn; width: 34; height: 34
            anchors { right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter }
            Rectangle { anchors.fill: parent; radius: 4
                color: clHover.hovered ? "#33ffffff" : "transparent"
                Behavior on color { ColorAnimation { duration: 150 } } }
            Text { anchors.centerIn: parent; font.family: iconFont; font.pixelSize: 18
                text: ""; color: clHover.hovered ? "#ffffff" : "#c8c8c8"
                Behavior on color { ColorAnimation { duration: 150 } }
                renderType: Text.NativeRendering }
            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; hoverEnabled: true
                onClicked: root.close() }
            HoverHandler { id: clHover }
        }
    }

    ListView {
        id: playlistView
        anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: 48
                  bottom: parent.bottom }
        model: playlist ? playlist.files : []
        cacheBuffer: 800
        clip: true

        delegate: Rectangle {
            id: plDelegate
            width: ListView.view.width; height: 42; clip: true
            color: plHover.hovered ? "#22ffffff"
                 : (index === playlist.currentIndex ? "#11ffffff" : "transparent")

            Row {
                anchors { left: parent.left; leftMargin: 8; verticalCenter: parent.verticalCenter }

                Text {
                    text: playlist && index < playlist.displayNames.length
                          ? (index + 1) + ". " + playlist.displayNames[index] : ""
                    width: 296
                    color: index === playlist.currentIndex ? "#ffffff" : "#b0b0b0"
                    font.pixelSize: 13
                    renderType: Text.NativeRendering
                }
            }

            MouseArea {
                anchors.fill: parent; cursorShape: Qt.PointingHandCursor; hoverEnabled: true
                onClicked: { playlist.setCurrentFile(modelData); root.fileSelected(modelData) }
            }

            HoverHandler { id: plHover }

            ToolTip {
                z: 11
                visible: plHover.hovered
                text: modelData
                delay: 600
                background: Rectangle {
                    color: "#d9111111"; radius: 4
                    border { width: 1; color: "#22ffffff" }
                }
                contentItem: Text {
                    text: modelData
                    color: "#e0e0e0"
                    font.pixelSize: 11
                }
            }
        }
    }
}
