#pragma once
#include "pipeline.hpp"
namespace ve {
[[nodiscard]] bool renderInterpolationVideo(const RenderRequest& request, const MediaInfo& input,
    const Settings& settings, const PipelineEstimate& estimate, const QString& destination,
    JobControl& control, const ProgressCallback& progress);
}
