#pragma once
#include <QImage>
#include <QByteArrayView>
#include <QtEndian>
#include <array>
#include <cstdint>
#include <stdexcept>

namespace ve {
inline quint16 rgb10Code(quint16 value) {
    return quint16((quint32(value)*1023+32767)/65535);
}
inline QByteArray gbrp10Bytes(const QImage& pixels) {
    const auto image = pixels.convertToFormat(QImage::Format_RGBA64);
    const auto planeBytes = qsizetype(image.width())*image.height()*2;
    QByteArray packed(planeBytes*3, Qt::Uninitialized);
    for (int y=0; y<image.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgba64*>(image.constScanLine(y));
        for (int x=0; x<image.width(); ++x) {
            const auto offset = (qsizetype(y)*image.width()+x)*2;
            qToLittleEndian(rgb10Code(row[x].green()), packed.data()+offset);
            qToLittleEndian(rgb10Code(row[x].blue()), packed.data()+planeBytes+offset);
            qToLittleEndian(rgb10Code(row[x].red()), packed.data()+planeBytes*2+offset);
        }
    }
    return packed;
}
// libswscale expands a ten-bit RGB sample by repeating its high bits. Native
// host consumers use that decoded RGBA16 representation, while filters and
// lossless delivery retain the original planar ten-bit codes.
inline QImage decodedRgb10Rgba(const QImage& pixels) {
    auto image = pixels.convertToFormat(QImage::Format_RGBA64);
    for (int y=0; y<image.height(); ++y) {
        auto* row = reinterpret_cast<QRgba64*>(image.scanLine(y));
        auto expand = [](quint16 value) { const auto code=rgb10Code(value); return quint16((code<<6)|(code>>4)); };
        for (int x=0; x<image.width(); ++x)
            row[x]=QRgba64::fromRgba64(expand(row[x].red()), expand(row[x].green()), expand(row[x].blue()), 65535);
    }
    return image;
}
inline QImage rgbaFromGbrp10(QByteArrayView pixels, QSize size) {
    const auto planeBytes = qsizetype(size.width())*size.height()*2;
    if (!size.isValid() || pixels.size() != planeBytes*3)
        throw std::runtime_error("Incomplete planar RGB10 frame.");
    QImage image(size, QImage::Format_RGBA64);
    if (image.isNull()) throw std::bad_alloc{};
    for (int y=0; y<size.height(); ++y) {
        auto* row = reinterpret_cast<QRgba64*>(image.scanLine(y));
        for (int x=0; x<size.width(); ++x) {
            const auto offset = (qsizetype(y)*size.width()+x)*2;
            auto channel = [&](int plane) {
                const auto code = qFromLittleEndian<quint16>(pixels.data()+plane*planeBytes+offset);
                if (code > 1023) throw std::runtime_error("A planar RGB10 channel exceeded its legal range.");
                return quint16((quint32(code)*65535+511)/1023);
            };
            row[x] = QRgba64::fromRgba64(channel(2), channel(0), channel(1), 65535);
        }
    }
    return image;
}
// Packed, straight-alpha pixels. The QImage owns every plane for the complete native call.
// No reinterpretation of float RGBA as float RGB is permitted at this boundary.
class PackedFrame {
  public:
    enum class Format : std::uint32_t { Rgba8 = 1, Rgba16 = 4 };
    explicit PackedFrame(const QImage& image)
        : image_(image.convertToFormat(image.format() == QImage::Format_RGBA8888
                                           ? QImage::Format_RGBA8888
                                           : QImage::Format_RGBA64)) {
        if (image_.isNull())
            throw std::runtime_error("A native frame must contain pixels.");
    }
    explicit PackedFrame(QSize size, Format format)
        : image_(size, format == Format::Rgba8 ? QImage::Format_RGBA8888 : QImage::Format_RGBA64) {
        if (image_.isNull())
            throw std::runtime_error("Cannot allocate a native frame.");
        image_.fill(Qt::transparent);
    }
    [[nodiscard]] Format format() const {
        return image_.format() == QImage::Format_RGBA8888 ? Format::Rgba8 : Format::Rgba16;
    }
    [[nodiscard]] const QImage& image() const { return image_; }
    [[nodiscard]] QImage& image() { return image_; }

  private:
    QImage image_;
};
} // namespace ve
