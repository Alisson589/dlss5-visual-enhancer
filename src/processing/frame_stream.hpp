#pragma once
#include "effects.hpp"

namespace ve {
class PreviewFrameCache;
// Pulling downstream feeds bounded lookahead into each persistent filter.
// All decoders, processes and native cards stay on their owning job thread.
class FrameStreamPipeline {
  public:
    FrameStreamPipeline(Settings settings, MediaInfo info, const QStringList& stages,
                        TimedFrameSource source, JobControl& control, bool finalOutput=true,
                        PreviewFrameCache* singleFrameCache=nullptr, QByteArray identity={});
    [[nodiscard]] std::optional<TimedImage> read();
  private:
    TimedFrameSource source_;
};
} // namespace ve
