#pragma once
#include "infrastructure/job.hpp"
#include <QImage>

namespace ve {
// Keep straight alpha separate from the native engines' RGB reconstruction.
// Lanczos uses the reference's sample centers, edge normalization and precision.
void restoreImageAlpha(QImage& output,const QImage& source,JobControl& control);
}
