#include "neural_video_delivery.hpp"
#include "media/native_frame.hpp"

namespace ve {
NeuralVideoDelivery::NeuralVideoDelivery(const QString& path, const QString& codec, const Settings& settings,
    const MediaInfo& info, int ordinal, bool rgb10, JobControl& control)
    : settings_(settings), control_(control), rgb10_(rgb10), device_(cudaPrimaryDevice(ordinal)) {
    auto output = info;
    if (rgb10_) output.pixelFormat = "gbrp10le";
    const auto name = codec.startsWith("H.264") ? QString("h264_nvenc")
        : codec.startsWith("H.265") ? QString("hevc_nvenc") : QString("av1_nvenc");
    encoder_ = std::make_unique<CudaVideoEncoder>(path, name, settings, output, TimeBase{1, 60000000},
                                                 device_.get(), ordinal, control);
    session_ = std::make_unique<NativeEngines::NeuralRendering>();
}
void NeuralVideoDelivery::write(const QImage& input, qint64 pts, qint64 duration, TimeBase base) {
    control_.checkpoint();
    auto image = rgb10_ ? decodedRgb10Rgba(input) : input;
    auto frame = encoder_->allocate();
    frame.pts = pts; frame.duration = duration; frame.timeBase = base;
    session_->process(image, frame, settings_, guide_.reset(image, .24));
    encoder_->write(frame);
}
void NeuralVideoDelivery::finish() {
    encoder_->finish();
    encoder_.reset();
    session_.reset();
}
} // namespace ve
