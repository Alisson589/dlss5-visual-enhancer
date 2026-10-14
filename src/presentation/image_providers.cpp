#include "image_providers.hpp"
#include "infrastructure/log.hpp"
#include <QElapsedTimer>
#include "infrastructure/paths.hpp"
#include <QColor>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QPainter>
#include <QRegularExpression>
#include <QUuid>
#include <QThreadPool>
#include <QThread>
#include <atomic>
#include <cstring>
#include <limits>

namespace ve {
QImage IconProvider::requestImage(const QString& id, QSize* size, const QSize& requested) {
    QImage image;
    {
        QMutexLocker lock(&mutex_);
        if (const auto* cached = cache_.object(id))
            image = *cached;
        else {
            const auto parts = id.split('/');
            if (parts.size() != 3 || !QRegularExpression("^[a-z0-9_]+$").match(parts[1]).hasMatch())
                return {};
            const int px = parts[0].toInt();
            if (!QList<int>{16, 20, 24, 32, 48, 64, 128, 256}.contains(px))
                return {};
            const QColor color('#' + parts[2]);
            if (!color.isValid())
                return {};
            image = QImage(Paths::instance().at("app/resources/desktop/qml/assets/icons/png/" + parts[0] + '/' +
                                                parts[1] + ".png"))
                        .convertToFormat(QImage::Format_ARGB32_Premultiplied);
            if (image.isNull())
                return {};
            QPainter painter(&image);
            painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            painter.fillRect(image.rect(), color);
            painter.end();
            cache_.insert(id, new QImage(image));
        }
    }
    if (size)
        *size = image.size();
    return requested.isValid() ? image.scaled(requested, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                               : image;
}
struct PreviewProvider::DiskCleanup {
    QThreadPool pool;
    std::atomic<qint64> pending{0};
    std::atomic<bool> closing{false};
    DiskCleanup() { pool.setMaxThreadCount(1); pool.setExpiryTimeout(1000); }
    ~DiskCleanup() { pool.waitForDone(); }
    void remove(QString path, qint64 bytes) {
        pending.fetch_add(bytes, std::memory_order_relaxed);
        pool.start([this, path=std::move(path), bytes] {
            for (int retry=0; retry<20; ++retry) {
                if (QFile::remove(path) || !QFileInfo::exists(path)) {
                    pending.fetch_sub(bytes, std::memory_order_relaxed);
                    return;
                }
                // During shutdown, do not accumulate 500 ms per locked frame.
                // The temporary-directory cleanup makes a final removal pass;
                // any remaining stale files are handled by startup cleanup.
                if (closing.load(std::memory_order_relaxed))
                    return;
                QThread::msleep(25);
            }
            // Keep failed deletions charged until the owned temporary directory closes.
        });
    }
};
struct PreviewProvider::DiskFrame {
    QString path;
    qint64 bytes = 0;
    std::shared_ptr<DiskCleanup> cleanup;
    ~DiskFrame() { if (!path.isEmpty()) cleanup->remove(path, bytes); }
};
PreviewProvider::PreviewProvider(int memoryKiB)
    : QQuickImageProvider(Image, ForceAsynchronousImageLoading),
      storage_(Paths::instance().at("data/temp/app/preview-images-XXXXXX")), cleanup_(std::make_shared<DiskCleanup>()) {
    images_.setMaxCost(std::max(0, memoryKiB));
    if (!storage_.isValid())
        throw std::runtime_error("Cannot create the preview image cache.");
}
PreviewProvider::~PreviewProvider() {
    QElapsedTimer elapsed;
    elapsed.start();
    cleanup_->closing.store(true, std::memory_order_relaxed);
    clear();
    cleanup_->pool.waitForDone();
    info("shutdown", QString("Preview files released in %1 ms.").arg(elapsed.elapsed()));
}
QString PreviewProvider::store(const QImage& source, const QString& key) {
    return storeFrame(source,key,false);
}
QString PreviewProvider::storeImmutable(const QImage& source, const QByteArray& identity) {
    if (identity.isEmpty()) throw std::runtime_error("Immutable preview identity is empty.");
    return storeFrame(source,"identified-"+QString::fromLatin1(QCryptographicHash::hash(identity,QCryptographicHash::Sha256).toHex()),true);
}
QString PreviewProvider::storeFrame(const QImage& source, const QString& key, bool immutable) {
    if (source.isNull())
        return {};
    if (immutable) {
        QMutexLocker lock(&mutex_);
        if (const auto existing=files_.value(key); existing && QFileInfo::exists(existing->path)) {
            order_.removeAll(key); order_.append(key);
            (void)images_.object(key);
            return "image://preview/"+key;
        }
    }
    const auto image = source.convertToFormat(source.depth() > 32 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    quint64 epoch = 0;
    {
        QMutexLocker lock(&mutex_);
        epoch = epoch_;
    }
    QString id = key;
    if (id.isEmpty()) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        QByteArray metadata;
        QDataStream description(&metadata, QIODevice::WriteOnly);
        description << image.size() << qint32(image.format()) << image.colorSpace().iccProfile() << image.devicePixelRatio()
                    << image.dotsPerMeterX() << image.dotsPerMeterY();
        hash.addData(metadata);
        for (int row = 0; row < image.height(); ++row)
            hash.addData(QByteArrayView(reinterpret_cast<const char*>(image.constScanLine(row)), qsizetype(image.width())*(image.depth()/8)));
        id = "content-" + QString::fromLatin1(hash.result().toHex());
        QMutexLocker lock(&mutex_);
        if (const auto existing = files_.value(id); existing && QFileInfo::exists(existing->path)) {
            order_.removeAll(id); order_.append(id);
            (void)images_.object(id);
            return "image://preview/" + id;
        }
    }
    if (!QRegularExpression("^[A-Za-z0-9_-]+$").match(id).hasMatch())
        throw std::runtime_error("Invalid preview image key.");
    auto entry = std::make_shared<DiskFrame>();
    entry->cleanup = cleanup_;
    entry->path = storage_.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + ".frame");
    // These session cache files have unique names and are published only after
    // the complete write. Atomic replacement and durable disk synchronization
    // are unnecessary here and can stall a preview on Windows storage.
    QFile file(entry->path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        throw std::runtime_error(file.errorString().toStdString());
    QDataStream header(&file);
    header << quint32(0x56454933) << image.size() << qint32(image.format()) << image.colorSpace().iccProfile()
           << image.devicePixelRatio() << image.dotsPerMeterX() << image.dotsPerMeterY();
    const auto bytes = QByteArrayView(reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes());
    if (file.write(bytes.data(), bytes.size()) != bytes.size() || !file.flush())
        throw std::runtime_error(file.errorString().toStdString());
    file.close();
    const auto fileBytes = QFileInfo(entry->path).size();
    entry->bytes = fileBytes;
    QMutexLocker lock(&mutex_);
    if (epoch_ != epoch) return {};
    if (const auto previous = files_.value(id)) diskBytes_ -= previous->bytes;
    files_[id] = std::move(entry);
    diskBytes_ += fileBytes;
    order_.removeAll(id);
    order_.append(id);
    const auto cost = image.sizeInBytes() / 1024 + 1;
    if (cost <= images_.maxCost())
        images_.insert(id, new QImage(image), int(cost));
    trim();
    return "image://preview/" + id;
}
void PreviewProvider::trim() {
    for (auto it = order_.begin(); diskBytes_ > diskLimit_ && it != order_.end();) {
        if (retained_.contains(*it) || *it == order_.back()) {
            ++it;
            continue;
        }
        images_.remove(*it);
        diskBytes_ -= files_.take(*it)->bytes;
        it = order_.erase(it);
    }
}
void PreviewProvider::retain(const QStringList& urls) {
    QMutexLocker lock(&mutex_);
    retained_.clear();
    for (const auto& url : urls)
        if (url.startsWith("image://preview/"))
            retained_.insert(url.mid(16).section('?', 0, 0));
    trim();
}
bool PreviewProvider::ownsPath(const QString& path) const {
    return path.startsWith(storage_.path() + '/', Qt::CaseInsensitive);
}
qint64 PreviewProvider::diskUsage() const {
    QMutexLocker lock(&mutex_);
    return diskBytes_ + cleanup_->pending.load(std::memory_order_relaxed);
}
qint64 PreviewProvider::memoryUsage() const {
    QMutexLocker lock(&mutex_);
    return qint64(images_.totalCost()) * 1024;
}
void PreviewProvider::clear() {
    QMutexLocker lock(&mutex_);
    images_.clear();
    ++epoch_;
    files_.clear();
    order_.clear();
    retained_.clear();
    diskBytes_ = 0;
}
QImage PreviewProvider::requestImage(const QString& urlId, QSize* size, const QSize& requested) {
    QImage image;
    std::shared_ptr<DiskFrame> entry;
    {
        QMutexLocker lock(&mutex_);
        const auto id = urlId.section('?', 0, 0);
        const auto* cached = images_.object(id);
        if (cached)
            image = *cached;
        else entry = files_.value(id);
    }
    // A lease keeps this immutable file alive while reading its native pixels.
    // No GUI bookkeeping can wait behind a large disk read or image conversion.
    if (entry) {
            QFile file(entry->path);
            if (file.open(QIODevice::ReadOnly)) {
                quint32 magic = 0;
                QSize dimensions;
                QByteArray profile;
                double dpr = 1;
                int dx = 0, dy = 0;
                qint32 format = 0;
                QDataStream header(&file);
                header >> magic >> dimensions >> format >> profile >> dpr >> dx >> dy;
                if (magic == 0x56454933 && dimensions.isValid() && dimensions.width() <= 16384 &&
                    dimensions.height() <= 16384 && (format == QImage::Format_RGBA64 || format == QImage::Format_RGBA8888)) {
                    image = QImage(dimensions, QImage::Format(format));
                    const auto data = file.readAll();
                    if (data.size() != image.sizeInBytes())
                        image = {};
                    else {
                        std::memcpy(image.bits(), data.constData(), size_t(data.size()));
                        image.setColorSpace(QColorSpace::fromIccProfile(profile));
                        image.setDevicePixelRatio(dpr);
                        image.setDotsPerMeterX(dx);
                        image.setDotsPerMeterY(dy);
                    }
                }
            }
    }
    if (size)
        *size = image.size();
    return requested.isValid() ? image.scaled(requested, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                               : image;
}
} // namespace ve
