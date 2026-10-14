#include "packet_stream.hpp"
#include "native_frame.hpp"
#include "av_pipe_io.hpp"
#include "media.hpp"
#include <exception>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
}

namespace ve {
namespace {
using PipeIo = av::PipeIo;
} // namespace
struct FramePacketReader::Impl {
    PipeIo pipe;
    AVFormatContext* format = nullptr;
    AVIOContext* io = nullptr;
    AVPacket* packet = nullptr;
    int stream = -1;
    RawPixelFormat pixels;
    Impl(PipeProcess& source, JobControl& control, RawPixelFormat layout)
        : pipe(source, control), pixels(layout) {
        auto& api = av::Api::get();
        try {
            format = api.avformat_alloc_context();
            if (!format)
                throw std::bad_alloc{};
            io = pipe.context();
            format->pb = io;
            format->flags |= AVFMT_FLAG_CUSTOM_IO;
            pipe.check(api.avformat_open_input(&format, nullptr, api.av_find_input_format("nut"), nullptr));
            if (!format->nb_streams)
                throw std::runtime_error("Video decoder returned no stream.");
            for (unsigned index=0;index<format->nb_streams;++index) {
                const auto type=format->streams[index]->codecpar->codec_type;
                if (type==AVMEDIA_TYPE_VIDEO && stream<0) stream=int(index);
            }
            if (stream<0) throw std::runtime_error("Video decoder returned no video stream.");
            packet = api.av_packet_alloc();
            if (!packet)
                throw std::bad_alloc{};
        } catch (...) {
            release();
            throw;
        }
    }
    void release() {
        auto& api = av::Api::get();
        api.av_packet_free(&packet);
        if (format)
            api.avformat_close_input(&format);
        io = nullptr;
    }
    ~Impl() { release(); }
};
FramePacketReader::FramePacketReader(PipeProcess& source, JobControl& control, RawPixelFormat format)
    : impl_(std::make_unique<Impl>(source, control, format)) {}
FramePacketReader::~FramePacketReader() = default;
TimeBase FramePacketReader::timeBase() const {
    const auto base = impl_->format->streams[impl_->stream]->time_base;
    return {base.num, base.den};
}
std::optional<TimedImage> FramePacketReader::read() {
    auto packet = readPixels();
    if (!packet)
        return {};
    if (packet->format == RawPixelFormat::Gbrp10)
        return TimedImage{rgbaFromGbrp10(packet->pixels, packet->size), packet->pts, packet->duration, packet->timeBase};
    if (packet->format == RawPixelFormat::P010)
        throw std::runtime_error("The image pipeline requires packed RGBA frames.");
    return TimedImage{fromRgbaBytes(packet->pixels, packet->size.width(), packet->size.height(),
                                  packet->format == RawPixelFormat::Rgba64), packet->pts,
                      packet->duration, packet->timeBase};
}
std::optional<TimedPixels> FramePacketReader::readPixels() {
    auto& state = *impl_;
    auto& api = av::Api::get();
    while (true) {
        state.pipe.control.checkpoint();
        api.av_packet_unref(state.packet);
        const auto status = api.av_read_frame(state.format, state.packet);
        if (state.pipe.failure)
            std::rethrow_exception(state.pipe.failure);
        if (status == AVERROR_EOF)
            return {};
        state.pipe.check(status);
        if (state.packet->stream_index != state.stream)
            continue;
        const auto* parameters = state.format->streams[state.stream]->codecpar;
        if (parameters->codec_id != AV_CODEC_ID_RAWVIDEO)
            throw std::runtime_error("Decoder did not return raw video.");
        const auto pixelFormat = state.pixels == RawPixelFormat::Rgba64 ? AV_PIX_FMT_RGBA64LE
            : state.pixels == RawPixelFormat::Rgba8 ? AV_PIX_FMT_RGBA
            : state.pixels == RawPixelFormat::Gbrp10 ? AV_PIX_FMT_GBRP10LE : AV_PIX_FMT_P010LE;
        if (parameters->format != AV_PIX_FMT_NONE &&
            parameters->format != pixelFormat)
            throw std::runtime_error("The decoder returned an unsupported raw pixel format.");
        const QByteArray data(reinterpret_cast<const char*>(state.packet->data), state.packet->size);
        const auto expected = qsizetype(parameters->width) * parameters->height *
            (state.pixels == RawPixelFormat::Rgba64 ? 8 : state.pixels == RawPixelFormat::Rgba8 ? 4
                : state.pixels == RawPixelFormat::Gbrp10 ? 6 : 3);
        if (data.size() != expected)
            throw std::runtime_error("The decoder returned an incomplete raw frame.");
        return TimedPixels{data,
                           {parameters->width, parameters->height},
                           state.pixels,
                           state.packet->pts,
                           state.packet->duration,
                           timeBase()};
    }
}
} // namespace ve
