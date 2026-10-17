#pragma once
#include <QImage>
#include <QImageReader>
#include <QSize>
#include <QString>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <stdexcept>

namespace ve {
inline constexpr int maximumImageDimension = 32768;
inline constexpr int maximumDlssTextureDimension = 16384;
inline constexpr int imageDecoderBudgetMiB = 32768;
inline constexpr quint64 maximumImagePixels = quint64(maximumImageDimension) * maximumImageDimension;

inline void configureImageDecoding() {
    // Set once, before readers run concurrently. Do not change process-wide
    // environment variables or disable the codecs' other data validation.
    static std::once_flag configured;
    std::call_once(configured, [] { QImageReader::setAllocationLimit(imageDecoderBudgetMiB); });
}

inline void requireResolution(QSize size, const QString& operation = QStringLiteral("Image"),
                              int maximum = maximumImageDimension) {
    if (size.width() <= 0 || size.height() <= 0 || size.width() > maximum || size.height() > maximum)
        throw std::runtime_error(QString("%1 dimensions %2x%3 must be positive and at most %4 pixels per side.")
            .arg(operation).arg(size.width()).arg(size.height()).arg(maximum).toStdString());
}

inline qsizetype imageByteCount(QSize size, int pixelBytes) {
    requireResolution(size);
    if (pixelBytes < 1 || pixelBytes > 16)
        throw std::runtime_error("Unsupported image pixel size.");
    // x64 only: even a 32K square float RGBA frame fits qsizetype. Widen
    // before multiplication; several supported frames exceed 32-bit sizes.
    return qsizetype(size.width()) * size.height() * pixelBytes;
}

enum class DimensionRounding { Ceil, EvenCeil, EvenNearest };
inline int checkedDimension(double value, DimensionRounding rounding = DimensionRounding::Ceil) {
    if (!std::isfinite(value) || value <= 0 || value > maximumImageDimension)
        throw std::runtime_error("Requested resolution must be finite, positive and at most 32768 pixels per side.");
    const double rounded = rounding == DimensionRounding::EvenNearest ? std::max(2., std::floor(value / 2 + .5) * 2)
        : rounding == DimensionRounding::EvenCeil ? std::ceil(value / 2) * 2 : std::ceil(value);
    if (rounded > maximumImageDimension)
        throw std::runtime_error("Aligned resolution exceeds 32768 pixels per side.");
    return int(rounded);
}

inline void requireImageAllocation(const QImage& image, const char* operation) {
    if (image.isNull())
        throw std::runtime_error(std::string(operation) + ": insufficient memory for the image buffer.");
}
} // namespace ve
