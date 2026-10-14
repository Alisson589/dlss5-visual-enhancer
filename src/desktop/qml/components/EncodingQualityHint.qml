import QtQuick
import ".."

Text {
    property string quality: "Auto (Default)"
    text: quality === "Max" ? qsTranslate("App", "Prioritizes quality over file size. Files can be very large.")
        : quality === "Best" ? qsTranslate("App", "Very high visual quality, with less compression.")
        : quality === "Good" ? qsTranslate("App", "High visual quality, with moderate compression.")
        : qsTranslate("App", "Balances visual quality and file size. Size adapts to scene detail.")
    wrapMode: Text.WordWrap
    color: Theme.textSecondary
    font.pixelSize: Theme.fontSizeSmall
}
