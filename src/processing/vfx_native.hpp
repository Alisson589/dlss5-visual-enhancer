#pragma once
#include "configuration/settings.hpp"
#include "media/cuda_video.hpp"
#include "video_effects/vfx_bridge.h"
#include <QImage>
#include <QVariantMap>
#include <memory>

namespace ve {
class JobControl;
struct MediaInfo;
[[nodiscard]] QSize vfxSpatialSize(QSize input,const Settings& settings);
[[nodiscard]] double vfxFrameRate(double source,const Settings& settings);
[[nodiscard]] QString vfxReport(const Settings& settings);
void retainVfxPixelAspect(MediaInfo& output,const MediaInfo& source);
[[nodiscard]] QImage vfxDisplayAspect(const QImage&,const MediaInfo& source);
[[nodiscard]] VfxFrameDescV2 vfxDescriptor(const CudaVideoFrame& input,const MediaInfo& colors);
[[nodiscard]] VfxFrameDescV2 vfxDescriptor(const QImage& input,const MediaInfo& colors);
class VfxSurface {
 public:
    VfxSurface()=default;
    [[nodiscard]] explicit operator bool()const{return bool(storage_);}
    [[nodiscard]] VfxFrameDescV2 descriptor()const;
    [[nodiscard]] QImage toHost(bool wide=false)const;
    [[nodiscard]] CudaVideoFrame toVideo(CudaFramePool& pool,bool p010,const MediaInfo& colors)const;
 private:
    struct Storage;
    struct Metrics;
    std::shared_ptr<Storage> storage_;
    explicit VfxSurface(std::shared_ptr<Storage> storage):storage_(std::move(storage)){}
    friend class VideoEffects;
};
class VideoEffects {
 public:
    static void initialize(int ordinal);
    [[nodiscard]] static QString diagnostics();
    VideoEffects(Settings settings,MediaInfo input,JobControl& control);
    ~VideoEffects();
    VideoEffects(const VideoEffects&)=delete;
    VideoEffects& operator=(const VideoEffects&)=delete;
    // Before/after methods allow frame generation to expand the timed stream.
    [[nodiscard]] VfxSurface spatial(const VfxFrameDescV2& input,bool reset=false);
    [[nodiscard]] VfxSurface generate(const VfxSurface& previous,const VfxSurface& current,float timestep,bool shotChange=false);
    [[nodiscard]] VfxSurface process(const VfxFrameDescV2& input,bool reset=false);
    [[nodiscard]] MediaInfo spatialInfo()const;
    [[nodiscard]] MediaInfo outputInfo()const;
    [[nodiscard]] QVariantMap performance()const;
 private:
    struct State;
    std::unique_ptr<State> state_;
};
}
