#pragma once
#include "processing/pipeline.hpp"
#include <optional>
namespace ve {
[[nodiscard]] QString neuralRenderNote(const Settings& settings);
[[nodiscard]] QStringList vfxSubtitleArguments(const MediaInfo& source,const Settings& settings);
void muxDelivery(const QString& video, const QString& destination, const RenderRequest& request,
    const MediaInfo& source, const Settings& settings, double duration, JobControl& control,
    std::optional<double> sourceTimeOrigin = {});
}
