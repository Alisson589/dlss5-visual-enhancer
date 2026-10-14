#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace ve {
struct FramePosition {
    double seconds = 0;
    std::int64_t number = 1; // The displayed timeline is one-based.
};
[[nodiscard]] inline FramePosition quantizePlayhead(double milliseconds, double fps, double duration) {
    if (!std::isfinite(milliseconds) || milliseconds < 0)
        milliseconds = 0;
    if (!std::isfinite(fps) || fps <= .01)
        fps = 30;
    double seconds = milliseconds / 1000;
    if (std::isfinite(duration) && duration > 0)
        seconds = std::min(seconds, duration);
    const auto bounded = [](double value) {
        return std::int64_t(std::clamp(value, 1., double(std::numeric_limits<int>::max())));
    };
    auto frame = bounded(std::floor(seconds * fps) + 1);
    if (std::isfinite(duration) && duration > 0)
        frame = std::min(frame, bounded(std::nearbyint(duration * fps)));
    return {double(frame - 1) / fps, frame};
}
} // namespace ve
