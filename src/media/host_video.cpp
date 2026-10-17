#include "host_video.hpp"
#include "resolution.hpp"
#include "av_api.hpp"
#include "cuda_video.hpp"
#include "native_frame.hpp"
#include "configuration/settings.hpp"
#include "infrastructure/log.hpp"
#include <QTransform>
#include <map>
#include <QColorSpace>
#include <QJsonArray>
#include <cstring>

namespace ve {
namespace {
AVPixelFormat selectCuda(AVCodecContext*, const AVPixelFormat* formats) {
    for (auto* current = formats; *current != AV_PIX_FMT_NONE; ++current)
        if (*current == AV_PIX_FMT_CUDA) return *current;
    return *formats; // Software fallback uses the first offered layout.
}
QString encoderName(const QString& codec) {
    if (codec.startsWith("H.264")) return codec.contains("NVENC") ? "h264_nvenc" : "libx264";
    if (codec.startsWith("H.265")) return codec.contains("NVENC") ? "hevc_nvenc" : "libx265";
    if (codec.startsWith("AV1")) return codec.contains("NVENC") ? "av1_nvenc" : "libsvtav1";
    return codec.startsWith("FFV1") ? "ffv1" : "prores_ks";
}
int depth(const MediaInfo& info) {
    auto working = info;
    working.sourceBitDepth = 0;
    return working.precisionBits() > 8 ? 16 : 8;
}
void retainHdrMetadata(AVCodecParameters* parameters, const MediaInfo& info) {
    if (!info.hdr) return;
    auto& api = av::Api::get();
    const auto rational = [](const QJsonValue& value) {
        const auto parts = value.toVariant().toString().split('/');
        if (parts.size() == 2 && parts[1].toInt() > 0) return AVRational{parts[0].toInt(), parts[1].toInt()};
        return av::Api::get().av_d2q(value.toVariant().toDouble(), 1000000);
    };
    for (const auto& value : info.metadata.value("side_data_list").toArray()) {
        const auto data = value.toObject();
        const auto type = data.value("side_data_type").toString();
        if (type == "Mastering display metadata") {
            auto* side = api.av_packet_side_data_new(&parameters->coded_side_data, &parameters->nb_coded_side_data,
                AV_PKT_DATA_MASTERING_DISPLAY_METADATA, sizeof(AVMasteringDisplayMetadata), 0);
            if (!side) throw std::bad_alloc{};
            auto* mastering = reinterpret_cast<AVMasteringDisplayMetadata*>(side->data);
            *mastering = {};
            const QStringList colors{"red", "green", "blue"};
            for (int i = 0; i < 3; ++i) {
                mastering->display_primaries[i][0] = rational(data.value(colors[i] + "_x"));
                mastering->display_primaries[i][1] = rational(data.value(colors[i] + "_y"));
            }
            mastering->white_point[0] = rational(data.value("white_point_x"));
            mastering->white_point[1] = rational(data.value("white_point_y"));
            mastering->min_luminance = rational(data.value("min_luminance"));
            mastering->max_luminance = rational(data.value("max_luminance"));
            mastering->has_primaries = data.contains("red_x") && data.contains("white_point_x");
            mastering->has_luminance = data.contains("min_luminance") && data.contains("max_luminance");
        } else if (type == "Content light level metadata") {
            auto* side = api.av_packet_side_data_new(&parameters->coded_side_data, &parameters->nb_coded_side_data,
                AV_PKT_DATA_CONTENT_LIGHT_LEVEL, sizeof(AVContentLightMetadata), 0);
            if (!side) throw std::bad_alloc{};
            auto* light = reinterpret_cast<AVContentLightMetadata*>(side->data);
            *light = {};
            light->MaxCLL = data.value("max_content").toVariant().toUInt();
            light->MaxFALL = data.value("max_average").toVariant().toUInt();
        }
    }
}
} // namespace
void retainStaticHdrMetadata(AVCodecParameters* parameters,const MediaInfo& info) {
    retainHdrMetadata(parameters,info);
}
void copyStaticHdrMetadata(AVFrame* frame,const AVCodecParameters* parameters) {
    auto& api=av::Api::get();
    for(int index=0;index<parameters->nb_coded_side_data;++index){
        const auto& side=parameters->coded_side_data[index];
        if(side.type!=AV_PKT_DATA_MASTERING_DISPLAY_METADATA&&side.type!=AV_PKT_DATA_CONTENT_LIGHT_LEVEL)continue;
        const auto type=side.type==AV_PKT_DATA_MASTERING_DISPLAY_METADATA?AV_FRAME_DATA_MASTERING_DISPLAY_METADATA:AV_FRAME_DATA_CONTENT_LIGHT_LEVEL;
        if(api.av_frame_get_side_data(frame,type))continue;
        auto* retained=api.av_frame_new_side_data(frame,type,side.size);
        if(!retained)throw std::bad_alloc{};
        std::memcpy(retained->data,side.data,side.size);
    }
}
QString ffv1PreviewPixelFormat(const MediaInfo& info) {
    const auto bits = std::max(info.precisionBits(), info.hdr ? 10 : 8);
    if (bits == 8) return "bgr0";
    for (int supported : {9, 10, 12, 14, 16})
        if (bits <= supported) return "gbrp" + QString::number(supported) + "le";
    throw std::runtime_error("The video precision exceeds the integer FFV1 preview range.");
}
struct HostRgb10Boundary::Impl {
    av::Api& api = av::Api::get();
    SwsContext* scaler = api.sws_alloc_context();
    av::Frame packed = av::frame();
    av::Frame decoded = av::frame();
    Impl() {
        if (!scaler) throw std::bad_alloc{};
        scaler->flags = SWS_BILINEAR; scaler->threads = 0;
    }
    ~Impl() { api.sws_free_context(&scaler); }
};
HostRgb10Boundary::HostRgb10Boundary() : impl_(std::make_unique<Impl>()) {}
HostRgb10Boundary::~HostRgb10Boundary() = default;
QImage HostRgb10Boundary::convert(const QImage& pixels, JobControl& control) {
    control.checkpoint();
    auto& s = *impl_;
    const auto image = pixels.convertToFormat(QImage::Format_RGBA64);
    if (s.packed->width != image.width() || s.packed->height != image.height()) {
        s.packed = av::frame();
        s.packed->width=image.width(); s.packed->height=image.height(); s.packed->format=AV_PIX_FMT_GBRP10LE;
        av::check(s.api.av_frame_get_buffer(s.packed.get(), 32), "Allocate RGB10 stage boundary");
    }
    av::check(s.api.av_frame_make_writable(s.packed.get()), "Reuse RGB10 stage boundary");
    auto input=av::frame();
    input->width=image.width(); input->height=image.height(); input->format=AV_PIX_FMT_RGBA64LE;
    input->data[0]=const_cast<uchar*>(image.constBits()); input->linesize[0]=image.bytesPerLine();
    input->colorspace=AVCOL_SPC_BT709; input->color_range=AVCOL_RANGE_JPEG;
    s.packed->colorspace=AVCOL_SPC_BT709; s.packed->color_range=AVCOL_RANGE_JPEG;
    av::check(s.api.sws_scale_frame(s.scaler, s.packed.get(), input.get()), "Pack RGB10 stage pixels");
    QByteArray bytes(qsizetype(image.width())*image.height()*6, Qt::Uninitialized);
    const auto rowBytes=qsizetype(image.width())*2, planeBytes=rowBytes*image.height();
    for (int plane=0; plane<3; ++plane) for (int y=0; y<image.height(); ++y) {
        control.checkpoint();
        std::memcpy(bytes.data()+plane*planeBytes+y*rowBytes, s.packed->data[plane]+y*s.packed->linesize[plane], size_t(rowBytes));
    }
    return rgbaFromGbrp10(bytes, image.size());
}
QImage HostRgb10Boundary::decodeRgba8(const QImage& pixels, JobControl& control) {
    control.checkpoint();
    auto& s=*impl_;
    if (s.packed->width != pixels.width() || s.packed->height != pixels.height()) {
        s.packed=av::frame();
        s.packed->width=pixels.width(); s.packed->height=pixels.height(); s.packed->format=AV_PIX_FMT_GBRP10LE;
        av::check(s.api.av_frame_get_buffer(s.packed.get(),32),"Allocate RGB10 decode input");
        s.decoded=av::frame();
    }
    if (!s.decoded->data[0]) {
        s.decoded->width=pixels.width(); s.decoded->height=pixels.height(); s.decoded->format=AV_PIX_FMT_RGBA;
        av::check(s.api.av_frame_get_buffer(s.decoded.get(),32),"Allocate RGBA8 decode boundary");
    }
    av::check(s.api.av_frame_make_writable(s.packed.get()),"Reuse RGB10 decode input");
    av::check(s.api.av_frame_make_writable(s.decoded.get()),"Reuse RGBA8 decode output");
    const auto raw=gbrp10Bytes(pixels);
    const auto rowBytes=qsizetype(pixels.width())*2, planeBytes=rowBytes*pixels.height();
    for (int plane=0; plane<3; ++plane) for (int y=0; y<pixels.height(); ++y)
        std::memcpy(s.packed->data[plane]+y*s.packed->linesize[plane],raw.constData()+plane*planeBytes+y*rowBytes,size_t(rowBytes));
    s.packed->colorspace=AVCOL_SPC_BT709; s.packed->color_range=AVCOL_RANGE_JPEG;
    s.decoded->colorspace=AVCOL_SPC_BT709; s.decoded->color_range=AVCOL_RANGE_JPEG;
    av::check(s.api.sws_scale_frame(s.scaler,s.decoded.get(),s.packed.get()),"Decode RGB10 as RGBA8");
    return QImage(s.decoded->data[0],pixels.width(),pixels.height(),s.decoded->linesize[0],QImage::Format_RGBA8888).copy();
}
struct HostPreviewBoundary::Impl {
    av::Api& api = av::Api::get();
    SwsContext* packing = api.sws_alloc_context();
    SwsContext* decoding = api.sws_alloc_context();
    av::Frame packed = av::frame(), decoded = av::frame();
    av::Frame rgb10 = av::frame();
    Impl() {
        if (!packing || !decoding) {
            api.sws_free_context(&packing); api.sws_free_context(&decoding);
            throw std::bad_alloc{};
        }
        packing->flags = SWS_BILINEAR; packing->threads = 0;
        decoding->flags = SWS_BICUBIC; decoding->threads = 0;
    }
    ~Impl() { api.sws_free_context(&packing); api.sws_free_context(&decoding); }
};
HostPreviewBoundary::HostPreviewBoundary() : impl_(std::make_unique<Impl>()) {}
HostPreviewBoundary::~HostPreviewBoundary() = default;
QImage HostPreviewBoundary::display(const QImage& pixels, const MediaInfo& info, JobControl& control) {
    control.checkpoint();
    auto& s = *impl_;
    const auto format = s.api.av_get_pix_fmt(ffv1PreviewPixelFormat(info).toUtf8().constData());
    if (s.packed->width != pixels.width() || s.packed->height != pixels.height() || s.packed->format != format) {
        s.packed = av::frame(); s.decoded = av::frame();
        s.packed->width=pixels.width(); s.packed->height=pixels.height(); s.packed->format=format;
        s.decoded->width=pixels.width(); s.decoded->height=pixels.height(); s.decoded->format=AV_PIX_FMT_RGBA64LE;
        av::check(s.api.av_frame_get_buffer(s.packed.get(),32), "Allocate preview packing boundary");
        av::check(s.api.av_frame_get_buffer(s.decoded.get(),32), "Allocate preview display boundary");
    }
    av::check(s.api.av_frame_make_writable(s.packed.get()), "Reuse preview packing boundary");
    av::check(s.api.av_frame_make_writable(s.decoded.get()), "Reuse preview display boundary");
    s.packed->colorspace=AVCOL_SPC_BT709; s.packed->color_range=AVCOL_RANGE_JPEG;
    if (info.pixelFormat=="gbrp10le" && format!=AV_PIX_FMT_BGR0) {
        if (s.rgb10->width!=pixels.width() || s.rgb10->height!=pixels.height()) {
            s.rgb10=av::frame();
            s.rgb10->width=pixels.width(); s.rgb10->height=pixels.height(); s.rgb10->format=AV_PIX_FMT_GBRP10LE;
            av::check(s.api.av_frame_get_buffer(s.rgb10.get(),32), "Allocate RGB10 preview boundary");
        }
        av::check(s.api.av_frame_make_writable(s.rgb10.get()), "Reuse RGB10 preview boundary");
        const auto image=pixels.convertToFormat(QImage::Format_RGBA64);
        for (int y=0;y<image.height();++y) {
            control.checkpoint();
            const auto* row=reinterpret_cast<const QRgba64*>(image.constScanLine(y));
            auto* green=reinterpret_cast<uint16_t*>(s.rgb10->data[0]+y*s.rgb10->linesize[0]);
            auto* blue=reinterpret_cast<uint16_t*>(s.rgb10->data[1]+y*s.rgb10->linesize[1]);
            auto* red=reinterpret_cast<uint16_t*>(s.rgb10->data[2]+y*s.rgb10->linesize[2]);
            const auto code=[](uint16_t value) { return uint16_t((uint32_t(value)*1023+32767)/65535); };
            for (int x=0;x<image.width();++x) { green[x]=code(row[x].green()); blue[x]=code(row[x].blue()); red[x]=code(row[x].red()); }
        }
        s.rgb10->colorspace=AVCOL_SPC_BT709; s.rgb10->color_range=AVCOL_RANGE_JPEG;
        av::check(s.api.sws_scale_frame(s.packing,s.packed.get(),s.rgb10.get()), "Pack RGB10 preview pixels");
    } else {
        const bool wide=pixels.depth()>32 && format!=AV_PIX_FMT_BGR0;
        const auto image=pixels.convertToFormat(wide ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
        auto source=av::frame();
        source->width=image.width(); source->height=image.height();
        source->format=wide ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGBA;
        source->data[0]=const_cast<uchar*>(image.constBits()); source->linesize[0]=image.bytesPerLine();
        source->colorspace=AVCOL_SPC_BT709; source->color_range=AVCOL_RANGE_JPEG;
        av::check(s.api.sws_scale_frame(s.packing,s.packed.get(),source.get()), "Pack exact preview pixels");
    }
    s.packed->colorspace=AVCOL_SPC_RGB;
    s.decoded->colorspace=AVCOL_SPC_RGB; s.decoded->color_range=AVCOL_RANGE_JPEG;
    av::check(s.api.sws_scale_frame(s.decoding,s.decoded.get(),s.packed.get()), "Decode preview display pixels");
    control.checkpoint();
    return QImage(s.decoded->data[0],pixels.width(),pixels.height(),s.decoded->linesize[0],QImage::Format_RGBA64).copy();
}
struct HostVideoDecoder::Impl {
    av::Api& api = av::Api::get();
    JobControl* control;
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVBufferRef* hardware = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* scaler = nullptr;
    av::Frame converted = av::frame();
    int stream = -1, bits = 8, rotation = 0;
    double start = 0, timestampOffset = 0;
    qint64 origin = AV_NOPTS_VALUE;
    bool draining = false;
    Impl(const QString& path, const MediaInfo& info, int ordinal, double seconds, bool preferCuda, JobControl& job,
         HostDecodePrecision precision, bool timelinePacking, int decodeThreads)
        : control(&job), bits(precision == HostDecodePrecision::Unorm8 ? 8 :
                            precision == HostDecodePrecision::Unorm16 ? 16 : depth(info)), start(seconds) {
        try {
            rotation = info.clockwiseRotation();
            format = api.avformat_alloc_context();
            if (!format) throw std::bad_alloc{};
            format->interrupt_callback = {[](void* opaque) {
                const auto* self = static_cast<Impl*>(opaque);
                return self->control && self->control->cancelled() ? 1 : 0;
            }, this};
            av::check(api.avformat_open_input(&format, path.toUtf8().constData(), nullptr, nullptr), "Open host source");
            av::check(api.avformat_find_stream_info(format, nullptr), "Probe host source");
            stream = api.av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            av::check(stream, "Find host video stream");
            {
                const auto* track=format->streams[stream];
                if (track->start_time!=AV_NOPTS_VALUE)
                    timestampOffset=track->start_time*av_q2d(track->time_base);
                else if (format->start_time!=AV_NOPTS_VALUE) timestampOffset=double(format->start_time)/AV_TIME_BASE;
            }
            const auto* decoder = api.avcodec_find_decoder(format->streams[stream]->codecpar->codec_id);
            if (!decoder) throw std::runtime_error("The source video codec is unavailable.");
            if (preferCuda) {
                for (int index = 0; const auto* capability = api.avcodec_get_hw_config(decoder, index); ++index) {
                    if (capability->device_type != AV_HWDEVICE_TYPE_CUDA) continue;
                    AVDictionary* options = nullptr;
                    api.av_dict_set(&options, "primary_ctx", "1", 0);
                    const auto code = api.av_hwdevice_ctx_create(&hardware, AV_HWDEVICE_TYPE_CUDA,
                        QByteArray::number(ordinal).constData(), options, 1);
                    api.av_dict_free(&options);
                    if (code < 0) api.av_buffer_unref(&hardware);
                    break;
                }
            }
            codec = api.avcodec_alloc_context3(decoder);
            if (!codec) throw std::bad_alloc{};
            av::check(api.avcodec_parameters_to_context(codec, format->streams[stream]->codecpar), "Describe host decoder");
            codec->pkt_timebase = format->streams[stream]->time_base;
            if (hardware) { codec->hw_device_ctx = api.av_buffer_ref(hardware); codec->get_format = selectCuda; }
            codec->thread_count = std::clamp(decodeThreads,1,16);
            av::check(api.avcodec_open2(codec, decoder, nullptr), "Open host decoder");
            packet = api.av_packet_alloc();
            scaler = api.sws_alloc_context();
            if (!packet || !scaler) throw std::bad_alloc{};
            scaler->flags = timelinePacking ? SWS_BICUBIC : SWS_BILINEAR;
            scaler->threads = 0;
            if (start > 0)
                av::check(api.avformat_seek_file(format, -1, INT64_MIN,
                    qint64((timestampOffset+std::max(0., start - .002)) * AV_TIME_BASE), INT64_MAX, 0), "Seek host source");
        } catch (...) { release(); throw; }
    }
    void release() {
        converted.reset();
        api.sws_free_context(&scaler);
        api.av_packet_free(&packet);
        api.avcodec_free_context(&codec);
        api.av_buffer_unref(&hardware);
        if (format) api.avformat_close_input(&format);
    }
    ~Impl() { release(); }
    av::Frame next() {
        for (;;) {
            control->checkpoint();
            auto frame = av::frame();
            const auto code = api.avcodec_receive_frame(codec, frame.get());
            if (code >= 0) return frame;
            if (code == AVERROR_EOF) return {};
            if (code != AVERROR(EAGAIN)) av::check(code, "Receive host video frame");
            if (draining) throw std::runtime_error("Host decoder did not finish draining.");
            for (;;) {
                api.av_packet_unref(packet);
                const auto read = api.av_read_frame(format, packet);
                if (read == AVERROR_EOF) {
                    av::check(api.avcodec_send_packet(codec, nullptr), "Drain host decoder");
                    draining = true;
                    break;
                }
                av::check(read, "Read host packet");
                if (packet->stream_index == stream) {
                    av::check(api.avcodec_send_packet(codec, packet), "Submit host packet");
                    break;
                }
            }
        }
    }
};
HostVideoDecoder::HostVideoDecoder(const QString& path, const MediaInfo& info, int ordinal,
    double start, bool cuda, JobControl& control, HostDecodePrecision precision, bool timelinePacking, int decodeThreads)
    : impl_(std::make_unique<Impl>(path, info, ordinal, start, cuda, control, precision, timelinePacking, decodeThreads)) {}
HostVideoDecoder::~HostVideoDecoder() = default;
void HostVideoDecoder::bindControl(JobControl& control) { impl_->control = &control; }
void HostVideoDecoder::detachControl() { impl_->control = nullptr; }
void HostVideoDecoder::seek(double seconds, JobControl& control) {
    auto& s = *impl_;
    s.control = &control;
    control.checkpoint();
    s.start = std::max(0., seconds);
    s.origin = AV_NOPTS_VALUE; s.draining = false;
    s.api.av_packet_unref(s.packet);
    s.api.avcodec_flush_buffers(s.codec);
    s.api.avformat_flush(s.format);
    av::check(s.api.avformat_seek_file(s.format, -1, INT64_MIN,
        qint64((s.timestampOffset+std::max(0., s.start-.002))*AV_TIME_BASE), INT64_MAX, 0), "Seek retained preview decoder");
}
double HostVideoDecoder::timeOrigin() const {
    const auto base=timeBase();
    if (impl_->origin==AV_NOPTS_VALUE) throw std::logic_error("Read the first frame before its time origin.");
    return impl_->origin*double(base.numerator)/base.denominator;
}
TimeBase HostVideoDecoder::timeBase() const {
    const auto base = impl_->format->streams[impl_->stream]->time_base;
    return {base.num, base.den};
}
std::optional<TimedImage> HostVideoDecoder::read() {
    auto& s = *impl_;
    for (;;) {
        auto frame = s.next();
        if (!frame) return {};
        if (frame->flags & AV_FRAME_FLAG_CORRUPT) throw std::runtime_error("Source decoder returned a corrupt frame.");
        const auto base = timeBase();
        if (frame->pts == AV_NOPTS_VALUE) throw std::runtime_error("Source frame has no timestamp.");
        if (frame->pts * double(base.numerator) / base.denominator + .002 < s.start+s.timestampOffset) continue;
        if (s.origin == AV_NOPTS_VALUE) s.origin = frame->pts;
        const auto pts = frame->pts - s.origin, duration = frame->duration;
        if (frame->format == AV_PIX_FMT_CUDA) {
            auto host = av::frame();
            av::check(s.api.av_hwframe_transfer_data(host.get(), frame.get(), 0), "Download NVDEC pixels");
            av::check(s.api.av_frame_copy_props(host.get(), frame.get()), "Copy decoded color metadata");
            frame = std::move(host);
        }
        auto* destination = s.converted.get();
        const auto pixelFormat = s.bits > 8 ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGBA;
        if (destination->width != frame->width || destination->height != frame->height || destination->format != pixelFormat) {
            s.converted = av::frame(); destination = s.converted.get();
            destination->width = frame->width; destination->height = frame->height; destination->format = pixelFormat;
            av::check(s.api.av_frame_get_buffer(destination, 32), "Allocate host conversion frame");
        }
        av::check(s.api.av_frame_make_writable(destination), "Reuse host conversion frame");
        // Packing does not request a gamut or transfer conversion. Neutralize
        // these fields so swscale only converts layout, matrix and range.
        frame->color_trc = destination->color_trc = AVCOL_TRC_UNSPECIFIED;
        frame->color_primaries = destination->color_primaries = AVCOL_PRI_UNSPECIFIED;
        destination->colorspace = frame->colorspace;
        destination->color_range = frame->color_range;
        destination->chroma_location = frame->chroma_location;
        av::check(s.api.sws_scale_frame(s.scaler, destination, frame.get()), "Pack decoded host pixels");
        QImage image(destination->data[0], destination->width, destination->height, destination->linesize[0],
            s.bits > 8 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
        image = s.rotation ? image.transformed(QTransform().rotate(s.rotation), Qt::FastTransformation) : image.copy();
        requireImageAllocation(image, "Decoded video image allocation");
        return TimedImage{image, pts, duration, base};
    }
}

struct HostVideoEncoder::Impl {
    av::Api& api = av::Api::get();
    JobControl& control;
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVStream* stream = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* scaler = nullptr;
    av::Frame converted = av::frame();
    av::Frame rgbInput = av::frame();
    bool rgb10Input = false, rgb8Preview = false;
    bool finished = false;
    bool explicitDurations=false;
    std::map<qint64,qint64> durations;
    Impl(const QString& path, const QString& choice, const Settings& settings, const MediaInfo& info,
         int ordinal, JobControl& job, HostEncodePurpose purpose) : control(job), rgb10Input(info.pixelFormat == "gbrp10le") {
        explicitDurations=info.metadata.contains("output_sample_aspect_ratio");
        AVDictionary* options = nullptr;
        try {
            const auto name = encoderName(choice);
            const auto* encoder = api.avcodec_find_encoder_by_name(name.toUtf8().constData());
            if (!encoder) throw std::runtime_error("Bundled output video codec is unavailable.");
            codec = api.avcodec_alloc_context3(encoder);
            if (!codec) throw std::bad_alloc{};
            codec->width = info.width; codec->height = info.height;
            codec->thread_count = 0;
            codec->thread_type = FF_THREAD_SLICE;
            codec->gop_size = -1; codec->max_b_frames = -1;
            // Keep each encoder's defaults, including SVT-AV1's valid QP limits.
            codec->bit_rate_tolerance = 128000;
            codec->time_base = {1, 60000000}; codec->framerate = api.av_d2q(info.fps, 1000000);
            const auto aspect=info.outputSampleAspectRatio();codec->sample_aspect_ratio = {aspect.width(),aspect.height()};
            const bool highDepth = info.hdr || depth(info) > 8 || settings.hdr_mode;
            codec->pix_fmt = name == "ffv1" ? purpose == HostEncodePurpose::NativePreview
                ? api.av_get_pix_fmt(ffv1PreviewPixelFormat(info).toUtf8().constData()) : AV_PIX_FMT_GBRP10LE
                : name == "prores_ks" ? AV_PIX_FMT_YUV422P10LE
                : name.startsWith("h264") || name == "libx264" || !highDepth ? AV_PIX_FMT_YUV420P
                : name.endsWith("_nvenc") ? AV_PIX_FMT_P010LE : AV_PIX_FMT_YUV420P10LE;
            rgb8Preview = purpose == HostEncodePurpose::NativePreview && codec->pix_fmt == AV_PIX_FMT_BGR0;
            codec->colorspace = name == "ffv1" ? AVCOL_SPC_RGB : info.hdr ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709;
            codec->color_range = name == "ffv1" ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
            auto primaries = api.av_color_primaries_from_name(info.primaries.toUtf8().constData());
            auto transfer = api.av_color_transfer_from_name(info.transfer.toUtf8().constData());
            codec->color_primaries = primaries < 0 ? info.hdr ? AVCOL_PRI_BT2020 : AVCOL_PRI_UNSPECIFIED : AVColorPrimaries(primaries);
            codec->color_trc = transfer < 0 ? info.hdr ? AVCOL_TRC_SMPTE2084 : AVCOL_TRC_UNSPECIFIED : AVColorTransferCharacteristic(transfer);
            const auto arguments = videoEncoderArguments(choice, settings.quality, info.width, info.height, info.fps, info.hdr);
            for (qsizetype index = 0; index + 1 < arguments.size(); index += 2) {
                const auto key = arguments[index].mid(1).section(':', 0, 0), value = arguments[index + 1];
                if (key == "b") {
                    auto bitrate = value; const bool kilo = bitrate.endsWith('k'); if (kilo) bitrate.chop(1);
                    codec->bit_rate = bitrate.toLongLong() * (kilo ? 1000 : 1);
                } else if (key != "c" && key != "pix_fmt")
                    api.av_dict_set(&options, key.toUtf8().constData(), value.toUtf8().constData(), 0);
            }
            if (name.endsWith("nvenc")) api.av_dict_set(&options, "gpu", QByteArray::number(ordinal).constData(), 0);
            av::check(api.avformat_alloc_output_context2(&format, nullptr, name == "ffv1"
                ? purpose == HostEncodePurpose::NativePreview ? "matroska" : "nut" : name == "prores_ks" ? "mov" : "mp4",
                path.toUtf8().constData()), "Create host video muxer");
            format->avoid_negative_ts=AVFMT_AVOID_NEG_TS_DISABLED;
            if (format->oformat->flags & AVFMT_GLOBALHEADER) codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            av::check(api.avcodec_open2(codec, encoder, &options), "Open host video encoder");
            api.av_dict_free(&options);
            stream = api.avformat_new_stream(format, nullptr);
            if (!stream) throw std::bad_alloc{};
            stream->time_base = codec->time_base; stream->avg_frame_rate = codec->framerate;
            stream->sample_aspect_ratio=codec->sample_aspect_ratio;
            av::check(api.avcodec_parameters_from_context(stream->codecpar, codec), "Describe host output");
            retainStaticHdrMetadata(stream->codecpar, info);
            av::check(api.avio_open(&format->pb, path.toUtf8().constData(), AVIO_FLAG_WRITE), "Open host video output");
            if (name!="ffv1") api.av_dict_set(&options,"video_track_timescale","60000000",0);
            av::check(api.avformat_write_header(format, &options), "Write host video header");
            api.av_dict_free(&options);
            packet = api.av_packet_alloc(); scaler = api.sws_alloc_context();
            if (!packet || !scaler) throw std::bad_alloc{};
            scaler->flags = SWS_BILINEAR; scaler->threads = 0;
            converted->width = codec->width; converted->height = codec->height; converted->format = codec->pix_fmt;
            av::check(api.av_frame_get_buffer(converted.get(), 32), "Allocate host encoder frame");
        } catch (...) { api.av_dict_free(&options); release(); throw; }
    }
    void packets() {
        for (;;) {
            control.checkpoint();
            const auto code = api.avcodec_receive_packet(codec, packet);
            if (code == AVERROR_EOF || code == AVERROR(EAGAIN)) return;
            av::check(code, "Receive host encoded packet");
            if(explicitDurations)if(auto found=durations.find(packet->pts);found!=durations.end()){packet->duration=found->second;durations.erase(found);}
            api.av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
            packet->stream_index = stream->index;
            av::check(api.av_interleaved_write_frame(format, packet), "Mux host encoded packet");
            api.av_packet_unref(packet);
        }
    }
    void write(AVFrame* source, qint64 pts, qint64 duration, TimeBase timeBase, bool forceIntra=false) {
        control.checkpoint();
        auto* destination = converted.get();
        if (source->width != codec->width || source->height != codec->height)
            throw std::runtime_error("Encoder frame geometry changed.");
        av::check(api.av_frame_make_writable(destination), "Reuse encoder pixels");
        const auto matrix = codec->colorspace == AVCOL_SPC_BT2020_NCL ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709;
        if (api.av_pix_fmt_desc_get(AVPixelFormat(source->format))->flags & AV_PIX_FMT_FLAG_RGB) {
            source->color_range = AVCOL_RANGE_JPEG;
            source->colorspace = matrix;
        } else if (source->colorspace == AVCOL_SPC_UNSPECIFIED) source->colorspace = matrix;
        source->color_trc = AVCOL_TRC_UNSPECIFIED;
        source->color_primaries = AVCOL_PRI_UNSPECIFIED;
        destination->colorspace = matrix; destination->color_range = codec->color_range;
        destination->chroma_location = source->chroma_location;
        destination->color_trc = AVCOL_TRC_UNSPECIFIED; destination->color_primaries = AVCOL_PRI_UNSPECIFIED;
        av::check(api.sws_scale_frame(scaler, destination, source), "Convert delivery pixels");
        destination->colorspace = codec->colorspace; destination->color_range = codec->color_range;
        destination->color_primaries = codec->color_primaries; destination->color_trc = codec->color_trc;
        destination->pts = api.av_rescale_q(pts, {timeBase.numerator, timeBase.denominator}, codec->time_base);
        destination->duration = api.av_rescale_q(duration, {timeBase.numerator, timeBase.denominator}, codec->time_base);
        if(explicitDurations)durations[destination->pts]=destination->duration;
        destination->pict_type=forceIntra ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        copyStaticHdrMetadata(destination,stream->codecpar);
        av::check(api.avcodec_send_frame(codec, destination), "Submit host encoder pixels");
        packets();
    }
    void release() {
        rgbInput.reset(); converted.reset(); api.sws_free_context(&scaler); api.av_packet_free(&packet); api.avcodec_free_context(&codec);
        if (format) { if (format->pb) api.avio_closep(&format->pb); api.avformat_free_context(format); format = nullptr; }
    }
    ~Impl() { release(); }
};
HostVideoEncoder::HostVideoEncoder(const QString& path, const QString& codec, const Settings& settings,
    const MediaInfo& info, int ordinal, JobControl& control, HostEncodePurpose purpose)
    : impl_(std::make_unique<Impl>(path, codec, settings, info, ordinal, control, purpose)) {}
HostVideoEncoder::~HostVideoEncoder() = default;
void HostVideoEncoder::write(const QImage& pixels, qint64 pts, qint64 duration, TimeBase timeBase, bool forceIntra) {
    auto& s = *impl_;
    if (s.rgb10Input && !s.rgb8Preview) {
        s.control.checkpoint();
        auto* frame = s.rgbInput.get();
        if (frame->width != pixels.width() || frame->height != pixels.height()) {
            s.rgbInput = av::frame(); frame = s.rgbInput.get();
            frame->width = pixels.width(); frame->height = pixels.height(); frame->format = AV_PIX_FMT_GBRP10LE;
            av::check(s.api.av_frame_get_buffer(frame, 32), "Allocate RGB10 delivery input");
        }
        av::check(s.api.av_frame_make_writable(frame), "Reuse RGB10 delivery input");
        const auto image = pixels.convertToFormat(QImage::Format_RGBA64);
        for (int y=0; y<image.height(); ++y) {
            s.control.checkpoint();
            const auto* row = reinterpret_cast<const QRgba64*>(image.constScanLine(y));
            auto* green = reinterpret_cast<uint16_t*>(frame->data[0]+y*frame->linesize[0]);
            auto* blue = reinterpret_cast<uint16_t*>(frame->data[1]+y*frame->linesize[1]);
            auto* red = reinterpret_cast<uint16_t*>(frame->data[2]+y*frame->linesize[2]);
            auto code = [](uint16_t value) { return uint16_t((uint32_t(value)*1023+32767)/65535); };
            for (int x=0; x<image.width(); ++x) { green[x]=code(row[x].green()); blue[x]=code(row[x].blue()); red[x]=code(row[x].red()); }
        }
        s.write(frame, pts, duration, timeBase, forceIntra);
        return;
    }
    // Preserve exact RGB8 codes when an 8-bit source used a wider model layout.
    // swscale's 16-to-8 dithering can otherwise change an original code by one.
    const bool wide = pixels.depth() > 32 && !s.rgb8Preview;
    const auto image = pixels.convertToFormat(wide ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    auto source = av::frame();
    source->width = image.width(); source->height = image.height(); source->format = wide ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGBA;
    source->data[0] = const_cast<uchar*>(image.constBits()); source->linesize[0] = image.bytesPerLine();
    s.write(source.get(), pts, duration, timeBase, forceIntra);
}
void HostVideoEncoder::write(const CudaVideoFrame& frame) {
    auto& s = *impl_;
    s.control.checkpoint();
    auto host = av::frame();
    auto* source = static_cast<AVFrame*>(frame.owner.get());
    av::check(s.api.av_hwframe_transfer_data(host.get(), source, 0), "Download delivery pixels");
    av::check(s.api.av_frame_copy_props(host.get(), source), "Copy delivery metadata");
    s.write(host.get(), frame.pts, frame.duration, frame.timeBase);
}
void HostVideoEncoder::finish() {
    auto& s = *impl_;
    if (s.finished) return;
    av::check(s.api.avcodec_send_frame(s.codec, nullptr), "Drain host encoder");
    s.packets();
    av::check(s.api.av_write_trailer(s.format), "Finish host video");
    s.finished = true;
}
} // namespace ve
