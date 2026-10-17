#pragma once
#include "infrastructure/job.hpp"
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

namespace ve {
struct MediaInfo {
    int width = 0, height = 0;
    // Retained through RGB working-layout changes so previews follow the source.
    int sourceBitDepth = 0;
    double fps = 0, duration = 0;
    qint64 frames = 0, fileBytes = 0;
    bool video = false, hdr = false, variableFrameRate = false;
    QString codec, pixelFormat, colorSpace = "bt709", primaries = "bt709", transfer = "bt709", range = "tv";
    QJsonObject metadata;
    QJsonArray audioStreams;
    [[nodiscard]] QString description() const;
    [[nodiscard]] int clockwiseRotation() const;
    [[nodiscard]] int precisionBits() const;
    [[nodiscard]] double presentationStartSeconds() const;
    // Set by processing routes that explicitly retain the source pixel aspect.
    [[nodiscard]] QSize outputSampleAspectRatio() const;
};
[[nodiscard]] MediaInfo probeMedia(const QString& path, JobControl* control = nullptr,
                                   const QStringList& inputOptions = {});
[[nodiscard]] QImage decodeImage(const QString& path, JobControl& control);
[[nodiscard]] QImage decodeImageData(const QByteArray& bytes, JobControl& control);
[[nodiscard]] QImage decodeVideoFrame(const QString& path, double seconds, JobControl& control);
void encodeImage(const QString& path, const QImage& pixels, const QString& format, int quality, int bitDepth,
                 JobControl& control, const QString& metadataSource = {}, const QString& note = {});
[[nodiscard]] QByteArray rgbaBytes(const QImage& image, bool sixteenBit = true);
[[nodiscard]] QImage fromRgbaBytes(const QByteArray& bytes, int width, int height, bool sixteenBit = true);
[[nodiscard]] QStringList videoEncoderArguments(const QString& codec, const QString& quality, int width,
                                                int height, double fps, bool hdr);
[[nodiscard]] double targetFrameRate(const QString& choice);
[[nodiscard]] QStringList audioEncoderArguments(const QString& path, const QString& container,
                                                const MediaInfo& source, JobControl& control);
} // namespace ve
