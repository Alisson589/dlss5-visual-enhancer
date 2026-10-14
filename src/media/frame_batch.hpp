#pragma once
#include <QSize>
#include <cstddef>

namespace ve {
// Small working batches for streaming stages; no intermediate video storage.
[[nodiscard]] size_t videoBatchCapacity(QSize largestFrame);
} // namespace ve
