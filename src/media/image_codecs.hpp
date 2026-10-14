#pragma once
#include "infrastructure/job.hpp"
#include <QImage>
#include <QSize>
namespace ve {
[[nodiscard]] QImage jpegOverWhite(const QImage& image);
void encodeJpeg(const QString& path, const QImage& image, int quality);
[[nodiscard]] bool isRawImage(const QString& path);
[[nodiscard]] bool isHeifImage(const QString& path);
[[nodiscard]] QSize rawImageSize(const QString& path);
[[nodiscard]] QImage decodeRawImage(const QString& path, JobControl& control);
[[nodiscard]] QImage decodeHeifImage(const QString& path, JobControl& control);
[[nodiscard]] QImage decodeControlMask(const QString& path, JobControl& control);
} // namespace ve
