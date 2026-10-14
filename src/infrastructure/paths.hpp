#pragma once
#include <QString>
#include <QStringList>

namespace ve {
class Paths {
  public:
    static const Paths& instance();
    QString root;
    [[nodiscard]] QString at(const QString& relative) const;
    [[nodiscard]] QString ffmpeg() const;
    [[nodiscard]] QString ffprobe() const;
    [[nodiscard]] QString temporary(const QString& prefix) const;
    [[nodiscard]] QString encode(const QString& path) const;
    [[nodiscard]] QString decode(const QString& path) const;
    void prepare() const;

  private:
    Paths();
};
[[nodiscard]] QString localPath(const QString& urlOrPath);
[[nodiscard]] bool isVideo(const QString& path);
[[nodiscard]] bool isImage(const QString& path);
[[nodiscard]] QStringList collectInputs(const QStringList& inputs, bool recursive = false,
                                        const QString& excluded = {});
[[nodiscard]] QString uniqueOutputPath(const QString& path);
} // namespace ve
