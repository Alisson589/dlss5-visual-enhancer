#include "video_backend.hpp"
#include "infrastructure/log.hpp"
#include "infrastructure/paths.hpp"
#include "infrastructure/process.hpp"
#include "infrastructure/vulkan_device.hpp"
#include "processing/effects.hpp"
#include "processing/native_engines.hpp"
#include "configuration/settings.hpp"
#include <QCache>
#include <QDateTime>
#include <QFileInfo>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QTemporaryDir>

namespace ve {
QString nativeDeliveryCodec(const Settings& settings) {
    auto codec = settings.codec;
    if (settings.ffmpeg_device == "cpu" || !QStringList{"H.264", "H.265", "AV1"}.contains(codec))
        return codec;
    const auto devices = detectGpus();
    const auto ordinal = gpuOrdinal(settings.ai_gpu_uuid);
    const auto found = std::ranges::find_if(devices, [ordinal](const Gpu& gpu) { return gpu.index == ordinal; });
    if (found != devices.end() && (settings.video_gpu_uuid == "auto" || settings.video_gpu_uuid == found->uuid) &&
        (settings.ffmpeg_device == "auto" ||
         settings.ffmpeg_device.mid(7).compare(found->uuid.mid(4), Qt::CaseInsensitive) == 0))
        codec += " (NVIDIA NVENC)";
    return codec;
}
namespace {
qint64 allocatedBytes(const QString& text) {
    return qint64(text.capacity()+1)*sizeof(QChar);
}
qint64 allocatedBytes(const QStringList& values) {
    qint64 bytes=qint64(values.capacity())*sizeof(QString);
    for (const auto& value : values) bytes+=allocatedBytes(value);
    return bytes;
}
int decoderPlanCost(const QString& key,const VideoDecoderPlan& plan) {
    // Charge retained string capacity and the cache node, not the serialized
    // length. Unique intermediate filenames must not accumulate forever.
    return int(allocatedBytes(key)+allocatedBytes(plan.inputOptions)+
               allocatedBytes(plan.filter)+sizeof(VideoDecoderPlan)+128);
}
QString option(const QStringList& options, const QString& name, const QString& fallback = {}) {
    const auto index = options.indexOf(name);
    return index < 0 || index + 1 >= options.size() ? fallback : options[index + 1];
}
QStringList removeOptions(const QStringList& options, const QSet<QString>& names) {
    QStringList result;
    for (qsizetype index = 0; index < options.size(); ++index) {
        if (names.contains(options[index])) {
            ++index;
            continue;
        }
        result.append(options[index]);
    }
    return result;
}
QString colors(const QString& primaries, const QString& transfer, const QString& matrix,
               const QString& range) {
    return ":color_primaries=" + primaries + ":color_trc=" + transfer + ":colorspace=" + matrix +
           ":range=" + range;
}
bool probeEncoder(const VulkanDevice& device, const VideoEncoderPlan& plan, QSize size, double fps,
                  JobControl& control) {
    static QMutex mutex;
    static QCache<QString, bool> supported(256*1024);
    const auto key = device.selection() + '|' + plan.options.join(' ') + '|' + plan.filter + '|' +
                     QString::number(size.width()) + 'x' + QString::number(size.height()) + '|' +
                     QString::number(fps, 'g', 12);
    QMutexLocker lock(&mutex);
    if (const auto* available=supported.object(key))
        return *available;
    QTemporaryDir directory(Paths::instance().at("data/temp/app/video-probe-XXXXXX"));
    if (!directory.isValid())
        throw std::runtime_error("Cannot create a video capability probe.");
    const auto sample = directory.filePath("sample.mkv");
    auto command = QStringList{"-v", "error", "-y"} + plan.deviceOptions +
                   QStringList{"-f",
                               "lavfi",
                               "-i",
                               "testsrc2=s=" + QString::number(size.width()) + 'x' +
                                   QString::number(size.height()) + ":r=" + QString::number(fps, 'g', 12),
                               "-vf",
                               "format=rgba64le," + plan.filter,
                               "-frames:v",
                               "3"} +
                   plan.options + QStringList{sample};
    const auto encoded = runProcess(Paths::instance().ffmpeg(), command, &control, 30000);
    bool available = encoded.exitCode == 0;
    if (available) {
        const auto decoded =
            runProcess(Paths::instance().ffmpeg(),
                       {"-v", "error", "-xerror", "-err_detect", "explode", "-i", sample, "-map", "0:v:0",
                        "-an", "-fps_mode", "passthrough", "-f", "framemd5", "-"},
                       &control, 30000);
        int count = 0;
        for (const auto& line : decoded.output.split('\n'))
            if (!line.trimmed().isEmpty() && !line.startsWith('#'))
                ++count;
        available = decoded.exitCode == 0 && count == 3;
    }
    supported.insert(key, new bool(available),int(allocatedBytes(key)+sizeof(bool)+128));
    return available;
}
} // namespace
VideoEncoderPlan planVideoEncoder(const QString& selection, const QStringList& softwareOptions, QSize size,
                                  double fps, const QString& primaries, const QString& transfer,
                                  const QString& matrix, const QString& range, JobControl& control) {
    const auto software = option(softwareOptions, "-c:v"), pixelFormat = option(softwareOptions, "-pix_fmt");
    const bool rgb = pixelFormat.startsWith("gbr");
    const auto tags =
        "setparams=colorspace=gbr:range=full:color_primaries=" + primaries + ":color_trc=" + transfer;
    VideoEncoderPlan fallback{
        softwareOptions,
        {},
        tags + (rgb ? ",format=gbrp10le,setparams=colorspace=gbr:range=full"
                    : ",scale=in_range=pc:out_range=" + range + ":out_color_matrix=" + matrix),
        software};
    const QMap<QString, QString> encoders{{"libx264", "h264_vulkan"},
                                          {"libx265", "hevc_vulkan"},
                                          {"libsvtav1", "av1_vulkan"},
                                          {"ffv1", "ffv1_vulkan"},
                                          {"prores_ks", "prores_ks_vulkan"}};
    if (!encoders.contains(software) || selection == "cpu")
        return fallback;
    const auto encoder = encoders.value(software);
    const bool compressed = encoder == "h264_vulkan" || encoder == "hevc_vulkan" || encoder == "av1_vulkan";
    // Vulkan's CQP fallback cannot preserve a CRF/CQ target. Keep the selected
    // quality-capable encoder instead of mapping every tier to fixed QP 18.
    if (compressed && ((!option(softwareOptions,"-crf").isEmpty() &&
                        option(softwareOptions,"-crf") != "0") ||
                       !option(softwareOptions,"-cq").isEmpty()))
        return fallback;
    const auto hardwareFormat =
        compressed ? (pixelFormat.contains("10") ? QString("p010le") : QString("nv12")) : pixelFormat;
    auto options =
        removeOptions(softwareOptions, {"-pix_fmt", "-preset", "-tune", "-crf", "-cq", "-rc", "-gpu",
                                        "-x264-params", "-x265-params", "-svtav1-params", "-cpu-used"});
    options[options.indexOf("-c:v") + 1] = encoder;
    if (compressed && option(softwareOptions, "-crf") == "0") {
        options = removeOptions(options, {"-b:v"});
        options << "-tune" << "lossless" << "-rc_mode" << "cqp" << "-qp" << "0";
    } else if (compressed && (option(options, "-b:v").isEmpty() || option(options, "-b:v") == "0")) {
        options = removeOptions(options, {"-b:v"});
        options << "-rc_mode" << "cqp" << "-qp" << "18";
    }
    options << "-color_primaries" << primaries << "-color_trc" << transfer << "-colorspace"
            << (rgb ? "0" : matrix) << "-color_range" << range;
    for (const auto& device : vulkanCandidates(selection)) {
        if (compressed) {
            const auto extension = "VK_KHR_video_encode_" + (encoder == "h264_vulkan"   ? QString("h264")
                                                             : encoder == "hevc_vulkan" ? QString("h265")
                                                                                        : QString("av1"));
            if (!device.extensions.contains(extension))
                continue;
        }
        VideoEncoderPlan plan{options, vulkanArguments(device),
                              tags + ",hwupload," +
                                  libplacebo("format=" + hardwareFormat +
                                             colors(primaries, transfer, rgb ? "gbr" : matrix, range)),
                              encoder};
        if (probeEncoder(device, plan, size, fps, control)) {
            info("video", "Encoder: " + encoder + " on " + device.name);
            return plan;
        }
    }
    info("video", "Encoder: " + software);
    return fallback;
}
VideoDecoderPlan planVideoDecoder(const QString& selection, const QString& source, JobControl& control,
                                 const QString& pixelFormat, int clockwiseRotation) {
    QString orientation;
    if (clockwiseRotation) {
        if (clockwiseRotation % 90) throw std::runtime_error("Unsupported video display rotation.");
        orientation = ":rotate=" + QString::number(clockwiseRotation / 90);
    }
    const auto packing = "format="+pixelFormat+":range=pc:colorspace=0";
    // Reconstruct RGB before rotating. Rotating subsampled YUV in that same
    // evaluation changes chroma sample locations, unlike a display transform.
    const auto conversion=libplacebo(packing)+(orientation.isEmpty() ? QString{} : ','+libplacebo(packing+orientation))+
        ",hwdownload,format="+pixelFormat+(orientation.isEmpty() ? QString{} : ",setsar=1");
    // libplacebo also reads AVFrame display matrices. Clear that metadata at
    // input so consecutive filters cannot apply the same orientation twice.
    const QStringList displayOptions{"-noautorotate", "-display_rotation:v:0", "0"};
    const auto software=VideoDecoderPlan{vulkanFilterArguments(selection)+displayOptions, "hwupload,"+conversion};
    if (selection == "cpu") return software;
    static QMutex mutex;
    static QCache<QString, VideoDecoderPlan> cached(512*1024);
    const QFileInfo file(source);
    const auto key = selection + '|' + pixelFormat + '|' + QString::number(clockwiseRotation) + '|' + file.absoluteFilePath() + '|' + QString::number(file.size()) + '|' +
                     QString::number(file.lastModified().toMSecsSinceEpoch());
    QMutexLocker lock(&mutex);
    if (const auto* plan=cached.object(key))
        return *plan;
    for (const auto& device : vulkanCandidates(selection)) {
        auto input =
            vulkanArguments(device) + displayOptions +
            QStringList{"-hwaccel", "vulkan", "-hwaccel_device", "ve_vk", "-hwaccel_output_format", "vulkan",
                        "-threads", "2"};
        const QString filter = conversion;
        const auto probe = runProcess(Paths::instance().ffmpeg(),
                                      QStringList{"-v", "error"} + input +
                                          QStringList{"-i", source, "-map", "0:v:0", "-an", "-vf", filter,
                                                      "-frames:v", "1", "-f", "null", "-"},
                                      &control, 30000);
        if (probe.exitCode == 0) {
            VideoDecoderPlan plan{input, filter};
            cached.insert(key, new VideoDecoderPlan(plan), decoderPlanCost(key,plan));
            return plan;
        }
    }
    cached.insert(key, new VideoDecoderPlan(software), decoderPlanCost(key,software));
    return software;
}
} // namespace ve
