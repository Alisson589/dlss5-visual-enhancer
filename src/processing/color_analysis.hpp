#pragma once
#include "effects.hpp"
#include <span>
namespace ve {
using ColorSamples = std::vector<Rgb>;
[[nodiscard]] Rgb rgbToLab(Rgb rgb);
[[nodiscard]] Rgb labToRgb(Rgb lab);
[[nodiscard]] ColorSamples imageColorSamples(const QImage& image, size_t limit = 220000,
                                             bool trimBorders = false);
[[nodiscard]] QVariantMap recommendColor(const ColorSamples& pixels, const Settings& settings,
                                         JobControl& control, bool hdr = false, size_t frames = 1);
[[nodiscard]] QVariantMap fitReferenceColor(const ColorSamples& source, const ColorSamples& reference,
                                            const Settings& settings, JobControl& control);
[[nodiscard]] std::vector<double> fixedColorFrameTimes(double duration, double fps);
[[nodiscard]] ColorSamples combinedVideoSamples(std::vector<ColorSamples> frames,
                                                size_t* usedFrames = nullptr);
} // namespace ve
