#pragma once
#include "effects.hpp"
#include "pipeline.hpp"
namespace ve {
[[nodiscard]] TimedFrameSource vfxFrameSource(Settings settings,MediaInfo info,TimedFrameSource source,JobControl& control);
[[nodiscard]] bool renderVfxVideo(const RenderRequest& request,const MediaInfo& source,const Settings& settings,
    const PipelineEstimate& estimate,const QString& destination,JobControl& control,const ProgressCallback& progress);
}
