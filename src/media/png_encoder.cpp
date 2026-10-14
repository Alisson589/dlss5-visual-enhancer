#include "png_encoder.hpp"
#include <QFuture>
#include <QSaveFile>
#include <QThreadPool>
#include <QtConcurrent>
#include <QtEndian>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <exception>

namespace ve {
namespace {
void write(QSaveFile& file, QByteArrayView bytes) {
    if (file.write(bytes.data(), bytes.size()) != bytes.size())
        throw std::runtime_error(file.errorString().toStdString());
}
void chunk(QSaveFile& file, const char* type, QByteArrayView bytes) {
    std::array<char,4> number{};
    qToBigEndian(quint32(bytes.size()), number.data());
    write(file, QByteArrayView(number.data(), 4));
    write(file, QByteArrayView(type, 4));
    write(file, bytes);
    auto crc = crc32(0, reinterpret_cast<const Bytef*>(type), 4);
    if (!bytes.empty()) crc = crc32(crc, reinterpret_cast<const Bytef*>(bytes.data()), uInt(bytes.size()));
    qToBigEndian(quint32(crc), number.data());
    write(file, QByteArrayView(number.data(), 4));
}
bool opaque(const QImage& image, JobControl& control) {
    for (int y=0; y<image.height(); ++y) {
        if (!(y%32)) control.checkpoint();
        const auto* row = image.constScanLine(y);
        for (int x=0; x<image.width(); ++x)
            if (row[x*4+3] != 255) return false;
    }
    return true;
}
struct Block {
    QByteArray data;
    uLong checksum = 1;
    qint64 bytes = 0;
    std::exception_ptr error;
};
Block compressRows(const QImage& image, int first, int count, bool last, JobControl& control) {
    Block result;
    try {
        const int channels = image.format() == QImage::Format_RGB888 ? 3 : 4;
        const int stride = image.width()*channels;
        QByteArray filtered(qsizetype(stride+1)*count, Qt::Uninitialized);
        for (int row=0; row<count; ++row) {
            if (!(row%16)) control.checkpoint();
            auto* to = reinterpret_cast<uchar*>(filtered.data()+qsizetype(stride+1)*row);
            *to++ = 1; // Sub is lossless and permits a vectorized row transform.
            const auto* from = image.constScanLine(first+row);
            std::copy_n(from, channels, to);
            for (int x=channels; x<stride; ++x) to[x] = uchar(from[x]-from[x-channels]);
        }
        result.bytes = filtered.size();
        result.checksum = adler32(1, reinterpret_cast<const Bytef*>(filtered.constData()), uInt(filtered.size()));
        z_stream stream{};
        if (deflateInit2(&stream, Z_BEST_SPEED, Z_DEFLATED, -MAX_WBITS, MAX_MEM_LEVEL, Z_DEFAULT_STRATEGY) != Z_OK)
            throw std::runtime_error("Cannot initialize PNG compression.");
        struct End { z_stream& stream; ~End() { deflateEnd(&stream); } } end{stream};
        result.data.resize(qsizetype(deflateBound(&stream, uLong(filtered.size())))+64);
        stream.next_in = reinterpret_cast<Bytef*>(filtered.data());
        stream.avail_in = uInt(filtered.size());
        stream.next_out = reinterpret_cast<Bytef*>(result.data.data());
        stream.avail_out = uInt(result.data.size());
        const int status = deflate(&stream, last ? Z_FINISH : Z_FULL_FLUSH);
        if ((last ? status != Z_STREAM_END : status != Z_OK) || stream.avail_in || !stream.avail_out)
            throw std::runtime_error("PNG compression did not complete.");
        result.data.resize(qsizetype(stream.total_out));
    } catch (...) { result.error = std::current_exception(); }
    return result;
}
} // namespace
void encodePng8(const QString& path, const QImage& source, const ImageMetadata& metadata, JobControl& control) {
    control.checkpoint();
    if (source.isNull() || source.format()!=QImage::Format_RGBA8888)
        throw std::runtime_error("PNG encoder requires a nonempty RGBA8 image.");
    const bool rgb = opaque(source, control);
    const auto image = rgb ? source.convertToFormat(QImage::Format_RGB888) : source;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error(file.errorString().toStdString());
    write(file, QByteArray::fromHex("89504e470d0a1a0a"));
    QByteArray header(13, '\0');
    qToBigEndian(quint32(image.width()), header.data());
    qToBigEndian(quint32(image.height()), header.data()+4);
    header[8]=8; header[9]=rgb ? 2 : 6;
    chunk(file,"IHDR",header);
    if (!metadata.icc.isEmpty()) {
        const auto compressed=qCompress(metadata.icc,9);
        if (compressed.size()<5) throw std::runtime_error("Cannot encode the PNG color profile.");
        auto profile=QByteArray("ICC Profile\0\0",13)+compressed.mid(4);
        chunk(file,"iCCP",profile);
    }
    if (image.dotsPerMeterX()>0 && image.dotsPerMeterY()>0) {
        QByteArray density(9,'\0');
        qToBigEndian(quint32(image.dotsPerMeterX()),density.data());
        qToBigEndian(quint32(image.dotsPerMeterY()),density.data()+4);
        density[8]=1; chunk(file,"pHYs",density);
    }
    if (!metadata.exif.isEmpty()) chunk(file,"eXIf",metadata.exif);
    if (!metadata.xmp.isEmpty())
        chunk(file,"iTXt",QByteArray("XML:com.adobe.xmp\0\0\0\0\0",22)+metadata.xmp);
    chunk(file,"IDAT",QByteArray::fromHex("7801")); // One zlib stream, independently compressed deflate blocks.
    QThreadPool pool;
    const int workers=std::clamp(QThread::idealThreadCount(),1,4);
    pool.setMaxThreadCount(workers);
    const int rows=std::clamp(1024*1024/(image.width()*(rgb ? 3 : 4)+1),1,256);
    uLong checksum=1;
    for (int first=0; first<image.height();) {
        std::vector<QFuture<Block>> batch;
        for (int task=0; task<workers && first<image.height(); ++task) {
            const int start=first, count=std::min(rows,image.height()-first);
            first+=count;
            const bool last=first==image.height();
            batch.push_back(QtConcurrent::run(&pool,[&image,start,count,last,&control] {
                return compressRows(image,start,count,last,control);
            }));
        }
        // Drain every worker even when cancelled; no task can outlive its image.
        std::exception_ptr error;
        for (auto& future : batch) {
            const auto block=future.result();
            if (block.error) { error=block.error; continue; }
            if (error) continue;
            try {
                control.checkpoint();
                chunk(file,"IDAT",block.data);
                checksum=adler32_combine(checksum,block.checksum,z_off_t(block.bytes));
            } catch (...) { error=std::current_exception(); }
        }
        if (error) std::rethrow_exception(error);
    }
    std::array<char,4> tail{};
    qToBigEndian(quint32(checksum),tail.data());
    chunk(file,"IDAT",QByteArrayView(tail.data(),4));
    chunk(file,"IEND",{});
    control.checkpoint();
    if (!file.commit()) throw std::runtime_error(file.errorString().toStdString());
}
} // namespace ve
