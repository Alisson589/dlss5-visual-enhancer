#include "video_preview.hpp"
#include "video_backend.hpp"
#include "host_video.hpp"
#include "configuration/settings.hpp"
#include "infrastructure/paths.hpp"
#include "infrastructure/process.hpp"
#include "processing/effects.hpp"
#include "processing/preview_cache.hpp"
#include <QDir>
#include "infrastructure/scratch.hpp"
#include <QTransform>
#include <QThread>
#include <cmath>
#include <deque>

namespace ve {
namespace {
struct PreviewSeek {
    QString fast, accurate;
    explicit PreviewSeek(double seconds) {
        const auto aligned = std::isfinite(seconds) ? std::max(0., seconds - .002) : 0.;
        const auto first = std::max(0., aligned - 2.);
        fast = QString::number(first, 'f', 6);
        accurate = QString::number(aligned - first, 'f', 6);
    }
};
QByteArray decode(const QStringList& arguments, JobControl& control) {
    control.checkpoint();
    auto result = runProcess(Paths::instance().ffmpeg(), arguments, &control, 0);
    if (result.exitCode) throw std::runtime_error(result.error.toStdString());
    control.checkpoint();
    return std::move(result.output);
}
} // namespace
struct VideoFrameReader::Impl {
    PreviewFrameCache frames{192*1024};
    std::unique_ptr<HostVideoDecoder> decoder;
    QByteArray identity;
    qint64 cursor = 0;
};
VideoFrameReader::VideoFrameReader() : impl_(std::make_unique<Impl>()) {}
VideoFrameReader::~VideoFrameReader() = default;
void VideoFrameReader::clear() {
    impl_->decoder.reset();
    impl_->identity.clear();
    impl_->frames.clear();
}
qint64 VideoFrameReader::allocatedBytes() const { return impl_->frames.allocatedBytes(); }
VideoFrameReader::Result VideoFrameReader::frame(const QString &path, const MediaInfo &info,
                                                 FramePosition position, JobControl &control, bool timeline,
                                                 int ordinal, bool cuda) {
    control.checkpoint();
    auto &s = *impl_;
    const auto source = QByteArray("video-source/v2") + previewFileIdentity(path) +
                        QJsonDocument(QJsonObject{{"timeline", timeline},
                                                  {"ordinal", ordinal},
                                                  {"cuda", cuda},
                                                  {"bits", info.precisionBits()},
                                                  {"rotation", info.clockwiseRotation()}})
                            .toJson(QJsonDocument::Compact);
    const auto identity =
        source + QJsonDocument(QJsonObject{{"frame", qint64(position.number)}, {"seconds", position.seconds}})
                     .toJson(QJsonDocument::Compact);
    const auto key = QCryptographicHash::hash(identity, QCryptographicHash::Sha256);
    if (auto cached = s.frames.find(key))
        return {std::move(cached->pixels), identity, true, cached->mediaTimestamp};
    if (timeline && info.precisionBits()>8) {
        // FFmpeg's wide YUV reconstruction has a distinct rounding boundary.
        // Cache its exact result rather than substituting host swscale pixels.
        s.decoder.reset(); s.identity.clear();
        auto pixels=decodeTimelineFrame(path,info,position.seconds,control);
        control.checkpoint(); s.frames.store(key,{pixels,info.hdr,false});
        return {std::move(pixels),identity,false};
    }
    try {
        const auto start = std::max(0., position.seconds - (timeline ? 0. : .002));
        if (!s.decoder || source != s.identity) {
            s.decoder = std::make_unique<HostVideoDecoder>(path, info, ordinal, start, cuda, control,
                                                           HostDecodePrecision::Source, timeline,
                                                           std::clamp(QThread::idealThreadCount(), 1, 16));
            s.identity = source;
        } else if (!info.variableFrameRate && position.number == s.cursor + 1) {
            s.decoder->bindControl(control);
        } else
            s.decoder->seek(start, control);
        auto decoded = s.decoder->read();
        if (!decoded)
            throw std::runtime_error("Selected video frame is not decodable.");
        s.cursor = position.number;
        s.decoder->detachControl();
        control.checkpoint();
        const double absoluteSeconds=decoded->seconds()+s.decoder->timeOrigin();
        s.frames.store(key, {decoded->image, info.hdr, false, absoluteSeconds});
        return {std::move(decoded->image), identity, false, absoluteSeconds};
    } catch (...) {
        // A cancelled demux/decoder may have buffered a partial packet. Retire
        // it while keeping the already validated immutable frame cache.
        s.decoder.reset();
        s.identity.clear();
        throw;
    }
}
std::vector<TimedImage> VideoFrameReader::temporalContext(const QString& path, const MediaInfo& info,
    FramePosition position, double absoluteSeconds, int count, JobControl& control, int ordinal, bool cuda, bool includeFollowing) {
    const double fps=std::isfinite(info.fps)&&info.fps>0?info.fps:30.;
    const auto bytes=qint64(info.width)*info.height*(info.precisionBits()>8?8:4);
    count=int(std::clamp<qint64>(count,0,std::max<qint64>(1,384ll*1024*1024/std::max<qint64>(1,bytes)-1)));
    const double start=std::max(0.,position.seconds-(count+2)/fps);
    HostVideoDecoder decoder(path,info,ordinal,start,cuda,control,HostDecodePrecision::Source,false,
                             std::clamp(QThread::idealThreadCount(),1,16));
    std::deque<TimedImage> frames;
    while(auto frame=decoder.read()) {
        control.checkpoint();
        const double seconds=frame->seconds()+decoder.timeOrigin();
        frames.push_back(std::move(*frame));
        if(int(frames.size())>count+1)frames.pop_front();
        if(seconds>=absoluteSeconds-1e-6) {
            if(std::abs(seconds-absoluteSeconds)>1e-5)
                throw std::runtime_error("Native SR preview could not match the selected frame timestamp.");
            if(includeFollowing){
                if(auto following=decoder.read())frames.push_back(std::move(*following));
                const double origin=decoder.timeOrigin();
                for(auto& selected:frames)selected.pts+=std::llround(origin*selected.timeBase.denominator/selected.timeBase.numerator);
            }
            return {std::make_move_iterator(frames.begin()),std::make_move_iterator(frames.end())};
        }
    }
    throw std::runtime_error("Native SR preview history ended before the selected frame.");
}
void VideoFrameReader::prefetch(const QString &path, const MediaInfo &info, FramePosition position,
                                JobControl &control, bool timeline, int ordinal, bool cuda) {
    if (info.variableFrameRate || info.fps <= 0 || (timeline && info.precisionBits()>8))
        return;
    for (int ahead = 1; ahead <= 2; ++ahead) {
        control.checkpoint();
        const FramePosition next{double(position.number + ahead - 1) / info.fps, position.number + ahead};
        if (info.duration > 0 && next.seconds >= info.duration - .002)
            break;
        (void)frame(path, info, next, control, timeline, ordinal, cuda);
    }
}
QImage decodeTimelineFrame(const QString &path, const MediaInfo &source, double seconds, JobControl &control,
                           TimelinePrecision precision) {
    const PreviewSeek seek(seconds);
    const bool wide = precision == TimelinePrecision::Unorm16 || source.precisionBits() > 8;
    const auto pixels = decode(
        {"-hide_banner", "-loglevel",   "error", "-xerror",  "-noautorotate", "-ss",
         seek.fast,      "-i",          path,    "-ss",      seek.accurate,   "-map",
         "0:v:0",        "-frames:v",   "1",     "-an",      "-sn",           "-dn",
         "-fps_mode",    "passthrough", "-f",    "rawvideo", "-pix_fmt",      wide ? "rgba64le" : "rgba",
         "pipe:1"},
        control);
    const auto codedWidth = source.metadata.value("width").toInt(source.width);
    const auto codedHeight = source.metadata.value("height").toInt(source.height);
    if (pixels.size() != qint64(codedWidth) * codedHeight * 4 * (wide ? 2 : 1))
        throw std::runtime_error("Timeline frame decoding returned unexpected dimensions.");
    auto image = fromRgbaBytes(pixels, codedWidth, codedHeight, wide);
    if (const auto rotation = source.clockwiseRotation())
        image = image.transformed(QTransform().rotate(rotation), Qt::FastTransformation);
    return image;
}
QImage decodeVideoPoster(const QString& path, double seconds, JobControl& control, int width) {
    const PreviewSeek seek(seconds);
    QStringList arguments{"-hide_banner", "-loglevel", "error", "-nostdin", "-filter_threads", "1",
        "-threads", "2", "-hwaccel", "none", "-ss", seek.fast, "-i", path, "-ss", seek.accurate,
        "-map", "0:v:0", "-frames:v", "1", "-an", "-sn", "-dn", "-threads", "1"};
    if (width > 0) arguments << "-vf" << "scale=" + QString::number(width) + ":-2";
    arguments << "-q:v" << "3" << "-f" << "mjpeg" << "pipe:1";
    auto image = QImage::fromData(decode(arguments, control), "JPEG");
    if (image.isNull()) throw std::runtime_error("Video poster decoding produced no image.");
    return image;
}
QImage decodeProcessedVideoPreview(const QString& path, const MediaInfo& source, JobControl& control,
                                  const QString& selection) {
    QStringList arguments{"-hide_banner", "-loglevel", "error"};
    if (source.hdr) {
        auto plan = planVideoDecoder(selection, path, control, "rgba64le", source.clockwiseRotation());
        const auto tone = libplacebo("color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=tv:"
                                    "tonemapping=mobius:format=yuv420p");
        const auto packing = libplacebo("format=rgba64le:range=pc:colorspace=0");
        plan.filter.replace(packing, tone + ',' + packing);
        arguments << plan.inputOptions << "-i" << path << "-vf" << plan.filter;
    } else arguments << "-i" << path;
    arguments << "-map" << "0:v:0" << "-frames:v" << "1" << "-an" << "-f" << "rawvideo"
              << "-pix_fmt" << "rgba64le" << "pipe:1";
    const auto bytes = decode(arguments, control);
    if (bytes.size() != qint64(source.width) * source.height * 8)
        throw std::runtime_error("Processed preview decoding returned unexpected dimensions.");
    return fromRgbaBytes(bytes, source.width, source.height);
}
QImage toneMapVideoPreview(const QImage& pixels, const MediaInfo& colors, JobControl& control, const QString& selection) {
    ScratchDirectory directory(QStringLiteral("hdr-preview-XXXXXX"));
    if (!directory.isValid()) throw std::runtime_error("Cannot create the HDR preview directory.");
    const auto path = directory.filePath("frame.mkv");
    auto output = colors;
    output.width = pixels.width(); output.height = pixels.height();
    output.pixelFormat = "rgba64le"; output.fps = 25;
    Settings settings;
    {
        HostVideoEncoder encoder(path, "FFV1", settings, output, 0, control, HostEncodePurpose::NativePreview);
        encoder.write(pixels, 0, 1, {1,25});
        encoder.finish();
    }
    return decodeProcessedVideoPreview(path, probeMedia(path, &control), control, selection);
}
QString extractVideoPreviewSource(const QString& path, const MediaInfo& source, const QString& directory,
                                 double seconds, double length, bool singleFrame, JobControl& control,
                                 const QString& selection) {
    const auto output = QDir(directory).filePath("source-preview.mkv");
    // Native host decoding retains every source channel before FFV1 packing.
    // CUDA is attempted for Automatic; unsupported sources use software decode.
    HostVideoDecoder decoder(path, source, 0, std::max(0., seconds-.002), selection == "auto", control);
    auto pixels = source;
    pixels.pixelFormat = source.precisionBits() > 8 ? "rgba64le" : "rgba";
    Settings settings;
    HostVideoEncoder encoder(output, "FFV1", settings, pixels, 0, control, HostEncodePurpose::NativePreview);
    qint64 written = 0;
    while (auto frame = decoder.read()) {
        control.checkpoint();
        if (length > 0 && frame->seconds() >= length - 1e-9) break;
        encoder.write(frame->image, frame->pts, frame->duration, frame->timeBase);
        ++written;
        if (singleFrame) break;
    }
    if (!written) throw std::runtime_error("Preview source contains no decodable video frames.");
    encoder.finish();
    return output;
}
} // namespace ve
