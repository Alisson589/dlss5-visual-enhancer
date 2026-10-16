#include "paths.hpp"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSet>
#include <QUrl>
#include <QStandardPaths>
#include <algorithm>
#include <stdexcept>
#include <windows.h>

namespace ve {
Paths::Paths() {
    root = qEnvironmentVariable("VE_APP_ROOT");
    if (root.isEmpty()) {
        wchar_t executable[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, executable, DWORD(std::size(executable)));
        if (!length || length >= std::size(executable))
            throw std::runtime_error("Cannot locate the application executable.");
        // The deployed executables live beside their resources. Development
        // helpers may select a deployed resource tree with VE_APP_ROOT.
        root = QFileInfo(QString::fromWCharArray(executable, int(length))).absolutePath();
    }
    root = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
}

const Paths& Paths::instance() {
    static Paths paths;
    return paths;
}
QString Paths::at(const QString& relative) const {
    return QDir(root).filePath(relative);
}
QString Paths::ffmpeg() const {
    return at("bin/ffmpeg/bin/ffmpeg.exe");
}
QString Paths::ffprobe() const {
    return at("bin/ffmpeg/bin/ffprobe.exe");
}
namespace {
QString writableDirectory(QStandardPaths::StandardLocation location, const QString& suffix = {}) {
    const auto base = QStandardPaths::writableLocation(location);
    if (base.isEmpty())
        throw std::runtime_error("Cannot locate per-user application storage.");
    const auto directory = suffix.isEmpty() ? base : QDir(base).filePath(suffix);
    if (!QDir().mkpath(directory))
        throw std::runtime_error(("Cannot create application storage: " + directory).toStdString());
    return directory;
}
} // namespace
QString Paths::configurationFile() const {
    return QDir(writableDirectory(QStandardPaths::AppConfigLocation)).filePath("config.ini");
}
QString Paths::cacheDirectory() const {
    return writableDirectory(QStandardPaths::CacheLocation);
}
QString Paths::nativeDataDirectory() const {
    // NGX requires a writable application-data argument, separate from DLL search paths.
    return writableDirectory(QStandardPaths::AppLocalDataLocation, "NVIDIA");
}
void Paths::prepare() const {
    for (const auto* relative : {"outputs", "logs"}) {
        if (!QDir().mkpath(at(relative)))
            throw std::runtime_error(("Cannot create application folder: " + at(relative)).toStdString());
    }
}
QString localPath(const QString& urlOrPath) {
    const QUrl url(urlOrPath);
    return url.isLocalFile() ? url.toLocalFile() : urlOrPath;
}
bool isVideo(const QString& path) {
    static const QSet<QString> extensions{"mp4", "mkv", "mov",  "avi", "webm", "m4v",
                                          "ts",  "mts", "m2ts", "wmv", "flv"};
    return extensions.contains(QFileInfo(path).suffix().toLower());
}
bool isImage(const QString& path) {
    static const QSet<QString> extensions{
        "png", "jpg", "jpeg", "webp", "avif", "tif", "tiff", "bmp", "heic", "heif", "svg", "gif", "3fr",
        "arw", "bay", "cap",  "cr2",  "cr3",  "dcr", "dcs",  "dng", "drf",  "eip",  "erf", "fff", "gpr",
        "iiq", "k25", "kdc",  "mdc",  "mef",  "mos", "mrw",  "nef", "nrw",  "obm",  "orf", "pef", "ptx",
        "pxn", "r3d", "raf",  "raw",  "rw2",  "rwl", "rwz",  "sr2", "srf",  "srw",  "x3f"};
    return extensions.contains(QFileInfo(path).suffix().toLower());
}
QStringList collectInputs(const QStringList& inputs, bool recursive, const QString& excluded) {
    QStringList result;
    QSet<QString> seen;
    auto add = [&](const QString& path) {
        const QFileInfo file(path);
        if (!file.isFile() || (!isImage(path) && !isVideo(path)))
            return;
        auto absolute = file.canonicalFilePath();
        if (absolute.isEmpty())
            absolute = file.absoluteFilePath();
        if (!excluded.isEmpty() && absolute.startsWith(excluded + '/', Qt::CaseInsensitive))
            return;
        const auto key = absolute.toCaseFolded();
        if (!seen.contains(key)) {
            seen.insert(key);
            result.append(absolute);
        }
    };
    for (const auto& input : inputs) {
        const auto path = localPath(input);
        const QFileInfo info(path);
        if (info.isDir()) {
            QDirIterator it(path, QDir::Files,
                            recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
            while (it.hasNext())
                add(it.next());
        } else if (info.isFile())
            add(path);
        else if (path.contains('*') || path.contains('?')) {
            QDir directory(info.absolutePath());
            for (const auto& file : directory.entryInfoList({info.fileName()}, QDir::Files, QDir::Name))
                add(file.absoluteFilePath());
        }
    }
    std::ranges::sort(result,
                      [](const QString& a, const QString& b) { return a.toCaseFolded() < b.toCaseFolded(); });
    return result;
}
QString uniqueOutputPath(const QString& path) {
    if (!QFileInfo::exists(path))
        return path;
    const QFileInfo info(path);
    for (int number = 2;; ++number) {
        const auto candidate = info.dir().filePath(info.completeBaseName() + '_' + QString::number(number) +
                                                   '.' + info.suffix());
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
}
} // namespace ve
