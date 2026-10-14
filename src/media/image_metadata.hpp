#pragma once
#include "infrastructure/job.hpp"
#include <QByteArray>
#include <QImage>
#include <QString>
namespace ve {
struct ImageMetadata {
    QByteArray exif, xmp, icc;
};
[[nodiscard]] ImageMetadata readImageMetadata(const QString& source, JobControl& control);
void addImageDescription(ImageMetadata& metadata, const QString& note);
void normalizeImageOrientation(ImageMetadata& metadata);
void writeImageMetadata(const QString& destination, const ImageMetadata& metadata, JobControl& control);
[[nodiscard]] QString mergeRenderDescription(const QString& existing, const QString& note);
} // namespace ve
