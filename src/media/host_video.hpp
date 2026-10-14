#pragma once
#include "media.hpp"
#include "packet_stream.hpp"
#include <memory>
#include <optional>

namespace ve {
struct Settings;
struct CudaVideoFrame;
enum class HostDecodePrecision { Source, Unorm8, Unorm16 };
enum class HostEncodePurpose { Delivery, NativePreview };
[[nodiscard]] QString ffv1PreviewPixelFormat(const MediaInfo& info);
class HostRgb10Boundary {
  public:
    HostRgb10Boundary();
    ~HostRgb10Boundary();
    [[nodiscard]] QImage convert(const QImage& image, JobControl& control);
    [[nodiscard]] QImage decodeRgba8(const QImage& image, JobControl& control);
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Job-owned codecs and a reusable reformatter at CPU pixel boundaries.
// Native consumers may use NVDEC even when the delivery codec is software.
class HostVideoDecoder {
  public:
    HostVideoDecoder(const QString& path, const MediaInfo& info, int ordinal,
                     double startSeconds, bool preferCuda, JobControl& control,
                     HostDecodePrecision precision = HostDecodePrecision::Source, bool timelinePacking = false,
                     int decodeThreads = 1);
    ~HostVideoDecoder();
    [[nodiscard]] std::optional<TimedImage> read();
    [[nodiscard]] TimeBase timeBase() const;
    [[nodiscard]] double timeOrigin() const;
    void seek(double startSeconds, JobControl& control);
    void bindControl(JobControl& control);
    void detachControl();
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Preserve the former lossless container's integer packing and display decode
// without encoding/muxing/decoding a one-frame video file.
class HostPreviewBoundary {
  public:
    HostPreviewBoundary();
    ~HostPreviewBoundary();
    [[nodiscard]] QImage display(const QImage& pixels, const MediaInfo& encodedInfo, JobControl& control);
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class HostVideoEncoder {
  public:
    HostVideoEncoder(const QString& path, const QString& codec, const Settings& settings,
                     const MediaInfo& output, int ordinal, JobControl& control,
                     HostEncodePurpose purpose = HostEncodePurpose::Delivery);
    ~HostVideoEncoder();
    void write(const QImage& image, qint64 pts, qint64 duration, TimeBase timeBase, bool forceIntra=false);
    void write(const CudaVideoFrame& frame);
    void finish();
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ve
