#include "paths.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSet>
#include <QUrl>
#include <QUuid>
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
        QDir directory(QFileInfo(QString::fromWCharArray(executable)).absolutePath());
        for (int depth = 0; depth < 8; ++depth) {
            const auto isBundle = [](const QDir& candidate) {
                return candidate.exists("app/bin/runtime") &&
                       candidate.exists("app/resources/desktop/qml/Main.qml");
            };
            if (isBundle(directory)) {
                root = directory.absolutePath();
                break;
            }
            QDir bundle(directory.filePath("Visual Enhancer"));
            if (isBundle(bundle)) {
                root = bundle.absolutePath();
                break;
            }
            if (!directory.cdUp())
                break;
        }
    }
    if (root.isEmpty())
        throw std::runtime_error("Cannot find Visual Enhancer/app. Build and deploy the application first.");
    root = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
}

const Paths& Paths::instance() {
    static Paths paths;
    return paths;
}
QString Paths::at(const QString& relative) const {
    const auto worker=qEnvironmentVariable("VE_VFX_WORKER_TMP");
    if(!worker.isEmpty()&&relative.startsWith("data/temp/app/"))return QDir(worker).filePath(relative.mid(14));
    return QDir(root).filePath(relative);
}
QString Paths::ffmpeg() const {
    return at("app/bin/ffmpeg/bin/ffmpeg.exe");
}
QString Paths::ffprobe() const {
    return at("app/bin/ffmpeg/bin/ffprobe.exe");
}
QString Paths::temporary(const QString& prefix) const {
    const auto path = at("data/temp/app/" + prefix + "-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().mkpath(path))
        throw std::runtime_error("Cannot create the portable temporary folder.");
    return path;
}
QString Paths::encode(const QString& path) const {
    if (path.isEmpty() || !QFileInfo(path).isAbsolute())
        return path;
    const auto relative = QDir(root).relativeFilePath(QFileInfo(path).absoluteFilePath());
    if (relative == ".")
        return "${APP_ROOT}";
    if (QFileInfo(relative).isAbsolute() || relative == ".." || relative.startsWith("../"))
        return path;
    return "${APP_ROOT}/" + relative;
}
QString Paths::decode(const QString& path) const {
    const auto migrate = [&](const QString& relative) {
        for (const auto* folder : {"config", "jobs", "logs", "outputs", "temp"}) {
            const QString prefix = QString::fromLatin1(folder);
            if (relative == prefix || relative.startsWith(prefix + '/'))
                return at("data/" + relative);
        }
        if (relative == "Video for tests" || relative.startsWith("Video for tests/"))
            return QDir::cleanPath(QDir(root).filePath("../Tests/Media" + relative.mid(15)));
        return at(relative);
    };
    if (path == "${APP_ROOT}")
        return root;
    if (path.startsWith("${APP_ROOT}/")) {
        const auto relative = path.mid(12);
        // Older saves prefixed external files on another Windows drive.
        // Recover drive-qualified paths without accepting relative traversal.
        const auto normalized = QDir::fromNativeSeparators(relative);
        if (normalized.size() >= 3 && normalized[0].isLetter() &&
            normalized[1] == ':' && normalized[2] == '/')
            return QDir::cleanPath(normalized);
        if (QFileInfo(relative).isAbsolute() || relative.split('/').contains(".."))
            return path;
        return migrate(relative);
    }
    // Existing absolute settings may still refer to the former flat workspace.
    // Only relocate its known data/media folders; external user paths stay valid.
    const auto oldRoot = QDir(root).absoluteFilePath("..") + '/';
    const auto normalized = QDir::fromNativeSeparators(path);
    const auto oldPrefix = QDir::cleanPath(oldRoot) + '/';
    if (QFileInfo(normalized).isAbsolute() && normalized.startsWith(oldPrefix, Qt::CaseInsensitive)) {
        const auto relative = normalized.mid(oldPrefix.size());
        for (const auto* folder : {"config", "jobs", "logs", "outputs", "temp", "Video for tests"}) {
            const QString prefix = QString::fromLatin1(folder);
            if (relative == prefix || relative.startsWith(prefix + '/'))
                return migrate(relative);
        }
    }
    return path;
}

void Paths::prepare() const {
    const QMap<QByteArray, QString> locations{{"TEMP", "data/temp/scratch"},
                                              {"TMP", "data/temp/scratch"},
                                              {"TMPDIR", "data/temp/scratch"},
                                              {"APPDATA", "data/temp/profile/Roaming"},
                                              {"LOCALAPPDATA", "data/temp/profile/Local"},
                                              {"XDG_CONFIG_HOME", "data/temp/profile/config"},
                                              {"XDG_DATA_HOME", "data/temp/profile/data"},
                                              {"XDG_CACHE_HOME", "data/temp/profile/cache"},
                                              {"QML_DISK_CACHE_PATH", "data/temp/qml-cache"},
                                              {"CUDA_CACHE_PATH", "data/temp/cuda-cache"}};
    for (auto it = locations.begin(); it != locations.end(); ++it) {
        if (!QDir().mkpath(at(it.value())))
            throw std::runtime_error("Portable application folders must be writable.");
        qputenv(it.key().constData(), at(it.value()).toUtf8());
    }
    for (const auto& path : {"data/temp/app", "data/config", "data/outputs", "data/logs", "data/jobs"})
        QDir().mkpath(at(path));
    qputenv("QT_DISABLE_SHADER_DISK_CACHE", "1");
    qputenv("QSG_RHI_DISABLE_SHADER_DISK_CACHE", "1");
    qputenv("QT_MEDIA_BACKEND", "ffmpeg");
}
QString localPath(const QString& urlOrPath) {
    const QUrl url(urlOrPath);
    return Paths::instance().decode(url.isLocalFile() ? url.toLocalFile() : urlOrPath);
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
