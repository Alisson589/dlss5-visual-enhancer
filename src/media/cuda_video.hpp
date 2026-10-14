#pragma once
#include "media/media.hpp"
#include "media/packet_stream.hpp"
#include <array>
#include <memory>
#include <optional>

namespace ve {
struct Settings;
enum class VideoPixelFormat : uint32_t { Nv12 = 2, P010 = 3 };
enum class FrameMemory : uint32_t { Host = 1, Cuda = 2 };
struct CudaVideoFrame {
    std::shared_ptr<void> owner; // AVFrame reference: includes its hardware pool/device lifetime.
    QSize size;
    VideoPixelFormat format = VideoPixelFormat::Nv12;
    FrameMemory memory = FrameMemory::Cuda;
    std::array<uint64_t, 3> planes{};
    std::array<uint32_t, 3> strides{};
    uint32_t colorMatrix = 1, colorRange = 0, rotation = 0, chromaLocation = 1; // Rotation is clockwise.
    qint64 pts = 0, duration = 0;
    TimeBase timeBase;
};
struct CudaUnavailable : std::runtime_error { using std::runtime_error::runtime_error; };
// Shared primary-context configuration precedes optional native-engine initialization.
void prepareCudaPrimaryContext(int ordinal);
[[nodiscard]] std::shared_ptr<void> cudaPrimaryDevice(int ordinal);
[[nodiscard]] CudaVideoFrame downloadVideoFrame(const CudaVideoFrame& frame);
class CudaFramePool {
  public:
    CudaFramePool(void* device, QSize size, bool p010, bool hdr);
    ~CudaFramePool();
    [[nodiscard]] CudaVideoFrame allocate();
    [[nodiscard]] CudaVideoFrame wrap(std::shared_ptr<void> pixels, const std::array<uint64_t, 3>& planes,
        const std::array<uint32_t, 3>& strides, uint32_t matrix, uint32_t range);
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class CudaVideoDecoder {
  public:
    CudaVideoDecoder(const QString& path, int ordinal, double startSeconds, JobControl& control);
    ~CudaVideoDecoder();
    [[nodiscard]] std::optional<CudaVideoFrame> read();
    [[nodiscard]] TimeBase timeBase() const;
    [[nodiscard]] double timeOrigin() const;
    [[nodiscard]] void* device() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class CudaVideoEncoder {
  public:
    CudaVideoEncoder(const QString& path, const QString& codec, const Settings& settings,
                     const MediaInfo& info, TimeBase timeBase, void* device, int ordinal,
                     JobControl& control);
    ~CudaVideoEncoder();
    [[nodiscard]] VideoPixelFormat pixelFormat() const;
    [[nodiscard]] CudaVideoFrame allocate();
    void write(const CudaVideoFrame& frame);
    void finish();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ve
