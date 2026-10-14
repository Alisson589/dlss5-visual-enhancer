#pragma once
#include "infrastructure/job.hpp"
#include <QSize>
#include <QStringList>

namespace ve {
struct Settings;
[[nodiscard]] QString nativeDeliveryCodec(const Settings& settings);
struct VideoEncoderPlan {
    QStringList options, deviceOptions;
    QString filter, backend;
};
[[nodiscard]] VideoEncoderPlan planVideoEncoder(const QString& selection, const QStringList& softwareOptions,
                                                QSize size, double fps, const QString& primaries,
                                                const QString& transfer, const QString& matrix,
                                                const QString& range, JobControl& control);
struct VideoDecoderPlan {
    QStringList inputOptions;
    QString filter;
};
[[nodiscard]] VideoDecoderPlan planVideoDecoder(const QString& selection, const QString& source,
                                                JobControl& control, const QString& pixelFormat = "rgba64le",
                                                int clockwiseRotation = 0);
} // namespace ve
