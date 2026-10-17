#pragma once
#include "configuration/settings.hpp"
#include <QImage>
#include <QString>
#include <QVariantList>
#include <memory>
#include <vector>
struct FrameDescriptorV1;
struct RtxvFrameDescV1;

namespace ve {
struct CudaVideoFrame;
struct GpuRgbFrame;
class CudaFramePool;
struct MediaInfo;
class JobControl;
struct Gpu {
    int index = 0;
    QString name, uuid, driver;
    qint64 memoryMiB = 0;
    bool compatible = false;
};
[[nodiscard]] std::vector<Gpu> detectGpus(JobControl* control = nullptr);
[[nodiscard]] int gpuOrdinal(const QString& selection);
class NativeEngines {
  public:
    static NativeEngines& instance();
    void prepare(int ordinal = 0, const QStringList& stages = {"neural_model", "dlss_super_resolution"});
    [[nodiscard]] QString diagnostics() const;
    [[nodiscard]] QImage neural(const QImage& input, const Settings& settings, bool video, bool reset);
    class NeuralRendering;
    class SuperResolution;
    class RayReconstruction;
    class RtxVideo;
    class Interpolation;

  private:
    NativeEngines();
    ~NativeEngines();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class NativeEngines::NeuralRendering {
  public:
    NeuralRendering();
    ~NeuralRendering();
    NeuralRendering(const NeuralRendering&) = delete;
    NeuralRendering& operator=(const NeuralRendering&) = delete;
    [[nodiscard]] QImage process(const QImage& input, const Settings& settings, bool video, bool reset,
                                 bool retainResetPreview = false);
    [[nodiscard]] bool sceneReset(const CudaVideoFrame& input, float threshold = .24f);
    void process(const CudaVideoFrame& input, CudaVideoFrame& output, const Settings& settings, bool reset);
    void process(const QImage& input, CudaVideoFrame& output, const Settings& settings, bool reset);
    [[nodiscard]] QImage processHost(const CudaVideoFrame& input, const Settings& settings, bool reset,
                                    bool sixteenBit);

  private:
    struct State;
    std::unique_ptr<State> state_;
};
class NativeEngines::SuperResolution {
  public:
    SuperResolution(QSize input, QSize output, const QString& mode, const QString& preset,
                    const QString& flowQuality, bool still, bool wideOutput = false,
                    quint32 colorTransfer = 0, bool alpha = false);
    ~SuperResolution();
    [[nodiscard]] QImage process(const QImage& input, bool reset, quint32 phase = 0, float frameTimeMs = 0);

  private:
    void* session_ = nullptr;
    QSize output_;
    bool still_=false, wideOutput_=false;
};
class NativeEngines::RayReconstruction {
  public:
    RayReconstruction(QSize size, const QString& flowQuality, bool still, bool wideOutput = false,
                    quint32 colorTransfer = 0, bool alpha = false);
    ~RayReconstruction();
    [[nodiscard]] QImage process(const QImage& input, bool reset, quint32 phase = 0, float frameTimeMs = 0);
    [[nodiscard]] CudaVideoFrame process(const CudaVideoFrame& input, CudaFramePool& pool,
                                         bool p010, bool reset, quint32 phase = 0, float frameTimeMs = 0);
    [[nodiscard]] GpuRgbFrame processRgb(const CudaVideoFrame& input,bool reset,quint32 phase=0,float frameTimeMs=0);
    [[nodiscard]] CudaVideoFrame process(const GpuRgbFrame& input,CudaFramePool& pool,
                                         bool p010,bool reset,quint32 phase=0,float frameTimeMs=0);
    [[nodiscard]] QString diagnostics() const;

  private:
    void* session_ = nullptr;
    QSize output_;
    bool still_=false, wideOutput_=false;
    quint32 transfer_=0;
};
class NativeEngines::RtxVideo {
  public:
    RtxVideo(QSize input, QSize output, const Settings& settings, bool hdr, bool video, bool wideInput = false);
    ~RtxVideo();
    [[nodiscard]] QImage process(const QImage& input);
    [[nodiscard]] QImage process(const CudaVideoFrame& input, const MediaInfo& colors);
    [[nodiscard]] bool linearHdr() const { return hdr_ && floatHdr_ && !video_; }

  private:
    void* session_ = nullptr;
    QSize output_;
    bool hdr_ = false;
    [[nodiscard]] QImage evaluate(const RtxvFrameDescV1& source);
    bool packed_ = false, floatHdr_ = false, video_ = false;
};
class NativeEngines::Interpolation {
  public:
    struct State;
    class Surface {
      public:
        Surface() = default;
        [[nodiscard]] QImage toHost() const;
        [[nodiscard]] QImage toHostRgb10() const;
        [[nodiscard]] CudaVideoFrame toVideo(CudaFramePool& pool, bool p010, uint32_t matrix, uint32_t range) const;
        [[nodiscard]] GpuRgbFrame toRgb() const;
        [[nodiscard]] explicit operator bool() const { return bool(state_); }
      private:
        struct Storage;
        std::shared_ptr<Storage> state_;
        explicit Surface(std::shared_ptr<Storage> state) : state_(std::move(state)) {}
        friend class Interpolation;
    };
    struct Result {
        Surface source;
        std::vector<Surface> generated;
        bool reset = false, sceneCut = false, duplicate = false;
    };
    Interpolation(QSize size, int generated, bool hdr, const QString& flowQuality,
                  bool highPrecision = false, int surfacePoolSize = 0);
    ~Interpolation();
    [[nodiscard]] std::vector<QImage> process(const QImage& input, bool reset);
    [[nodiscard]] Result evaluate(const QImage& input, bool reset = false, bool detectCuts = true,quint32 transfer=0xffffffffu);
    [[nodiscard]] Result evaluate(const CudaVideoFrame& input, const MediaInfo& colors, bool reset = false,
                                  bool detectCuts = true);
    [[nodiscard]] Result evaluate(const Surface& input, bool reset = false, bool detectCuts = false);
    [[nodiscard]] Result evaluate(const GpuRgbFrame& input,bool reset=false,bool detectCuts=true);
    [[nodiscard]] static int nativeMaximum();

  private:
    [[nodiscard]] Result evaluateDescriptor(const FrameDescriptorV1& descriptor, bool reset, bool detectCuts);
    [[nodiscard]] Result copyCurrentInput(Result result, const FrameDescriptorV1& descriptor,quint32 transfer=1);
    [[nodiscard]] Surface own(void* surface);
    std::shared_ptr<State> state_;
    QSize size_;
    bool hdr_ = false;
    bool floating_ = false;
};
} // namespace ve
