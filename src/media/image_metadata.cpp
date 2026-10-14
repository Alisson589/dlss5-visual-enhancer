#include "image_metadata.hpp"
#include "infrastructure/paths.hpp"
#include "infrastructure/process.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <limits>
#include <vector>
#include <zlib.h>

namespace ve {
namespace {
class Tiff {
  public:
    explicit Tiff(QByteArray bytes) : bytes_(std::move(bytes)), little_(bytes_.startsWith("II")) {
        if (bytes_.size() < 8 || (!little_ && !bytes_.startsWith("MM")) || read16(2) != 42)
            throw std::runtime_error("Unsupported EXIF metadata layout.");
    }
    quint16 read16(qsizetype at) const {
        bounds(at, 2);
        return little_ ? qFromLittleEndian<quint16>(bytes_.constData() + at)
                       : qFromBigEndian<quint16>(bytes_.constData() + at);
    }
    quint32 read32(qsizetype at) const {
        bounds(at, 4);
        return little_ ? qFromLittleEndian<quint32>(bytes_.constData() + at)
                       : qFromBigEndian<quint32>(bytes_.constData() + at);
    }
    void put16(qsizetype at, quint16 value) {
        bounds(at, 2);
        if (little_)
            qToLittleEndian(value, bytes_.data() + at);
        else
            qToBigEndian(value, bytes_.data() + at);
    }
    void put32(qsizetype at, quint32 value) {
        bounds(at, 4);
        if (little_)
            qToLittleEndian(value, bytes_.data() + at);
        else
            qToBigEndian(value, bytes_.data() + at);
    }
    [[nodiscard]] QByteArray bytes() const { return bytes_; }
    QString description() const {
        const auto root = read32(4);
        const auto count = read16(root);
        for (int i = 0; i < count; ++i) {
            const auto entry = root + 2 + i * 12;
            if (read16(entry) != 270 || read16(entry + 2) != 2)
                continue;
            const auto length = read32(entry + 4);
            if (!length || length > 1024 * 1024)
                return {};
            const auto offset = length <= 4 ? entry + 8 : read32(entry + 8);
            bounds(offset, length);
            auto text = bytes_.mid(offset, length);
            if (text.endsWith('\0'))
                text.chop(1);
            return QString::fromUtf8(text);
        }
        return {};
    }
    QByteArray withDescription(const QString& value) {
        const auto root = read32(4);
        const auto count = read16(root);
        bounds(root + 2, qsizetype(count) * 12 + 4);
        std::vector<QByteArray> entries;
        entries.reserve(size_t(count) + 1);
        for (int i = 0; i < count; ++i) {
            const auto at = root + 2 + i * 12;
            if (read16(at) == 270)
                continue;
            entries.push_back(bytes_.mid(at, 12));
        }
        QByteArray description = value.toUtf8();
        description.append('\0');
        if (quint64(bytes_.size()) + quint64(description.size()) + quint64(entries.size()) * 12 + 18 >
                std::numeric_limits<quint32>::max() ||
            entries.size() >= 65535)
            throw std::runtime_error("EXIF metadata is too large.");
        if (bytes_.size() % 2)
            bytes_.append('\0');
        const auto newRoot = bytes_.size();
        const auto textAt = newRoot + 2 + qsizetype(entries.size() + 1) * 12 + 4;
        const auto next = read32(root + 2 + count * 12);
        bytes_.resize(textAt + description.size());
        put32(4, quint32(newRoot));
        put16(newRoot, quint16(entries.size() + 1));
        QByteArray descriptionEntry(12, '\0');
        if (little_) {
            qToLittleEndian(quint16(270), descriptionEntry.data());
            qToLittleEndian(quint16(2), descriptionEntry.data() + 2);
            qToLittleEndian(quint32(description.size()), descriptionEntry.data() + 4);
            qToLittleEndian(quint32(textAt), descriptionEntry.data() + 8);
        } else {
            qToBigEndian(quint16(270), descriptionEntry.data());
            qToBigEndian(quint16(2), descriptionEntry.data() + 2);
            qToBigEndian(quint32(description.size()), descriptionEntry.data() + 4);
            qToBigEndian(quint32(textAt), descriptionEntry.data() + 8);
        }
        if (description.size() <= 4) {
            descriptionEntry.replace(8, 4, description.leftJustified(4, '\0'));
        }
        entries.push_back(descriptionEntry);
        std::ranges::sort(entries, [this](const QByteArray& a, const QByteArray& b) {
            return (little_ ? qFromLittleEndian<quint16>(a.constData())
                            : qFromBigEndian<quint16>(a.constData())) <
                   (little_ ? qFromLittleEndian<quint16>(b.constData())
                            : qFromBigEndian<quint16>(b.constData()));
        });
        for (qsizetype i = 0; i < qsizetype(entries.size()); ++i) {
            bytes_.replace(newRoot + 2 + i * 12, 12, entries[size_t(i)]);
            if ((little_ ? qFromLittleEndian<quint16>(entries[size_t(i)].constData())
                         : qFromBigEndian<quint16>(entries[size_t(i)].constData())) == 274) {
                put16(newRoot + 2 + i * 12 + 2, 3);
                put32(newRoot + 2 + i * 12 + 4, 1);
                put32(newRoot + 2 + i * 12 + 8, 0);
                put16(newRoot + 2 + i * 12 + 8, 1);
            }
        }
        put32(textAt - 4, next);
        bytes_.replace(textAt, description.size(), description);
        return bytes_;
    }

  private:
    void bounds(qsizetype at, qsizetype size) const {
        if (at < 0 || size < 0 || at > bytes_.size() || size > bytes_.size() - at)
            throw std::runtime_error("Invalid EXIF metadata offset.");
    }
    QByteArray bytes_;
    bool little_;
};
QByteArray segment(quint8 marker, const QByteArray& payload) {
    if (payload.size() > 65533)
        throw std::runtime_error("Image metadata exceeds the JPEG segment limit.");
    QByteArray bytes(4, Qt::Uninitialized);
    bytes[0] = '\xff';
    bytes[1] = char(marker);
    qToBigEndian(quint16(payload.size() + 2), bytes.data() + 2);
    return bytes + payload;
}
QByteArray exv(const ImageMetadata& metadata) {
    QByteArray bytes = QByteArray::fromHex("ff014578697632");
    if (!metadata.exif.isEmpty())
        bytes += segment(0xe1, QByteArray("Exif\0\0", 6) + metadata.exif);
    if (!metadata.xmp.isEmpty())
        bytes += segment(0xe1, QByteArray("http://ns.adobe.com/xap/1.0/\0", 29) + metadata.xmp);
    bytes += QByteArray::fromHex("ffd9");
    return bytes;
}
QByteArray pngChunk(const QByteArray& name, const QByteArray& data) {
    QByteArray bytes(4, Qt::Uninitialized);
    qToBigEndian(quint32(data.size()), bytes.data());
    bytes += name;
    bytes += data;
    const auto crc =
        quint32(crc32(0, reinterpret_cast<const Bytef*>(bytes.constData() + 4), uInt(bytes.size() - 4)));
    QByteArray tail(4, Qt::Uninitialized);
    qToBigEndian(crc, tail.data());
    return bytes + tail;
}
void save(const QString& path, const QByteArray& data) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        throw std::runtime_error(file.errorString().toStdString());
}
} // namespace
QString mergeRenderDescription(const QString& existing, const QString& note) {
    if (existing.isEmpty())
        return note;
    static const QRegularExpression own(
        "(?m)^(?:Visual Enhancer|DLSS 5 Visual Enhancer):\\r?\\nDLSS 5 Neural Rendering Settings - NR Style "
        "- (?:Style [0-2]|Default|Natural|Cinematic),[^\\r\\n]+$");
    auto merged = existing;
    if (own.match(existing).hasMatch()) {
        merged.replace(own, note);
        return merged;
    }
    return existing + "\n\n" + note;
}
ImageMetadata readImageMetadata(const QString& source, JobControl& control) {
    ImageMetadata metadata;
    if (source.isEmpty())
        return metadata;
    if (QFileInfo(source).suffix().compare("png", Qt::CaseInsensitive) == 0) {
        QFile input(source);
        if (input.open(QIODevice::ReadOnly)) {
            const auto bytes = input.readAll();
            for (qsizetype at = 8; at + 12 <= bytes.size();) {
                const auto length = qFromBigEndian<quint32>(bytes.constData() + at);
                if (length > quint64(bytes.size() - at - 12))
                    break;
                const auto name = bytes.mid(at + 4, 4), data = bytes.mid(at + 8, length);
                if (name == "eXIf")
                    metadata.exif = data;
                if (name == "iTXt" && data.startsWith("XML:com.adobe.xmp")) {
                    const auto separator = data.indexOf('\0');
                    if (separator >= 0 && separator + 5 <= data.size() && data[separator + 1] == 0)
                        metadata.xmp = data.mid(separator + 5);
                }
                at += 12 + length;
            }
            if (!metadata.exif.isEmpty())
                return metadata;
        }
    }
    const auto data =
        runProcess(Paths::instance().at("app/bin/exiv2/exiv2.exe"), {"-ea-", source}, &control, 30000).output;
    if (data.size() > 16 * 1024 * 1024)
        throw std::runtime_error("Image metadata exceeds the supported size.");
    for (qsizetype at = 7; at + 4 <= data.size();) {
        if (quint8(data[at]) != 255)
            break;
        const auto marker = quint8(data[at + 1]);
        if (marker == 0xd9)
            break;
        const auto length = qFromBigEndian<quint16>(data.constData() + at + 2);
        if (length < 2 || at + 2 + length > data.size())
            break;
        const auto payload = data.mid(at + 4, length - 2);
        if (marker == 0xe1) {
            if (payload.startsWith(QByteArray("Exif\0\0", 6)))
                metadata.exif = payload.mid(6);
            else if (payload.startsWith(QByteArray("http://ns.adobe.com/xap/1.0/\0", 29)))
                metadata.xmp = payload.mid(29);
        }
        at += 2 + length;
    }
    return metadata;
}
void addImageDescription(ImageMetadata& metadata, const QString& note) {
    auto bytes = metadata.exif;
    if (bytes.isEmpty())
        bytes = QByteArray::fromHex("49492a0008000000000000000000");
    Tiff tiff(bytes);
    metadata.exif = tiff.withDescription(mergeRenderDescription(tiff.description(), note));
}
void normalizeImageOrientation(ImageMetadata& metadata) {
    if (metadata.exif.isEmpty())
        return;
    Tiff tiff(metadata.exif);
    const auto root = tiff.read32(4);
    const auto count = tiff.read16(root);
    for (int index = 0; index < count; ++index) {
        const auto entry = root + 2 + index * 12;
        if (tiff.read16(entry) != 274)
            continue;
        tiff.put16(entry + 2, 3);
        tiff.put32(entry + 4, 1);
        tiff.put32(entry + 8, 0);
        tiff.put16(entry + 8, 1);
    }
    metadata.exif = tiff.bytes();
}
void writeImageMetadata(const QString& destination, const ImageMetadata& metadata, JobControl& control) {
    control.checkpoint();
    const auto extension = QFileInfo(destination).suffix().toLower();
    if ((extension == "jpg" || extension == "jpeg") && !metadata.icc.isEmpty()) {
        QFile input(destination);
        if (!input.open(QIODevice::ReadOnly))
            throw std::runtime_error(input.errorString().toStdString());
        const auto original = input.readAll();
        input.close();
        if (!original.startsWith(QByteArray::fromHex("ffd8")))
            throw std::runtime_error("Invalid JPEG image.");
        const auto count = (metadata.icc.size() + 65518) / 65519;
        if (count > 255)
            throw std::runtime_error("ICC profile is too large.");
        QByteArray chunks;
        for (qsizetype index = 0; index < count; ++index) {
            auto payload = QByteArray("ICC_PROFILE\0", 12);
            payload.append(char(index + 1));
            payload.append(char(count));
            payload += metadata.icc.mid(index * 65519, 65519);
            chunks += segment(0xe2, payload);
        }
        save(destination, original.left(2) + chunks + original.mid(2));
    }
    if (QFileInfo(destination).suffix().compare("png", Qt::CaseInsensitive) == 0) {
        QFile input(destination);
        if (!input.open(QIODevice::ReadOnly))
            throw std::runtime_error(input.errorString().toStdString());
        const auto original = input.readAll();
        input.close();
        if (!original.startsWith(QByteArray::fromHex("89504e470d0a1a0a")) || original.size() < 33)
            throw std::runtime_error("Invalid PNG image.");
        QByteArray chunks;
        if (!metadata.exif.isEmpty())
            chunks += pngChunk("eXIf", metadata.exif);
        if (!metadata.xmp.isEmpty())
            chunks += pngChunk("iTXt", QByteArray("XML:com.adobe.xmp\0\0\0\0\0", 22) + metadata.xmp);
        save(destination, original.left(33) + chunks + original.mid(33));
        return;
    }
    if (metadata.exif.isEmpty() && metadata.xmp.isEmpty())
        return;
    QTemporaryDir temporary(Paths::instance().at("data/temp/app/metadata-XXXXXX"));
    if (!temporary.isValid())
        throw std::runtime_error("Cannot create the metadata directory.");
    save(temporary.filePath(QFileInfo(destination).completeBaseName() + ".exv"), exv(metadata));
    const auto result = runProcess(Paths::instance().at("app/bin/exiv2/exiv2.exe"),
                                   {"-ieix", "-l", temporary.path(), destination}, &control, 30000);
    if (result.exitCode)
        throw std::runtime_error(QString::fromUtf8(result.error).toStdString());
}
} // namespace ve
