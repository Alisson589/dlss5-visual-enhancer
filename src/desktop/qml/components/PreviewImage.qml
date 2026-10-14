import QtQuick

Image {
    // Keep the displayed texture until the replacement finishes loading.
    // Preview providers load asynchronously, including memory-cache hits.
    retainWhileLoading: true
    asynchronous: true
    cache: false
    smooth: true
}
