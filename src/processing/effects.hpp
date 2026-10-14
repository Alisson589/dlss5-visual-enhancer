#pragma once
#include "configuration/settings.hpp"
#include "infrastructure/job.hpp"
#include "media/media.hpp"
#include "media/packet_stream.hpp"
#include <QImage>
#include <array>
#include <span>
#include <vector>
#include <memory>
#include <functional>

namespace ve {
struct CudaVideoFrame;
using TimedFrameSource=std::function<std::optional<TimedImage>()>;
// A job-owned Vulkan filter graph for a consecutive group of video cards.
// Each card retains the reference's RGB10 boundary inside the same graph.
class VideoFilterStage {
  public:
    VideoFilterStage(QStringList stages, QSize input, const MediaInfo& colors,
                     const Settings& settings, quint64 firstFrame = 0);
    VideoFilterStage(QStringList stages, const MediaInfo& colors, const Settings& settings,
                     const QString& source, double startSeconds, JobControl& control);
    VideoFilterStage(QStringList stages, const MediaInfo& colors, const Settings& settings,
                     TimedFrameSource source, JobControl& control);
    ~VideoFilterStage();
    [[nodiscard]] std::vector<QImage> process(std::span<const QImage> frames, JobControl& control,
                                             bool planarRgb10 = false);
    [[nodiscard]] std::vector<CudaVideoFrame> processP010(std::span<const QImage> frames, JobControl& control,
                                                        bool planarRgb10 = true);
    [[nodiscard]] std::optional<TimedImage> read();
    [[nodiscard]] TimeBase timeBase() const;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
using Rgb = std::array<float, 3>;
struct ColorGrade {
    double lut_mix = 100, lut_exposure = 0, lut_contrast = 0, lut_highlights = 0, lut_shadows = 0,
           lut_whites = 0, lut_blacks = 0, lut_midtones = 0, lut_temperature = 0, lut_tint = 0, lut_hue = 0,
           lut_vibrance = 0, lut_saturation = 0;
    ColorGrade() = default;
    explicit ColorGrade(const Settings& s)
        : lut_mix(s.lut_mix), lut_exposure(s.lut_exposure), lut_contrast(s.lut_contrast),
          lut_highlights(s.lut_highlights), lut_shadows(s.lut_shadows), lut_whites(s.lut_whites),
          lut_blacks(s.lut_blacks), lut_midtones(s.lut_midtones), lut_temperature(s.lut_temperature),
          lut_tint(s.lut_tint), lut_hue(s.lut_hue), lut_vibrance(s.lut_vibrance),
          lut_saturation(s.lut_saturation) {}
    [[nodiscard]] bool hasAdjustments() const noexcept {
        return lut_mix!=100 || lut_exposure || lut_contrast || lut_highlights || lut_shadows ||
            lut_whites || lut_blacks || lut_midtones || lut_temperature || lut_tint || lut_hue ||
            lut_vibrance || lut_saturation;
    }
};
struct CubeLut {
    int size = 2;
    Rgb minimum{0, 0, 0}, maximum{1, 1, 1};
    std::vector<Rgb> entries;
    [[nodiscard]] Rgb sample(Rgb rgb) const;
    static CubeLut load(const QString& path);
    static CubeLut identity();
};
[[nodiscard]] Rgb gradeRgb(Rgb input, Rgb sampled, const Settings& settings);
[[nodiscard]] Rgb gradeRgb(Rgb input, Rgb sampled, const ColorGrade& settings);
void saveGradedLut(const QString& path, const Settings& settings, JobControl& control,int decimalPlaces=7);
// Coloring-only still previews always resample the selected cube at the
// requested resolution, including identity and unadjusted imported cubes.
[[nodiscard]] QImage colorStillPreview(const QImage& pixels, const Settings& settings, JobControl& control);
[[nodiscard]] QImage colorImage(const QImage& pixels, const Settings& settings, JobControl& control);
[[nodiscard]] QImage sharpenImage(const QImage& pixels, const Settings& settings, JobControl& control,
                                  const QString& transfer = "srgb");
[[nodiscard]] QImage grainImage(const QImage& pixels, const Settings& settings, JobControl& control,
                                quint64 frame = 0, bool hdr = false, bool video = false);
[[nodiscard]] QImage resizeImage(const QImage& pixels, QSize output, const QString& filter,
                                 JobControl& control, bool fit = false, const QString& device = "auto");
[[nodiscard]] std::vector<QImage> sharpenFrames(std::span<const QImage> frames, const Settings& settings,
                                                JobControl& control, const QString& transfer);
[[nodiscard]] std::vector<QImage> grainFrames(std::span<const QImage> frames, const Settings& settings,
                                              JobControl& control, quint64 firstFrame, bool hdr, bool video);
[[nodiscard]] std::vector<QImage> resizeFrames(std::span<const QImage> frames, QSize output,
                                               const QString& filter, JobControl& control, bool fit = false,
                                               const QString& device = "auto");
[[nodiscard]] QString libplacebo(const QString& options = {});
[[nodiscard]] QString filterPath(const QString& path);
} // namespace ve
