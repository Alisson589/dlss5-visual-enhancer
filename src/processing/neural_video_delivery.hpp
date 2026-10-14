#pragma once
#include "configuration/settings.hpp"
#include "media/cuda_video.hpp"
#include "native_engines.hpp"
#include "temporal_guide.hpp"

namespace ve {
// Final NR writes directly into the encoder's CUDA pool. Host pixels retain
// the upstream card's precision until the one required upload.
class NeuralVideoDelivery {
  public:
    NeuralVideoDelivery(const QString& path, const QString& codec, const Settings& settings,
                        const MediaInfo& info, int ordinal, bool rgb10, JobControl& control);
    void write(const QImage& input, qint64 pts, qint64 duration, TimeBase base);
    void finish();
  private:
    Settings settings_;
    JobControl& control_;
    bool rgb10_;
    TemporalGuide guide_;
    std::shared_ptr<void> device_;
    // Reverse destruction closes NVENC and its retained frames before NR.
    std::unique_ptr<NativeEngines::NeuralRendering> session_;
    std::unique_ptr<CudaVideoEncoder> encoder_;
};
} // namespace ve
