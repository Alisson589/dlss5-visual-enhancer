#pragma once
#include "media.hpp"
#include "timeline.hpp"
#include "host_video.hpp"
#include <memory>

namespace ve {
enum class TimelinePrecision { Source, Unorm16 };
class VideoFrameReader {
  public:
    struct Result {
        QImage pixels;
        QByteArray identity;
        bool cached = false;
        double absoluteSeconds = 0;
    };
    VideoFrameReader();
    ~VideoFrameReader();
    [[nodiscard]] Result frame(const QString &path, const MediaInfo &info, FramePosition position,
                               JobControl &control, bool timelinePacking, int ordinal, bool preferCuda);
    void prefetch(const QString &path, const MediaInfo &info, FramePosition position, JobControl &control,
                  bool timelinePacking, int ordinal, bool preferCuda);
    void clear();
    [[nodiscard]] std::vector<TimedImage> temporalContext(const QString& path, const MediaInfo& info,
        FramePosition position, double absoluteSeconds, int precedingFrames, JobControl& control,
        int ordinal, bool preferCuda, bool includeFollowing=false);
    [[nodiscard]] qint64 allocatedBytes() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Timeline stills retain the decoder's source precision. Posters use the
// reference's inexpensive JPEG path; neither operation opens native sessions.
[[nodiscard]] QImage decodeTimelineFrame(const QString &path, const MediaInfo &source, double seconds,
                                         JobControl &control,
                                         TimelinePrecision precision = TimelinePrecision::Source);
[[nodiscard]] QImage decodeVideoPoster(const QString &path, double seconds, JobControl &control,
                                       int width = 960);
[[nodiscard]] QImage decodeProcessedVideoPreview(const QString &path, const MediaInfo &source,
                                                 JobControl &control, const QString &selection = "auto");
[[nodiscard]] QImage toneMapVideoPreview(const QImage &pixels, const MediaInfo &colors, JobControl &control,
                                         const QString &selection = "auto");
// The caller owns the directory for the complete processing operation.
[[nodiscard]] QString extractVideoPreviewSource(const QString &path, const MediaInfo &source,
                                                const QString &directory, double seconds, double length,
                                                bool singleFrame, JobControl &control,
                                                const QString &selection = "auto");
} // namespace ve
