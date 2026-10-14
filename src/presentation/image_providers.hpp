#pragma once
#include <QCache>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>
#include <QSet>
#include <QTemporaryDir>
#include <memory>

namespace ve {
class IconProvider : public QQuickImageProvider {
  public:
    IconProvider() : QQuickImageProvider(Image) {}
    QImage requestImage(const QString& id, QSize* size, const QSize& requested) override;

  private:
    QCache<QString, QImage> cache_{512};
    QMutex mutex_;
};
class PreviewProvider : public QQuickImageProvider {
  public:
    static constexpr qint64 diskBudget = 2ll * 1024 * 1024 * 1024;
    explicit PreviewProvider(int memoryKiB = 384 * 1024);
    ~PreviewProvider() override;
    [[nodiscard]] QString store(const QImage& image, const QString& key = {});
    // Identity must describe exact pixels and metadata, including all controls.
    [[nodiscard]] QString storeImmutable(const QImage& image, const QByteArray& identity);
    void clear();
    void retain(const QStringList& urls);
    [[nodiscard]] bool ownsPath(const QString& path) const;
    [[nodiscard]] qint64 diskUsage() const;
    [[nodiscard]] qint64 memoryUsage() const;
    QImage requestImage(const QString& id, QSize* size, const QSize& requested) override;

  private:
    QCache<QString, QImage> images_{384 * 1024}; // KiB: keeps large image costs within Qt's integer API.
    mutable QMutex mutex_;
    QTemporaryDir storage_;
    struct DiskCleanup;
    struct DiskFrame;
    std::shared_ptr<DiskCleanup> cleanup_;
    QHash<QString, std::shared_ptr<DiskFrame>> files_;
    QStringList order_;
    QSet<QString> retained_;
    qint64 diskBytes_ = 0, diskLimit_ = diskBudget;
    quint64 epoch_ = 0;
    void trim();
    QString storeFrame(const QImage& image, const QString& key, bool immutable);
};
} // namespace ve
