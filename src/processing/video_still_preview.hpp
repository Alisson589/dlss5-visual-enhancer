#pragma once
#include "media/host_video.hpp"
#include "pipeline.hpp"
#include "preview_cache.hpp"

namespace ve {
class VideoStillPreview {
  public:
    [[nodiscard]] QImage process(const QImage &input, Settings settings, MediaInfo info,
                                 const QStringList &stages, PreviewFrameCache &cache,
                                 const QByteArray &identity, JobControl &control,
                                 const std::vector<TimedImage>& temporal = {}, double generatedSeconds=-1);

  private:
    HostPreviewBoundary display_;
};
} // namespace ve
