#include "frame_batch.hpp"
#include <algorithm>
#include <windows.h>

namespace ve {
size_t videoBatchCapacity(QSize largestFrame) {
    // Bound detached working images while leaving memory for temporal models,
    // decoders and encoders. The batch never grows with the video's duration.
    qint64 budget = 256ll * 1024 * 1024;
    MEMORYSTATUSEX memory{sizeof(MEMORYSTATUSEX)};
    if (GlobalMemoryStatusEx(&memory))
        budget = std::min(budget, qint64(memory.ullAvailPhys / 8));
    const auto workingBytes = qint64(largestFrame.width()) * largestFrame.height() * 8 * 5;
    return size_t(std::clamp<qint64>(budget / std::max<qint64>(1, workingBytes), 1, 32));
}
} // namespace ve
