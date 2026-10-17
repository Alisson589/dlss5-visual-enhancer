#include "image_codecs.hpp"
#include "resolution.hpp"
#include "infrastructure/paths.hpp"
#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QLibrary>
#include <QRgba64>
#include <QSaveFile>
#include <QSet>
#include <cstdio>
#include <cstdlib>
#include <jpeglib.h>
#include <libraw/libraw.h>
#include <windows.h>
#define LIBHEIF_STATIC_BUILD
#include <algorithm>
#include <cstring>
#include <libheif/heif.h>
#include <memory>

namespace ve {
namespace {
void rawCheck(int result) {
    if (result != LIBRAW_SUCCESS)
        throw std::runtime_error(libraw_strerror(result));
}
void openRaw(LibRaw& decoder, const QString& path) {
    rawCheck(decoder.open_file(path.toStdWString().c_str()));
}
int rawProgress(void* context, enum LibRaw_progress, int, int) {
    return static_cast<JobControl*>(context)->cancelled() ? 1 : 0;
}
class HeifLibrary {
  public:
    HeifLibrary() {
        const auto directory = Paths::instance().at("bin/runtime/image-codecs");
        const auto names = QDir(directory).entryList({"libheif*.dll"}, QDir::Files);
        if (names.isEmpty())
            throw std::runtime_error("The bundled HEIF image decoder is missing.");
        const auto path = QDir::toNativeSeparators(QDir(directory).filePath(names.front())).toStdWString();
        library_ = LoadLibraryExW(path.c_str(), nullptr,
                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!library_)
            throw std::runtime_error("Cannot load the bundled HEIF codec.");
    }
    template <class Function> Function resolve(const char* name) {
        const auto symbol = GetProcAddress(library_, name);
        if (!symbol)
            throw std::runtime_error("HEIF decoder is missing export " + std::string(name));
        return reinterpret_cast<Function>(symbol);
    }
    static HeifLibrary& instance() {
        static HeifLibrary library;
        return library;
    }

  private:
    HMODULE library_ = nullptr;
};
void heifCheck(const heif_error& error) {
    if (error.code != heif_error_Ok)
        throw std::runtime_error(error.message ? error.message : "HEIF decoding failed.");
}
} // namespace
QImage decodeControlMask(const QString& path, JobControl& control) {
    configureImageDecoding();
    control.checkpoint();
    QImage image;
    if (isRawImage(path))
        image = decodeRawImage(path, control);
    else if (isHeifImage(path))
        image = decodeHeifImage(path, control);
    else {
        QImageReader reader(path);
        if (reader.size().isValid()) requireResolution(reader.size(), "Control mask");
        reader.setAutoTransform(true);
        image = reader.read();
        if (image.isNull())
            throw std::runtime_error(reader.errorString().toStdString());
    }
    const bool grayscale = image.format() == QImage::Format_Grayscale8 ||
                           image.format() == QImage::Format_Grayscale16 ||
                           image.format() == QImage::Format_Mono || image.format() == QImage::Format_MonoLSB;
    const auto rgba = image.convertToFormat(QImage::Format_RGBA64);
    requireResolution(image.size(), "Control mask");
    requireImageAllocation(rgba, "Control mask conversion");
    QImage result(image.size(), QImage::Format_RGBA32FPx4);
    requireImageAllocation(result, "Control mask allocation");
    // Read the unassociated 16-bit channels directly to avoid rounding through
    // Qt's premultiplied float conversion. Grayscale broadcasts to all four.
    for (int y = 0; y < result.height(); ++y) {
        control.checkpoint();
        auto* row = reinterpret_cast<float*>(result.scanLine(y));
        const auto* source = reinterpret_cast<const QRgba64*>(rgba.constScanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            row[x * 4] = source[x].red() / 65535.f;
            row[x * 4 + 1] = grayscale ? row[x * 4] : source[x].green() / 65535.f;
            row[x * 4 + 2] = grayscale ? row[x * 4] : source[x].blue() / 65535.f;
            row[x * 4 + 3] = grayscale ? row[x * 4] : source[x].alpha() / 65535.f;
        }
    }
    return result;
}
QImage jpegOverWhite(const QImage& image) {
    const auto source=image.convertToFormat(QImage::Format_RGBA8888);
    requireImageAllocation(source, "JPEG input conversion");
    QImage result(image.size(), QImage::Format_RGB888);
    requireImageAllocation(result, "JPEG image allocation");
    for (int y=0;y<source.height();++y) {
        const auto* from=source.constScanLine(y);
        auto* to=result.scanLine(y);
        for (int x=0;x<source.width();++x) {
            const unsigned alpha=from[x*4+3];
            for (int c=0;c<3;++c)
                to[x*3+c]=uchar((unsigned(from[x*4+c])*alpha+255*(255-alpha)+127)/255);
        }
    }
    result.setColorSpace(image.colorSpace());
    result.setDotsPerMeterX(image.dotsPerMeterX());
    result.setDotsPerMeterY(image.dotsPerMeterY());
    return result;
}
void encodeJpeg(const QString& path, const QImage& image, int quality) {
    struct Compression {
        jpeg_compress_struct compressor{};
        jpeg_error_mgr error{};
        unsigned char* buffer = nullptr;
        unsigned long length = 0;
        bool created = false;
        ~Compression() {
            if (created)
                jpeg_destroy_compress(&compressor);
            std::free(buffer);
        }
    } state;
    state.compressor.err = jpeg_std_error(&state.error);
    state.error.error_exit = [](j_common_ptr info) {
        char message[JMSG_LENGTH_MAX]{};
        info->err->format_message(info, message);
        throw std::runtime_error(message);
    };
    jpeg_create_compress(&state.compressor);
    state.created = true;
    jpeg_mem_dest(&state.compressor, &state.buffer, &state.length);
    state.compressor.image_width = JDIMENSION(image.width());
    state.compressor.image_height = JDIMENSION(image.height());
    state.compressor.input_components = 3;
    state.compressor.in_color_space = JCS_RGB;
    jpeg_set_defaults(&state.compressor);
    for (int i = 0; i < 3; ++i) {
        state.compressor.comp_info[i].h_samp_factor = 1;
        state.compressor.comp_info[i].v_samp_factor = 1;
    }
    jpeg_set_quality(&state.compressor, quality, TRUE);
    jpeg_start_compress(&state.compressor, TRUE);
    const auto rgb = image.convertToFormat(QImage::Format_RGB888);
    while (state.compressor.next_scanline < state.compressor.image_height) {
        auto* row = const_cast<JSAMPLE*>(rgb.constScanLine(int(state.compressor.next_scanline)));
        jpeg_write_scanlines(&state.compressor, &row, 1);
    }
    jpeg_finish_compress(&state.compressor);
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) ||
        output.write(reinterpret_cast<const char*>(state.buffer), qsizetype(state.length)) !=
            qsizetype(state.length) ||
        !output.commit())
        throw std::runtime_error(output.errorString().toStdString());
}
bool isRawImage(const QString& path) {
    static const QSet<QString> extensions{
        "3fr", "arw", "bay", "cap", "cr2", "cr3", "dcr", "dcs", "dng", "drf", "eip", "erf", "fff",
        "gpr", "iiq", "k25", "kdc", "mdc", "mef", "mos", "mrw", "nef", "nrw", "obm", "orf", "pef",
        "ptx", "pxn", "r3d", "raf", "raw", "rw2", "rwl", "rwz", "sr2", "srf", "srw", "x3f"};
    return extensions.contains(QFileInfo(path).suffix().toLower());
}
bool isHeifImage(const QString& path) {
    return QStringList{"heic", "heif"}.contains(QFileInfo(path).suffix().toLower());
}
QSize rawImageSize(const QString& path) {
    LibRaw decoder;
    openRaw(decoder, path);
    rawCheck(decoder.adjust_sizes_info_only());
    return {decoder.imgdata.sizes.iwidth, decoder.imgdata.sizes.iheight};
}
QImage decodeRawImage(const QString& path, JobControl& control) {
    control.checkpoint();
    LibRaw decoder;
    decoder.imgdata.rawparams.max_raw_memory_mb = imageDecoderBudgetMiB;
    decoder.set_progress_handler(rawProgress, &control);
    decoder.imgdata.params.use_camera_wb = 1;
    decoder.imgdata.params.output_color = 1;
    decoder.imgdata.params.output_bps = 16;
    openRaw(decoder, path);
    requireResolution({decoder.imgdata.sizes.width, decoder.imgdata.sizes.height}, "RAW image");
    const auto unpacked = decoder.unpack();
    control.checkpoint();
    rawCheck(unpacked);
    const auto processed = decoder.dcraw_process();
    control.checkpoint();
    rawCheck(processed);
    int width=0,height=0,colors=0,bits=0;
    decoder.get_mem_image_format(&width,&height,&colors,&bits);
    if (bits != 16 || (colors != 3 && colors != 4))
        throw std::runtime_error("LibRaw did not return a supported RGB image.");
    requireResolution({width,height}, "Decoded RAW image");
    QImage result(width,height,QImage::Format_RGBA64);
    requireImageAllocation(result, "RAW image allocation");
    // Export directly into owned rows, avoiding a second full RGB image and
    // the legacy processed-image record's 32-bit data_size field.
    rawCheck(decoder.copy_mem_image(result.bits(),int(result.bytesPerLine()),0));
    for (int y = 0; y < result.height(); ++y) {
        control.checkpoint();
        auto* row = reinterpret_cast<QRgba64*>(result.scanLine(y));
        for (int x = result.width()-1; x >= 0; --x) {
            const auto* source = reinterpret_cast<const quint16*>(result.constScanLine(y)) + x*colors;
            row[x] = QRgba64::fromRgba64(source[0], source[1], source[2], 65535);
        }
    }
    result.setColorSpace(QColorSpace::SRgb);
    return result;
}
QImage decodeHeifImage(const QString& path, JobControl& control) {
    control.checkpoint();
    auto& library = HeifLibrary::instance();
#define HEIF_FUNCTION(name) const auto name = library.resolve<decltype(&::name)>(#name)
    HEIF_FUNCTION(heif_context_alloc);
    HEIF_FUNCTION(heif_context_get_security_limits);
    HEIF_FUNCTION(heif_context_free);
    HEIF_FUNCTION(heif_context_read_from_memory_without_copy);
    HEIF_FUNCTION(heif_context_get_primary_image_handle);
    HEIF_FUNCTION(heif_image_handle_release);
    HEIF_FUNCTION(heif_image_handle_get_width);
    HEIF_FUNCTION(heif_image_handle_get_height);
    HEIF_FUNCTION(heif_decode_image);
    HEIF_FUNCTION(heif_image_release);
    HEIF_FUNCTION(heif_image_get_width);
    HEIF_FUNCTION(heif_image_get_height);
    HEIF_FUNCTION(heif_image_get_plane_readonly);
    HEIF_FUNCTION(heif_image_get_bits_per_pixel_range);
    HEIF_FUNCTION(heif_image_handle_get_raw_color_profile_size);
    HEIF_FUNCTION(heif_image_handle_get_raw_color_profile);
#undef HEIF_FUNCTION
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error(file.errorString().toStdString());
    const auto bytes = file.readAll();
    std::unique_ptr<heif_context, decltype(heif_context_free)> context(heif_context_alloc(),
                                                                       heif_context_free);
    if (!context)
        throw std::bad_alloc{};
    auto* limits = heif_context_get_security_limits(context.get());
    limits->max_image_size_pixels = maximumImagePixels;
    limits->max_memory_block_size = quint64(imageDecoderBudgetMiB) * 1024 * 1024;
    heifCheck(heif_context_read_from_memory_without_copy(context.get(), bytes.constData(),
                                                         size_t(bytes.size()), nullptr));
    heif_image_handle* rawHandle = nullptr;
    heifCheck(heif_context_get_primary_image_handle(context.get(), &rawHandle));
    std::unique_ptr<heif_image_handle, decltype(heif_image_handle_release)> handle(rawHandle,
                                                                                   heif_image_handle_release);
    requireResolution({heif_image_handle_get_width(handle.get()),heif_image_handle_get_height(handle.get())}, "HEIF image");
    heif_image* rawImage = nullptr;
    heifCheck(heif_decode_image(handle.get(), &rawImage, heif_colorspace_RGB,
                                heif_chroma_interleaved_RRGGBBAA_LE, nullptr));
    std::unique_ptr<heif_image, decltype(heif_image_release)> image(rawImage, heif_image_release);
    const int width = heif_image_get_width(image.get(), heif_channel_interleaved),
              height = heif_image_get_height(image.get(), heif_channel_interleaved);
    requireResolution({width,height}, "Decoded HEIF image");
    int stride = 0;
    const auto* data = heif_image_get_plane_readonly(image.get(), heif_channel_interleaved, &stride);
    const int depth = heif_image_get_bits_per_pixel_range(image.get(), heif_channel_interleaved);
    if (!data || width < 1 || height < 1 || depth < 1 || depth > 16 || stride < width * 8)
        throw std::runtime_error("HEIF decoder returned invalid image dimensions or precision.");
    QImage result(width, height, QImage::Format_RGBA64);
    requireImageAllocation(result, "HEIF image allocation");
    const auto maximum = double((1u << depth) - 1);
    for (int y = 0; y < height; ++y) {
        control.checkpoint();
        auto* to = reinterpret_cast<QRgba64*>(result.scanLine(y));
        const auto* from = reinterpret_cast<const quint16*>(data + qsizetype(y) * stride);
        for (int x = 0; x < width; ++x) {
            const auto convert = [&](int channel) {
                return quint16(std::clamp(std::lround(from[x * 4 + channel] * 65535. / maximum), 0l, 65535l));
            };
            to[x] = QRgba64::fromRgba64(convert(0), convert(1), convert(2), convert(3));
        }
    }
    const auto profileBytes = heif_image_handle_get_raw_color_profile_size(handle.get());
    if (profileBytes && profileBytes < 16 * 1024 * 1024) {
        QByteArray profile(qsizetype(profileBytes), Qt::Uninitialized);
        heifCheck(heif_image_handle_get_raw_color_profile(handle.get(), profile.data()));
        result.setColorSpace(QColorSpace::fromIccProfile(profile));
    }
    if (!result.colorSpace().isValid())
        result.setColorSpace(QColorSpace::SRgb);
    auto converted=result.convertedToColorSpace(QColorSpace::SRgb);
    requireImageAllocation(converted, "HEIF color conversion");
    return converted;
}
} // namespace ve
