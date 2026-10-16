#include "video_still_preview.hpp"
#include "frame_stream.hpp"
#include "infrastructure/paths.hpp"
#include "media/video_preview.hpp"
#include "infrastructure/scratch.hpp"
#include <utility>

namespace ve {
QImage VideoStillPreview::process(const QImage &input, Settings settings, MediaInfo info,
                                  const QStringList &requestedStages, PreviewFrameCache &cache,
                                  const QByteArray &identity, JobControl &control,
                                  const std::vector<TimedImage>& temporal, double generatedSeconds) {
    ProcessingCacheActivity cacheActivity;
    const auto stages=normalizeVfxStages(requestedStages,settings);
    // These are the exact metadata/layout boundaries that the old source FFV1
    // container supplied to the streaming video pipeline. Rotation is already
    // applied by HostVideoDecoder; all source RGB codes are retained.
    info.width = input.width();
    info.height = input.height();
    info.sourceBitDepth = info.precisionBits();
    // The former source container quantized wide layouts back to the original
    // integer depth before the streaming stages read them. Preserve that step.
    info.pixelFormat = ffv1PreviewPixelFormat(info);
    const bool nativeTemporal=stages.contains("dlss_super_resolution");
    if(nativeTemporal)info.pixelFormat=input.depth()>32?"rgba64le":"rgba";
    info.colorSpace = "gbr";
    info.range = "pc";
    info.codec = "ffv1";
    info.metadata.remove("side_data_list");
    info.metadata.remove("tags");
    info.metadata["width"] = info.width;
    info.metadata["height"] = info.height;
    auto temporalIdentity=identity;
    temporalIdentity+=QByteArray::number(generatedSeconds,'g',17);
    if(!temporal.empty()) {
        temporalIdentity+="/native-sr-history/v1/";
        for(const auto& frame:temporal) {
            temporalIdentity+=previewPixelIdentity(frame.image,control);
            temporalIdentity+=QByteArray::number(frame.pts)+'/'+QByteArray::number(frame.timeBase.numerator)+'/'+QByteArray::number(frame.timeBase.denominator);
        }
    }
    const auto keys = previewStageKeys(temporalIdentity, settings, info, stages, true, Paths::instance().root);
    const auto displayKey =
        QCryptographicHash::hash(QByteArray("video-display/v1") + (keys.isEmpty() ? identity : keys.back()),
                                 QCryptographicHash::Sha256);
    if (auto cached = cache.find(displayKey))
        return std::move(cached->pixels);
    if (!nativeTemporal && info.sourceBitDepth > 8 && info.sourceBitDepth < 16 && !stages.isEmpty() &&
        QStringList{"scale_method", "cas_sharpening", "grain", "coloring"}.contains(stages.front())) {
        // Leading GPU filters reconstruct sub-16-bit planar source codes with
        // libplacebo before their first UNORM16 boundary. Retain that exact
        // conversion until its in-memory replacement has pixel parity.
        ScratchDirectory directory(QStringLiteral("preview-wide-XXXXXX"));
        if (!directory.isValid())
            throw std::runtime_error("Cannot create the wide preview directory.");
        const auto sourcePath = directory.filePath("source.mkv");
        auto packing = info;
        packing.pixelFormat = "rgba64le";
        {
            HostVideoEncoder encoder(sourcePath, "FFV1", settings, packing, 0, control,
                                     HostEncodePurpose::NativePreview);
            encoder.write(input, 0, 1, {1, 30});
            encoder.finish();
        }
        settings.video_enabled_stages = stages;
        const auto rendered = directory.filePath("processed.mkv");
        const auto result =
            Pipeline{}.render({sourcePath, rendered, settings, Workflow::NeuralRendering, true}, control);
        auto pixels = decodeProcessedVideoPreview(rendered, result.info, control, settings.ffmpeg_device);
        control.checkpoint();
        cache.store(displayKey, {pixels, false, false});
        return pixels;
    }
    auto source = input;
    if (!nativeTemporal && info.sourceBitDepth > 8) {
        auto packing = info;
        packing.pixelFormat = "rgba64le";
        source = display_.display(input, packing, control);
    }
    auto frames=temporal;
    if(frames.empty())frames.push_back({source,0,1,{1,30}});
    size_t next=0;
    FrameStreamPipeline pipeline(
        settings, info, stages, [&frames,&next]() -> std::optional<TimedImage> {
            if(next>=frames.size())return {};
            return frames[next++];
        }, control, true,temporal.empty()?&cache:nullptr,temporalIdentity);
    auto output = pipeline.read();
    const double generatedTarget=generatedSeconds>=0&&!frames.empty()?std::max(0.,generatedSeconds-frames.front().seconds()):-1;
    while(auto frame=pipeline.read()) {control.checkpoint();if(generatedTarget>=0&&frame->seconds()>generatedTarget+1e-7)break;output=std::move(frame);}
    if (!output)
        throw std::runtime_error("Video still preview returned no frame.");
    if (!stages.isEmpty() && stages.back() != "neural_model" && stages.back() != "dlss_super_resolution")
        info.pixelFormat = "gbrp10le";
    else if (info.pixelFormat == "gbrp10le" && !stages.isEmpty())
        info.pixelFormat = "rgba64le";
    if (stages.contains("super_resolution")) {
        info.primaries = "bt709";
        info.transfer = "bt709";
    }
    if(hasVfxStages(stages)){
        if(!info.hdr){info.primaries="bt709";info.transfer="bt470m";}
    }
    if(stages.contains("rtx_video_hdr")){info.hdr=true;info.primaries="bt2020";info.transfer="smpte2084";}
    info.width = output->image.width();
    info.height = output->image.height();
    auto pixels = display_.display(output->image, info, control);
    control.checkpoint();
    cache.store(displayKey, {pixels, false, false});
    return pixels;
}
} // namespace ve
