#pragma once
#include "image_metadata.hpp"

namespace ve {
// Lossless 8-bit PNG encoding with bounded, parallel compression blocks.
void encodePng8(const QString& path, const QImage& image, const ImageMetadata& metadata, JobControl& control);
}
