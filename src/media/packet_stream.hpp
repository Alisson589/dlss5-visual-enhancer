#pragma once
#include "infrastructure/process.hpp"
#include <QImage>
#include <memory>
#include <optional>
namespace ve {
struct TimeBase {
    int numerator = 1, denominator = 1000000;
};
struct TimedImage {
    QImage image;
    qint64 pts = 0, duration = 0;
    TimeBase timeBase;
    [[nodiscard]] double seconds() const { return double(pts) * timeBase.numerator / timeBase.denominator; }
};
enum class RawPixelFormat { Rgba8, Rgba64, P010, Gbrp10 };
struct TimedPixels {
    QByteArray pixels;
    QSize size;
    RawPixelFormat format = RawPixelFormat::Rgba64;
    qint64 pts = 0, duration = 0;
    TimeBase timeBase;
    [[nodiscard]] double seconds() const { return double(pts) * timeBase.numerator / timeBase.denominator; }
};
class FramePacketReader {
  public:
    FramePacketReader(PipeProcess& source, JobControl& control,
                      RawPixelFormat format = RawPixelFormat::Rgba64);
    ~FramePacketReader();
    [[nodiscard]] std::optional<TimedImage> read();
    [[nodiscard]] std::optional<TimedPixels> readPixels();
    [[nodiscard]] TimeBase timeBase() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ve
