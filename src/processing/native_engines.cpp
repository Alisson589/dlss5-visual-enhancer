#include "native_engines.hpp"
#include "frame_interpolation/dlssg_bridge.h"
#include "infrastructure/log.hpp"
#include "infrastructure/paths.hpp"
#include "infrastructure/process.hpp"
#include "media/image_codecs.hpp"
#include "media/native_frame.hpp"
#include "media/cuda_video.hpp"
#include "neural_rendering/dlss5nr_bridge.h"
#include "rtx_video/rtx_video_bridge.h"
#include "super_resolution/dlss_bridge.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QColorSpace>
#include <QJsonDocument>
#include <QRgba64>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <utility>
#include <windows.h>

namespace ve {
namespace {
using NativeInit = int(__cdecl*)(int, const wchar_t*, const wchar_t*, char*, int);
class Module {
  public:
    Module(const QString& folder, const QString& filename, const QString& prefix)
        : directory_(Paths::instance().at("bin/runtime/" + folder)), filename_(filename), prefix_(prefix) {}
    template <class Function> [[nodiscard]] Function function(const char* name) {
        std::call_once(load_, [this] {
            const auto path = QDir::toNativeSeparators(directory_ + '/' + filename_).toStdWString();
            library_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!library_) throw std::runtime_error("Cannot load native engine " + filename_.toStdString() +
                ": Windows error " + std::to_string(GetLastError()));
        });
        const auto symbol = GetProcAddress(library_, name);
        if (!symbol)
            throw std::runtime_error("Native engine is missing export " + std::string(name));
        return reinterpret_cast<Function>(symbol);
    }
    void initialize(int ordinal) {
        if (initialized_ && ordinal_ == ordinal)
            return;
        prepareCudaPrimaryContext(ordinal);
        std::array<char, 4096> error{};
        const auto init = function<NativeInit>((prefix_ + "_init_v2").toUtf8().constData());
        const auto data = Paths::instance().nativeDataDirectory();
        if (!init(ordinal, reinterpret_cast<const wchar_t*>(directory_.utf16()),
                  reinterpret_cast<const wchar_t*>(data.utf16()), error.data(),
                  int(error.size())))
            throw std::runtime_error(error.data());
        initialized_ = true;
        ordinal_ = ordinal;
    }
    [[nodiscard]] QString version() {
        using Version = const char*(__cdecl*)();
        return QString::fromUtf8(function<Version>((prefix_ + "_version").toUtf8().constData())());
    }

  private:
    QString directory_;
    QString filename_;
    std::once_flag load_;
    // Keep driver modules resident through worker/global teardown, as before.
    HMODULE library_ = nullptr;
    QString prefix_;
    bool initialized_ = false;
    int ordinal_ = -1;
};
void check(int okay, const std::array<char, 4096>& error) {
    if (!okay)
        throw std::runtime_error(error[0] ? error.data() : "Native engine call failed.");
}
uint32_t flowIndex(const QString& quality) {
    return uint32_t(std::max<qsizetype>(0, QStringList{"High", "Medium", "Low"}.indexOf(quality)));
}
Dlss5NrFrameDescV1 cudaDescriptor(const CudaVideoFrame& view) {
    if (view.memory != FrameMemory::Cuda)
        throw std::runtime_error("NR's CUDA frame contract requires device memory; host input must use packed RGBA.");
    if (!view.owner || view.size.isEmpty() || !view.planes[0] || !view.planes[1])
        throw std::runtime_error("Invalid owned CUDA frame.");
    Dlss5NrFrameDescV1 result{};
    result.struct_size = sizeof(result); result.abi_version = 11;
    result.memory_type = DLSS5NR_CUDA;
    result.pixel_format = std::to_underlying(view.format);
    result.width = uint32_t(view.size.width()); result.height = uint32_t(view.size.height());
    std::ranges::copy(view.planes, result.planes);
    std::ranges::copy(view.strides, result.strides);
    result.color_matrix = view.colorMatrix; result.color_range = view.colorRange;
    result.rotation = view.rotation; result.reserved = view.chromaLocation;
    result.timestamp = view.pts;
    return result;
}
Dlss5NrFrameDescV1 packedDescriptor(PackedFrame& packed) {
    Dlss5NrFrameDescV1 frame{};
    frame.struct_size = sizeof(frame); frame.abi_version = 11;
    frame.memory_type = DLSS5NR_HOST;
    frame.pixel_format = std::to_underlying(packed.format());
    frame.width = uint32_t(packed.image().width()); frame.height = uint32_t(packed.image().height());
    frame.planes[0] = reinterpret_cast<uint64_t>(packed.image().bits());
    frame.strides[0] = uint32_t(packed.image().bytesPerLine());
    frame.color_matrix = 1; frame.color_range = 1;
    return frame;
}
Dlss5NrRenderParamsV11 neuralParameters(const Settings& settings, QSize size, bool video, bool reset,
                                       QImage& mask, QString& maskIdentity) {
    Dlss5NrRenderParamsV11 parameters{};
    parameters.struct_size = sizeof(parameters); parameters.abi_version = 11;
    const auto style = QStringList{"Style 0", "Style 1", "Style 2"}.indexOf(settings.nr_style);
    if (style < 0)
        throw std::runtime_error("Unsupported Neural Rendering style.");
    parameters.style = int(style);
    parameters.intensity = float(settings.nr_intensity);
    parameters.tone = float(settings.local_tone_strength);
    parameters.structure = float(settings.local_structure_strength);
    parameters.skin = float(settings.skin_structure_strength);
    parameters.automask = settings.automatic_mask; parameters.reset = reset;
    parameters.nr_passes = settings.nr_passes;
    parameters.native_intensity = std::min(1.f, parameters.intensity);
    parameters.mask_memory_type = DLSS5NR_NONE;
    parameters.motion_mode = video ? 1u : 0u;
    parameters.optical_flow_quality = video ? flowIndex(settings.nr_optical_flow_quality) : 0u;
    const auto path = settings.nr_mask.value("path").toString();
    if (!path.isEmpty()) {
        const QFileInfo file(path);
        const auto supplied = qvariant_cast<QImage>(settings.nr_mask.value("pixels"));
        const auto identity = file.absoluteFilePath() + '|' + QString::number(file.size()) + '|' +
            QString::number(file.lastModified().toMSecsSinceEpoch()) + '|' +
            settings.nr_mask.value("revision").toString() + '|' + QString::number(supplied.cacheKey()) + '|' +
            QString::number(size.width()) + 'x' + QString::number(size.height());
        if (mask.isNull() || identity != maskIdentity) {
            mask = qvariant_cast<QImage>(settings.nr_mask.value("pixels"));
            if (mask.isNull()) { JobControl control; mask = decodeControlMask(path, control); }
            mask = mask.scaled(size, Qt::IgnoreAspectRatio, Qt::FastTransformation);
            maskIdentity = identity;
        }
        if (mask.isNull() || mask.size() != size || mask.format() != QImage::Format_RGBA32FPx4)
            throw std::runtime_error("The Neural Rendering float RGBA control mask is unavailable.");
        parameters.mask_memory_type = DLSS5NR_HOST;
        parameters.mask_width = uint32_t(mask.width()); parameters.mask_height = uint32_t(mask.height());
        parameters.mask_stride = uint32_t(mask.bytesPerLine());
        parameters.mask_plane = reinterpret_cast<uint64_t>(mask.constBits());
        parameters.mask_revision = std::max<quint64>(1,
            settings.nr_mask.value("revision", QFileInfo(path).lastModified().toMSecsSinceEpoch()).toULongLong());
        parameters.automask = 0;
    } else {
        mask = {};
        maskIdentity.clear();
    }
    return parameters;
}
} // namespace
struct NativeEngines::Impl {
    Module nr{"dlssnr", "neuroframe_engine_neural_rendering.dll", "dlss5nr"};
    Module sr{"dlsssr", "neuroframe_engine_super_resolution.dll", "dlsssr"};
    Module rtx{"rtx_video", "neuroframe_engine_upscaling.dll", "rtxv"};
    Module fi{"dlssg", "neuroframe_engine_frame_interpolation.dll", "fi"};
    std::recursive_mutex mutex;
    std::mutex neuralSessionMutex;
    bool neuralPoisoned = false;
    int ordinal = 0;
    QString diagnostics;
};
NativeEngines::NativeEngines() : impl_(std::make_unique<Impl>()) {}
NativeEngines::~NativeEngines() = default;
NativeEngines& NativeEngines::instance() {
    static NativeEngines engines;
    return engines;
}
namespace {
std::vector<Gpu> queryGpus(JobControl* control) {
    std::vector<Gpu> gpus;
    try {
        const auto result =
            runProcess("nvidia-smi", {"--query-gpu=index,name,uuid,memory.total,driver_version,compute_cap",
                                      "--format=csv,noheader,nounits"}, control);
        if (result.exitCode != 0)
            return gpus;
        for (const auto& line : QString::fromUtf8(result.output).split('\n', Qt::SkipEmptyParts)) {
            const auto columns = line.split(',');
            if (columns.size() < 6)
                continue;
            gpus.push_back({columns[0].trimmed().toInt(), columns[1].trimmed(), columns[2].trimmed(),
                            columns[4].trimmed(), columns[3].trimmed().toLongLong(),
                            columns[5].trimmed().toDouble() >= 7.5});
        }
    } catch (const Cancelled&) {
        throw;
    } catch (const std::exception& error) {
        info("gpu", QString::fromUtf8(error.what()));
    }
    return gpus;
}
} // namespace
std::vector<Gpu> detectGpus(JobControl* control) {
    static std::mutex mutex;
    static std::optional<std::vector<Gpu>> devices;
    std::lock_guard lock(mutex);
    if (control) control->checkpoint();
    if (!devices) {
        auto found = queryGpus(control);
        if (control) control->checkpoint();
        devices = std::move(found);
    }
    return *devices;
}
int gpuOrdinal(const QString& selection) {
    if (selection == "auto")
        return 0;
    for (const auto& gpu : detectGpus())
        if (gpu.uuid.compare(selection, Qt::CaseInsensitive) == 0 || QString::number(gpu.index) == selection)
            return gpu.index;
    throw std::runtime_error("Selected NVIDIA GPU is unavailable.");
}
void NativeEngines::prepare(int ordinal, const QStringList& stages) {
    std::lock_guard lock(impl_->mutex);
    impl_->ordinal = ordinal;
    if (stages.contains("neural_model") || stages.contains("dlss_super_resolution"))
        impl_->sr.initialize(ordinal); // Register NGX search paths before feature 18 creates its device.
    if (stages.contains("neural_model")) impl_->nr.initialize(ordinal);
    if (stages.contains("frame_generation")) impl_->fi.initialize(ordinal);
    if (stages.contains("super_resolution") || stages.contains("rtx_video_hdr")) impl_->rtx.initialize(ordinal);
    using Abi = uint32_t(__cdecl*)();
    if (impl_->nr.function<Abi>("dlss5nr_frame_abi_version")() != 11 ||
        impl_->sr.function<Abi>("dlsssr_abi_version")() != 4)
        throw std::runtime_error("The bundled NVIDIA engines use an incompatible ABI.");
    impl_->diagnostics = "Neural Rendering " + impl_->nr.version() + " (ABI 11)\nDLSS Super Resolution " +
                         impl_->sr.version() + " (ABI 4)";
    for (auto* module : {&impl_->fi, &impl_->rtx}) {
        try {
            impl_->diagnostics += '\n' + module->version();
        } catch (const std::exception& error) {
            impl_->diagnostics += '\n' + QString::fromUtf8(error.what());
        }
    }
    info("runtime", impl_->diagnostics);
}
QString NativeEngines::diagnostics() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->diagnostics;
}
struct NativeEngines::NeuralRendering::State {
    // Feature 18 has one process-wide temporal history. Jobs own it exclusively.
    std::unique_lock<std::mutex> ownership{instance().impl_->neuralSessionMutex};
    QImage mask;
    QString maskIdentity;
    qint64 frames=0;
    bool reusableStill=false;
};
NativeEngines::NeuralRendering::NeuralRendering() : state_(std::make_unique<State>()) {
    auto& impl = *instance().impl_;
    std::lock_guard lock(impl.mutex);
    if (impl.neuralPoisoned)
        throw std::runtime_error("Neural Rendering did not release its previous session. Restart the application.");
    impl.nr.initialize(impl.ordinal);
}
NativeEngines::NeuralRendering::~NeuralRendering() {
    auto& impl = *instance().impl_;
    std::lock_guard lock(impl.mutex);
    // Still evaluation resets native history on every call. Keep one feature
    // and its scratch buffers ready for the next image/preview; the bridge
    // rebuilds on geometry, style, device or motion-mode changes. Explicit
    // reset video previews use the same reuse; temporal jobs and failures
    // retain the existing full-release ownership rules.
    if (state_->reusableStill) return;
    try {
        // The synchronous frame ABI completes all GPU work before returning. NGX itself
        // stays loaded; only the job's feature, surfaces and scratch buffers are released.
        using Release = int(__cdecl*)();
        if (!impl.nr.function<Release>("dlss5nr_release_session")()) {
            impl.neuralPoisoned = true;
            info("runtime", "Neural Rendering session still owns pending GPU work.");
        }
    } catch (const std::exception& error) {
        impl.neuralPoisoned = true;
        info("runtime", QString::fromUtf8(error.what()));
    }
}
QImage NativeEngines::neural(const QImage& input, const Settings& settings, bool video, bool reset) {
    NeuralRendering session;
    return session.process(input, settings, video, reset);
}
QImage NativeEngines::NeuralRendering::process(const QImage& input, const Settings& settings, bool video,
                                             bool reset, bool retainResetPreview) {
    state_->reusableStill=false;
    auto& impl = *instance().impl_;
    std::lock_guard lock(impl.mutex);
    PackedFrame source(input);
    PackedFrame output(source.image().size(), source.format());
    const auto parameters = neuralParameters(settings, input.size(), video, reset, state_->mask, state_->maskIdentity);
    std::array<char, 4096> error{};
    static_assert(sizeof(Dlss5NrFrameDescV1) == 88);
    static_assert(sizeof(Dlss5NrRenderParamsV11) == 88);
    static_assert(sizeof(Dlss5NrFrameResultV1) == 56);
    const auto from = packedDescriptor(source), to = packedDescriptor(output);
    Dlss5NrFrameResultV1 result{};
    result.struct_size = sizeof(result);
    result.abi_version = 11;
    using Process = int(__cdecl*)(const Dlss5NrFrameDescV1*, const Dlss5NrFrameDescV1*,
                                 const Dlss5NrRenderParamsV11*, Dlss5NrFrameResultV1*, char*, int);
    check(impl.nr.function<Process>("dlss5nr_process_frame_v11")(
              &from, &to, &parameters, &result, error.data(), int(error.size())), error);
    // Isolated video-frame previews use the video model/motion mode, but reset
    // history every time. Keep only completed GPU resources, never the lease.
    state_->reusableStill=(!video || retainResetPreview && reset) && settings.nr_mask.value("path").toString().isEmpty();
    return output.image();
}
bool NativeEngines::NeuralRendering::sceneReset(const CudaVideoFrame& input, float threshold) {
    auto& impl = *instance().impl_;
    std::lock_guard lock(impl.mutex);
    const auto source = cudaDescriptor(input);
    std::array<char, 4096> error{};
    float score = 0;
    int reset = 0;
    using Score = int(__cdecl*)(const Dlss5NrFrameDescV1*, float, float*, int*, char*, int);
    check(impl.nr.function<Score>("dlss5nr_scene_score_v1")(&source, threshold, &score, &reset,
          error.data(), int(error.size())), error);
    return reset != 0;
}
void NativeEngines::NeuralRendering::process(const CudaVideoFrame& input, CudaVideoFrame& output,
                                             const Settings& settings, bool reset) {
    state_->reusableStill=false;
    auto& impl = *instance().impl_;
    std::lock_guard lock(impl.mutex);
    const auto source = cudaDescriptor(input), destination = cudaDescriptor(output);
    const auto parameters = neuralParameters(settings, output.size, true, reset, state_->mask, state_->maskIdentity);
    Dlss5NrFrameResultV1 result{};
    result.struct_size = sizeof(result); result.abi_version = 11;
    std::array<char, 4096> error{};
    using Process = int(__cdecl*)(const Dlss5NrFrameDescV1*, const Dlss5NrFrameDescV1*,
                                 const Dlss5NrRenderParamsV11*, Dlss5NrFrameResultV1*, char*, int);
    check(impl.nr.function<Process>("dlss5nr_process_frame_v11")(&source, &destination, &parameters,
          &result, error.data(), int(error.size())), error);
    output.pts = input.pts; output.duration = input.duration;
    output.timeBase = input.timeBase;
}
void NativeEngines::NeuralRendering::process(const QImage& input, CudaVideoFrame& output,
                                             const Settings& settings, bool reset) {
    state_->reusableStill=false;
    auto& impl = *instance().impl_;
    std::lock_guard lock(impl.mutex);
    PackedFrame packed(input);
    auto source = packedDescriptor(packed);
    source.timestamp = output.pts;
    const auto destination = cudaDescriptor(output);
    const auto parameters = neuralParameters(settings, output.size, true, reset, state_->mask, state_->maskIdentity);
    Dlss5NrFrameResultV1 result{};
    result.struct_size = sizeof(result); result.abi_version = 11;
    std::array<char, 4096> error{};
    using Process = int(__cdecl*)(const Dlss5NrFrameDescV1*, const Dlss5NrFrameDescV1*,
                                 const Dlss5NrRenderParamsV11*, Dlss5NrFrameResultV1*, char*, int);
    check(impl.nr.function<Process>("dlss5nr_process_frame_v11")(&source, &destination, &parameters,
          &result, error.data(), int(error.size())), error);
}
QImage NativeEngines::NeuralRendering::processHost(const CudaVideoFrame& input, const Settings& settings,
                                                 bool reset, bool sixteenBit) {
    state_->reusableStill=false;
    auto& impl = *instance().impl_;
    std::lock_guard lock(impl.mutex);
    const auto source = cudaDescriptor(input);
    const auto size = input.rotation % 180 ? QSize(input.size.height(), input.size.width()) : input.size;
    PackedFrame output(size, sixteenBit ? PackedFrame::Format::Rgba16 : PackedFrame::Format::Rgba8);
    auto destination = packedDescriptor(output);
    destination.color_matrix = input.colorMatrix;
    destination.color_range = 1;
    const auto parameters = neuralParameters(settings, output.image().size(), true, reset, state_->mask, state_->maskIdentity);
    Dlss5NrFrameResultV1 result{};
    result.struct_size = sizeof(result); result.abi_version = 11;
    std::array<char, 4096> error{};
    using Process = int(__cdecl*)(const Dlss5NrFrameDescV1*, const Dlss5NrFrameDescV1*,
                                 const Dlss5NrRenderParamsV11*, Dlss5NrFrameResultV1*, char*, int);
    check(impl.nr.function<Process>("dlss5nr_process_frame_v11")(&source, &destination, &parameters,
        &result, error.data(), int(error.size())), error);
    return output.image();
}
NativeEngines::SuperResolution::SuperResolution(QSize input, QSize output, const QString& mode,
                                                const QString& preset, const QString& flowQuality, bool still,
                                                bool wideOutput, quint32 colorTransfer, bool alpha)
    : output_(output),still_(still),wideOutput_(wideOutput) {
    auto& impl = *instance().impl_;
    std::lock_guard lock(impl.mutex);
    impl.sr.initialize(impl.ordinal);
    SessionDesc descriptor{
        sizeof(SessionDesc),
        4,
        uint32_t(input.width()),
        uint32_t(input.height()),
        uint32_t(output.width()),
        uint32_t(output.height()),
        uint32_t(
            QStringList{"DLAA", "Quality", "Balanced", "Performance", "Ultra Performance"}.indexOf(mode)),
        uint32_t(QStringList{"Default", "J", "K", "L", "M"}.indexOf(preset)),
        uint32_t(still),
        flowIndex(flowQuality), colorTransfer, alpha ? 1u : 0u};
    std::array<char, 4096> error{};
    using Create = void*(__cdecl*)(const SessionDesc*, char*, int);
    session_ =
        impl.sr.function<Create>("dlsssr_session_create")(&descriptor, error.data(), int(error.size()));
    check(session_ != nullptr, error);
}
NativeEngines::SuperResolution::~SuperResolution() {
    if (session_) {
        auto& impl = *instance().impl_;
        std::lock_guard lock(impl.mutex);
        impl.sr.function<void(__cdecl*)(void*)>("dlsssr_session_release")(session_);
    }
}
QImage NativeEngines::SuperResolution::process(const QImage& input, bool reset, quint32 phase, float frameTimeMs) {
    auto& impl = *instance().impl_;
    std::lock_guard lock(impl.mutex);
    const auto source = input.convertToFormat(input.format() == QImage::Format_RGBA8888
        ? QImage::Format_RGBA8888 : QImage::Format_RGBA64);
    const bool eightBit=still_ && !wideOutput_ && source.depth()<=32;
    QImage output(output_,eightBit ? QImage::Format_RGBA8888 : QImage::Format_RGBA64);
    FrameDesc frame{sizeof(FrameDesc),
                    4,
                    source.constBits(),
                    uint32_t(source.bytesPerLine()),
                    output.bits(),
                    uint32_t(output.bytesPerLine()),
                    uint32_t(reset),
                    phase,
                    source.format() == QImage::Format_RGBA8888 ? 1u : 2u,
                    eightBit ? 1u : 2u, frameTimeMs};
    FrameResult result{};
    result.size = sizeof(result);
    result.abi = 4;
    std::array<char, 4096> error{};
    using Process = int(__cdecl*)(void*, const FrameDesc*, FrameResult*, char*, int);
    check(impl.sr.function<Process>("dlsssr_process_rgba16f")(session_, &frame, &result, error.data(),
                                                              int(error.size())),
          error);
    output.setColorSpace(input.colorSpace());
    return output;
}
NativeEngines::RtxVideo::RtxVideo(QSize input, QSize output, const Settings& settings, bool hdr, bool video, bool wideInput)
    : output_(output), hdr_(hdr), packed_(video && wideInput), floatHdr_(hdr && settings.upscale_hdr_precision == "FP16"), video_(video) {
    auto& impl = *instance().impl_;
    std::lock_guard lock(impl.mutex);
    impl.rtx.initialize(impl.ordinal);
    RtxvSessionDescV1 descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.abi_version = 1;
    descriptor.input_width = uint32_t(input.width());
    descriptor.input_height = uint32_t(input.height());
    descriptor.output_width = uint32_t(output.width());
    descriptor.output_height = uint32_t(output.height());
    descriptor.input_format = packed_ ? RTXV_R10 : RTXV_RGBA8;
    descriptor.output_format = hdr ? (floatHdr_ ? RTXV_RGBA16F : RTXV_R10) : descriptor.input_format;
    descriptor.vsr_enabled = !hdr;
    descriptor.vsr_quality =
        uint32_t(video ? settings.upscale_vsr_quality : settings.upscale_image_vsr_quality);
    descriptor.hdr_enabled = hdr;
    descriptor.hdr_contrast = uint32_t(settings.upscale_hdr_contrast);
    descriptor.hdr_saturation = uint32_t(settings.upscale_hdr_saturation);
    descriptor.hdr_middle_gray = uint32_t(settings.upscale_hdr_middle_gray);
    descriptor.hdr_peak_luminance = uint32_t(settings.upscale_hdr_peak_luminance);
    descriptor.reserved[0] = video ? 0u : 1u;
    std::array<char, 4096> error{};
    using Create = void*(__cdecl*)(const RtxvSessionDescV1*, char*, int);
    session_ = impl.rtx.function<Create>("rtxv_session_create")(&descriptor, error.data(), int(error.size()));
    check(session_ != nullptr, error);
}
NativeEngines::RtxVideo::~RtxVideo() {
    if (session_) {
        auto& impl = *instance().impl_;
        std::lock_guard lock(impl.mutex);
        impl.rtx.function<void(__cdecl*)(void*)>("rtxv_session_release")(session_);
    }
}
QImage NativeEngines::RtxVideo::process(const QImage& input) {
    auto source = input.convertToFormat(QImage::Format_RGBA8888);
    QByteArray planar;
    RtxvFrameDescV1 from{};
    from.struct_size=sizeof(from); from.abi_version=1; from.memory_type=RTXV_HOST;
    from.width=uint32_t(input.width()); from.height=uint32_t(input.height());
    from.color_matrix=1; from.color_range=1;
    if (packed_ && video_) {
        planar=gbrp10Bytes(input);
        const auto planeBytes=qsizetype(input.width())*input.height()*2;
        from.pixel_format=RTXV_GBRP10;
        for (int plane=0; plane<3; ++plane) {
            from.planes[plane]=reinterpret_cast<uint64_t>(planar.constData()+plane*planeBytes);
            from.strides[plane]=uint32_t(input.width())*2;
        }
        return evaluate(from);
    }
    from.pixel_format=RTXV_RGBA8;
    from.planes[0]=reinterpret_cast<uint64_t>(source.constBits()); from.strides[0]=uint32_t(source.bytesPerLine());
    return evaluate(from);
}
QImage NativeEngines::RtxVideo::process(const CudaVideoFrame& input, const MediaInfo& colors) {
    if (!input.owner || input.memory!=FrameMemory::Cuda) throw std::runtime_error("RTX Video requires owned CUDA input planes.");
    RtxvFrameDescV1 from{};
    from.struct_size=sizeof(from); from.abi_version=1; from.memory_type=RTXV_CUDA;
    from.width=uint32_t(input.size.width()); from.height=uint32_t(input.size.height());
    from.pixel_format=input.format==VideoPixelFormat::P010 ? RTXV_P010 : RTXV_NV12;
    std::ranges::copy(input.planes,from.planes); std::ranges::copy(input.strides,from.strides);
    const auto fallback=colors.height==576 ? QString("bt470bg") : colors.height<576 ? QString("smpte170m") : QString("bt709");
    auto known=[&](const QString& value) { return value.isEmpty() || QStringList{"unknown","unspecified","reserved"}.contains(value) ? fallback : value; };
    const auto matrix=known(colors.colorSpace), primary=known(colors.primaries), transfer=colors.transfer;
    from.color_matrix=matrix.contains("2020") ? 2u : QStringList{"bt470bg","smpte170m","smpte240m","fcc"}.contains(matrix) ? 0u : 1u;
    from.color_range=uint32_t(colors.range=="pc");
    from.color_primaries=primary=="bt2020" ? 2u : QStringList{"bt470bg","bt470m"}.contains(primary) ? 0u
        : QStringList{"smpte170m","smpte240m","smpte431","smpte432"}.contains(primary) ? 3u : 1u;
    from.color_transfer=QStringList{"iec61966-2-1","srgb"}.contains(transfer) ? 1u
        : QStringList{"gamma22","bt470m"}.contains(transfer) ? 2u : QStringList{"gamma28","bt470bg"}.contains(transfer) ? 3u : transfer=="linear" ? 4u : 0u;
    // The reference's RTX adapter passes its zero-based chroma code; libav
    // stores the same locations starting at one. FI/NR use separate contracts.
    // RTX uses counter-clockwise degrees, unlike the NR/FI frame contracts.
    from.rotation=(360-input.rotation)%360; from.reserved[0]=input.chromaLocation ? input.chromaLocation-1 : 0;
    return evaluate(from);
}
QImage NativeEngines::RtxVideo::evaluate(const RtxvFrameDescV1& from) {
    auto& impl=*instance().impl_;
    std::lock_guard lock(impl.mutex);
    QImage output(output_, hdr_ && floatHdr_ ? QImage::Format_RGBA16FPx4 : QImage::Format_RGBA8888);
    auto descriptor = [](const QImage& image, int format) {
        RtxvFrameDescV1 frame{};
        frame.struct_size = sizeof(frame);
        frame.abi_version = 1;
        frame.memory_type = RTXV_HOST;
        frame.pixel_format = uint32_t(format);
        frame.width = uint32_t(image.width());
        frame.height = uint32_t(image.height());
        frame.planes[0] = reinterpret_cast<uint64_t>(image.constBits());
        frame.strides[0] = uint32_t(image.bytesPerLine());
        frame.color_matrix = 1;
        frame.color_range = 1;
        return frame;
    };
    auto to = descriptor(output, hdr_ ? (floatHdr_ ? RTXV_RGBA16F : RTXV_R10) : packed_ ? RTXV_R10 : RTXV_RGBA8);
    QByteArray planar;
    if (video_) {
        const auto planeBytes=qsizetype(output_.width())*output_.height()*2;
        planar=QByteArray(planeBytes*3,Qt::Uninitialized);
        to.pixel_format=RTXV_GBRP10;
        for (int plane=0; plane<3; ++plane) {
            to.planes[plane]=reinterpret_cast<uint64_t>(planar.data()+plane*planeBytes);
            to.strides[plane]=uint32_t(output_.width())*2;
        }
    }
    RtxvFrameResultV1 result{};
    result.struct_size = sizeof(result);
    result.abi_version = 1;
    std::array<char, 4096> error{};
    using Process =
        int(__cdecl*)(void*, const RtxvFrameDescV1*, const RtxvFrameDescV1*, RtxvFrameResultV1*, char*, int);
    check(impl.rtx.function<Process>("rtxv_process_frame_v1")(session_, &from, &to, &result, error.data(),
                                                              int(error.size())),
          error);
    if (video_) {
        return rgbaFromGbrp10(planar,output_);
    }
    if (to.pixel_format == RTXV_R10) {
        QImage precise(output_, QImage::Format_RGBA64);
        for (int y = 0; y < precise.height(); ++y) {
            auto* row = reinterpret_cast<QRgba64*>(precise.scanLine(y));
            const auto* data = reinterpret_cast<const quint32*>(output.constScanLine(y));
            for (int x = 0; x < precise.width(); ++x) {
                const auto value = data[x];
                auto channel = [&](int shift) {
                    return quint16((((value >> shift) & 1023) * 65535u + 511) / 1023);
                };
                row[x] =
                    QRgba64::fromRgba64(channel(0), channel(10), channel(20), quint16((value >> 30) * 21845));
            }
        }
        return precise;
    }
    return output;
}
struct NativeEngines::Interpolation::State {
    void* session = nullptr;
    ~State() {
        if (!session) return;
        auto& impl = *instance().impl_;
        std::lock_guard lock(impl.mutex);
        impl.fi.function<void(__cdecl*)(void*)>("fi_session_release")(session);
    }
};
struct NativeEngines::Interpolation::Surface::Storage {
    void* handle = nullptr;
    FrameDescriptorV1 descriptor{};
    std::shared_ptr<Interpolation::State> session;
    ~Storage() {
        if (!handle) return;
        auto& impl = *instance().impl_;
        std::lock_guard lock(impl.mutex);
        impl.fi.function<void(__cdecl*)(void*)>("fi_surface_release")(handle);
    }
};
NativeEngines::Interpolation::Interpolation(QSize size, int generated, bool hdr, const QString& flowQuality,
                                            bool highPrecision, int surfacePoolSize)
    : state_(std::make_shared<State>()), size_(size), hdr_(hdr), floating_(hdr) {
    Q_UNUSED(highPrecision);
    auto& impl = *instance().impl_;
    std::lock_guard lock(impl.mutex);
    impl.fi.initialize(impl.ordinal);
    using Abi = uint32_t(__cdecl*)();
    if (impl.fi.function<Abi>("fi_abi_version")() != 4)
        throw std::runtime_error("DLSS Frame Generation requires ABI 4.");
    SessionDescriptorV1 descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.abi_version = 4;
    descriptor.width = uint32_t(size.width());
    descriptor.height = uint32_t(size.height());
    descriptor.generated_count = uint32_t(generated);
    descriptor.hdr = uint32_t(floating_);
    descriptor.surface_pool_size = uint32_t(surfacePoolSize > 0 ? surfacePoolSize : generated > 1 ? 32 : 16);
    descriptor.optical_flow_quality = flowIndex(flowQuality);
    std::array<char, 4096> error{};
    using Create = void*(__cdecl*)(const SessionDescriptorV1*, char*, int);
    state_->session = impl.fi.function<Create>("fi_session_create")(&descriptor, error.data(), int(error.size()));
    check(state_->session != nullptr, error);
}
NativeEngines::Interpolation::~Interpolation() = default;
int NativeEngines::Interpolation::nativeMaximum() {
    auto& impl = *instance().impl_;
    std::lock_guard lock(impl.mutex);
    impl.fi.initialize(impl.ordinal);
    BridgeStatusV1 status{};
    status.struct_size = sizeof(status);
    status.abi_version = 4;
    using Status = int(__cdecl*)(BridgeStatusV1*);
    if (!impl.fi.function<Status>("fi_get_status_v1")(&status))
        return 2;
    return int(status.multi_frame_count_max) + 1;
}
std::vector<QImage> NativeEngines::Interpolation::process(const QImage& input, bool reset) {
    const auto result = evaluate(input, reset, !reset);
    std::vector<QImage> output;
    output.reserve(result.generated.size());
    for (const auto& surface : result.generated) output.push_back(surface.toHost());
    return output;
}
NativeEngines::Interpolation::Result NativeEngines::Interpolation::evaluate(const QImage& input, bool reset, bool detectCuts) {
    const auto source =
        input.convertToFormat(floating_ ? QImage::Format_RGBA16FPx4 : QImage::Format_RGBA8888);
    FrameDescriptorV1 frame{};
    frame.struct_size = sizeof(frame);
    frame.abi_version = 4;
    frame.memory_type = 1;
    frame.pixel_format = floating_ ? 2u : 1u;
    frame.width = uint32_t(source.width());
    frame.height = uint32_t(source.height());
    frame.planes[0] = reinterpret_cast<uint64_t>(source.constBits());
    frame.strides[0] = uint32_t(source.bytesPerLine());
    frame.color_matrix = hdr_ ? 2u : 1u;
    frame.color_range = 1;
    frame.color_primaries = hdr_ ? 9u : 1u;
    frame.color_transfer = hdr_ ? 16u : 1u;
    frame.reserved[0] = 1;
    frame.reserved[1] = 1;
    return copyCurrentInput(evaluateDescriptor(frame, reset, detectCuts), frame);
}
NativeEngines::Interpolation::Surface NativeEngines::Interpolation::own(void* handle) {
    if (!handle) throw std::runtime_error("DLSSG returned an empty surface.");
    auto storage = std::make_shared<Surface::Storage>();
    storage->session = state_; storage->handle = handle;
    storage->descriptor.struct_size = sizeof(FrameDescriptorV1); storage->descriptor.abi_version = 4;
    auto& impl = *instance().impl_;
    using Describe = int(__cdecl*)(void*, FrameDescriptorV1*);
    if (!impl.fi.function<Describe>("fi_surface_frame_desc")(handle, &storage->descriptor))
        throw std::runtime_error("DLSSG returned an invalid surface descriptor.");
    return Surface(std::move(storage));
}
NativeEngines::Interpolation::Result NativeEngines::Interpolation::evaluateDescriptor(
    const FrameDescriptorV1& frame, bool reset, bool detectCuts) {
    auto& impl = *instance().impl_;
    std::lock_guard lock(impl.mutex);
    FrameResultV1 result{};
    result.struct_size = sizeof(result);
    result.abi_version = 4;
    std::array<char, 4096> error{};
    using Process =
        int(__cdecl*)(void*, const FrameDescriptorV1*, uint32_t, uint32_t, FrameResultV1*, char*, int);
    check(impl.fi.function<Process>("fi_process_frame_v1")(state_->session, &frame, floating_ ? 2u : 1u,
                                                           uint32_t(reset) | (uint32_t(detectCuts) << 1), &result, error.data(),
                                                           int(error.size())),
          error);
    Result output;
    output.reset = result.reset != 0; output.sceneCut = result.scene_cut != 0; output.duplicate = result.duplicate != 0;
    if (result.generated_count > std::size(result.surface_handles))
        throw std::runtime_error("DLSSG returned too many generated surfaces.");
    output.generated.reserve(result.generated_count);
    using Release = void(__cdecl*)(void*);
    const auto release = impl.fi.function<Release>("fi_surface_release");
    uint32_t claimed = 0;
    try {
        for (; claimed < result.generated_count;) {
            auto* handle = reinterpret_cast<void*>(result.surface_handles[claimed++]);
            output.generated.push_back(own(handle));
        }
    } catch (...) {
        for (; claimed < result.generated_count; ++claimed) release(reinterpret_cast<void*>(result.surface_handles[claimed]));
        throw;
    }
    return output;
}
NativeEngines::Interpolation::Result NativeEngines::Interpolation::evaluate(
    const CudaVideoFrame& input, const MediaInfo& colors, bool reset, bool detectCuts) {
    FrameDescriptorV1 frame{};
    frame.struct_size = sizeof(frame); frame.abi_version = 4;
    frame.memory_type = uint32_t(input.memory); frame.pixel_format = input.format == VideoPixelFormat::P010 ? 5u : 4u;
    frame.width = uint32_t(input.size.width()); frame.height = uint32_t(input.size.height());
    std::ranges::copy(input.planes, frame.planes); std::ranges::copy(input.strides, frame.strides);
    frame.color_matrix = colors.colorSpace.contains("2020") ? 2u : colors.colorSpace == "bt709" ? 1u
        : colors.colorSpace == "fcc" ? 3u : colors.colorSpace == "smpte240m" ? 4u : colors.hdr ? 2u : 0u;
    frame.color_range = uint32_t(colors.range == "pc");
    frame.color_primaries = colors.primaries == "bt2020" ? 2u
        : QStringList{"bt470bg", "bt470m", "smpte170m"}.contains(colors.primaries) ? 0u : 1u;
    frame.color_transfer = colors.transfer == "iec61966-2-1" ? 1u : colors.transfer == "bt470m" ? 2u
        : colors.transfer == "bt470bg" ? 3u : colors.transfer == "linear" ? 4u : 0u;
    frame.rotation = input.rotation; frame.reserved[0] = 1; frame.reserved[1] = input.chromaLocation;
    return copyCurrentInput(evaluateDescriptor(frame, reset, detectCuts), frame);
}
NativeEngines::Interpolation::Result NativeEngines::Interpolation::copyCurrentInput(Result result, const FrameDescriptorV1& frame) {
    auto& impl = *instance().impl_;
    std::lock_guard lock(impl.mutex);
    std::array<char, 4096> error{};
    using Copy = void*(__cdecl*)(void*, char*, int);
    auto* handle = impl.fi.function<Copy>("fi_session_copy_input_rgb")(state_->session, error.data(), int(error.size()));
    check(handle != nullptr, error);
    result.source = own(handle);
    // Describing a surface only supplies its layout. Preserve interpretation separately.
    auto& descriptor = result.source.state_->descriptor;
    descriptor.color_matrix = frame.color_matrix; descriptor.color_range = frame.color_range;
    descriptor.color_primaries = frame.color_primaries; descriptor.color_transfer = frame.color_transfer;
    descriptor.reserved[0] = 1; descriptor.reserved[1] = frame.reserved[1];
    for (auto& surface : result.generated) {
        auto& generated = surface.state_->descriptor;
        generated.color_matrix = descriptor.color_matrix; generated.color_range = descriptor.color_range;
        generated.color_primaries = descriptor.color_primaries; generated.color_transfer = descriptor.color_transfer;
        generated.reserved[0] = 1; generated.reserved[1] = frame.reserved[1];
    }
    return result;
}
NativeEngines::Interpolation::Result NativeEngines::Interpolation::evaluate(
    const Surface& input, bool reset, bool detectCuts) {
    if (!input) throw std::runtime_error("Interpolation requires an owned input surface.");
    auto result = evaluateDescriptor(input.state_->descriptor, reset, detectCuts);
    result.source = input;
    for (auto& surface : result.generated) {
        auto& descriptor = surface.state_->descriptor;
        const auto& from = input.state_->descriptor;
        descriptor.color_matrix = from.color_matrix; descriptor.color_range = from.color_range;
        descriptor.color_primaries = from.color_primaries; descriptor.color_transfer = from.color_transfer;
        descriptor.reserved[0] = 1; descriptor.reserved[1] = from.reserved[1];
    }
    return result;
}
QImage NativeEngines::Interpolation::Surface::toHost() const {
    if (!state_) throw std::runtime_error("Cannot download a released DLSSG surface.");
    auto& impl = *instance().impl_;
    std::lock_guard lock(impl.mutex);
    const auto& descriptor = state_->descriptor;
    QImage pixels(int(descriptor.width), int(descriptor.height), descriptor.pixel_format == 2
        ? QImage::Format_RGBA16FPx4 : QImage::Format_RGBA8888);
    std::array<char, 4096> error{};
    using Copy = int(__cdecl*)(void*, void*, uint32_t, char*, int);
    check(impl.fi.function<Copy>("fi_surface_copy_rgb_to_host")(state_->handle, pixels.bits(),
        uint32_t(pixels.bytesPerLine()), error.data(), int(error.size())), error);
    return pixels;
}
QImage NativeEngines::Interpolation::Surface::toHostRgb10() const {
    if (!state_ || state_->descriptor.pixel_format != 2)
        throw std::runtime_error("RGB10 packing requires an owned HDR interpolation surface.");
    auto& impl = *instance().impl_;
    std::lock_guard lock(impl.mutex);
    const QSize size(int(state_->descriptor.width), int(state_->descriptor.height));
    const uint32_t stride=uint32_t(size.width())*2;
    const auto planeBytes=qsizetype(stride)*size.height();
    QByteArray packed(planeBytes*3, Qt::Uninitialized);
    std::array<char,4096> error{};
    using Copy=int(__cdecl*)(void*,void*,uint32_t,void*,uint32_t,void*,uint32_t,char*,int);
    check(impl.fi.function<Copy>("fi_surface_copy_gbrp10_to_host")(state_->handle,
        packed.data(), stride, packed.data()+planeBytes, stride, packed.data()+planeBytes*2, stride,
        error.data(), int(error.size())), error);
    return rgbaFromGbrp10(packed, size);
}
CudaVideoFrame NativeEngines::Interpolation::Surface::toVideo(CudaFramePool& pool, bool p010,
    uint32_t matrix, uint32_t range) const {
    if (!state_) throw std::runtime_error("Cannot encode a released DLSSG surface.");
    auto& impl = *instance().impl_;
    std::lock_guard lock(impl.mutex);
    std::array<char, 4096> error{};
    using Convert = void*(__cdecl*)(void*, uint32_t, uint32_t, uint32_t, char*, int);
    auto storage = std::make_shared<Storage>(); storage->session = state_->session;
    storage->handle = impl.fi.function<Convert>("fi_surface_convert_video")(state_->handle, p010 ? 5u : 4u,
        matrix, range, error.data(), int(error.size()));
    check(storage->handle != nullptr, error);
    storage->descriptor.struct_size = sizeof(FrameDescriptorV1); storage->descriptor.abi_version = 4;
    using Describe = int(__cdecl*)(void*, FrameDescriptorV1*);
    if (!impl.fi.function<Describe>("fi_surface_frame_desc")(storage->handle, &storage->descriptor))
        throw std::runtime_error("DLSSG conversion returned an invalid surface.");
    const auto& frame = storage->descriptor;
    return pool.wrap(storage, {frame.planes[0], frame.planes[1], frame.planes[2]},
        {frame.strides[0], frame.strides[1], frame.strides[2]}, matrix, range);
}
} // namespace ve
